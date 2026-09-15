echo "Installing dependencies..."
sudo pacman -Syu --needed --noconfirm base-devel git cmake nasm make clang qemu-full
git clone https://aur.archlinux.org/yay.git
cd yay
makepkg -si --noconfirm
