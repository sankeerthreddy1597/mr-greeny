#include <string.h>
#include <math.h>
#include <stdlib.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
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
#include "esp_heap_caps.h"
#include "cJSON.h"

static const char *TAG = "mr_greeny";

// Matches the binary framing used by OmniBot's Pixel bot: [1-byte packet type][payload].
// We only need the mic-PCM type since this board has no camera.
#define AUDIO_PACKET_TYPE   0x10
// backend -> board: Gemini's reply audio, resampled server-side to 16kHz to
// match the mic/speaker's shared duplex I2S bus (see backend/app.py).
#define REPLY_PCM_PACKET_TYPE 0x20

#define MIC_SAMPLE_RATE     16000
#define MIC_CHANNELS        1
#define MIC_BITS            16
#define MIC_CHUNK_SAMPLES   1024 // ~64 ms per chunk at 16 kHz
#define MIC_CHUNK_BYTES     (MIC_CHUNK_SAMPLES * (MIC_BITS / 8))

#define SPEAKER_SAMPLE_RATE 16000 // must match MIC_SAMPLE_RATE -- shared duplex I2S bus
#define SPEAKER_CHANNELS    1
#define SPEAKER_BITS        16
// Generous single-message cap; a fragment straddling this gets dropped with
// a warning rather than overflowing the reassembly buffer.
#define REPLY_AUDIO_MAX_BYTES 32768

static esp_websocket_client_handle_t s_ws_client;
static esp_codec_dev_handle_t s_mic_dev;
static esp_codec_dev_handle_t s_speaker_dev;
static QueueHandle_t s_playback_queue;
static uint8_t *s_reassembly_buf; // PSRAM -- see init_reply_audio_buffer()

typedef struct {
    uint8_t *data;
    size_t len;
} playback_chunk_t;

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

