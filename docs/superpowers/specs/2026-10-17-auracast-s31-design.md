# AES67 → Auracast Bridge auf ESP32-S31 — Design

Stand: 2026-10-17. Status: Entwurf, Review durch Mala ausstehend.

## Zweck

Ein FPGA empfängt AES67 und gibt 48-kHz-Audio als I2S aus. Takt (BCLK, WS) und Daten kommen vom FPGA und sind über PTP an das Netzwerk gebunden. Ein ESP32-S31 nimmt dieses Audio als „stumpfer“ I2S-Slave entgegen und sendet es als Auracast-Broadcast (Bluetooth LE Audio, LC3). Das FPGA muss dafür nichts ESP-Spezifisches tun.

## Ziele

- Der ESP32-S31 empfängt Stereo-PCM 48 kHz als I2S-Slave vom FPGA.
- Er sendet einen Auracast-Broadcast (BAP Broadcast Source plus Public-Broadcast-Announcement), den handelsübliche Auracast-Empfänger finden und abspielen (Smartphone mit Auracast-Unterstützung, LE-Audio-Kopfhörer, Auracast-Lautsprecher).
- Die Taktabweichung zwischen Audiotakt (PTP) und Bluetooth-Takt (Modul-Quarz) gleicht ein Software-Resampler unhörbar aus. Er wird über den Pufferfüllstand nachgeregelt und arbeitet ohne Sample-Slips und ohne periodische Klicks.
- Der Dauerbetrieb läuft stabil: kein Puffer-Über- oder -Unterlauf über Stunden, solange das FPGA einen gültigen Takt liefert.

## Nicht-Ziele (erste Version)

- Kein Standard-Quality-Stream (16/24 kHz) für Hörgeräte. Er kann später als zweite Subgroup dazukommen.
- Keine mehreren Programme oder Sprachen (mehrere BIGs).
- Keine Laufzeitkonfiguration (Web, UART-Kommandos). Konfiguriert wird zur Build-Zeit über menuconfig.
- Kein AES67 oder Ethernet auf dem ESP selbst. Der EMAC des S31 könnte zwar IEEE 1588, das ist aber nicht Teil dieses Projekts.
- Keine Latenzoptimierung unter ~100 ms Ende-zu-Ende.

## Anti-Ziele

Das Projekt ist gescheitert, auch wenn es technisch läuft, wenn

- die Driftkorrektur hörbar ist (Klicks, Aussetzer, Wow/Flutter),
- der Broadcast nach einem Taktverlust am I2S (FPGA rekonfiguriert, PTP-Verlust) stehen bleibt oder einen Neustart braucht,
- das FPGA ESP-spezifisches Timing liefern muss.

## Randbedingungen

**Hardware**
- Board: ESP32-S31-Function-CoreBoard-1, Board-Rev V1.0, Modul ESP32-S31-WROOM-3 (16 MB Flash, 16 MB PSRAM laut Board-Definition; beim ersten Boot prüfen).
- Der 40-MHz-Quarz sitzt im Modul. Ihn durch einen PTP-gelockten Takt zu ersetzen ist auf diesem Board nicht möglich.
- Der On-Board-Codec ES8311 belegt GPIO 50–56 (I2S/I2C), der Verstärker NS4150B hängt an GPIO 57. Beide werden nicht benutzt.
- Ethernet (YT8531) belegt GPIO 4–19. Wird nicht benutzt.

