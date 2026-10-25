// KC-87 PLAY-Test fuer Raspberry Pi Pico (Pico SDK, C)
//
// Gibt an GP3 (PLAY_SIGNAL_PICO) ein Rechtecksignal mit 50 % Tastverhaeltnis aus,
// um den Pegel der PLAY-Schaltung (R4, R5, RV2) am KC-87 einzustellen. Die
// Frequenzen sind die Sollwerte des KC-87 (hc-ddr.hucki.net, Kassetteninterface):
// Trennzeichen 600 Hz, 1-Bit 1200 Hz, 0-Bit 2400 Hz.
//
// Steuerung ueber USB-CDC (ein Zeichen genuegt):
//   1 = 1200 Hz  (Bit 1)
//   2 = 2400 Hz  (Bit 0)
//   3 =  600 Hz  (Trennzeichen)
//   a = 1200 Hz und 2400 Hz im Wechsel, jeweils 1 s
//   0 = Ausgang aus (Low)
//   h = Hilfe

#include <stdio.h>
#include "pico/stdlib.h"
#include "hardware/pwm.h"
#include "hardware/clocks.h"

#define PIN_OUT      3
#define FREQ_BIT1    1200u             // Periode ca. 833 us
#define FREQ_BIT0    2400u             // Periode ca. 417 us
#define FREQ_SEP      600u             // Periode ca. 1667 us
#define ALT_MS       1000

static uint slice, chan;

static void led(bool on) {
#ifdef PICO_DEFAULT_LED_PIN
    gpio_put(PICO_DEFAULT_LED_PIN, on);
#else
    (void)on;
#endif
}

// Setzt die PWM auf hz (50 % Tastverhaeltnis), hz = 0 schaltet den Ausgang auf Low.
static void tone(uint32_t hz) {
    if (hz == 0) {
        pwm_set_chan_level(slice, chan, 0);
        led(false);
        printf("Ausgang aus\n");
        return;
    }
    uint32_t clk = clock_get_hz(clk_sys);
    uint64_t per16 = (uint64_t)clk * 16u;                       // Takt in 1/16-Schritten
    uint32_t div16 = (uint32_t)((per16 + (uint64_t)hz * 65536u - 1) / ((uint64_t)hz * 65536u));
    if (div16 < 16) div16 = 16;                                 // Teiler mindestens 1
    uint32_t wrap = (uint32_t)(per16 / ((uint64_t)div16 * hz)) - 1;
    pwm_set_clkdiv_int_frac(slice, div16 >> 4, div16 & 15);
    pwm_set_wrap(slice, (uint16_t)wrap);
    pwm_set_chan_level(slice, chan, (uint16_t)((wrap + 1) / 2));
    led(true);
    printf("%lu Hz (eingestellt: %lu Hz)\n", (unsigned long)hz,
           (unsigned long)(per16 / ((uint64_t)div16 * (wrap + 1))));
}

static void help(void) {
    printf("KC87 PLAY-Test, Ausgang GP3\n"
           "  1 = %u Hz (Bit 1)   2 = %u Hz (Bit 0)   3 = %u Hz (Trennzeichen)\n"
           "  a = 1/2 im Wechsel (1 s)   0 = aus   h = Hilfe\n",
           FREQ_BIT1, FREQ_BIT0, FREQ_SEP);
}

int main(void) {
    stdio_init_all();
#ifdef PICO_DEFAULT_LED_PIN
    gpio_init(PICO_DEFAULT_LED_PIN);
    gpio_set_dir(PICO_DEFAULT_LED_PIN, true);
#endif

    gpio_set_function(PIN_OUT, GPIO_FUNC_PWM);
    slice = pwm_gpio_to_slice_num(PIN_OUT);
    chan = pwm_gpio_to_channel(PIN_OUT);
    pwm_set_chan_level(slice, chan, 0);
    pwm_set_enabled(slice, true);

    bool alternate = true;
    bool bit1 = true;
    absolute_time_t next = get_absolute_time();
    tone(FREQ_BIT1);                 // Start: gleich ein Signal fuer das Oszilloskop
    bool announced = false;

    while (true) {
        if (!announced && stdio_usb_connected()) {
            sleep_ms(100);
            help();
            announced = true;
        }

        int ch = getchar_timeout_us(1000);
        switch (ch) {
        case '1': alternate = false; tone(FREQ_BIT1); break;
        case '2': alternate = false; tone(FREQ_BIT0); break;
        case '3': alternate = false; tone(FREQ_SEP); break;
        case '0': alternate = false; tone(0); break;
        case 'a': alternate = true; bit1 = true; next = get_absolute_time(); break;
        case 'h': case '?': help(); break;
        default: break;
        }

        if (alternate && absolute_time_diff_us(get_absolute_time(), next) <= 0) {
            tone(bit1 ? FREQ_BIT1 : FREQ_BIT0);
            bit1 = !bit1;
            next = delayed_by_ms(next, ALT_MS);
        }
    }
}
