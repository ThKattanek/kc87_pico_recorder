#!/usr/bin/env python3
"""
Konvertiert eine .bin Datei in ein C-Array Format für Flash-Embedding
"""
import sys

def bin_to_c_array(filename, array_name="test_data"):
    print(f"// Generated from {filename}")
    print(f"static const uint16_t {array_name}[] = {{")
    
    with open(filename, 'rb') as f:
        data = f.read()
        
    # Je 2 Bytes als uint16_t little-endian interpretieren
    count = 0
    for i in range(0, len(data), 2):
        if i + 1 < len(data):
            # Little-endian: LSB first
            word = data[i] | (data[i+1] << 8)
            print(f"    0x{word:04X},", end="")
            count += 1
            
            if count % 8 == 0:
                print()  # Neue Zeile alle 8 Werte
            else:
                print(" ", end="")
                
    print("\n};")
    print(f"static const size_t {array_name}_size = {count};")

if __name__ == "__main__":
    if len(sys.argv) != 2:
        print("Usage: ./bin_to_c_array.py <filename>")
        sys.exit(1)
    
    bin_to_c_array(sys.argv[1], "test_1khz_data")