**SDK**
- ESP-IDF Branch `release/v6.1` (HEAD), nicht der Tag `v6.1`. Der Branch enthält den Fix für die ISO-TX-Power (esp-idf#18946, Commit `c526feea98`) und eine neuere S31-Controller-Library (`e62f358958`).
- Der S31 ist in v6.1 ein Preview-Target: `idf.py --preview set-target esp32s31`.
- LE-Audio-Stack: ESP-BLE-AUDIO aus ESP-IDF mit Bluedroid als Host (Default der IDF-Beispiele). ESP-BLE-AUDIO ist laut Doku „Preview“.
- LC3-Encoder: `espressif/esp_audio_codec` ~2.6 (Binary-Library mit S31-Support, Espressif-Lizenz).

**Schnittstelle FPGA → ESP (meine Annahme, bitte prüfen)**
- I2S Philips-Standard, 2 Slots à 32 Bit, also 64 BCLK pro Frame. BCLK 3,072 MHz, WS 48 kHz. Kein MCLK.
- 24-Bit-Daten MSB-first, linksbündig im 32-Bit-Slot.
- 3,3-V-LVCMOS-Pegel.
- Pins an Stiftleiste J2: BCLK → GPIO40 (J2-19), WS → GPIO42 (J2-20), DATA → GPIO39 (J2-21), GND → J2-33. Alle drei Pins sind über menuconfig änderbar. Strapping-Pins (36, 37, 60, 61) werden gemieden.

## Architektur

```
 FPGA (PTP-Takt)                       ESP32-S31 (Quarz-Takt)
 ───────────────                       ──────────────────────────────────────────────────────────────
 BCLK/WS/DATA ──► I2S0 Slave RX ──DMA──► ISR: Kopie in Ring + Zeitstempel
                                          │
                                          ▼
                              SPSC-Ring (int32 Stereo, 4096 Frames)
                                          │  Füllstand (interpoliert über ISR-Zeitstempel)
                                          ▼
 BLE-Controller ──"sent"-Callback──► Encoder-Task (Core 1), einmal pro SDU-Intervall (10 ms):
 (ISO-Intervall,                       PI-Regler → Verhältnis → Resampler (64 Taps, polyphas)
  Quarz-Takt)                          → 480 Frames/Kanal → LC3 (2× mono) → 2 SDUs (BIS L/R)
        ▲                                                                   │
        └───────────────────────────────────────────────────────────────────┘
```

**Taktdomänen.** Alles hinter dem Ring läuft im Quarz-Takt des ESP: Controller, `esp_timer` und APLL leiten sich vom selben 40-MHz-Quarz ab. Die Ausgaberate wird vom BLE-Controller vorgegeben, denn pro bestätigtem SDU wird genau ein neues erzeugt. Der Resampler ist die einzige Stelle, an der beide Domänen aufeinandertreffen.

**Regelung.** Gemessen wird der Füllstand des Rings im Moment jedes Ticks. Damit die DMA-Blockgröße (240 Frames) kein Messrauschen erzeugt, wird der Füllstand mit dem Zeitstempel des letzten DMA-Interrupts interpoliert. Ein PI-Regler mit ~0,02 Hz Bandbreite und einem 1-s-Tiefpass auf der Messung stellt das Verhältnis des Resamplers ein (Begrenzung ±500 ppm). Abweichungen außerhalb des Bereichs gelten als Fehler (falsche Samplerate vom FPGA).

**Zustände der Pipeline.** `NO_SIGNAL` (kein I2S-Takt), dann `PRIME` (Ring bis zum Sollwert füllen), dann `RUN`. Bei Unterlauf, Überlauf oder Signalverlust geht es zurück auf `NO_SIGNAL` bzw. `PRIME`. Außerhalb von `RUN` werden LC3-codierte Stille-Frames gesendet, damit der Broadcast für die Empfänger durchgehend bestehen bleibt.

**Broadcast.** Ein BIG mit einer Subgroup und zwei BIS (Front Left, Front Right), Preset LC3 48_4_1 (48 kHz, 10 ms, 120 Byte/Frame, 96 kbit/s pro Kanal). Ext. Advertising enthält Broadcast Audio Announcement (0x1852, zufällige Broadcast_ID pro Boot), Public Broadcast Announcement (0x1856, High Quality) und Broadcast_Name. Periodic Advertising trägt die BASE. Standardmäßig unverschlüsselt.

**Testbetrieb ohne FPGA.** I2S1 läuft als Master mit APLL und absichtlich verstimmter Rate (48002 Hz ≈ +41,7 ppm) auf denselben Pins. Die Eingänge von I2S0 werden über die GPIO-Matrix intern daraufgelegt, es sind keine Kabel nötig. Damit lassen sich Driftregelung, Taktverlust (Ton aus/an) und Ende-zu-Ende-Audio testen. Im Testbetrieb darf das FPGA nicht angeschlossen sein.

**Latenz (Schätzung).** Ring ~20 ms + LC3-Frame 10 ms + LC3-Lookahead ~2,5 ms + Transport bis 20 ms + Presentation Delay 40 ms ≈ 75–95 ms.

## Entscheidungen

| Entscheidung | Von wem |
|---|---|
| ESP ist I2S-Slave, Takt kommt vom FPGA | Mala |
| Taktausgleich per Software-Resampler im ESP | Mala |
| ESP-IDF 6.1, Board CoreBoard-1 V1.0 | Mala |
| Branch `release/v6.1` statt Tag | Claude (Bugfixes) |
| ESP-BLE-AUDIO + Bluedroid, nicht GMF `esp_bt_audio` (kein PBP, SD-Karten-Pipeline) | Claude |
| LC3 über `esp_audio_codec`, liblc3 als Ausweichlösung | Claude |
| Preset 48_4_1, Stereo als 2 BIS, unverschlüsselt, Name „AES67 Auracast“ | Claude (in menuconfig änderbar) |
| I2S-Format 2×32 Bit Philips, Pins GPIO40/42/39 | Claude (bitte mit FPGA abgleichen) |
| Pacing über den „sent“-Callback des Controllers, Watchdog 50 ms | Claude |
| Resampler: Float, 64 Taps, 128 Phasen, Kaiser-Fenster, lineare Phaseninterpolation | Claude |
| Logs und Flashen über den CP2102N-UART-Port | Claude (wegen esp-idf#19221) |

## Offen

- Welches Auracast-Empfangsgerät für Tests zur Verfügung steht.
- Ob das FPGA-I2S-Format zur Annahme oben passt.
- Zulässige Sendeleistung am Einsatzort. Default +9 dBm, über menuconfig änderbar.
