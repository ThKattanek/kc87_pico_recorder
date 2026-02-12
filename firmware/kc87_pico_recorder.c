#include <stdio.h>
#include "pico/stdlib.h"
#include "pico/stdio_usb.h"
#include "hardware/gpio.h"
#include "hardware/timer.h"
#include "hardware/uart.h"
#include "config.h"

// UART Configuration (Debug Probe UART Bridge)
// UART0: TX = GPIO0, RX = GPIO1
#define UART_ID uart0
#define UART_TX_PIN 0
#define UART_RX_PIN 1
#define UART_BAUD_RATE 115200

// Serial Data Encoding
// Data format: 16-bit sample where MSB is edge type (1=Rising, 0=Falling) and lower 15 bits are delta in microseconds
// Example: 0x800A = Rising edge with 10 μs delta, 0x0005 = Falling edge with 5 μs delta
// Data sent in an Block-Format with max 255 samples per block (510 bytes + 5 bytes overhead) for efficient transmission.
// Block format: [START-BLOCK][SAMPLE_COUNT][SAMPLE0][SAMPLE1][SAMPLE2]...[SAMPLEN][END-BLOCK]
// START-BLOCK = 0x0000, END-BLOCK = 0x8000

// Header Block structure:
// 0x0000 - 0x0000 [2 Bytes] START-BLOCK
// 0x0002 - 0x00   [1 Byte]  BLOCK_TYPE (0x00 = Header-Block)
// 0x0003 - 0x01   [1 Byte]  VERSION (0x01)
// 0x0004 - 0x8000 [2 Bytes] END-BLOCK (0x8000)

// Sample Block structure (version 0x01):
// 0x0000 - 0x0000 [2 Bytes] START-BLOCK
// 0x0002 - 0x..   [1 Byte] BLOCK_TYPE (0x01 = Sample-Block
// 0x0003 - 0x..   [1 Byte] SAMPLE_COUNT (N, max 255)
// 0x0004 - 0x..   [2*N Bytes] SAMPLE0, SAMPLE1, ..., SAMPLEN-1
// 0x..   - 0x8000 [2 Bytes] END-BLOCK (0x8000)

// End of Block is always signaled by the fixed END-BLOCK marker (0x8000). The number of samples is specified in the SAMPLE_COUNT field.
// End of Stream is signaled by consecutive 0x8000 marker 0x8000 

// Recording variables
volatile uint32_t last_timestamp = 0;
volatile uint32_t timestamp = 0;
volatile bool recording = false;
volatile bool send_header_flag = false;
volatile uint16_t sample_count = 0;
volatile uint16_t sample_buffer[256]; // Buffer for 256 samples (512 bytes)
volatile uint16_t ring_buffer[1024]; // Ring buffer for 1024 samples
volatile uint16_t ring_head = 0;
volatile uint16_t ring_tail = 0;

#define RECORDING_TIMEOUT_US 5 * 1000 * 1000 // 5s inactivity timeout

void send_header_block()
{
    printf("[DEBUG] Sending header block\n");
    // Header block format:
    // START-BLOCK (0x0000), BLOCK_TYPE (0x00), VERSION (0x01), END-BLOCK (0x8000)
    uint8_t header_buf[6] = {
        0x00, 0x00,  // START-BLOCK
        0x00,        // BLOCK_TYPE: Header-Block
        0x01,        // VERSION: 0x01
        0x00, 0x80   // END-BLOCK
    };
    uart_write_blocking(UART_ID, header_buf, 6);
}
    
void gpio_callback(uint gpio, uint32_t events)
{
    if (gpio != GPIO_RECORD_PIN)
        return;

    if (!recording) 
    {
        // Start recording on first trigger
        recording = true;
        send_header_flag = true; // Flag to send header block on next sample
        printf("[DEBUG] Recording started\n");
    }

    timestamp = time_us_32();
    
    // Calculate delta from last timestamp
    uint32_t delta_us;
    if (timestamp >= last_timestamp) 
    {
        delta_us = timestamp - last_timestamp;
    } else 
    {
        // Timer overflow handling 
        delta_us = (UINT32_MAX - last_timestamp) + timestamp + 1;
    }
    
    // Limit delta to 15-bit range (32767 μs = ~32ms max)
    if (delta_us > 32767) 
    {
        delta_us = 32767;
    }
    
    // Encode: Edge-Bit in MSB (Rise=1, Fall=0), Delta in lower 15 bits
    uint16_t edge_bit = (events & GPIO_IRQ_EDGE_RISE) ? 0x8000 : 0x0000;
    uint16_t next_head = (ring_head + 1) % 1024;
    if (next_head == ring_tail) {
        // Drop oldest sample on overflow to keep ring buffer consistent.
        ring_tail = (ring_tail + 1) % 1024;
    }
    ring_buffer[ring_head] = (uint16_t)delta_us | edge_bit;
    ring_head = next_head;
    
    last_timestamp = timestamp;
}

