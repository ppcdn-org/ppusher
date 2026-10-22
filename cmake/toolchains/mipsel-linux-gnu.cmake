# little-endian MIPS Linux (MT7620/MT7621 and similar router/camera SoCs; OpenWrt: use its own toolchain
# name, e.g. mipsel-openwrt-linux-musl, via -DPPUSHER_TRIPLE=...). See arm-linux-gnueabihf.cmake for usage.
set(CMAKE_SYSTEM_NAME Linux)
set(CMAKE_SYSTEM_PROCESSOR mipsel)
if(NOT PPUSHER_TRIPLE)
  set(PPUSHER_TRIPLE mipsel-linux-gnu)
endif()
set(_triple ${PPUSHER_TRIPLE})
set(CMAKE_C_COMPILER ${_triple}-gcc)
set(CMAKE_CXX_COMPILER ${_triple}-g++)
include(${CMAKE_CURRENT_LIST_DIR}/sysroot.cmake)
