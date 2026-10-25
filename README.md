# KC87 Pico Recorder

KC87 Kassetten-Recorder Emulator mit Raspberry Pi Pico (RP2040) oder Pico 2 (RP2350).

## Status

**✅ Recording (Aufnahme): funktional**  
**✅ Auswertung am PC: Blöcke, Prüfsummen, BASIC-Programmname, TAP-Export** (in JKCEMU geladen und gestartet)  
❌ Playback (Wiedergabe): Noch nicht implementiert

## Projektstruktur

```
kc87_pico_recorder/
├── firmware/            Pico-Firmware (C, Pico SDK, PIO)
├── pc_commandline_tool/ PC-Kommandozeilentool kc87tool (C++17)
├── hardware/            KiCad-Schaltplan & PCB
├── doc/                 Dokumentation
├── PROTOCOL.md          Protokoll-Dokumentation
└── README.md
```

## Hardware

- Raspberry Pi Pico (RP2040) oder Pico 2 (RP2350), optional mit Debug-Probe
- **GPIO 2**: Eingang für das KC87-Signal (KC87 → Pico, Programm-Save)
- **GPIO 3**: Playback-Ausgang (`PLAY_SIGNAL_PICO` — Pico → KC87), im Schaltplan vorgesehen, in der Firmware noch nicht implementiert

Maßgeblich für die Pinbelegung ist der Schaltplan in `hardware/`.

## Firmware

- Quellen: `firmware/main.c`, `firmware/pulse_timer.pio` (Target `kc87_logger`)
- Ein PIO-Zustandsautomat misst abwechselnd die Dauer der High- und Low-Phasen an GP2
- Timing-Auflösung: 0,2 µs (5 MHz Zähltakt, 32-Bit-Zähler)
- Die Messwerte laufen per DMA in einen 32-KB-Ringpuffer; daher gehen auch bei kurzen USB-Verzögerungen keine Pulse verloren
- Übertragung über USB-CDC (siehe [PROTOCOL.md](PROTOCOL.md))
- Gesteuert vom PC: `S` startet die Aufnahme, `X` beendet sie

### Protokoll (Kurzfassung)

Alle Wörter sind `uint32` little-endian:

| Richtung | Inhalt |
|----------|--------|
| PC → Pico | `'S'` Start, `'X'` Stopp |
| Pico → PC | Header: `"KC87"` (`0x3738434B`), `tick_hz` |
| Pico → PC | Daten: Dauer in Ticks, abwechselnd High, Low, High, … |
| Pico → PC | Ende: `0xFFFFFFFF`, Status, Anzahl Werte |

Status: Bit 0 = Ringpuffer-Überlauf, Bit 1 = PIO-Stall.

### Test-Firmware für den PLAY-Pegel

