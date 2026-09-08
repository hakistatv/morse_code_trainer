idf_component_register(SRCS "lcd_ui.c"
                       INCLUDE_DIRS "."
                       REQUIRES esp_lcd
                       PRIV_REQUIRES esp_driver_spi esp_driver_i2c esp_io_expander_tca9554
                                     esp_lcd_axs15231b esp_lvgl_port esp_timer)
