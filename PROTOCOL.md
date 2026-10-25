# KC87 Pico Recorder Transferprotokoll

## Überblick

Der Pico misst per PIO die Dauer jeder High- und Low-Phase am Eingang GP2 und überträgt die Werte über USB-CDC an den PC. Die Aufnahme wird vom PC gesteuert.

Alle Wörter sind `uint32`, little-endian.

## Befehle (PC → Pico)

| Zeichen | Bedeutung |
|---------|-----------|
| `'S'` | Aufnahme starten |
| `'X'` | Aufnahme stoppen |
| `'P'` | Wiedergabe starten |

## Datenstrom (Pico → PC)

```
┌────────────┬─────────┬──────────────────────────┬────────────┬────────┬────────┐
│ MAGIC      │ tick_hz │ Dauer0, Dauer1, Dauer2…  │ END_MARKER │ status │ count  │
│ 0x3738434B │         │ (je 1 Wort)              │ 0xFFFFFFFF │        │        │
└────────────┴─────────┴──────────────────────────┴────────────┴────────┴────────┘
     Header (8 Bytes)          Daten                    Trailer (12 Bytes)
```

- **MAGIC**: `"KC87"` (Bytes `4B 43 38 37`)
- **tick_hz**: Zählfrequenz in Hz (ca. 5 MHz, also 0,2 µs pro Tick)
- **Daten**: Dauer in Ticks, abwechselnd High, Low, High, Low, … (erster Wert ist immer eine High-Phase, da der PIO-Code auf High synchronisiert)
- **END_MARKER**: `0xFFFFFFFF`
- **status**: Bit 0 = Ringpuffer-Überlauf (Pulse verloren), Bit 1 = PIO-RX-Stall (Zeiten unzuverlässig)
- **count**: Anzahl der gesendeten Datenwörter

Umrechnung in Mikrosekunden: `us = ticks * 1e6 / tick_hz`

## Ablauf

1. PC öffnet den Port (DTR muss gesetzt sein) und sendet `'S'`
2. Pico sendet den Header und danach laufend Messwerte
3. PC sendet nach der gewünschten Aufnahmedauer `'X'`
4. Pico sendet restliche Daten und den Trailer, danach wartet er auf den nächsten `'S'`

Der Pico beendet die Aufnahme nicht selbst; eine Inaktivitäts-Erkennung gibt es nicht.

## Wiedergabe

1. PC sendet `'P'`
2. Pico antwortet mit dem Header (`"KC87"`, `tick_hz`, wie bei der Aufnahme)
3. PC sendet die Pulsdauern in Ticks als `uint32`, abwechselnd High, Low, High, … (erster Wert ist High), danach `0xFFFFFFFF`
4. Der Pico gibt die Pulse an GP3 aus. Er sammelt zuerst 2048 Werte (oder bis zum Ende), danach startet die Ausgabe. Ist sein Ringpuffer (8192 Werte) voll, nimmt er keine Daten an, die USB-Verbindung bremst den PC.
5. Am Ende sendet der Pico `0xFFFFFFFF`, `status`, `count` und bleibt auf Low

- **status**: Bit 0 = Unterlauf (Daten kamen zu spät), Bit 1 = Abbruch (5 s keine Daten oder USB getrennt)
- **count**: Anzahl der ausgegebenen Pulse (ohne die automatisch angehängte Low-Phase)
- Endet die Folge mit einem High-Puls, hängt der Pico 20 ms Low an
- Ein Puls dauert im PIO-Programm Wert + 4 Takte, die Firmware zieht die 4 Takte vorher ab (Mindestdauer 4 Ticks)

## Interna der Firmware

- PIO-Programm `pulse_timer.pio`: 2 Takte pro Zählschritt, Zähler X läuft von `0xFFFFFFFF` abwärts. Die Firmware invertiert daher jeden Wert (`~x`), um die Ticks zu erhalten.
- Die Werte laufen per DMA in einen 32-KB-Ringpuffer (8192 Wörter) und werden in Blöcken zu 256 Wörtern über USB gesendet.

## Ausgabeformat des PC-Tools

`kc87tool record` speichert nicht den Rohstrom, sondern eine CSV-Datei:

```
level;dauer_us
1;123.400
0;118.200
...
```

`level` ist 1 für High- und 0 für Low-Phasen, `dauer_us` die Dauer in Mikrosekunden.
