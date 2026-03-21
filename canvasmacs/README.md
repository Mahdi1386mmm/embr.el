# canvasmacs

Emacs 31 (PGTK/Wayland) with the [canvas image patch](https://github.com/minad/emacs-canvas-patch) baked in.

This is a minimal fork of the official Arch Linux `emacs-wayland` PKGBUILD.

## Build and install

```sh
cd canvasmacs
makepkg -si
```

`-s` installs missing dependencies (via pacman), `-i` installs the built package when done.

## Uninstall

```sh
sudo pacman -R emacs-canvas-wayland
```

## What the patch adds

New image type `:type canvas` with pixel buffer access via dynamic modules (`canvas_pixel`, `canvas_refresh`). See the [upstream bug](https://debbugs.gnu.org/cgi/bugreport.cgi?bug=80281) for details.
