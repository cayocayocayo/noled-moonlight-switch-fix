# noled

Screen-off overlay for Nintendo Switch.

Turns off the screen backlight while a background task is running (downloads, file transfers, etc.). Prevents OLED burn-in and saves battery — works on all Switch models (V1, V2, Lite, OLED).

## Features

| Mode | Activation | Behavior |
|------|-----------|----------|
| **Screen Off** | Quick tap | Turns off the backlight. Device may auto-sleep normally. |
| **Screen Off + Stay Awake** | Hold A for 1.5 s | Turns off the backlight and keeps the device awake by periodically pinging the system idle service. |

Press any button to exit and restore the screen.

## Installation

1. Make sure you have [Ultrahand Overlay](https://github.com/ppkantorski/Ultrahand-Overlay) or [Tesla Menu](https://github.com/WerWolv/Tesla-Menu) installed with `nx-ovlloader`.
2. Download `noled.ovl` from the [Releases](https://github.com/kemalsanli/noled/releases) page.
3. Copy it to `/switch/.overlays/` on your SD card.
4. Open the overlay menu (default: **L + DPad Down + Right Stick**) and select **noled**.

## Building from source

Requires [devkitPro](https://devkitpro.org/) with the `switch-dev` group installed.

```bash
git clone --recursive https://github.com/kemalsanli/noled.git
cd noled
make
```

The output file will be at `noled.ovl`.

## How it works

- **Backlight control** — Uses the `lbl` service to turn the screen off and on.
- **Stay-awake mode** — Sends `ReportUserIsActive` (command 5) to the `idle:sys` service every 30 seconds, resetting the auto-sleep timer without changing any system settings.
- **Sleep detection** — Monitors frame timing to detect when the device has slept and woken up. On detection, the overlay exits cleanly without flashing the screen.

No system settings are modified. If the overlay crashes, the device returns to normal behavior automatically.

## Compatibility

- All Nintendo Switch models (V1, V2, Lite, OLED)
- Works with any overlay menu that loads `.ovl` files from `/switch/.overlays/` ([Ultrahand](https://github.com/ppkantorski/Ultrahand-Overlay), [Tesla Menu](https://github.com/WerWolv/Tesla-Menu), etc.)
- Built with [libtesla](https://github.com/WerWolv/libtesla)
- Tested on Atmosphere CFW

## License

[GPL-2.0](LICENSE)
