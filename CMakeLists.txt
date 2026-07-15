cmake_minimum_required(VERSION 3.13)

include(pico_sdk_import.cmake)

# Build for the smallest supported flash so one firmware image works on both
# development Pico boards and the 4 Mbit production hardware.
set(DOCKIR_FLASH_SIZE_BYTES "524288" CACHE STRING
        "Logical flash size for the shared DockIR firmware image")
set(PICO_FLASH_SIZE_BYTES "${DOCKIR_FLASH_SIZE_BYTES}" CACHE STRING
        "Flash size used by the Pico SDK linker" FORCE)

project(pico_ir_keyboard C CXX ASM)
set(CMAKE_C_STANDARD 11)
set(CMAKE_CXX_STANDARD 17)

pico_sdk_init()

add_compile_options(-Wall
        -Wno-format          # int != int32_t as far as the compiler is concerned because gcc has int32_t as long int
        -Wno-unused-function # we have some for the docs that aren't called
        -Wno-maybe-uninitialized
        )

add_subdirectory(nec_receive_library)

add_executable(pico_ir_keyboard)

target_sources(pico_ir_keyboard PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}/src/main.c
        ${CMAKE_CURRENT_SOURCE_DIR}/src/usb_descriptors.c
        )

target_include_directories(pico_ir_keyboard PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}/src
        )

target_compile_definitions(pico_ir_keyboard PUBLIC
        DOCKIR_FLASH_SIZE_BYTES=${DOCKIR_FLASH_SIZE_BYTES}
        DOCKIR_LOG_UART_BAUD=115200
        DOCKIR_LOG_UART_TX_PIN=0
        DOCKIR_LOG_UART_RX_PIN=1
        PICO_DEFAULT_UART_BAUD_RATE=115200
        PICO_DEFAULT_UART_TX_PIN=0
        PICO_DEFAULT_UART_RX_PIN=1
        PICO_FLASH_SIZE_BYTES=${DOCKIR_FLASH_SIZE_BYTES}
        )

target_link_libraries(pico_ir_keyboard PUBLIC
        pico_stdlib
        hardware_flash
        hardware_pio
        hardware_sync
        pico_unique_id
        nec_receive_library
        )

pico_enable_stdio_uart(pico_ir_keyboard 1)
pico_enable_stdio_usb(pico_ir_keyboard 0)
pico_add_extra_outputs(pico_ir_keyboard)

family_configure_device_example(pico_ir_keyboard noos)
