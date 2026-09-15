# CodeOS AGENTS.md

Guidance for humans and coding agents working on this tree.

## Goals
- Make CodeOS feel like a daily-driver hobby OS
- Android / Linux app support (containers + Zircon)
- Keep the desktop approachable and polished

## Layout notes
- **Canonical panel apps** live in `pkgs/core/panels/src/` — the kernel Makefile
  compiles those, not the older copies under `kernel/kernel/`.
- Userspace programs are built from `pkgs/core/<name>/src/` via
  `kernel/userspace/Makefile`, then embedded with `scripts/gen_initramfs.py`.
- Kernel-injected packages use a `pkgs/core/<name>/src/KERN` marker. The
  graphics stack lives in `pkgs/core/graphics/` and is compiled into the
  kernel automatically.
- OpenWeb’s HTTP backend is Rust: `kernel/kernel/rust_ow/` (needs `cargo`).

## Build
```sh
make -C kernel all
make -C kernel codeos-1-kernel.iso
make -C kernel run-iso
```
Host needs `x86_64-elf-gcc`, `xorriso`, `python3`, `cargo`, and QEMU for run targets.

## Todo
1. Moss-style CCP fetch/sync — stone.index metadata, `fetch fetch`, `sync -u` (in progress)
2. Improve kernel quality (memory, sched, syscalls, drivers)
3. Docker-like containers for apps
4. Improve OpenWeb
5. GUI polish (Big Sur / ThormiumOS direction)
6. Consistent SVG / icon pipeline
7. Drop stale duplicates under `kernel/kernel/` once panels are sole source of truth

## VM / app-compat stack (done)
- crosvm: launcher at `pkgs/core/crosvm-launcher/src/` (kernel-syscall only: SHM, FORK/EXECVE/WAIT, READDIR over `/tmp/crosvm-cmds/{name}.cmd|.ctl|.pid`). Wired into userspace `CORE_PROGS` + kernel `USER_PROGS`.
- VM control: `SYSCALL_VM` (52) + `VM_CMD_*` in `kernel/kernel/syscall.c`; `vm_manager_init()` called from `main.c`. Shell commands: `vm list|info|start|stop|pause|resume|run`.
- Linux compat: `linux_syscall_handler` routed via personality; added KILL(62), TGKILL(234), GETPPID(64), GETEUID/GETEGID(107/108), SETUID/SETGID(105/106), SIGALTSTACK(131), CLONE(56), READLINKAT(267), NEWFSTATAT(262). `linux-runner` sets PERSONALITY_LINUX then execve.
- Process signals: `proc_kill()` (SIGTERM/SIGKILL→zombie+wake parent, SIGSTOP/SIGCONT), `SYSCALL_KILL` (51).
- Android: `android-apps` program (list/containers/launch) in `pkgs/core/android-apps/src/`; container exec syscall fixed (argv now via a4=r10); `android-container` exposes props/binder/ashmem.
