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
mkdir -p logs && python tools/serial_log.py --port $PORT --seconds 10 --reset | tee logs/boot.log
```

Flashen und Logs laufen über den **USB-UART-Port (CP2102N)** mit **921600 Baud** (`serial_log.py` nutzt das als Default;
die ROM-Zeilen am Anfang kommen mit 115200 Baud und erscheinen als Zeichensalat — beim Auswerten `grep -a` verwenden). Der USB-Serial/JTAG-Port
bricht auf diesem Board nach einigen Sekunden weg (esp-idf#19221).

## Konfiguration (menuconfig → *AES67 Auracast bridge*)

| Option | Default | Bedeutung |
|---|---|---|
| I2S input → BCLK/WS/DATA GPIO | 40 / 42 / 39 | Pins an J2 |
| Input FIFO fill set point | 960 Frames (20 ms) | Sollwert des Driftreglers; bestimmt einen Teil der Latenz |
| Broadcast name | `AES67 Auracast` | Name, den Empfänger anzeigen (Broadcast_Name) |
| Broadcast code | leer | leer = unverschlüsselt, sonst max. 16 Byte |
| LC3 broadcast preset | 48_4_1 | 48 kHz, 10 ms, 120 Byte (96 kbit/s pro Kanal); alternativ 48_2_1 / 48_6_1 |
| TX power | +9 dBm | wird auf die nächstkleinere unterstützte Stufe abgerundet |
| Audio source | I2S | `SYNTH` = 440/880 Hz direkt, ohne I2S (Empfänger testen) |

## Testbetrieb ohne FPGA

**FPGA vorher abziehen**, sonst treiben zwei Quellen dieselben Pins.

- `Test mode → Generate test tone`: I2S1 erzeugt als Master 440 Hz links / 880 Hz rechts (−12 dBFS)
  auf den Eingangspins; die I2S0-Eingänge werden intern über die GPIO-Matrix zurückgeführt.
- `Test tone sample rate`: absichtlich neben 48000 Hz, um Drift zu erzeugen. Der XTAL-Teiler
  trifft nicht jede Rate exakt: 48002 Hz → real +41,74 ppm, 47998 Hz → real −34,04 ppm,
  48100 Hz → real +2081 ppm (Fehlerfall „falsche Samplerate“).
- `Delay before the test tone starts`: prüft den Start mit spätem I2S-Signal.
- `Switch the test tone off for 2 s every 30 s`: prüft Taktverlust und Wiederanlauf.
- Im Testbetrieb loggt die Pipeline zusätzlich `tone check (after resampler): L=…Hz R=…Hz`
  — damit lassen sich Kanalzuordnung und Resampling ohne Empfänger prüfen.

## Statuszeile lesen

Alle 5 s:

```
pipeline: state=RUN fill=960.0 ppm=+41.62 in_ppm=+41.74 underrun=0 ovf=0 lost=0 senderr=0 wd=1 tick_us=[9824..10249] mean=10000.0 proc_us_max=5261 sent=14389 heap=114316 bt_ppm=+0.41
```

| Feld | Bedeutung |
|---|---|
| `state` | `NO_SIGNAL` (kein I2S-Takt) → `PRIME` (FIFO füllt sich) → `RUN`; `RATE_ERROR` = Eingangsrate außerhalb ±500 ppm (nach 5 s neuer Versuch). Außerhalb von `RUN` wird Stille gesendet. Nach einer Taktlücke startet der Regler mit dem zuletzt gelernten Verhältnis (Warmstart). |
| `fill` | geschätzter FIFO-Füllstand in Frames, Mittel im 5-s-Fenster (Soll 960) |
| `ppm` | Resampler-Verhältnis − 1 (Mittel im Fenster): Abweichung des I2S-Takts gegenüber dem Bluetooth-Takt |
| `in_ppm` | gemessene I2S-Eingangsrate gegenüber `esp_timer`, kumulativ — sollte ≈ `ppm` + `bt_ppm` sein |
| `underrun` / `ovf` | FIFO leer gelaufen / übergelaufen (jeweils Rückfall nach `PRIME`) |
| `lost` | Anzahl I2S-Taktverluste im Zustand `RUN` |
| `senderr` / `wd` | Sendefehler / Watchdog-Nachfüllungen der Controller-Queue |
| `tick_us` / `mean` | Abstand der SDU-Bestätigungen des Controllers (min..max / Mittel im 5-s-Fenster) |
| `proc_us_max` | max. Rechenzeit pro SDU-Paar (Resampler + 2 × LC3), Budget 10 000 µs |
| `sent` | bestätigte SDU-Paare seit Start |
| `heap` | freier Heap in Byte |
| `bt_ppm` | Rate der SDU-Ticks gegenüber `esp_timer`, kumulativ seit Streamstart |

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
| 2026-10-17 | Pipeline-Rechenzeit (Resampler + 2 × LC3) | max. 5,6 ms pro 10 ms (Resampler ~2,2 ms nach Umstellung auf Festkomma-Positionen) |
| 2026-10-17 | Drift 10 min, Testton real +41,74 ppm | durchgehend `RUN`, Füllstand 959,5–960,6, Regler-Mittel +41,75 ppm (5-s-Fenster 40,0–43,8), alle Fehlerzähler 0, Heap ≥ 111 KB |
| 2026-10-17 | Drift 2 min, Testton real −34,04 ppm | Regler −32,7 … −35,3 ppm, Füllstand 960, alle Fehlerzähler 0 |
| 2026-10-17 | Start mit 5 s verspätetem I2S-Takt | Broadcast startet in `NO_SIGNAL`, nach Taktbeginn `PRIME → RUN` |
| 2026-10-17 | Taktlücken (2 s alle 30 s, 4×) | je `RUN → NO_SIGNAL → PRIME → RUN`, `lost` +1, Warmstart mit gelerntem Verhältnis, Ticks lückenlos, kein Reset |
| 2026-10-17 | Falsche Rate (real +2081 ppm) | `RATE_ERROR` ~13 s nach `RUN`, zyklischer Wiederanlauf ohne Reset, `in_ppm` zeigt +2081 |
| offen | Hörtest | Braucht einen Auracast-Empfänger (AirPods können kein Auracast) |
| offen | Betrieb am FPGA | Testton aus, FPGA an J2; `in_ppm`/`ppm` zeigen die reale Abweichung PTP ↔ Modul-Quarz |

## Latenz (geschätzt)

FIFO-Sollwert 20 ms + Resampler 0,7 ms + LC3-Frame 10 ms + LC3-Lookahead ~2,5 ms + Transport ≤ 20 ms
+ Presentation Delay 40 ms ≈ **75–95 ms** Ende-zu-Ende. Kleinere FIFO-Sollwerte senken die Latenz, lassen aber
weniger Reserve für Jitter.
