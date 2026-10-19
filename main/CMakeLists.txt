idf_component_register(
    SRCS "seedtool_app.c"
         "app_seed.c"
         "app_ui.c"
         "app_wallet.c"
         "app_words.c"
         "seedtool_bbqr.c"
         "seedtool_core.c"
         "seedtool_display.c"
         "seedtool_render.c"
         "seedtool_platform_esp.c"
         "seedtool_wordlist.c"
         "seedtool_logo.c"
         "origo_esp_main.c"
         "qrcode.c"
         "fonts/DefaultFont.c"
         "fonts/Ubuntu16.c"
    INCLUDE_DIRS "."
    PRIV_REQUIRES libwally-seed esp_driver_gpio esp_driver_spi esp_driver_ledc esp_hw_support heap)

target_compile_options(${COMPONENT_LIB} PRIVATE -Wall -Wextra -Werror)

# The version About shows: ESP-IDF's PROJECT_VER, `git describe --always --tags
# --dirty` of the source, the same string the image's app description carries.
# It is a fact about the source, so it keeps the build reproducible.
idf_build_get_property(project_ver PROJECT_VER)
target_compile_definitions(${COMPONENT_LIB} PRIVATE ORIGO_VERSION="${project_ver}")

idf_build_get_property(link_options LINK_OPTIONS)
list(APPEND link_options "-Wl,--wrap=abort")
idf_build_set_property(LINK_OPTIONS "${link_options}" APPEND)
