# esp32-linein-stream

An ESPHome external component that turns an **ESP32-P4 + a WM8782 I2S ADC board**
into a low-latency line-in audio source for **Music Assistant** (or any player that
can open a URL). It captures stereo line-level audio and serves it as a streaming
WAV over HTTP, with a runtime **gain** control and a **3-band parametric EQ**
exposed as Home Assistant sliders.

Built and tested on a **UeeKKoo ESP32-P4 PoE Ethernet AI Dev Board** (native RMII
Ethernet, no external SPI ethernet chip), running alongside a Sendspin DAC player
on the second I2S port. The component itself is board-agnostic — it started life
on an ESP32-S3 + external W5500 board, and moved to the P4 to give the capture/
EQ/stream pipeline and Sendspin playback separate CPU and network headroom (see
*Performance notes* below).

## Features

- Stereo 24-bit capture from an I2S ADC, streamed as 16-bit PCM/WAV on a TCP port.
- Add to Music Assistant as a **Radio / URL** source — ffmpeg probes the WAV header.
- Runtime **digital gain** (linear multiplier) with clip clamping.
- **3-band RBJ biquad EQ** (low-shelf / peaking / high-shelf), float DSP on the MCU's FPU.
- Master or slave I2S, selectable MCLK multiple, and standard-I2S or left-justified framing.
- No cloud, no external audio libraries — just the ESP-IDF `i2s_std` driver + lwIP sockets.

## Hardware

Total cost roughly **~$70** — a fraction of a commercial network line-in streamer.

| Part | Model (as used) | ~Price | Notes |
|------|-----------------|--------|-------|
| ESP32-P4 board | [UeeKKoo ESP32-P4 PoE Ethernet AI Dev Board](https://www.amazon.com/dp/B0FN7JQ2V8) (ESP32-P4-POE-ETH) | ~$25 | ESP32-P4, RISC-V dual-core (400 MHz) + LP core, 32 MB in-package PSRAM + 32 MB NOR flash, **native RMII Ethernet MAC** with onboard PoE module (power + data over one cable), MIPI-CSI/DSI, USB 2.0 OTG, SDIO 3.0 TF slot, 27 free GPIOs. Chosen to replace the original ESP32-S3 board — native EMAC instead of an SPI-bridged W5500 gives the capture/EQ/stream pipeline and Sendspin playback separate CPU and network headroom. |
| I2S ADC board | [EBTOOLS "I2S ADC Audio Card Module"](https://www.amazon.com/dp/B0D7NBKVTQ) (WM8782) | ~$26 | 24.576 MHz oscillator, **Master/Slave** + **16/24bit** DIP switches, 3.5 mm line input with ~6 dB input amp. **Master mode = 96k/192k only**; 48 kHz is **Slave mode**, which is why this build runs the ADC as a slave. |
| I2S DAC board *(optional)* | [Zopsc PCM5102A DAC Decoder Board](https://www.amazon.com/dp/B0C7KQPR7S) | ~$18 | Same PCM5102A chip as before — self-clocking via an internal PLL (no external MCLK needed), 2.1 Vrms 3.5 mm line/headphone out, switchable filter mode via the FLT pin. Only needed for the **player-output** half of the build (the Sendspin `speaker`); not required to stream line-in. |
| Line-level source | — | — | e.g. a turntable pre-amp or mixer AUX out. |

> The DAC is optional: it's there because this device is *both* a line-in **source**
> and a Music Assistant **player** (via Sendspin). If you only want line-in
> streaming, you can omit the DAC/`speaker`/`sendspin` blocks entirely.
>
> **Board note:** this build's ESP32-P4 module is an **engineering-sample**
> revision, which requires `engineering_sample: true` in the `esp32:` block
> (already set in `linein-streamer.yaml`). If your module is production silicon,
> you may not need that flag — check your chip's marking/revision.

### Wiring (this build)

**Line-in capture — WM8782 ADC → ESP32:**

| Signal | ESP32-P4 GPIO | ADC board |
|--------|---------------|-----------|
| LRCLK  | GPIO2 | LRCK |
| DATA   | GPIO4 (input) | DATA / DOUT |
| BCLK   | GPIO3 | BCK  |
| MCLK   | GPIO5 | MCLK |
| 3V3 / GND | 3V3 / GND | VCC / GND |

**DAC output (optional) — ESP32 → PCM5102A:**

| Signal | ESP32-P4 GPIO | DAC board |
|--------|---------------|-----------|
| BCLK   | GPIO14 | BCK  |
| DATA   | GPIO15 | DIN  |
| LRCLK  | GPIO06 | LCK  |
| 5V / GND | 5V / GND | VIN / GND |

(The PCM5102A self-clocks from BCLK, so no MCLK wire is needed on the DAC side.)

### WM8782 DIP switches / jumper

- **Master/Slave → Slave** — the ESP32 drives all clocks.
- **16/24bit → 24bit**.
- **MCLK jumper → off the on-board oscillator**, MCLK fed from the ESP32 (GPIO5).

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
  din_pin: GPIO4           # required (data input)
  mclk_pin: GPIO5          # optional; emit MCLK for the ADC
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

## Performance notes (why P4, not S3)

This project started on an ESP32-S3 + external W5500 (SPI) Ethernet chip. Running
the line-in capture/EQ/stream pipeline **and** Sendspin playback at the same time
caused audible stutter: the W5500 requires bit-banging every Ethernet frame over
SPI, and that overhead — sharing a core with the timing-critical I2S capture loop —
was enough to occasionally miss the DMA deadline. Moving to the ESP32-P4's **native
RMII EMAC** (no SPI-bridged Ethernet chip) resolved it completely, running on Core 0
alongside the capture/HTTP tasks while Sendspin runs on Core 1.

With that headroom back, I2S buffering was halved (`dma_desc_num` 8→6,
`dma_frame_num`/`FRAMES_PER_READ` 512→256), cutting capture-side buffer latency
from ~85 ms to ~32 ms, with no regression. Sendspin's task stack was also moved to
PSRAM (`task_stack_in_psram: true`) now that 32 MB is available.

## Known issues

- **Faint broadband hiss on the line-in capture path**, present even with no
  source connected (i.e. ADC self-noise, not a real signal). Ground-loop has been
  ruled out. A bulk electrolytic decoupling cap across the WM8782 board's VCC/GND
  improved it slightly but did not eliminate it — the remainder is likely a mix of
  the WM8782's own noise floor and rail noise that only a proper ceramic (0.1 µF)
  bypass cap placed right at the chip's supply pins would catch. This is a known
  limitation of breadboard/dupont-wire construction; a planned custom PCB revision
  should close the gap with decoupling built into the layout instead of bolted on
  afterward.

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
