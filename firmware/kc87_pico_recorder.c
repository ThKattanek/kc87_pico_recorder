#include <stdio.h>
#include "pico/stdlib.h"
#include "hardware/gpio.h"
#include "hardware/timer.h"
#include "hardware/irq.h"
#include "hardware/sync.h"
#include "config.h"

volatile uint32_t last_timestamp = 0;
volatile uint32_t timestamp = 0;
bool edge_type = false; // false = falling, true = rising

// SLIP special bytes
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
    uint16_t word = (uint16_t)(((edge ? 1u : 0u) << 15) | (delta_us & 0x7FFFu));
    uint8_t b0 = (uint8_t)(word & 0xFFu);
    uint8_t b1 = (uint8_t)((word >> 8) & 0xFFu);

    // SLIP frame: END <payload> END
    putchar_raw(SLIP_END);
    slip_write_byte(b0);
    slip_write_byte(b1);
    putchar_raw(SLIP_END);
}

void gpio_callback(uint gpio, uint32_t events)
{
    timestamp = time_us_32();
  
    if (events & GPIO_IRQ_EDGE_RISE) 
    {
        // Flanke erkannt: Anstieg
        edge_type = true;
    }
    
    if (events & GPIO_IRQ_EDGE_FALL) {
        // Flanke erkannt: Abfall
        edge_type = false;  
    }
}   

int main()
{
    stdio_init_all();

    // GPIO als Eingang konfigurieren
    gpio_init(GPIO_RECORD_PIN);
    gpio_set_dir(GPIO_RECORD_PIN, GPIO_IN);
    
    // IRQ für GPIO konfigurieren
    timestamp = last_timestamp = time_us_32();
    sleep_us(2);
    gpio_set_irq_enabled_with_callback(GPIO_RECORD_PIN, GPIO_IRQ_EDGE_RISE | GPIO_IRQ_EDGE_FALL, true, gpio_callback);

    while (true)
    {
        uint32_t ts = 0;
        bool edge = false;

        uint32_t irq_state = save_and_disable_interrupts();
        if (timestamp != 0) {
            ts = timestamp;
            edge = edge_type;
            timestamp = 0;
        }
        restore_interrupts(irq_state);

        if (ts != 0)
        {
            uint32_t delta = ts - last_timestamp;
            last_timestamp = ts;
            if (delta > 0x7FFFu) {
                delta = 0x7FFFu;
            }
            send_sample((uint16_t)delta, edge);
        }
    }
}
