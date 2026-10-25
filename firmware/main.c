// KC-87 Signal-Logger und -Player fuer Raspberry Pi Pico (Pico SDK, C)
//
// Aufnahme: Misst per PIO die Dauer jedes High- und Low-Pulses an GP2 und streamt
// sie ueber USB-CDC zum PC. Die PIO-Werte laufen per DMA in einen 32-KB-Ringpuffer,
// dadurch gehen auch bei kurzen USB-Verzoegerungen keine Pulse verloren.
//
// Wiedergabe: Der PC schickt die Pulsdauern, ein PIO-Programm gibt sie an GP3 aus.
//
// Protokoll (alle Woerter uint32 little-endian):
//   PC -> Pico : 'S' = Aufnahme starten, 'X' = Aufnahme stoppen, 'P' = Wiedergabe
//   Aufnahme, Pico -> PC:
//                Header   'K','C','8','7', tick_hz
//                Daten    Dauer in Ticks, abwechselnd High, Low, High, ...
//                Ende     0xFFFFFFFF, Status (Bit0 = Ringpuffer-Ueberlauf,
//                         Bit1 = PIO-Stall), Anzahl Werte
//   Wiedergabe:  Pico -> PC Header (wie oben), danach PC -> Pico Dauern in Ticks
//                (High, Low, ...) und 0xFFFFFFFF als Ende. Zum Schluss Pico -> PC:
//                0xFFFFFFFF, Status (Bit0 = Unterlauf, Bit1 = Abbruch), Anzahl Werte

#include <stdio.h>
#include <string.h>
#include "pico/stdlib.h"
#include "hardware/pio.h"
#include "hardware/dma.h"
#include "hardware/clocks.h"
#include "pulse_timer.pio.h"
#include "pulse_out.pio.h"

#define PIN_IN       2                 // GP2 = Pin 4
#define PIN_OUT      3                 // GP3 = Pin 5 (PLAY_SIGNAL_PICO)
#define SM_FREQ_HZ   10000000u         // 2 Takte pro Schritt -> 5 MHz = 0,2 us
#define RING_BITS    15                // 2^15 Byte = 32 KB
#define RING_WORDS   (1u << (RING_BITS - 2))
#define CHUNK_WORDS  256
#define END_MARKER   0xFFFFFFFFu

#define PLAY_FREQ_HZ     5000000u          // 1 Takt pro Schritt -> 5 MHz = 0,2 us
#define PLAY_OVERHEAD    4u                // Takte pro Puls ausser der Zaehlschleife
#define PLAY_RING_WORDS  8192u
#define PLAY_PREFILL     2048u             // so viele Pulse sammeln, bevor die Ausgabe startet
#define PLAY_IDLE_TICKS  100000u           // 20 ms Low am Ende
#define PLAY_TIMEOUT_US  5000000u          // so lange darf der PC nichts senden

static uint32_t ring[RING_WORDS] __attribute__((aligned(1u << RING_BITS)));
static uint32_t txbuf[CHUNK_WORDS];

static PIO pio = pio0;
static uint sm, offset;
static int dma_ch;
static uint32_t tick_hz;

static uint32_t play_ring[PLAY_RING_WORDS];
static PIO play_pio = pio1;
static uint play_sm, play_offset;
static uint32_t play_tick_hz;

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

static void setup_play(void) {
    play_offset = pio_add_program(play_pio, &pulse_out_program);
    play_sm = pio_claim_unused_sm(play_pio, true);

    pio_gpio_init(play_pio, PIN_OUT);
    pio_sm_set_consecutive_pindirs(play_pio, play_sm, PIN_OUT, 1, true);

    pio_sm_config c = pulse_out_program_get_default_config(play_offset);
    sm_config_set_set_pins(&c, PIN_OUT, 1);
    sm_config_set_fifo_join(&c, PIO_FIFO_JOIN_TX);
    float div = (float)clock_get_hz(clk_sys) / PLAY_FREQ_HZ;
    sm_config_set_clkdiv(&c, div);
    pio_sm_init(play_pio, play_sm, play_offset, &c);

    play_tick_hz = (uint32_t)((float)clock_get_hz(clk_sys) / div + 0.5f);
}

