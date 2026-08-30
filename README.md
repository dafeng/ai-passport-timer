<p align="right">
  <a href="README.zh_CN.md">简体中文</a> · <strong>English</strong>
</p>

# FoloToy Countdown Reminder

A countdown play for AI Passport. The device boots into preset selection. When time is up it beeps and flashes **TIME UP**.

Presets: **5 / 10 / 15 / 20 / 25 / 30 minutes**.

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

## GitHub Actions firmware

Same flow as the upstream [CI build notes](https://github.com/FoloToy/ai-passport/blob/main/docs/development/CI-build-and-release.md): ordinary branch pushes do not publish a Release. A tag does.

1. Enable Actions under **Settings → Actions** (off by default on a new fork).
2. Push a tag such as `v0.1.0-timer`:

```bash
git tag v0.1.0-timer
git push origin v0.1.0-timer
```

3. `Build firmware` runs `./tools/validate.sh --firmware` with ESP-IDF 5.5.3 / ESP32-C3.
4. On success it creates a GitHub Release with `FoloToy-AI-Passport-full.bin`.
5. **Run workflow** on `Build firmware` uploads an artifact only, with no Release.

`Pull request checks` on `main` / PRs also builds firmware and keeps the artifact for 7 days.

Flash with the [web flasher](https://ai-passport.folotoy.cn/tools/web-flasher/): pick the Release `FoloToy-AI-Passport-full.bin` and write from `0x0` to an 8 MB board.
