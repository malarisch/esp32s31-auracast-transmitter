idf_component_register(SRCS "main.c" "i2s_in.c" "test_tone.c"
                       INCLUDE_DIRS "."
                       REQUIRES nvs_flash esp_driver_i2s esp_driver_gpio esp_timer esp_rom esp_hal_i2s audio_dsp)
