# esp32-linein-stream

Open ESPHome firmware for building your own **Stream Daddy** or **Stream Kiddo** from
off-the-shelf parts: no custom PCB, just a dev board, an I2S audio module or two, and
some wiring. Flash it from the ESPHome Device Builder in Home Assistant.

| Device | What it does |
|--------|--------------|
| **Stream Daddy** | Line-in streamer and network player. Captures stereo line-level audio (turntable, mixer, CD player) and serves it as a live stream that Music Assistant or any player can open from a URL. It is also a Sendspin player, so it can play in sync with other players on your network. Gain and a 3-band EQ are sliders in Home Assistant. |
| **Stream Kiddo** | Sendspin player only: a synced speaker output for any room. |

Parts lists and soldering notes are in the build guide on each product's page on the
Novelty Factor Technologies site. This README covers the firmware side.

> The finished Stream Daddy and Stream Kiddo devices run their own production firmware.
> This repo is the open DIY version for the dev-board builds.

## What you need

- A **Home Assistant** install with the **ESPHome Device Builder** add-on, version
  **2026.9 or newer**. (Tested with 2026.9.1.)
- **Music Assistant** if you want Sendspin playback or want to play Daddy's stream.
- The hardware from the build guide, wired up as shown below.

## 1. Pick your file

The dev boards differ in how they get on the network, so there is one file for each:

| Your board | Stream Daddy | Stream Kiddo |
|------------|--------------|--------------|
| **UeeKKoo ESP32-P4 PoE Ethernet** (wired, PoE) | [`stream-daddy-ethernet.yaml`](stream-daddy-ethernet.yaml) | [`stream-kiddo-ethernet.yaml`](stream-kiddo-ethernet.yaml) |
| **Waveshare ESP32-P4-WIFI6** (Wi-Fi) | [`stream-daddy-wifi.yaml`](stream-daddy-wifi.yaml) | [`stream-kiddo-wifi.yaml`](stream-kiddo-wifi.yaml) |

## 2. Wire it up

The wiring depends on the board, because the Waveshare board's Wi-Fi chip uses GPIO14
and GPIO15 internally.

**Line-in capture (Stream Daddy only): PCM1808 ADC module, both boards**

