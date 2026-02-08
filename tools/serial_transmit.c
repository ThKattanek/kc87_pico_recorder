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

    sh->handle = CreateFileA(path, GENERIC_WRITE, 0, NULL, OPEN_EXISTING, 0, NULL);
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
    dcb.fDtrControl = DTR_CONTROL_ENABLE;
    dcb.fRtsControl = RTS_CONTROL_ENABLE;
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
    timeouts.ReadIntervalTimeout = MAXDWORD;
    timeouts.ReadTotalTimeoutConstant = 0;
    timeouts.ReadTotalTimeoutMultiplier = 0;
    timeouts.WriteTotalTimeoutConstant = 1000;  // 1 second write timeout
    timeouts.WriteTotalTimeoutMultiplier = 0;
    if (!SetCommTimeouts(sh->handle, &timeouts)) {
        return -1;
    }

    // Clear any existing data in the buffers
    PurgeComm(sh->handle, PURGE_RXCLEAR | PURGE_TXCLEAR);

    return 0;
}

static int write_serial(serial_handle_t *sh, const uint8_t *data, size_t len)
{
    DWORD written = 0;
    if (!WriteFile(sh->handle, data, (DWORD)len, &written, NULL)) {
        return -1;
    }
    return (int)written;
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
    sh->fd = open(port, O_WRONLY | O_NOCTTY);
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
    tio.c_cc[VTIME] = 1;

    if (tcsetattr(sh->fd, TCSANOW, &tio) != 0) {
        return -1;
    }
    return 0;
}

static int write_serial(serial_handle_t *sh, const uint8_t *data, size_t len)
{
    ssize_t n = write(sh->fd, data, len);
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

static void usage(const char *prog)
{
    fprintf(stderr,
            "Usage: %s -p <port> -i <input_file> [-b baud]\n"
            "  -p <port>       Serial port (e.g., /dev/ttyACM0, COM3)\n"
            "  -i <input_file> Binary input file to transmit\n"
            "  -b <baud>       Baud rate (default: 115200)\n"
            "\n"
            "Example: %s -p /dev/ttyACM0 -i capture.bin -b 115200\n",
            prog, prog);
}

static int slip_write_byte(serial_handle_t *sh, uint8_t b)
{
    uint8_t output[2];
    size_t output_len = 0;
    
    if (b == SLIP_END) {
        output[0] = SLIP_ESC;
        output[1] = SLIP_ESC_END;
        output_len = 2;
    } else if (b == SLIP_ESC) {
        output[0] = SLIP_ESC;
        output[1] = SLIP_ESC_ESC;
        output_len = 2;
    } else {
        output[0] = b;
        output_len = 1;
    }
    
    return write_serial(sh, output, output_len);
}

static int send_frame(serial_handle_t *sh, const uint8_t *data, size_t len)
{
    uint8_t end_byte = SLIP_END;
    
    // Send frame start
    if (write_serial(sh, &end_byte, 1) < 0) return -1;
    
    // Send data with SLIP encoding
    for (size_t i = 0; i < len; i++) {
        if (slip_write_byte(sh, data[i]) < 0) return -1;
    }
    
    // Send frame end
    if (write_serial(sh, &end_byte, 1) < 0) return -1;
    
    return 0;
}

int main(int argc, char **argv)
{
    const char *port = NULL;
    const char *input_path = NULL;
    int baud = 115200;

    for (int i = 1; i < argc; ++i) {
        if (strcmp(argv[i], "-p") == 0 && i + 1 < argc) {
            port = argv[++i];
        } else if (strcmp(argv[i], "-i") == 0 && i + 1 < argc) {
            input_path = argv[++i];
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

    if (!port || !input_path) {
        usage(argv[0]);
        return 1;
    }

    // Open input file
    FILE *input = fopen(input_path, "rb");
    if (!input) {
        perror("open input file");
        return 1;
    }

    // Initialize serial connection
    serial_handle_t sh;
    memset(&sh, 0, sizeof(sh));
#ifndef _WIN32
    sh.fd = -1;
#else
    sh.handle = INVALID_HANDLE_VALUE;
#endif

    if (open_serial(&sh, port, baud) != 0) {
        perror("open/configure serial");
        fclose(input);
        return 1;
    }

    printf("Transmitting %s to %s at %d baud...\n", input_path, port, baud);
    
    // Get file size
    fseek(input, 0, SEEK_END);
    long file_size = ftell(input);
    fseek(input, 0, SEEK_SET);
    
    printf("File size: %ld bytes (%ld samples)\n", file_size, file_size / 2);
    
    uint8_t sample[2];
    uint64_t samples_sent = 0;
    uint64_t total_samples = file_size / 2;
    
    // Send each 2-byte sample as a SLIP frame
    while (fread(sample, 1, 2, input) == 2) {
        if (send_frame(&sh, sample, 2) != 0) {
            perror("send sample");
            break;
        }
        
        samples_sent++;
        
        // Progress indicator
        if (samples_sent % 1000 == 0) {
            double progress = (double)samples_sent / total_samples * 100.0;
            printf("Progress: %llu/%llu samples (%.1f%%)\n", 
                   samples_sent, total_samples, progress);
        }
        
        // Small delay to prevent overwhelming the Pico
        // Adjust this based on your requirements - smaller delay for better timing
#ifdef _WIN32
        Sleep(0);  // Yield to other processes but don't sleep
#else
        usleep(100);  // 0.1ms - reduced from 1ms
#endif
    }
    
    printf("Transmission complete. Sent %llu samples.\n", samples_sent);
    
    fclose(input);
    close_serial(&sh);
    return 0;
}