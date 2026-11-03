#!/bin/sh
# Install MDA-8086 Kit Studio for this user (no root password needed):
#   ~/.local/share/kitstudio/    the program
#   ~/.local/bin/kitstudio       command to start it
#   menu entry "MDA-8086 Kit Studio" with its icon
# Remove it again with:  sh install.sh --remove
set -e
here="$(cd "$(dirname "$0")" && pwd)"
dest="$HOME/.local/share/kitstudio"
apps="$HOME/.local/share/applications"
icons="$HOME/.local/share/icons/hicolor/256x256/apps"

if [ "$1" = "--remove" ]; then
  rm -rf "$dest"
  rm -f "$HOME/.local/bin/kitstudio" "$apps/kitstudio.desktop" "$icons/kitstudio.png"
  echo "MDA-8086 Kit Studio removed."
  exit 0
fi

mkdir -p "$dest/bin" "$HOME/.local/bin" "$apps" "$icons"
cp "$here/kitstudio" "$dest/kitstudio"
cp "$here/bin/kitstudio-x86_64" "$here/bin/kitstudio-i386" "$dest/bin/"
cp "$here/README_BANGLA.txt" "$here/JWasm_License.html" "$dest/" 2>/dev/null || true
cp "$here/kitstudio.png" "$icons/kitstudio.png"
chmod 755 "$dest/kitstudio" "$dest/bin/kitstudio-x86_64" "$dest/bin/kitstudio-i386"
ln -sf "$dest/kitstudio" "$HOME/.local/bin/kitstudio"
cat > "$apps/kitstudio.desktop" <<EOF
[Desktop Entry]
Type=Application
Name=MDA-8086 Kit Studio
Comment=Write, assemble and run programs on the MDA-8086 trainer kit
Exec="$dest/kitstudio" %f
Icon=kitstudio
Terminal=false
Categories=Development;Education;
StartupWMClass=kitstudio
EOF
chmod 644 "$apps/kitstudio.desktop"
if command -v update-desktop-database >/dev/null 2>&1; then
  update-desktop-database "$apps" >/dev/null 2>&1 || true
fi
echo "MDA-8086 Kit Studio is installed."
echo "Start it from the menu (MDA-8086 Kit Studio) or with:  $dest/kitstudio"

# the kit's serial port needs the group of the port device (usually "dialout")
grp=dialout
for d in /dev/ttyUSB0 /dev/ttyACM0 /dev/ttyS0; do
  if [ -e "$d" ]; then
    g=$(stat -c %G "$d" 2>/dev/null || echo dialout)
    if [ "$g" != root ]; then grp=$g; fi
    break
  fi
done
if [ "$(id -u)" != 0 ] && ! id -nG 2>/dev/null | tr ' ' '\n' | grep -qx "$grp"; then
  echo
  echo "To use the kit's serial port, run this once (it asks for your password):"
  echo "    sudo usermod -aG $grp \$USER"
  echo "then log out and log in again."
fi
