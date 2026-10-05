#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    uint8_t b0;
    uint8_t b1;
    uint8_t b2;
} avr_signature_t;

typedef struct {
    uint8_t low;
    uint8_t high;
    uint8_t extended;
    uint8_t lock;
} avr_fuses_t;

typedef enum {
    AVR_ISP_OK = 0,

    AVR_ISP_ERROR_FIRMWARE_SIZE,
    AVR_ISP_ERROR_SYNC,
    AVR_ISP_ERROR_SIGNATURE,
    AVR_ISP_ERROR_ERASE_TIMEOUT,
    AVR_ISP_ERROR_PROGRAM_TIMEOUT,
    AVR_ISP_ERROR_VERIFY
} avr_isp_result_t;


void avr_isp_init(void);

bool avr_isp_enter(void);
void avr_isp_leave(void);

avr_signature_t avr_isp_read_signature(void);
avr_fuses_t avr_isp_read_fuses(void);

bool avr_isp_chip_erase(void);

bool avr_isp_program_flash(
    const uint8_t *firmware,
    size_t firmware_size
);

bool avr_isp_verify_flash(
    const uint8_t *firmware,
    size_t firmware_size,
    size_t *bad_address,
    uint8_t *expected,
    uint8_t *actual
);

avr_isp_result_t avr_isp_program_attiny85(
    const uint8_t *firmware,
    size_t firmware_size
);

const char *avr_isp_result_string(
    avr_isp_result_t result
);

#ifdef __cplusplus
}
#endif