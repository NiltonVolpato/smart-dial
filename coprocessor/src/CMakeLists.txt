#This file was automatically generated for projects
#without default 'CMakeLists.txt' file.

FILE(GLOB_RECURSE app_sources ${CMAKE_SOURCE_DIR}/src/*.*)

idf_component_register(SRCS ${app_sources}
                       INCLUDE_DIRS "." "../include"
                       REQUIRES nvs_flash esp_wifi bt esp_event esp_netif driver expat esp_http_client vfs tether tether_idf)

target_compile_options(${COMPONENT_LIB} PRIVATE -Wno-missing-field-initializers)
# mjansson/mdns (a submodule), header-only. SYSTEM: its warnings aren't ours.
target_include_directories(${COMPONENT_LIB} SYSTEM PRIVATE
    "${CMAKE_SOURCE_DIR}/../third_party/mdns")
# The release build (sdkconfig.release.defaults) raises our code from -O2.
if(CONFIG_COMPILER_OPTIMIZATION_PERF)
  target_compile_options(${COMPONENT_LIB} PRIVATE -O3)
endif()