// Fast, continuous open/close pulse simulating a talking mouth. Only runs
// while actually talking (started/stopped in show_talking) -- letting this
// run forever in the background even while hidden meant it kept invalidating
// s_mouth every 150ms with nobody watching, which was a plausible contributor
// to leftover "residue" pixels around exactly the moment visibility toggled.
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
    // Pulse animation is started/stopped per-talking-turn in show_talking(),
    // not here -- see the note on start_mouth_pulse().

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
// two-refresh-cycle staggering show_eyes/show_status_text use. The pulse
// animation itself only runs while talking=true (see start_mouth_pulse) --
// stopping it here and resetting to the closed height means there's no
// stray mid-pulse frame left behind if a redraw around this transition ever
// fails to queue.
static void show_talking(bool talking)
{
    bsp_display_lock(-1);
    if (talking) {
        lv_obj_clear_flag(s_mouth, LV_OBJ_FLAG_HIDDEN);
        start_mouth_pulse();
    } else {
        lv_anim_delete(s_mouth, mouth_pulse_cb);
        lv_obj_set_height(s_mouth, MOUTH_HEIGHT_CLOSED);
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

// Reassembles Gemini reply audio (REPLY_PCM_PACKET_TYPE) across however many
// WEBSOCKET_EVENT_DATA fragments one logical WS message gets split into --
// these chunks are a few KB of 16kHz PCM per streamed part, comfortably over
// the client's default 1KB buffer_size (bumped in start_websocket(), but
// reassembly here makes correctness independent of that tuning). Complete
// frames get copied into a fresh heap buffer and queued for playback_task;
// dropped (with a warning) if a frame ever exceeds REPLY_AUDIO_MAX_BYTES.
static void handle_ws_binary(const esp_websocket_event_data_t *data)
{
    static int s_received = 0;
    static bool s_overflowed = false;

    if (data->op_code != WS_TRANSPORT_OPCODES_BINARY || data->data_len <= 0) {
        return;
    }
    if (s_reassembly_buf == NULL) {
        return; // init_reply_audio_buffer() hasn't run yet
    }

    if (data->payload_offset == 0) {
        s_received = 0;
        s_overflowed = false;
    }

    if (s_overflowed) {
        return;
    }

    if (s_received + data->data_len > REPLY_AUDIO_MAX_BYTES) {
        ESP_LOGW(TAG, "reply audio frame too large (%d bytes), dropping", data->payload_len);
        s_overflowed = true;
        return;
    }

    memcpy(&s_reassembly_buf[s_received], data->data_ptr, data->data_len);
    s_received += data->data_len;

    if (s_received != data->payload_len) {
        return; // wait for more fragments
    }

    if (s_received < 1 || s_reassembly_buf[0] != REPLY_PCM_PACKET_TYPE) {
        return;
    }

    size_t audio_len = s_received - 1;
    playback_chunk_t chunk = {
        .data = malloc(audio_len),
        .len = audio_len,
    };
    if (chunk.data == NULL) {
        ESP_LOGW(TAG, "playback chunk alloc failed (%d bytes)", (int)audio_len);
        return;
    }
    memcpy(chunk.data, &s_reassembly_buf[1], audio_len);

    if (xQueueSend(s_playback_queue, &chunk, 0) != pdTRUE) {
        ESP_LOGW(TAG, "playback queue full, dropping chunk");
        free(chunk.data);
    }
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
    case WEBSOCKET_EVENT_DATA: {
        const esp_websocket_event_data_t *data = (const esp_websocket_event_data_t *)event_data;
        if (data->op_code == WS_TRANSPORT_OPCODES_TEXT) {
            handle_ws_text(data);
        } else if (data->op_code == WS_TRANSPORT_OPCODES_BINARY) {
            handle_ws_binary(data);
        }
        break;
    }
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

// Dedicated task drains s_playback_queue and writes to the speaker codec, so
// a slow/blocking hardware write never stalls the WebSocket client's own
// task (which is what enqueues chunks from handle_ws_binary).
//
// The speaker's I2S TX channel is opened lazily (on the first chunk of a
// reply) and closed again after PLAYBACK_IDLE_CLOSE_MS of no new chunks,
// rather than held open continuously from boot. Confirmed on hardware: an
// always-open TX channel continuously clocks (even silence) via DMA, and
// that constant activity contended with the display's own SPI DMA queue
// badly enough to corrupt the whole screen permanently, not just during
// playback -- keeping it open only during actual replies confines that
// contention to the (much smaller) windows when audio is really playing.
#define PLAYBACK_IDLE_CLOSE_MS 800

static void playback_task(void *arg)
{
    playback_chunk_t chunk;
    bool speaker_open = false;

    for (;;) {
        if (xQueueReceive(s_playback_queue, &chunk, pdMS_TO_TICKS(PLAYBACK_IDLE_CLOSE_MS)) != pdTRUE) {
            if (speaker_open) {
                esp_codec_dev_close(s_speaker_dev);
                speaker_open = false;
            }
            continue;
        }

        if (!speaker_open) {
            esp_codec_dev_sample_info_t fs = {
                .sample_rate = SPEAKER_SAMPLE_RATE,
                .channel = SPEAKER_CHANNELS,
                .bits_per_sample = SPEAKER_BITS,
            };
            int open_ret = esp_codec_dev_open(s_speaker_dev, &fs);
            if (open_ret != ESP_CODEC_DEV_OK) {
                ESP_LOGW(TAG, "speaker open failed: %d", open_ret);
                free(chunk.data);
                continue;
            }
            speaker_open = true;

            // Must be set AFTER open (ESP_CODEC_DEV_WRONG_STATE otherwise).
            // Never set at all before this -- almost certainly why nothing
            // was audible despite writes succeeding without error.
            int vol_ret = esp_codec_dev_set_out_vol(s_speaker_dev, 80);
            if (vol_ret != ESP_CODEC_DEV_OK) {
                ESP_LOGW(TAG, "speaker set_out_vol failed: %d", vol_ret);
            }
        }

        int ret = esp_codec_dev_write(s_speaker_dev, chunk.data, (int)chunk.len);
        if (ret != ESP_CODEC_DEV_OK) {
            ESP_LOGW(TAG, "speaker write failed: %d", ret);
        }
        free(chunk.data);
    }
}

// The 32KB reassembly buffer must NOT be a plain `static` array -- that
// lands in internal DIRAM, which the display's SPI DMA queue also draws
// from (a scarce, shared pool distinct from the 8MB of PSRAM). A static
// array here was confirmed (via `idf.py size` + a bisection test) to push
// DIRAM usage high enough to make the display driver's own DMA-capable
// allocations fail continuously -- corrupting the whole screen, not just
// during playback. PSRAM has no such contention.
static void init_reply_audio_buffer(void)
{
    s_reassembly_buf = heap_caps_malloc(REPLY_AUDIO_MAX_BYTES, MALLOC_CAP_SPIRAM);
    assert(s_reassembly_buf != NULL);
}

static void start_speaker(void)
{
    // Just gets the codec handle -- doesn't touch I2S yet. The actual
    // esp_codec_dev_open() (which enables the I2S TX channel) happens lazily
    // in playback_task, only while a reply is actually playing.
    s_speaker_dev = bsp_audio_codec_speaker_init();
    assert(s_speaker_dev);

    s_playback_queue = xQueueCreate(8, sizeof(playback_chunk_t));
    assert(s_playback_queue != NULL);

    xTaskCreatePinnedToCore(playback_task, "playback", 4096, NULL, 5, NULL, 0);
}

// TEMPORARY: plays a synthesized 440Hz tone through the exact same queue ->
// lazy-open -> esp_codec_dev_write() path real Gemini audio uses, but with
// no network/backend/Gemini involved at all -- isolates whether the speaker
// hardware/wiring/codec path itself works, independent of everything else.
// Remove once speaker output is confirmed working end-to-end.
static void play_test_tone(void)
{
    const int duration_ms = 1000;
    const int n_samples = SPEAKER_SAMPLE_RATE * duration_ms / 1000;
    int16_t *tone = heap_caps_malloc(n_samples * sizeof(int16_t), MALLOC_CAP_SPIRAM);
    if (tone == NULL) {
        ESP_LOGW(TAG, "test tone alloc failed");
        return;
    }
    const float freq = 440.0f;
    const float two_pi = 6.28318530718f;
    for (int i = 0; i < n_samples; i++) {
        tone[i] = (int16_t)(8000.0f * sinf(two_pi * freq * i / SPEAKER_SAMPLE_RATE));
    }

    playback_chunk_t chunk = {
        .data = (uint8_t *)tone,
        .len = n_samples * sizeof(int16_t),
    };
    if (xQueueSend(s_playback_queue, &chunk, pdMS_TO_TICKS(100)) != pdTRUE) {
        ESP_LOGW(TAG, "test tone enqueue failed");
        free(tone);
    } else {
        ESP_LOGI(TAG, "test tone queued (440Hz, 1s) -- listen for it now");
    }
}

static void start_websocket(void)
{
    esp_websocket_client_config_t ws_cfg = {
        .uri = CONFIG_MRGREENY_BACKEND_WS_URL,
        // Default is 1KB; Gemini reply audio chunks are several KB. handle_ws_binary
        // reassembles fragments regardless, but a bigger buffer means fewer of them
        // in practice.
        .buffer_size = 8192,
    };
    s_ws_client = esp_websocket_client_init(&ws_cfg);
    esp_websocket_register_events(s_ws_client, WEBSOCKET_EVENT_ANY, ws_event_handler, NULL);
    esp_websocket_client_start(s_ws_client);
}

// The SPI transaction queue depth (CONFIG_BSP_LCD_TRANS_QUEUE_DEPTH) is
// already at the BSP Kconfig's enforced max of 10 -- not a lever we can
// pull. LVGL's own task defaults to no core affinity (can land on either
// core), same as our mic/network/audio tasks and Wi-Fi/LWIP's internal
// tasks (which ESP-IDF conventionally keeps on core 0) -- if LVGL's task
// happens to share a core with a burst of network/audio activity, its own
// queue-servicing can fall behind, which is a real, plausible contributor
// to the SPI NO_MEM bursts seen during sustained mic-streaming/Gemini-Live
// activity. Pinning LVGL to core 1 and our own tasks to core 0 removes that
// specific contention path (though see the caveat where this is used below
// -- it reduces one real source of contention, it's not guaranteed to be
// the only one).
static lv_display_t *start_display(void)
{
    bsp_display_cfg_t cfg = {
        .lv_adapter_cfg = ESP_LV_ADAPTER_DEFAULT_CONFIG(),
        .rotation = ESP_LV_ADAPTER_ROTATE_0,
        .tear_avoid_mode = ESP_LV_ADAPTER_TEAR_AVOID_MODE_NONE,
        .touch_flags = {
            .swap_xy = 0,
            .mirror_x = 1,
            .mirror_y = 1,
        },
    };
    cfg.lv_adapter_cfg.task_core_id = 1;
    return bsp_display_start_with_config(&cfg);
}

void app_main(void)
{
    ESP_ERROR_CHECK(nvs_flash_init());
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());

    start_display();
    setup_ui();

    // Blocks until Wi-Fi station is up. SSID/password are set via
    // `idf.py menuconfig` -> Example Connection Configuration.
    ESP_ERROR_CHECK(example_connect());

    init_reply_audio_buffer();
    start_websocket();
    start_mic();
    start_speaker();
    play_test_tone();  // TEMPORARY: remove once speaker output is confirmed working

    xTaskCreatePinnedToCore(mic_stream_task, "mic_stream", 4096, NULL, 5, NULL, 0);
}
