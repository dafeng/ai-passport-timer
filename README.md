<p align="right">
  <a href="README.zh_CN.md">简体中文</a> · <strong>English</strong>
</p>

# FoloToy Countdown Reminder

A countdown play for AI Passport. The device boots into preset selection. When time is up it beeps and flashes **TIME UP**.

Presets: **30 seconds / 5 / 10 / 15 / 20 / 30 minutes**.

## Keys

| State | Up / Down | OK click | OK long press |
| --- | --- | --- | --- |
| Select | Change preset | Start | — |
| Running | — | Pause | Back to select |
| Paused | — | Resume | Back to select |
| Done | — | Pick again | — |

UI copy is English (the stock font has no CJK glyphs). Battery SOC sits under the top-right cloud and is hidden when unavailable.

## Layout

```text
main/                 timer logic, UI, boot
components/bsp/       display / keys / audio / battery
bootloader_components Recovery key hook (flash compatibility)
tests/                host tests for the timer state machine
tools/                validation and firmware checks
```

## Build

Requires **ESP-IDF 5.5.3**, target ESP32-C3.

```bash
idf.py --version
./tools/validate.sh --static
./tools/validate.sh --firmware
```

Merged image: `build/FoloToy-AI-Passport-full.bin`.
