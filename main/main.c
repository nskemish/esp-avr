#include <stdio.h>
#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "driver/gpio.h"

#include "avr_isp.h"

// ============================================================
// User interface
// ============================================================

#define PROGRAM_BUTTON GPIO_NUM_15
#define STATUS_LED     GPIO_NUM_25

// ============================================================
// Embedded ATtiny85 firmware
// ============================================================

extern const uint8_t attiny85_bin_start[]
    asm("_binary_attiny85_bin_start");

extern const uint8_t attiny85_bin_end[]
    asm("_binary_attiny85_bin_end");

// ============================================================
// LED helpers
// ============================================================

static void led_set(bool state)
{
    gpio_set_level(
        STATUS_LED,
        state ? 1 : 0
    );
}

static void led_blink(
    int count,
    int on_ms,
    int off_ms
)
{
    for (int i = 0; i < count; ++i) {

        led_set(true);

        vTaskDelay(
            pdMS_TO_TICKS(on_ms)
        );

        led_set(false);

        if (i != count - 1) {
            vTaskDelay(
                pdMS_TO_TICKS(off_ms)
            );
        }
    }
}

static void indicate_success(void)
{
    // Three deliberate slow flashes.
    led_blink(
        3,
        300,
        250
    );

    vTaskDelay(
        pdMS_TO_TICKS(500)
    );

    // Solid ON briefly.
    led_set(true);

    vTaskDelay(
        pdMS_TO_TICKS(1500)
    );

    led_set(false);
}

static void indicate_error(void)
{
    // Fast error pattern.
    led_blink(
        8,
        80,
        80
    );

    led_set(false);
}

// ============================================================
// GPIO
// ============================================================

static void ui_init(void)
{
    gpio_config_t led = {
        .pin_bit_mask =
            1ULL << STATUS_LED,

        .mode =
            GPIO_MODE_OUTPUT,

        .pull_up_en =
            GPIO_PULLUP_DISABLE,

        .pull_down_en =
            GPIO_PULLDOWN_DISABLE,

        .intr_type =
            GPIO_INTR_DISABLE
    };

    gpio_config(&led);

    gpio_config_t button = {
        .pin_bit_mask =
            1ULL << PROGRAM_BUTTON,

        .mode =
            GPIO_MODE_INPUT,

        .pull_up_en =
            GPIO_PULLUP_ENABLE,

        .pull_down_en =
            GPIO_PULLDOWN_DISABLE,

        .intr_type =
            GPIO_INTR_DISABLE
    };

    gpio_config(&button);

    led_set(false);
}

static bool button_pressed(void)
{
    return
        gpio_get_level(
            PROGRAM_BUTTON
        ) == 0;
}

static bool wait_for_button_press(void)
{
    if (!button_pressed()) {
        return false;
    }

    // Debounce.
    vTaskDelay(
        pdMS_TO_TICKS(30)
    );

    return button_pressed();
}

static void wait_for_button_release(void)
{
    while (button_pressed()) {
        vTaskDelay(
            pdMS_TO_TICKS(10)
        );
    }

    // Release debounce.
    vTaskDelay(
        pdMS_TO_TICKS(30)
    );
}

// ============================================================
// Programming operation
// ============================================================

static avr_isp_result_t program_target(void)
{
    size_t firmware_size =
        (size_t)(
            attiny85_bin_end -
            attiny85_bin_start
        );

    printf("\n");
    printf("========================================\n");
    printf(" ATtiny85 STANDALONE PROGRAMMER\n");
    printf("========================================\n");

    printf(
        "Firmware size: %u bytes\n",
        (unsigned)firmware_size
    );

    if (
        firmware_size == 0 ||
        firmware_size > 8192
    ) {
        printf(
            "ERROR: Invalid firmware size.\n"
        );

        return AVR_ISP_ERROR_FIRMWARE_SIZE;
    }

    printf("Entering ISP...\n");

    if (!avr_isp_enter()) {

        avr_isp_leave();

        printf(
            "ERROR: Unable to enter ISP.\n"
        );

        return AVR_ISP_ERROR_SYNC;
    }

    avr_signature_t signature =
        avr_isp_read_signature();

    printf(
        "Signature: %02X %02X %02X\n",
        signature.b0,
        signature.b1,
        signature.b2
    );

    if (
        signature.b0 != 0x1E ||
        signature.b1 != 0x93 ||
        signature.b2 != 0x0B
    ) {
        printf(
            "ERROR: Target is not ATtiny85.\n"
        );

        printf(
            "Flash will NOT be erased.\n"
        );

        avr_isp_leave();

        return AVR_ISP_ERROR_SIGNATURE;
    }

    printf("ATtiny85 detected.\n");

    avr_fuses_t fuses =
        avr_isp_read_fuses();

    printf(
        "Fuses: L=%02X H=%02X E=%02X LOCK=%02X\n",
        fuses.low,
        fuses.high,
        fuses.extended,
        fuses.lock
    );

    printf("Chip erase...\n");

    if (!avr_isp_chip_erase()) {

        printf(
            "ERROR: Chip erase timeout.\n"
        );

        avr_isp_leave();

        return AVR_ISP_ERROR_ERASE_TIMEOUT;
    }

    printf("Erase OK.\n");
    printf("Programming Flash...\n");

    if (!avr_isp_program_flash(
            attiny85_bin_start,
            firmware_size
        )) {

        printf(
            "ERROR: Flash programming timeout.\n"
        );

        avr_isp_leave();

        return AVR_ISP_ERROR_PROGRAM_TIMEOUT;
    }

    printf("Programming OK.\n");
    printf("Verifying...\n");

    size_t bad_address = 0;
    uint8_t expected = 0;
    uint8_t actual = 0;

    if (!avr_isp_verify_flash(
            attiny85_bin_start,
            firmware_size,
            &bad_address,
            &expected,
            &actual
        )) {

        printf(
            "VERIFY ERROR @ 0x%04X: "
            "expected %02X, read %02X\n",
            (unsigned)bad_address,
            expected,
            actual
        );

        avr_isp_leave();

        return AVR_ISP_ERROR_VERIFY;
    }

    printf("Verify OK.\n");

    avr_isp_leave();

    printf("Programming complete.\n");

    return AVR_ISP_OK;
}

// ============================================================
// main
// ============================================================

void app_main(void)
{
    ui_init();

    avr_isp_init();

    size_t firmware_size =
        (size_t)(
            attiny85_bin_end -
            attiny85_bin_start
        );

    printf("\n");
    printf("ATtiny85 standalone programmer ready.\n");

    printf(
        "Embedded firmware: %u bytes\n",
        (unsigned)firmware_size
    );

    printf(
        "Insert ATtiny85 and press PROGRAM.\n"
    );

    while (1) {

        if (!wait_for_button_press()) {

            vTaskDelay(
                pdMS_TO_TICKS(20)
            );

            continue;
        }

        wait_for_button_release();

        printf("\nPROGRAM pressed.\n");

        // LED solid while actual operation is running.
        led_set(true);

        avr_isp_result_t result =
            program_target();

        led_set(false);

        printf(
            "Result: %s\n",
            avr_isp_result_string(result)
        );

        if (result == AVR_ISP_OK) {
            indicate_success();
        }
        else {
            indicate_error();
        }

        printf(
            "\nReady for next ATtiny85.\n"
        );
    }
}
