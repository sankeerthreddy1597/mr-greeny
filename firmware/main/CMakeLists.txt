idf_component_register(
    SRCS main.c
    INCLUDE_DIRS .
    PRIV_REQUIRES
        json
        nvs_flash
        esp_event
        esp_netif
        protocol_examples_common
        espressif__esp_websocket_client
        waveshare__esp32_s3_touch_amoled_1_75
        espressif__esp_codec_dev
        lvgl__lvgl)
