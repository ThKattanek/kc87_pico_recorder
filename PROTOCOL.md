# KC87 Pico Recorder Transferprotokoll

## Überblick

Das KC87 Pico Recorder System verwendet ein blockbasiertes Binärprotokoll zur Übertragung von GPIO-Ereignissen vom Raspberry Pi Pico an einen Host-Computer über USB-Seriell.

## Protokoll-Konstanten

```c
#define BLOCK_START       0x0000  // Start-Marker eines Blocks
#define BLOCK_END         0x8000  // End-Marker eines Blocks
#define BLOCK_TYPE_HEADER 0x00    // Header-Block (Session-Start)
#define BLOCK_TYPE_SAMPLES 0x01   // Sample-Block (Daten)
#define PROTOCOL_VERSION  0x01    // Protokoll-Version
```

## Sample-Datenformat

Jedes GPIO-Ereignis wird als 16-Bit Wort (Little-Endian) kodiert:

```
Bit 15:    Edge-Typ (1 = steigend, 0 = fallend)
Bit 14-0:  Delta-Zeit in Mikrosekunden (0–32767 µs)
```

**Byte-Anordnung:**
- Byte 0: Niederwertiges Byte (Bits 0–7)
- Byte 1: Höherwertiges Byte (Bits 8–15)

**Dekodierung:**
```c
uint16_t sample = (byte1 << 8) | byte0;
bool rising_edge = (sample & 0x8000) != 0;
uint16_t delta_us = sample & 0x7FFF;
```

## Block-Strukturen

### Header-Block

Signalisiert den Beginn einer Recording-Session:

```
┌──────────────┬────────────┬─────────┬──────────────┐
│ START-BLOCK  │ BLOCK_TYPE │ VERSION │  END-BLOCK   │
│   0x0000     │    0x00    │  0x01   │   0x8000     │
│  (2 Bytes)   │  (1 Byte)  │(1 Byte) │  (2 Bytes)   │
└──────────────┴────────────┴─────────┴──────────────┘
```

**Gesamtgröße:** 6 Bytes

### Sample-Block

Enthält 1–255 GPIO-Samples:

```
┌──────────────┬────────────┬───────┬─────────┬─────────┬───┬───────────┬──────────────┐
│ START-BLOCK  │ BLOCK_TYPE │ COUNT │ SAMPLE0 │ SAMPLE1 │...│ SAMPLEN-1 │  END-BLOCK   │
│   0x0000     │    0x01    │   N   │(2 Bytes)│(2 Bytes)│   │ (2 Bytes) │   0x8000     │
│  (2 Bytes)   │  (1 Byte)  │(1 Byte)│         │         │   │           │  (2 Bytes)   │
└──────────────┴────────────┴───────┴─────────┴─────────┴───┴───────────┴──────────────┘
```

**Gesamtgröße:** 6 + (N × 2) Bytes  
**Maximale Größe:** 6 + (255 × 2) = 516 Bytes

### End-of-Stream

Zwei aufeinanderfolgende `END-BLOCK`-Marker signalisieren das Ende der Aufnahme:

```
┌──────────────┬──────────────┐
│  END-BLOCK   │  END-BLOCK   │
│   0x8000     │   0x8000     │
│  (2 Bytes)   │  (2 Bytes)   │
└──────────────┴──────────────┘
```

## Übertragungsablauf

1. **Session-Start:** Header-Block beim ersten GPIO-Event
2. **Datenübertragung:** Sample-Blöcke mit bis zu 255 Samples
3. **Session-Ende:** End-of-Stream nach 5 Sekunden Inaktivität

## Übertragungsbeispiel

```
# Header-Block (Session-Start)
00 00 00 01 00 80

# Sample-Block mit 3 Samples
00 00 01 03   # START, TYPE=Samples, COUNT=3
  0A 80       # Sample 0: Steigend, 10 µs
  05 00       # Sample 1: Fallend, 5 µs
  0C 80       # Sample 2: Steigend, 12 µs
00 80         # END-BLOCK

# Sample-Block mit 2 Samples
00 00 01 02   # START, TYPE=Samples, COUNT=2
  07 00       # Sample 0: Fallend, 7 µs
  08 80       # Sample 1: Steigend, 8 µs
00 80         # END-BLOCK

# End-of-Stream
00 80         # END-BLOCK
00 80         # END-BLOCK (Stream-Ende)
```

## Serielle Konfiguration

**USB-Seriell Einstellungen:**
- Baudrate: 115200 bps (konfigurierbar)
- Datenbits: 8
- Parität: Keine
- Stoppbits: 1
- Flow Control: Keine

**Timeout:**
- Inaktivitäts-Timeout (Firmware): 5 Sekunden → automatischer End-of-Stream

## Ausgabedatei-Format

Die Host-Software speichert **alle Bytes im Block-Format** in der `.bin`-Datei, beginnend mit dem Header-Block bis einschließlich der End-of-Stream-Marker:

```
# Kompletter Inhalt einer .bin Datei:

[Header-Block: 6 Bytes]
[Sample-Block 1: 6 + N₁×2 Bytes]
[Sample-Block 2: 6 + N₂×2 Bytes]
...
[Sample-Block n: 6 + Nₙ×2 Bytes]
[End-of-Stream: 2 Bytes (0x80 0x00)]
```

Die Datei kann direkt für Playback verwendet oder mit den gleichen Parsing-Regeln analysiert werden, die in diesem Dokument beschrieben sind.