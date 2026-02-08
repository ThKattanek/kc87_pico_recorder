#!/usr/bin/env python3
"""
Analysiert .bin Dateien vom KC87 Pico Recorder für Signalqualität
"""
import struct
import sys
import os

# Block protocol constants
BLOCK_START = 0x0000
BLOCK_END = 0x8000
BLOCK_TYPE_HEADER = 0x00
BLOCK_TYPE_SAMPLES = 0x01

def parse_bin_file(data):
    """Parst eine .bin Datei im Block-Format und extrahiert Samples"""
    samples = []
    pos = 0
    header_found = False
    
    while pos < len(data):
        # Need at least 6 bytes for minimum block
        if pos + 6 > len(data):
            break
        
        # Check for START-BLOCK
        start_marker = struct.unpack('<H', data[pos:pos+2])[0]
        if start_marker != BLOCK_START:
            # Skip invalid data
            pos += 1
            continue
        
        block_type = data[pos + 2]
        
        if block_type == BLOCK_TYPE_HEADER:
            version = data[pos + 3]
            end_marker = struct.unpack('<H', data[pos+4:pos+6])[0]
            if end_marker == BLOCK_END:
                header_found = True
                pos += 6
                continue
        elif block_type == BLOCK_TYPE_SAMPLES and header_found:
            sample_count = data[pos + 3]
            expected_size = 6 + (sample_count * 2)
            
            if pos + expected_size > len(data):
                break
            
            end_marker = struct.unpack('<H', data[pos+expected_size-2:pos+expected_size])[0]
            if end_marker == BLOCK_END:
                # Extract samples
                for i in range(sample_count):
                    sample_pos = pos + 4 + (i * 2)
                    sample = struct.unpack('<H', data[sample_pos:sample_pos+2])[0]
                    edge = (sample & 0x8000) != 0
                    delta_us = sample & 0x7FFF
                    samples.append((edge, delta_us))
                
                pos += expected_size
                continue
        
        # Check for End-of-Stream (0x8000 0x8000)
        if pos + 4 <= len(data):
            marker1 = struct.unpack('<H', data[pos:pos+2])[0]
            marker2 = struct.unpack('<H', data[pos+2:pos+4])[0]
            if marker1 == BLOCK_END and marker2 == BLOCK_END:
                # End of stream reached
                break
        
        # Skip unknown data
        pos += 1
    
    return samples

