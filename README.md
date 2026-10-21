# AES67 → Auracast Bridge (ESP32-S31)

Firmware for the **ESP32-S31-Function-CoreBoard-1** (V1.0) that turns an AES67 audio stream into an **Auracast**
broadcast (Bluetooth LE Audio, LC3). It is built for live sound: the PA feed goes straight into hearing aids and
LE Audio receivers, so latency is kept low (about 60 ms from the I2S input to playback in the default mode).

An FPGA receives AES67 and outputs 48 kHz stereo PCM over I2S, clocked from PTP. The ESP32-S31 is the I2S slave.
Its Bluetooth clock comes from the module crystal and drifts against PTP. A software resampler, steered by a
control loop on the input FIFO fill level, absorbs that drift. A host controller configures the stream at runtime
over **I2C**. On the target board this is a second ESP running Zephyr, the board's network and control stack.

```
AES67 ──► FPGA ──I2S (48 kHz, PTP clock)──► ESP32-S31 ──LE Audio (LC3, 1 or 2 BIS)──► hearing aids,
                                                ▲                                        LE Audio receivers
                       Zephyr host ───I2C───────┘  (configuration, START/RESET, status)
```

## Contents

- [Features](#features)
- [Hardware and wiring](#hardware-and-wiring)
- [Building and flashing](#building-and-flashing)
- [Operation](#operation)
- [I2C control protocol](#i2c-control-protocol)
- [Build-time configuration](#build-time-configuration)
- [Serial log](#serial-log)
- [Bench testing without a host](#bench-testing-without-a-host)
- [PC tools](#pc-tools)
- [Latency](#latency)
- [Known issues and limitations](#known-issues-and-limitations)
- [Repository layout and tests](#repository-layout-and-tests)
- [Verification log](#verification-log)

## Features

- **I2S slave input:** 48 kHz, 2 × 32-bit slots, 24-bit data. Clock drift is compensated by an adaptive
  polyphase resampler driven by a PI controller. The firmware also detects a missing clock or a wrong input rate.
- **Auracast source:** BAP broadcast source with a Public Broadcast Announcement and a Broadcast_Name, optionally
  encrypted.
- **LC3 presets** (all 10 ms frames): 16_2_1 and 24_2_1 (Standard Quality, receivable by hearing aids), and
  48_2_1 / 48_4_1 / 48_6_1 (High Quality).
- **Channel modes:** one mono BIS carrying (L+R)/2, left only or right only; or two BIS (front left / front right).
- **Two subgroups in one BIG (optional):** a Standard Quality subgroup for hearing aids plus a 48 kHz High
  Quality subgroup for headphones, up to 4 BIS. The Public Broadcast Announcement then advertises SQ and HQ, and
  every receiver picks the subgroup it can decode.
- **Runtime configuration over I2C:** preset, channel mode, streaming context, Immediate Rendering flag,
  presentation delay, TX power, FIFO set point, Broadcast_ID, name and code. A status block is readable over I2C.
- **Low-latency defaults for hearing aids:** 24_2_1 mono, *Live* context, Immediate Rendering flag, 20 ms
  presentation delay.
- **PC tools:** subscribe to the broadcast with BlueZ, and measure the end-to-end latency against the AES67 reference.

## Hardware and wiring

All signals go to header **J2**. Levels are 3.3 V LVCMOS. The GPIOs can be changed in `idf.py menuconfig`.

| Signal | From | J2 pin | GPIO |
|---|---|---|---|
| I2S BCLK (3.072 MHz) | FPGA | J2-19 | GPIO40 |
| I2S WS / LRCLK (48 kHz) | FPGA | J2-20 | GPIO42 |
| I2S DATA | FPGA | J2-21 | GPIO39 |
| I2C SDA | host | J2-17 | GPIO43 |
| I2C SCL | host | J2-18 | GPIO44 |
| GND | | J2-33 | — |

The internal pull-ups are enabled on SDA and SCL, but the bus should still have proper external pull-ups.

### I2S format

- Philips I2S. The ESP is the slave: BCLK and WS come from the FPGA. No MCLK.
- 48000 Hz, 2 slots × 32 bit (64 BCLK per frame), 24-bit data MSB first, left-aligned in the slot.
- **DATA is sampled on the falling BCLK edge** by default (`Sample DATA on the falling BCLK edge`):
  - The FPGA changes DATA shortly after the rising edge.
  - The S31 slave samples 80–120 ns after the edge, because it synchronises its inputs to an internal clock of
    8 × BCLK. On the rising edge it therefore hits the data transition: bits arrive one clock early or at random,
    which is heard as fast clicking.
  - A DAC samples exactly on the edge and never notices.
  - A transmitter that changes DATA on the falling edge, as the I2S specification says, needs this option off.

## Building and flashing

Requires **ESP-IDF `release/v6.1`**, not the v6.1 tag: the branch has the BIS TX-power fix (esp-idf#18946) and
updated S31 Bluetooth controller libraries. The S31 is a preview target in v6.1.

```bash
. ~/.espressif/tools/activate_idf_v6.1.sh
idf.py --preview set-target esp32s31      # once
idf.py build
PORT=/dev/serial/by-id/usb-Silicon_Labs_CP2102N_USB_to_UART_Bridge_Controller_<serial>-if00-port0
idf.py -p $PORT flash
python tools/serial_log.py --port $PORT --seconds 10 --reset      # boot log
```

Flash and log through the **CP2102N USB-UART port** at 115200 baud. The USB Serial/JTAG port of this board drops
out after a few seconds (esp-idf#19221). From a non-interactive shell, run everything inside one
`bash -c '. ~/.espressif/tools/activate_idf_v6.1.sh && …'`: `idf.py` and the IDF `python` are only on the `PATH`
after activation.

## Operation

1. **Boot:** Bluetooth is initialised and the I2C slave starts, about 0.6 s after reset. Nothing is broadcast yet.
   `STATE` reads `WAIT_CONFIG`.
2. **Configure:** the host writes the configuration registers in any order. Registers it leaves alone keep their
   power-on values (the Live defaults).
3. **START:** the S31 validates the configuration.
   - If a value is invalid, it reports `CONFIG_ERROR` with the offending register and keeps waiting.
   - Otherwise it starts the I2S input, the encoder pipeline and the broadcast: `STARTING`, then `STREAMING` once
     all BIS are up (about 0.5 s).
4. **Change:** the configuration is locked while broadcasting. To change it, the host sends **RESET**; the S31
   restarts and goes back to step 1.

The configuration lives in RAM only. After any restart, including a watchdog reset or brownout, the S31 waits for
the host again. `RESET_REASON` tells the host why it restarted.

Inside the pipeline, the encoder task produces one SDU per BIS whenever the controller has room in its queue,
paced by the Bluetooth "sent" callbacks. The I2S input goes through these states:

```
NO_SIGNAL ──clock present──► PRIME ──FIFO at set point, rate OK──► RUN
RUN ──clock lost──► NO_SIGNAL        RUN ──underrun / overflow──► PRIME
RUN or PRIME ──input rate off by more than ±600 ppm (e.g. 44.1 kHz)──► RATE_ERROR ──5 s──► NO_SIGNAL
```

Outside `RUN` the broadcast keeps running and carries encoded silence. After a gap, the controller restarts from
the last learned clock ratio (warm start).

## I2C control protocol

The complete protocol is defined in
[`components/ctrl_regs/include/auracast_i2c_regs.h`](components/ctrl_regs/include/auracast_i2c_regs.h). The header
depends only on `<stdint.h>` and is meant to be copied into the host firmware.

### Bus rules

- 7-bit address **0x3A** (Kconfig), 100 or 400 kHz.
- 8-bit register address with auto-increment (0xFF wraps to 0x00). Multi-byte values are **little endian**.
- **Write:** `[reg][data…]`, for example Zephyr `i2c_burst_write()`.
- **Read:** `[reg]`, repeated start, read N bytes, for example Zephyr `i2c_burst_read()` / `i2c_write_read()`.
  - **Always set the register address before reading.** A read without it returns leftovers of the previous read.
  - **Exactly 31 bytes per read**, one fill of the hardware TX FIFO. Read NAME (32 bytes) and the status block
    (44 bytes) in two parts (e.g. 0x80 and 0x8D). Bytes beyond 31 are invalid. A shorter read leaves bytes in the
    FIFO; the slave hardware then pre-loads one of them into its shift register, where no FIFO reset reaches it,
    and that byte prefixes every later read until a 32-byte read drains it. Reading the whole fill avoids this.
- **Clock stretching:** at the start of a read the S31 holds SCL low until the data is ready, typically well below
  1 ms. The host's I2C master must allow clock stretching.
- **Rejected writes are still ACKed.** They set `ERROR` and `ERROR_REG` instead. Both hold the last rejection and
  are cleared by an accepted START.
- **NAME and CODE:** always write the whole field, padded with 0x00. Otherwise the tail of the old value remains.

### Register map

Power-on values are in **bold**.

| Addr | Register | Size | Access | Content |
|---|---|---|---|---|
| 0x00 | MAGIC | 4 | R | `'A' 'U' 'R' 'A'` |
| 0x04 | PROTO_VERSION | 1 | R | 2 (protocol 1 plus the HQ registers 0x50…0x53) |
| 0x05 | FW_VERSION | 3 | R | major, minor, patch |
| 0x08 | STATE | 1 | R | 0 WAIT_CONFIG, 1 STARTING, 2 STREAMING, 3 CONFIG_ERROR, 4 FAULT |
| 0x09 | ERROR | 1 | R | 0 NONE, 1 READ_ONLY, 2 LOCKED, 3 BAD_VALUE, 4 BAD_COMMAND, 5 START_FAILED |
| 0x0A | ERROR_REG | 1 | R | register address the error refers to |
| 0x0B | RESET_REASON | 1 | R | `esp_reset_reason_t` of the last restart |
| 0x0C | COMMAND | 1 | W | 0x5A START, 0xA5 RESET (reads 0) |
| 0x10 | PRESET | 1 | RW | 0 16_2_1, **1 24_2_1**, 2 48_2_1, 3 48_4_1, 4 48_6_1 |
| 0x11 | CHANNELS | 1 | RW | **0 mono (L+R)/2**, 1 stereo (2 BIS), 2 mono left, 3 mono right |
| 0x12 | CONTEXT | 2 | RW | streaming context bit mask, non-zero, bits 0x0FFF only (**0x0040 Live**, 0x0004 Media) |
| 0x14 | FLAGS | 1 | RW | bit 0: Immediate Rendering flag (**1**); other bits must be 0 |
| 0x15 | TX_POWER | 1 | RW | int8 dBm, −15…20 (**9**), rounded down to a supported level |
| 0x16 | PD_US | 2 | RW | presentation delay, 20000…40000 µs (**20000**) |
| 0x18 | FIFO_TARGET | 2 | RW | input FIFO set point, 840…3000 frames at 48 kHz (**840**) |
| 0x1A | BROADCAST_ID | 4 | RW | 0…0xFFFFFF, or **0xFFFFFFFF = random** at every start |
| 0x20 | NAME | 32 | RW | Broadcast_Name, UTF-8, 4…32 bytes, padded with 0x00 (**`AES67 Auracast`**) |
| 0x40 | CODE | 16 | RW | Broadcast_Code, 0…16 bytes, padded with 0x00; **empty = unencrypted** |
| 0x50 | HQ_PRESET | 1 | RW | second subgroup (High Quality): 2 48_2_1, 3 48_4_1, 4 48_6_1, **0xFF = none**; requires PRESET = 16_2_1 or 24_2_1 |
| 0x51 | HQ_CHANNELS | 1 | RW | channel mode of the HQ subgroup, values as CHANNELS (**1 stereo**) |
| 0x52 | HQ_CONTEXT | 2 | RW | streaming context of the HQ subgroup (**0x0004 Media**) |
| 0x80 | Status block | 44 | R | see below, refreshed every 500 ms |

Configuration registers are writable in `WAIT_CONFIG` and `CONFIG_ERROR`; afterwards writes are ignored with
`ERROR = LOCKED`. Writes to read-only or reserved addresses set `ERROR = READ_ONLY`. Reserved addresses read 0.

At START the S31 checks every range above and requires a name of at least 4 bytes. A value the Bluetooth stack
still refuses leads to `FAULT` / `START_FAILED`; only RESET helps.

**Two subgroups.** With `HQ_PRESET` set, the BIG carries the Standard Quality subgroup (PRESET / CHANNELS /
CONTEXT) and the High Quality subgroup (the HQ registers). **Order in the BIG:** a stereo subgroup comes first so
its pair is BIS 1 and 2 (real receivers only synchronise to a stereo pair there, see Known issues); when both or
neither are stereo, SQ comes first. The start log names the order. All BIS of a BIG share one QoS: SDU interval
10 ms, the larger SDU size, the longer transport latency (20 ms) and the presentation delay `PD_US`. Every
sub-event is sized for the larger PDU, so the retransmission count is reduced until the BIG fits the ISO interval
(see Known issues). A master that never writes 0x50 gets exactly the protocol 1 behaviour.

### Status block

The status block is meaningful from `STARTING` on.

| Addr | Field | Size | Content |
|---|---|---|---|
| 0x80 | PIPE_STATE | 1 | 0 NO_SIGNAL, 1 PRIME, 2 RUN, 3 RATE_ERROR |
| 0x81 | STATUS_FLAGS | 1 | bit 0: I2S clock present, bit 1: all BIS streaming |
| 0x82 | FIFO_FILL | 2 | input FIFO fill, frames at 48 kHz |
| 0x84 | DRIFT_CPPM | 4 | int32, resampler correction in 0.01 ppm |
| 0x88 | IN_CPPM | 4 | int32, measured input rate against 48 kHz in 0.01 ppm |
| 0x8C | I2S_BITS | 4 | OR of all raw I2S slot words since the last refresh; must end in `00` for 24-bit data |
| 0x90 | UNDERRUN | 4 | FIFO underruns |
| 0x94 | OVERFLOW | 4 | FIFO overflows |
| 0x98 | LOST | 4 | I2S clock losses while running |
| 0x9C | SEND_ERR | 4 | SDUs refused by the controller |
| 0xA0 | SENT | 4 | SDU sets sent |
| 0xA4 | ACTIVE_ID | 4 | Broadcast_ID in use |
| 0xA8 | UPTIME_MS | 4 | ms since boot |

### Example (Zephyr, sketch)

```c
#include "auracast_i2c_regs.h"

const uint8_t addr = AURA_I2C_DEFAULT_ADDR;
uint8_t magic[4], state;

i2c_burst_read(bus, addr, AURA_REG_MAGIC, magic, sizeof(magic));     /* "AURA" */

uint8_t preset = AURA_PRESET_48_4_1, channels = AURA_CH_STEREO;
uint8_t ctx[2] = {0x04, 0x00};                                         /* Media */
uint8_t name[AURA_NAME_SIZE] = "Main Hall";                            /* rest zero-padded */
i2c_burst_write(bus, addr, AURA_REG_PRESET, &preset, 1);
i2c_burst_write(bus, addr, AURA_REG_CHANNELS, &channels, 1);
i2c_burst_write(bus, addr, AURA_REG_CONTEXT, ctx, sizeof(ctx));
i2c_burst_write(bus, addr, AURA_REG_NAME, name, sizeof(name));

/* Optional: add a 48 kHz stereo subgroup for headphones next to a 24_2_1 mono subgroup for hearing aids */
uint8_t hq[4] = {AURA_PRESET_48_4_1, AURA_CH_STEREO, 0x04, 0x00};    /* HQ_PRESET, HQ_CHANNELS, HQ_CONTEXT */
i2c_burst_write(bus, addr, AURA_REG_HQ_PRESET, hq, sizeof(hq));

uint8_t cmd = AURA_CMD_START;
i2c_burst_write(bus, addr, AURA_REG_COMMAND, &cmd, 1);
i2c_burst_read(bus, addr, AURA_REG_STATE, &state, 1);                 /* STARTING, later STREAMING */
```

## Build-time configuration

`idf.py menuconfig` → *AES67 Auracast bridge*. Everything that describes the stream is set over I2C. Kconfig only
holds what depends on the board.

| Option | Default | Meaning |
|---|---|---|
| I2S input → BCLK / WS / DATA GPIO | 40 / 42 / 39 | pins on J2 |
| Sample DATA on the falling BCLK edge | on | see [I2S format](#i2s-format); check with `i2s bits` |
| Slave sampling clock (bclk_div) | 8 × BCLK | internal sampling clock of the slave; 16 was worse on the FPGA |
| I2C control → slave address | 0x3A | 7-bit |
| I2C control → SDA / SCL GPIO | 43 / 44 | pins on J2 |
| I2C control → AUTOSTART | off | broadcast right after boot with the power-on values, without a host (bench use) |
| AUTOSTART: channel mode / HQ preset / HQ channels | 0 / 255 / 1 | written into registers 0x11, 0x50 and 0x51 before the automatic START |
| Audio source | I2S | `SYNTH`: 440/880 Hz generated internally, no I2S (receiver tests) |
| Test mode (no FPGA) | off | see [Bench testing](#bench-testing-without-a-host) |
| Debug: skip send / ignore completion | 0 | only for testing the self-healing TX pacing |

## Serial log

The pipeline logs a status line every 5 s:

```
pipeline: state=RUN fill=840.0 ppm=-0.07 in_ppm=-0.87 underrun=0 ovf=0 lost=0 senderr=0 wd=1 tick_us=[9674..10348] mean=10000.0 proc_us_max=2090 proc_us_mean=1850 late=0 sent=11795 heap=99328 bt_ppm=+0.72
pipeline: zero_sdus=0 pcm peak dBFS: -15.1 -14.3 -240.0 -240.0
pipeline: i2s bits=0xffffff00
```

| Field | Meaning |
|---|---|
| `state` | input state, see [Operation](#operation) |
| `fill` | estimated FIFO fill in frames, mean over the window (target = FIFO_TARGET) |
| `ppm` | resampler ratio − 1: I2S clock against the Bluetooth clock |
| `in_ppm` | measured I2S input rate against `esp_timer` since the signal appeared; should be ≈ `ppm` + `bt_ppm` |
| `underrun` / `ovf` | FIFO ran empty / overflowed (each falls back to `PRIME`) |
| `lost` | I2S clock losses in `RUN` |
| `senderr` / `wd` | send errors / watchdog refills of the controller queue (one at start-up is normal) |
| `tick_us` / `mean` | interval of the controller's SDU confirmations, min..max / mean |
| `proc_us_max` / `proc_us_mean` | longest / mean processing time per SDU set (resampler + LC3), budget 10 000 µs |
| `late` | SDU sets in the window that took longer than 10 ms (the two queued SDUs absorb single ones) |
| `sent` | confirmed SDU sets since start |
| `heap` | free heap in bytes |
| `bt_ppm` | SDU tick rate against `esp_timer`, cumulative |

`zero_sdus` counts SDUs sent as all-zero bytes (LC3 silence) in the window, `pcm peak` is the peak level fed to
each of the up to four encoders (−240 = unused BIS). `i2s bits` is the OR of all raw 32-bit slot words in the window. For 24-bit data the low byte must be `00`. A
value ending in `80` means DATA is sampled one bit early, which points to the wrong BCLK edge; one ending in `01`
means one bit late.

## Bench testing without a host

- **AUTOSTART** (Kconfig): broadcasts right after boot with the power-on values (Live 24_2_1 mono). The channel
  mode, the HQ preset and the HQ channel mode can be set next to it (registers 0x11, 0x50, 0x51), so a two-subgroup
  broadcast runs without a host; for anything else change the `AURA_DEFAULT_*` values in `auracast_i2c_regs.h`
  locally. The I2C slave still runs, and its status is still readable.
- **Synthetic source** (`Audio source → SYNTH`): 440 Hz left, 880 Hz right, generated directly in the encoder task
  (no I2S, no resampler). Use it to test receivers.
- **Test tone** (`Test mode → Generate test tone`). **Disconnect the FPGA first**, otherwise two sources drive the
  same pins.
  - I2S1 runs as master on the input pins (440/880 Hz, −12 dBFS) and is looped back into I2S0 through the GPIO
    matrix.
  - The tone rate is deliberately off 48 kHz to create drift. The XTAL divider is not exact: 48002 Hz is really
    +41.74 ppm, 47998 Hz −34.04 ppm, and 48100 Hz +2081 ppm (the "wrong rate" case).
  - Further options delay the tone start, or switch it off for 2 s every 30 s (clock loss and recovery).
  - In test mode the log adds `tone check (after resampler): L=…Hz R=…Hz`.

## PC tools

All tools are in `tools/`. They run on a Linux PC with BlueZ and PipeWire.

### Listening: `auracast_listen.py`

```bash
tools/auracast_listen.py          # subscribes to "AES67 Auracast" until Ctrl-C (--name, --address, --keep-scanning)
```

- **Requirements:** BlueZ with `Experimental = true` and
  `KernelExperimental = 6fbaf188-05e0-496a-9885-d6ddfdb4e03e` in `/etc/bluetooth/main.conf`, a controller with
  `sync-receiver` (`btmgmt info`), and PipeWire with LC3.
- **Behaviour:**
  - The script removes a stale device object first; without that, BlueZ does not resync after a board reset.
  - It scans, selects every BIS, and subscribes again when the broadcast disappears.
  - PipeWire then offers one `bluez_input.*` source per BIS. WirePlumber usually routes it to the default speakers
    at once: `pw-link -d` disconnects it, `pw-record --target …` records it.

### Latency: `measure_latency.py`

```bash
/usr/bin/python3 tools/measure_latency.py --subscribe --seconds 20 --pd-us 20000 --fifo-frames 840
/usr/bin/python3 tools/measure_latency.py --pcap capture.pcapng --pd-us 20000      # evaluate an existing capture
```

**How it works:**
- One `dumpcap` captures both the AES67 multicast and the PC's Bluetooth monitor, so both sides share the PC clock.
- It decodes L24 and LC3 (with `liblc3`) and aligns the two signals by cross-correlation. PipeWire is not in the
  measurement path.
- **Time reference:**
  - The RTP timestamp marks the capture of the first sample.
  - The packet reaches the PC 1 ms + 29 µs (sender) + 12 µs (switch) later.
  - The FPGA at the ESP outputs each sample on I2S 96 samples (2 ms) after capture.

**What it reports for each BIS:**
- which AES67 channel the BIS carries;
- the latency, measured in 2–6 windows: the median, with a warning if the spread exceeds 0.5 ms;
- lost SDUs and lost packets;
- disturbed 10 ms blocks. A block counts as disturbed when one of its 1 ms sections has an SNR below 1 dB and at
  least 6 dB more error than `liblc3` on the same reference.

**Special cases:**
- Silence or purely periodic content gives "ambiguous" instead of a number.
- A silent BIS, or a restart of the board or the AES67 sender, is reported; the longest segment is then used.

**Requirements:**
- `dumpcap` with capabilities (group `wireshark`), `liblc3.so.1` and numpy;
- the PC in the AES67 network (`--iface`, `--group`; the AES67 daemon must be running so the multicast reaches the
  interface);
- without a host, firmware built with AUTOSTART.

`--subscribe` starts `auracast_listen.py` inside the capture, so *BIG Sync Established* (the transport latency) is
recorded too. The audio then plays on the default speakers.

### Other tools

- `auracast_listen.py --bis 1`: select only some BIS of the BIG (comma-separated BIS numbers), e.g. one
  subgroup; by default every BIS is selected and PipeWire gets one source per BIS.
- `serial_log.py --port PORT --seconds N [--reset]`: serial log through the CP2102N.
- `sine_check.py recording.wav`: checks a recording of a 440 Hz sine block by block.
- Tests: `/usr/bin/python3 -m unittest discover -s tools -p 'test_*.py'`.

## Latency

Measured with `measure_latency.py` on 2026-10-19, FPGA input with music:

| Configuration | FIFO | I2S input → SDU at the PC | Rendered at the receiver* |
|---|---|---|---|
| 48_4_1 stereo, Media, PD 40 ms | 960 | 37.0 ms (BIS 1) / 40.3 ms (BIS 2) | 80.3 … 86.9 ms |
| 48_4_1 stereo, Media, PD 40 ms | 840 | 34.5 / 37.8 ms | 77.8 … 84.4 ms |
| 24_2_1 mono, Live, PD 20 ms | 960 | 42.8 ms | 62.8 … 64.0 ms |
| 24_2_1 mono, Live, PD 20 ms | 900 | 41.6 ms | 61.6 … 62.7 ms |
| **24_2_1 mono, Live, PD 20 ms (default)** | **840** | **40.3 ms** | **60.3 … 61.5 ms** |

Two subgroups (2026-10-20, FIFO 840, PD 20 ms, SQ Live + HQ Media; BIS in BIG order):

| Configuration | BIS | I2S input → SDU at the PC | Rendered at the receiver* |
|---|---|---|---|
| 24_2_1 mono + 48_6_1 stereo | 3 | 35.3 ms (SQ) / 36.8, 39.2 ms (HQ) | 55.3 … 65.6 ms |
| 24_2_1 stereo + 48_6_1 stereo | 4 | 36.1, 37.7 ms (SQ) / 38.4, 40.0 ms (HQ) | 56.1 … 66.4 ms |
| 24_2_1 stereo + 48_4_1 stereo | 4 | 34.5, 36.5 ms (SQ) / 37.5, 39.5 ms (HQ) | 54.5 … 65.9 ms |
| 24_2_1 mono alone, regression | 1 | 40.7 ms | 60.7 … 61.8 ms |

With two subgroups the SQ BIS arrive *earlier* than in the single 24_2_1 configuration: the drift-corrected
signal stays at 48 kHz (64-tap resampler, 0.7 ms) and the SQ path adds a 128-tap FIR decimator (1.3 ms), which
together is less than the single configuration's 128-tap decimating resampler plus its longer wait in the
controller queue (see below). The BIG transport latency is that of the larger PDU (6.4 ms).

\* **How the range is computed:**
- The lower bound is arrival + presentation delay.
- The upper bound adds the BIG transport latency from *BIG Sync Established*: 6.6 ms for 48_4_1, 1.2 ms for 24_2_1.
- With the Immediate Rendering flag, receivers may render earlier; how much earlier depends on the device.
- Add 2 ms to get the latency from AES67 capture instead of from the I2S input.
- Two runs per setting agreed within 0.1 ms.

**Where the time goes, up to the arrival:**
- FIFO set point: 17.5–20 ms
- LC3 frame: 10 ms
- LC3 algorithmic delay: 2.5 ms
- resampler: 0.7 ms (1.3 ms at 24 kHz)
- waiting in the controller TX queue: 2 SDUs

The 24 kHz configuration arrives later than 48 kHz despite its shorter transport latency. Its radio event is
shorter, so the confirmation comes earlier after the anchor point and the next SDU waits longer in the queue. It is
still about 20 ms faster to playback because of the 20 ms presentation delay. 840 frames is the smallest FIFO set
point that runs without underruns, given the block and DMA granularity.

## Known issues and limitations

- **I2C path: first host bring-up done (Zephyr on ESP32-S3, 2026-10-20).** The S31 answers its address and the
  register file is reachable. Found and fixed on the bench: a register-less read after a short read left bytes in
  the TX FIFO, the next fill overflowed into the driver's ring buffer, and from then on every `write_read` returned
  stale register contents. The TX FIFO is now reset right before every fill (`handle_request`), so the ring buffer
  is never used.
  - **Bring-up check:** do a short read, then `write_read` one byte at 0x08. It must return STATE, not
    leftover bytes.
- **Reads must take exactly 31 bytes.** A shorter read leaves data in the TX FIFO, and the slave hardware pre-loads
  the next byte into its shift register. `i2c_slave_reset_tx_fifo()` (FIFO + FSM reset) would clear it, but the
  IDF allows that only after a completed read, for which the slave gets no callback; the plain FIFO reset the
  firmware does before every fill and at every write does not reach the shift register. Seen on the bench
  (2026-10-20): after one 1-byte read every `write_read` returned one stale byte in front of the real data, for
  good. Reading the full 31 bytes drains the FIFO, so nothing is pre-loaded; a 32-byte read recovers a shifted
  slave once.
- **PipeWire 1.6.2 plays 24 kHz broadcasts choppy and metallic** (19–30 % correct blocks). The radio data is
  correct: the SDUs captured over HCI and decoded with liblc3 are complete and match the AES67 reference. 48 kHz
  plays cleanly (98–99.7 %), including 48 kHz mono with Live QoS. To listen on a PC, use a 48 kHz preset; for hearing
  aids, 24_2_1 remains the recommended setting.
- **Channel swap at the FPGA:** in stereo, BIS 1 (announced as left) carries the **right** AES67 channel. The cause
  is open; WS polarity between FPGA and ESP is suspected.
- **Free heap depends on the preset**, because the resampler's filter table grows with the decimation factor:
  117 KB at 48 kHz stereo, 99 KB at 24 kHz mono, 57 KB at 16 kHz stereo. With two subgroups the resampler runs at
  48 kHz (32 KB table) and each extra LC3 encoder costs 6–9 KB: 95 KB free with 3 BIS, 86 KB with 4 BIS.
- **Two subgroups share one BIG QoS, and airtime limits the retransmissions.** Every sub-event of the BIG is sized
  for the larger PDU, also for the 60-byte SQ BIS. The controller rejects a BIG that does not fit the 10 ms ISO
  interval (`BT_HCI: opcode=0x2068, status= 11: Unsupported Value`, state `FAULT`), so the firmware reduces the RTN
  with an airtime model (`components/audio_dsp/big_airtime.c`, budget 8.5 ms, 4 MIC bytes per PDU when
  encrypted; a reduction is logged at start): 3 BIS with 48_6_1 get RTN 2,
  4 BIS with 48_4_1 RTN 2, 4 BIS with 48_6_1 RTN 1. The SQ BIS get the same RTN as the HQ BIS, so with a
  stereo SQ subgroup and 48_6_1 the hearing-aid stream has only one retransmission; prefer 48_4_1 or SQ mono.
  Separate BIGs would use the airtime better (How-To guide, Figure 3.3) but the controller supports one BIG
  per advertising set here.
- **Encoder load with 4 BIS is close to the 10 ms frame:** mean 7.7 ms, maximum 8.3–9.7 ms per frame set with
  24_2_1 stereo + 48_6_1 stereo (3 BIS: max. 7.2–7.8 ms). Runs of 2–5 min show no underrun, send error or
  watchdog refill, because two SDUs per BIS are queued in the controller (20 ms of slack). A 4-BIS
  configuration should be checked with `proc_us_max` and `late` in the status line after any firmware change; the
firmware logs a warning at start for 4 BIS with 48_6_1.
- **A silent capture means a silent input, not a broken BIG.** Several 4-BIS measurements on 2026-10-20/21
  decoded every BIS as LC3 silence (all-zero SDUs) while the AES67 stream on the network carried music; the cause
  was an FPGA that was not configured and fed silence over I2S. The status line now shows it directly:
  `pcm peak dBFS` −240 on every BIS and `zero_sdus` counting up. Check these before suspecting the radio.
- **No Audio_Channel_Allocation at subgroup level.** The firmware first announced stereo subgroups with
  `FL|FR` in the level-2 codec configuration next to the per-BIS FL / FR entries (level 3). Real receivers (a
  hearing-aid class Auracast receiver and Sennheiser headphones) then treated the 48 kHz BIS as two-channel
  (2 × 120 octets), showed the broadcast as "Unknown" and refused it, while BlueZ on the PC decoded it. The
  24 kHz stereo subgroup passed (2 × 60 octets). Since 2026-10-21 only the level-3 entries carry the
  allocation, as in the Auracast guides' BASE examples; the firmware logs the advertising payloads at start.
- **Receivers take a stereo pair only at BIS 1 and 2.** With the SQ mono subgroup first, the HQ stereo pair sat
  at BIS 2 and 3; the same receivers and headphones that play "HQ stereo alone" (BIS 1 and 2) and "SQ stereo +
  HQ mono" could not subscribe, with PD 20 and 40 ms alike, while the PC decoded the pair (slowly and only on a
  second attempt). With the stereo subgroup moved to the front (2026-10-21) "SQ mono + HQ stereo" plays. Hence
  **one stereo subgroup per BIG**: with SQ stereo + HQ stereo the second pair (BIS 3 and 4) is expected to fail
  on such receivers; use SQ mono + HQ stereo, or SQ stereo + HQ mono.
- **Board:** the ES8311 codec occupies I2S pins 52–56, so the FPGA connects through J2 and the GPIO matrix. Avoid
  the strapping pins 36/37/60/61.
- **No real hearing aid tested yet.** AirPods cannot receive Auracast.

## Repository layout and tests

```
main/
  main.c            boot, waits for START, starts pipeline and broadcast
  ctrl_i2c.c        I2C slave front end (ISR → queue → task), status refresh
  broadcast.c       BAP broadcast source, PBP announcement, presets, TX pacing
  adv_bluedroid.c   extended + periodic advertising (Bluedroid)
  pipeline.c        encoder task: I2S FIFO → drift control → resampler → LC3
  i2s_in.c          I2S slave input (DMA → SPSC ring)
  test_tone.c       I2S1 test-tone master for bench tests
components/
  audio_dsp/        resampler, drift controller, rate check, SPSC ring, TX pacing (pure C, host-testable)
  ctrl_regs/        I2C register file and protocol header (pure C, host-testable)
tools/              PC tools (listening, latency measurement, logs) and their tests
```

Host tests (Linux, gcc):

```bash
cmake -S components/audio_dsp/host_test -B build_host && cmake --build build_host && ctest --test-dir build_host
cmake -S components/ctrl_regs/host_test -B build_host_ctrl && cmake --build build_host_ctrl && ctest --test-dir build_host_ctrl
/usr/bin/python3 -m unittest discover -s tools -p 'test_*.py'
```

## Verification log

| Date | What | Result |
|---|---|---|
| 2026-10-17 | Chip | ESP32-S31 rev v0.0, 40 MHz crystal, CPU 320 MHz, 16 MB flash (PSRAM not used) |
| 2026-10-17 | I2S slave clock | needs the APLL: 2 × 32 bit at 48 kHz requires ≥ 24.576 MHz with divider ≥ 2; the 40 MHz XTAL is not enough |
| 2026-10-17 | LC3 encoder, 2 × mono 48 kHz / 120 B | 6.0 ms per frame pair from flash (DIO), 4.6 ms (QIO), 3.0 ms with the code in IRAM (`main/lc3_iram.lf`) |
| 2026-10-17 | Drift, test tone at +41.74 ppm, 10 min | always `RUN`, controller mean +41.75 ppm, all error counters 0 |
| 2026-10-17 | Drift, test tone at −34.04 ppm, 2 min | controller −32.7 … −35.3 ppm, all error counters 0 |
| 2026-10-17 | Late I2S clock, clock gaps (2 s every 30 s) | starts in `NO_SIGNAL`; each gap `RUN → NO_SIGNAL → PRIME → RUN` with warm start, no reset |
| 2026-10-17 | Wrong rate (+2081 ppm, 44.1 kHz) | `RATE_ERROR` with the measured rate in the log, nothing played, retry every 5.5 s |
| 2026-10-17 | Every 500th completion callback dropped, 60 s | always `RUN`, pacing recovers through sequence numbers |
| 2026-10-19 | Reception on a Linux PC (BlueZ 5.87, PipeWire 1.6.2, MediaTek MT7902) | announcement, PA/BASE/BIG sync and LC3 decode correct; synth tones 440.0 / 880.0 Hz, > 72 dB separation |
| 2026-10-19 | FPGA input (AES67, music) | PTP-locked: `in_ppm` −0.7 … +2.5, controller ±5 ppm, all error counters 0 (after switching to the falling BCLK edge) |
| 2026-10-19 | Live mode 24_2_1 mono | 1 BIS without channel allocation, context Live, PBA with SQ and Immediate Rendering metadata; processing max. 1.8 ms per 10 ms (48_4_1 stereo: 4.9 ms) |
| 2026-10-19 | Glitch check against the AES67 reference | 48_4_1 and 24_2_1, 9 recordings of 20–60 s: 0 disturbed blocks, 0 lost SDUs |
| 2026-10-19 | I2C control firmware at the FPGA, no host | without AUTOSTART: waits, a 30 s scan finds no broadcast. With AUTOSTART: 24_2_1 mono 2 min `RUN` with all counters 0; 16_2_1 mono and stereo, 48_4_1 stereo `RUN` |
| 2026-10-21 | Real Auracast receivers (hearing-aid class device, Sennheiser headphones), two subgroups | mono + mono, SQ stereo + HQ mono and (stereo subgroup first) SQ mono + HQ stereo play; with the level-2 channel allocation the broadcast showed as "Unknown", with the stereo pair at BIS 2/3 it could not be subscribed |
| 2026-10-20 | Two subgroups (SQ + HQ) in one BIG | 24_2_1 mono + 48_6_1 stereo (3 BIS) and 24_2_1 stereo + 48_6_1 / 48_4_1 stereo (4 BIS): 2–5 min `RUN`, underrun/ovf/lost/senderr 0, PBA with SQ and HQ; PC receives all BIS, latency per BIS measured (see Latency); single-subgroup regression 24_2_1 mono 40.7 ms |
| 2026-10-20 | BIG airtime | controller rejects 3 BIS × 155 B at RTN 4 and 4 BIS × 155 B at RTN 2 (`Unsupported Value`), accepts 4 BIS × 120 B at RTN 2 and 4 BIS × 155 B at RTN 1 |
| 2026-10-20 | Encoder load, two subgroups | 3 BIS (48_6_1): max. 7.2–7.8 ms per 10 ms; 4 BIS (48_6_1): mean 7.7 ms, max. 8.3–9.7 ms; free heap 95 / 86 KB |
| open | Hearing aid / consumer LE Audio receiver | — |
| open | I2C control with the Zephyr host | — |
