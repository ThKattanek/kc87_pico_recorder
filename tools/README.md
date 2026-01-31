# Serial Capture Tool

Build the host CLI tool (Windows/Linux) from this folder.

## Build (CMake)

### Linux

```bash
cmake -S . -B build
cmake --build build
```

### Cross-Compilation for Windows (on Ubuntu/Kubuntu)

To build Windows executables from Linux, you need the MinGW-w64 cross-compiler:

#### 1. Install MinGW-w64 cross-compiler
```bash
sudo apt update
sudo apt install mingw-w64 cmake
```

#### 2. Create a CMake toolchain file
Create a file called `mingw-w64-toolchain.cmake` in the tools directory:

```cmake
# mingw-w64-toolchain.cmake
set(CMAKE_SYSTEM_NAME Windows)
set(CMAKE_SYSTEM_PROCESSOR x86_64)

# Specify the cross compiler
set(CMAKE_C_COMPILER x86_64-w64-mingw32-gcc)
set(CMAKE_CXX_COMPILER x86_64-w64-mingw32-g++)
set(CMAKE_RC_COMPILER x86_64-w64-mingw32-windres)

# Where to find libraries and headers
set(CMAKE_FIND_ROOT_PATH /usr/x86_64-w64-mingw32)

# Search for programs in the build host directories
set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)
# Search for libraries and headers in the target environment
set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE ONLY)
```

#### 3. Cross-compile for Windows
```bash
# Create build directory for Windows
cmake -S . -B build-windows -DCMAKE_TOOLCHAIN_FILE=mingw-w64-toolchain.cmake
cmake --build build-windows

# For 32-bit Windows (optional)
# Use i686-w64-mingw32-gcc instead and adjust toolchain file accordingly
```

#### 4. Result
The Windows executable will be created as:
- `build-windows/serial_capture.exe`

#### Alternative: One-liner without toolchain file
```bash
cmake -S . -B build-windows \
  -DCMAKE_SYSTEM_NAME=Windows \
  -DCMAKE_C_COMPILER=x86_64-w64-mingw32-gcc \
  -DCMAKE_CXX_COMPILER=x86_64-w64-mingw32-g++
cmake --build build-windows
```

### Windows (from scratch)

For a fresh Windows installation, you need to install the following tools:

#### 1. Install Visual Studio Community (free)
- Download from: https://visualstudio.microsoft.com/downloads/
- During installation, select "Desktop development with C++"
- This includes the MSVC compiler and Windows SDK

#### 2. Install CMake
- Download from: https://cmake.org/download/
- Choose "Windows x64 Installer"
- During installation, select "Add CMake to the system PATH for all users"

#### 3. Install Git (optional, if you want to clone the repository)
- Download from: https://git-scm.com/download/win
- Use default settings during installation

#### 4. Build the project
Open **Command Prompt** or **PowerShell** and navigate to the tools directory:

```cmd
cd path\to\kc87_pico_recorder\tools
cmake -S . -B build
cmake --build build --config Release
```

Alternatively, you can use **Visual Studio Developer Command Prompt** (recommended):
- Open "Developer Command Prompt for VS 2022" from Start Menu
- Navigate to the tools directory
- Run the same cmake commands

#### 5. Alternative: Using Visual Studio IDE
You can also open the project directly in Visual Studio:
- Open Visual Studio
- File → Open → CMake...
- Select the `CMakeLists.txt` in the tools directory
- Visual Studio will automatically configure the project
- Build → Build All

The executable will be:
- Linux: `build/serial_capture`
- Windows: `build/Debug/serial_capture.exe` (or `build/Release/...`)

## Usage

```bash
serial_capture -p <port> -o <out_file> [-b baud] [-w wav_file]
```

Parameters:
- `-p <port>`: Serial port (e.g., /dev/ttyACM0, COM3)
- `-o <out_file>`: Binary output file
- `-b <baud>`: Baud rate (default: 115200)
- `-w <wav_file>`: Optional WAV output file

Examples:

```bash
# Basic usage - capture only binary data
serial_capture -p /dev/ttyACM0 -o capture.bin -b 115200

# Capture both binary data and WAV audio
serial_capture -p /dev/ttyACM0 -o capture.bin -b 115200 -w audio.wav
```

```powershell
# Windows examples
serial_capture -p COM6 -o capture.bin -b 115200
serial_capture -p COM6 -o capture.bin -b 115200 -w audio.wav
```

## Output Format

The output file contains raw 2-byte payloads per event (little-endian). Each payload word is:

- Bit 15: `edge` (1=rising, 0=falling)
- Bits 14..0: `delta_us` (0..32767)
