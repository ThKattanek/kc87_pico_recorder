# KC87 Pico Recorder

KC87 Kassetten-Recorder Emulator mit Raspberry Pi Pico (RP2350).

## Status

**✅ Recording (Aufnahme): Voll funktional**  
❌ Playback (Wiedergabe): Entfernt (Timing-Probleme ungelöst)

## Hardware

- Raspberry Pi Pico (RP2350) mit Debug-Probe
- GPIO_2: Recording-Eingang (KC87 → Pico)
- GPIO_3: Nicht verwendet (war für Playback gedacht)

## Firmware

Die Firmware unterstützt nur noch Recording:
- `firmware/kc87_pico_recorder_clean.c` - Saubere Recording-only Version
- Erfassst GPIO-Flanken über Hardware-Interrupts
- Überträgt SLIP-kodierte Samples über USB
- Perfekte Timing-Auflösung: ±1μs

## Tools

### serial_capture
Empfängt SLIP-Daten vom Pico und speichert als .bin Datei.

```bash
cd tools
make serial_capture
./serial_capture -p /dev/ttyACM1 -o aufnahme.bin
# Strg+C zum Stoppen
```

### analyze_bin.py 
Analysiert aufgenommene .bin Dateien im Detail.

```bash
./analyze_bin.py aufnahme.bin
```

Ausgabe:
- Frequenz-Analyse (Hz, Jitter)
- Pattern-Validierung 
- Signal-Qualitäts-Bewertung
- Detaillierte Perioden-Statistiken

## Bewährte Workflow

1. **Pico flashen**: Compile Project → Flash
2. **Recording starten**: `./serial_capture -p /dev/ttyACM1 -o test.bin`
3. **KC87 Tape abspielen**: GPIO_2 mit KC87 Kassetten-Ausgang verbinden
4. **Recording stoppen**: Strg+C nach Aufnahme
5. **Analyse**: `./analyze_bin.py test.bin`

## Validierte Ergebnisse

Das Recording-System wurde mit Signalgeneratoren getestet:
- **1kHz**: 1000.0 Hz ±0.2 Hz (0.02% Jitter) ✅
- **2kHz**: 2000.0 Hz ±8.2 Hz (0.41% Jitter) ✅  
- **3kHz**: 3000.0 Hz ±14.5 Hz (0.48% Jitter) ✅
- **4kHz**: 3999.8 Hz ±22.6 Hz (0.56% Jitter) ✅

Perfekte Aufnahme-Qualität für KC87-Kassetten!

## Bekannte Probleme

- Playback-Funktionalität wurde entfernt (USB-Timing-Konflikte)
- GPIO_3 wird nicht mehr verwendet
- Nur Recording wird unterstützt