`firmware/play_test.c` (Target `kc87_play_test`, eigene `.uf2`-Datei) gibt an GPIO 3 ein Rechtecksignal mit 50 % Tastverhältnis aus. Damit lässt sich der Pegel der PLAY-Schaltung einstellen, noch ohne Playback-Software: PLAY_SIGNAL mit dem Oszilloskop messen und mit RV2 auf ca. 250 mV PP abgleichen (Einstellbereich ca. 200 bis 295 mV PP) (KC87-Eingang: Re = 10 kΩ, mind. 24 mV PP). Die Frequenzen sind die Sollwerte des KC87 (laut [Kassetteninterface](https://hc-ddr.hucki.net/wiki/doku.php/z9001/software/os/kassetteninterface)); am echten KC87 habe ich etwas niedrigere Werte gemessen (siehe [Bandformat](#bandformat)). Nach dem Start läuft der Wechsel zwischen 1,2 und 2,4 kHz (je 1 s).

Steuerung über die USB-Schnittstelle (z. B. mit `picocom -b 115200 /dev/ttyACM0` oder einem anderen Terminalprogramm), ein Zeichen genügt:

| Zeichen | Ausgabe |
|---------|---------|
| `1` | 1200 Hz (Bit 1) |
| `2` | 2400 Hz (Bit 0) |
| `3` | 600 Hz (Trennzeichen) |
| `a` | 1200 Hz und 2400 Hz im Wechsel, je 1 s |
| `0` | Ausgang aus (Low) |
| `h` | Hilfe |

## PC-Tool: kc87tool

Quelle: `pc_commandline_tool/kc87tool.cpp` (Windows, Linux, macOS).

```bash
kc87tool record <port> [--sec 30] [--out aufnahme]   # z.B. COM5 oder /dev/ttyACM0
kc87tool analyze <datei.csv>                          # Statistik
kc87tool decode <datei.csv> [--tap datei.tap]         # Blöcke + Programmname, optional TAP-Datei
kc87tool wav <datei.csv> [--rate 44100]               # Rechtecksignal als WAV
```

- **record**: sendet `S`, zeichnet `--sec` Sekunden auf (Standard 30), sendet `X` und speichert die Pulse als `<out>.csv`. Danach werden automatisch `analyze` und `decode` ausgeführt; dabei entsteht auch `<out>.tap`. Meldet Ringpuffer-Überlauf, PIO-Stall oder fehlenden Endmarker als Warnung.
- **analyze**: gibt Min/Median/Max für High, Low und Periodendauer aus, sowie die häufigsten Periodenlängen.
- **decode**: zerlegt das Bandsignal in Bytes und Blöcke (siehe [Bandformat](#bandformat)). Zeigt je Block Nummer und Prüfsummen-Status, bei BASIC-Programmen auch den Programmnamen. Mit `--tap <datei>` werden alle Blöcke mit gültiger Prüfsumme als TAP-Datei geschrieben, damit sie in einem Emulator geladen werden können (mit JKCEMU 0.9.9.1 getestet: laden und starten funktioniert). Blöcke mit falscher Prüfsumme werden übersprungen.
- **wav**: erzeugt `<name>.wav` (8 Bit, Mono) aus der CSV-Datei.

CSV-Format: Kopfzeile `level;dauer_us`, danach je Zeile `1;<µs>` (High) oder `0;<µs>` (Low).

Beispielausgabe von `decode`:

```
Dekodiert: 260 Bytes, 2 Bloecke
  Block   0: Pruefsumme OK
  Block   1: Pruefsumme OK  BASIC-Programm "TEST123"
TAP geschrieben: aufnahme.tap (2 Bloecke)
```

### Bandformat

Aus einer echten `CSAVE`-Aufnahme abgelesen:

| Symbol | Periode | Frequenz |
|--------|---------|----------|
| Bit 0 | 2 Pulse à ca. 250 µs | ca. 2 kHz |
| Bit 1 | 2 Pulse à ca. 455 µs | ca. 1,1 kHz |
| Trennzeichen | 2 Pulse à ca. 870 µs | ca. 575 Hz |

Sollwerte laut [Kassetteninterface](https://hc-ddr.hucki.net/wiki/doku.php/z9001/software/os/kassetteninterface): Trennzeichen 600 Hz, 1-Bit 1200 Hz, 0-Bit 2400 Hz. Die gemessenen Werte liegen 5 bis 16 % darunter. Die Wiki-Tabelle ordnet 0- und 1-Bit umgekehrt zu; in den Aufnahmen ist die längere Periode das 1-Bit (nur so stimmen Prüfsummen und `D3 D3 D3`).

- Ein Byte besteht aus 8 Bits, LSB zuerst. Vor jedem Byte steht ein Trennzeichen.
- Vor jedem Block steht ein Vorton aus 1-Bits (der erste ist sehr lang, ca. 6000 Perioden).
- Ein Block hat 130 Bytes: Blocknummer, 128 Datenbytes, Prüfsumme (Summe der Datenbytes modulo 256).
- BASIC-Programme (`CSAVE`) enthalten `D3 D3 D3`, danach den Programmnamen (8 Zeichen) und den Programmtext.
- Das TAP-Format: 16 Byte Kennung `\xC3KC-TAPE by AF. `, danach je Block Blocknummer + 128 Datenbytes.

Die Schwellen für die Unterscheidung liegen bei 350 µs und 650 µs pro Puls (Konstanten `SHORT_MAX_US` und `MID_MAX_US` in `kc87tool.cpp`).

## Build

### Firmware

Über die VS Code Pico-Extension oder manuell (Pico SDK 2.3.0, `PICO_SDK_PATH` setzen):

```bash
cd firmware
mkdir -p build && cd build
cmake ..
ninja          # oder: make
```

Ergebnis: `kc87_logger.uf2` / `.elf` (Aufnahme) und `kc87_play_test.uf2` / `.elf` (PLAY-Test).

Die Ziel-Platine steht in `firmware/CMakeLists.txt` (`set(PICO_BOARD pico ...)`):

- Pico 1 (RP2040): `pico` (Standard)
- Pico 2 (RP2350): `pico2`, oder beim Konfigurieren mit `cmake -DPICO_BOARD=pico2 ..` (bei einem schon vorhandenen `build/`-Ordner diesen vorher löschen)

### PC-Tool

```bash
cd pc_commandline_tool
cmake -S . -B build
cmake --build build
```

Alternativ ohne CMake: `g++ -std=c++17 -O2 -o kc87tool kc87tool.cpp` (unter Windows mit MinGW/MSYS2 bzw. `cl /std:c++17 /O2 /EHsc kc87tool.cpp` mit Visual Studio).

## Workflow

1. **Pico flashen**: Firmware kompilieren → über Debug-Probe oder UF2 flashen
2. **Aufnahme starten**: `kc87tool record /dev/ttyACM0 --sec 30 --out aufnahme`
3. **Programm-Save am KC87 starten** (Signal an GPIO 2)
4. **Auswerten**: Analyse und Dekodierung laufen nach der Aufnahme automatisch; später erneut mit `kc87tool analyze aufnahme.csv` und `kc87tool decode aufnahme.csv --tap aufnahme.tap`
5. **Im Emulator laden**: die erzeugte `.tap`-Datei (z. B. in JKCEMU)

## Bekannte Einschränkungen

- Playback-Funktionalität ist noch nicht implementiert
- Die Aufnahmedauer wird vom PC festgelegt (`--sec`), es gibt keinen automatischen Stopp nach Inaktivität
- Dekodiert und geprüft ist bisher nur eine `CSAVE`-Aufnahme (BASIC). Maschinencode-Dateien sind nicht getestet, die Header-Felder werden nicht ausgewertet.
- Als Exportformat gibt es nur TAP (mit JKCEMU 0.9.9.1 getestet, mit einem BASIC-Programm). Die Formate KCC und SSS gibt es nicht.
- Der Ringpuffer fasst 8192 Messwerte; bei sehr hoher Pulsrate und blockierter USB-Verbindung kann er überlaufen (Status-Bit 0)