| ESP32-P4 pin | ADC module |
|--------------|------------|
| GPIO2 | LRCK |
| GPIO3 | BCK |
| GPIO4 | DOUT (data into the ESP32) |
| GPIO5 | SCKI (system clock) |
| 3V3 / 5V, GND | VCC, GND (per the module's silkscreen) |

Wire the RCA jacks to the module's left and right input pads.

**Line-out: PCM5102A DAC module (Daddy and Kiddo)**

| Signal | UeeKKoo (Ethernet) | Waveshare (Wi-Fi) | DAC module |
|--------|--------------------|-------------------|------------|
| LRCLK | GPIO6 | GPIO6 | LCK |
| BCLK | GPIO14 | GPIO7 | BCK |
| DATA | GPIO15 | GPIO8 | DIN |
| Power | 5V, GND | 5V, GND | VIN, GND |

The ESP32 provides all the I2S clocks (it is the I2S master), so the ADC needs its
clock fed from GPIO5 and nothing else. If you use the all-in-one ADC module that makes
its own clocks, see *Clocking* below.

## 3. Flash it from Home Assistant

1. Open the **ESPHome Device Builder** in Home Assistant.
2. Click **New Device**, give it any name, and choose **Skip** at the board selection.
3. Open the new device, click **Edit**, and **replace everything** with the contents of
   the file you picked in step 1.
4. Wi-Fi boards: the builder already keeps `wifi_ssid` and `wifi_password` in
   **Secrets**, which the file uses. Make sure they are right for your network.
5. Click **Install**. The first time, flash it over USB: plug the board in and choose
   **Plug into this computer** (needs a Chromium browser and HTTPS access to Home
   Assistant), or choose **Manual download**, pick the factory image, and flash it with
   [ESPHome Web](https://web.esphome.io). After that, updates go over the air.
6. Home Assistant should offer the new device under **Settings > Devices & services**.
   Click **Add**.

The first build takes a while: it downloads the toolchain and compiles for the ESP32-P4.

**Changing the name.** Edit `name` and `friendly_name` at the top of the file before the
first install. If you build more than one, give each a different name. The stream URL and
entity names follow it.

**Optional hardening.** The files work on a home network as they are. If you want an API
encryption key or OTA password, copy them from a fresh Device Builder config into the
commented-out lines in the `api:` and `ota:` blocks.

## 4. Use it

**Stream Daddy**

- Home Assistant gets **Line-In Gain** and **EQ Bass / Mid / Treble** sliders, a
  **Stream URL** sensor and an **IP Address** sensor. The values are remembered across
  reboots.
- In Music Assistant, add a Radio station / URL with the stream address, for example
  `http://stream-daddy.local:8080/` (use your device's name; the exact URL is in the
  **Stream URL** sensor). Music Assistant reads the WAV header automatically. If `.local`
  names don't resolve on your network, use the IP address instead and give the device a
  DHCP reservation.
- Daddy also shows up in Music Assistant as a **Sendspin** player. When nothing is
  playing on it, the line-in is also played straight out of its DAC.

**Stream Kiddo**

- Shows up in Music Assistant as a **Sendspin** player and in Home Assistant as a media
  player, with volume control.

## Which chip revision?

Early ESP32-P4 boards used an engineering-sample chip; newer ones are production
silicon. The firmware has to match, set by `engineering_sample` at the top of each
file. The defaults are:

- UeeKKoo Ethernet board: `"true"`
- Waveshare Wi-Fi board: `"false"`

To check your board, look at the install log when it boots: it prints
`chip revision: v1.x` (engineering sample, use `"true"`) or `v3.x` (production, use
`"false"`). If you see `chip revision check failed`, flip the setting and install again.

## Troubleshooting

| Symptom | Likely cause |
|---------|--------------|
| `chip revision check failed` | Wrong `engineering_sample` value; see above |
| Won't compile, can't find a secret | Wi-Fi boards: add `wifi_ssid` and `wifi_password` to the builder's Secrets |
| Wi-Fi board never joins | Wrong Wi-Fi details. It opens its own hotspot (named after the device); join it to set them |
| Silence, or every sample is zero | Wrong pins, or the ADC has no clock |
| Full-scale random noise | Clock/framing mismatch, or a bad MCLK/sample-rate ratio |
| Screech with some structure | Wrong `i2s_format`; try `philips` or `msb` |
| Helicopter / "machine gun" chop | Two clock sources. Feed the ADC's clock from GPIO5 |
| Clean but quiet | Raise the source level or **Line-In Gain** |
| Resets when Sendspin starts playing | The 3V3 supply is sagging. Use a better power source or add bulk capacitance near the board |

## Clocking

The ADC needs a clean clock that divides to the sample rate. The firmware generates
all the I2S clocks itself (BCLK, LRCLK and a 256 x 48 kHz system clock on GPIO5), so
everything is in one clock domain. Running the ADC from its own separate oscillator
instead causes periodic sample slips that sound like a helicopter chop.

The all-in-one 3.5 mm ADC module (the Taidacent PCM1808/PCM1802) runs the other way
round: it makes its own clocks. For it, in the `linein_stream:` block set
`i2s_mode: slave`, remove `mclk_pin`, and wire the module's LRCK and BCK outputs to
GPIO2 and GPIO3, which then act as inputs on the ESP32.

## Reference: `linein_stream`

The custom component that captures the ADC and serves the stream. Daddy's file already
includes it with these settings:

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
  sample_rate: 48000
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

The three **EQ** sliders map to the bands in this order. Add more bands and sliders for
a fuller EQ.

## License

[MIT](LICENSE) © 2026 Drew Gourley
