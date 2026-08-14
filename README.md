# WMD

WMD (Wall Movie Daemon) is a lightweight native Wayland live-wallpaper daemon, branched from WPD. It uses GTK 3 layer-shell surfaces and GStreamer decoding, with no subprocess renderer and no compositor-specific APIs.

## Status

WMD 0.2.0 provides a persistent per-user daemon, native looping video playback, configurable render FPS, dropped-frame backpressure, WPD scaling modes, static-image fallback, state restoration, and the inherited wallpaper picker layouts. It validates GStreamer preroll before accepting state and safely owns playback elements across replacement and failure. WPD remains maintained separately on `main` as the still-wallpaper daemon.

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

Set `WMD_FOREGROUND=1` to keep the daemon attached for systemd user services
or diagnostics; normal invocation still detaches immediately.

Play a looping live wallpaper:

```sh
wmd video "/path/with spaces/ambient.webm" fill
```

Set or inspect the render limit without restarting playback:

```sh
wmd --fps
wmd --fps 30
wmd --hardware auto
wmd config
```

`wmd config` opens a dependency-free terminal UI. FPS and hardware mode are
persisted in `switcher.conf` and sent to the running daemon immediately.
Hardware modes are `auto` (prefer an available hardware decoder), `on` (require
VA H.264 decoding), and `off` (force software decoding). Mode changes apply to
the next video. WMD scales and rate-limits decoded frames before the app sink,
and never copies frames larger than the largest connected output.

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

On Arch Linux, MP4 playback normally requires `gst-plugins-good` for the
QuickTime/ISO demuxer and `gst-libav` or another H.264 decoder. WMD validates
preroll before accepting a video, so missing codecs are reported by
`wmd video` instead of being saved as a broken restore state.

Intel hardware decode additionally requires `gst-plugin-va` and an appropriate
VA driver such as `intel-media-driver`. Use `wmd --hardware on` to require it;
WMD returns a clear error instead of silently falling back when unavailable.
