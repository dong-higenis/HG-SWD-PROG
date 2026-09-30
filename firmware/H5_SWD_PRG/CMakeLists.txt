cmake_minimum_required(VERSION 3.13)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON CACHE INTERNAL "")
set(CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/tools")
include(arm-none-eabi-gcc)

set(PRJ_NAME stm32h5-fw)


project(${PRJ_NAME}
  LANGUAGES ASM C CXX
)


set(EXECUTABLE ${PRJ_NAME}.elf)

file(GLOB_RECURSE APP_SOURCES CONFIGURE_DEPENDS
  src/ap/*.c src/ap/*.cpp src/bsp/*.c src/bsp/*.s
  src/common/*.c src/hw/*.c)
file(GLOB RTOS_SOURCES CONFIGURE_DEPENDS
  src/lib/FreeRTOS/Source/*.c
  src/lib/FreeRTOS/Source/portable/GCC/ARM_CM33_NTZ/non_secure/*.c
  src/lib/FreeRTOS/Source/CMSIS_RTOS/*.c)
file(GLOB FATFS_SOURCES CONFIGURE_DEPENDS src/lib/fatfs/*.c)
set(HAL_SOURCES
  src/lib/ST/STM32H5xx_HAL_Driver/Src/stm32h5xx_hal.c
  src/lib/ST/STM32H5xx_HAL_Driver/Src/stm32h5xx_hal_cortex.c
  src/lib/ST/STM32H5xx_HAL_Driver/Src/stm32h5xx_hal_dcache.c
  src/lib/ST/STM32H5xx_HAL_Driver/Src/stm32h5xx_hal_dma.c
  src/lib/ST/STM32H5xx_HAL_Driver/Src/stm32h5xx_hal_dma_ex.c
  src/lib/ST/STM32H5xx_HAL_Driver/Src/stm32h5xx_hal_exti.c
  src/lib/ST/STM32H5xx_HAL_Driver/Src/stm32h5xx_hal_flash.c
  src/lib/ST/STM32H5xx_HAL_Driver/Src/stm32h5xx_hal_flash_ex.c
  src/lib/ST/STM32H5xx_HAL_Driver/Src/stm32h5xx_hal_gpio.c
  src/lib/ST/STM32H5xx_HAL_Driver/Src/stm32h5xx_hal_i2c.c
  src/lib/ST/STM32H5xx_HAL_Driver/Src/stm32h5xx_hal_i2c_ex.c
  src/lib/ST/STM32H5xx_HAL_Driver/Src/stm32h5xx_hal_icache.c
  src/lib/ST/STM32H5xx_HAL_Driver/Src/stm32h5xx_hal_pwr.c
  src/lib/ST/STM32H5xx_HAL_Driver/Src/stm32h5xx_hal_pwr_ex.c
  src/lib/ST/STM32H5xx_HAL_Driver/Src/stm32h5xx_hal_rcc.c
  src/lib/ST/STM32H5xx_HAL_Driver/Src/stm32h5xx_hal_rcc_ex.c
  src/lib/ST/STM32H5xx_HAL_Driver/Src/stm32h5xx_hal_rtc.c
  src/lib/ST/STM32H5xx_HAL_Driver/Src/stm32h5xx_hal_rtc_ex.c
  src/lib/ST/STM32H5xx_HAL_Driver/Src/stm32h5xx_hal_sd.c
  src/lib/ST/STM32H5xx_HAL_Driver/Src/stm32h5xx_hal_sd_ex.c
  src/lib/ST/STM32H5xx_HAL_Driver/Src/stm32h5xx_hal_tim.c
  src/lib/ST/STM32H5xx_HAL_Driver/Src/stm32h5xx_hal_tim_ex.c
  src/lib/ST/STM32H5xx_HAL_Driver/Src/stm32h5xx_hal_uart.c
  src/lib/ST/STM32H5xx_HAL_Driver/Src/stm32h5xx_hal_uart_ex.c
  src/lib/ST/STM32H5xx_HAL_Driver/Src/stm32h5xx_hal_xspi.c
  src/lib/ST/STM32H5xx_HAL_Driver/Src/stm32h5xx_ll_sdmmc.c
  src/lib/ST/STM32H5xx_HAL_Driver/Src/stm32h5xx_ll_dlyb.c
)
add_executable(${EXECUTABLE} src/main.cpp ${APP_SOURCES} ${RTOS_SOURCES}
  ${FATFS_SOURCES} ${HAL_SOURCES}
  src/lib/FreeRTOS/Source/portable/MemMang/heap_4.c)
target_include_directories(${EXECUTABLE} PRIVATE
  src src/ap src/ap/modules src/ap/modules/common
  src/bsp src/bsp/device src/bsp/rtos
  src/common src/common/core src/common/hw/include src/hw src/hw/driver
  src/lib src/lib/fatfs
  src/lib/ST/CMSIS/Include src/lib/ST/CMSIS/Device/ST/STM32H5xx/Include
  src/lib/ST/STM32H5xx_HAL_Driver/Inc
  src/lib/FreeRTOS/Source/CMSIS_RTOS src/lib/FreeRTOS/Source/include
  src/lib/FreeRTOS/Source/portable/GCC/ARM_CM33_NTZ/non_secure)

target_compile_definitions(${EXECUTABLE} PRIVATE
  -DSTM32H563xx
  -DUSE_HAL_DRIVER  
  )

target_compile_options(${EXECUTABLE} PRIVATE
  -mcpu=cortex-m33
  -mthumb
  -mfpu=fpv4-sp-d16
  -mfloat-abi=hard

  -fdata-sections
  -ffunction-sections
  
  -fstack-usage 
  -Wmissing-field-initializers
  -Wuninitialized 
  # -Wenum-conversion

  -Wall
  -g3
  -Og
  )

# Only the timing-critical SWD path uses speed optimization. Keep the rest of
# the application's debug-friendly build settings.
set_source_files_properties(
  src/hw/driver/swd.c
  src/hw/driver/swd/swd_transport.c
  src/hw/driver/swd/swd_probe.c
  src/hw/driver/swd/swd_program.c
  PROPERTIES COMPILE_OPTIONS "-O3")

target_link_options(${EXECUTABLE} PRIVATE
  "-T${CMAKE_CURRENT_SOURCE_DIR}/src/bsp/ldscript/STM32H563xx_FLASH.ld"

  -mcpu=cortex-m33
  -mthumb
  -mfpu=fpv4-sp-d16
  -mfloat-abi=hard
  -specs=nano.specs
  -lc
  -lm
  -lstdc++
  -lsupc++
  -fno-rtti
  
  -Wl,-Map=${PRJ_NAME}.map,--cref
  -Wl,--gc-sections
  -Xlinker -print-memory-usage -Xlinker
  )

set (CMAKE_C_FLAGS "-Wenum-conversion")
set (CMAKE_CXX_FLAGS "-fno-rtti")

message(${CMAKE_CXX_COMPILER_VERSION})
if(CMAKE_CXX_COMPILER_VERSION VERSION_GREATER_EQUAL 11)
  string(APPEND CMAKE_EXE_LINKER_FLAGS "-Wl,--no-warn-rwx-segments")
endif()

add_custom_command(TARGET ${EXECUTABLE} 
  POST_BUILD
  COMMAND ${CMAKE_OBJCOPY} ARGS -O binary ${EXECUTABLE} ${PROJECT_NAME}.bin
  COMMENT "Generating firmware binary"
  )  
  

