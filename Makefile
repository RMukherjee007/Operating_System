DOCKER_IMAGE := fulmenos-dev
IN_DOCKER := $(wildcard /.dockerenv)
UNAME_S := $(shell uname -s)
USE_DOCKER := $(if $(IN_DOCKER),,$(filter Darwin,$(UNAME_S)))

.PHONY: all build run run-gui run-gtk test grub-iso run-grub run-grub-gtk vm-setup clean docker-build

all: build

ifneq ($(USE_DOCKER),)
# Host macOS wrapper: ALWAYS build and run inside Docker, never directly on host!

DOCKER_TTY := $(shell [ -t 0 ] && echo "-it" || echo "-i")

docker-build:
	docker build -t $(DOCKER_IMAGE) .

ensure-image:
	@docker image inspect $(DOCKER_IMAGE) >/dev/null 2>&1 || docker build -t $(DOCKER_IMAGE) .

build grub-iso clean test: ensure-image
	docker run --rm -v "$(CURDIR):/workspace" -w /workspace $(DOCKER_IMAGE) make $@

run run-grub: ensure-image
	docker run --rm $(DOCKER_TTY) -v "$(CURDIR):/workspace" -w /workspace $(DOCKER_IMAGE) make $@

run-gui: ensure-image
	docker run --rm $(DOCKER_TTY) -p 5900:5900 -p 6080:6080 -v "$(CURDIR):/workspace" -w /workspace $(DOCKER_IMAGE) make $@

else
# Inside Docker container: actual AArch64 cross-compilation and QEMU execution

CC      := aarch64-linux-gnu-gcc
CXX     := aarch64-linux-gnu-g++
AS      := aarch64-linux-gnu-gcc
LD      := aarch64-linux-gnu-ld
OBJCOPY := aarch64-linux-gnu-objcopy

CFLAGS  := -ffreestanding -fno-builtin -fno-stack-protector -nostdlib \
           -mgeneral-regs-only -mstrict-align -Wall -Wextra -Werror=implicit-function-declaration \
           -O2 -Iinclude
CXXFLAGS := $(CFLAGS) -fno-exceptions -fno-rtti -fno-use-cxa-atexit
ASFLAGS := -ffreestanding -Iinclude
LDFLAGS := -nostdlib --no-warn-rwx-segments -T linker.ld

USER_CFLAGS   := -ffreestanding -fno-builtin -fno-stack-protector -nostdlib \
                 -mgeneral-regs-only -mstrict-align -Wall -Wextra -O2 -Iinclude -Iuser
USER_CXXFLAGS := $(USER_CFLAGS) -fno-exceptions -fno-rtti -fno-use-cxa-atexit
USER_LDFLAGS  := -nostdlib --no-warn-rwx-segments -T user_progs/user.ld

BUILD_DIR := build
USER_BUILD := $(BUILD_DIR)/user_progs

ASM_SRCS := \
	boot/boot.S \
	boot/vectors.S \
	boot/context.S

C_SRCS := \
	boot/efi_stub.c \
	kernel/main.c \
	kernel/string.c \
	kernel/irq.c \
	kernel/syscall.c \
	drivers/uart.c \
	drivers/ramfb.c \
	drivers/font8x16.c \
	drivers/virtio_kbd.c \
	drivers/gic.c \
	drivers/timer.c \
	mm/pmm.c \
	mm/mmu.c \
	mm/heap.c \
	sched/scheduler.c \
	sync/sync.c \
	fs/vfs.c \
	fs/initrd.c \
	user/elf_loader.c \
	user/minic.c \
	shell/shell.c \
	shell/cmd_linux.c \
	shell/cmd_demos.c \
	shell/nano.c \
	shell/gui.c

ASM_OBJS := $(patsubst %.S,$(BUILD_DIR)/%.o,$(ASM_SRCS))
C_OBJS   := $(patsubst %.c,$(BUILD_DIR)/%.o,$(C_SRCS))
INITRD_C := $(BUILD_DIR)/initrd_data.c
INITRD_O := $(BUILD_DIR)/initrd_data.o

USER_ELFS := \
	$(USER_BUILD)/hello.elf \
	$(USER_BUILD)/fib.elf \
	$(USER_BUILD)/primes.elf \
	$(USER_BUILD)/calc.elf

KERNEL_ELF := $(BUILD_DIR)/fulmenos-qemu.elf
KERNEL_BIN := $(BUILD_DIR)/fulmen.bin
GRUB_ISO   := $(BUILD_DIR)/fulmen-grub.iso

build: $(KERNEL_ELF) $(KERNEL_BIN)

$(BUILD_DIR)/%.o: %.S
	@mkdir -p $(dir $@)
	$(AS) $(ASFLAGS) -c $< -o $@

$(BUILD_DIR)/%.o: %.c
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -c $< -o $@

# User-space EL0 runtime and ELF binaries
$(USER_BUILD)/ulib.o: user/ulib.c
	@mkdir -p $(dir $@)
	$(CC) $(USER_CFLAGS) -c $< -o $@

$(USER_BUILD)/hello.elf: user_progs/hello.c $(USER_BUILD)/ulib.o user_progs/user.ld
	@mkdir -p $(dir $@)
	$(CC) $(USER_CFLAGS) -c $< -o $(USER_BUILD)/hello.o
	$(LD) $(USER_LDFLAGS) $(USER_BUILD)/ulib.o $(USER_BUILD)/hello.o -o $@

