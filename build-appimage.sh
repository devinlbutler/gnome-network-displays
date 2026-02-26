#!/bin/bash
set -e

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
cd "$SCRIPT_DIR"

APPIMAGE_BUILDDIR="builddir-appimage"
APPDIR="AppDir"

# Download linuxdeploy if not present
if [ ! -f linuxdeploy-x86_64.AppImage ]; then
    echo "Downloading linuxdeploy..."
    wget -O linuxdeploy-x86_64.AppImage \
        "https://github.com/linuxdeploy/linuxdeploy/releases/download/continuous/linuxdeploy-x86_64.AppImage"
    chmod +x linuxdeploy-x86_64.AppImage
fi

# Download GStreamer plugin if not present
if [ ! -f linuxdeploy-plugin-gstreamer-x86_64.sh ]; then
    echo "Downloading linuxdeploy GStreamer plugin..."
    wget -O linuxdeploy-plugin-gstreamer-x86_64.sh \
        "https://raw.githubusercontent.com/linuxdeploy/linuxdeploy-plugin-gstreamer/master/linuxdeploy-plugin-gstreamer.sh"
    chmod +x linuxdeploy-plugin-gstreamer-x86_64.sh
fi

# Configure if needed
if [ ! -d "$APPIMAGE_BUILDDIR" ]; then
    echo "Configuring meson build..."
    meson setup "$APPIMAGE_BUILDDIR" . --prefix=/usr -Dfirewalld_zone=false -Dbuild_daemon=false
fi

# Build
echo "Building..."
meson compile -C "$APPIMAGE_BUILDDIR"

# Install into AppDir
echo "Installing into AppDir..."
rm -rf "$APPDIR"
DESTDIR="$(pwd)/$APPDIR" meson install -C "$APPIMAGE_BUILDDIR"

# Create AppImage
echo "Creating AppImage..."
./linuxdeploy-x86_64.AppImage --appimage-extract-and-run --appdir "$APPDIR" \
    --desktop-file "$APPDIR/usr/share/applications/com.desktopcast.DesktopCast.desktop" \
    --icon-file "$APPDIR/usr/share/icons/hicolor/scalable/apps/com.desktopcast.DesktopCast.svg" \
    --plugin gstreamer \
    --output appimage

echo ""
echo "Done! AppImage: desktopCast-x86_64.AppImage"
ls -lh desktopCast-x86_64.AppImage

# Deploy to ~/AppImages if --deploy flag is passed
if [[ "$1" == "--deploy" ]]; then
    echo "Deploying to ~/AppImages/..."
    cp desktopCast-x86_64.AppImage ~/AppImages/
    echo "Deployed: ~/AppImages/desktopCast-x86_64.AppImage"
    ls -lh ~/AppImages/desktopCast-x86_64.AppImage
fi
