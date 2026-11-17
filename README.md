# FulmenOS (AArch64 Bare-Metal Microkernel)

**FulmenOS** is a bare-metal, freestanding 64-bit ARM (**AArch64**) microkernel-style operating system written from scratch in AArch64 Assembly, C, and C++ (no standard libraries), built and tested inside **Docker** using `qemu-system-aarch64`.

---

## Core Architecture

- **Boot & CPU Init (`boot/boot.S`, `boot/vectors.S`, `boot/context.S`)**
  - Dual-boot ARM64 Image Header supporting both direct `qemu-system-aarch64 -kernel` and UEFI/GRUB2 ISO (`boot/grub/grub.cfg`).
  - Drops cleanly from EL2 to **EL1h**, enables FP/SIMD (`CPACR_EL1`), installs a 2048-byte aligned `VBAR_EL1` Exception Vector Table, zeroes `.bss`, and enters `kernel_main()`.
  - Hardware **EL1 ↔ EL0** privilege separation (`eret` into EL0t for user programs, `svc #0` traps back into EL1).
- **MMIO Drivers (`drivers/`)**
  - **PL011 UART** (`0x09000000`) for interactive serial CLI.
  - **QEMU `ramfb`** (`0x09020000` `fw_cfg` DMA, `800x600` XRGB8888) displaying a live HUD status bar + graphical console mirror via an embedded 8×16 bitmap font.
  - **ARM GICv2** (`0x08000000` / `0x08010000`) and **ARM Generic Physical Timer** (`100 Hz` preemptive ticks).
- **Memory Management (`mm/`)**
  - **Physical Page Frame Allocator (`mm/pmm.c`)**: 4 KB bitmap managing 128 MB (`0x40000000–0x48000000`).
  - **ARMv8-A MMU (`mm/mmu.c`)**: 3-level `TTBR0_EL1` page tables (Device-nGnRnE for MMIO, Normal-WB for RAM, Normal-NC for framebuffer, `EL0+EL1 RW` with `PXN` isolation for user arena `0x44000000–0x44200000`).
  - **Kernel Heap (`mm/heap.c`)**: Coalescing doubly-linked free-list (`kmalloc`, `kcalloc`, `krealloc`, `kfree`).
- **Preemptive Scheduler & Deadlock Guard (`sched/`, `sync/`)**
  - Switchable live between **Round Robin (`rr`)**, **Priority (`prio`)**, and **4-Queue MLFQ (`mlfq`)** with anti-starvation priority boosts.
  - Blocking **Mutexes**, **Counting Semaphores**, **Lock-Order Hierarchy checks**, and live **Wait-For Graph (WFG) Cycle Detection** (`-EDEADLK`).
- **VFS RAM Disk, EL0 C/C++ Binaries & Built-in Mini-C Interpreter (`fs/`, `user/`, `user_progs/`, `shell/`)**
  - Hierarchical tree (`/`, `/bin`, `/src`, `/etc`, `/home`, `/tmp`) with Linux CLI commands (`ls`, `cd`, `pwd`, `mkdir`, `touch`, `cat`, `write`, `cp`, `mv`, `rm`, `grep`, `wc`, `head`, `tail`, `find`, `hexdump`, `ps`, `top`, `kill`, `free`, `df`, `uname`, `uptime`, `clear`, `history`).
  - **Pre-compiled Native EL0 Binaries (`/bin/`)**: Real AArch64 ELF64 executables compiled from C and C++ (`/bin/hello`, `/bin/fib`, `/bin/primes`, `/bin/calc`), loaded into `0x44000000` and executed in **EL0** via `eret` and `svc #0` syscalls.
  - **In-Kernel Mini-C Interpreter (`cc <file.c>`)**: Interprets `.c` files stored in the RAM disk live inside the shell without needing an external compiler. Supported C subset: `#include`/`#define` skip, `int`/`long`/`char`/`void` types, local & global variables, 1D integer arrays, string/char literals, `if`/`else`, `while`, `for`, `break`, `continue`, `return`, recursive user functions, arithmetic/relational/logical/bitwise operators, and built-ins (`printf`, `puts`, `putchar`, `sleep`, `uptime`, `getpid`, `atoi`, `strlen`).

---

## Running FulmenOS in Docker (Required)

All cross-compilation and QEMU execution runs cleanly inside Docker (`Dockerfile` + `docker-compose.yml`) without touching the host macOS toolchain.

### 1. Build the Kernel inside Docker
```bash
make build
```

### 2. Run Interactive Serial CLI (Headless QEMU inside Docker)
```bash
make run
```

### 3. Run with Graphical `ramfb` Display (noVNC in Browser)
```bash
make run-gui
```
Then open **http://localhost:6080/vnc.html** in your browser to see the `800x600` graphical FulmenOS screen alongside the terminal CLI!

### 4. Run Full Automated Subsystem Verification Suite
```bash
make test
```

### 5. Build UEFI/GRUB2 Bootable ISO inside Docker
```bash
make grub-iso
```

---

## Key Demo Commands Inside FulmenOS Shell

| Command | Description |
| :--- | :--- |
| `help` | List all Linux, user-space, and OS demo commands |
| `test_all` | Run all kernel subsystem self-tests sequentially |
| `sched rr \| prio \| mlfq` | Switch active CPU scheduler live |
| `sched_test` | Spawn worker threads & print tick-by-tick execution trace |
| `mem` / `pagetable 0x44000000` / `alloc_test` | Inspect PMM, walk `TTBR0_EL1` page tables, test heap coalescing |
| `mutex_test` / `sem_test` / `deadlock_test` | Test mutexes, producer-consumer semaphores, & WFG deadlock prevention |
| `el0_test` / `hello` / `fib 15` / `primes` / `calc` | Run compiled C and C++ ELF64 programs in hardware **EL0** |
| `cc /src/demo.c` | Run C source live with the built-in Mini-C interpreter |
