#include <errno.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

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

static void usage(const char *prog)
{
    fprintf(stderr,
            "Usage: %s -p <port> -o <out_file> [-b baud]\n"
            "Example: %s -p /dev/ttyACM0 -o capture.bin -b 115200\n",
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
    dcb.fDtrControl = DTR_CONTROL_DISABLE;
    dcb.fRtsControl = RTS_CONTROL_DISABLE;
    if (!SetCommState(sh->handle, &dcb)) {
        return -1;
    }

    COMMTIMEOUTS timeouts;
    memset(&timeouts, 0, sizeof(timeouts));
    timeouts.ReadIntervalTimeout = 1;
    timeouts.ReadTotalTimeoutConstant = 1;
    timeouts.ReadTotalTimeoutMultiplier = 0;
    if (!SetCommTimeouts(sh->handle, &timeouts)) {
        return -1;
    }

    return 0;
}

static int read_serial(serial_handle_t *sh, uint8_t *b)
{
    DWORD read = 0;
    if (!ReadFile(sh->handle, b, 1, &read, NULL)) {
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

    tio.c_cc[VMIN] = 1;
    tio.c_cc[VTIME] = 0;

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

int main(int argc, char **argv)
{
    const char *port = NULL;
    const char *out_path = NULL;
    int baud = 115200;

    for (int i = 1; i < argc; ++i) {
        if (strcmp(argv[i], "-p") == 0 && i + 1 < argc) {
            port = argv[++i];
        } else if (strcmp(argv[i], "-o") == 0 && i + 1 < argc) {
            out_path = argv[++i];
        } else if (strcmp(argv[i], "-b") == 0 && i + 1 < argc) {
            baud = atoi(argv[++i]);
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

    uint8_t frame[2];
    size_t frame_len = 0;
    bool escaping = false;
    uint64_t count = 0;
    double start = now_seconds();

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
            continue;
        }

        if (b == SLIP_END) {
            if (frame_len == 2) {
                fwrite(frame, 1, 2, out);
                count++;
                if (count % 1000 == 0) {
                    fflush(out);
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

    fclose(out);
    close_serial(&sh);
    return 0;
}
