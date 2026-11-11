idf_component_register(SRCS "main.c" "audio_p4.c" "es8311_p4.c" "sd_log.c"
                       INCLUDE_DIRS "."
                       REQUIRES needle esp_driver_i2s esp_driver_i2c esp_driver_gpio
                                esp_partition esp_timer fatfs sdmmc vfs nvs_flash heap
                                esp_common)

# Per-phase timings (mel, stem, encoder, cross K/V, decoder) are printed for
# every utterance; the timers cost a few reads per clip.
idf_component_get_property(nd_lib needle COMPONENT_LIB)
target_compile_definitions(${nd_lib} PUBLIC ND_PROFILE)
