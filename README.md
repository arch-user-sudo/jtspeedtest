# jtspeedtest

A small, zen-styled network speed test client.

Raw **Wayland** + **cairo** + **Pango** — no X11, no GTK, no Qt. The palette and
typography follow `~/JTLab/config.h`: `#030303` background, `#404040` borders,
`#E8A03C` amber accent by default, `#66B366` success green, and the Liberation
font. The accent can be overridden at launch.

## Build & run

```sh
make
./jtspeedtest
./jtspeedtest '#378484'   # override the amber accent
```

The accent can be given as a positional `#RRGGBB` or `RRGGBB` value, or with
`--accent COLOR`. Quote the `#` in a shell. Without an accent argument the
original amber `#E8A03C` is used. The hover and pressed shades are derived from
the chosen color automatically.

The accent can also be changed while the window is running:

```sh
jtspeedtestctl accent '#378484'
jtspeedtestctl '#378484'      # shorthand
jtspeedtestctl quit
```

JTLab uses this control socket to update the accent live when the wallpaper
changes, so a running speed test window does not need to restart.

Dependencies: `wayland-client`, `wayland-protocols`, `wayland-scanner`,
`xkbcommon`, `cairo`, `pangocairo`, `libcurl` (plus `pkg-config`). The build
generates the xdg-shell and xdg-decoration bindings automatically.

A compositor with `xdg-shell` support is required (sway, Hyprland, river, KDE
Plasma, GNOME, …).

Install the binary and hicolor icon into `/usr/local`:

```sh
sudo make install
```

That installs `/usr/local/bin/jtspeedtest`, `/usr/local/bin/jtspeedtestctl`,
plus the matching icons under `/usr/local/share/icons/hicolor`. Uninstall with
`sudo make uninstall`. Use `make install PREFIX=/usr` or set `DESTDIR` for
packaging.

## Controls

| Input | Action |
| --- | --- |
| `Space` / `Enter` | start or stop the test |
| `Esc` / `q` | quit |
| Click the gauge | start a test when idle |
| Drag the header | move the window |

## The test

- **Latency / jitter** — six warm HTTP round trips to Cloudflare.
- **Download** — up to 25 MB from `speed.cloudflare.com/__down`.
- **Upload** — up to 10 MB to `speed.cloudflare.com/__up`, capped at 12 s.
- **Server** — Cloudflare's `cdn-cgi/trace` endpoint (location / IP shown in
  the footer).

The endpoints and payload sizes are `#define`s near the top of
`jtspeedtest.c` if you want to point it at your own LibreSpeed-style server.

## Zen details

While a test runs, the interface breathes: a slow radial glow, drifting dust,
expanding rings that follow the transfer, a sweeping indeterminate arc while
latency is measured, and eased gauge values so the readout never snaps around.
Completion blooms softly green.

A static preview can be rendered without opening a window:

```sh
./jtspeedtest --screenshot preview.png
# or
make preview
```

## HiDPI

The window follows the `wl_output` scale of the monitor it is on (integer
factors). To force a scale for testing:

```sh
JTSPEEDTEST_SCALE=2 ./jtspeedtest
```
