# Serial Capture Tool

Build the host CLI tool (Windows/Linux) from this folder.

## Build (CMake)

```bash
cmake -S . -B build
cmake --build build
```

The executable will be:
- Linux: `build/serial_capture`
- Windows: `build/Debug/serial_capture.exe` (or `build/Release/...`)

## Usage

```bash
serial_capture -p <port> -o <file> [-b baud]
```

Examples:

```bash
serial_capture -p /dev/ttyACM0 -o capture.bin -b 115200
```

```powershell
serial_capture -p COM6 -o capture.bin -b 115200
```

## Output Format

The output file contains raw 2-byte payloads per event (little-endian). Each payload word is:

- Bit 15: `edge` (1=rising, 0=falling)
- Bits 14..0: `delta_us` (0..32767)
