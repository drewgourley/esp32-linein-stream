# esp32-linein-stream

An ESPHome external component that turns an **ESP32-S3 + a WM8782 I2S ADC board**
into a low-latency line-in audio source for **Music Assistant** (or any player that
can open a URL). It captures stereo line-level audio and serves it as a streaming
WAV over HTTP, with a runtime **gain** control and a **3-band parametric EQ**
exposed as Home Assistant sliders.

Built and tested on a **Waveshare ESP32-S3-ETH-compatible board** (the Wonrabai
board in the BOM below, which uses the Waveshare ESPHome board definition),
running alongside a Sendspin DAC player on the second I2S port. The component
itself is board-agnostic.

## Features

- Stereo 24-bit capture from an I2S ADC, streamed as 16-bit PCM/WAV on a TCP port.
- Add to Music Assistant as a **Radio / URL** source — ffmpeg probes the WAV header.
- Runtime **digital gain** (linear multiplier) with clip clamping.
- **3-band RBJ biquad EQ** (low-shelf / peaking / high-shelf), float DSP on the S3 FPU.
- Master or slave I2S, selectable MCLK multiple, and standard-I2S or left-justified framing.
- No cloud, no external audio libraries — just the ESP-IDF `i2s_std` driver + lwIP sockets.

## Hardware

Total cost roughly **~$60** — a fraction of a commercial network line-in streamer.

