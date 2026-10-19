# AES67 → Auracast Bridge (ESP32-S31)

Firmware für das **ESP32-S31-Function-CoreBoard-1** (V1.0). Der ESP32-S31 empfängt
48-kHz-Stereo-PCM als I2S-Slave von einem FPGA (AES67, PTP-Takt) und sendet es als
Auracast-Broadcast (Bluetooth LE Audio, LC3). Die Taktabweichung zwischen PTP und dem
Modul-Quarz gleicht ein geregelter Software-Resampler aus.

Die Stream-Konfiguration kommt zur Laufzeit über **I2C** (S31 = Slave) von der Zephyr-Firmware des
Audio-Interface-Boards: Nach dem Boot wartet der S31, bis der Master konfiguriert und START schickt (siehe
[I2C-Steuerung](#i2c-steuerung)). Einstellbar sind u. a.:

- **Preset:** 16_2_1, 24_2_1 (Standard Quality, für Hörgeräte) oder 48_2_1 / 48_4_1 / 48_6_1 (High Quality), alle
  mit 10-ms-Frames.
- **Kanäle:** ein Mono-BIS (L+R)/2, nur L oder nur R, oder zwei BIS links/rechts.
- Name, Broadcast-Code, Streaming-Kontext, Immediate-Rendering-Flag, Presentation Delay, Sendeleistung,
  FIFO-Sollwert, Broadcast_ID.

Die Werte nach dem Boot entsprechen dem **Live-Modus für Hörgeräte:** ein Mono-BIS (L+R)/2, LC3 24_2_1 (24 kHz,
60 Byte), Kontext *Live*, Immediate-Rendering-Flag, Presentation Delay 20 ms. Für Livesound, bei dem die Zuhörer
auch den Raum hören: Viele Hörgeräte empfangen nur Standard Quality, und die Latenz ist kleiner. Bisheriger
HQ-Modus = 48_4_1, Stereo, Kontext *Media* (0x0004), 40 ms.

Design: [docs/superpowers/specs/2026-10-17-auracast-s31-design.md](docs/superpowers/specs/2026-10-17-auracast-s31-design.md)

## Verdrahtung (FPGA → Stiftleiste J2)

| Signal | J2-Pin | GPIO |
|---|---|---|
| BCLK (3,072 MHz) | J2-19 | GPIO40 |
| WS / LRCLK (48 kHz) | J2-20 | GPIO42 |
| DATA | J2-21 | GPIO39 |
| I2C SDA (vom Master) | J2-17 | GPIO43 |
| I2C SCL (vom Master) | J2-18 | GPIO44 |
| GND | J2-33 | — |

Pegel 3,3 V LVCMOS. Für I2C sind die internen Pull-ups an; am Bus gehören trotzdem externe Pull-ups hin. Die GPIOs lassen sich in `idf.py menuconfig` → *AES67 Auracast bridge* ändern.

## I2S-Format

- Philips-I2S, ESP ist Slave (BCLK und WS kommen vom FPGA), kein MCLK
- 48000 Hz, 2 Slots × 32 Bit (64 BCLK pro Frame)
- 24-Bit-Daten MSB-first, linksbündig im 32-Bit-Slot
- Der ESP tastet DATA per Default auf der **fallenden** BCLK-Flanke ab (`Sample DATA on the falling BCLK edge`):
  Der FPGA wechselt DATA kurz nach der steigenden Flanke, und der S31-Slave tastet erst 80–120 ns nach der Flanke
  ab (Synchronisierung mit 8 × BCLK). Auf der steigenden Flanke trifft er den Datenwechsel: Bits einen Takt zu
  früh oder zufällig, hörbar als schnelles Klacken (~17 Hz, Schwebung zwischen den Takten). Ein DAC tastet direkt
  auf der Flanke ab und merkt davon nichts. Ein Sender, der DATA I2S-konform auf der fallenden Flanke wechselt,
  braucht die Option aus.

## Build & Flash

```bash
. ~/.espressif/tools/activate_idf_v6.1.sh      # ESP-IDF release/v6.1
idf.py --preview set-target esp32s31           # S31 ist in v6.1 ein Preview-Target
idf.py build
PORT=/dev/serial/by-id/usb-Silicon_Labs_CP2102N_USB_to_UART_Bridge_Controller_64a7b6c7d0bcf011a704167148e9de0f-if00-port0
idf.py -p $PORT flash
mkdir -p logs && python tools/serial_log.py --port $PORT --seconds 10 --reset | tee logs/boot.log
```

Flashen und Logs laufen über den **USB-UART-Port (CP2102N)** mit **115200 Baud** (Default von `serial_log.py`). Der USB-Serial/JTAG-Port
bricht auf diesem Board nach einigen Sekunden weg (esp-idf#19221).

## Konfiguration (menuconfig → *AES67 Auracast bridge*)

| Option | Default | Bedeutung |
|---|---|---|
| I2S input → BCLK/WS/DATA GPIO | 40 / 42 / 39 | Pins an J2 |
| Sample DATA on the falling BCLK edge | an | siehe I2S-Format; Kontrolle über `i2s bits=` |
| Slave sampling clock (bclk_div) | 8 × BCLK | interner Abtasttakt des Slaves; verschiebt den Abtastzeitpunkt (16 war am FPGA schlechter) |
| I2C control → slave address | 0x3A | 7 Bit |
| I2C control → SDA / SCL GPIO | 43 / 44 | Pins an J2 |
| I2C control → Start at boot (AUTOSTART) | aus | sendet sofort mit den Werten nach dem Boot, ohne I2C-Master (Tisch-Aufbau, Latenzmessung) |
| Audio source | I2S | `SYNTH` = 440/880 Hz direkt, ohne I2S (Empfänger testen) |
| Debug: skip send / ignore completion | 0 | nur für Tests der Pacing-Selbstheilung |

## I2C-Steuerung

Das Protokoll steht vollständig in
[components/ctrl_regs/include/auracast_i2c_regs.h](components/ctrl_regs/include/auracast_i2c_regs.h). Der Header
hängt nur von `<stdint.h>` ab und kann so in die Zephyr-Firmware übernommen werden.

- 7-Bit-Adresse 0x3A, 100 oder 400 kHz. 8-Bit-Registeradresse, Auto-Inkrement, Mehrbytewerte Little Endian.
- Schreiben: `[reg][daten…]` (Zephyr: `i2c_burst_write`). Lesen: `[reg]`, Repeated Start, N Byte
  (`i2c_write_read`). **Vor jedem Lesen den Registerzeiger setzen**: Ein Lesen ohne vorangehendes Schreiben liefert
  Reste des letzten Lesezugriffs.
- **Höchstens 31 Byte pro Lesezugriff** (eine Füllung der Hardware-FIFO). NAME (32 Byte) und den Statusblock
  (44 Byte) in zwei Zugriffen lesen. Mehr gelesene Bytes sind ungültig.
- NAME und CODE immer **ganz** schreiben, mit 0x00 aufgefüllt; sonst bleibt der Rest des alten Werts stehen.
- Beim Lesen hält der S31 SCL fest, bis die Antwort bereitsteht (typisch deutlich unter 1 ms). Der Master muss
  **Clock Stretching** zulassen.
- Abgelehnte Schreibzugriffe werden trotzdem geACKt; sie setzen `ERROR` und `ERROR_REG`. Die beiden zeigen die
  letzte Ablehnung; erst ein angenommenes START löscht sie.

Ablauf:

1. Nach dem Boot ist `STATE` = WAIT_CONFIG (0); es wird nichts gesendet. `MAGIC` (0x00) liest `AURA`.
2. Der Master schreibt die Konfiguration (0x10–0x4F), beliebige Reihenfolge, nur was von den Werten nach dem Boot
   abweicht.
3. `COMMAND` (0x0C) = 0x5A (START): Der S31 prüft alles. Bei einem Fehler ist `STATE` = CONFIG_ERROR (3),
   `ERROR` = BAD_VALUE (3) und `ERROR_REG` zeigt auf das Feld; korrigieren und START erneut. Sonst STARTING (1), nach
   etwa 0,5 s STREAMING (2). FAULT (4) heißt: Der Bluetooth-Stack hat abgelehnt, nur RESET hilft.
4. Ab STARTING sind die Konfigurationsregister gesperrt (`ERROR` = LOCKED). Ändern: `COMMAND` = 0xA5 (RESET). Der
   S31 startet etwa 10 ms später neu und antwortet nach rund 0,6 s wieder auf seine Adresse (der I2C-Slave startet
   nach der Bluetooth-Initialisierung; im Log bei 553 ms nach dem App-Start). Die Konfiguration liegt nur im RAM: Nach jedem Neustart, auch
   durch Watchdog oder Brownout, wartet der S31 wieder (`RESET_REASON` 0x0B sagt, warum er neu gestartet ist).

| Adr. | Register | Bytes | Inhalt (Wert nach dem Boot) |
|---|---|---|---|
| 0x00 | MAGIC | 4 | `AURA` |
| 0x04 | PROTO_VERSION / FW_VERSION | 1 / 3 | 1 / 1.0.0 |
| 0x08 | STATE, ERROR, ERROR_REG, RESET_REASON | je 1 | s. o. |
| 0x0C | COMMAND | 1 | 0x5A START, 0xA5 RESET |
| 0x10 | PRESET | 1 | 0 = 16_2_1, **1 = 24_2_1**, 2 = 48_2_1, 3 = 48_4_1, 4 = 48_6_1 |
| 0x11 | CHANNELS | 1 | **0 = Mono (L+R)/2**, 1 = Stereo (2 BIS), 2 = Mono L, 3 = Mono R |
| 0x12 | CONTEXT | 2 | Streaming-Context-Bitmaske, nur Bits 0x0FFF (**0x0040 Live**, 0x0004 Media) |
| 0x14 | FLAGS | 1 | Bit 0 Immediate Rendering (**1**) |
| 0x15 | TX_POWER | 1 | int8 dBm, −15…20 (**9**) |
| 0x16 | PD_US | 2 | Presentation Delay 20000…40000 µs (**20000**) |
| 0x18 | FIFO_TARGET | 2 | FIFO-Sollwert 840…3000 Frames (**840**) |
| 0x1A | BROADCAST_ID | 4 | 0…0xFFFFFF, **0xFFFFFFFF = zufällig** pro Start |
| 0x20 | NAME | 32 | UTF-8, 4…32 Byte, mit 0x00 aufgefüllt (**AES67 Auracast**) |
| 0x40 | CODE | 16 | Broadcast-Code, mit 0x00 aufgefüllt; **leer = unverschlüsselt** |
| 0x80 | Status | 44 | Pipeline-Zustand, I2S-Signal/BIS-Flags, FIFO-Füllung, Drift und Eingangsrate (0,01 ppm), `i2s bits`, Zähler (underrun, overflow, lost, send_err, sent), aktive Broadcast_ID, Uptime; alle 500 ms aktualisiert |

Host-Tests der Registerdatei:

```bash
cmake -S components/ctrl_regs/host_test -B build_host_ctrl && cmake --build build_host_ctrl && ctest --test-dir build_host_ctrl --output-on-failure
```

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
| `state` | `NO_SIGNAL` (kein I2S-Takt) → `PRIME` (FIFO füllt sich) → `RUN`; `RATE_ERROR` = gemessene Eingangsrate weicht mehr als ±600 ppm von 48 kHz ab, z. B. 44,1 kHz (Fehlermeldung mit der gemessenen Rate, nach 5 s neuer Versuch). In `PRIME` wird die Rate 0,5 s lang gemessen, bevor `RUN` beginnt. Außerhalb von `RUN` wird Stille gesendet. Nach einer Taktlücke startet der Regler mit dem zuletzt gelernten Verhältnis (Warmstart). |
| `fill` | geschätzter FIFO-Füllstand in Frames, Mittel im 5-s-Fenster (Soll 960) |
| `ppm` | Resampler-Verhältnis − 1 (Mittel im Fenster): Abweichung des I2S-Takts gegenüber dem Bluetooth-Takt |
| `in_ppm` | gemessene I2S-Eingangsrate gegenüber `esp_timer`, seit Signalbeginn (startet nach jeder Lücke neu) — sollte ≈ `ppm` + `bt_ppm` sein |
| `underrun` / `ovf` | FIFO leer gelaufen / übergelaufen (jeweils Rückfall nach `PRIME`) |
| `lost` | Anzahl I2S-Taktverluste im Zustand `RUN` |
| `senderr` / `wd` | Sendefehler / Watchdog-Nachfüllungen der Controller-Queue (Pacing über Sequenznummern; ein verlorener Bestätigungs-Callback korrigiert sich beim nächsten) |
| `tick_us` / `mean` | Abstand der SDU-Bestätigungen des Controllers (min..max / Mittel im 5-s-Fenster) |
| `proc_us_max` | max. Rechenzeit pro SDU-Paar (Resampler + 2 × LC3), Budget 10 000 µs |
| `sent` | bestätigte SDU-Paare seit Start |
| `heap` | freier Heap in Byte |
| `bt_ppm` | Rate der SDU-Ticks gegenüber `esp_timer`, kumulativ seit Streamstart |

Darunter steht alle 5 s `i2s bits=0x…`, das ODER aller rohen 32-Bit-Slotwörter im Fenster. Bei 24-Bit-Daten muss
das untere Byte `00` sein. `…80` bedeutet, dass ein Bit zu früh abgetastet wird (BCLK-Flanke falsch), `…01`,
dass es ein Bit zu spät ist. Bei einem Sender mit echten 32-Bit-Daten ist die Maske nicht aussagekräftig.

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
| 2026-10-17 | Falsche Rate 44,1 kHz (nach Review-Fix) | `RATE_ERROR` 0,5 s nach Signalbeginn mit „44100 Hz“ im Log, kein Abspielen, keine Unterläufe, Wiederholversuch alle 5,5 s |
| 2026-10-17 | Jeder 500. Bestätigungs-Callback verworfen, 60 s | durchgehend `RUN`, Füllstand 960, `wd` unverändert — Pacing heilt sich über Sequenznummern |
| 2026-10-19 | Empfang am Linux-PC (BlueZ 5.87 + PipeWire 1.6.2, MediaTek MT7902), Synth-Quelle | Auracast-Ankündigung (0x1852 + ID, 0x1856 HQ, Name) korrekt; BlueZ synchronisiert PA/BASE/BIG und legt 2 Transports an; PipeWire decodiert LC3: links 440,0 Hz, rechts 880,0 Hz, je −15 dBFS RMS, > 72 dB Abstand, keine Aussetzer in 6 s |
| 2026-10-19 | Betrieb am FPGA (AES67, Musik) | I2S-Takt PTP-gerastet: `in_ppm` −0,7 … +2,5, Regler ±5 ppm um den Sollwert, alle Fehlerzähler 0. Daten erst nach Umstellen auf die fallende BCLK-Flanke sauber (siehe I2S-Format) |
| 2026-10-19 | Live-Modus 24_2_1 mono, BlueZ | 1 Transport, 24 kHz/10 ms/60 B ohne Channel-Allocation, Location 0, PD 20000, Kontext Live (0x0040); PBA 0x1856 mit Feature SQ (0x02) und Metadaten `01 09` (Immediate Rendering); Rechenzeit max. 1,8 ms pro 10 ms (HQ: 4,9 ms) |
| 2026-10-19 | Störungsfreiheit (Referenzvergleich mit `measure_latency.py`) | HQ und Live, 9 Aufzeichnungen à 20–60 s: 0 gestörte 10-ms-Blöcke, 0 verlorene SDUs |
| 2026-10-19 | Kanalzuordnung am FPGA | BIS 1 (angekündigt links) trägt den **rechten** AES67-Kanal, BIS 2 den linken. Ursache offen (WS-Polarität FPGA ↔ ESP?) |
| 2026-10-19 | I2C-Steuerung am FPGA, ohne I2C-Master | Ohne AUTOSTART: wartet, 30 s Scan findet keinen Broadcast. Mit AUTOSTART: Live 24_2_1 2 min `RUN`, alle Fehlerzähler 0, `i2s bits` …00, Heap 99 KB; 16_2_1 mono `RUN`, vom PC empfangen, Rechenzeit max. 1,8 ms, Heap 67 KB; 48_4_1 stereo `RUN`, 2 BIS, Heap 117 KB. Die I2C-Strecke selbst ist noch nicht getestet |
| offen | Hörtest mit Hörgerät / Consumer-Empfänger | z. B. LE-Audio-Kopfhörer (AirPods können kein Auracast) |

### Empfang am Linux-PC nachvollziehen

Voraussetzungen: BlueZ mit `Experimental = true` und `KernelExperimental = 6fbaf188-05e0-496a-9885-d6ddfdb4e03e`
in `/etc/bluetooth/main.conf`, Controller mit `sync-receiver` (`btmgmt info`), PipeWire mit LC3.

```bash
tools/auracast_listen.py         # abonniert "AES67 Auracast", bis Ctrl-C (--name, --address, --keep-scanning)
```

Das Skript entfernt ein altes Geräteobjekt (BlueZ synchronisiert nach einem Board-Reset sonst nicht neu), scannt,
wählt beide BIS aus und abonniert von selbst neu, wenn der Broadcast wegfällt. PipeWire bietet danach je BIS eine
Quelle `bluez_input.<MAC>…` an, die WirePlumber meist sofort auf die Standard-Lautsprecher legt (`pw-link -d` trennt,
`pw-record --target …` nimmt auf). Von Hand geht dasselbe mit `bluetoothctl`:

**Bekannter Fehler am PC (PipeWire 1.6.2):** Den Live-Modus (24 kHz) gibt PipeWire zerhackt und metallisch wieder.
Die Funkdaten sind dabei korrekt: Die per HCI mitgeschnittenen SDUs, mit liblc3 dekodiert, sind lückenlos und
stimmen mit der AES67-Referenz überein. Gemessen am 2026-10-19 durch blockweisen Vergleich der PipeWire-Quelle mit
genau diesen SDUs: Live 24 kHz nur 19–30 % korrekte Blöcke mit laufenden Zeitsprüngen; HQ 48 kHz stereo 99,7 %;
dieselbe Live-Ankündigung mit 48 kHz mono (auch mit Live-QoS RTN 2 / 10 ms) 98 %. Auslöser ist allein die
Abtastrate 24 kHz im BAP-Empfangspfad von PipeWire, nicht Presentation Delay, Mono, Kontext oder QoS. Zum Hören am PC
einen 48-kHz-Preset nehmen; für Hörgeräte bleibt 24_2_1 der empfohlene Standard.

```bash
remove <MAC>                     # nach einem Board-Reset
scan le                          # findet "AES67 Auracast", legt sid0/bis1 und sid0/bis2 als Transports an
transport.select /org/bluez/hci0/dev_<MAC>/sid0/bis1/fd0 /org/bluez/hci0/dev_<MAC>/sid0/bis2/fd1
```

## Latenz (gemessen)

`tools/measure_latency.py` schneidet mit einem `dumpcap` den AES67-Stream (den das FPGA am ESP ausgibt) und den
Bluetooth-Monitor des PCs auf derselben Uhr mit, dekodiert L24 und LC3 (`liblc3`) und richtet beide per
Kreuzkorrelation aus. PipeWire ist nicht im Messpfad. Zeitbezug: RTP-Timestamp = Erfassung des ersten Samples;
das Paket ist 1 ms + 29 µs (Sender) + 12 µs (SG350) später am PC; das FPGA am ESP gibt jedes Sample 96 Samples (2 ms)
nach der Erfassung als I2S aus.

| Modus | FIFO | I2S-Eingang → SDU am PC | gerendert beim Hörgerät* |
|---|---|---|---|
| HQ 48_4_1, PD 40 ms (vorher) | 960 | 37,0 ms (BIS 1) / 40,3 ms (BIS 2) | 80,3 … 86,9 ms |
| HQ 48_4_1, PD 40 ms | 840 | 34,5 / 37,8 ms | 77,8 … 84,4 ms |
| Live 24_2_1, PD 20 ms | 960 | 42,8 ms | 62,8 … 64,0 ms |
| Live 24_2_1, PD 20 ms | 900 | 41,6 ms | 61,6 … 62,7 ms |
| **Live 24_2_1, PD 20 ms (Default)** | **840** | **40,3 ms** | **60,3 … 61,5 ms** |

\* Untergrenze: Ankunft + Presentation Delay; Obergrenze: zusätzlich die BIG-Transportlatenz aus *BIG Sync
Established* (HQ 6,6 ms, Live 1,2 ms). Mit Immediate Rendering dürfen Hörgeräte früher rendern
(gerätespezifisch). Ab AES67-Erfassung kommen jeweils 2 ms dazu. Zwei Läufe je Einstellung lagen ≤ 0,1 ms auseinander.

Die Strecke bis zur Ankunft enthält: FIFO-Sollwert (17,5–20 ms), LC3-Frame 10 ms, LC3-Algorithmus 2,5 ms, Resampler
0,7 ms (Live 1,3 ms) und die Wartezeit in der Sende-Queue (2 SDUs, Takt der Sende-Bestätigungen). Im Live-Modus ist
das Funkereignis kürzer, die Bestätigung kommt früher nach dem Anker und die nächste SDU wartet länger. Deshalb ist
die Ankunft dort trotz kürzerer Transportlatenz später als bei HQ; gerendert ist Live wegen 20 statt 40 ms
Presentation Delay trotzdem ~20 ms schneller.

```bash
/usr/bin/python3 tools/measure_latency.py --subscribe --seconds 20 --pd-us 20000 --fifo-frames 840
/usr/bin/python3 tools/measure_latency.py --pcap capture.pcapng --pd-us 20000    # vorhandenen Mitschnitt auswerten
/usr/bin/python3 -m unittest discover -s tools -p 'test_*.py'                    # Tests der PC-Werkzeuge
```

Voraussetzungen: `dumpcap` mit Capabilities (Gruppe `wireshark`), `liblc3.so.1`, numpy, der PC im AES67-Netz
(`--iface`, `--group`). `--subscribe` startet `auracast_listen.py` innerhalb des Mitschnitts, damit auch *BIG Sync
Established* (Transportlatenz) darin landet; dabei läuft das Audio über die Standard-Lautsprecher. Das Tool meldet je
BIS außerdem, welchen AES67-Kanal es trägt, verlorene SDUs/Pakete und gestörte 10-ms-Blöcke. Gestört heißt: ein
1-ms-Abschnitt mit SNR < 1 dB, der mindestens 6 dB mehr Fehler hat als liblc3 auf derselben Referenz; Stellen, die
für LC3 bei dieser Bitrate einfach schwer sind, zählen so nicht, ebenso wenig vom PC verdeckte Verluste. Die Latenz
wird in 2–6 Fenstern bestimmt (Median, Warnung bei > 0,5 ms Spanne). Stille oder rein periodischer Inhalt ergibt
„mehrdeutig“ statt einer Zahl, ein stummer BIS wird gemeldet, ein Neustart von Board oder AES67-Sender ebenso (dann
zählt das längste Segment). Defaults `--pd-us 20000 --fifo-frames 840` entsprechen den Werten nach dem Boot
(Live-Modus); zum Messen ohne I2C-Master die Firmware mit AUTOSTART bauen.

`tools/sine_check.py aufnahme.wav` prüft eine Aufnahme mit 440-Hz-Sinus blockweise (für Tests mit Sinus statt
Musik).
