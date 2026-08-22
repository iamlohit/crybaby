idf_component_register(
    SRCS "main.c" "mic.c" "net.c" "display.c" "ui.c" "speaker.c"
    INCLUDE_DIRS "."
    REQUIRES driver esp_wifi esp_http_client esp_event nvs_flash esp_lcd esp_adc json mbedtls
)
