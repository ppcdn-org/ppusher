# 64-bit ARM Linux (Cortex-A53/A55/A72 boxes). See arm-linux-gnueabihf.cmake for usage.
set(CMAKE_SYSTEM_NAME Linux)
set(CMAKE_SYSTEM_PROCESSOR aarch64)
set(_triple aarch64-linux-gnu)
set(CMAKE_C_COMPILER ${_triple}-gcc)
set(CMAKE_CXX_COMPILER ${_triple}-g++)
include(${CMAKE_CURRENT_LIST_DIR}/sysroot.cmake)
