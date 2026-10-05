#include "avr_isp.h"

#include "driver/gpio.h"
#include "esp_rom_sys.h"

// ============================================================
// ESP32-WROOM-32 -> ATtiny85 ISP
// ============================================================

#define AVR_MOSI    GPIO_NUM_23
#define AVR_MISO    GPIO_NUM_19
#define AVR_SCK     GPIO_NUM_18
#define AVR_RESET   GPIO_NUM_22

// ============================================================
// ATtiny85 parameters
// ============================================================

#define ATTINY85_FLASH_SIZE_BYTES   8192
#define ATTINY85_PAGE_SIZE_WORDS    32
#define ATTINY85_PAGE_SIZE_BYTES    64

#define ATTINY85_SIGNATURE_0        0x1E
#define ATTINY85_SIGNATURE_1        0x93
#define ATTINY85_SIGNATURE_2        0x0B

// ============================================================
// ISP timing
// ============================================================

// 5 us HIGH + 5 us LOW ~= 100 kHz SCK.
//
// Fabrički ATtiny85 ima dovoljno spor clock da ovo bude
// vrlo konzervativna ISP brzina.
#define ISP_HALF_PERIOD_US          5

#define ISP_ENABLE_DELAY_US         25000
#define ISP_RESET_PULSE_US          2000

#define ISP_SYNC_ATTEMPTS           5

#define ISP_READY_POLL_INTERVAL_US  100
#define ISP_READY_TIMEOUT_US        100000

// ============================================================
// Low-level SPI mode 0 bit-bang
// ============================================================

static inline void isp_delay(void)
{
    esp_rom_delay_us(ISP_HALF_PERIOD_US);
}

static uint8_t isp_transfer(uint8_t tx)
{
    uint8_t rx = 0;

    for (int bit = 7; bit >= 0; --bit) {

        // MOSI setup while SCK LOW.
        gpio_set_level(
            AVR_MOSI,
            (tx >> bit) & 1
        );

        isp_delay();

        // Rising edge:
        // ATtiny samples MOSI here.
        gpio_set_level(AVR_SCK, 1);

        isp_delay();

        // MISO is stable here.
        rx <<= 1;

        if (gpio_get_level(AVR_MISO)) {
            rx |= 1;
        }

        // Falling edge:
        // ATtiny prepares/shifts next MISO bit.
        gpio_set_level(AVR_SCK, 0);

        isp_delay();
    }

    return rx;
}

static uint8_t isp_command(
    uint8_t b0,
    uint8_t b1,
    uint8_t b2,
    uint8_t b3
)
{
    isp_transfer(b0);
    isp_transfer(b1);
    isp_transfer(b2);

    return isp_transfer(b3);
}

// ============================================================
// RDY / BSY
// ============================================================

static bool isp_wait_ready(void)
{
    uint32_t elapsed = 0;

    while (elapsed < ISP_READY_TIMEOUT_US) {

        uint8_t status =
            isp_command(
                0xF0,
                0x00,
                0x00,
                0x00
            );

        // bit 0:
        // 0 = ready
        // 1 = busy
        if ((status & 0x01) == 0) {
            return true;
        }

        esp_rom_delay_us(
            ISP_READY_POLL_INTERVAL_US
        );

        elapsed +=
            ISP_READY_POLL_INTERVAL_US;
    }

    return false;
}

// ============================================================
// Init
// ============================================================

void avr_isp_init(void)
{
    gpio_config_t outputs = {
        .pin_bit_mask =
            (1ULL << AVR_MOSI) |
            (1ULL << AVR_SCK) |
            (1ULL << AVR_RESET),

        .mode = GPIO_MODE_OUTPUT,

        .pull_up_en =
            GPIO_PULLUP_DISABLE,

        .pull_down_en =
            GPIO_PULLDOWN_DISABLE,

        .intr_type =
            GPIO_INTR_DISABLE
    };

    gpio_config(&outputs);

    gpio_config_t input = {
        .pin_bit_mask =
            (1ULL << AVR_MISO),

        .mode =
            GPIO_MODE_INPUT,

        .pull_up_en =
            GPIO_PULLUP_DISABLE,

        .pull_down_en =
            GPIO_PULLDOWN_DISABLE,

        .intr_type =
            GPIO_INTR_DISABLE
    };

    gpio_config(&input);

    gpio_set_level(AVR_SCK, 0);
    gpio_set_level(AVR_MOSI, 0);

    // ATtiny released initially.
    gpio_set_level(AVR_RESET, 1);

    esp_rom_delay_us(5000);
}

