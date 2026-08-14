# WPD

WPD is a small Wayland wallpaper daemon and set of GTK 3 layer-shell wallpaper pickers. It targets standard Wayland/wlroots interfaces and contains no compositor-specific integration.

## Dependencies

- Meson and Ninja
- GTK+ 3.24
- gtk-layer-shell (`gtk-layer-shell-0` pkg-config package)
- Cairo / GDK Pixbuf
- libheif (required for HEIF and HEIC)

Package names vary. On Arch Linux the relevant packages are commonly `meson ninja gtk3 gtk-layer-shell libheif`; on Debian-family distributions they are commonly `meson ninja-build libgtk-3-dev libgtk-layer-shell-dev libheif-dev`.

## Build and install

```sh
make
make install
```

The Makefile is a small wrapper around Meson. The equivalent direct commands are:

```sh
meson setup build
ninja -C build
ninja -C build install
```

Use `meson setup build --prefix="$HOME/.local"` for a user-local installation.

## Setup and use

Put wallpapers in `${XDG_CONFIG_HOME:-$HOME/.config}/wpd/papers/`, then start the persistent daemon from your compositor startup configuration:

```sh
wpd daemon
```

`wpd daemon` detaches after starting and returns control to the shell. Runtime warnings are appended to `${XDG_STATE_HOME:-$HOME/.local/state}/wpd/daemon.log`.

Change the wallpaper or open a picker with:

```sh
wpd image "/path/with spaces/wallpaper.heic" fill
wpd switcher
wpd carousel
wpd 3d
wpd theme light
```

The daemon restores its last valid wallpaper. It uses one background layer-shell surface per monitor and is event-driven while idle.

Switcher, carousel, and 3d apply wallpapers using `fill` by default. `wpd 3d` replaces the former concept experiments with a perspective-mapped carousel: cards recede, compress toward their far edge, scale by depth, overlap back-to-front, wrap around the library, and retain the selected wallpaper title. It supports mouse selection, Left/Right or `h`/`j`, Enter, scrolling, and Escape.

The 1.1 switcher scales its deck to narrow outputs, adds pointer hover lift, frames the band with Matugen colors, and shows the selected filename and library position beneath the cards.

In 0.4.3-ng, the experimental `refine` layout was merged into the default switcher and the standalone command was removed. The switcher now uses the emphasized center-card layout with five visible cards and a compact rounded band.

0.4.4 restores continuous carousel-style picker navigation, supports both `background=band` and a truly alpha-transparent `background=transparent`, and always initializes daemon surfaces before presenting them to prevent a white background or startup flash. The former `-ng` rebuild is now the main WPD line.

## Configuration

All settings live in `${XDG_CONFIG_HOME:-$HOME/.config}/wpd/switcher.conf`. Daemon keys are `transition`, `duration_ms`, `fps`, and `matugen_command`. Use `wpd transition`, `wpd transition list`, or `wpd transition MODE`; changes are persisted and sent to the running daemon immediately. Available modes are `fade`, `wipe`, `slide`, `grow`, `center`, `corner`, `curve-corner`, `infection`, `radial`, `diamond`, `blinds`, `checker`, `wave`, `curtain`, `clock`, `dissolve`, and `sweep`. Append modifiers with `+`, for example `checker+invert`, `wipe+right`, `slide+bottom`, `corner+left`, or `curve-corner+btmleft`. Direction aliases include `top`, `bottom`, `topleft`, `topright`, `btmleft`, `btmright`, `tl`, `tr`, `bl`, and `br`.

`matugen_command` is tokenized directly and never passed to a shell. `%f` becomes one filename argument, so spaces are safe. After the wallpaper transition finishes, WPD starts the hook asynchronously with `/dev/null` as stdin. For Matugen 4, WPD automatically adds `--source-color-index 0` and `--quiet` unless the command already specifies a source-color preference; this selects the most dominant color without a prompt. If several wallpapers are selected quickly, only the final wallpaper runs the hook. Leave the value empty to disable it. The last result is written to `${XDG_STATE_HOME:-$HOME/.local/state}/wpd/matugen-status` as `ok` or the captured error.

Copy `examples/switcher.conf` to `${XDG_CONFIG_HOME:-$HOME/.config}/wpd/switcher.conf` to customize both pickers. It supports `background`, `skew`, `radius`, `dim`, `spacing`, `selected_width`, `side_width`, `height`, and `border`. Cards are rectangular by default; nonzero `skew` and `radius` opt into curved parallelograms. `background` accepts `band` or `transparent`. Invalid values retain defaults.

Theme is shared by both pickers: `wpd theme light`, `wpd theme dark`, or `wpd theme` to print the current setting. The command updates the `theme` key in `switcher.conf` and applies the next time a picker opens.

After Matugen completes, WPD reads its generated JSON colors and updates the five `*_color` keys in `switcher.conf` without replacing geometry or daemon settings. The next switcher or carousel uses Matugen's surface, foreground, primary, outline, and shadow colors automatically. `wpd theme light` and `wpd theme dark` also select the matching Matugen mode.

The switcher uses Left/Right, Enter, Escape, scrolling, and card clicks. Thumbnail work happens outside the GTK main thread and is cached under `${XDG_CACHE_HOME:-$HOME/.cache}/wpd/`.
