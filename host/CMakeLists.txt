cmake_minimum_required(VERSION 3.16)
project(origo_simulator C)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS ON)

find_package(PkgConfig REQUIRED)
pkg_check_modules(SDL2 REQUIRED IMPORTED_TARGET sdl2)

set(ORIGO_ROOT "${CMAKE_CURRENT_LIST_DIR}/..")
set(WALLY_COMPONENT "${ORIGO_ROOT}/components/libwally-core")
set(WALLY "${WALLY_COMPONENT}/upstream")

add_library(wally_host STATIC "${WALLY}/src/amalgamation/combined.c")
target_include_directories(wally_host
    PUBLIC "${WALLY}/include"
    PRIVATE "${WALLY_COMPONENT}"
            "${WALLY}"
            "${WALLY}/src"
            "${WALLY}/src/ccan"
            "${WALLY}/src/secp256k1"
            "${WALLY}/src/secp256k1/src"
            "${WALLY}/src/secp256k1/include"
            "${WALLY}/src/secp256k1/src/modules/ecdh"
            "${WALLY}/src/secp256k1/src/modules/ecdsa_s2c"
            "${WALLY}/src/secp256k1/src/modules/extrakeys"
            "${WALLY}/src/secp256k1/src/modules/generator"
            "${WALLY}/src/secp256k1/src/modules/recovery"
            "${WALLY}/src/secp256k1/src/modules/schnorrsig")
target_compile_definitions(wally_host PRIVATE
    NDEBUG=1 BUILD_MINIMAL=1 ECMULT_WINDOW_SIZE=8 HAVE_BUILTIN_POPCOUNT=1 _FORTIFY_SOURCE=2)
target_compile_options(wally_host PRIVATE
    -fstack-protector-strong -Wno-nonnull-compare -Wno-unused-function
    -Wno-maybe-uninitialized -Wno-type-limits -Wno-array-parameter)

add_executable(origo-simulator
    origo_simulator.c
    origo_sdl.c
    "${ORIGO_ROOT}/main/seedtool_app.c"
    "${ORIGO_ROOT}/main/seedtool_core.c"
    "${ORIGO_ROOT}/main/seedtool_render.c"
    "${ORIGO_ROOT}/main/seedtool_wordlist.c"
    "${ORIGO_ROOT}/main/seedtool_logo.c"
    "${ORIGO_ROOT}/main/qrcode.c"
    "${ORIGO_ROOT}/main/fonts/DefaultFont.c"
    "${ORIGO_ROOT}/main/fonts/Ubuntu16.c")
target_include_directories(origo-simulator PRIVATE "${ORIGO_ROOT}/main")
target_compile_options(origo-simulator PRIVATE -Wall -Wextra -Werror -fstack-protector-strong)
target_link_libraries(origo-simulator PRIVATE wally_host PkgConfig::SDL2 m)

enable_testing()
add_test(NAME origo-host-self-test COMMAND origo-simulator --self-test)
