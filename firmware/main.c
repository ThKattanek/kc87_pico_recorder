// KC-87 Signal-Logger fuer Raspberry Pi Pico (Pico SDK, C)
//
// Misst per PIO die Dauer jedes High- und Low-Pulses an GP2 und streamt sie
// ueber USB-CDC zum PC. Die PIO-Werte laufen per DMA in einen 32-KB-Ringpuffer,
// dadurch gehen auch bei kurzen USB-Verzoegerungen keine Pulse verloren.
//
// Protokoll (alle Woerter uint32 little-endian):
//   PC -> Pico : 'S' = Aufnahme starten, 'X' = Aufnahme stoppen
//   Pico -> PC : Header   'K','C','8','7', tick_hz
//                Daten    Dauer in Ticks, abwechselnd High, Low, High, ...
//                Ende     0xFFFFFFFF, Status (Bit0 = Ringpuffer-Ueberlauf,
//                         Bit1 = PIO-Stall), Anzahl Werte

#include <stdio.h>
#include <string.h>
#include "pico/stdlib.h"
#include "hardware/pio.h"
#include "hardware/dma.h"
#include "hardware/clocks.h"
#include "pulse_timer.pio.h"

#define PIN_IN       2                 // GP2 = Pin 4
#define SM_FREQ_HZ   10000000u         // 2 Takte pro Schritt -> 5 MHz = 0,2 us
#define RING_BITS    15                // 2^15 Byte = 32 KB
#define RING_WORDS   (1u << (RING_BITS - 2))
#define CHUNK_WORDS  256
#define END_MARKER   0xFFFFFFFFu

static uint32_t ring[RING_WORDS] __attribute__((aligned(1u << RING_BITS)));
static uint32_t txbuf[CHUNK_WORDS];

static PIO pio = pio0;
static uint sm, offset;
static int dma_ch;
static uint32_t tick_hz;

static void send(const void *p, int len) {
    stdio_put_string((const char *)p, len, false, false);
}

static void led(bool on) {
#ifdef PICO_DEFAULT_LED_PIN
    gpio_put(PICO_DEFAULT_LED_PIN, on);
#else
    (void)on;
#endif
}

static void setup_pio(void) {
    offset = pio_add_program(pio, &pulse_timer_program);
    sm = pio_claim_unused_sm(pio, true);

    pio_gpio_init(pio, PIN_IN);
    gpio_set_dir(PIN_IN, false);
    gpio_pull_down(PIN_IN);
    gpio_set_input_hysteresis_enabled(PIN_IN, true);   // Schmitt-Trigger
    pio_sm_set_consecutive_pindirs(pio, sm, PIN_IN, 1, false);

    pio_sm_config c = pulse_timer_program_get_default_config(offset);
    sm_config_set_in_pins(&c, PIN_IN);
    sm_config_set_jmp_pin(&c, PIN_IN);
    sm_config_set_fifo_join(&c, PIO_FIFO_JOIN_RX);
    float div = (float)clock_get_hz(clk_sys) / SM_FREQ_HZ;
    sm_config_set_clkdiv(&c, div);
    pio_sm_init(pio, sm, offset, &c);

    tick_hz = (uint32_t)((float)clock_get_hz(clk_sys) / div / 2.0f + 0.5f);
    dma_ch = dma_claim_unused_channel(true);
}

static void start_capture(void) {
    pio_sm_set_enabled(pio, sm, false);
    dma_channel_abort(dma_ch);
    pio_sm_clear_fifos(pio, sm);
    pio_sm_restart(pio, sm);
    pio_sm_exec(pio, sm, pio_encode_jmp(offset));
    pio->fdebug = 0xFFFFFFFFu;                         // Debug-Flags loeschen

    dma_channel_config dc = dma_channel_get_default_config(dma_ch);
    channel_config_set_transfer_data_size(&dc, DMA_SIZE_32);
    channel_config_set_read_increment(&dc, false);
    channel_config_set_write_increment(&dc, true);
    channel_config_set_ring(&dc, true, RING_BITS);     // Ring auf Schreibadresse
    channel_config_set_dreq(&dc, pio_get_dreq(pio, sm, false));
    dma_channel_configure(dma_ch, &dc, ring, &pio->rxf[sm], 0xFFFFFFFFu, true);

    pio_sm_set_enabled(pio, sm, true);
}

static void capture(void) {
    const uint32_t header[2] = { 0x3738434Bu /* "KC87" */, tick_hz };
    send(header, sizeof header);

    start_capture();
    led(true);

    uint32_t consumed = 0;
    uint32_t status = 0;
    bool stop = false;

    while (true) {
        uint32_t written = 0xFFFFFFFFu - dma_hw->ch[dma_ch].transfer_count;
        uint32_t avail = written - consumed;

        if (avail > RING_WORDS) {                      // Daten ueberschrieben
            status |= 1u;
            consumed = written - RING_WORDS;
            avail = RING_WORDS;
        }

        if (avail) {
            uint32_t idx = consumed & (RING_WORDS - 1);
            uint32_t n = avail;
            if (n > CHUNK_WORDS) n = CHUNK_WORDS;
            if (n > RING_WORDS - idx) n = RING_WORDS - idx;
            for (uint32_t i = 0; i < n; i++)
                txbuf[i] = ~ring[idx + i];             // Zaehlerstand -> Ticks
            consumed += n;
            send(txbuf, (int)(n * 4));
            continue;                                  // erst Puffer leeren
        }

        if (stop) break;

        int ch = getchar_timeout_us(0);
        if (ch == 'X') {
            pio_sm_set_enabled(pio, sm, false);
            busy_wait_us(50);                          // letzte DMA-Transfers abwarten
            stop = true;
        }
    }

    dma_channel_abort(dma_ch);
    if (pio->fdebug & (1u << (PIO_FDEBUG_RXSTALL_LSB + sm)))
        status |= 2u;

    const uint32_t trailer[3] = { END_MARKER, status, consumed };
    send(trailer, sizeof trailer);
    stdio_flush();
    led(false);
}

int main(void) {
    stdio_init_all();
#ifdef PICO_DEFAULT_LED_PIN
    gpio_init(PICO_DEFAULT_LED_PIN);
    gpio_set_dir(PICO_DEFAULT_LED_PIN, true);
    gpio_put(PICO_DEFAULT_LED_PIN, true);
#endif
    setup_pio();

    while (true) {
        int ch = getchar_timeout_us(100000);
        if (ch == 'S')
            capture();
    }
}
