idf_component_register(SRCS "main.c" "solar.c" "gnss.c" "rtc.c" "rtc_core.c" "lcd.c"
                       INCLUDE_DIRS "."
                       REQUIRES nvs_flash esp_lcd esp_driver_uart esp_driver_spi esp_driver_gpio)
