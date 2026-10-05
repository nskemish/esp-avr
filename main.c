/*
 * ============================================================
 * ATtiny85 ESP32 HARDWARE SUPERVISOR
 * ============================================================
 *
 * Purpose
 * -------
 *
 * This ATtiny85 is intentionally independent from the ESP32.
 *
 * It handles one physical button:
 *
 *   SHORT PRESS (< 10 s)
 *       -> reset ESP32 through EN
 *
 *   LONG PRESS (>= 10 s)
 *       -> assert a signal to an ESP32 GPIO
 *       -> do NOT reset ESP32
 *
 *
 * ------------------------------------------------------------
 * PIN ASSIGNMENT - ATtiny85 DIP-8
 * ------------------------------------------------------------
 *
 *                 ATtiny85
 *
 *                +---\/---+
 * RESET / PB5  1 |        | 8  VCC
 * BUTTON / PB3 2 |        | 7  PB2
 * AP_OUT / PB4 3 |        | 6  PB1 / ESP32_EN
 * GND          4 |        | 5  PB0
 *                +--------+
 *
 *
 * PB3 / pin 2
 *     Physical button.
 *
 *     Button should connect PB3 to GND when pressed.
 *
 *     Firmware enables the internal pull-up.
 *     An external ~10 kOhm pull-up to 3.3 V is recommended
 *     for maximum hardware robustness.
 *
 *
 * PB4 / pin 3
 *     Long-press output.
 *
 *     Connect to selected ESP32 GPIO.
 *
 *     LOW  = inactive
 *     HIGH = button has been held for >= 10 seconds
 *
 *     Recommended:
 *         47k - 100k external pull-down on this line.
 *
 *     Reason:
 *     During ATtiny reset all GPIOs become inputs / tri-state.
 *     The pull-down therefore guarantees that the ESP32 never
 *     accidentally sees a HIGH long-press signal.
 *
 *
 * PB1 / pin 6
 *     ESP32 EN reset control.
 *
 *     IMPORTANT:
 *
 *     This output is NOT driven HIGH.
 *
 *     It emulates an open-drain output:
 *
 *         DDRB.PB1 = 0
 *             -> input / Hi-Z
 *             -> ESP32 EN external pull-up keeps EN HIGH
 *
 *         DDRB.PB1 = 1
 *         PORTB.PB1 = 0
 *             -> output LOW
 *             -> ESP32 EN pulled to GND
 *             -> ESP32 resets
 *
 *
 * ------------------------------------------------------------
 * DATASHEET BASIS
 * ------------------------------------------------------------
 *
 * ATtiny25/45/85 Datasheet, section "I/O Ports":
 *
 *     DDxn = 0, PORTxn = 0
 *         -> Input, no pull-up, Tri-state (Hi-Z)
 *
 *     DDxn = 1, PORTxn = 0
 *         -> Output Low (Sink)
 *
 *
 * Timer0:
 *
 *     CTC mode uses OCR0A as TOP.
 *
 *     F_CPU = 1 MHz
 *     prescaler = 8
 *
 *     timer clock:
 *
 *         1,000,000 / 8 = 125,000 Hz
 *
 *     one timer count:
 *
 *         8 us
 *
 *     OCR0A = 124:
 *
 *         125 counts * 8 us = 1000 us = 1 ms
 *
 *
 * Watchdog:
 *
 *     The ATtiny85 watchdog has an independent watchdog
 *     oscillator.
 *
 *     If main code ever stops servicing the watchdog,
 *     the watchdog resets the ATtiny85.
 *
 *
 * ============================================================
 */

#include <avr/io.h>
#include <avr/interrupt.h>
#include <avr/wdt.h>
#include <util/atomic.h>

#include <stdbool.h>
#include <stdint.h>


// ============================================================
// Compile-time requirements
// ============================================================

#ifndef F_CPU
#define F_CPU 1000000UL
#endif

