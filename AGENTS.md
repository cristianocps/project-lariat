# AGENTS.md — Working on Project Lariat with AI

Guidance for AI agents (and humans) contributing to **Project Lariat**, a
from-scratch 64-bit x86_64 operating system: custom bootloader, preemptive SMP
kernel, Linux-numbered POSIX syscall ABI, in-kernel VFS (ramfs/FAT32/ext4),
networking stack, a musl userland, and an on-device toolchain.

This is an **OS kernel**, not an app. A wrong pointer, lock, or ABI byte does not
throw an exception — it silently corrupts memory, deadlocks a core, or triple-
faults the machine. Treat every change as if it ships to firmware: **security,
scalability, and stability are not afterthoughts, they are the acceptance
criteria.** When in doubt, prefer the smaller, verifiable change.

## Start here (read before editing)

- `README.md` — architecture overview and boot flow.
- `docs/ARCHITECTURE.md` — subsystem map.
- `docs/LAYOUT.md` — **where code lives + the `include/` vs `include/uapi/` rule**.
- `docs/ROADMAP.md` — phases, what is done, what is next.
- `docs/FILESYSTEM.md` — namespace, mounts, firmlinks, persistence model.
- `docs/adr/` — Architecture Decision Records; read the latest few for context.

## Golden rules

1. **One concern per change.** Touch one subsystem at a time, then verify the
   build and a boot before moving on. Mechanical refactors (file moves) are
   separate from behavior changes.
2. **Land docs with the code.** Repo convention: a change that makes a decision
   ships its ADR (`docs/adr/NNNN-*.md` + index in `docs/adr/README.md`) and
   updates the affected doc (`ROADMAP.md`, `FILESYSTEM.md`, …) in the *same*
   change. Increment the ADR number; never edit a historical ADR's decision.
3. **Verify, don't assume.** A change is not done until the kernel **builds
   clean** and **boots to a login prompt**. Filesystem changes must keep
   `e2fsck -fn` clean. See "Verification".
4. **Match the existing style.** Read the neighboring file first. No new
   dependencies, no reformatting unrelated code, no narrating comments — comment
   only non-obvious intent, invariants, or hardware quirks.
5. **Never commit build output.** `build/`, `*.img`, `*.elf`, `*.o`, `*.bin`,
   `sysroot/`, `.local_libs/` are gitignored. Don't commit disk images, the
   sysroot, or fetched dependencies. Only commit when explicitly asked.

## Build, run, verify

```bash
make all            # build build/lariat.bin (kernel + boot sector)
make run-headless   # boot in QEMU, serial console on stdout (best for agents)
make run            # boot with display + data disks (FAT32 + ext4)
make debug          # QEMU paused with a GDB stub on :1234
make disks          # (re)create the FAT32 and ext4 data images if missing
```

- The kernel is built **freestanding**: `-ffreestanding -nostdlib -mcmodel=large
  -fno-pie -mno-sse -mno-red-zone`. There is **no libc, no SSE, no stack
  protector** in kernel code. Don't reach for standard headers or floating
  point; use the in-tree `kapi`/`string` helpers.
- A header change rebuilds its dependents automatically (`-MMD`). If a struct
  that crosses the syscall boundary changes, rebuild **both** the kernel and
  `userspace/` so layouts stay in lockstep.
- Headless boot is the agent-friendly path: drive it from a short script that
  waits for the login prompt before sending input (the UART can drop bytes typed
  before the shell is ready). Inspect serial output rather than guessing.

### Verification checklist (per change)

- [ ] `make all` succeeds with no new warnings (`-Wall -Wextra`).
- [ ] `make run-headless` reaches the `login:` prompt and `init` self-tests pass.
- [ ] Filesystem/ext4 changes: `e2fsck -fn ext4.img` is clean after the test
      (use a *fresh* image — stale images carry old corruption).
- [ ] No regression in the touched subsystem's existing behavior.

## Layout & placement (summary of `docs/LAYOUT.md`)

- `boot/` real-mode bootloader · `cpu/` GDT/IDT/SYSCALL/timer · `kernel/` core,
  `kernel/fs/`, `kernel/net/`, `kernel/ipc/`, `kernel/core/` · `drivers/<class>/`
  (`block|char|input|net|video`) · `userspace/` + `userspace/libc/` ·
  `toolchain/` · `docs/`.
- **Header policy is load-bearing:** `include/uapi/` is the **public kernel↔user
  ABI** (syscall numbers, shared structs/ioctls) — the only kernel headers
  userspace and the sysroot may include. Everything else stays in `include/` and
  is kernel-internal. If a definition crosses the syscall boundary it goes in
  `uapi/`; otherwise it does not.
