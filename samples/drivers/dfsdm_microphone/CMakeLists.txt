# samples/drivers/dfsdm_microphone/CMakeLists.txt

cmake_minimum_required(VERSION 3.20.0)

# 1. Pull in the Zephyr Build System
find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})

# 2. Define the project name
project(dfsdm_microphone)

# 3. Add your source file(s) to the 'app' target
target_sources(app PRIVATE src/main.c)
