echo "Installing dependencies..."
sudo pacman -Syu --needed --noconfirm base-devel git cmake nasm make clang qemu-full
git clone https://aur.archlinux.org/yay.git
cd yay
makepkg -si --noconfirm
yay -Syu --needed --noconfirm rustup
yay -Syu --needed --noconfirm rust-analyzer
yay -Syu --needed --noconfirm llvm
yay -Syu --needed --noconfirm lld
sudo pacman -Syu --needed --noconfirm libelf
sudo pacman -Syu --needed --noconfirm libelf-devel
sudo pacman -Syu --needed --noconfirm qemu-arch-extra
sudo pacman -Syu --needed --noconfirm qemu-guest-agent
sudo pacman -Syu --needed --noconfirm qemu-virtio
sudo pacman -Syu --needed --noconfirm qemu-guest-wifi
yay -Syu --needed --noconfirm x86_64-elf-g++ x86_64-elf-gcc
cd ..
rustup default stable
rustup update
echo "Dependencies installed successfully."
sleep 1
echo "Cloning CodeOS repository..."
git clone https://github.com/tech-for-everyone/CodeOS
cd CodeOS
echo "Compiling CodeOS and dependencies..."
make -j$(nproc)
echo "CodeOS compiled successfully."
echo "Running CodeOS in QEMU..."
./run.sh