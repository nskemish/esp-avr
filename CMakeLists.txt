# The following lines of boilerplate have to be in your project's
# CMakeLists in this exact order for cmake to work correctly
cmake_minimum_required(VERSION 3.5)

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(app-template)


# ============================================================

# ATtiny85 firmware

# ============================================================

set(AVR_GCC

    "/opt/homebrew/bin/avr-gcc"

)

set(AVR_OBJCOPY

    "/opt/homebrew/bin/avr-objcopy"

)

set(AVR_SIZE

    "/opt/homebrew/bin/avr-size"

)

set(AVR_SOURCE

    "${CMAKE_SOURCE_DIR}/avr/main.c"

)

set(AVR_BUILD_DIR

    "${CMAKE_BINARY_DIR}/avr"

)

set(AVR_ELF

    "${AVR_BUILD_DIR}/attiny85.elf"

)

set(AVR_BIN

    "${AVR_BUILD_DIR}/attiny85.bin"

)

# ============================================================

# Compile + link ATtiny85

# ============================================================

add_custom_command(

    OUTPUT

        "${AVR_BIN}"

    COMMAND

        ${CMAKE_COMMAND} -E make_directory

        "${AVR_BUILD_DIR}"

    COMMAND

        "${AVR_GCC}"

        -mmcu=attiny85

        -DF_CPU=1000000UL

        -Os

        -ffunction-sections

        -fdata-sections

        -Wall

        -Wextra

        -Wl,--gc-sections

        "${AVR_SOURCE}"

        -o

        "${AVR_ELF}"

    COMMAND

        "${AVR_OBJCOPY}"

        -O binary

        -R .eeprom

        "${AVR_ELF}"

        "${AVR_BIN}"

    COMMAND

        "${AVR_SIZE}"

        --mcu=attiny85

        --format=avr

        "${AVR_ELF}"

    DEPENDS

        "${AVR_SOURCE}"

    COMMENT

        "Building ATtiny85 firmware"

    VERBATIM

)

# ============================================================

# AVR build target

# ============================================================

add_custom_target(

    attiny85_firmware

    DEPENDS "${AVR_BIN}"

)

# ============================================================

# Embed ATtiny85 binary into ESP32 firmware

# ============================================================

target_add_binary_data(

    ${PROJECT_NAME}.elf

    "${AVR_BIN}"

    BINARY

    DEPENDS attiny85_firmware

)