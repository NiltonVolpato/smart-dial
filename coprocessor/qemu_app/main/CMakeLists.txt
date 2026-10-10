# Copyright © 2026 Nilton Volpato
# SPDX-License-Identifier: MIT

# The app's sources, as on the device, but for its main (Wi-Fi) and BLE.
file(GLOB_RECURSE app_sources "${CMAKE_CURRENT_LIST_DIR}/../../src/*.cpp")
list(FILTER app_sources EXCLUDE REGEX "/src/main\\.cpp$")
list(FILTER app_sources EXCLUDE REGEX "/src/ble/")
list(FILTER app_sources EXCLUDE REGEX "/src/provisioning/improv_ble\\.cpp$")

idf_component_register(
    SRCS
        "main.cpp"
        "provisioning_stub.cpp"
        ${app_sources}
    INCLUDE_DIRS
        "."
        "${CMAKE_BINARY_DIR}"
        "../../include"
        "../../src"
    REQUIRES
        nvs_flash
        esp_event
        esp_netif
        esp_eth
        esp_timer
        esp_wifi
        driver
        expat
        esp_http_client
        esp_http_server
        vfs
        tether
        tether_idf
)

target_compile_options(${COMPONENT_LIB} PRIVATE -Wno-missing-field-initializers)
