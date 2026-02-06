#include <stdio.h>
#include "pico/stdlib.h"
#include "hardware/gpio.h"
#include "hardware/timer.h"
#include "hardware/irq.h"
#include "hardware/sync.h"
#include "config.h"

// Recording variables
volatile uint32_t last_timestamp = 0;
volatile uint32_t timestamp = 0;

// SLIP special bytes for output encoding
#define SLIP_END     0xC0
#define SLIP_ESC     0xDB
#define SLIP_ESC_END 0xDC  
#define SLIP_ESC_ESC 0xDD

static void slip_write_byte(uint8_t b)
{
    if (b == SLIP_END) {
        putchar_raw(SLIP_ESC);
        putchar_raw(SLIP_ESC_END);
    } else if (b == SLIP_ESC) {
        putchar_raw(SLIP_ESC);
        putchar_raw(SLIP_ESC_ESC);
    } else {
        putchar_raw(b);
    }
}

static void send_sample(uint16_t delta_us, bool edge)
{
    // Encode: Edge-Bit in MSB, Delta in lower 15 bits
    uint16_t data = delta_us | (edge ? 0x8000 : 0x0000);
    
    uint8_t b0 = data & 0xFF;        // LSB
    uint8_t b1 = (data >> 8) & 0xFF; // MSB

    // SLIP frame: END <payload> END
    putchar_raw(SLIP_END);
    slip_write_byte(b0);
    slip_write_byte(b1);
    putchar_raw(SLIP_END);
}

void gpio_callback(uint gpio, uint32_t events)
{
    if (gpio != GPIO_RECORD_PIN)
        return;

    timestamp = time_us_32();
    
    // Calculate delta from last timestamp
    uint32_t delta_us;
    if (timestamp >= last_timestamp) {
        delta_us = timestamp - last_timestamp;
    } else {
        // Timer overflow handling 
        delta_us = (UINT32_MAX - last_timestamp) + timestamp + 1;
    }
    
    // Limit delta to 15-bit range (32767 μs = ~32ms max)
    if (delta_us > 32767) {
        delta_us = 32767;
    }
    
    // Determine edge type
    bool current_edge = (events & GPIO_IRQ_EDGE_RISE) != 0;
    
    // Send sample
    send_sample((uint16_t)delta_us, current_edge);
    
    last_timestamp = timestamp;
}

int main()
{
    stdio_init_all();

    // GPIO-Pins konfigurieren (nur Recording)
    gpio_init(GPIO_RECORD_PIN);
    gpio_set_dir(GPIO_RECORD_PIN, GPIO_IN);
    
    // IRQ für Recording-GPIO konfigurieren
    timestamp = last_timestamp = time_us_32();
    sleep_us(2);
    gpio_set_irq_enabled_with_callback(GPIO_RECORD_PIN, GPIO_IRQ_EDGE_RISE | GPIO_IRQ_EDGE_FALL, true, gpio_callback);

    printf("KC87 Pico Recorder - Ready (Recording Only)\n");
    printf("Trigger GPIO_2 to start recording...\n");
    
    // Hauptschleife - nur warten
    while (true) {
        sleep_ms(1000);
    }

    return 0;
}