$(USER_BUILD)/fib.elf: user_progs/fib.c $(USER_BUILD)/ulib.o user_progs/user.ld
	@mkdir -p $(dir $@)
	$(CC) $(USER_CFLAGS) -c $< -o $(USER_BUILD)/fib.o
	$(LD) $(USER_LDFLAGS) $(USER_BUILD)/ulib.o $(USER_BUILD)/fib.o -o $@

$(USER_BUILD)/primes.elf: user_progs/primes.c $(USER_BUILD)/ulib.o user_progs/user.ld
	@mkdir -p $(dir $@)
	$(CC) $(USER_CFLAGS) -c $< -o $(USER_BUILD)/primes.o
	$(LD) $(USER_LDFLAGS) $(USER_BUILD)/ulib.o $(USER_BUILD)/primes.o -o $@

$(USER_BUILD)/calc.elf: user_progs/calc.cpp $(USER_BUILD)/ulib.o user_progs/user.ld
	@mkdir -p $(dir $@)
	$(CXX) $(USER_CXXFLAGS) -c $< -o $(USER_BUILD)/calc.o
	$(LD) $(USER_LDFLAGS) $(USER_BUILD)/ulib.o $(USER_BUILD)/calc.o -o $@

# Pack ELF binaries and source code into RAM disk C payload
$(INITRD_C): $(USER_ELFS) user_progs/hello.c user_progs/fib.c user_progs/primes.c user_progs/calc.cpp user_progs/demo.c scripts/pack_initrd.py
	python3 scripts/pack_initrd.py --out $@ \
		--entry /bin/hello=$(USER_BUILD)/hello.elf \
		--entry /bin/fib=$(USER_BUILD)/fib.elf \
		--entry /bin/primes=$(USER_BUILD)/primes.elf \
		--entry /bin/calc=$(USER_BUILD)/calc.elf \
		--entry /src/hello.c=user_progs/hello.c \
		--entry /src/fib.c=user_progs/fib.c \
		--entry /src/primes.c=user_progs/primes.c \
		--entry /src/calc.cpp=user_progs/calc.cpp \
		--entry /src/demo.c=user_progs/demo.c

$(INITRD_O): $(INITRD_C)
	$(CC) $(CFLAGS) -c $< -o $@

$(KERNEL_ELF): $(ASM_OBJS) $(C_OBJS) $(INITRD_O) linker.ld
	$(LD) $(LDFLAGS) $(ASM_OBJS) $(C_OBJS) $(INITRD_O) -o $@

$(KERNEL_BIN): $(KERNEL_ELF)
	$(OBJCOPY) -O binary $< $@
	@ls -lh $(KERNEL_ELF) $(KERNEL_BIN)

run: $(KERNEL_ELF)
	qemu-system-aarch64 -M virt,gic-version=2 -cpu cortex-a72 -m 256M -nic none \
		-kernel $(KERNEL_ELF) -display none -device ramfb -serial mon:stdio

run-gui: $(KERNEL_ELF)
	@echo "Starting noVNC on http://localhost:6080/vnc.html (VNC :5900)..."
	@websockify --web /usr/share/novnc 6080 localhost:5900 >/tmp/novnc.log 2>&1 & \
	WPID=$$!; \
	trap "kill $$WPID 2>/dev/null || true" EXIT; \
	qemu-system-aarch64 -M virt,gic-version=2 -cpu cortex-a72 -m 256M -nic none \
		-kernel $(KERNEL_ELF) -device ramfb \
		-device virtio-keyboard-device -device virtio-tablet-device \
		-vnc :0 -serial mon:stdio

grub-iso: $(KERNEL_BIN) boot/grub/grub.cfg
	@rm -rf $(BUILD_DIR)/iso
	@mkdir -p $(BUILD_DIR)/iso/boot/grub
	@cp $(KERNEL_BIN) $(BUILD_DIR)/iso/boot/fulmen.bin
	@cp boot/grub/grub.cfg $(BUILD_DIR)/iso/boot/grub/grub.cfg
	grub-mkrescue -o $(GRUB_ISO) $(BUILD_DIR)/iso
	@ls -lh $(GRUB_ISO)

run-grub: grub-iso
	qemu-system-aarch64 -M virt,gic-version=2 -cpu cortex-a72 -m 256M -nic none \
		-bios /usr/share/qemu-efi-aarch64/QEMU_EFI.fd \
		-cdrom $(GRUB_ISO) -display none -device ramfb -serial mon:stdio

run-gtk: $(KERNEL_ELF)
	qemu-system-aarch64 -M virt,gic-version=2 -cpu cortex-a72 -m 256M -nic none \
		-kernel $(KERNEL_ELF) -device ramfb \
		-device virtio-keyboard-device -device virtio-tablet-device \
		-serial null -display gtk,show-cursor=on

run-grub-gtk: grub-iso
	qemu-system-aarch64 -M virt,gic-version=2 -cpu cortex-a72 -m 256M -nic none \
		-bios /usr/share/qemu-efi-aarch64/QEMU_EFI.fd \
		-cdrom $(GRUB_ISO) -device ramfb \
		-device virtio-keyboard-device -device virtio-tablet-device \
		-serial null -display gtk,show-cursor=on

vm-setup:
	sudo apt-get update && sudo apt-get install -y \
		gcc-aarch64-linux-gnu g++-aarch64-linux-gnu binutils-aarch64-linux-gnu \
		make python3 nano qemu-system-arm qemu-system-gui qemu-efi-aarch64 \
		grub-efi-arm64-bin grub-common xorriso mtools

test: $(KERNEL_ELF) $(KERNEL_BIN)
	python3 scripts/verify_os.py

clean:
	rm -rf $(BUILD_DIR)

endif
