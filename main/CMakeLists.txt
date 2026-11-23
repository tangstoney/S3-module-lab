idf_component_register(SRCS "fob_host.c" "fob_proto.c" "ble_central.c" "alarm.c" "voice_dl.c" "key.c" "main.c" "radar.c" "voice.c" "shell.c"
                       PRIV_REQUIRES esp_driver_uart esp_driver_gpio esp_driver_i2c esp_timer console esp_system
                                     bt nvs_flash nimble_central_utils
                       INCLUDE_DIRS "")

# Only embed the 96 KB WAV when the one-shot download build is enabled.
if(CONFIG_LAB_VOICE_DOWNLOAD_AUDIO)
    target_add_binary_data(${COMPONENT_LIB} "audio/0001.wav" BINARY RENAME_TO "voice_0001_wav")
endif()
