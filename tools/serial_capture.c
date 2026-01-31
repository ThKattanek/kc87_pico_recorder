#include <errno.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <math.h>

#ifdef _WIN32
#include <windows.h>
#else
#include <fcntl.h>
#include <termios.h>
#include <unistd.h>
#endif

#define SLIP_END     0xC0
#define SLIP_ESC     0xDB
#define SLIP_ESC_END 0xDC
#define SLIP_ESC_ESC 0xDD

// WAV file constants
#define WAV_SAMPLE_RATE 44100
#define WAV_CHANNELS 1
#define WAV_BITS_PER_SAMPLE 16

typedef struct {
    char riff[4];           // "RIFF"
    uint32_t chunk_size;    // File size - 8
    char wave[4];           // "WAVE"
    char fmt[4];            // "fmt "
    uint32_t fmt_size;      // 16 for PCM
    uint16_t audio_format;  // 1 for PCM
    uint16_t num_channels;  // 1 for mono
    uint32_t sample_rate;   // 44100
    uint32_t byte_rate;     // sample_rate * channels * bits_per_sample / 8
    uint16_t block_align;   // channels * bits_per_sample / 8
    uint16_t bits_per_sample; // 16
    char data[4];           // "data"
    uint32_t data_size;     // Number of bytes in data
} wav_header_t;

static void usage(const char *prog)
{
    fprintf(stderr,
            "Usage: %s -p <port> -o <out_file> [-b baud] [-w wav_file]\n"
            "  -p <port>     Serial port (e.g., /dev/ttyACM0, COM3)\n"
            "  -o <out_file> Binary output file\n"
            "  -b <baud>     Baud rate (default: 115200)\n"
            "  -w <wav_file> Optional WAV output file\n"
            "\n"
            "Example: %s -p /dev/ttyACM0 -o capture.bin -b 115200 -w audio.wav\n",
            prog, prog);
}

static double now_seconds(void)
{
#ifdef _WIN32
    static LARGE_INTEGER freq;
    static bool freq_init = false;
    LARGE_INTEGER counter;
    if (!freq_init) {
        QueryPerformanceFrequency(&freq);
        freq_init = true;
    }
    QueryPerformanceCounter(&counter);
    return (double)counter.QuadPart / (double)freq.QuadPart;
#else
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec + (double)ts.tv_nsec / 1e9;
#endif
}

#ifdef _WIN32
typedef struct {
    HANDLE handle;
} serial_handle_t;

static int open_serial(serial_handle_t *sh, const char *port, int baud)
{
    char path[64];
    if (strncmp(port, "\\\\.\\", 4) == 0) {
        snprintf(path, sizeof(path), "%s", port);
    } else {
        snprintf(path, sizeof(path), "\\\\.\\%s", port);
    }

    sh->handle = CreateFileA(path, GENERIC_READ, 0, NULL, OPEN_EXISTING, 0, NULL);
    if (sh->handle == INVALID_HANDLE_VALUE) {
        return -1;
    }

    DCB dcb;
    memset(&dcb, 0, sizeof(dcb));
    dcb.DCBlength = sizeof(dcb);
    if (!GetCommState(sh->handle, &dcb)) {
        return -1;
    }
    dcb.BaudRate = (DWORD)baud;
    dcb.ByteSize = 8;
    dcb.Parity = NOPARITY;
    dcb.StopBits = ONESTOPBIT;
    dcb.fBinary = TRUE;
    dcb.fDtrControl = DTR_CONTROL_ENABLE;   // Enable DTR
    dcb.fRtsControl = RTS_CONTROL_ENABLE;   // Enable RTS
    dcb.fDsrSensitivity = FALSE;
    dcb.fTXContinueOnXoff = FALSE;
    dcb.fOutX = FALSE;
    dcb.fInX = FALSE;
    dcb.fErrorChar = FALSE;
    dcb.fNull = FALSE;
    dcb.fAbortOnError = FALSE;
    if (!SetCommState(sh->handle, &dcb)) {
        return -1;
    }

    COMMTIMEOUTS timeouts;
    memset(&timeouts, 0, sizeof(timeouts));
    timeouts.ReadIntervalTimeout = MAXDWORD;    // Return immediately if data available
    timeouts.ReadTotalTimeoutConstant = 0;      // No total timeout
    timeouts.ReadTotalTimeoutMultiplier = 0;    // No per-byte timeout
    timeouts.WriteTotalTimeoutConstant = 1000;  // 1 second write timeout
    timeouts.WriteTotalTimeoutMultiplier = 0;
    if (!SetCommTimeouts(sh->handle, &timeouts)) {
        return -1;
    }

    // Clear any existing data in the buffers
    PurgeComm(sh->handle, PURGE_RXCLEAR | PURGE_TXCLEAR);

    return 0;
}

