#!/usr/bin/env bash
# setup.sh — prepare everything needed to build and run CodeOS.
# Targets Arch-based systems (pacman + yay).

set -euo pipefail

if ! command -v pacman >/dev/null 2>&1; then
    echo "This setup script targets Arch-based systems (pacman) only." >&2
    echo "On Debian/Ubuntu install: build-essential qemu-system-x86 xorriso python3" >&2
    exit 1
fi

echo "Installing dependencies..."
sudo pacman -Syu --needed --noconfirm base-devel git cmake nasm make clang qemu-full
sudo pacman -Syu --needed --noconfirm libelf
sudo pacman -Syu --needed --noconfirm libelf-devel
sudo pacman -Syu --needed --noconfirm qemu-arch-extra
sudo pacman -Syu --needed --noconfirm qemu-guest-agent
sudo pacman -Syu --needed --noconfirm qemu-virtio
sudo pacman -Syu --needed --noconfirm qemu-guest-wifi
sudo pacman -Syu --needed --noconfirm grub      # grub-mkrescue, for `make iso`
sudo pacman -Syu --needed --noconfirm python    # generator scripts in kernel/

# yay (AUR helper), built only if missing
if ! command -v yay >/dev/null 2>&1; then
    echo "Building yay (AUR helper)..."
    [ -d yay/.git ] || git clone https://aur.archlinux.org/yay.git
    cd yay
    makepkg -si --noconfirm
    cd ..
fi

yay -Syu --needed --noconfirm rustup
yay -Syu --needed --noconfirm rust-analyzer
yay -Syu --needed --noconfirm llvm
yay -Syu --needed --noconfirm lld
yay -Syu --needed --noconfirm x86_64-elf-g++ x86_64-elf-gcc   # cross toolchain

rustup default stable
rustup update

# The kernel build pins a specific Rust toolchain (kernel/Makefile:
# RUST_TOOLCHAIN) and compiles the Rust components for x86_64-unknown-none.
RUST_PINNED=1.92.0-x86_64-unknown-linux-gnu
echo "Installing pinned Rust toolchain ($RUST_PINNED)..."
rustup toolchain install "$RUST_PINNED"
rustup target add x86_64-unknown-none --toolchain "$RUST_PINNED"

echo "Dependencies installed successfully."

# CodeOS source
if [ -d CodeOS/.git ]; then
    echo "Updating existing CodeOS checkout..."
    git -C CodeOS pull --ff-only
    cd CodeOS
else
    echo "Cloning CodeOS repository..."
    git clone https://github.com/tech-for-everyone/CodeOS
    cd CodeOS
fi

echo "Compiling CodeOS and dependencies..."
make -j"$(nproc)"

echo "CodeOS compiled successfully."
echo "Boot it with:"
echo "  ./run.sh                                   # builds ISO + runs QEMU"
echo "  qemu-system-x86_64 -cdrom kernel/codeos-1-kernel.iso"