#!/usr/bin/env bash
# Launch FulmenOS inside a Parallels Desktop Ubuntu VM in a standalone QEMU GTK display window.

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT_DIR="$(cd "${SCRIPT_DIR}/.." && pwd)"
cd "${ROOT_DIR}"

if [ "$(uname -s)" != "Linux" ]; then
    echo "Error: Run this script inside your Parallels Ubuntu Linux VM (not on macOS host)." >&2
    exit 1
fi

MODE="direct"
while [ $# -gt 0 ]; do
    case "$1" in
        --grub)
            MODE="grub"
            shift
            ;;
        -h|--help)
            echo "Usage: $0 [--grub]"
            echo "  (default)  Build kernel and launch QEMU direct boot in standalone GTK display"
            echo "  --grub     Build UEFI/GRUB2 ISO and boot via QEMU_EFI.fd in standalone GTK display"
            exit 0
            ;;
        *)
            echo "Unknown option: $1" >&2
            exit 1
            ;;
    esac
done

needs_setup=0
if ! command -v aarch64-linux-gnu-gcc >/dev/null 2>&1 ||
   ! command -v qemu-system-aarch64 >/dev/null 2>&1 ||
   [ ! -f /usr/lib/aarch64-linux-gnu/qemu/ui-gtk.so ]; then
    needs_setup=1
fi

if [ "${MODE}" = "grub" ]; then
    if ! command -v grub-mkrescue >/dev/null 2>&1 ||
       [ ! -f /usr/share/qemu-efi-aarch64/QEMU_EFI.fd ]; then
        needs_setup=1
    fi
fi

if [ "${needs_setup}" -eq 1 ]; then
    echo "Installing missing AArch64 toolchain and QEMU GTK dependencies..."
    make vm-setup
fi

if [ "${MODE}" = "grub" ]; then
    echo "Building FulmenOS UEFI/GRUB2 ISO..."
    make grub-iso
    echo "Launching FulmenOS via UEFI/GRUB2 in standalone GTK window..."
    exec make run-grub-gtk
else
    echo "Building FulmenOS AArch64 kernel..."
    make build
    echo "Launching FulmenOS direct QEMU boot in standalone GTK window..."
    exec make run-gtk
fi
