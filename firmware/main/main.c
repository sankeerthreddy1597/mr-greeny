#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "nvs_flash.h"
#include "esp_log.h"
#include "esp_err.h"
#include "esp_netif.h"
#include "esp_event.h"
#include "esp_websocket_client.h"
#include "protocol_examples_common.h"
#include "lvgl.h"
#include "bsp/esp-bsp.h"
#include "bsp/display.h"
#include "esp_codec_dev.h"

static const char *TAG = "mr_greeny";

// Matches the binary framing used by OmniBot's Pixel bot: [1-byte packet type][payload].
// We only need the mic-PCM type since this board has no camera.
#define AUDIO_PACKET_TYPE   0x10

#define MIC_SAMPLE_RATE     16000
#define MIC_CHANNELS        1
#define MIC_BITS            16
#define MIC_CHUNK_SAMPLES   1024 // ~64 ms per chunk at 16 kHz
#define MIC_CHUNK_BYTES     (MIC_CHUNK_SAMPLES * (MIC_BITS / 8))

static esp_websocket_client_handle_t s_ws_client;
static esp_codec_dev_handle_t s_mic_dev;

static void set_status_text(const char *text)
{
    bsp_display_lock(-1);
    lv_obj_t *scr = lv_scr_act();
    lv_obj_clean(scr);
    lv_obj_t *label = lv_label_create(scr);
    lv_label_set_text(label, text);
    lv_obj_set_style_text_align(label, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_center(label);
    bsp_display_unlock();
}

static void ws_event_handler(void *handler_args, esp_event_base_t base, int32_t event_id, void *event_data)
{
    switch (event_id) {
    case WEBSOCKET_EVENT_CONNECTED:
        ESP_LOGI(TAG, "backend connected");
        set_status_text("Mr. Greeny\nlistening...");
        break;
    case WEBSOCKET_EVENT_DISCONNECTED:
    case WEBSOCKET_EVENT_ERROR:
        ESP_LOGW(TAG, "backend disconnected, retrying...");
        set_status_text("Mr. Greeny\nreconnecting...");
        break;
    default:
        break;
    }
}

// Streams raw mic PCM to the backend continuously; the backend runs wake-word
// detection + VAD on this stream (see ../backend). Nothing AI-related runs on
// the board itself, matching the client/server split documented in
// PIXEL_ARCHITECTURE_NOTES.md.
static void mic_stream_task(void *arg)
{
    static uint8_t packet[1 + MIC_CHUNK_BYTES];
    packet[0] = AUDIO_PACKET_TYPE;
    int16_t *pcm = (int16_t *)&packet[1];

    for (;;) {
        int ret = esp_codec_dev_read(s_mic_dev, pcm, MIC_CHUNK_BYTES);
        if (ret != ESP_CODEC_DEV_OK) {
            ESP_LOGW(TAG, "mic read failed: %d", ret);
            vTaskDelay(pdMS_TO_TICKS(50));
            continue;
        }

        if (esp_websocket_client_is_connected(s_ws_client)) {
            esp_websocket_client_send_bin(s_ws_client, (const char *)packet, sizeof(packet), portMAX_DELAY);
        }
    }
}

static void start_mic(void)
{
    s_mic_dev = bsp_audio_codec_microphone_init();
    assert(s_mic_dev);

    esp_codec_dev_sample_info_t fs = {
        .sample_rate = MIC_SAMPLE_RATE,
        .channel = MIC_CHANNELS,
        .bits_per_sample = MIC_BITS,
    };
    ESP_ERROR_CHECK(esp_codec_dev_open(s_mic_dev, &fs));
}

static void start_websocket(void)
{
    esp_websocket_client_config_t ws_cfg = {
        .uri = CONFIG_MRGREENY_BACKEND_WS_URL,
    };
    s_ws_client = esp_websocket_client_init(&ws_cfg);
    esp_websocket_register_events(s_ws_client, WEBSOCKET_EVENT_ANY, ws_event_handler, NULL);
    esp_websocket_client_start(s_ws_client);
}

void app_main(void)
{
    ESP_ERROR_CHECK(nvs_flash_init());
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());

    bsp_display_start();
    set_status_text("Mr. Greeny\nconnecting...");

    // Blocks until Wi-Fi station is up. SSID/password are set via
    // `idf.py menuconfig` -> Example Connection Configuration.
    ESP_ERROR_CHECK(example_connect());

    start_websocket();
    start_mic();

    xTaskCreate(mic_stream_task, "mic_stream", 4096, NULL, 5, NULL);
}
