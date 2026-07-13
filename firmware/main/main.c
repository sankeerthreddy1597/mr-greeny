#include <string.h>
#include <math.h>
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
#include "cJSON.h"

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

static lv_obj_t *s_status_label;
static lv_obj_t *s_eyes_cont;
static lv_obj_t *s_eye_left;
static lv_obj_t *s_eye_right;
static lv_obj_t *s_mouth;

#define EYE_WIDTH   90
#define EYE_HEIGHT  130
#define EYE_GAP     60

#define MOUTH_WIDTH         70
#define MOUTH_HEIGHT_CLOSED 6
#define MOUTH_HEIGHT_OPEN   30
#define MOUTH_Y_OFFSET      150

// Blink loop: closes to 10px over 120ms, holds, reopens over 120ms, then
// waits ~2.6s before repeating -- runs forever once started, driven entirely
// by LVGL's own animation timer (no extra task/timer bookkeeping needed).
static void blink_height_cb(void *var, int32_t v)
{
    lv_obj_set_height((lv_obj_t *)var, v);
}

static void start_blink_anim(lv_obj_t *eye)
{
    lv_anim_t a;
    lv_anim_init(&a);
    lv_anim_set_var(&a, eye);
    lv_anim_set_exec_cb(&a, blink_height_cb);
    lv_anim_set_values(&a, EYE_HEIGHT, 10);
    lv_anim_set_duration(&a, 120);
    lv_anim_set_reverse_duration(&a, 120);
    lv_anim_set_repeat_count(&a, LV_ANIM_REPEAT_INFINITE);
    lv_anim_set_repeat_delay(&a, 2600);
    lv_anim_start(&a);
}

