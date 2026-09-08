idf_component_register(SRCS "morse_code_trainer_main.c"
                       PRIV_REQUIRES spi_flash esp_hw_support
                                     morse_tree lcd_ui morse_key sidetone word_list
                       INCLUDE_DIRS "")