def analyze_bin_file(filename):
    """Analysiert eine .bin Datei und gibt detaillierte Statistiken aus"""
    
    if not os.path.exists(filename):
        print(f"Fehler: Datei {filename} nicht gefunden")
        return
    
    with open(filename, 'rb') as f:
        data = f.read()
    
    if len(data) == 0:
        print(f"Fehler: {filename} ist leer")
        return
    
    # Parse block format
    samples = parse_bin_file(data)
    
    print(f"\n{'='*60}")
    print(f"ANALYSE: {filename}")
    print(f"{'='*60}")
    print(f"Dateigröße:     {len(data)} Bytes")
    print(f"Anzahl Samples: {len(samples)}")
    
    if len(samples) == 0:
        print("Keine Samples gefunden!")
        return
    
    # Grundlegende Statistiken
    deltas = [s[1] for s in samples]
    edges = [s[0] for s in samples]
    
    print(f"\nDelta-Zeit Statistiken:")
    print(f"  Min:           {min(deltas)} μs")
    print(f"  Max:           {max(deltas)} μs") 
    print(f"  Durchschnitt:  {sum(deltas)/len(deltas):.1f} μs")
    
    # Overflow-Erkennung (32767 ist Maximum für 15 Bits)
    overflows = [d for d in deltas if d >= 32767]
    if overflows:
        print(f"  OVERFLOWS:     {len(overflows)} Samples mit Maximalwert (Timer-Overflow)")
    
    # Sehr große Werte (verdächtig für Audio-Signale) - aber erste/letzte Samples ignorieren
    large_deltas = []
    for i, d in enumerate(deltas):
        if d > 10000 and i != 0 and i != len(deltas)-1:  # Erste und letzte Samples ausschließen
            large_deltas.append((i, d))
    if large_deltas:
        print(f"  GROSSE WERTE:  {len(large_deltas)} Samples > 10ms (ohne erstes/letztes Sample)")
    
    # Pattern-Analyse (sollte alternieren: True, False, True, False...)
    # Ignoriere erstes und letztes Sample bei Pattern-Prüfung
    print(f"\nFlanken-Pattern Analyse (ohne erstes/letztes Sample):")
    pattern_samples = edges[1:-1] if len(edges) > 2 else edges  # Erstes und letztes ausschließen
    start_expected = edges[1] if len(edges) > 1 else True  # Pattern vom zweiten Sample ableiten
    
    expected_pattern = start_expected
    pattern_errors = 0
    
    for i, edge in enumerate(pattern_samples):
        if edge != expected_pattern:
            pattern_errors += 1
            if pattern_errors <= 5:  # Zeige nur erste 5 Fehler
                actual_index = i + 1  # +1 weil wir das erste übersprungen haben
                print(f"  Pattern-Fehler bei Sample {actual_index}: erwartet {expected_pattern}, gefunden {edge}")
        expected_pattern = not expected_pattern
    
    if pattern_errors == 0:
        print(f"  Pattern perfekt: {len(pattern_samples)} Samples alternieren korrekt")
    else:
        print(f"  Pattern-Fehler: {pattern_errors}/{len(pattern_samples)} Samples nicht alternierend")
    
    # Frequenz-Analyse (nur für korrekte Pattern)
    # Ignoriere erste und letzte Periode bei Frequenzberechnung da unvollständig
    if pattern_errors < len(pattern_samples) * 0.1:  # Weniger als 10% Fehler
        frequencies = []
        periods_us = []
        
        # Berechne Frequenz für jedes HIGH-LOW Paar, aber überspringe erste und letzte
        start_idx = 1  # Überspringe erste Periode
        end_idx = len(samples) - 2  # Überspringe letzte Periode
        
        for i in range(start_idx + 1, end_idx, 2):
            if i < len(samples):
                high_time = samples[i-1][1]  # Steigende Flanke
                low_time = samples[i][1]     # Fallende Flanke
                period_us = high_time + low_time
                if period_us > 0:
                    freq_hz = 1000000 / period_us
                    frequencies.append(freq_hz)
                    periods_us.append(period_us)
        
        if frequencies:
            avg_freq = sum(frequencies) / len(frequencies)
            min_freq = min(frequencies)
            max_freq = max(frequencies)
            
            # Jitter-Berechnung
            freq_variance = sum((f - avg_freq)**2 for f in frequencies) / len(frequencies)
            freq_std = freq_variance ** 0.5
            jitter_percent = (freq_std / avg_freq) * 100 if avg_freq > 0 else 0
            
            print(f"\nFrequenz-Analyse ({len(frequencies)} Perioden, ohne erste/letzte):")
            print(f"  Durchschnitt:  {avg_freq:.1f} Hz")
            print(f"  Bereich:       {min_freq:.1f} - {max_freq:.1f} Hz")
            print(f"  Jitter:        ±{freq_std:.1f} Hz ({jitter_percent:.2f}%)")
            
            # Bestimme erwartete Frequenz (1, 2, 3, 4 kHz)
            expected_freqs = [1000, 2000, 3000, 4000]
            closest_expected = min(expected_freqs, key=lambda x: abs(x - avg_freq))
            freq_error = abs(avg_freq - closest_expected)
            freq_error_percent = (freq_error / closest_expected) * 100
            
            print(f"  Erwartet:      {closest_expected} Hz")
            print(f"  Abweichung:    {freq_error:.1f} Hz ({freq_error_percent:.2f}%)")
            
            # Qualitätsbewertung
            print(f"\nQualitätsbewertung:")
            if jitter_percent < 1.0:
                print(f"  Jitter:        SEHR GUT (< 1%)")
            elif jitter_percent < 3.0:
                print(f"  Jitter:        GUT (< 3%)")
            elif jitter_percent < 10.0:
                print(f"  Jitter:        MÄSSIG (< 10%)")
            else:
                print(f"  Jitter:        SCHLECHT (> 10%)")
            
            if freq_error_percent < 1.0:
                print(f"  Frequenz:      SEHR GUT (< 1% Abweichung)")
            elif freq_error_percent < 5.0:
                print(f"  Frequenz:      GUT (< 5% Abweichung)")
            else:
                print(f"  Frequenz:      SCHLECHT (> 5% Abweichung)")
                
            # Timing-Details für Perioden 2-11 (überspringe erste und evtl. letzte)
            print(f"\nPerioden 2-11 Details (ohne erste/letzte):")
            print(f"{'Periode':>7} | {'HIGH (μs)':>9} | {'LOW (μs)':>8} | {'Gesamt':>8} | {'Freq (Hz)':>9}")
            print(f"{'-'*7}-+-{'-'*9}-+-{'-'*8}-+-{'-'*8}-+-{'-'*9}")
            
            # Zeige Perioden 2-11 (skip erste)
            period_start = 1  # Beginne mit zweiter Periode
            for i in range(10):  # Zeige 10 Perioden
                freq_idx = i
                sample_idx = period_start + (i * 2)
                
                if freq_idx < len(frequencies) and sample_idx + 1 < len(samples):
                    high_time = samples[sample_idx][1]
                    low_time = samples[sample_idx + 1][1] if sample_idx + 1 < len(samples) else 0
                    total_time = high_time + low_time
                    freq = frequencies[freq_idx]
                    print(f"{period_start + i + 1:7d} | {high_time:9d} | {low_time:8d} | {total_time:8d} | {freq:9.1f}")
    
    else:
        print(f"\nFrequenz-Analyse übersprungen (zu viele Pattern-Fehler)")
    
    print(f"\n{'='*60}")

def main():
    """Hauptfunktion"""
    if len(sys.argv) < 2:
        print("Usage: python3 analyze_bin.py <file1.bin> [file2.bin] ...")
        print("\nBeispiel:")
        print("  python3 analyze_bin.py 1khz.bin 2khz.bin 3khz.bin 4khz.bin")
        sys.exit(1)
    
    for filename in sys.argv[1:]:
        analyze_bin_file(filename)
        
    print(f"\nAnalyse abgeschlossen für {len(sys.argv)-1} Dateien.")

if __name__ == "__main__":
    main()