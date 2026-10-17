# AES67 → Auracast Bridge (ESP32-S31)

Firmware für das **ESP32-S31-Function-CoreBoard-1** (V1.0). Der ESP32-S31 empfängt
48-kHz-Stereo-PCM als I2S-Slave von einem FPGA (AES67, PTP-Takt) und sendet es als
Auracast-Broadcast (Bluetooth LE Audio, LC3). Die Taktabweichung zwischen PTP und dem
Modul-Quarz gleicht ein geregelter Software-Resampler aus.

Design: [docs/superpowers/specs/2026-10-17-auracast-s31-design.md](docs/superpowers/specs/2026-10-17-auracast-s31-design.md)

## Verdrahtung (FPGA → Stiftleiste J2)

| Signal | J2-Pin | GPIO |
|---|---|---|
| BCLK (3,072 MHz) | J2-19 | GPIO40 |
| WS / LRCLK (48 kHz) | J2-20 | GPIO42 |
| DATA | J2-21 | GPIO39 |
| GND | J2-33 | — |

Pegel 3,3 V LVCMOS. Die GPIOs lassen sich in `idf.py menuconfig` → *AES67 Auracast bridge* ändern.

## I2S-Format

- Philips-I2S, ESP ist Slave (BCLK und WS kommen vom FPGA), kein MCLK
- 48000 Hz, 2 Slots × 32 Bit (64 BCLK pro Frame)
- 24-Bit-Daten MSB-first, linksbündig im 32-Bit-Slot

## Build & Flash

```bash
. ~/.espressif/tools/activate_idf_v6.1.sh      # ESP-IDF release/v6.1
idf.py --preview set-target esp32s31           # S31 ist in v6.1 ein Preview-Target
idf.py build
PORT=/dev/serial/by-id/usb-Silicon_Labs_CP2102N_USB_to_UART_Bridge_Controller_64a7b6c7d0bcf011a704167148e9de0f-if00-port0
idf.py -p $PORT flash
python tools/serial_log.py --port $PORT --seconds 10 --reset
```

Flashen und Logs laufen über den **USB-UART-Port (CP2102N)**. Der USB-Serial/JTAG-Port
bricht auf diesem Board nach einigen Sekunden weg (esp-idf#19221).

## Messwerte

| Datum | Was | Ergebnis |
|---|---|---|
| 2026-10-17 | Chip | ESP32-S31 Revision v0.0, 40-MHz-Quarz, CPU 320 MHz (App-Log; esptool nennt 300 MHz) |
| 2026-10-17 | Flash | 16 MB erkannt. PSRAM ist nicht aktiviert (nicht benötigt), Größe ungeprüft |
| 2026-10-17 | I2S-Slave-Takt | Braucht die APLL: 2×32 Bit bei 48 kHz verlangen intern ≥ 24,576 MHz mit Teiler ≥ 2, XTAL (40 MHz) reicht nicht |
| 2026-10-17 | Testton 48002 Hz (XTAL, Teiler 3+88/345 → real +41,74 ppm) | gemessen +41,73 … +41,79 ppm, Töne 440,0/880,0 Hz, kein Überlauf |
| 2026-10-17 | Testton 47998 Hz (XTAL, Teiler 3+12/47 → real −34,04 ppm) | gemessen −34,03 … −34,07 ppm |
| 2026-10-17 | LC3-Encoder, 2 × mono 48 kHz/10 ms/120 B | 6,0 ms aus Flash (DIO) → 4,6 ms (QIO) → 3,0 ms (Code im IRAM, `main/lc3_iram.lf`); ASM-Variante allein bringt nichts |
| 2026-10-17 | Broadcast, Synth-Quelle, 70 s | Tick-Mittel 10 000,0 µs, Spanne 9,5–10,6 ms (keine Bündelung), senderr=0, wd=0, Rechenzeit max. 3,2 ms/Paar, Heap frei 224 KB |
| 2026-10-17 | Selbstheilung (jedes 500. Paar nicht gesendet), 60 s | Ticks unverändert 10 000 µs, kein Stillstand, Queue füllt sich beim nächsten Tick wieder auf |
| offen | Hörtest | Braucht einen Auracast-Empfänger (AirPods können kein Auracast) |
