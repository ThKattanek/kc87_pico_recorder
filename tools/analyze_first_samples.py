#!/usr/bin/env python3
"""
Analysiere die ersten Samples einer .bin Datei im Detail
"""
import struct
import sys

def analyze_first_samples(filename, count=20):
    print(f"Erste {count} Samples aus {filename}:")
    print("=" * 60)
    
    with open(filename, 'rb') as f:
        for i in range(count):
            data = f.read(2)
            if len(data) < 2:
                print(f"Sample {i+1}: EOF erreicht")
                break
                
            # Little-endian decode
            word = struct.unpack('<H', data)[0]
            
            # Extract edge and delta
            edge = (word & 0x8000) != 0
            delta_us = word & 0x7FFF
            
            edge_str = "STEIGEND" if edge else "FALLEND"
            print(f"Sample {i+1:2d}: Delta={delta_us:4d}μs  Edge={edge_str:8s}  (0x{word:04X})")
            
if __name__ == "__main__":
    if len(sys.argv) != 2:
        print("Usage: ./analyze_first_samples.py <filename>")
        sys.exit(1)
    
    analyze_first_samples(sys.argv[1])