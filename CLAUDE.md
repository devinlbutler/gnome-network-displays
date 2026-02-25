# desktopCast - Developer Instructions

## Build Steps

After any code change, always:

1. **Build**: `meson compile -C builddir`
2. **Build AppImage**: `./build-appimage.sh`

The `desktopCast-x86_64.AppImage` must always be rebuilt and kept up to date before pushing.

## App Identity

- App ID: `com.desktopcast.DesktopCast`
- Binary: `desktopcast`
- AppImage build script: `build-appimage.sh`