#if F_CPU != 1000000UL
#error "This firmware currently requires F_CPU = 1 MHz"
#endif


// ============================================================
// Pin definitions
// ============================================================

#define PIN_BUTTON          PB3
#define PIN_LONG_PRESS_OUT  PB4
#define PIN_ESP32_EN        PB1


// ============================================================
// Timing
// ============================================================

// A press becomes a LONG press at this point.
#define LONG_PRESS_TIME_MS      10000UL

// ESP32 EN LOW duration during short-press reset.
#define ESP32_RESET_TIME_MS       200UL

// Stable input time required before accepting a transition.
#define DEBOUNCE_TIME_MS           30UL

// Watchdog timeout.
//
// Main loop executes much faster than this.
// If firmware hangs, ATtiny will restart automatically.
#define WATCHDOG_TIMEOUT WDTO_1S


// ============================================================
// Global 1 ms system tick
// ============================================================

static volatile uint32_t system_ms = 0;


// ============================================================
// State machine
// ============================================================

typedef enum
{
    STATE_IDLE = 0,

    // Button is currently held.
    STATE_PRESSED,

    // 10 second threshold has been reached.
    STATE_LONG_PRESS

} button_state_t;


// ============================================================
// Timer0 ISR
// ============================================================
//
// Hardware timer generates one interrupt every 1 ms.
//
// Keep this ISR intentionally tiny.
//
// No button processing.
// No GPIO processing.
// No watchdog processing.
//
// It only provides a monotonic millisecond counter.
//
// ============================================================

ISR(TIMER0_COMPA_vect)
{
    system_ms++;
}


// ============================================================
// Time helpers
// ============================================================

static uint32_t millis_get(void)
{
    uint32_t value;

    /*
     * ATtiny85 is an 8-bit CPU.
     *
     * Reading a uint32_t therefore takes multiple instructions.
     * Timer interrupt could otherwise occur halfway through
     * the read and produce a corrupted timestamp.
     *
     * ATOMIC_BLOCK temporarily prevents that.
     */

    ATOMIC_BLOCK(ATOMIC_RESTORESTATE)
    {
        value = system_ms;
    }

    return value;
}


// ============================================================
// ESP32 EN control
// ============================================================
//
// ATtiny85 does not need a dedicated open-drain peripheral.
//
// Datasheet Port configuration:
//
//     DDR = 0
//     PORT = 0
//         -> input / tri-state / Hi-Z
//
//     DDR = 1
//     PORT = 0
//         -> output LOW / sink
//
// PORT latch is therefore permanently kept at zero.
// Only DDR changes.
//
// ============================================================

static inline void esp32_en_release(void)
{
    /*
     * PB1 becomes INPUT / Hi-Z.
     *
     * External ESP32 EN pull-up then pulls EN HIGH.
     */

    DDRB &= ~(1 << PIN_ESP32_EN);
}


static inline void esp32_en_assert_reset(void)
{
    /*
     * PORT latch MUST already be zero.
     *
     * Changing DDR to output therefore immediately creates:
     *
     *     OUTPUT LOW
     *
     * and pulls ESP32 EN to ground.
     */

    DDRB |= (1 << PIN_ESP32_EN);
}


// ============================================================
// Long press output
// ============================================================

static inline void long_press_output_low(void)
{
    PORTB &= ~(1 << PIN_LONG_PRESS_OUT);
}


static inline void long_press_output_high(void)
{
    PORTB |= (1 << PIN_LONG_PRESS_OUT);
}


// ============================================================
// Button raw read
// ============================================================

static inline bool button_raw_pressed(void)
{
    /*
     * Button connects PB3 to GND.
     *
     * Therefore:
     *
     *     0 -> pressed
     *     1 -> released
     */

    return
        (PINB & (1 << PIN_BUTTON)) == 0;
}


// ============================================================
// ESP32 reset pulse
// ============================================================

