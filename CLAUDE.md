# desktopCast - Developer Instructions

## Build Steps

After any code change, always:

1. **Build**: `meson compile -C builddir`
2. **Build Flatpak**: `flatpak-builder --force-clean flatpak-build com.desktopcast.DesktopCast.json`
3. **Export bundle**: `flatpak build-export /tmp/desktopcast-repo flatpak-build && flatpak build-bundle /tmp/desktopcast-repo desktopcast.flatpak com.desktopcast.DesktopCast`

The `desktopcast.flatpak` bundle must always be rebuilt and kept up to date before pushing.

## App Identity

- App ID: `com.desktopcast.DesktopCast`
- Binary: `desktopcast`
- Flatpak manifest: `com.desktopcast.DesktopCast.json`
- Runtime: GNOME Platform 48 (Flathub)
