add_library(device)

target_include_directories(device PUBLIC CMSIS/Core/Include ARMCM7/Include)

target_compile_definitions(device PUBLIC ARMCM7_DP)

target_sources(device
    PRIVATE
        ARMCM7/Source/system_ARMCM7.c
        ARMCM7/Source/GCC/startup_ARMCM7.S
)

add_linkerscript(linker ARMCM7/Source/GCC/gcc_arm.ld)

target_link_libraries(device PUBLIC linker)