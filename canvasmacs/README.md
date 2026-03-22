# canvasmacs

Emacs 31 (PGTK/Wayland) with the [canvas image patch](https://github.com/minad/emacs-canvas-patch) baked in.

## Using canvas with embr

Set `embr-render-backend` to `'canvas` to enable the native canvas render path. embr decodes JPEG frames directly into the canvas pixel buffer via a native C module, skipping the per-frame disk round-trip. Works without canvas too -- `'default` is the safe fallback for any Emacs build.

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

## embr benchmark: canvas vs default

Canvas decodes JPEG and writes pixels directly into the canvas buffer via a native C module, bypassing Emacs' `create-image` + `erase-buffer` + `insert-image` cycle.

| Metric | Canvas | Default |
|--------|--------|---------|
| Input-to-frame p50 | **10.0ms** | 14.4ms |
| Input-to-frame p95 | **28.4ms** | 44.8ms |
| Frame interval p50 | **28.9ms** | 29.5ms |
| Effective FPS | **34.6** | 33.9 |
| FPS 30+ bucket | **78.0%** | 79.5% |
| Drop ratio | **0.304** | 0.336 |
| Render skips | **0** | 20 |
| Freezes | 1 (1485ms) | 1 (2250ms) |
| Severe freezes | **0** | 1 |

Canvas wins on input latency (30-35% lower at p50/p95), zero render skips again, lower drop ratio, and no severe freezes. Both use screencast transport.
