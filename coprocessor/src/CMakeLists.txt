#This file was automatically generated for projects
#without default 'CMakeLists.txt' file.

FILE(GLOB_RECURSE app_sources ${CMAKE_SOURCE_DIR}/src/*.*)

idf_component_register(SRCS ${app_sources}
                       INCLUDE_DIRS "." "../include"
                       REQUIRES nvs_flash esp_wifi bt esp_event esp_netif driver expat esp_http_client vfs tether tether_idf)

target_compile_options(${COMPONENT_LIB} PRIVATE -Wno-missing-field-initializers)
# The release build (sdkconfig.release.defaults) raises our code from -O2.
if(CONFIG_COMPILER_OPTIMIZATION_PERF)
  target_compile_options(${COMPONENT_LIB} PRIVATE -O3)
endif()
