cmake_minimum_required(VERSION 3.20.0)

find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(stm32n6_zephyr_2d_shapes)

target_compile_definitions(app PRIVATE
  USE_HAL_DRIVER
  APP_HAS_PARALLEL_NETWORKS=0
  LL_ATON_PLATFORM=LL_ATON_PLAT_STM32N6
  LL_ATON_OSAL=LL_ATON_OSAL_ZEPHYR
  LL_ATON_SW_FALLBACK=1
  LL_ATON_DBG_BUFFER_INFO_EXCLUDED=1
)

target_include_directories(app PRIVATE
  include
  ai/inc
  ai/ll_aton
  ai/shapes_mobilenet_v2
  ai/device_stm32n6
  bsp/STM32N6570-DK
  bsp/Components/aps256xx
  bsp/Components/mx66uw1g45g
)

target_sources(app PRIVATE
  src/main.c
  src/heartbeat.c
  src/shapes_classifier.c
  src/stm32n6_external_flash.c
  src/stm32n6_npu_preinit.c
  ai/shapes_mobilenet_v2/shapes_mobilenet_v2.c
  ai/shapes_mobilenet_v2/stai_shapes_mobilenet_v2.c
  ai/device_stm32n6/mcu_cache.c
  ai/device_stm32n6/npu_cache.c
  ai/ll_aton/ll_aton.c
  ai/ll_aton/ll_aton_cipher.c
  ai/ll_aton/ll_aton_lib.c
  ai/ll_aton/ll_aton_lib_sw_operators.c
  ai/ll_aton/ll_aton_osal_zephyr.c
  ai/ll_aton/ll_aton_runtime.c
  ai/ll_aton/ll_aton_stai_internal.c
  ai/ll_aton/ll_aton_util.c
  ai/ll_aton/ll_sw_float.c
  ai/ll_aton/ll_sw_integer.c
  bsp/STM32N6570-DK/stm32n6570_discovery_xspi.c
  bsp/Components/aps256xx/aps256xx.c
  bsp/Components/mx66uw1g45g/mx66uw1g45g.c
  ${ZEPHYR_BASE}/../modules/hal/stm32/stm32cube/stm32n6xx/drivers/src/stm32n6xx_hal_gpio.c
  ${ZEPHYR_BASE}/../modules/hal/stm32/stm32cube/stm32n6xx/drivers/src/stm32n6xx_hal_pwr.c
  ${ZEPHYR_BASE}/../modules/hal/stm32/stm32cube/stm32n6xx/drivers/src/stm32n6xx_hal_pwr_ex.c
)

target_link_libraries(app PRIVATE
  ${CMAKE_CURRENT_SOURCE_DIR}/ai/lib/NetworkRuntime1201_CM55_GCC.a
  m
)
