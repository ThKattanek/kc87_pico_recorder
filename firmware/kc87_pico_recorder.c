#include <stdio.h>
#include "pico/stdlib.h"
#include "hardware/gpio.h"
#include "hardware/timer.h"
#include "hardware/irq.h"
#include "config.h"

volatile uint32_t last_timestamp = 0;
volatile uint32_t timestamp = 0;
bool edge_type = false; // false = falling, true = rising

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
        if(timestamp != 0) 
        {
            // Hier können Sie den gespeicherten Zeitstempel verarbeiten
            printf("Flanke erkannt: %u, Typ: %s\n", timestamp - last_timestamp, edge_type ? "steigend" : "fallend");
            last_timestamp = timestamp;
            timestamp = 0;
        }
    }
}
