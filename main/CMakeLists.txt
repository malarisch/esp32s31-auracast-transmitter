idf_component_register(SRCS "main.c" "i2s_in.c" "test_tone.c" "broadcast.c" "adv_bluedroid.c" "pipeline.c"
                       INCLUDE_DIRS "."
                       LDFRAGMENTS "lc3_iram.lf"
                       REQUIRES nvs_flash bt esp_driver_i2s esp_driver_gpio esp_timer esp_rom esp_hal_i2s
                                audio_dsp ctrl_regs espressif__esp_audio_codec)
