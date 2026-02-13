# drivers/dfsdm/CMakeLists.txt

zephyr_library()

# Compile dfsdm_stm32.c only if CONFIG_DFSDM_STM32 is "y"
zephyr_library_sources_ifdef(CONFIG_DFSDM_STM32 dfsdm_stm32.c)