static lv_obj_t *make_eye(lv_obj_t *parent)
{
    lv_obj_t *eye = lv_obj_create(parent);
    lv_obj_remove_style_all(eye);
    lv_obj_set_size(eye, EYE_WIDTH, EYE_HEIGHT);
    lv_obj_set_style_bg_color(eye, lv_color_white(), 0);
    lv_obj_set_style_bg_opa(eye, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(eye, EYE_WIDTH / 2, 0);
    return eye;
}

// Fast, continuous open/close pulse simulating a talking mouth. Runs
// forever in the background once started; toggling s_mouth's HIDDEN flag
// (see show_talking) is what actually turns it on/off visually.
static void mouth_pulse_cb(void *var, int32_t v)
{
    lv_obj_set_height((lv_obj_t *)var, v);
}

static void start_mouth_pulse(void)
{
    lv_anim_t a;
    lv_anim_init(&a);
    lv_anim_set_var(&a, s_mouth);
    lv_anim_set_exec_cb(&a, mouth_pulse_cb);
    lv_anim_set_values(&a, MOUTH_HEIGHT_CLOSED, MOUTH_HEIGHT_OPEN);
    lv_anim_set_duration(&a, 150);
    lv_anim_set_reverse_duration(&a, 150);
    lv_anim_set_repeat_count(&a, LV_ANIM_REPEAT_INFINITE);
    lv_anim_start(&a);
}

// Builds both UI states once at startup; ws_event_handler only ever toggles
// which one is hidden/visible and updates the label's text in place, instead
// of tearing down and recreating objects on every reconnect attempt (that
// full-screen invalidate churn was overwhelming the panel's SPI queue and
// freezing the display -- see PIXEL_ARCHITECTURE_NOTES.md for why LVGL's
// retained-mode tree is supposed to make this cheap).
static void setup_ui(void)
{
    bsp_display_lock(-1);

    lv_obj_t *scr = lv_scr_act();
    lv_obj_set_style_bg_color(scr, lv_color_black(), 0);

    s_status_label = lv_label_create(scr);
    lv_obj_set_style_text_align(s_status_label, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_text_color(s_status_label, lv_color_white(), 0);
    lv_obj_center(s_status_label);
    lv_label_set_text(s_status_label, "Mr. Greeny\nconnecting...");

    s_eyes_cont = lv_obj_create(scr);
    lv_obj_remove_style_all(s_eyes_cont);
    lv_obj_set_size(s_eyes_cont, LV_PCT(100), LV_PCT(100));
    lv_obj_set_flex_flow(s_eyes_cont, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(s_eyes_cont, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(s_eyes_cont, EYE_GAP, 0);
    lv_obj_add_flag(s_eyes_cont, LV_OBJ_FLAG_HIDDEN);

    s_eye_left = make_eye(s_eyes_cont);
    s_eye_right = make_eye(s_eyes_cont);
    start_blink_anim(s_eye_left);
    start_blink_anim(s_eye_right);

    s_mouth = lv_obj_create(scr);
    lv_obj_remove_style_all(s_mouth);
    lv_obj_set_size(s_mouth, MOUTH_WIDTH, MOUTH_HEIGHT_CLOSED);
    lv_obj_set_style_bg_color(s_mouth, lv_color_white(), 0);
    lv_obj_set_style_bg_opa(s_mouth, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(s_mouth, MOUTH_HEIGHT_CLOSED / 2, 0);
    lv_obj_align(s_mouth, LV_ALIGN_CENTER, 0, MOUTH_Y_OFFSET);
    lv_obj_add_flag(s_mouth, LV_OBJ_FLAG_HIDDEN);
    start_mouth_pulse();

    bsp_display_unlock();
}

// Hiding one big object and showing another in the same lock scope invalidates
// both areas at once; LVGL's next refresh tick can then queue more SPI
// transactions than the panel driver's queue depth, and any transaction that
// fails to queue just never reaches the panel -- leaving stale pixels from
// whichever object was "hidden" behind the new one. Splitting hide/show into
// two separate refresh cycles (with a delay for LVGL's periodic task to
// actually flush the first one) keeps each invalidate small enough on its own.
#define UI_TRANSITION_SETTLE_MS 50

static void show_status_text(const char *text)
{
    bsp_display_lock(-1);
    lv_obj_add_flag(s_eyes_cont, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(s_mouth, LV_OBJ_FLAG_HIDDEN);
    bsp_display_unlock();

    vTaskDelay(pdMS_TO_TICKS(UI_TRANSITION_SETTLE_MS));

    bsp_display_lock(-1);
    lv_label_set_text(s_status_label, text);
    lv_obj_clear_flag(s_status_label, LV_OBJ_FLAG_HIDDEN);
    bsp_display_unlock();
}

static void show_eyes(void)
{
    bsp_display_lock(-1);
    lv_obj_add_flag(s_status_label, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(s_mouth, LV_OBJ_FLAG_HIDDEN);
    bsp_display_unlock();

    vTaskDelay(pdMS_TO_TICKS(UI_TRANSITION_SETTLE_MS));

    bsp_display_lock(-1);
    lv_obj_clear_flag(s_eyes_cont, LV_OBJ_FLAG_HIDDEN);
    bsp_display_unlock();
}

// Mouth toggles on top of the already-visible eyes; a single small object's
// visibility flag is a small enough invalidate that it doesn't need the
// two-refresh-cycle staggering show_eyes/show_status_text use.
static void show_talking(bool talking)
{
    bsp_display_lock(-1);
    if (talking) {
        lv_obj_clear_flag(s_mouth, LV_OBJ_FLAG_HIDDEN);
    } else {
        lv_obj_add_flag(s_mouth, LV_OBJ_FLAG_HIDDEN);
    }
    bsp_display_unlock();
}

// Handles {"type": "assistant_speaking", "state": "start"|"stop"} sent by the
// backend's LiveSession while a Gemini reply is being generated (see
// backend/app.py). Only single, unfragmented text frames are handled --
// these JSON messages are tiny and always arrive in one frame in practice.
static void handle_ws_text(const esp_websocket_event_data_t *data)
{
    if (data->op_code != WS_TRANSPORT_OPCODES_TEXT || data->data_len <= 0) {
        return;
    }
    if (data->payload_offset != 0 || data->data_len != data->payload_len) {
        return;
    }

    cJSON *root = cJSON_ParseWithLength(data->data_ptr, data->data_len);
    if (root == NULL) {
        return;
    }

    const cJSON *type = cJSON_GetObjectItemCaseSensitive(root, "type");
    if (cJSON_IsString(type) && type->valuestring != NULL &&
        strcmp(type->valuestring, "assistant_speaking") == 0) {
        const cJSON *state = cJSON_GetObjectItemCaseSensitive(root, "state");
        if (cJSON_IsString(state) && state->valuestring != NULL) {
            show_talking(strcmp(state->valuestring, "start") == 0);
        }
    }

    cJSON_Delete(root);
}

static void ws_event_handler(void *handler_args, esp_event_base_t base, int32_t event_id, void *event_data)
{
    switch (event_id) {
    case WEBSOCKET_EVENT_CONNECTED:
        ESP_LOGI(TAG, "backend connected");
        show_eyes();
        break;
    case WEBSOCKET_EVENT_DISCONNECTED:
    case WEBSOCKET_EVENT_ERROR:
        ESP_LOGW(TAG, "backend disconnected, retrying...");
        show_status_text("Mr. Greeny\nreconnecting...");
        break;
    case WEBSOCKET_EVENT_DATA:
        handle_ws_text((const esp_websocket_event_data_t *)event_data);
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

    // ~64ms/chunk at 16kHz -> log roughly once/sec. Prints real min/max/RMS
    // so a dead mic (flat zeros, or a stuck DC value) is visibly distinct
    // from real ambient sound.
    int log_counter = 0;

    for (;;) {
        int ret = esp_codec_dev_read(s_mic_dev, pcm, MIC_CHUNK_BYTES);
        if (ret != ESP_CODEC_DEV_OK) {
            ESP_LOGW(TAG, "mic read failed: %d", ret);
            vTaskDelay(pdMS_TO_TICKS(50));
            continue;
        }

        if (++log_counter >= 15) {
            log_counter = 0;
            int16_t min_v = INT16_MAX, max_v = INT16_MIN;
            int64_t sum_sq = 0;
            for (int i = 0; i < MIC_CHUNK_SAMPLES; i++) {
                int16_t s = pcm[i];
                if (s < min_v) min_v = s;
                if (s > max_v) max_v = s;
                sum_sq += (int32_t)s * (int32_t)s;
            }
            int rms = (int)sqrtf((float)sum_sq / MIC_CHUNK_SAMPLES);
            ESP_LOGI(TAG, "mic: min=%d max=%d rms=%d", min_v, max_v, rms);
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

    // BSP defaults to 30dB; push to the ES7210's max (37.5dB) since normal
    // speech was only reaching rms ~20-60 out of a possible 32767 -- observed
    // via the mic: min/max/rms log in mic_stream_task.
    int gain_ret = esp_codec_dev_set_in_gain(s_mic_dev, 37.5f);
    if (gain_ret != ESP_CODEC_DEV_OK) {
        ESP_LOGW(TAG, "set_in_gain failed: %d", gain_ret);
    }
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
    setup_ui();

    // Blocks until Wi-Fi station is up. SSID/password are set via
    // `idf.py menuconfig` -> Example Connection Configuration.
    ESP_ERROR_CHECK(example_connect());

    start_websocket();
    start_mic();

    xTaskCreate(mic_stream_task, "mic_stream", 4096, NULL, 5, NULL);
}