- New kernel file → add to `KERNEL_C`/`KERNEL_ASM` and the right `dirs:`/pattern
  rule in the `Makefile`. New driver class → new `drivers/<class>/` + pattern
  rule. New `/bin` program → `userspace/` + embed entry in the `Makefile`.

## Security (ring-0 owns the machine — earn every privilege)

- **Never trust a user pointer.** Syscalls run on user-supplied addresses;
  validate them (e.g. confirm the page is mapped via the VMM helpers) before
  reading/writing, and bound every length. A bad pointer must return `-EFAULT`,
  never fault the kernel. The dash work already had to bounds-check a stale
  `clear_child_tid` write — treat that as the standard, not the exception.
- **Keep the kernel/user boundary exact.** The syscall ABI is byte-for-byte
  Linux x86_64 (`struct stat` layout, errno-as-negative-return, full 6 argument
  registers). Changing a shared struct is an ABI change: update `uapi/`, rebuild
  both sides, and note it in an ADR.
- **Least privilege in userland.** `login` authenticates against
  `/etc/passwd`+`/etc/shadow` and **drops privileges before exec'ing the shell**;
  keep that invariant. Don't widen file modes or run user code as root.
- **Immutable system, persistent data.** `/`, `/bin` are rebuilt from the kernel
  image each boot; `/etc`, `/home`, `/usr` are firmlinks onto the ext4 data
  volume (see `docs/FILESYSTEM.md`, `adr/0019`). Don't make the system tree
  writable or depend on persisting state outside `/var`.
- **Fail safe.** If the data volume is absent, the kernel boots **rescue mode**
  with a usable static `/bin`. New boot logic must preserve a path to a login
  prompt when persistence/hardware is missing.

## Scalability (SMP-first, no global bottlenecks)

- **Assume every code path runs on all cores at once.** The scheduler is
  preemptive and multi-core; APs run ring-3 threads off a shared ready queue.
  Any data shared across cores needs a lock or per-CPU isolation.
- **Lock discipline:** take the right lock (`sched_lock`, PMM lock, device
  locks) for the shortest possible span; never hold a lock across a blocking
  operation or a context switch you don't control. Document any new lock's
  ordering to avoid deadlocks. Prefer per-CPU data over a new global lock.
- **Don't hardcode ceilings.** The PMM sizes its bitmap from e820 (no RAM cap);
  follow that spirit — size from discovered hardware, support >4 GB via the
  direct map (`phys_to_virt`/`virt_to_phys`), and avoid fixed-size tables where a
  workload could outgrow them (argv limits, fd tables, extent trees all had to
  grow — design for growth up front).
- **TLB/coherence:** changes to shared kernel mappings need a TLB shootdown;
  don't assume a single core's view is global.

## Stability (it must not silently break)

- **No silent failure.** The class of bug this project keeps hitting is the
  *silent* one: a full extent list that `break`s and truncates a file, a dropped
  trailing argv entry, an unwired syscall returning `ENOSYS`. Surface errors
  (negative errno, a log line on the serial console) — never swallow them.
- **Crash-consistency:** the ext4 volume is write-through (no journal). Order
  metadata writes so an interrupted operation leaves a consistent (fsck-clean)
  state; free both data and tree/metadata blocks on truncate/unlink.
- **Memory hygiene:** every `kmalloc`/page alloc has an owner and a free path;
  `execve`/`fork`/process-exit must reclaim address spaces and fds. Leaks here
  compound across uptime.
- **Determinism on boot:** `world_setup` and friends are idempotent and must
  tolerate re-runs and missing volumes. Don't add boot steps that block forever
  or assume a device is present.

## Anti-patterns (do not)

- Don't add a global lock when per-CPU data or a finer lock works.
- Don't widen the syscall/ABI surface without an ADR and a userland rebuild.
- Don't introduce floating point, SSE, or libc/standard-header use in the kernel.
- Don't "fix" a test by deleting the assertion or skipping the fsck.
- Don't leave a `TODO`/stub on a path that can be reached at runtime without a
  safe fallback.
- Don't claim done without a clean build and a boot to login.

## When unsure

Read the nearest ADR and the subsystem's existing code, prefer the minimal
change, and write down the decision (ADR + doc) as part of the change. If a
trade-off is genuinely open (security vs. ergonomics, scope, a destructive
operation), surface it instead of guessing.
