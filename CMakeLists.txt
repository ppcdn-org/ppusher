cmake_minimum_required(VERSION 3.10)
project(ppusher VERSION 0.1.0 LANGUAGES C)

set(CMAKE_C_STANDARD 99)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS ON)

option(PPUSHER_BUILD_TESTS "Build unit tests" ON)
option(PPUSHER_BUILD_CLI "Build ppusher-cli" ON)
option(PPUSHER_WITH_SRT "Build the libsrt sender (session + public API); OFF builds only the pure-logic core" ON)
set(PPUSHER_TLS "openssl" CACHE STRING "TLS backend for ppcenter HTTPS / wss: openssl | none")
set_property(CACHE PPUSHER_TLS PROPERTY STRINGS openssl none)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
  set(CMAKE_BUILD_TYPE Release CACHE STRING "" FORCE)
endif()

if(CMAKE_C_COMPILER_ID MATCHES "GNU|Clang")
  add_compile_options(-Wall -Wextra -Wno-unused-parameter -ffunction-sections -fdata-sections)
  if(NOT APPLE)
    add_link_options(-Wl,--gc-sections)
  endif()
endif()

find_package(Threads REQUIRED)

# ---- pure logic: no I/O, no libsrt ----
set(PPUSHER_CORE_SRC
  src/pp_platform.c
  src/pp_json.c
  src/pp_streamid.c
  src/pp_degrade.c
  src/pp_loss.c
  src/pp_sm.c
  src/pp_annexb.c
  src/pp_ts.c
)
# ---- HTTP(S) / WebSocket / ppcenter client: sockets (+ optional OpenSSL), no libsrt ----
set(PPUSHER_NET_SRC
  src/pp_net.c
  src/pp_http.c
  src/pp_ws.c
  src/pp_ppcenter.c
)

add_library(ppusher_core STATIC ${PPUSHER_CORE_SRC})
target_include_directories(ppusher_core PUBLIC src)

add_library(ppusher_net STATIC ${PPUSHER_CORE_SRC} ${PPUSHER_NET_SRC})
target_include_directories(ppusher_net PUBLIC src)
if(WIN32)
  target_link_libraries(ppusher_net PUBLIC ws2_32)
endif()
if(PPUSHER_TLS STREQUAL "openssl")
  find_package(OpenSSL REQUIRED)
  target_compile_definitions(ppusher_net PUBLIC PPUSHER_HAVE_OPENSSL=1)
  target_link_libraries(ppusher_net PUBLIC OpenSSL::SSL OpenSSL::Crypto)
elseif(NOT PPUSHER_TLS STREQUAL "none")
  message(FATAL_ERROR "PPUSHER_TLS must be openssl or none")
endif()

if(PPUSHER_WITH_SRT)
  # libsrt: pkg-config first, then plain find_path/find_library (cross builds: set CMAKE_PREFIX_PATH / SRT_ROOT)
  find_package(PkgConfig QUIET)
  set(PPUSHER_SRT_TARGET "")
  if(PkgConfig_FOUND)
    pkg_check_modules(SRT QUIET IMPORTED_TARGET srt)
    if(SRT_FOUND)
      set(PPUSHER_SRT_TARGET PkgConfig::SRT)
    endif()
  endif()
  if(NOT PPUSHER_SRT_TARGET)
    find_path(SRT_INCLUDE_DIR srt/srt.h HINTS ${SRT_ROOT} PATH_SUFFIXES include)
    find_library(SRT_LIBRARY NAMES srt libsrt HINTS ${SRT_ROOT} PATH_SUFFIXES lib)
    if(NOT SRT_INCLUDE_DIR OR NOT SRT_LIBRARY)
      message(FATAL_ERROR "libsrt not found: install libsrt-dev, or pass -DSRT_ROOT=<prefix>, or -DPPUSHER_WITH_SRT=OFF")
    endif()
    add_library(ppusher_srt_imported INTERFACE)
    target_include_directories(ppusher_srt_imported INTERFACE ${SRT_INCLUDE_DIR})
    target_link_libraries(ppusher_srt_imported INTERFACE ${SRT_LIBRARY})
    set(PPUSHER_SRT_TARGET ppusher_srt_imported)
  endif()

  add_library(ppusher ${PPUSHER_CORE_SRC} ${PPUSHER_NET_SRC} src/ppusher.c)
  target_include_directories(ppusher PUBLIC include PRIVATE src)
  target_link_libraries(ppusher PUBLIC Threads::Threads ${PPUSHER_SRT_TARGET})
  if(WIN32)
    target_link_libraries(ppusher PUBLIC ws2_32)
  endif()
  if(PPUSHER_TLS STREQUAL "openssl")
    target_compile_definitions(ppusher PRIVATE PPUSHER_HAVE_OPENSSL=1)
    target_link_libraries(ppusher PUBLIC OpenSSL::SSL OpenSSL::Crypto)
  endif()
  set_target_properties(ppusher PROPERTIES OUTPUT_NAME ppusher VERSION ${PROJECT_VERSION})
  install(TARGETS ppusher ARCHIVE DESTINATION lib LIBRARY DESTINATION lib RUNTIME DESTINATION bin)
  install(FILES include/ppusher/ppusher.h DESTINATION include/ppusher)
endif()

if(PPUSHER_WITH_SRT AND PPUSHER_BUILD_CLI)
  add_subdirectory(examples)
endif()

if(PPUSHER_BUILD_TESTS)
  enable_testing()
  add_subdirectory(tests)
endif()
