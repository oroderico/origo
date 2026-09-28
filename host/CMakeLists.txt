cmake_minimum_required(VERSION 3.16)
project(origo_host C)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS ON)

find_package(PkgConfig REQUIRED)

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

set(ORIGO_WARNINGS -Wall -Wextra -Werror -fstack-protector-strong)

# -DORIGO_SANITIZE=ON builds everything below under AddressSanitizer and
# UndefinedBehaviorSanitizer: reading the code finds the bugs someone thought
# to look for, these find the out-of-bounds access nobody did. Never applied
# to wally_host: libwally is vendored, and secp256k1's inline assembly does not
# compile instrumented. Needs a sanitizer runtime; on Fedora configure with
# -DCMAKE_C_COMPILER=clang.
option(ORIGO_SANITIZE "Build Origo's sources with ASan and UBSan" OFF)
function(origo_target target)
    target_include_directories(${target} PRIVATE "${ORIGO_ROOT}/main")
    target_compile_options(${target} PRIVATE ${ORIGO_WARNINGS})
    if(ORIGO_SANITIZE)
        target_compile_options(${target} PRIVATE
            -fsanitize=address,undefined -fno-omit-frame-pointer -fno-sanitize-recover=all -g)
        target_link_options(${target} PRIVATE -fsanitize=address,undefined)
    endif()
endfunction()

set(CORE_SOURCES
    "${ORIGO_ROOT}/main/seedtool_bbqr.c"
    "${ORIGO_ROOT}/main/seedtool_core.c"
    "${ORIGO_ROOT}/main/seedtool_wordlist.c")
set(APP_SOURCES
    "${ORIGO_ROOT}/main/seedtool_app.c"
    "${ORIGO_ROOT}/main/app_seed.c"
    "${ORIGO_ROOT}/main/app_ui.c"
    "${ORIGO_ROOT}/main/app_wallet.c"
    "${ORIGO_ROOT}/main/app_words.c")
set(RENDER_SOURCES
    "${ORIGO_ROOT}/main/seedtool_render.c"
    "${ORIGO_ROOT}/main/seedtool_logo.c"
    "${ORIGO_ROOT}/main/qrcode.c"
    "${ORIGO_ROOT}/main/fonts/DefaultFont.c"
    "${ORIGO_ROOT}/main/fonts/Ubuntu16.c")

enable_testing()
foreach(suite core entropy_stats render)
    add_executable(test_${suite} "${ORIGO_ROOT}/tests/test_${suite}.c" ${CORE_SOURCES})
    origo_target(test_${suite})
    target_link_libraries(test_${suite} PRIVATE wally_host m)
    add_test(NAME ${suite} COMMAND test_${suite})
endforeach()
target_sources(test_render PRIVATE ${RENDER_SOURCES})

add_executable(test_qrcode "${ORIGO_ROOT}/tests/test_qrcode.c" "${ORIGO_ROOT}/main/qrcode.c")
origo_target(test_qrcode)
add_test(NAME qrcode COMMAND test_qrcode)

# The app replayed from a key script, for checking that a change to it leaves
# every screen as it was (see origo_replay.c).
add_executable(origo-replay origo_replay.c ${APP_SOURCES} ${CORE_SOURCES} ${RENDER_SOURCES})
origo_target(origo-replay)
target_link_libraries(origo-replay PRIVATE wally_host m)

# The simulator runs the firmware's app against an SDL window. The tests above
# do not need it, so a machine without SDL still builds and runs them.
pkg_check_modules(SDL2 IMPORTED_TARGET sdl2)
if(SDL2_FOUND)
    add_executable(origo-simulator origo_sdl.c ${APP_SOURCES} ${CORE_SOURCES} ${RENDER_SOURCES})
    origo_target(origo-simulator)
    target_link_libraries(origo-simulator PRIVATE wally_host PkgConfig::SDL2 m)
endif()
