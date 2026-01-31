# KC87 Pico Recorder Transferprotokoll Dokumentation

## Überblick

Das KC87 Pico Recorder System verwendet ein serielles Übertragungsprotokoll basierend auf SLIP (Serial Line Internet Protocol) zur Übertragung von GPIO-Ereignissen (Flanken) vom Raspberry Pi Pico an einen Host-Computer.

## Architektur

### Firmware (Pico)
- **Datei**: `firmware/kc87_pico_recorder.c`
- **Funktion**: Erfassung von GPIO-Flanken und Übertragung via SLIP-Protokoll
- **GPIO Pin**: GPIO 2 (konfigurierbar in `config.h`)

### Host-Software (PC)
- **Datei**: `tools/serial_capture.c`
- **Funktion**: Empfang und Dekodierung der SLIP-Nachrichten, Speicherung in Binärdatei

## SLIP-Protokoll Grundlagen

SLIP (Serial Line Internet Protocol) ist ein einfaches Protokoll zur Übertragung von Paketen über serielle Verbindungen.

### SLIP Konstanten
```c
#define SLIP_END     0xC0    // Frame-Ende Markierung
#define SLIP_ESC     0xDB    // Escape-Zeichen
#define SLIP_ESC_END 0xDC    // Escaped SLIP_END
#define SLIP_ESC_ESC 0xDD    // Escaped SLIP_ESC
```

### SLIP Escape-Sequenzen
- Wenn `0xC0` (SLIP_END) in den Daten vorkommt → wird zu `0xDB 0xDC`
- Wenn `0xDB` (SLIP_ESC) in den Daten vorkommt → wird zu `0xDB 0xDD`

## Datenformat

### Sample-Datenstruktur

Jedes GPIO-Ereignis wird als 16-Bit Wort übertragen:

```
Bit 15:    Edge-Typ (0 = fallend, 1 = steigend)
Bit 14-0:  Delta-Zeit in Mikrosekunden (0-32767 µs)
```

### Byte-Anordnung
- **Byte 0**: Niederwertiges Byte (Bits 0-7)
- **Byte 1**: Höherwertiges Byte (Bits 8-15)

## Protokoll-Flow

### Sender (Pico Firmware)

1. **Initialisierung**:
   ```c
   gpio_init(GPIO_RECORD_PIN);
   gpio_set_dir(GPIO_RECORD_PIN, GPIO_IN);
   gpio_set_irq_enabled_with_callback(GPIO_RECORD_PIN, 
                                      GPIO_IRQ_EDGE_RISE | GPIO_IRQ_EDGE_FALL, 
                                      true, gpio_callback);
   ```

2. **Interrupt-Handler**:
   - Erfasst Timestamp mit `time_us_32()`
   - Bestimmt Flankentyp (steigend/fallend)

3. **Sample-Übertragung**:
   ```c
   // Frame-Format: SLIP_END <byte0> <byte1> SLIP_END
   putchar_raw(SLIP_END);
   slip_write_byte(b0);    // Escaped wenn nötig
   slip_write_byte(b1);    // Escaped wenn nötig
   putchar_raw(SLIP_END);
   ```

4. **Delta-Zeit Berechnung**:
   ```c
   uint32_t delta = timestamp - last_timestamp;
   if (delta > 0x7FFFu) {
       delta = 0x7FFFu;  // Maximale Delta-Zeit: 32767 µs
   }
   ```

### Empfänger (Host-Software)

1. **SLIP-Frame Dekodierung**:
   - Erkennung von Frame-Grenzen durch `SLIP_END` (0xC0)
   - Verarbeitung von Escape-Sequenzen
   - Extraktion der 2-Byte Nutzdaten

2. **Sample-Rekonstruktion**:
   ```c
   // Aus empfangenen Bytes
   uint16_t word = (b1 << 8) | b0;
   bool edge = (word & 0x8000) != 0;         // Bit 15
   uint16_t delta_us = word & 0x7FFF;        // Bits 0-14
   ```

## Serielle Konfiguration

### Standard-Einstellungen
- **Baudrate**: 115200 bps (konfigurierbar)
- **Datenbits**: 8
- **Parität**: Keine
- **Stoppbits**: 1
- **Flow Control**: Keine (DTR/RTS deaktiviert)

### Timeout-Verhalten
- **Read-Timeout**: 100ms (Host-Software)
- **Inaktivitäts-Timeout**: 5 Sekunden (Host beendet Aufnahme automatisch)

## Frame-Struktur

### Gültiger Frame
```
[SLIP_END] [DATA_BYTE_0] [DATA_BYTE_1] [SLIP_END]
   0xC0        b0             b1          0xC0
```

### Frame mit Escape-Sequenzen (Beispiel)
Wenn `b0 = 0xC0` (SLIP_END):
```
[SLIP_END] [SLIP_ESC] [SLIP_ESC_END] [DATA_BYTE_1] [SLIP_END]
   0xC0       0xDB        0xDC            b1          0xC0
```

## Fehlerbehandlung

### Firmware-Seite
- **Overflow-Schutz**: Delta-Zeit wird auf 32767 µs begrenzt
- **Interrupt-Synchronisation**: Kritische Abschnitte werden mit `save_and_disable_interrupts()` geschützt

