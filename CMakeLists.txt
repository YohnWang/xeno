cmake_minimum_required(VERSION 4.3)

set(CMAKE_SYSTEM_NAME Linux)
set(CMAKE_SYSTEM_PROCESSOR arm)
set(CMAKE_C_COMPILER arm-none-eabi-gcc)
set(CMAKE_CXX_COMPILER arm-none-eabi-g++)
set(CMAKE_TRY_COMPILE_TARGET_TYPE STATIC_LIBRARY)
set(CMAKE_C_STANDARD 23)
set(CMAKE_CXX_STANDARD 26)

add_compile_options(-mthumb -mcpu=cortex-m7 -mfpu=fpv5-d16 -mfloat-abi=hard)
add_link_options(-mthumb -mcpu=cortex-m7 -mfpu=fpv5-d16 -mfloat-abi=hard -Wl,--no-warn-rwx-segments)

add_compile_options(-ftls-model=local-exec)
add_link_options(-ftls-model=local-exec -nostartfiles)

project(xeno)
enable_language(C CXX ASM)

set(FREERTOS_PORT GCC_ARM_CM4F)

add_subdirectory(arm_simulator)
add_subdirectory(xeno)