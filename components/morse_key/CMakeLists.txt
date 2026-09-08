idf_component_register(SRCS "morse_key.c"
                       INCLUDE_DIRS "."
                       REQUIRES morse_tree
                       PRIV_REQUIRES lcd_ui esp_driver_gpio esp_timer)