int main() 
{   
    // Initialize ONLY USB stdio for debug output (not UART!)
    stdio_usb_init();

    sleep_ms(1000); // Short delay to ensure USB connection is established before printing debug messages

    printf("KC87 Pico Recorder - ");
    printf("Version: " FW_VERSION_STRING "\n");
    
    // Initialize UART for binary data stream (Debug Probe UART or FT232L)
    uart_init(UART_ID, UART_BAUD_RATE);
    gpio_set_function(UART_TX_PIN, GPIO_FUNC_UART);
    gpio_set_function(UART_RX_PIN, GPIO_FUNC_UART);
    
    // GPIO-Pins konfigurieren (nur Recording)
    gpio_init(GPIO_RECORD_PIN);
    gpio_set_dir(GPIO_RECORD_PIN, GPIO_IN);
    
    // IRQ für Recording-GPIO konfigurieren
    timestamp = last_timestamp = time_us_32();
    sleep_us(2);
    gpio_set_irq_enabled_with_callback(GPIO_RECORD_PIN, GPIO_IRQ_EDGE_RISE | GPIO_IRQ_EDGE_FALL, true, gpio_callback);

    printf("[DEBUG] KC87 Pico Recorder started\n");
    printf("[DEBUG] UART: %d baud on GPIO%d/GPIO%d\n", UART_BAUD_RATE, UART_TX_PIN, UART_RX_PIN);
    printf("[DEBUG] Waiting for signal on GPIO%d...\n", GPIO_RECORD_PIN);

    uint32_t delta_us;
    uint32_t last_timeout_check = 0;

    // Hauptschleife
    while (true) 
    {
        if(send_header_flag) 
        {
            send_header_block();
            send_header_flag = false;
            sample_count = 0; // Reset sample count for new recording session
            last_timeout_check = time_us_32(); // Reset timeout check timer
        }

        if(recording)
        {
            // Drain ring buffer in batch (up to 255 samples at once)
            while (ring_tail != ring_head && sample_count < 255) 
            {
                sample_buffer[sample_count++] = ring_buffer[ring_tail];
                ring_tail = (ring_tail + 1) % 1024;
            }

            // If we have 255 samples, send a block
            if (sample_count == 255) 
            {
                    // Send block format:
                    // START-BLOCK (0x0000), BLOCK_TYPE (0x01), SAMPLE_COUNT (0xFF), SAMPLES..., END-BLOCK (0x8000)
                    // Total: 4 bytes header + 510 bytes samples + 2 bytes end = 516 bytes
                    uint8_t block_buf[516];
                    block_buf[0] = 0x00; // START-BLOCK LSB
                    block_buf[1] = 0x00; // START-BLOCK MSB
                    block_buf[2] = 0x01; // BLOCK_TYPE: Sample-Block
                    block_buf[3] = 0xFF; // SAMPLE_COUNT: 255

                    for (int i = 0; i < 255; i++) 
                    {
                        block_buf[4 + i * 2] = sample_buffer[i] & 0xFF;       // Sample LSB
                        block_buf[4 + i * 2 + 1] = (sample_buffer[i] >> 8) & 0xFF; // Sample MSB
                    }

                    block_buf[514] = 0x00; // END-BLOCK LSB
                    block_buf[515] = 0x80; // END-BLOCK MSB
                    
                    uart_write_blocking(UART_ID, block_buf, 516);
                    printf("[DEBUG] Sent data block (255 samples)\n");

                    sample_count = 0; // Reset sample count for next block
            }
            
            // Check timeout periodically (every 100ms) even if ring buffer is not empty
            uint32_t current_time = time_us_32();
            uint32_t time_since_last_check;
            if (current_time >= last_timeout_check) {
                time_since_last_check = current_time - last_timeout_check;
            } else {
                time_since_last_check = (UINT32_MAX - last_timeout_check) + current_time + 1;
            }
            
            if (time_since_last_check >= 100000) // Check every 100ms
            {
                last_timeout_check = current_time;
                
                // Calculate delta from last edge event to check for inactivity
                // Handle timer overflow
                if (current_time >= timestamp) {
                    delta_us = current_time - timestamp;
                } else {
                    delta_us = (UINT32_MAX - timestamp) + current_time + 1;
                }

                if(delta_us > RECORDING_TIMEOUT_US) // inactivity timeout
                {
                    if(sample_count > 0) // If there are remaining samples, send them in a final block
                    {
                        // Send remaining samples in a final block
                        uint8_t final_block_buf[4 + sample_count * 2 + 2];
                        final_block_buf[0] = 0x00; // START-BLOCK LSB
                        final_block_buf[1] = 0x00; // START-BLOCK MSB
                        final_block_buf[2] = 0x01; // BLOCK_TYPE: Sample-Block
                        final_block_buf[3] = sample_count & 0xFF; // SAMPLE_COUNT

                        for (int i = 0; i < sample_count; i++) 
                        {
                            final_block_buf[4 + i * 2] = sample_buffer[i] & 0xFF; // Sample LSB
                            final_block_buf[4 + i * 2 + 1] = (sample_buffer[i] >> 8) & 0xFF; // Sample MSB
                        }

                        final_block_buf[4 + sample_count * 2] = 0x00; // END-BLOCK LSB
                        final_block_buf[4 + sample_count * 2 + 1] = 0x80; // END-BLOCK MSB
                        
                        uart_write_blocking(UART_ID, final_block_buf, 4 + sample_count * 2 + 2);
                        printf("[DEBUG] Sent final block (%d samples)\n", sample_count);
                    }
                    
                    // Send End of Stream marker (two consecutive END-BLOCKs)
                    uint8_t end_stream[2] = {0x00, 0x80};
                    uart_write_blocking(UART_ID, end_stream, 2);
                    printf("[DEBUG] Recording stopped (timeout after %d us inactivity)\n", delta_us);

                    recording = false; // Stop recording until next trigger
                    sample_count = 0; // Reset sample count
                    ring_head = ring_tail = 0; // Clear ring buffer
                }
            }
        }
        
        if (!recording || ring_tail == ring_head) {
            sleep_us(100); // Short sleep to service USB stack (printf) when no data pending
        }
    }

    return 0;
}