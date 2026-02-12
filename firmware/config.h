#define GPIO_RECORD_PIN 2   // GPIO Pin für Aufnahme-Taste (Im Schaltplan das Signal: "KC87_REC_PICO")
#define GPIO_PLAY_PIN 3     // GPIO Pin für Wiedergabe-Taste (Im Schaltplan das Signal: "KC87_PLAY_PICO")

// Firmware Version (defined via CMake)
#ifndef FW_VERSION_MAJOR
#define FW_VERSION_MAJOR 0
#endif
#ifndef FW_VERSION_MINOR
#define FW_VERSION_MINOR 1
#endif
#ifndef FW_VERSION_PATCH
#define FW_VERSION_PATCH 0
#endif

// Version string helper macros
#define STRINGIFY(x) #x
#define TOSTRING(x) STRINGIFY(x)
#define FW_VERSION_STRING TOSTRING(FW_VERSION_MAJOR) "." TOSTRING(FW_VERSION_MINOR) "." TOSTRING(FW_VERSION_PATCH)