// ============================================================
// Enter / leave programming mode
// ============================================================

bool avr_isp_enter(void)
{
    gpio_set_level(AVR_SCK, 0);
    gpio_set_level(AVR_MOSI, 0);

    for (
        int attempt = 0;
        attempt < ISP_SYNC_ATTEMPTS;
        ++attempt
    ) {

        // Positive RESET pulse after SCK is LOW.
        gpio_set_level(AVR_RESET, 1);

        esp_rom_delay_us(
            ISP_RESET_PULSE_US
        );

        // Enter reset.
        gpio_set_level(AVR_RESET, 0);

        // Datasheet requires >=20ms.
        esp_rom_delay_us(
            ISP_ENABLE_DELAY_US
        );

        // Programming Enable:
        //
        // AC 53 00 00
        //
        // 0x53 must be echoed while third byte
        // is transmitted.

        isp_transfer(0xAC);
        isp_transfer(0x53);

        uint8_t echo =
            isp_transfer(0x00);

        isp_transfer(0x00);

        if (echo == 0x53) {
            return true;
        }
    }

    return false;
}

void avr_isp_leave(void)
{
    gpio_set_level(AVR_SCK, 0);
    gpio_set_level(AVR_MOSI, 0);

    // Release reset -> application starts.
    gpio_set_level(AVR_RESET, 1);

    esp_rom_delay_us(1000);
}

// ============================================================
// Identification
// ============================================================

avr_signature_t avr_isp_read_signature(void)
{
    avr_signature_t sig;

    sig.b0 =
        isp_command(
            0x30,
            0x00,
            0x00,
            0x00
        );

    sig.b1 =
        isp_command(
            0x30,
            0x00,
            0x01,
            0x00
        );

    sig.b2 =
        isp_command(
            0x30,
            0x00,
            0x02,
            0x00
        );

    return sig;
}

avr_fuses_t avr_isp_read_fuses(void)
{
    avr_fuses_t fuses;

    fuses.low =
        isp_command(
            0x50,
            0x00,
            0x00,
            0x00
        );

    fuses.high =
        isp_command(
            0x58,
            0x08,
            0x00,
            0x00
        );

    fuses.extended =
        isp_command(
            0x50,
            0x08,
            0x00,
            0x00
        );

    fuses.lock =
        isp_command(
            0x58,
            0x00,
            0x00,
            0x00
        );

    return fuses;
}

// ============================================================
// Chip erase
// ============================================================

bool avr_isp_chip_erase(void)
{
    isp_command(
        0xAC,
        0x80,
        0x00,
        0x00
    );

    return isp_wait_ready();
}

// ============================================================
// Flash read
// ============================================================

static uint8_t flash_read_byte(
    uint16_t byte_address
)
{
    uint16_t word_address =
        byte_address >> 1;

    uint8_t command =
        (byte_address & 1)
            ? 0x28
            : 0x20;

    return isp_command(
        command,
        (uint8_t)(word_address >> 8),
        (uint8_t)(word_address & 0xFF),
        0x00
    );
}

// ============================================================
// Flash programming
// ============================================================

static bool flash_program_page(
    const uint8_t *data,
    size_t data_size,
    uint16_t byte_base
)
{
    uint16_t word_base =
        byte_base >> 1;

    for (
        uint8_t word_offset = 0;
        word_offset < ATTINY85_PAGE_SIZE_WORDS;
        ++word_offset
    ) {
        size_t low_index =
            (size_t)byte_base +
            ((size_t)word_offset * 2);

        size_t high_index =
            low_index + 1;

        uint8_t low =
            low_index < data_size
                ? data[low_index]
                : 0xFF;

        uint8_t high =
            high_index < data_size
                ? data[high_index]
                : 0xFF;

        // Datasheet requires LOW byte to be
        // loaded before HIGH byte for each word.

        isp_command(
            0x40,
            0x00,
            word_offset,
            low
        );

        isp_command(
            0x48,
            0x00,
            word_offset,
            high
        );
    }

    // Commit page.
    isp_command(
        0x4C,
        (uint8_t)(word_base >> 8),
        (uint8_t)(word_base & 0xFF),
        0x00
    );

    return isp_wait_ready();
}

