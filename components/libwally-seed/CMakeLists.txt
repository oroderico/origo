set(wally_component "${CMAKE_CURRENT_LIST_DIR}/../libwally-core")
set(wally "${wally_component}/upstream")

idf_component_register(
    SRCS "${wally}/src/amalgamation/combined.c"
    INCLUDE_DIRS "${wally}/include"
    PRIV_INCLUDE_DIRS "${CMAKE_CURRENT_LIST_DIR}"
                      "${wally_component}"
                      "${wally}"
                      "${wally}/src"
                      "${wally}/src/ccan"
                      "${wally}/src/secp256k1"
                      "${wally}/src/secp256k1/src"
                      "${wally}/src/secp256k1/include"
                      "${wally}/src/secp256k1/src/modules/ecdh"
                      "${wally}/src/secp256k1/src/modules/ecdsa_s2c"
                      "${wally}/src/secp256k1/src/modules/extrakeys"
                      "${wally}/src/secp256k1/src/modules/generator"
                      "${wally}/src/secp256k1/src/modules/recovery"
                      "${wally}/src/secp256k1/src/modules/schnorrsig"
    PRIV_REQUIRES mbedtls)

set_source_files_properties(
    "${wally}/src/amalgamation/combined.c"
    PROPERTIES COMPILE_FLAGS
    "-Wno-nonnull-compare -Wno-unused-function -Wno-error=maybe-uninitialized -Wno-type-limits -Wno-error=array-parameter")

target_compile_definitions(${COMPONENT_TARGET} PRIVATE
    NDEBUG=1
    BUILD_MINIMAL=1
    HAVE_MBEDTLS_SHA256_H
    HAVE_MBEDTLS_SHA512_H
    ECMULT_WINDOW_SIZE=8
    HAVE_BUILTIN_POPCOUNT=1
    _FORTIFY_SOURCE=2)
target_compile_options(${COMPONENT_TARGET} PRIVATE -fstack-protector-strong)

add_custom_command(TARGET ${COMPONENT_LIB} POST_BUILD
    COMMAND ${CMAKE_OBJCOPY} --strip-unneeded
            -W sha256_init -W sha256_done -W sha512_init -W sha512_done
            ${CMAKE_CURRENT_BINARY_DIR}/liblibwally-seed.a)
