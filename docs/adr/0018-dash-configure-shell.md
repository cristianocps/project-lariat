# ADR-0018: dash as the `configure` shell, and the kernel fixes it forced

- Status: Accepted
- Date: 2026-05-31

## Context

ADR-0017 brought up GNU `make`, leaving the **autotools `configure` chain** as
the last big piece before a `gcc`-rebuilds-`gcc` bootstrap (Phase 7b). A
`configure` script is a ~20k-line POSIX shell program: it relies on shell
functions, `&&`/`||` lists, command substitution `$(...)` over pipelines,
`case`/`esac`, `for` loops, `[ ]`/`test`, parameter expansion (`${x%.c}`),
arithmetic `$((…))`, here-documents, and a working fork/exec/pipe/wait runtime.
It also re-execs itself under a "better" shell it finds on `PATH`.

The in-tree `/bin/sh` (`userspace/sh.c`) is a deliberately small bootstrap
shell — pipelines, redirection, `;`, job control — and nothing more. Growing it
into a `configure`-capable shell would be a large, bug-prone effort that
duplicates a solved problem. The pragmatic choice is to **adopt an existing
POSIX shell**. Among the candidates (dash, busybox `ash`, bash), **dash** is the
right first target: it is Debian/Ubuntu's `/bin/sh` precisely *because* it runs
`configure` correctly and fast, it is a tiny self-contained C program with no
dependency beyond libc, and it is strictly POSIX (no bashisms to emulate).

## Decision

1. **Cross-build dash and install it as the system `sh`.**
   `toolchain/build-dash.sh` (pinned `DASH_VER` in `versions.sh`) configures
   dash `--host=x86_64-linux-musl --build=<host>` with the cross `gcc` and a
   host `BUILD_CC` for its build-time generators, producing a **dynamic PIE**.
   The package (`dash-<ver>.lpkg`, deps `libc-dev`) installs the binary as
   **both `/usr/bin/dash` and `/usr/bin/sh`** (two copies — the on-device `lpkg`
   needs no symlink support). Because `/usr/bin` precedes `/bin` on `PATH`
   (ADR-0014), `sh` now resolves to dash, while the in-tree `/bin/sh` remains the
   always-present bootstrap fallback. The kernel has no `#!` (shebang) handling,
   so scripts are invoked as `dash script` / `sh script`, not `./script`; that
   is sufficient for `configure`, which is run as `sh configure`.

Bringing dash up exercised fork/exec/pipe/wait far harder than the in-tree
shell ever did, exposing **four latent kernel bugs**. Fixing them is the bulk of
this ADR:

2. **Release file descriptors at process *exit*, not at reap** (`kernel/sched.c`).
   Lariat previously closed a process's fds only when its parent called `wait`
   (`fd_table_free` lived in the waitpid reap path). That violates POSIX and
   *deadlocks* dash's command substitution: for `x=$(echo a | wc -w)`, dash forks
   a child whose stdout is a capture pipe, then **reads the pipe to EOF before it
   `wait`s**. EOF only arrives when the last write end closes — but the exiting
   child was a zombie still *holding* that write end, and the parent could not
   reap it because it was blocked in `read`. `thread_exit()` now frees the fd
   table when the thread goes zombie (before taking the scheduler lock), so the
   write end closes promptly and the reader gets EOF. Reap-time cleanup becomes a
   no-op (`fdt` is already `NULL`).

3. **Wire `vfork` (syscall 58)** (`cpu/syscall.c`). dash uses `vfork`-then-exec
   to run *simple* external commands (e.g. `cat`), while reserving plain `fork`
   for subshells and pipeline stages. `SYS_vfork` was never installed in the
   syscall table, so musl's `vfork()` returned `ENOSYS` and dash aborted the
   command with **"Cannot fork"** — but only for the first simple external
   command, which is why pipelines worked yet a bare `cat <<EOF` failed. `vfork`
   is now aliased to a full `fork`: the child runs in its own copied address
   space, which is safe for its only use (`dup2`/`close`/`exec`) and avoids the
   complexity of true parent-suspending vfork.

4. **`clear_child_tid` correctness** (`kernel/elf.c`, `cpu/syscall.c`). The
   `CLONE_CHILD_CLEARTID` pointer the kernel zeroes on exit was neither reset
   across `execve` (Linux clears it — the old value refers to the previous
   image's address space) nor bounds-checked before the write. A child that
   `fork`+`exec`'d then exited could make the kernel write to a stale user
   address and **page-fault in `sys_exit`**. `execve` now clears the pointer, and
   the exit-time write first checks the page is mapped (`vmm_virt_to_phys_in`),
   so a bogus user pointer can never fault the kernel.

5. **In-tree `wc` honours `-l`/`-w`/`-c`** (`userspace/wc.c`). It previously
   ignored all flags and always printed three counts; `$(… | wc -w)` is
   pervasive in shell scripts, so the bootstrap `wc` now prints only the
   requested counts.

## Consequences

- **dash runs the full configure-class feature set on device.** A single script
  exercising every construct passes end-to-end:

  ```
  lpkg install /var/pkgs/dash-0.5.12.lpkg     # installs /usr/bin/{dash,sh}
  dash /var/shtest.sh
    fn:hi · and_or=1 · param=main.o · cmdsub=3 · case=c-source · for=abc
    test=yes · arith=42 · heredoc=works · DASH_ALL_OK
  ```

  and dash drives a `make` recipe that the in-tree shell cannot:

  ```
  make SHELL=/usr/bin/dash -f Makefile2   →  RECIPE_AND_OK · words=3
  ```

- **The shell is no longer the `configure` blocker.** The exit-time fd release
  and `vfork` fixes are general correctness improvements that benefit every
  forky workload, not just dash. Remaining for the `configure` chain are the
  data-shuffling tools (`sed`/`grep`/`awk`, the rest of coreutils) and `m4`;
  once those are on device, real `./configure` runs become tractable.

- **Deferred:** true `vfork` semantics (parent suspension / shared address
  space) — unnecessary while the alias works; and `#!` shebang handling in
  `execve`, which would let scripts run as `./configure` directly rather than
  `sh configure`.
