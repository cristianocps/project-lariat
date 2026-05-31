# ADR-0010: Account administration and a stronger password hash

- Status: Accepted
- Date: 2026-05-30

## Context

Phase 4 needs real account management. The system already had `/etc/passwd`,
`/etc/shadow`, and `/etc/group` (seeded/persisted via ADR-0004) plus
`login`/`su`/`passwd`, but: there were no tools to add or remove users; the
account-parsing buffers in libc were capped at 2 KiB; the shared password hash
(`crypt-lite`) was a single 64-bit FNV-style digest; and edits to `/etc` made at
runtime were not written back to the persistent store, so they were lost on the
next boot (boot restores `/etc` from `/disk/etc`).

A subtle correctness bug compounded this: ramfs had no `truncate` operation, so
`open(..., O_TRUNC)` was a no-op on `/etc`. Rewriting a file with shorter
content (e.g. removing a user) left the old suffix dangling.

## Decision

- **`useradd` / `userdel`** (`userspace/{useradd,userdel}.c`): create/remove a
  local account across all three databases, allocating the next free uid in
  `[1000,60000)`, creating the home directory, and using a locked (`*`)
  password until `passwd` sets one. Both are embedded in the kernel image so a
  fresh disk has them, and both require uid 0.

- **Persist-on-edit** (`etc_sync` in libc `pwd.c`): after editing `/etc/<db>`,
  the account tools mirror it to `/disk/etc/<db>` so changes survive a reboot.
  `passwd` now does the same for `/etc/shadow`.

- **Lift the 2 KiB caps**: the passwd/shadow parse and rewrite buffers grow to
  16 KiB (`PW_FILEBUF`).

- **Stronger hash (`crypt-lite v2`)**: a 256-bit, salted (up to 16 chars),
  4096-round digest formatted `$L2$<salt>$<64 hex>`, with four mixing lanes and
  periodic salt/key re-injection. `crypt_lite()` dispatches by the stored
  scheme prefix: a `$L$...` hash still verifies with v1 (backward compatible),
  while fresh hashes (kernel seed, `passwd`) use v2. Hash buffers widened to 96
  bytes throughout (libc `crypt`, kernel seed, `login`/`su`/`passwd`).

- **ramfs `truncate`**: implement `O_TRUNC`/`ftruncate` for ramfs, zeroing the
  freed tail, so `/etc` rewrites actually shorten the file.

## Consequences

- `crypt-lite` remains a custom, non-vetted hash; it is deliberately wider and
  costlier than before but is still not a substitute for a real KDF. Replacing
  it with a vetted algorithm (once a crypto library is packaged) only requires a
  new `$id$` scheme and dispatch entry; old hashes keep verifying.
- Group membership beyond the primary group is still not modelled; `useradd`
  creates a matching primary group only.
- An init self-test exercises the full lifecycle (hash determinism/verification,
  `useradd` -> live+persistent presence -> `userdel` -> absence).