static void esp32_reset_pulse(void)
{
    /*
     * Only this function is allowed to intentionally pull
     * ESP32 EN LOW.
     *
     * The watchdog continues to be serviced during the pulse.
     *
     * Even if some future modification caused execution to
     * become stuck here, watchdog reset would reset ATtiny.
     *
     * During ATtiny reset its pins are tri-stated, therefore
     * ESP32 EN would again be released by hardware.
     */

    uint32_t start = millis_get();

    esp32_en_assert_reset();

    while (
        (uint32_t)(millis_get() - start)
        < ESP32_RESET_TIME_MS
    )
    {
        wdt_reset();
    }

    /*
     * ALWAYS release EN after pulse.
     */

    esp32_en_release();
}


// ============================================================
// Timer initialization
// ============================================================

static void timer0_init(void)
{
    /*
     * Timer0 CTC mode.
     *
     * F_CPU = 1 MHz
     *
     * prescaler /8:
     *
     *     1 MHz / 8 = 125 kHz
     *
     * OCR0A = 124:
     *
     *     125 timer clocks = 1 ms
     */

    TCCR0A = 0;
    TCCR0B = 0;

    TCNT0 = 0;

    OCR0A = 124;

    /*
     * WGM01 = 1
     * WGM00 = 0
     * WGM02 = 0
     *
     * -> CTC mode
     */

    TCCR0A =
        (1 << WGM01);

    /*
     * CS02:0 = 010
     *
     * -> clock / 8
     */

    TCCR0B =
        (1 << CS01);

    /*
     * Enable Output Compare Match A interrupt.
     */

    TIMSK |=
        (1 << OCIE0A);
}


// ============================================================
// Watchdog initialization
// ============================================================

static void watchdog_init(void)
{
    /*
     * If the MCU booted because of a previous watchdog reset,
     * WDRF may still be set.
     *
     * Clear it before configuring the watchdog again.
     */

    MCUSR &= ~(1 << WDRF);

    wdt_reset();

    /*
     * avr-libc performs the timed WDT configuration sequence
     * required by the AVR hardware.
     */

    wdt_enable(
        WATCHDOG_TIMEOUT
    );
}


// ============================================================
// Hardware-safe initialization
// ============================================================

static void hardware_init(void)
{
    /*
     * --------------------------------------------------------
     * FIRST PRIORITY:
     * ESP32 EN must be safe.
     * --------------------------------------------------------
     *
     * PORTB1 = 0 first.
     *
     * Then PB1 remains input / Hi-Z.
     */

    PORTB &=
        ~(1 << PIN_ESP32_EN);

    DDRB &=
        ~(1 << PIN_ESP32_EN);


    /*
     * --------------------------------------------------------
     * Long press output
     * --------------------------------------------------------
     *
     * Prepare LOW before enabling output driver.
     *
     * This prevents an unwanted HIGH glitch.
     */

    PORTB &=
        ~(1 << PIN_LONG_PRESS_OUT);

    DDRB |=
        (1 << PIN_LONG_PRESS_OUT);


    /*
     * --------------------------------------------------------
     * Button
     * --------------------------------------------------------
     *
     * Input.
     */

    DDRB &=
        ~(1 << PIN_BUTTON);

    /*
     * Internal pull-up ON.
     */

    PORTB |=
        (1 << PIN_BUTTON);


    /*
     * Ensure final known state.
     */

    esp32_en_release();

    long_press_output_low();
}


// ============================================================
// Main
// ============================================================