static int read_serial(serial_handle_t *sh, uint8_t *b)
{
    DWORD read = 0;
    if (!ReadFile(sh->handle, b, 1, &read, NULL)) {
        DWORD error = GetLastError();
        if (error == ERROR_IO_PENDING) {
            // This shouldn't happen with synchronous I/O, but handle it
            return 0;
        }
        // Set errno for compatibility with main error handling
        errno = EIO;
        return -1;
    }
    return (int)read;
}

static void close_serial(serial_handle_t *sh)
{
    if (sh->handle != INVALID_HANDLE_VALUE) {
        CloseHandle(sh->handle);
    }
}

#else
typedef struct {
    int fd;
} serial_handle_t;

static speed_t baud_to_speed(int baud)
{
    switch (baud) {
        case 9600: return B9600;
        case 19200: return B19200;
        case 38400: return B38400;
        case 57600: return B57600;
        case 115200: return B115200;
        case 230400: return B230400;
#ifdef B460800
        case 460800: return B460800;
#endif
#ifdef B921600
        case 921600: return B921600;
#endif
        default: return 0;
    }
}

static int open_serial(serial_handle_t *sh, const char *port, int baud)
{
    sh->fd = open(port, O_RDONLY | O_NOCTTY);
    if (sh->fd < 0) {
        return -1;
    }

    struct termios tio;
    if (tcgetattr(sh->fd, &tio) != 0) {
        return -1;
    }

    cfmakeraw(&tio);
    tio.c_cflag |= (CLOCAL | CREAD);

    speed_t spd = baud_to_speed(baud);
    if (spd == 0) {
        errno = EINVAL;
        return -1;
    }
    cfsetispeed(&tio, spd);
    cfsetospeed(&tio, spd);

    tio.c_cc[VMIN] = 0;
    tio.c_cc[VTIME] = 1;  // 100ms timeout for read operations

    if (tcsetattr(sh->fd, TCSANOW, &tio) != 0) {
        return -1;
    }
    return 0;
}

static int read_serial(serial_handle_t *sh, uint8_t *b)
{
    ssize_t n = read(sh->fd, b, 1);
    if (n < 0) {
        return -1;
    }
    return (int)n;
}

static void close_serial(serial_handle_t *sh)
{
    if (sh->fd >= 0) {
        close(sh->fd);
    }
}
#endif

static void write_wav_header(FILE *wav_file, uint32_t data_size)
{
    wav_header_t header;
    memcpy(header.riff, "RIFF", 4);
    header.chunk_size = 36 + data_size;
    memcpy(header.wave, "WAVE", 4);
    memcpy(header.fmt, "fmt ", 4);
    header.fmt_size = 16;
    header.audio_format = 1;
    header.num_channels = WAV_CHANNELS;
    header.sample_rate = WAV_SAMPLE_RATE;
    header.byte_rate = WAV_SAMPLE_RATE * WAV_CHANNELS * WAV_BITS_PER_SAMPLE / 8;
    header.block_align = WAV_CHANNELS * WAV_BITS_PER_SAMPLE / 8;
    header.bits_per_sample = WAV_BITS_PER_SAMPLE;
    memcpy(header.data, "data", 4);
    header.data_size = data_size;
    
    fseek(wav_file, 0, SEEK_SET);
    fwrite(&header, sizeof(header), 1, wav_file);
}

static void update_wav_file(FILE *wav_file, uint16_t delta_us, bool edge, 
                           int16_t *wav_buffer, size_t *wav_buffer_pos, 
                           size_t wav_buffer_size, bool *current_state)
{
    if (!wav_file) return;
    
    // Calculate number of samples for this time delta
    double delta_seconds = delta_us / 1000000.0;
    size_t samples = (size_t)(delta_seconds * WAV_SAMPLE_RATE);
    
    // Fill buffer with current state
    int16_t sample_value = *current_state ? 16383 : -16383;  // ~50% of max int16
    
    for (size_t i = 0; i < samples && *wav_buffer_pos < wav_buffer_size; i++) {
        wav_buffer[(*wav_buffer_pos)++] = sample_value;
        
        // Flush buffer when full
        if (*wav_buffer_pos >= wav_buffer_size) {
            fwrite(wav_buffer, sizeof(int16_t), wav_buffer_size, wav_file);
            *wav_buffer_pos = 0;
        }
    }
    
    // Update state based on edge type
    *current_state = edge;
}

