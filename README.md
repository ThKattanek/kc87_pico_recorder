# KC87 Pico Recorder

KC87 Kassetten-Recorder Emulator mit Raspberry Pi Pico 2 (RP2350).

## Status

**✅ Recording (Aufnahme): Voll funktional**  
❌ Playback (Wiedergabe): Noch nicht implementiert

## Projektstruktur

```
kc87_pico_recorder/
├── firmware/          Pico-Firmware (C, Pico SDK)
├── tools/             Host-Tools (C, Python)
├── hardware/          KiCad-Schaltplan & PCB
├── doc/               Dokumentation
├── PROTOCOL.md        Protokoll-Dokumentation
└── README.md
```

## Hardware

- Raspberry Pi Pico 2 (RP2350) mit Debug-Probe
- **GPIO 3**: Recording-Eingang (`KC87_REC_PICO` — KC87 → Pico)
- **GPIO 2**: Playback-Ausgang (`KC87_PLAY_PICO` — Pico → KC87, noch nicht implementiert)

## Firmware

- Quelldatei: `firmware/kc87_pico_recorder.c`
- Konfiguration: `firmware/config.h`
- Erfasst GPIO-Flanken über Hardware-Interrupts mit Ringpuffer
- Überträgt Samples in einem blockbasierten Binärprotokoll über USB (siehe [PROTOCOL.md](PROTOCOL.md))
- Timing-Auflösung: 1 μs (15-Bit Delta, max. 32767 μs)
- Automatisches Recording-Ende nach 5 s Inaktivität (End-of-Stream-Marker)

### Datenformat (16-Bit Sample)

| Bit 15 | Bit 14–0 |
|--------|----------|
| Flanke (1 = steigend, 0 = fallend) | Delta-Zeit in Mikrosekunden |

### Block-Protokoll

Die Übertragung erfolgt in Blöcken mit Header- und Sample-Blöcken:

```
[START-BLOCK 0x0000] [BLOCK_TYPE] [PAYLOAD...] [END-BLOCK 0x8000]
```

Details siehe [PROTOCOL.md](PROTOCOL.md).

## Tools

### serial_capture

Empfängt Block-Daten vom Pico und speichert als `.bin` Datei **im kompletten Block-Format** (Header-Block → Sample-Blöcke → End-of-Stream). Optional wird zusätzlich eine WAV-Audiodatei (44.1 kHz, 16-Bit PCM Mono) erzeugt.

```bash
# Nur Binärdaten
./serial_capture -p /dev/ttyACM0 -o aufnahme.bin

# Binärdaten + WAV-Audio
./serial_capture -p /dev/ttyACM0 -o aufnahme.bin -w audio.wav

# Mit spezifischer Baudrate
./serial_capture -p /dev/ttyACM0 -o aufnahme.bin -b 230400
```

Das Recording endet automatisch, sobald die Firmware den End-of-Stream-Marker sendet (5 s Inaktivität).

### serial_transmit

Sendet eine `.bin`-Datei als SLIP-kodierte Samples über eine serielle Verbindung (für zukünftige Playback-Funktion).

```bash
./serial_transmit -p /dev/ttyACM0 -i aufnahme.bin
```

### analyze_bin.py

Analysiert aufgenommene `.bin`-Dateien im Detail.

```bash
python3 tools/analyze_bin.py aufnahme.bin
```

Ausgabe:
- Delta-Zeit-Statistiken (Min/Max/Durchschnitt)
- Flanken-Pattern-Validierung (alternierend steigend/fallend)
- Frequenz-Analyse (Hz, Jitter)
- Overflow- und Ausreißer-Erkennung

### Weitere Python-Tools

- `analyze_first_samples.py` — Analyse der ersten Samples einer Aufnahme
- `bin_to_c_array.py` — Konvertiert `.bin`-Dateien in ein C-Array

## Build

### Firmware

Über die VS Code Pico-Extension oder manuell:

```bash
cd firmware
mkdir -p build && cd build
cmake ..
ninja          # oder: make
```

### Host-Tools (Linux)

```bash
cd tools
cmake -S . -B build
cmake --build build
```

### Host-Tools (Cross-Compilation für Windows)

```bash
cd tools
cmake -S . -B build-windows -DCMAKE_TOOLCHAIN_FILE=mingw-w64-toolchain.cmake
cmake --build build-windows
```

Weitere Build-Details (inkl. Windows nativ) siehe [tools/README.md](tools/README.md).

## Workflow

1. **Pico flashen**: Firmware kompilieren → über Debug-Probe oder UF2 flashen
2. **Recording starten**: `./serial_capture -p /dev/ttyACM0 -o aufnahme.bin`
3. **KC87 Tape abspielen**: GPIO 3 mit KC87-Kassettenausgang verbinden
4. **Recording endet automatisch** nach 5 s Inaktivität
5. **Analyse**: `python3 tools/analyze_bin.py aufnahme.bin`

## Bekannte Einschränkungen

- Playback-Funktionalität ist noch nicht in der Firmware implementiert
- Maximale Delta-Zeit pro Sample: 32767 μs (~32 ms) — längere Pausen werden auf diesen Wert begrenzt
- Bei sehr hoher Flankenfrequenz kann die serielle USB-Übertragung zum Engpass werden