bool avr_isp_program_flash(
    const uint8_t *data,
    size_t size
)
{
    if (
        data == NULL ||
        size == 0 ||
        size > ATTINY85_FLASH_SIZE_BYTES
    ) {
        return false;
    }

    size_t pages =
        (
            size +
            ATTINY85_PAGE_SIZE_BYTES -
            1
        ) /
        ATTINY85_PAGE_SIZE_BYTES;

    for (
        size_t page = 0;
        page < pages;
        ++page
    ) {
        uint16_t byte_base =
            (uint16_t)(
                page *
                ATTINY85_PAGE_SIZE_BYTES
            );

        if (!flash_program_page(
                data,
                size,
                byte_base
            )) {
            return false;
        }
    }

    return true;
}

// ============================================================
// Verify
// ============================================================

bool avr_isp_verify_flash(
    const uint8_t *data,
    size_t size,
    size_t *bad_address,
    uint8_t *expected,
    uint8_t *actual
)
{
    if (
        data == NULL ||
        size == 0 ||
        size > ATTINY85_FLASH_SIZE_BYTES
    ) {
        return false;
    }

    for (
        size_t address = 0;
        address < size;
        ++address
    ) {
        uint8_t read =
            flash_read_byte(
                (uint16_t)address
            );

        if (read != data[address]) {

            if (bad_address) {
                *bad_address = address;
            }

            if (expected) {
                *expected = data[address];
            }

            if (actual) {
                *actual = read;
            }

            return false;
        }
    }

    return true;
}

// ============================================================
// Complete ATtiny85 programming operation
// ============================================================

avr_isp_result_t avr_isp_program_attiny85(
    const uint8_t *data,
    size_t size
)
{
    if (
        data == NULL ||
        size == 0 ||
        size > ATTINY85_FLASH_SIZE_BYTES
    ) {
        return AVR_ISP_ERROR_FIRMWARE_SIZE;
    }

    if (!avr_isp_enter()) {
        avr_isp_leave();
        return AVR_ISP_ERROR_SYNC;
    }

    avr_signature_t sig =
        avr_isp_read_signature();

    // Absolutely do not erase an unknown device.
    if (
        sig.b0 != ATTINY85_SIGNATURE_0 ||
        sig.b1 != ATTINY85_SIGNATURE_1 ||
        sig.b2 != ATTINY85_SIGNATURE_2
    ) {
        avr_isp_leave();
        return AVR_ISP_ERROR_SIGNATURE;
    }

    // Device identity is confirmed.
    // It is now safe to erase.
    if (!avr_isp_chip_erase()) {
        avr_isp_leave();
        return AVR_ISP_ERROR_ERASE_TIMEOUT;
    }

    if (!avr_isp_program_flash(
            data,
            size
        )) {
        avr_isp_leave();
        return AVR_ISP_ERROR_PROGRAM_TIMEOUT;
    }

    size_t bad_address = 0;
    uint8_t expected = 0;
    uint8_t actual = 0;

    if (!avr_isp_verify_flash(
            data,
            size,
            &bad_address,
            &expected,
            &actual
        )) {

        (void)bad_address;
        (void)expected;
        (void)actual;

        avr_isp_leave();

        return AVR_ISP_ERROR_VERIFY;
    }

    avr_isp_leave();

    return AVR_ISP_OK;
}

const char *avr_isp_result_string(
    avr_isp_result_t result
)
{
    switch (result) {

        case AVR_ISP_OK:
            return "OK";

        case AVR_ISP_ERROR_FIRMWARE_SIZE:
            return "INVALID FIRMWARE SIZE";

        case AVR_ISP_ERROR_SYNC:
            return "ISP SYNC FAILED";

        case AVR_ISP_ERROR_SIGNATURE:
            return "NOT AN ATTINY85";

        case AVR_ISP_ERROR_ERASE_TIMEOUT:
            return "CHIP ERASE TIMEOUT";

        case AVR_ISP_ERROR_PROGRAM_TIMEOUT:
            return "FLASH PROGRAM TIMEOUT";

        case AVR_ISP_ERROR_VERIFY:
            return "VERIFY FAILED";

        default:
            return "UNKNOWN ERROR";
    }
}