### Host-Seite
- **Oversized Frames**: Werden verworfen bis zum nächsten `SLIP_END`
- **Unvollständige Frames**: Werden beim nächsten `SLIP_END` zurückgesetzt
- **Timeout-Detection**: Automatisches Beenden bei fehlenden Daten
- **Serial-Fehler**: Fehlerbehandlung für Read-Operationen

## Verwendung

### Kompilierung
```bash
# Firmware
cd firmware
mkdir -p build && cd build
cmake ..
make

# Host-Tool
cd tools
mkdir -p build && cd build
cmake ..
make
```

### Ausführung
```bash
# Host-Tool starten (nur Binärdaten)
./serial_capture -p /dev/ttyACM0 -o capture.bin -b 115200

# Host-Tool starten (Binärdaten + WAV-Audiodatei)
./serial_capture -p /dev/ttyACM0 -o capture.bin -b 115200 -w audio.wav
```

### Parameter
- `-p <port>`: Serieller Port (z.B. `/dev/ttyACM0`, `COM3`)
- `-o <file>`: Ausgabedatei für Binärdaten
- `-b <baud>`: Baudrate (Standard: 115200)
- `-w <wav_file>`: **NEU** - Optionale WAV-Audiodatei

## WAV-Datei-Generierung

### Funktionsweise
Das erweiterte serial_capture Tool kann nun optional eine WAV-Audiodatei generieren, die das digitale Signal rekonstruiert:

- **Samplerate**: 44100 Hz (CD-Qualität)
- **Format**: 16-Bit PCM Mono
- **Signalpegel**: ±16383 (etwa 50% vom Maximum für saubere Darstellung)

### Audio-Rekonstruktion
1. **Zustandsverfolgung**: Das Tool verfolgt den aktuellen GPIO-Zustand (HIGH/LOW)
2. **Zeitbasierte Interpolation**: Basierend auf den Delta-Zeitstempeln werden Audio-Samples generiert
3. **Edge-basierte Zustandsänderung**: Bei jeder Flanke (steigend/fallend) wird der Signalzustand gewechselt

### WAV-Datei Aufbau
```c
// WAV Header mit Standard-Parametern
Sample Rate: 44100 Hz
Channels: 1 (Mono)
Bits per Sample: 16
Audio Format: PCM
```

### Verwendungsbeispiele
```bash
# Nur Binärdaten erfassen
./serial_capture -p /dev/ttyACM0 -o data.bin

# Binärdaten + WAV-Audio gleichzeitig
./serial_capture -p /dev/ttyACM0 -o data.bin -w signal.wav

# Mit spezifischer Baudrate
./serial_capture -p /dev/ttyACM0 -o data.bin -w signal.wav -b 230400
```

## Datenformat der Ausgabedatei

Die Ausgabedatei enthält eine Sequenz von 2-Byte Samples im Little-Endian Format:

```
[Sample1_Lo] [Sample1_Hi] [Sample2_Lo] [Sample2_Hi] ...
```

Jedes Sample kann wie folgt dekodiert werden:
```c
uint16_t sample = (high_byte << 8) | low_byte;
bool rising_edge = (sample & 0x8000) != 0;
uint16_t delta_microseconds = sample & 0x7FFF;
```

## Performance-Charakteristika

- **Maximale Auflösung**: 1 Mikrosekunde
- **Maximaler Delta-Wert**: 32767 Mikrosekunden (32.767 ms)
- **Typische Übertragungsrate**: ~1000 Samples/Sekunde bei kontinuierlicher Aktivität
- **Minimale GPIO-Latenz**: Hardware-Interrupt basiert, sehr niedrig

## Limitierungen

1. **Delta-Zeit Overflow**: Bei längeren Pausen > 32767 µs wird der Wert auf Maximum begrenzt
2. **Baudrate-Limit**: Bei sehr hoher Flankenfrequenz kann die serielle Übertragung zum Bottleneck werden
3. **Keine Synchronisation**: Keine explizite Synchronisation zwischen Sender und Empfänger
4. **Keine Checksummen**: Keine Integritätsprüfung der übertragenen Daten
5. **WAV-Dateigröße**: Bei längeren Aufnahmen können sehr große WAV-Dateien entstehen (44.1 KB pro Sekunde)
6. **Audio-Auflösung**: Die WAV-Rekonstruktion ist nur so genau wie die ursprünglichen Mikrosekunden-Timestamps

## Troubleshooting

### Häufige Probleme

1. **Keine Daten empfangen**:
   - Prüfung der seriellen Verbindung
   - Korrekte Baudrate verwenden
   - GPIO-Pin korrekt angeschlossen

2. **Unterbrochene Übertragung**:
   - Timeout-Werte anpassen
   - Serielle Puffergrößen prüfen
   - Hardware-Verbindung stabilisieren

3. **Falsche Timestamps**:
   - System-Timer Kalibrierung prüfen
   - Interrupt-Latenz berücksichtigen

### Debug-Ausgaben

Das Host-Tool zeigt regelmäßig Statistiken an:
```
1000 samples, 150.3 samples/s
2000 samples, 145.7 samples/s
...
```

Diese Ausgaben helfen bei der Überprüfung der Übertragungsleistung und der Erkennung von Problemen.