int main(int argc, char **argv)
{
    const char *port = NULL;
    const char *out_path = NULL;
    const char *wav_path = NULL;
    int baud = 115200;

    for (int i = 1; i < argc; ++i) {
        if (strcmp(argv[i], "-p") == 0 && i + 1 < argc) {
            port = argv[++i];
        } else if (strcmp(argv[i], "-o") == 0 && i + 1 < argc) {
            out_path = argv[++i];
        } else if (strcmp(argv[i], "-b") == 0 && i + 1 < argc) {
            baud = atoi(argv[++i]);
        } else if (strcmp(argv[i], "-w") == 0 && i + 1 < argc) {
            wav_path = argv[++i];
        } else if (strcmp(argv[i], "-h") == 0 || strcmp(argv[i], "--help") == 0) {
            usage(argv[0]);
            return 0;
        } else {
            usage(argv[0]);
            return 1;
        }
    }

    if (!port || !out_path) {
        usage(argv[0]);
        return 1;
    }

    serial_handle_t sh;
    memset(&sh, 0, sizeof(sh));
#ifndef _WIN32
    sh.fd = -1;
#else
    sh.handle = INVALID_HANDLE_VALUE;
#endif

    if (open_serial(&sh, port, baud) != 0) {
        perror("open/configure serial");
        close_serial(&sh);
        return 1;
    }

    FILE *out = fopen(out_path, "wb");
    if (!out) {
        perror("open output file");
        close_serial(&sh);
        return 1;
    }
    
    FILE *wav_file = NULL;
    int16_t *wav_buffer = NULL;
    size_t wav_buffer_pos = 0;
    const size_t wav_buffer_size = 4096;
    bool current_state = false;
    
    if (wav_path) {
        wav_file = fopen(wav_path, "wb");
        if (!wav_file) {
            perror("open WAV file");
            fclose(out);
            close_serial(&sh);
            return 1;
        }
        
        // Allocate WAV buffer
        wav_buffer = malloc(wav_buffer_size * sizeof(int16_t));
        if (!wav_buffer) {
            perror("allocate WAV buffer");
            fclose(wav_file);
            fclose(out);
            close_serial(&sh);
            return 1;
        }
        
        // Write placeholder header (will be updated at end)
        write_wav_header(wav_file, 0);
        fprintf(stderr, "Recording to WAV file: %s\n", wav_path);
    }

    uint8_t frame[2];
    size_t frame_len = 0;
    bool escaping = false;
    uint64_t count = 0;
    double start = now_seconds();
    double last_data_time = start;
    const double timeout_seconds = 5.0;

    for (;;) {
        uint8_t b;
        int n = read_serial(&sh, &b);
        if (n < 0) {
            if (errno == EINTR) {
                continue;
            }
            perror("read");
            break;
        }
        if (n == 0) {
            // Check for timeout when no data is available
            double current_time = now_seconds();
            if (current_time - last_data_time >= timeout_seconds) {
                fprintf(stderr, "No data received for %.1f seconds. Exiting...\n", timeout_seconds);
                break;
            }
            continue;
        }

        // Update time when data is received
        last_data_time = now_seconds();

        if (b == SLIP_END) {
            if (frame_len == 2) {
                fwrite(frame, 1, 2, out);
                
                // Process for WAV file if enabled
                if (wav_file) {
                    uint16_t word = (frame[1] << 8) | frame[0];
                    bool edge = (word & 0x8000) != 0;
                    uint16_t delta_us = word & 0x7FFF;
                    update_wav_file(wav_file, delta_us, edge, wav_buffer, 
                                   &wav_buffer_pos, wav_buffer_size, &current_state);
                }
                
                count++;
                if (count % 1000 == 0) {
                    fflush(out);
                    if (wav_file) fflush(wav_file);
                    double elapsed = now_seconds() - start;
                    double rate = elapsed > 0 ? count / elapsed : 0.0;
                    fprintf(stderr, "%llu samples, %.1f samples/s\n",
                            (unsigned long long)count, rate);
                }
            }
            frame_len = 0;
            escaping = false;
            continue;
        }

        if (escaping) {
            if (b == SLIP_ESC_END) {
                b = SLIP_END;
            } else if (b == SLIP_ESC_ESC) {
                b = SLIP_ESC;
            }
            escaping = false;
        } else if (b == SLIP_ESC) {
            escaping = true;
            continue;
        }

        if (frame_len < sizeof(frame)) {
            frame[frame_len++] = b;
        } else {
            // Oversized frame: discard until next END
            frame_len = 0;
            escaping = false;
        }
    }

    // Finalize WAV file if created
    if (wav_file) {
        // Flush remaining buffer
        if (wav_buffer_pos > 0) {
            fwrite(wav_buffer, sizeof(int16_t), wav_buffer_pos, wav_file);
        }
        
        // Calculate total data size and update header
        long wav_data_size = ftell(wav_file) - sizeof(wav_header_t);
        write_wav_header(wav_file, (uint32_t)wav_data_size);
        
        fclose(wav_file);
        free(wav_buffer);
        fprintf(stderr, "WAV file completed: %ld bytes of audio data\n", wav_data_size);
    }
    
    fclose(out);
    close_serial(&sh);
    return 0;
}
