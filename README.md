# WMD

WMD (Wall Movie Daemon) is a lightweight native Wayland live-wallpaper daemon, branched from WPD. It uses GTK 3 layer-shell surfaces and GStreamer decoding, with no subprocess renderer and no compositor-specific APIs.

## Status

WMD 0.1.0 is the first usable branch release. It provides a persistent per-user daemon, native looping video playback, dropped-frame backpressure, WPD scaling modes, static-image fallback, state restoration, and the inherited wallpaper picker layouts. WPD remains maintained separately on `main` as the still-wallpaper daemon.

## Dependencies

- Meson and Ninja
- GTK+ 3.24 and gtk-layer-shell
- GStreamer 1.20+ core, app, video, and the codec plugins needed by your files
- Cairo / GDK Pixbuf
- libheif

## Build and install

```sh
make
sudo make install
```

For a user-local install:

```sh
meson setup build --prefix="$HOME/.local"
ninja -C build install
```

## Use

Start WMD once from compositor startup:

```sh
wmd daemon
```

Play a looping live wallpaper:

```sh
wmd video "/path/with spaces/ambient.webm" fill
```

Scaling modes are `fill` (default), `fit`, `stretch`, and `none`. A new `video` command replaces the current stream without restarting the daemon. Audio is discarded intentionally. End-of-stream seeks back to the beginning.

Static wallpapers and the inherited WPD tools remain available:

```sh
wmd image wallpaper.heic fill
wmd switcher
wmd grid
wmd roots
wmd layouts
```

WMD uses independent paths so it cannot collide with WPD:

- Config and media library: `${XDG_CONFIG_HOME:-$HOME/.config}/wmd/`
- Cache: `${XDG_CACHE_HOME:-$HOME/.cache}/wmd/`
- State: `${XDG_STATE_HOME:-$HOME/.local/state}/wmd/`
- Runtime socket: `$XDG_RUNTIME_DIR/wmd/socket`

The last video or image is restored when the daemon starts. WMD remains a wallpaper daemon: it is not a desktop shell, media player UI, panel, or widget framework.
