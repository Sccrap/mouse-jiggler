# Mouse Jiggler for Flipper Zero

A [Flipper Zero](https://flipperzero.one/) FAP application that turns the
device into a USB HID mouse and periodically nudges the cursor to keep the
host computer (Windows / macOS / Linux) from going to sleep or locking on
idle.

The jiggle is deliberately large and slow enough (25 px, with a 150 ms pause)
to be **clearly visible** on screen — this is not a stealth tool, it's a
visible "yes, I'm still here" nudge.

## Features

- Emulates a real USB HID mouse via `furi_hal_usb_hid` — no drivers needed on
  the host, works like any plug-and-play mouse.
- Simple on-device UI: status (`ACTIVE` / `PAUSED`), USB connection state,
  countdown to the next jiggle, and a cycle counter.
- Adjustable interval (5–120 s, default 45 s) via the D-pad, no rebuild
  required.
- Clean lifecycle: the previous USB profile (normally the CDC/serial console)
  is restored automatically on exit, so nothing is left in a broken state.
- The actual HID sending runs on a dedicated background thread, separate
  from the GUI thread, so the UI never blocks the jiggle and vice versa.

## Controls

| Button      | Action                                          |
|-------------|--------------------------------------------------|
| `OK`        | Start / Pause jiggling                          |
| `Left`      | Decrease interval by 5 s (min 5 s)              |
| `Right`     | Increase interval by 5 s (max 120 s)            |
| `Back`      | Stop jiggling, restore USB mode, exit            |

## Requirements

- A Flipper Zero running official firmware, or a compatible fork
  (Momentum / Unleashed / Xtreme).
- [`ufbt`](https://github.com/flipperdevices/flipperzero-ufbt) (micro Flipper
  Build Tool) to build and deploy the app — no full firmware source checkout
  needed.
- Python 3 and `pip` to install `ufbt`.

## Building

```bash
pip3 install --user ufbt

git clone <this-repo-url> mouse-jiggler
cd mouse-jiggler

# Download/update the SDK for the current firmware release (first run only,
# or whenever you want to target a newer firmware version)
ufbt update

# Build the .fap
ufbt
```

The resulting binary is written to `dist/mouse_jiggler.fap`.

## Deploying to the device

With the Flipper connected over USB:

```bash
ufbt launch
```

This builds, uploads over USB, and starts the app on the device in one step.

Alternatively:

- **qFlipper**: File manager → `/ext/apps/Tools/` → drag and drop
  `dist/mouse_jiggler.fap`, then launch it from **Apps → Tools → Mouse
  Jiggler** on the device.
- **SD card**: copy `mouse_jiggler.fap` into `apps/Tools/` on the Flipper's
  SD card directly.

## Project structure

```
.
├── application.fam    # FAP manifest (appid, entry point, build metadata)
├── mouse_jiggler.c     # Application source
└── dist/               # Build output (generated, not tracked in git)
```

## How it works

- `mouse_jiggler_app()` (main thread) owns the GUI, the USB profile switch
  (`furi_hal_usb_set_config(&usb_hid, ...)`), and input handling. It restores
  the previous USB mode on exit.
- `jiggler_worker()` (a dedicated `FuriThread`) owns the countdown and sends
  the actual HID mouse-move reports (`furi_hal_hid_mouse_move`), independent
  of the GUI thread.
- Shared state (`AppState`) is protected by a `FuriMutex` and read by the
  GUI draw callback for rendering.

See the comments at the top of `mouse_jiggler.c` for the full architecture
rationale, including why a dedicated thread was used instead of a
`FuriTimer` for HID sending.

## Disclaimer

This tool changes the Flipper Zero's USB personality to a HID mouse. Only
use it on hosts you own or have permission to interact with. Restore the
device to its normal profile (exit the app with `Back`) before disconnecting
if you plan to use it as a serial device afterwards.

## License

No license has been chosen yet for this project. Add a `LICENSE` file
before treating it as open source under a specific license.