| Part | Model (as used) | ~Price | Notes |
|------|-----------------|--------|-------|
| ESP32-S3 board | [Wonrabai ESP32-S3-ETH](https://www.amazon.com/dp/B0DLP18N8M) | ~$31 | ESP32-S3R8, 240 MHz dual-core LX7, **8 MB PSRAM**, 16 MB flash, **W5500 Ethernet** (SPI), Wi-Fi/BLE, USB-C, microSD, optional PoE. Two I2S ports — one for capture, one for the DAC. |
| I2S ADC board | [EBTOOLS "I2S ADC Audio Card Module"](https://www.amazon.com/dp/B0D7NBKVTQ) (WM8782) | ~$26 | 24.576 MHz oscillator, **Master/Slave** + **16/24bit** DIP switches, 3.5 mm line input with ~6 dB input amp. **Master mode = 96k/192k only**; 48 kHz is **Slave mode**, which is why this build runs the ADC as a slave. |
| I2S DAC board *(optional)* | [AITRIP PCM5102A](https://www.amazon.com/dp/B08Y6N2FDC) | ~$9 (2-pk) | Self-clocking stereo DAC, 2 Vrms line out on a 3.5 mm jack. Only needed for the **player-output** half of the build (the Sendspin `speaker`); not required to stream line-in. |
| Line-level source | — | — | e.g. a turntable pre-amp or mixer AUX out. |

> The DAC is optional: it's there because this device is *both* a line-in **source**
> and a Music Assistant **player** (via Sendspin). If you only want line-in
> streaming, you can omit the DAC/`speaker`/`sendspin` blocks entirely.

### Wiring (this build)

**Line-in capture — WM8782 ADC → ESP32:**

| Signal | ESP32-S3 GPIO | ADC board |
|--------|---------------|-----------|
| BCLK   | GPIO3 | BCK  |
| LRCLK  | GPIO2 | LRCK |
| DATA   | GPIO17 (input) | DATA / DOUT |
| MCLK   | GPIO1 | MCLK |
| 3V3 / GND | 3V3 / GND | VCC / GND |

**DAC output (optional) — ESP32 → PCM5102A:**

| Signal | ESP32-S3 GPIO | DAC board |
|--------|---------------|-----------|
| BCLK   | GPIO15 | BCK  |
| LRCLK  | GPIO16 | LCK  |
| DATA   | GPIO18 | DIN  |
| 5V / GND | 5V / GND | VIN / GND |

(The PCM5102A self-clocks from BCLK, so no MCLK wire is needed on the DAC side.)

### WM8782 DIP switches / jumper

- **Master/Slave → Slave** — the ESP32 drives all clocks.
- **16/24bit → 24bit**.
- **MCLK jumper → off the on-board oscillator**, MCLK fed from the ESP32 (GPIO1).

This puts MCLK, BCLK and LRCLK all in one clock domain (the ESP32), which is what
gives glitch-free audio. See *Clocking notes* below for why this matters.

## Installation

Copy the `components/` folder next to your ESPHome YAML (the config uses
`external_components: source: type: local, path: components`), **or** point ESPHome
at this repo:

```yaml
external_components:
  - source:
      type: git
      url: https://github.com/drewgourley/esp32-linein-stream
      path: components
    refresh: 0s
```

> Note: with `type: local`, ESPHome reads the copy in its own config directory, so
> re-copy `components/` after any change. The git source avoids that.

Then install/flash the config (`esphome run linein-streamer.yaml`).

## Configuration reference (`linein_stream`)

```yaml
linein_stream:
  id: linein_streamer
  bclk_pin: GPIO3          # required
  lrclk_pin: GPIO2         # required
  din_pin: GPIO17          # required (data input)
  mclk_pin: GPIO1          # optional; emit MCLK for the ADC
  mclk_multiple: 256       # 128/256/384/512/768 (MCLK = mclk_multiple x sample_rate)
  i2s_mode: master         # master | slave
  i2s_port: 1              # 0 | 1 (use a free I2S port)
  i2s_format: philips      # philips (standard I2S) | msb (left-justified)
  sample_rate: 48000       # must match the ADC's clock family (see notes)
  gain: 1.0                # linear input gain (runtime-adjustable)
  channels: 2              # 2 = stereo, 1 = mono downmix
  port: 8080               # HTTP port for the WAV stream
  equalizer:               # optional, up to 8 bands, applied in order
    - type: low_shelf      # peaking | low_shelf | high_shelf
      frequency: 100
      q: 0.707
      gain: 0              # dB
    - type: peaking
      frequency: 1000
      q: 0.9
      gain: 0
    - type: high_shelf
      frequency: 10000
      q: 0.707
      gain: 0
```

## Home Assistant controls

The example exposes template `number` sliders:

- **Line-In Gain** (dB, converted to a linear multiplier).
- **EQ Bass / EQ Mid / EQ Treble** (dB per band), calling `set_band_gain(index, dB)`.

All are `restore_value: true` so they persist across reboots. Add more bands +
sliders for a fuller graphic EQ (the slider index matches the `equalizer:` order).

## Music Assistant

Add the stream as a Radio station / custom URL:

```
http://<device-ip>:8080/
```

Music Assistant's ffmpeg backend reads the WAV header automatically. A static IP or
mDNS name is recommended so the URL stays stable.

## Clocking notes (why this works)

A WM8782 needs `MCLK / LRCLK` to be a supported ratio (256/384/512/…). With the
board's 24.576 MHz oscillator, only 48 kHz-family rates divide cleanly — 44.1 kHz
gives a non-integer ratio and produces full-scale noise. Running two independent
crystals (ESP32 vs the board's oscillator) causes a periodic sample slip that
sounds like a "machine gun"/helicopter chop. Feeding MCLK from the ESP32 (board in
**Slave**) puts everything in one clock domain and fixes both problems.

## Troubleshooting quick reference

| Symptom | Likely cause |
|---------|--------------|
| Silence / all-zero samples | Wrong pins, or slave mode with no incoming clocks |
| Full-scale random noise | Clock/framing mismatch, or invalid MCLK/rate ratio |
| Screech but some structure | Wrong `i2s_format` (try `philips` vs `msb`) |
| "Machine gun" / helicopter chop | Two clock domains — feed MCLK from the ESP32 |
| Clean but quiet | Analog level low; raise the source or `gain` |

## License

[MIT](LICENSE) © 2026 Drew Gourley
