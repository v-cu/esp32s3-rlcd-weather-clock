idf_component_register(
    SRCS "pcf85063.c" "wifi_sync.c" "clock_task.c" "shtc3.c" "battery.c" "weather.c" "env_history.c" "overlay.c" "page_charts.c" "page_weather.c" "page_diag.c"
    INCLUDE_DIRS "."
    PRIV_REQUIRES main driver esp_wifi esp_netif esp_event nvs_flash lwip ui app_bsp esp_adc esp_http_client esp-tls json esp_app_format
)