int main(void)
{
    /*
     * Interrupts are disabled automatically after AVR reset.
     *
     * Configure critical GPIO first.
     */

    hardware_init();

    /*
     * Initialize watchdog BEFORE entering normal operation.
     */

    watchdog_init();

    /*
     * Configure the hardware timebase.
     */

    timer0_init();

    /*
     * Start interrupts.
     */

    sei();


    // ========================================================
    // Button debounce state
    // ========================================================

    bool raw_previous =
        button_raw_pressed();

    bool button_stable =
        raw_previous;

    uint32_t raw_changed_at =
        millis_get();


    // ========================================================
    // Functional state
    // ========================================================

    button_state_t state =
        STATE_IDLE;

    uint32_t press_started_at =
        0;


    // ========================================================
    // Main supervisor loop
    // ========================================================

    for (;;)
    {
        /*
         * ----------------------------------------------------
         * WATCHDOG HEARTBEAT
         * ----------------------------------------------------
         *
         * If main loop stops reaching this instruction,
         * watchdog will reset the ATtiny.
         */

        wdt_reset();


        uint32_t now =
            millis_get();


        // ====================================================
        // RAW BUTTON + DEBOUNCE
        // ====================================================

        bool raw =
            button_raw_pressed();


        /*
         * Raw signal changed.
         *
         * Start a new debounce interval.
         */

        if (raw != raw_previous)
        {
            raw_previous =
                raw;

            raw_changed_at =
                now;
        }


        /*
         * Has raw input remained unchanged for at least
         * DEBOUNCE_TIME_MS?
         */

        if (
            raw != button_stable &&
            (uint32_t)(now - raw_changed_at)
                >= DEBOUNCE_TIME_MS
        )
        {
            /*
             * Accept new debounced button state.
             */

            button_stable =
                raw;


            // ================================================
            // BUTTON PRESSED
            // ================================================

            if (button_stable)
            {
                /*
                 * Begin timing.
                 */

                press_started_at =
                    now;

                state =
                    STATE_PRESSED;

                /*
                 * Always ensure long-press output is LOW
                 * at beginning of a new press.
                 */

                long_press_output_low();

                /*
                 * EN remains released.
                 *
                 * Merely pressing the button must never
                 * immediately reset the ESP32.
                 */

                esp32_en_release();
            }


            // ================================================
            // BUTTON RELEASED
            // ================================================

            else
            {
                /*
                 * What happens now depends completely on
                 * whether the 10 second threshold was reached.
                 */

                if (state == STATE_PRESSED)
                {
                    /*
                     * Button was released BEFORE long threshold.
                     *
                     * SHORT PRESS.
                     *
                     * Generate bounded ESP32 reset pulse.
                     */

                    long_press_output_low();

                    esp32_reset_pulse();
                }

                else if (state == STATE_LONG_PRESS)
                {
                    /*
                     * Long press already occurred.
                     *
                     * Release GPIO signal.
                     *
                     * IMPORTANT:
                     * NO ESP32 reset here.
                     */

                    long_press_output_low();

                    esp32_en_release();
                }


                /*
                 * Return to idle regardless of previous state.
                 */

                state =
                    STATE_IDLE;
            }
        }


        // ====================================================
        // LONG PRESS DETECTION
        // ====================================================

        if (
            state == STATE_PRESSED &&
            button_stable
        )
        {
            uint32_t held_time =
                (uint32_t)(
                    now -
                    press_started_at
                );


            if (
                held_time >=
                LONG_PRESS_TIME_MS
            )
            {
                /*
                 * Threshold reached.
                 *
                 * Permanently classify this physical press
                 * as LONG.
                 *
                 * From this point forward release can NEVER
                 * generate a short-press reset.
                 */

                state =
                    STATE_LONG_PRESS;


                /*
                 * Tell ESP32 that long press occurred.
                 */

                long_press_output_high();


                /*
                 * Explicitly ensure EN is released.
                 */

                esp32_en_release();
            }
        }


        // ====================================================
        // Defensive invariant
        // ====================================================
        //
        // Outside the actual reset pulse, ESP32 EN should
        // always be Hi-Z.
        //
        // Keeping this statement here makes the safety
        // requirement explicit even if code is extended later.
        //

        esp32_en_release();
    }
}