static void play_stop_low(void) {
    pio_sm_set_enabled(play_pio, play_sm, false);
    pio_sm_exec(play_pio, play_sm, pio_encode_set(pio_pins, 0));
}

static void play(void) {
    const uint32_t header[2] = { 0x3738434Bu /* "KC87" */, play_tick_hz };
    send(header, sizeof header);
    stdio_flush();

    pio_sm_set_enabled(play_pio, play_sm, false);
    pio_sm_clear_fifos(play_pio, play_sm);
    pio_sm_restart(play_pio, play_sm);
    pio_sm_exec(play_pio, play_sm, pio_encode_jmp(play_offset));
    pio_sm_exec(play_pio, play_sm, pio_encode_set(pio_pins, 0));
    led(true);

    uint32_t head = 0, tail = 0;                       // Ringpuffer: Schreib-/Leseindex
    uint32_t count = 0, status = 0;
    uint32_t word = 0;
    int nbytes = 0;
    bool ended = false, running = false, abort_play = false;
    uint32_t last_ticks = 0;
    absolute_time_t last_rx = get_absolute_time();

    while (true) {
        if (!stdio_usb_connected()) { abort_play = true; break; }

        // Daten vom PC in den Ringpuffer (solange Platz ist, sonst blockiert der PC)
        for (int i = 0; i < 256 && !ended && head - tail < PLAY_RING_WORDS; i++) {
            int ch = getchar_timeout_us(0);
            if (ch < 0) break;
            last_rx = get_absolute_time();
            word |= (uint32_t)ch << (8 * nbytes);
            if (++nbytes < 4) continue;
            nbytes = 0;
            if (word == END_MARKER) {
                ended = true;
                if (count & 1u)                        // letzter Puls war High: Low anhaengen
                    play_ring[head++ % PLAY_RING_WORDS] = PLAY_IDLE_TICKS;
            } else {
                play_ring[head++ % PLAY_RING_WORDS] = word;
                count++;
            }
            word = 0;
        }

        if (!running && (ended || head - tail >= PLAY_PREFILL)) {
            while (tail != head && !pio_sm_is_tx_fifo_full(play_pio, play_sm)) {
                uint32_t t = play_ring[tail++ % PLAY_RING_WORDS];
                pio_sm_put(play_pio, play_sm, t > PLAY_OVERHEAD ? t - PLAY_OVERHEAD : 0);
                last_ticks = t;
            }
            pio_sm_set_enabled(play_pio, play_sm, true);
            running = true;
        }

        if (running) {
            while (tail != head && !pio_sm_is_tx_fifo_full(play_pio, play_sm)) {
                uint32_t t = play_ring[tail++ % PLAY_RING_WORDS];
                pio_sm_put(play_pio, play_sm, t > PLAY_OVERHEAD ? t - PLAY_OVERHEAD : 0);
                last_ticks = t;
            }
            if (tail == head) {
                if (ended) break;
                if (pio_sm_is_tx_fifo_empty(play_pio, play_sm))
                    status |= 1u;                      // Unterlauf: Daten kamen zu spaet
            }
        }

        if (!ended && absolute_time_diff_us(last_rx, get_absolute_time()) > PLAY_TIMEOUT_US) {
            abort_play = true;
            break;
        }
    }

    if (!abort_play) {
        while (!pio_sm_is_tx_fifo_empty(play_pio, play_sm)) tight_loop_contents();
        busy_wait_us((uint64_t)last_ticks * 1000000u / play_tick_hz + 1000);   // letzten Puls ausgeben
    } else {
        status |= 2u;
    }
    play_stop_low();

    const uint32_t trailer[3] = { END_MARKER, status, count };
    send(trailer, sizeof trailer);
    stdio_flush();
    led(false);
}

int main(void) {
    stdio_init_all();
#ifdef PICO_DEFAULT_LED_PIN
    gpio_init(PICO_DEFAULT_LED_PIN);
    gpio_set_dir(PICO_DEFAULT_LED_PIN, true);
  //  gpio_put(PICO_DEFAULT_LED_PIN, true);
#endif
    setup_pio();
    setup_play();

    while (true) {
        int ch = getchar_timeout_us(100000);
        if (ch == 'S')
            capture();
        else if (ch == 'P')
            play();
    }
}
