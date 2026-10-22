# 32-bit ARM Linux (Cortex-A7/A9 class boxes: Allwinner, i.MX6, Rockchip RK3xxx ...).
# Usage: cmake -S . -B build-arm -DCMAKE_TOOLCHAIN_FILE=cmake/toolchains/arm-linux-gnueabihf.cmake \
#              -DPPUSHER_SYSROOT=/path/to/sysroot   (sysroot must contain libsrt + openssl, built with the same toolchain)
set(CMAKE_SYSTEM_NAME Linux)
set(CMAKE_SYSTEM_PROCESSOR arm)
set(_triple arm-linux-gnueabihf)
set(CMAKE_C_COMPILER ${_triple}-gcc)
set(CMAKE_CXX_COMPILER ${_triple}-g++)
include(${CMAKE_CURRENT_LIST_DIR}/sysroot.cmake)
