idf_component_register(
    SRCS "seedtool_app.c"
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
    PRIV_REQUIRES libwally-seed esp_driver_gpio esp_driver_spi esp_hw_support)

target_compile_options(${COMPONENT_LIB} PRIVATE -Werror=unused-result)

idf_build_get_property(link_options LINK_OPTIONS)
list(APPEND link_options "-Wl,--wrap=abort")
idf_build_set_property(LINK_OPTIONS "${link_options}" APPEND)
