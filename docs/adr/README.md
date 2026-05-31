# Architecture Decision Records

Each ADR captures one significant decision: its context, the decision, and the
consequences. New decisions get the next number; use `0000-template.md` as a
starting point. Land an ADR in the same change as the code that implements it.

| ADR | Title | Status |
|-----|-------|--------|
| [0001](0001-cross-compile-then-self-host.md) | Cross-compile real apps first, self-host GCC later | Accepted |
| [0002](0002-dynamic-linking-and-musl.md) | Add dynamic linking and port musl as the system libc | Accepted |
| [0003](0003-package-format-and-lpkg.md) | Package format and the `lpkg` package manager | Accepted |
| [0004](0004-persistent-root-fs.md) | Persistent writable root filesystem | Accepted |
| [0005](0005-display-server-protocol.md) | Display server protocol for the desktop | Accepted |
| [0006](0006-hybrid-kernel-direction.md) | Hybrid (macOS/XNU-style) kernel direction | Accepted |
| [0007](0007-ipc-ports-and-service-manager.md) | IPC ports and the service manager | Accepted |
| [0009](0009-procfs-config-and-services.md) | procfs, /etc configuration, and init services | Accepted |
| [0010](0010-account-administration.md) | Account administration and a stronger password hash | Accepted |
| [0011](0011-gui-settings-app.md) | GUI Settings app | Accepted |
| [0012](0012-self-hosting-native-toolchain.md) | Self-hosting native toolchain packaged for on-device builds | Accepted |
| [0013](0013-unified-filesystem-namespace.md) | Unified filesystem namespace (drop "drive letter" mounts) | Accepted |
| [0014](0014-executable-resolution-and-real-bin.md) | PATH-based executable resolution and a real /bin | Accepted |
| [0015](0015-gnu-userland-and-buildable-portfolio.md) | Robust GNU userland and a self-hosted, buildable app portfolio | Accepted |
| [0016](0016-closing-the-self-hosting-loop.md) | Closing the self-hosting loop (gcc compiles & runs on device) | Accepted |
