idf_component_register(SRCS "main.c" "radar.c" "voice.c" "shell.c"
                       PRIV_REQUIRES esp_driver_uart esp_driver_gpio esp_timer console esp_system
                       INCLUDE_DIRS "")
