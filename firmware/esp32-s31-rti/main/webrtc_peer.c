#include "webrtc_peer.h"

#include <stdbool.h>
#include <inttypes.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "app_state.h"
#include "app_brand.h"
#include "board_display.h"
#include "cJSON.h"
#include "esp_aec.h"
#include "esp_attr.h"
#include "esp_log.h"
#include "esp_heap_caps.h"
#include "esp_peer.h"
#include "esp_peer_default.h"
#include "esp_timer.h"
#include "esp_opus_dec.h"
#include "esp_opus_enc.h"
#include "esp_vad.h"
#if CONFIG_MIKU_RTI_OFFICIAL_CAPTURE_AEC
#include "esp_capture.h"
#include "esp_capture_audio_src_if.h"
#include "esp_capture_defaults.h"
#endif
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "freertos/idf_additions.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/stream_buffer.h"
#include "freertos/task.h"

#define RTC_EVENT_LOCAL_SDP BIT0
#define RTC_EVENT_DATA_OPEN BIT1
#define RTC_EVENT_LOOP_DONE BIT2
#define RTC_EVENT_FAILED BIT3
#define RTC_EVENT_MIC_DONE BIT4
#define RTC_EVENT_PLAYBACK_DONE BIT5
#define RTC_EVENT_DATA_DONE BIT6
#define RTC_EVENT_CONNECTED BIT7
#define RTC_SIGNAL_TIMEOUT_MS 25000
#define RTC_EVENT_JSON_MAX (12 * 1024)
#define RTC_DATA_EVENT_QUEUE_LENGTH 32
#define RTC_DATA_EVENT_STACK_SIZE (20 * 1024)
#define RTC_DATA_EVENT_TASK_PRIORITY 5
#define RTC_PEER_TASK_PRIORITY 6
#define RTC_STREAMING_UI_INTERVAL_US 80000
#define RTC_PIPELINE_STATS_INTERVAL_US 5000000
#define RTC_TRANSCRIPT_MAX 1024
#define RTC_OPUS_PACKET_MAX 1500
#define RTC_PCM_BUFFER_MAX 4096
// Opus packets may legally contain 120 ms; decode all of it before rendering.
#define RTC_GROUP_OPUS_FRAME_SAMPLES 1920
#define RTC_GROUP_PCM_BUFFER_MAX (RTC_GROUP_OPUS_FRAME_SAMPLES * 2 * sizeof(int16_t))
#define RTC_GROUP_PLAYBACK_QUEUE_FRAMES 6
#define RTC_GROUP_MAX_QUEUE_AGE_US 200000
#define RTC_CAPTURE_FRAME_SAMPLES 320
#define RTC_CAPTURE_CHANNELS 2
#define RTC_OPUS_FRAME_SAMPLES 320
#define RTC_CAPTURE_FRAME_BYTES \
    (RTC_CAPTURE_FRAME_SAMPLES * RTC_CAPTURE_CHANNELS * sizeof(int16_t))
#define RTC_AEC_MAX_FRAME_SAMPLES 1024
#define RTC_AEC_ACCUMULATOR_SAMPLES (RTC_AEC_MAX_FRAME_SAMPLES + RTC_CAPTURE_FRAME_SAMPLES)
#define RTC_AEC_REFERENCE_FRAMES 8
#define RTC_AEC_MODE AEC_MODE_FD_LOW_COST
#define RTC_AEC_REFERENCE_DELAY_MS 40
#define RTC_AEC_REFERENCE_DELAY_SAMPLES \
    (16000 * RTC_AEC_REFERENCE_DELAY_MS / 1000)
#define RTC_AEC_REFERENCE_MAX_BUFFER_MS 80
#define RTC_AEC_REFERENCE_MAX_BUFFER_BYTES \
    (16000 * RTC_AEC_REFERENCE_MAX_BUFFER_MS / 1000 * sizeof(int16_t))
#define RTC_AEC_REFERENCE_TRIM_SAMPLES 16
#define RTC_MIC_STACK_SIZE (48 * 1024)
#define RTC_PLAYBACK_STACK_SIZE (28 * 1024)
#define RTC_PLAYBACK_QUEUE_FRAMES 24
#define RTC_PLAYBACK_PREBUFFER_FRAMES 12
#define RTC_PLAYBACK_TASK_PRIORITY 8
#define RTC_STACK_LOG_INTERVAL 250
// Keep the playback window continuous across 20 ms writes, but release the
// microphone quickly enough to retain a short reply immediately after playout.
#define RTC_PLAYBACK_TAIL_MS 40
#define RTC_BARGE_IN_HOLD_MS 300
#define RTC_BARGE_IN_MIN_CLEAN_MEAN 180
#define RTC_BARGE_IN_MAX_CLEAN_MEAN 700
#define RTC_BARGE_IN_NOISE_MULTIPLIER 3
#define RTC_BARGE_IN_NOISE_MARGIN 32
#define RTC_BARGE_IN_PEAK_MULTIPLIER 3
#define RTC_BARGE_IN_MIN_RAW_MEAN 400
#define RTC_BARGE_IN_MAX_RAW_MEAN 1400
#define RTC_BARGE_IN_RAW_MULTIPLIER 2
#define RTC_BARGE_IN_RAW_MARGIN 32
#define RTC_BARGE_IN_MIN_FRAMES 3
#define RTC_BARGE_IN_WARMUP_MS 800
#define RTC_IDLE_YIELD_INTERVAL 100
#define RTC_PLAYBACK_STATS_INTERVAL 100
#define RTC_AUDIO_PACKET_GAP_WARN_US 35000
#define RTC_PLAYBACK_PLC_MAX_FRAMES 3
#define RTC_DIAG_REFERENCE_SAMPLES 8192
#define RTC_DIAG_LAG_STEP_SAMPLES 160
#define RTC_DIAG_LAG_COUNT 21
#define RTC_DIAG_REPORT_FRAMES 50

static const char *TAG = "webrtc_peer";

#if CONFIG_MIKU_RTI_CAPTURE_DIAGNOSTIC
typedef struct {
    int16_t reference[RTC_DIAG_REFERENCE_SAMPLES];
    size_t reference_pos;
    uint64_t sample_count;
    uint64_t mic0_abs;
    uint64_t mic1_abs;
    uint64_t mic0_sq;
    uint64_t mic1_sq;
    int64_t mic_cross;
    int64_t ref_cross0[RTC_DIAG_LAG_COUNT];
    int64_t ref_cross1[RTC_DIAG_LAG_COUNT];
    uint64_t ref_sq[RTC_DIAG_LAG_COUNT];
    uint32_t frames;
    bool active;
} capture_diag_t;
#endif

// esp_audio_codec bundles libopus but its wrapper rejects an empty input
// packet. Use the public decoder API so packet-loss concealment can be
// requested with data=NULL as specified by Opus.
typedef struct OpusDecoder OpusDecoder;
extern OpusDecoder *opus_decoder_create(int32_t sample_rate, int channels, int *error);
extern int opus_decode(OpusDecoder *decoder, const unsigned char *data, int32_t len,
                       int16_t *pcm, int frame_size, int decode_fec);
extern void opus_decoder_destroy(OpusDecoder *decoder);

typedef struct {
    size_t size;
    int64_t received_us;
    uint32_t pts;
    uint8_t data[RTC_OPUS_PACKET_MAX];
} opus_packet_t;

typedef struct {
    size_t size;
    uint8_t data[];
} data_event_t;

typedef struct {
    esp_peer_handle_t peer;
    EventGroupHandle_t events;
    SemaphoreHandle_t lock;
    TaskHandle_t loop_task;
    TaskHandle_t data_task;
    TaskHandle_t mic_task;
    TaskHandle_t playback_task;
    bool running;
    bool audio_running;
    bool data_channel_requested;
    bool group_mode;
    EventBits_t retired_workers;
    char *local_sdp;
    uint16_t data_stream_id;
    char user_text[RTC_TRANSCRIPT_MAX];
    char assistant_text[RTC_TRANSCRIPT_MAX];
    board_audio_t audio;
    void *opus_encoder;
    OpusDecoder *opus_decoder;
    int encoder_input_size;
    int encoder_output_size;
    QueueHandle_t playback_queue;
    QueueHandle_t data_queue;
    StreamBufferHandle_t reference_stream;
#if CONFIG_MIKU_RTI_OFFICIAL_CAPTURE_AEC
    esp_capture_audio_src_if_t *capture_source;
#endif
    aec_handle_t *aec;
    int aec_frame_samples;
    bool aec_enabled;
    uint32_t reference_overflows;
    uint32_t playback_overflows;
    uint32_t playback_input_gaps;
    int64_t last_audio_packet_us;
    uint32_t last_audio_pts;
    bool has_last_audio_pts;
    int64_t last_assistant_render_us;
    bool assistant_render_pending;
    uint32_t data_event_drops;
    bool reference_primed;
    volatile TickType_t playback_guard_until_tick;
} rtc_peer_ctx_t;

static rtc_peer_ctx_t rtc;
// Short critical sections protect only gate values and aggregate counters, never
// codec, I/O, allocation, or esp_peer calls. A packet admitted before a release
// may already be in transport; every subsequent packet is checked again.
static portMUX_TYPE group_state_lock = portMUX_INITIALIZER_UNLOCKED;
static bool group_mode_active;
static bool group_transport_connected;
static bool group_audio_ready;
static bool group_send_enabled;
static int64_t group_send_expires_at_us;
static uint32_t group_send_epoch;
static webrtc_group_stats_t group_stats;

void webrtc_peer_group_set_sending(bool enabled, int64_t expires_at_us)
{
    const int64_t now = esp_timer_get_time();
    portENTER_CRITICAL(&group_state_lock);
    const bool allow = enabled && group_mode_active && group_transport_connected &&
                       group_audio_ready && expires_at_us > now;
    if (allow != group_send_enabled) {
        ++group_send_epoch;
    }
    group_send_enabled = allow;
    group_send_expires_at_us = allow ? expires_at_us : 0;
    portEXIT_CRITICAL(&group_state_lock);
}

static bool group_sending_allowed(uint32_t *epoch)
{
    const int64_t now = esp_timer_get_time();
    portENTER_CRITICAL(&group_state_lock);
    if (group_send_enabled && group_send_expires_at_us <= now) {
        group_send_enabled = false;
        group_send_expires_at_us = 0;
        ++group_send_epoch;
    }
    const bool allowed = group_mode_active && group_transport_connected &&
                         group_audio_ready && group_send_enabled;
    if (epoch != NULL) {
        *epoch = group_send_epoch;
    }
    portEXIT_CRITICAL(&group_state_lock);
    return allowed;
}

bool webrtc_peer_group_connected(void)
{
    portENTER_CRITICAL(&group_state_lock);
    const bool connected = group_mode_active && group_transport_connected && group_audio_ready;
    portEXIT_CRITICAL(&group_state_lock);
    return connected;
}

void webrtc_peer_group_get_stats(webrtc_group_stats_t *stats)
{
    if (stats == NULL) {
        return;
    }
    portENTER_CRITICAL(&group_state_lock);
    *stats = group_stats;
    portEXIT_CRITICAL(&group_state_lock);
}

static void group_mark_unavailable(void)
{
    portENTER_CRITICAL(&group_state_lock);
    group_audio_ready = false;
    group_transport_connected = false;
    group_send_enabled = false;
    group_send_expires_at_us = 0;
    ++group_send_epoch;
    portEXIT_CRITICAL(&group_state_lock);
}

static void finish_worker(EventBits_t done)
{
    // EventGroupSetBits may unblock the controller before returning. Publish
    // retirement only after it returns, so the controller cannot delete an
    // EventGroup that a completing worker still accesses.
    xEventGroupSetBits(rtc.events, done);
    portENTER_CRITICAL(&group_state_lock);
    rtc.retired_workers |= done;
    portEXIT_CRITICAL(&group_state_lock);
    // No access to the rtc context after publishing retirement.
    vTaskDeleteWithCaps(NULL);
}

static float pcm_rms(const int16_t *pcm, size_t samples)
{
    if (samples == 0) {
        return 0;
    }
    uint64_t squares = 0;
    for (size_t index = 0; index < samples; ++index) {
        const int32_t sample = pcm[index];
        squares += (uint64_t)((int64_t)sample * sample);
    }
    return (float)(sqrt((double)squares / samples) / 32768.0);
}

static void *alloc_audio_workspace(size_t bytes)
{
    // Group buffers can be 120 ms long and must not consume the scarce
    // internal/DMA heap merely because they fall below malloc's 16 KB cutoff.
    // The codec's synchronous I2S read/write copies between these CPU buffers
    // and driver-owned DMA buffers. AEC's DMA_ATTR arrays remain internal.
    if (rtc.group_mode) {
        return heap_caps_malloc(bytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    }
    return malloc(bytes);
}

static aec_handle_t *persistent_aec;
static int persistent_aec_frame_samples;
static uint32_t peer_cycle;
static DMA_ATTR int16_t aec_dma_mic[RTC_AEC_MAX_FRAME_SAMPLES]
    __attribute__((aligned(16)));
static DMA_ATTR int16_t aec_dma_reference[RTC_AEC_MAX_FRAME_SAMPLES]
    __attribute__((aligned(16)));
static DMA_ATTR int16_t aec_dma_output[RTC_AEC_MAX_FRAME_SAMPLES]
    __attribute__((aligned(16)));

#if CONFIG_MIKU_RTI_CAPTURE_DIAGNOSTIC
static void capture_diag_reset(capture_diag_t *diag)
{
    memset(diag, 0, sizeof(*diag));
}

static double normalized_correlation(int64_t cross, uint64_t lhs_sq, uint64_t rhs_sq)
{
    if (lhs_sq == 0 || rhs_sq == 0) {
        return 0.0;
    }
    return (double)cross / sqrt((double)lhs_sq * (double)rhs_sq);
}

static void capture_diag_process(capture_diag_t *diag, const int16_t *capture,
                                 const int16_t *reference, bool playback_window)
{
    if (!playback_window) {
        if (diag->active) {
            capture_diag_reset(diag);
        }
        return;
    }
    diag->active = true;
    for (size_t index = 0; index < RTC_CAPTURE_FRAME_SAMPLES; ++index) {
        const int32_t mic0 = capture[index * RTC_CAPTURE_CHANNELS];
        const int32_t mic1 = capture[index * RTC_CAPTURE_CHANNELS + 1];
        const int32_t ref = reference[index];
        diag->reference[diag->reference_pos] = (int16_t)ref;
        for (size_t lag_index = 0; lag_index < RTC_DIAG_LAG_COUNT; ++lag_index) {
            const size_t lag = lag_index * RTC_DIAG_LAG_STEP_SAMPLES;
            const size_t ref_index =
                (diag->reference_pos + RTC_DIAG_REFERENCE_SAMPLES - lag) %
                RTC_DIAG_REFERENCE_SAMPLES;
            const int32_t delayed_ref = diag->reference[ref_index];
            diag->ref_cross0[lag_index] += (int64_t)mic0 * delayed_ref;
            diag->ref_cross1[lag_index] += (int64_t)mic1 * delayed_ref;
            diag->ref_sq[lag_index] += (uint64_t)((int64_t)delayed_ref * delayed_ref);
        }
        diag->reference_pos =
            (diag->reference_pos + 1) % RTC_DIAG_REFERENCE_SAMPLES;
        diag->mic0_abs += mic0 < 0 ? (uint32_t)-mic0 : (uint32_t)mic0;
        diag->mic1_abs += mic1 < 0 ? (uint32_t)-mic1 : (uint32_t)mic1;
        diag->mic0_sq += (uint64_t)((int64_t)mic0 * mic0);
        diag->mic1_sq += (uint64_t)((int64_t)mic1 * mic1);
        diag->mic_cross += (int64_t)mic0 * mic1;
        ++diag->sample_count;
    }
    ++diag->frames;
    if (diag->frames < RTC_DIAG_REPORT_FRAMES) {
        return;
    }

    size_t best0 = 0;
    size_t best1 = 0;
    double best_corr0 = 0.0;
    double best_corr1 = 0.0;
    for (size_t lag_index = 0; lag_index < RTC_DIAG_LAG_COUNT; ++lag_index) {
        const double corr0 = normalized_correlation(
            diag->ref_cross0[lag_index], diag->mic0_sq, diag->ref_sq[lag_index]);
        const double corr1 = normalized_correlation(
            diag->ref_cross1[lag_index], diag->mic1_sq, diag->ref_sq[lag_index]);
        if (fabs(corr0) > fabs(best_corr0)) {
            best_corr0 = corr0;
            best0 = lag_index;
        }
        if (fabs(corr1) > fabs(best_corr1)) {
            best_corr1 = corr1;
            best1 = lag_index;
        }
    }
    const double mic_corr = normalized_correlation(
        diag->mic_cross, diag->mic0_sq, diag->mic1_sq);
    ESP_LOGI(TAG,
             "CAPTURE_DIAG speaker-only raw: samples=%u mean_abs(ch0/ch1)=%u/%u "
             "corr(ch0,ch1)=%.3f playback_best(ch0)=%.3f@%ums "
             "playback_best(ch1)=%.3f@%ums",
             (unsigned)diag->sample_count,
             (unsigned)(diag->mic0_abs / diag->sample_count),
             (unsigned)(diag->mic1_abs / diag->sample_count), mic_corr,
             best_corr0, (unsigned)(best0 * 10), best_corr1,
             (unsigned)(best1 * 10));
    capture_diag_reset(diag);
}
#endif

#if CONFIG_MIKU_RTI_OFFICIAL_CAPTURE_AEC
static void capture_thread_scheduler(const char *name,
                                     esp_capture_thread_schedule_cfg_t *cfg)
{
    cfg->stack_in_ext = true;
    if (strcmp(name, "buffer_in") == 0) {
        cfg->stack_size = 8 * 1024;
        cfg->priority = 10;
        cfg->core_id = 0;
    }
}

static esp_err_t official_capture_aec_start(const board_audio_t *audio)
{
    esp_capture_set_thread_scheduler(capture_thread_scheduler);

    // board_audio_init opens the ADC for the legacy direct-read path. The
    // official source owns the ADC lifecycle and reopens it with the S31
    // four-slot/two-selected-channel layout expected by ESP-SR ("MR").
    esp_codec_dev_close(audio->microphone);
    esp_capture_audio_aec_src_cfg_t cfg = {
        .mic_layout = "MR",
        .record_handle = audio->microphone,
        .channel = 4,
        .channel_mask = 1 | 2,
        .data_on_vad = false,
    };
    rtc.capture_source = esp_capture_new_audio_aec_src(&cfg);
    if (rtc.capture_source == NULL) {
        ESP_LOGE(TAG, "official AEC capture source allocation failed");
        return ESP_ERR_NO_MEM;
    }

    esp_capture_audio_info_t requested = {
        .format_id = ESP_CAPTURE_FMT_ID_PCM,
        .sample_rate = 16000,
        .channel = 1,
        .bits_per_sample = 16,
    };
    esp_capture_audio_info_t negotiated = {0};
    if (rtc.capture_source->open(rtc.capture_source) != ESP_CAPTURE_ERR_OK ||
        rtc.capture_source->negotiate_caps(rtc.capture_source, &requested,
                                           &negotiated) != ESP_CAPTURE_ERR_OK ||
        rtc.capture_source->start(rtc.capture_source) != ESP_CAPTURE_ERR_OK) {
        ESP_LOGE(TAG, "official AEC capture source start failed");
        rtc.capture_source->stop(rtc.capture_source);
        rtc.capture_source->close(rtc.capture_source);
        free(rtc.capture_source);
        rtc.capture_source = NULL;
        return ESP_FAIL;
    }
    ESP_LOGI(TAG,
             "official S31 AEC capture active: layout=MR input_slots=4 mask=0x03 "
             "output=%uHz/%uch/%ubit",
             negotiated.sample_rate, negotiated.channel, negotiated.bits_per_sample);
    return ESP_OK;
}

static void official_capture_aec_stop(void)
{
    if (rtc.capture_source == NULL) {
        return;
    }
    rtc.capture_source->stop(rtc.capture_source);
    rtc.capture_source->close(rtc.capture_source);
    free(rtc.capture_source);
    rtc.capture_source = NULL;
    ESP_LOGI(TAG, "official S31 AEC capture stopped");
}
#endif

static void log_heap_state(const char *phase, uint32_t cycle)
{
    ESP_LOGI(TAG,
             "RTC heap cycle=%" PRIu32 " phase=%s internal=%u largest_internal=%u "
             "dma=%u largest_dma=%u psram=%u largest_psram=%u",
             cycle, phase,
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT),
             (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT),
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_DMA | MALLOC_CAP_8BIT),
             (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_DMA | MALLOC_CAP_8BIT),
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT),
             (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
}

esp_err_t webrtc_peer_prepare(void)
{
    const int ret = esp_peer_pre_generate_cert();
    if (ret != ESP_PEER_ERR_NONE) {
        ESP_LOGE(TAG, "DTLS certificate pre-generation failed: %d", ret);
        return ESP_FAIL;
    }
    ESP_LOGI(TAG, "DTLS certificate pre-generated for Session reuse");
    return ESP_OK;
}

static size_t utf8_prefix_length(const char *text, size_t maximum)
{
    size_t offset = 0;
    while (text[offset] != '\0' && offset < maximum) {
        const unsigned char lead = (unsigned char)text[offset];
        size_t codepoint = 1;
        if ((lead & 0xE0) == 0xC0) {
            codepoint = 2;
        } else if ((lead & 0xF0) == 0xE0) {
            codepoint = 3;
        } else if ((lead & 0xF8) == 0xF0) {
            codepoint = 4;
        }
        if (offset + codepoint > maximum) {
            break;
        }
        bool valid = codepoint == 1;
        if (codepoint > 1) {
            valid = true;
            for (size_t index = 1; index < codepoint; ++index) {
                if (text[offset + index] == '\0' ||
                    ((unsigned char)text[offset + index] & 0xC0) != 0x80) {
                    valid = false;
                    break;
                }
            }
        }
        if (!valid) {
            codepoint = 1;
        }
        offset += codepoint;
    }
    return offset;
}

static void set_text(char *destination, size_t capacity, const char *text)
{
    destination[0] = '\0';
    if (text == NULL) {
        return;
    }
    const size_t length = utf8_prefix_length(text, capacity - 1);
    memcpy(destination, text, length);
    destination[length] = '\0';
}

static void append_text(char *destination, size_t capacity, const char *text)
{
    if (text == NULL || text[0] == '\0') {
        return;
    }
    const size_t used = strlen(destination);
    if (used >= capacity - 1) {
        return;
    }
    const size_t length = utf8_prefix_length(text, capacity - 1 - used);
    memcpy(destination + used, text, length);
    destination[used + length] = '\0';
}

static const char *json_string(const cJSON *root, const char *name)
{
    const cJSON *value = cJSON_GetObjectItemCaseSensitive(root, name);
    return cJSON_IsString(value) && value->valuestring != NULL ? value->valuestring : NULL;
}

static void render_user_delta(const cJSON *root)
{
    append_text(rtc.user_text, sizeof(rtc.user_text), json_string(root, "text"));
    append_text(rtc.user_text, sizeof(rtc.user_text), json_string(root, "stash"));
    append_text(rtc.user_text, sizeof(rtc.user_text), json_string(root, "delta"));
}

static void flush_pending_assistant_transcript(void)
{
    if (!rtc.assistant_render_pending || rtc.assistant_text[0] == '\0') {
        return;
    }
    const int64_t now_us = esp_timer_get_time();
    if (rtc.last_assistant_render_us != 0 &&
        now_us - rtc.last_assistant_render_us < RTC_STREAMING_UI_INTERVAL_US) {
        return;
    }
    const int64_t render_start_us = now_us;
    board_display_set_transcript(BOARD_TRANSCRIPT_ASSISTANT,
                                 rtc.assistant_text, true);
    const int64_t render_us = esp_timer_get_time() - render_start_us;
    rtc.last_assistant_render_us = esp_timer_get_time();
    rtc.assistant_render_pending = false;
    if (render_us >= 20000) {
        ESP_LOGW(TAG, "streaming transcript render slow: %" PRId64 "us",
                 render_us);
    }
}

static void handle_realtime_event(const uint8_t *data, size_t size)
{
    if (data == NULL || size == 0 || size > RTC_EVENT_JSON_MAX) {
        return;
    }
    char *json = malloc(size + 1);
    if (json == NULL) {
        return;
    }
    memcpy(json, data, size);
    json[size] = '\0';
    cJSON *root = cJSON_Parse(json);
    free(json);
    if (root == NULL) {
        return;
    }

    const char *type = json_string(root, "type");
    if (type == NULL) {
        cJSON_Delete(root);
        return;
    }
    const bool user_delta =
        strcmp(type, "conversation.item.input_audio_transcription.delta") == 0;
    const bool assistant_delta =
        strcmp(type, "response.audio_transcript.delta") == 0 ||
        strcmp(type, "response.output_audio_transcript.delta") == 0 ||
        strcmp(type, "response.text.delta") == 0;
    if (user_delta || assistant_delta) {
        ESP_LOGD(TAG, "realtime event: %s", type);
    } else {
        ESP_LOGI(TAG, "realtime event: %s", type);
    }
    if (user_delta) {
        render_user_delta(root);
    } else if (strcmp(type, "conversation.item.input_audio_transcription.completed") == 0) {
        const char *transcript = json_string(root, "transcript");
        if (transcript != NULL) {
            set_text(rtc.user_text, sizeof(rtc.user_text), transcript);
            ESP_LOGI(TAG, "user transcript done: %s", transcript);
        }
        board_display_set_transcript(BOARD_TRANSCRIPT_USER, rtc.user_text, false);
    } else if (strcmp(type, "response.created") == 0) {
        rtc.assistant_text[0] = '\0';
        rtc.last_assistant_render_us = 0;
        rtc.assistant_render_pending = false;
        board_display_set_transcript(BOARD_TRANSCRIPT_ASSISTANT, "", true);
        rti_app_set_state(RTI_APP_STATE_AGENT_SPEAKING);
        board_display_set_conversation_state(BOARD_CONVERSATION_ACTIVE, "Assistant responding", true);
    } else if (assistant_delta) {
        const char *delta = json_string(root, "delta");
        append_text(rtc.assistant_text, sizeof(rtc.assistant_text), delta);
        rtc.assistant_render_pending = true;
        flush_pending_assistant_transcript();
    } else if (strcmp(type, "response.audio_transcript.done") == 0 ||
               strcmp(type, "response.output_audio_transcript.done") == 0 ||
               strcmp(type, "response.text.done") == 0) {
        const char *transcript = json_string(root, "transcript");
        if (transcript == NULL) {
            transcript = json_string(root, "text");
        }
        if (transcript != NULL) {
            set_text(rtc.assistant_text, sizeof(rtc.assistant_text), transcript);
            ESP_LOGI(TAG, "assistant transcript done: %s", transcript);
        }
        board_display_set_transcript(BOARD_TRANSCRIPT_ASSISTANT, rtc.assistant_text, false);
        rtc.last_assistant_render_us = esp_timer_get_time();
        rtc.assistant_render_pending = false;
    } else if (strcmp(type, "response.done") == 0) {
        board_display_commit_transcript_turn();
        rti_app_set_state(RTI_APP_STATE_LISTENING);
        board_display_set_conversation_state(BOARD_CONVERSATION_ACTIVE, "Listening", true);
    } else if (strcmp(type, "input_audio_buffer.speech_started") == 0) {
        rtc.user_text[0] = '\0';
        board_display_set_transcript(BOARD_TRANSCRIPT_USER, "", false);
        rti_app_set_state(RTI_APP_STATE_USER_SPEAKING);
        board_display_set_conversation_state(BOARD_CONVERSATION_ACTIVE, "Listening to you", true);
    } else if (strcmp(type, "input_audio_buffer.speech_stopped") == 0) {
        board_display_set_conversation_state(BOARD_CONVERSATION_ACTIVE, "Understanding", true);
    } else if (strcmp(type, "error") == 0) {
        ESP_LOGW(TAG, "provider sent a realtime error event");
        board_display_set_conversation_state(BOARD_CONVERSATION_ERROR,
                                             "Realtime provider reported an error", true);
    }
    cJSON_Delete(root);
}

static int peer_state_handler(esp_peer_state_t state, void *ctx)
{
    (void)ctx;
    ESP_LOGI(TAG, "peer state=%d", state);
    if (rtc.group_mode) {
        if (state == ESP_PEER_STATE_CONNECTED) {
            portENTER_CRITICAL(&group_state_lock);
            group_transport_connected = true;
            portEXIT_CRITICAL(&group_state_lock);
            xEventGroupSetBits(rtc.events, RTC_EVENT_CONNECTED);
        } else {
            portENTER_CRITICAL(&group_state_lock);
            const bool was_connected = group_transport_connected;
            portEXIT_CRITICAL(&group_state_lock);
            if (state == ESP_PEER_STATE_CONNECT_FAILED ||
                (was_connected && (state == ESP_PEER_STATE_DISCONNECTED ||
                                   state == ESP_PEER_STATE_CLOSED))) {
                group_mark_unavailable();
                xEventGroupSetBits(rtc.events, RTC_EVENT_FAILED);
            }
        }
        return 0;
    }
    if (state == ESP_PEER_STATE_CONNECT_FAILED) {
        xEventGroupSetBits(rtc.events, RTC_EVENT_FAILED);
    } else if (state == ESP_PEER_STATE_DATA_CHANNEL_CONNECTED && !rtc.data_channel_requested) {
        esp_peer_data_channel_cfg_t channel_cfg = {
            .type = ESP_PEER_DATA_CHANNEL_RELIABLE,
            .ordered = true,
            .label = "oai-events",
        };
        const int ret = esp_peer_create_data_channel(rtc.peer, &channel_cfg);
        if (ret == ESP_PEER_ERR_NONE) {
            rtc.data_channel_requested = true;
        } else {
            ESP_LOGE(TAG, "DataChannel create failed after SCTP connect: %d", ret);
            xEventGroupSetBits(rtc.events, RTC_EVENT_FAILED);
        }
    }
    return 0;
}

static int peer_msg_handler(esp_peer_msg_t *msg, void *ctx)
{
    (void)ctx;
    if (msg == NULL || msg->type != ESP_PEER_MSG_TYPE_SDP || msg->data == NULL || msg->size <= 0) {
        return 0;
    }
    char *sdp = malloc((size_t)msg->size + 1);
    if (sdp == NULL) {
        xEventGroupSetBits(rtc.events, RTC_EVENT_FAILED);
        return -1;
    }
    memcpy(sdp, msg->data, msg->size);
    sdp[msg->size] = '\0';
    xSemaphoreTake(rtc.lock, portMAX_DELAY);
    free(rtc.local_sdp);
    rtc.local_sdp = sdp;
    xSemaphoreGive(rtc.lock);
    xEventGroupSetBits(rtc.events, RTC_EVENT_LOCAL_SDP);
    return 0;
}

static int prefer_dtls_client_role(char *sdp)
{
    static const char from[] = "a=setup:actpass";
    static const char to[] = "a=setup:active";
    int replacements = 0;
    char *match = sdp;
    while ((match = strstr(match, from)) != NULL) {
        const size_t tail_length = strlen(match + sizeof(from) - 1) + 1;
        memcpy(match, to, sizeof(to) - 1);
        memmove(match + sizeof(to) - 1, match + sizeof(from) - 1, tail_length);
        match += sizeof(to) - 1;
        replacements++;
    }
    return replacements;
}

static bool sdp_line_starts_with(const char *line, const char *line_end, const char *prefix)
{
    const size_t prefix_length = strlen(prefix);
    return (size_t)(line_end - line) >= prefix_length &&
           memcmp(line, prefix, prefix_length) == 0;
}

static esp_err_t advertise_audio_recovery(char **sdp_ptr)
{
    if (sdp_ptr == NULL || *sdp_ptr == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    char *sdp = *sdp_ptr;
    char *audio = strstr(sdp, "m=audio ");
    if (audio == NULL) {
        return ESP_ERR_NOT_FOUND;
    }
    char *audio_end = strstr(audio + 1, "\nm=");
    if (audio_end == NULL) {
        audio_end = sdp + strlen(sdp);
    } else {
        audio_end++;
    }

    char *rtpmap = strstr(audio, "a=rtpmap:");
    while (rtpmap != NULL && rtpmap < audio_end) {
        char *line_end = strchr(rtpmap, '\n');
        if (line_end == NULL || line_end > audio_end) {
            line_end = audio_end;
        }
        char *opus = strstr(rtpmap, " opus/");
        if (opus != NULL && opus < line_end) {
            break;
        }
        rtpmap = strstr(line_end, "a=rtpmap:");
    }
    if (rtpmap == NULL || rtpmap >= audio_end) {
        return ESP_ERR_NOT_FOUND;
    }

    char *payload_end = NULL;
    const long payload_type = strtol(rtpmap + strlen("a=rtpmap:"), &payload_end, 10);
    if (payload_end == rtpmap + strlen("a=rtpmap:") ||
        payload_type < 0 || payload_type > 127) {
        return ESP_ERR_INVALID_RESPONSE;
    }

    char addition[96];
    const int addition_length = snprintf(
        addition, sizeof(addition),
        "a=fmtp:%ld minptime=10;useinbandfec=1\r\n",
        payload_type);
    if (addition_length <= 0 || (size_t)addition_length >= sizeof(addition)) {
        return ESP_FAIL;
    }

    char *insert_at = strchr(rtpmap, '\n');
    if (insert_at == NULL) {
        insert_at = sdp + strlen(sdp);
    } else {
        insert_at++;
    }
    const size_t prefix_length = (size_t)(insert_at - sdp);
    const size_t suffix_length = strlen(insert_at);
    char *expanded = malloc(prefix_length + (size_t)addition_length + suffix_length + 1);
    if (expanded == NULL) {
        return ESP_ERR_NO_MEM;
    }
    memcpy(expanded, sdp, prefix_length);
    memcpy(expanded + prefix_length, addition, (size_t)addition_length);
    memcpy(expanded + prefix_length + (size_t)addition_length,
           insert_at, suffix_length + 1);

    free(sdp);
    *sdp_ptr = expanded;
    ESP_LOGI(TAG, "advertised Opus in-band FEC for payload %ld", payload_type);
    return ESP_OK;
}

static void log_audio_sdp_features(const char *side, const char *sdp)
{
    if (sdp == NULL) {
        ESP_LOGW(TAG, "SDP %s audio diagnostics unavailable", side);
        return;
    }

    const char *audio = strstr(sdp, "m=audio ");
    if (audio == NULL) {
        ESP_LOGW(TAG, "SDP %s has no audio media section", side);
        return;
    }
    const char *audio_end = strstr(audio + 1, "\nm=");
    if (audio_end == NULL) {
        audio_end = audio + strlen(audio);
    } else {
        audio_end++;
    }

    int opus_payload_type = -1;
    bool rtcp_mux = false;
    for (const char *line = audio; line < audio_end;) {
        const char *line_end = memchr(line, '\n', (size_t)(audio_end - line));
        if (line_end == NULL) {
            line_end = audio_end;
        }
        if (sdp_line_starts_with(line, line_end, "a=rtpmap:")) {
            const char *payload = line + strlen("a=rtpmap:");
            char *payload_end = NULL;
            const long parsed = strtol(payload, &payload_end, 10);
            const char *opus = payload_end == NULL ? NULL : strstr(payload_end, " opus/");
            if (payload_end != payload && payload_end < line_end &&
                opus != NULL && opus < line_end) {
                opus_payload_type = (int)parsed;
            }
        } else if (sdp_line_starts_with(line, line_end, "a=rtcp-mux")) {
            rtcp_mux = true;
        }
        line = line_end < audio_end ? line_end + 1 : audio_end;
    }

    bool nack = false;
    bool transport_cc = false;
    bool inband_fec = false;
    char payload_feedback[32];
    char payload_transport_cc[40];
    char payload_fmtp[24];
    if (opus_payload_type >= 0) {
        snprintf(payload_feedback, sizeof(payload_feedback), "a=rtcp-fb:%d nack",
                 opus_payload_type);
        snprintf(payload_transport_cc, sizeof(payload_transport_cc),
                 "a=rtcp-fb:%d transport-cc", opus_payload_type);
        snprintf(payload_fmtp, sizeof(payload_fmtp), "a=fmtp:%d ", opus_payload_type);
    } else {
        payload_feedback[0] = '\0';
        payload_transport_cc[0] = '\0';
        payload_fmtp[0] = '\0';
    }
    for (const char *line = audio; line < audio_end;) {
        const char *line_end = memchr(line, '\n', (size_t)(audio_end - line));
        if (line_end == NULL) {
            line_end = audio_end;
        }
        if (sdp_line_starts_with(line, line_end, "a=rtcp-fb:* nack") ||
            (payload_feedback[0] != '\0' &&
             sdp_line_starts_with(line, line_end, payload_feedback))) {
            nack = true;
        }
        if (sdp_line_starts_with(line, line_end, "a=rtcp-fb:* transport-cc") ||
            (payload_transport_cc[0] != '\0' &&
             sdp_line_starts_with(line, line_end, payload_transport_cc))) {
            transport_cc = true;
        }
        if (payload_fmtp[0] != '\0' &&
            sdp_line_starts_with(line, line_end, payload_fmtp)) {
            const char *fec = strstr(line, "useinbandfec=1");
            if (fec != NULL && fec < line_end) {
                inband_fec = true;
            }
        }
        line = line_end < audio_end ? line_end + 1 : audio_end;
    }

    ESP_LOGI(TAG,
             "SDP %s audio: opus_pt=%d nack=%s fec=%s transport_cc=%s rtcp_mux=%s",
             side, opus_payload_type, nack ? "yes" : "no", inband_fec ? "yes" : "no",
             transport_cc ? "yes" : "no", rtcp_mux ? "yes" : "no");
}

static esp_err_t send_data_payload(const char *payload)
{
    if (payload == NULL || rtc.peer == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    esp_peer_data_frame_t frame = {
        .type = ESP_PEER_DATA_CHANNEL_STRING,
        .stream_id = rtc.data_stream_id,
        .data = (uint8_t *)payload,
        .size = (int)strlen(payload),
    };
    return esp_peer_send_data(rtc.peer, &frame) == ESP_PEER_ERR_NONE ? ESP_OK : ESP_FAIL;
}

static esp_err_t send_session_update(void)
{
    cJSON *root = cJSON_CreateObject();
    cJSON *session = cJSON_CreateObject();
    cJSON *modalities = cJSON_CreateArray();
    cJSON *transcription = cJSON_CreateObject();
    cJSON *turn = cJSON_CreateObject();
    if (root == NULL || session == NULL || modalities == NULL || transcription == NULL || turn == NULL) {
        cJSON_Delete(root);
        cJSON_Delete(session);
        cJSON_Delete(modalities);
        cJSON_Delete(transcription);
        cJSON_Delete(turn);
        return ESP_ERR_NO_MEM;
    }
    cJSON_AddItemToArray(modalities, cJSON_CreateString("text"));
    cJSON_AddItemToArray(modalities, cJSON_CreateString("audio"));
    cJSON_AddStringToObject(root, "type", "session.update");
    cJSON_AddItemToObject(root, "session", session);
    cJSON_AddItemToObject(session, "modalities", modalities);
    cJSON_AddStringToObject(session, "voice", rti_session_get_voice_profile());
    cJSON_AddStringToObject(session, "input_audio_format", "pcm");
    cJSON_AddStringToObject(session, "output_audio_format", "pcm");
    char instructions[384];
    strlcpy(instructions,
            board_display_is_chinese()
                ? "你是" APP_DISPLAY_NAME "的实时语音助手。请用中文自然对话，回答简洁，适合语音播报。"
                  "当用户询问天气、新闻、价格、赛果等实时信息时，请使用网络搜索后再回答。"
                : "You are the voice assistant in " APP_DISPLAY_NAME ". Reply naturally and concisely in English. "
                  "Use web search before answering questions about current weather, news, prices, or scores.",
            sizeof(instructions));
    cJSON_AddStringToObject(session, "instructions", instructions);
#if CONFIG_MIKU_RTI_ENABLE_WEB_SEARCH
    cJSON *search_options = cJSON_CreateObject();
    if (search_options == NULL ||
        !cJSON_AddBoolToObject(session, "enable_search", true) ||
        !cJSON_AddBoolToObject(search_options, "enable_source", true)) {
        cJSON_Delete(search_options);
        cJSON_Delete(root);
        return ESP_ERR_NO_MEM;
    }
    cJSON_AddItemToObject(session, "search_options", search_options);
#endif
    cJSON_AddStringToObject(transcription, "model", "qwen3-asr-flash-realtime");
    cJSON_AddItemToObject(session, "input_audio_transcription", transcription);
    cJSON_AddStringToObject(turn, "type", "semantic_vad");
    cJSON_AddNumberToObject(turn, "threshold", 0.5);
    cJSON_AddNumberToObject(turn, "silence_duration_ms", 1100);
    cJSON_AddBoolToObject(turn, "create_response", true);
    cJSON_AddBoolToObject(turn, "interrupt_response", true);
    cJSON_AddItemToObject(session, "turn_detection", turn);

    char *payload = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    if (payload == NULL) {
        return ESP_ERR_NO_MEM;
    }
    const esp_err_t err = send_data_payload(payload);
    free(payload);
    return err;
}

static esp_err_t send_welcome_prompt(void)
{
    static const char response_create[] =
        "{\"type\":\"response.create\",\"response\":{\"modalities\":[\"text\",\"audio\"]}}";
    cJSON *root = cJSON_CreateObject();
    cJSON *item = cJSON_CreateObject();
    cJSON *content = cJSON_CreateArray();
    cJSON *part = cJSON_CreateObject();
    char prompt[256];
    strlcpy(prompt,
            board_display_is_chinese()
                ? "请只用中文说一句简短欢迎语：" APP_WELCOME_ZH "不要补充其他内容。"
                : "Say only this brief welcome: " APP_WELCOME_EN,
            sizeof(prompt));
    if (root == NULL || item == NULL || content == NULL || part == NULL ||
        !cJSON_AddStringToObject(root, "type", "conversation.item.create") ||
        !cJSON_AddStringToObject(item, "type", "message") ||
        !cJSON_AddStringToObject(item, "role", "user") ||
        !cJSON_AddStringToObject(part, "type", "input_text") ||
        !cJSON_AddStringToObject(part, "text", prompt)) {
        cJSON_Delete(root);
        cJSON_Delete(item);
        cJSON_Delete(content);
        cJSON_Delete(part);
        return ESP_ERR_NO_MEM;
    }
    cJSON_AddItemToArray(content, part);
    cJSON_AddItemToObject(item, "content", content);
    cJSON_AddItemToObject(root, "item", item);
    char *welcome_item = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    if (welcome_item == NULL) {
        return ESP_ERR_NO_MEM;
    }
    const esp_err_t welcome_err = send_data_payload(welcome_item);
    free(welcome_item);
    if (welcome_err != ESP_OK ||
        send_data_payload(response_create) != ESP_OK) {
        return ESP_FAIL;
    }
    ESP_LOGI(TAG, "welcome response requested");
    return ESP_OK;
}

static int peer_channel_open_handler(esp_peer_data_channel_info_t *channel, void *ctx)
{
    (void)ctx;
    if (channel == NULL || channel->label == NULL || strcmp(channel->label, "oai-events") != 0) {
        return 0;
    }
    rtc.data_stream_id = channel->stream_id;
    ESP_LOGI(TAG, "oai-events DataChannel opened");
    if (send_session_update() == ESP_OK) {
        xEventGroupSetBits(rtc.events, RTC_EVENT_DATA_OPEN);
    } else {
        ESP_LOGE(TAG, "could not send session.update");
        xEventGroupSetBits(rtc.events, RTC_EVENT_FAILED);
    }
    return 0;
}

static int peer_data_handler(esp_peer_data_frame_t *frame, void *ctx)
{
    (void)ctx;
    if (!rtc.running || frame == NULL || frame->data == NULL || frame->size <= 0 ||
        frame->size > RTC_EVENT_JSON_MAX || rtc.data_queue == NULL) {
        return 0;
    }
    const size_t size = (size_t)frame->size;
    data_event_t *event = heap_caps_malloc(
        sizeof(*event) + size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (event == NULL) {
        event = malloc(sizeof(*event) + size);
    }
    if (event == NULL) {
        ESP_LOGW(TAG, "DataChannel event allocation failed: size=%u",
                 (unsigned)size);
        return 0;
    }
    event->size = size;
    memcpy(event->data, frame->data, size);
    if (xQueueSend(rtc.data_queue, &event, 0) != pdTRUE) {
        free(event);
        const uint32_t drops = ++rtc.data_event_drops;
        if (drops == 1 || drops % 10 == 0) {
            ESP_LOGW(TAG, "DataChannel event queue full: drops=%" PRIu32, drops);
        }
    }
    return 0;
}

static int peer_channel_close_handler(esp_peer_data_channel_info_t *channel, void *ctx)
{
    (void)channel;
    (void)ctx;
    ESP_LOGI(TAG, "oai-events DataChannel closed");
    return 0;
}

static int peer_audio_info_handler(esp_peer_audio_stream_info_t *info, void *ctx)
{
    (void)ctx;
    if (info != NULL) {
        ESP_LOGI(TAG, "remote audio codec=%d rate=%" PRIu32 " channels=%u",
                 info->codec, info->sample_rate, info->channel);
    }
    return 0;
}

static int peer_audio_data_handler(esp_peer_audio_frame_t *frame, void *ctx)
{
    (void)ctx;
    if (!rtc.audio_running || frame == NULL || frame->data == NULL || frame->size <= 0 ||
        frame->size > RTC_OPUS_PACKET_MAX || rtc.playback_queue == NULL) {
        return 0;
    }
    const int64_t received_us = esp_timer_get_time();
    opus_packet_t packet = {
        .size = (size_t)frame->size,
        .received_us = received_us,
        .pts = frame->pts,
    };
    const uint32_t pts_delta =
        rtc.has_last_audio_pts ? frame->pts - rtc.last_audio_pts : 0;
    if (rtc.last_audio_packet_us != 0) {
        const int64_t gap_us = received_us - rtc.last_audio_packet_us;
        if (gap_us > RTC_AUDIO_PACKET_GAP_WARN_US) {
            const uint32_t gaps = ++rtc.playback_input_gaps;
            if (gaps == 1 || gaps % 10 == 0 || gap_us >= 80000) {
                ESP_LOGW(TAG,
                         "playback packet gap: count=%" PRIu32 " gap=%" PRId64
                         "us pts=%" PRIu32 " pts_delta=%" PRIu32 " queue=%u",
                         gaps, gap_us, frame->pts, pts_delta,
                         (unsigned)uxQueueMessagesWaiting(rtc.playback_queue));
            }
        }
    }
    rtc.last_audio_packet_us = received_us;
    rtc.last_audio_pts = frame->pts;
    rtc.has_last_audio_pts = true;
    memcpy(packet.data, frame->data, packet.size);
    if (rtc.group_mode) {
        portENTER_CRITICAL(&group_state_lock);
        ++group_stats.received_packets;
        portEXIT_CRITICAL(&group_state_lock);
    }
    if (xQueueSend(rtc.playback_queue, &packet, 0) != pdTRUE) {
        if (rtc.group_mode) {
            // Bound conversational delay by discarding the oldest queued packet.
            opus_packet_t stale;
            (void)xQueueReceive(rtc.playback_queue, &stale, 0);
            (void)xQueueSend(rtc.playback_queue, &packet, 0);
        }
        const uint32_t overflows = ++rtc.playback_overflows;
        if (overflows == 1 || overflows % 10 == 0) {
            ESP_LOGW(TAG, "playback queue overflow: count=%" PRIu32, overflows);
        }
    }
    return 0;
}

static aec_handle_t *create_full_duplex_aec(void)
{
    aec_config_t config = {
        .mic_num = 1,
        .ref_num = 1,
        .out_num = 1,
        .filter_length = 4,
        .sample_rate = 16000,
        .caps = MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT,
        .mode = RTC_AEC_MODE,
        .nlp_level = AEC_NLP_LEVEL_AGGR,
    };
    return aec_create_from_config(&config);
}

esp_err_t webrtc_peer_aec_self_test(void)
{
    if (persistent_aec != NULL) {
        return ESP_OK;
    }
    const size_t internal_before = heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    const size_t psram_before = heap_caps_get_free_size(MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    aec_handle_t *aec = create_full_duplex_aec();
    if (aec == NULL) {
        ESP_LOGE(TAG, "AEC self-test could not create full-duplex instance");
        return ESP_ERR_NO_MEM;
    }
    const int frame_samples = aec_get_chunksize(aec);
    if (frame_samples <= 0 || frame_samples > RTC_AEC_MAX_FRAME_SAMPLES) {
        ESP_LOGE(TAG, "AEC self-test returned invalid frame size: %d", frame_samples);
        aec_destroy(aec);
        return ESP_ERR_INVALID_SIZE;
    }

    const size_t internal_during = heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    const size_t psram_during = heap_caps_get_free_size(MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    ESP_LOGI(TAG,
             "AEC initialized and pinned: %s frame=%d internal=%uB psram=%uB",
             aec_get_mode_string(RTC_AEC_MODE), frame_samples,
             (unsigned)(internal_before - internal_during), (unsigned)(psram_before - psram_during));
    // Keep this clean instance for all Sessions. Recreating ESP-SR after the
    // RTC/audio stacks fragment internal memory can place private FFT scratch
    // in 0x2e... RTC RAM, which the optimized S31/P4 FFT path cannot access.
    persistent_aec = aec;
    persistent_aec_frame_samples = frame_samples;
    return ESP_OK;
}

static void queue_playback_reference(const int16_t *pcm, size_t frame_count, uint8_t channels,
                                     int16_t *mono)
{
    if (
#if CONFIG_MIKU_RTI_CAPTURE_DIAGNOSTIC
        rtc.reference_stream == NULL ||
#else
        !rtc.aec_enabled || rtc.reference_stream == NULL ||
#endif
        pcm == NULL || mono == NULL ||
        frame_count == 0 || (channels != 1 && channels != 2)) {
        return;
    }
    const TickType_t now_tick = xTaskGetTickCount();
    if (rtc.playback_guard_until_tick == 0 ||
        (int32_t)(rtc.playback_guard_until_tick - now_tick) <= 0) {
        rtc.reference_primed = false;
    }
    if (!rtc.reference_primed) {
        static const int16_t silence[RTC_AEC_REFERENCE_DELAY_SAMPLES] = {0};
        const size_t delay_bytes = sizeof(silence);
        if (xStreamBufferSend(rtc.reference_stream, silence, delay_bytes, 0) ==
            delay_bytes) {
            rtc.reference_primed = true;
            ESP_LOGI(TAG, "AEC software reference primed with %d ms delay",
                     RTC_AEC_REFERENCE_DELAY_MS);
        } else {
            ++rtc.reference_overflows;
            return;
        }
    }
    for (size_t index = 0; index < frame_count; ++index) {
        if (channels == 1) {
            mono[index] = pcm[index];
        } else {
            const int32_t left = pcm[index * 2];
            const int32_t right = pcm[index * 2 + 1];
            mono[index] = (int16_t)((left + right) / 2);
        }
    }
    const size_t bytes = frame_count * sizeof(*mono);
    // Speaker playout is the product clock: never stall it behind the AEC
    // reference consumer. The local playout gate contains residual echo when
    // clock drift makes us drop a reference frame.
    if (xStreamBufferSend(rtc.reference_stream, mono, bytes, 0) != bytes) {
        ++rtc.reference_overflows;
    }
}

static void microphone_task(void *arg)
{
    (void)arg;
    uint32_t encoded_frames = 0;
    uint32_t aec_frames = 0;
    uint32_t last_logged_encoded_frames = 0;
    uint32_t last_logged_aec_frames = 0;
    uint32_t reference_underflows = 0;
    uint32_t reference_trimmed_samples = 0;
    uint64_t speaking_mic0_abs = 0;
    uint64_t speaking_mic1_abs = 0;
    uint64_t speaking_ref_abs = 0;
    uint64_t speaking_out_abs = 0;
    uint32_t speaking_capture_samples = 0;
    uint32_t speaking_aec_samples = 0;
    int64_t aec_total_us = 0;
    int64_t aec_max_us = 0;
    uint32_t capture_frames = 0;
    uint32_t clean_noise_floor = 0;
    uint32_t raw_echo_floor = 0;
    uint32_t barge_in_candidate_frames = 0;
    uint32_t raw_candidate_rejections = 0;
    TickType_t barge_in_until_tick = 0;
    TickType_t playback_started_tick = 0;
    bool barge_in_logged = false;
    uint32_t next_pts = (uint32_t)(esp_timer_get_time() / 1000);
    uint32_t observed_send_epoch = 0;
    const size_t encoder_samples = (size_t)rtc.encoder_input_size / sizeof(int16_t);
    int16_t *capture = alloc_audio_workspace(RTC_CAPTURE_FRAME_BYTES);
    int16_t *capture_mono = alloc_audio_workspace(RTC_CAPTURE_FRAME_SAMPLES * sizeof(*capture_mono));
    int16_t *mic_accumulator = alloc_audio_workspace(RTC_AEC_ACCUMULATOR_SAMPLES * sizeof(*mic_accumulator));
    int16_t *clean_accumulator = alloc_audio_workspace(RTC_AEC_ACCUMULATOR_SAMPLES * sizeof(*clean_accumulator));
    int16_t *aec_mic = aec_dma_mic;
    int16_t *aec_reference = aec_dma_reference;
    int16_t *aec_output = aec_dma_output;
    uint8_t *encoded = malloc((size_t)rtc.encoder_output_size);
    vad_handle_t barge_vad = vad_create(VAD_MODE_4);
#if CONFIG_MIKU_RTI_CAPTURE_DIAGNOSTIC
    capture_diag_t *capture_diag = calloc(1, sizeof(*capture_diag));
    int16_t *diagnostic_reference =
        calloc(RTC_CAPTURE_FRAME_SAMPLES, sizeof(*diagnostic_reference));
#endif
    if (capture == NULL || capture_mono == NULL || mic_accumulator == NULL ||
        clean_accumulator == NULL || encoded == NULL || barge_vad == NULL ||
        encoder_samples == 0 ||
        encoder_samples > RTC_AEC_ACCUMULATOR_SAMPLES
#if CONFIG_MIKU_RTI_CAPTURE_DIAGNOSTIC
        || capture_diag == NULL || diagnostic_reference == NULL
#endif
        ) {
        ESP_LOGE(TAG, "microphone buffers unavailable");
        rtc.audio_running = false;
        group_mark_unavailable();
        xEventGroupSetBits(rtc.events, RTC_EVENT_FAILED);
        free(capture);
        free(capture_mono);
        free(mic_accumulator);
        free(clean_accumulator);
        free(encoded);
        if (barge_vad != NULL) {
            vad_destroy(barge_vad);
        }
#if CONFIG_MIKU_RTI_CAPTURE_DIAGNOSTIC
        free(capture_diag);
        free(diagnostic_reference);
#endif
        finish_worker(RTC_EVENT_MIC_DONE);
        return;
    }

#if !CONFIG_MIKU_RTI_OFFICIAL_CAPTURE_AEC
    ESP_LOGI(TAG, "AEC DMA buffers: mic=%p reference=%p output=%p", aec_mic,
             aec_reference, aec_output);
#endif
#if !CONFIG_MIKU_RTI_OFFICIAL_CAPTURE_AEC
    size_t mic_samples = 0;
#endif
    size_t clean_samples = 0;
    while (rtc.audio_running) {
#if CONFIG_MIKU_RTI_OFFICIAL_CAPTURE_AEC
        esp_capture_stream_frame_t capture_frame = {
            .stream_type = ESP_CAPTURE_STREAM_TYPE_AUDIO,
            .data = (uint8_t *)capture_mono,
            .size = RTC_CAPTURE_FRAME_SAMPLES * sizeof(*capture_mono),
        };
        if (rtc.capture_source == NULL ||
            rtc.capture_source->read_frame(rtc.capture_source, &capture_frame) !=
                ESP_CAPTURE_ERR_OK ||
            capture_frame.size != RTC_CAPTURE_FRAME_SAMPLES * sizeof(*capture_mono)) {
            ESP_LOGW(TAG, "official AEC microphone read failed");
            vTaskDelay(pdMS_TO_TICKS(20));
            continue;
        }
#else
        if (esp_codec_dev_read(rtc.audio.microphone, capture, RTC_CAPTURE_FRAME_BYTES) != 0) {
            ESP_LOGW(TAG, "microphone read failed");
            vTaskDelay(pdMS_TO_TICKS(20));
            continue;
        }
#endif
        const bool agent_speaking = !rtc.group_mode &&
                                    rti_app_get_state() == RTI_APP_STATE_AGENT_SPEAKING;
        for (size_t index = 0; index < RTC_CAPTURE_FRAME_SAMPLES; ++index) {
#if CONFIG_MIKU_RTI_OFFICIAL_CAPTURE_AEC
            const int16_t mic0 = capture_mono[index];
            const int16_t mic1 = 0;
#else
            const int16_t mic0 = capture[index * RTC_CAPTURE_CHANNELS];
            const int16_t mic1 = capture[index * RTC_CAPTURE_CHANNELS + 1];
            capture_mono[index] = mic0;
#endif
            if (agent_speaking) {
                speaking_mic0_abs += mic0 < 0 ? -(int32_t)mic0 : mic0;
                speaking_mic1_abs += mic1 < 0 ? -(int32_t)mic1 : mic1;
                ++speaking_capture_samples;
            }
        }

        if (rtc.group_mode) {
            const float rms = pcm_rms(capture_mono, RTC_CAPTURE_FRAME_SAMPLES);
            portENTER_CRITICAL(&group_state_lock);
            ++group_stats.source_frames;
            if (rms > group_stats.source_rms_peak) {
                group_stats.source_rms_peak = rms;
            }
            portEXIT_CRITICAL(&group_state_lock);
            uint32_t epoch;
            (void)group_sending_allowed(&epoch);
            if (epoch != observed_send_epoch) {
                // The software AEC keeps running while PTT is closed. On each
                // gate generation discard partial speech from the old press,
                // while continuing to consume the live playback reference.
                clean_samples = 0;
#if !CONFIG_MIKU_RTI_OFFICIAL_CAPTURE_AEC
                mic_samples = 0;
#endif
                observed_send_epoch = epoch;
            }
        }
        const TickType_t now_tick = xTaskGetTickCount();
        const bool playback_window = rtc.playback_guard_until_tick != 0 &&
                                     (int32_t)(rtc.playback_guard_until_tick - now_tick) > 0;
        if (playback_window && playback_started_tick == 0) {
            playback_started_tick = now_tick;
            vad_reset_trigger(barge_vad);
        }
        const bool barge_in_ready =
            playback_window && playback_started_tick != 0 &&
            (now_tick - playback_started_tick) >= pdMS_TO_TICKS(RTC_BARGE_IN_WARMUP_MS);
        bool barge_in_active = barge_in_until_tick != 0 &&
                               (int32_t)(barge_in_until_tick - now_tick) > 0;
        uint32_t barge_threshold = 0;
#if CONFIG_MIKU_RTI_CAPTURE_DIAGNOSTIC
        // This run measures speaker leakage only; never let the diagnostic
        // itself create another model turn.
        bool gate_uplink = playback_window;
#elif CONFIG_MIKU_RTI_OFFICIAL_CAPTURE_AEC
        // The official source already subtracts the codec's hardware playback
        // reference, so keep the uplink open for natural barge-in.
        bool gate_uplink = false;
#else
        bool gate_uplink = playback_window && !barge_in_active;
#endif
#if CONFIG_MIKU_RTI_CAPTURE_DIAGNOSTIC
        memset(diagnostic_reference, 0,
               RTC_CAPTURE_FRAME_SAMPLES * sizeof(*diagnostic_reference));
        if (rtc.reference_stream != NULL) {
            xStreamBufferReceive(
                rtc.reference_stream, diagnostic_reference,
                RTC_CAPTURE_FRAME_SAMPLES * sizeof(*diagnostic_reference), 0);
        }
        capture_diag_process(capture_diag, capture, diagnostic_reference,
                             playback_window);
#endif
        if (!playback_window) {
            clean_noise_floor = 0;
            raw_echo_floor = 0;
            barge_in_candidate_frames = 0;
            barge_in_until_tick = 0;
            playback_started_tick = 0;
            barge_in_active = false;
            barge_in_logged = false;
            vad_reset_trigger(barge_vad);
        }

#if CONFIG_MIKU_RTI_OFFICIAL_CAPTURE_AEC
        memcpy(clean_accumulator + clean_samples, capture_mono,
               RTC_CAPTURE_FRAME_SAMPLES * sizeof(*capture_mono));
        clean_samples += RTC_CAPTURE_FRAME_SAMPLES;
#else
        if (rtc.aec_enabled) {
            if (mic_samples + RTC_CAPTURE_FRAME_SAMPLES > RTC_AEC_ACCUMULATOR_SAMPLES) {
                ESP_LOGW(TAG, "AEC microphone accumulator overflow");
                mic_samples = 0;
            }
            memcpy(mic_accumulator + mic_samples, capture_mono,
                   RTC_CAPTURE_FRAME_SAMPLES * sizeof(*capture_mono));
            mic_samples += RTC_CAPTURE_FRAME_SAMPLES;

            while (mic_samples >= (size_t)rtc.aec_frame_samples) {
                const size_t aec_bytes = (size_t)rtc.aec_frame_samples * sizeof(*aec_mic);
                const size_t reference_available =
                    xStreamBufferBytesAvailable(rtc.reference_stream);
                if (reference_available > RTC_AEC_REFERENCE_MAX_BUFFER_BYTES) {
                    const size_t trim_bytes =
                        RTC_AEC_REFERENCE_TRIM_SAMPLES * sizeof(*aec_reference);
                    const size_t trimmed = xStreamBufferReceive(
                        rtc.reference_stream, aec_reference, trim_bytes, 0);
                    reference_trimmed_samples +=
                        (uint32_t)(trimmed / sizeof(*aec_reference));
                }
                memcpy(aec_mic, mic_accumulator, aec_bytes);
                const size_t reference_bytes = xStreamBufferReceive(
                    rtc.reference_stream, aec_reference, aec_bytes, 0);
                if (reference_bytes < aec_bytes) {
                    memset((uint8_t *)aec_reference + reference_bytes, 0,
                           aec_bytes - reference_bytes);
                    if (rti_app_get_state() == RTI_APP_STATE_AGENT_SPEAKING) {
                        ++reference_underflows;
                    }
                }
                const int64_t started_us = esp_timer_get_time();
                aec_process(rtc.aec, aec_mic, aec_reference, aec_output);
                uint64_t clean_abs = 0;
                uint64_t raw_abs = 0;
                uint32_t clean_peak = 0;
                for (int index = 0; index < rtc.aec_frame_samples; ++index) {
                    const int16_t raw_sample = aec_mic[index];
                    const int16_t sample = aec_output[index];
                    raw_abs += raw_sample < 0 ? -(int32_t)raw_sample : raw_sample;
                    const uint32_t magnitude =
                        sample < 0 ? (uint32_t)-(int32_t)sample : (uint32_t)sample;
                    clean_abs += magnitude;
                    if (magnitude > clean_peak) {
                        clean_peak = magnitude;
                    }
                }
                const uint32_t clean_mean =
                    (uint32_t)(clean_abs / (uint32_t)rtc.aec_frame_samples);
                const uint32_t raw_mean =
                    (uint32_t)(raw_abs / (uint32_t)rtc.aec_frame_samples);
                if (playback_window) {
                    const vad_state_t vad_state =
                        vad_process(barge_vad, aec_output, 16000, 30);
                    if (clean_noise_floor == 0) {
                        clean_noise_floor = clean_mean;
                    }
                    if (raw_echo_floor == 0) {
                        raw_echo_floor = raw_mean;
                    }
                    barge_threshold =
                        clean_noise_floor * RTC_BARGE_IN_NOISE_MULTIPLIER +
                        RTC_BARGE_IN_NOISE_MARGIN;
                    if (barge_threshold < RTC_BARGE_IN_MIN_CLEAN_MEAN) {
                        barge_threshold = RTC_BARGE_IN_MIN_CLEAN_MEAN;
                    }
                    if (barge_threshold > RTC_BARGE_IN_MAX_CLEAN_MEAN) {
                        barge_threshold = RTC_BARGE_IN_MAX_CLEAN_MEAN;
                    }
                    uint32_t raw_threshold =
                        raw_echo_floor * RTC_BARGE_IN_RAW_MULTIPLIER +
                        RTC_BARGE_IN_RAW_MARGIN;
                    if (raw_threshold < RTC_BARGE_IN_MIN_RAW_MEAN) {
                        raw_threshold = RTC_BARGE_IN_MIN_RAW_MEAN;
                    }
                    if (raw_threshold > RTC_BARGE_IN_MAX_RAW_MEAN) {
                        raw_threshold = RTC_BARGE_IN_MAX_RAW_MEAN;
                    }
                    const bool speech_energy_candidate =
                        barge_in_ready && vad_state == VAD_SPEECH &&
                        clean_mean >= barge_threshold &&
                        clean_peak >= barge_threshold * RTC_BARGE_IN_PEAK_MULTIPLIER;
                    const bool near_end_candidate =
                        speech_energy_candidate && raw_mean >= raw_threshold;
                    if (speech_energy_candidate && !near_end_candidate) {
                        ++raw_candidate_rejections;
                        if (raw_candidate_rejections == 1 ||
                            raw_candidate_rejections % 25U == 0) {
                            ESP_LOGI(
                                TAG,
                                "barge-in echo candidate rejected: clean_mean=%u "
                                "raw_mean=%u raw_threshold=%u raw_floor=%u",
                                clean_mean, raw_mean, raw_threshold, raw_echo_floor);
                        }
                    }
                    if (near_end_candidate) {
                        if (barge_in_candidate_frames < RTC_BARGE_IN_MIN_FRAMES) {
                            ++barge_in_candidate_frames;
                        }
                        if (barge_in_candidate_frames >= RTC_BARGE_IN_MIN_FRAMES) {
                            barge_in_until_tick =
                                now_tick + pdMS_TO_TICKS(RTC_BARGE_IN_HOLD_MS);
                        }
                    } else {
                        barge_in_candidate_frames = 0;
                        if (!barge_in_active) {
                            // Follow stationary AEC residue slowly, but do not
                            // teach a near-end speech candidate into the floor.
                            clean_noise_floor =
                                (clean_noise_floor * 15U + clean_mean) / 16U;
                            if (!speech_energy_candidate) {
                                raw_echo_floor =
                                    (raw_echo_floor * 15U + raw_mean) / 16U;
                            }
                        }
                    }
                    barge_in_active =
                        barge_in_until_tick != 0 &&
                        (int32_t)(barge_in_until_tick - now_tick) > 0;
                    gate_uplink = !barge_in_active;
                    if (barge_in_active && !barge_in_logged) {
                        ESP_LOGI(
                            TAG,
                            "local barge-in opened from AEC: clean_mean=%u "
                            "clean_peak=%u threshold=%u noise_floor=%u "
                            "raw_mean=%u raw_threshold=%u raw_floor=%u vad=speech",
                            clean_mean, clean_peak, barge_threshold,
                            clean_noise_floor, raw_mean, raw_threshold,
                            raw_echo_floor);
                        barge_in_logged = true;
                    } else if (!barge_in_active) {
                        barge_in_logged = false;
                    }
                }
                if (agent_speaking) {
                    for (int index = 0; index < rtc.aec_frame_samples; ++index) {
                        const int16_t reference = aec_reference[index];
                        const int16_t output = aec_output[index];
                        speaking_ref_abs += reference < 0 ? -(int32_t)reference : reference;
                        speaking_out_abs += output < 0 ? -(int32_t)output : output;
                    }
                    speaking_aec_samples += (uint32_t)rtc.aec_frame_samples;
                }
                const int64_t elapsed_us = esp_timer_get_time() - started_us;
                aec_total_us += elapsed_us;
                if (elapsed_us > aec_max_us) {
                    aec_max_us = elapsed_us;
                }
                ++aec_frames;

                if (clean_samples + (size_t)rtc.aec_frame_samples >
                    RTC_AEC_ACCUMULATOR_SAMPLES) {
                    ESP_LOGW(TAG, "AEC output accumulator overflow");
                    clean_samples = 0;
                }
                memcpy(clean_accumulator + clean_samples, aec_output, aec_bytes);
                clean_samples += (size_t)rtc.aec_frame_samples;
                mic_samples -= (size_t)rtc.aec_frame_samples;
                memmove(mic_accumulator, mic_accumulator + rtc.aec_frame_samples,
                        mic_samples * sizeof(*mic_accumulator));
            }
        } else {
            if (rti_app_get_state() == RTI_APP_STATE_AGENT_SPEAKING) {
                memset(capture_mono, 0, RTC_CAPTURE_FRAME_SAMPLES * sizeof(*capture_mono));
            }
            memcpy(clean_accumulator + clean_samples, capture_mono,
                   RTC_CAPTURE_FRAME_SAMPLES * sizeof(*capture_mono));
            clean_samples += RTC_CAPTURE_FRAME_SAMPLES;
        }
#endif

        while (clean_samples >= encoder_samples) {
            // AEC/reference consumption above is continuous even while PTT is
            // closed. Only cleaned audio from the current live grant may enter
            // the encoder, and the send path checks the grant once more.
            if (rtc.group_mode) {
                uint32_t encode_epoch;
                if (!group_sending_allowed(&encode_epoch) ||
                    encode_epoch != observed_send_epoch) {
                    clean_samples = 0;
                    next_pts = (uint32_t)(esp_timer_get_time() / 1000);
                    break;
                }
                // PTT authority is independent of the AI barge-in heuristic.
                gate_uplink = false;
            }
            if (gate_uplink) {
                memset(clean_accumulator, 0, rtc.encoder_input_size);
            }
            esp_audio_enc_in_frame_t input = {
                .buffer = (uint8_t *)clean_accumulator,
                .len = (uint32_t)rtc.encoder_input_size,
            };
            esp_audio_enc_out_frame_t output = {
                .buffer = encoded,
                .len = (uint32_t)rtc.encoder_output_size,
            };
            if (esp_opus_enc_process(rtc.opus_encoder, &input, &output) == ESP_AUDIO_ERR_OK &&
                output.encoded_bytes != 0) {
                esp_peer_audio_frame_t frame = {
                    .pts = next_pts,
                    .data = encoded,
                    .size = (int)output.encoded_bytes,
                };
                uint32_t send_epoch = 0;
                if (!rtc.group_mode || (group_sending_allowed(&send_epoch) &&
                                        send_epoch == observed_send_epoch)) {
                    if (esp_peer_send_audio(rtc.peer, &frame) != ESP_PEER_ERR_NONE) {
                        ESP_LOGW(TAG, "audio send failed");
                    } else if (rtc.group_mode) {
                        portENTER_CRITICAL(&group_state_lock);
                        ++group_stats.sent_packets;
                        portEXIT_CRITICAL(&group_state_lock);
                    }
                }
                ++encoded_frames;
            }
            next_pts += 20;
            clean_samples -= encoder_samples;
            memmove(clean_accumulator, clean_accumulator + encoder_samples,
                    clean_samples * sizeof(*clean_accumulator));
        }
        if (rtc.group_mode) {
            uint32_t output_epoch;
            if (!group_sending_allowed(&output_epoch) ||
                output_epoch != observed_send_epoch) {
                clean_samples = 0;
                next_pts = (uint32_t)(esp_timer_get_time() / 1000);
            }
        }
        if (rtc.aec_enabled &&
            aec_frames - last_logged_aec_frames >= RTC_STACK_LOG_INTERVAL) {
            last_logged_aec_frames = aec_frames;
            ESP_LOGI(TAG,
                     "AEC frames=%" PRIu32 " avg=%" PRId64 "us max=%" PRId64
                     "us ref_wait=%" PRIu32 " ref_drop=%" PRIu32 " ref_buffer=%u",
                     aec_frames, aec_total_us / aec_frames, aec_max_us,
                     reference_underflows, rtc.reference_overflows,
                     (unsigned)xStreamBufferBytesAvailable(rtc.reference_stream));
            ESP_LOGI(TAG, "AEC reference clock trim: samples=%" PRIu32,
                     reference_trimmed_samples);
            ESP_LOGI(TAG,
                     "AEC signal mean_abs: mic0=%u mic1=%u ref=%u out=%u samples=%u/%u",
                     speaking_capture_samples == 0
                         ? 0
                         : (unsigned)(speaking_mic0_abs / speaking_capture_samples),
                     speaking_capture_samples == 0
                         ? 0
                         : (unsigned)(speaking_mic1_abs / speaking_capture_samples),
                     speaking_aec_samples == 0
                         ? 0
                         : (unsigned)(speaking_ref_abs / speaking_aec_samples),
                     speaking_aec_samples == 0
                         ? 0
                         : (unsigned)(speaking_out_abs / speaking_aec_samples),
                     speaking_capture_samples, speaking_aec_samples);
            speaking_mic0_abs = 0;
            speaking_mic1_abs = 0;
            speaking_ref_abs = 0;
            speaking_out_abs = 0;
            speaking_capture_samples = 0;
            speaking_aec_samples = 0;
        }
        if (encoded_frames - last_logged_encoded_frames >= RTC_STACK_LOG_INTERVAL) {
            last_logged_encoded_frames = encoded_frames;
            ESP_LOGI(TAG, "microphone stack remaining: %u bytes",
                     (unsigned)uxTaskGetStackHighWaterMark(NULL));
        }
#if CONFIG_MIKU_RTI_OFFICIAL_CAPTURE_AEC
        // Direct source reads can be satisfied from the AFE cache immediately.
        // Pace each encoded 20 ms frame so CPU1's Idle task and the receive
        // path are not starved while the cache drains.
        vTaskDelay(1);
        ++capture_frames;
#else
        if (++capture_frames % RTC_IDLE_YIELD_INTERVAL == 0) {
            // ESP-SR HIGH_PERF plus Opus can keep CPU1 continuously runnable.
            // Let the watched Idle task run briefly without changing audio pacing.
            vTaskDelay(1);
        }
#endif
    }
    free(capture);
    free(capture_mono);
    free(mic_accumulator);
    free(clean_accumulator);
    free(encoded);
    vad_destroy(barge_vad);
#if CONFIG_MIKU_RTI_CAPTURE_DIAGNOSTIC
    free(capture_diag);
    free(diagnostic_reference);
#endif
    finish_worker(RTC_EVENT_MIC_DONE);
}

static void playback_task(void *arg)
{
    (void)arg;
    uint32_t decoded_frames = 0;
    uint32_t concealed_frames = 0;
    uint32_t concealment_failures = 0;
    uint32_t fec_decode_frames = 0;
    uint32_t fec_decode_failures = 0;
    uint32_t underflows = 0;
    uint32_t late_resets = 0;
    uint32_t clipped_samples = 0;
    uint32_t measured_samples = 0;
    uint32_t pcm_peak = 0;
    int64_t max_queue_age_us = 0;
    const size_t pcm_bytes = rtc.group_mode ? RTC_GROUP_PCM_BUFFER_MAX : RTC_PCM_BUFFER_MAX;
    uint8_t *pcm = alloc_audio_workspace(pcm_bytes);
    int16_t *reference_mono = alloc_audio_workspace(pcm_bytes);
    opus_packet_t *packet = alloc_audio_workspace(sizeof(*packet));
    bool logged_format = false;
    bool logged_group_buffer = false;
    bool playout_buffering = true;
    bool has_playback_pts = false;
    uint32_t last_playback_pts = 0;
    uint32_t concealed_since_packet = 0;
    uint32_t consecutive_underflow_plc = 0;
    TickType_t next_playback_tick = 0;
    if (pcm == NULL || reference_mono == NULL || packet == NULL) {
        ESP_LOGE(TAG, "playback buffer unavailable");
        rtc.audio_running = false;
        group_mark_unavailable();
        xEventGroupSetBits(rtc.events, RTC_EVENT_FAILED);
        free(pcm);
        free(reference_mono);
        free(packet);
        finish_worker(RTC_EVENT_PLAYBACK_DONE);
        return;
    }
    while (rtc.audio_running) {
        if (playout_buffering) {
            const UBaseType_t queued = uxQueueMessagesWaiting(rtc.playback_queue);
            // Audio RTP can arrive before the response.created data-channel
            // event changes the UI state. Do not let that ordering race bypass
            // the startup jitter buffer.
            const UBaseType_t target = rtc.group_mode ? 1 : RTC_PLAYBACK_PREBUFFER_FRAMES;
            if (queued < target) {
                // The S31 build uses a 100 Hz tick, so 5 ms rounds down to
                // zero. Always block for one tick to let IDLE1 feed the WDT.
                vTaskDelay(1);
                continue;
            }
            if (!rtc.group_mode || !logged_group_buffer) {
                ESP_LOGI(TAG, "playback buffered: queue=%u target=%u",
                         (unsigned)queued, (unsigned)target);
                logged_group_buffer = true;
            }
            playout_buffering = false;
            has_playback_pts = false;
            concealed_since_packet = 0;
            consecutive_underflow_plc = 0;
            next_playback_tick = 0;
        }
        const bool have_packet =
            xQueueReceive(rtc.playback_queue, packet, pdMS_TO_TICKS(20)) == pdTRUE;
        uint32_t plc_to_play = 0;
        uint32_t fec_to_play = 0;
        if (rtc.group_mode && !have_packet) {
            // A PTT burst can be shorter than the old 240 ms AI prebuffer.
            // Silence between speakers is intentional, not packet-loss audio.
            playout_buffering = true;
            next_playback_tick = 0;
            has_playback_pts = false;
            continue;
        }
        if (!have_packet) {
            if (next_playback_tick != 0 &&
                rti_app_get_state() == RTI_APP_STATE_AGENT_SPEAKING) {
                ++underflows;
                if (underflows == 1 || underflows % 10 == 0) {
                    ESP_LOGW(TAG, "playback queue underflow: count=%" PRIu32, underflows);
                }
                if (consecutive_underflow_plc < RTC_PLAYBACK_PLC_MAX_FRAMES) {
                    plc_to_play = 1;
                    ++consecutive_underflow_plc;
                    ++concealed_since_packet;
                } else {
                    // PLC is intended for short loss bursts. Rebuffer after
                    // 60 ms rather than synthesizing increasingly poor audio.
                    playout_buffering = true;
                    has_playback_pts = false;
                    concealed_since_packet = 0;
                    consecutive_underflow_plc = 0;
                    next_playback_tick = 0;
                    continue;
                }
            } else if (rti_app_get_state() != RTI_APP_STATE_AGENT_SPEAKING) {
                // Each response is a new playout burst. Re-arm the startup
                // buffer after the previous response drains; otherwise the
                // next response starts from an empty queue and immediately
                // underflows before its first packet burst arrives.
                playout_buffering = true;
                next_playback_tick = 0;
                has_playback_pts = false;
                concealed_since_packet = 0;
                consecutive_underflow_plc = 0;
            }
        } else {
            consecutive_underflow_plc = 0;
            const int64_t queue_age_us = esp_timer_get_time() - packet->received_us;
            if (rtc.group_mode && queue_age_us > RTC_GROUP_MAX_QUEUE_AGE_US) {
                has_playback_pts = false;
                next_playback_tick = 0;
                continue;
            }
            if (queue_age_us > max_queue_age_us) {
                max_queue_age_us = queue_age_us;
            }
            if (has_playback_pts && !rtc.group_mode) {
                const uint32_t pts_delta = packet->pts - last_playback_pts;
                if (pts_delta > 20 && pts_delta < 1000) {
                    uint32_t missing = pts_delta / 20 - 1;
                    if (missing > concealed_since_packet) {
                        missing -= concealed_since_packet;
                        if (missing <= RTC_PLAYBACK_PLC_MAX_FRAMES) {
                            // Opus in-band FEC for packet N is carried by
                            // packet N+1. Conceal older losses first, then
                            // recover the frame immediately before this packet.
                            plc_to_play = missing - 1;
                            fec_to_play = 1;
                        } else {
                            plc_to_play = RTC_PLAYBACK_PLC_MAX_FRAMES;
                        }
                    }
                }
            }
            last_playback_pts = packet->pts;
            has_playback_pts = true;
            concealed_since_packet = 0;
        }

        const uint32_t decode_count =
            plc_to_play + fec_to_play + (have_packet ? 1U : 0U);
        for (uint32_t decode_index = 0; decode_index < decode_count; ++decode_index) {
            const bool conceal = decode_index < plc_to_play;
            const bool decode_fec =
                fec_to_play != 0 && decode_index == plc_to_play;
            int frame_count = opus_decode(
                rtc.opus_decoder,
                conceal ? NULL : packet->data,
                conceal ? 0 : (int32_t)packet->size,
                (int16_t *)pcm,
                rtc.group_mode ? RTC_GROUP_OPUS_FRAME_SAMPLES : RTC_OPUS_FRAME_SAMPLES,
                decode_fec ? 1 : 0);
            bool fec_fallback_plc = false;
            if (decode_fec && frame_count <= 0) {
                ++fec_decode_failures;
                frame_count = opus_decode(rtc.opus_decoder, NULL, 0,
                                          (int16_t *)pcm,
                                          RTC_OPUS_FRAME_SAMPLES, 0);
                fec_fallback_plc = frame_count > 0;
            }
            if (frame_count <= 0) {
                if (conceal) {
                    ++concealment_failures;
                }
                continue;
            }
            const size_t sample_count = (size_t)frame_count * RTC_CAPTURE_CHANNELS;
            const size_t decoded_size = sample_count * sizeof(int16_t);
            const int16_t *decoded_pcm = (const int16_t *)pcm;
            for (size_t index = 0; index < sample_count; ++index) {
                const int32_t sample = decoded_pcm[index];
                const uint32_t magnitude =
                    sample < 0 ? (uint32_t)-sample : (uint32_t)sample;
                if (magnitude > pcm_peak) {
                    pcm_peak = magnitude;
                }
                if (magnitude >= 32760U) {
                    ++clipped_samples;
                }
            }
            measured_samples += (uint32_t)sample_count;
            if (!logged_format) {
                ESP_LOGI(TAG,
                         "decoded playback: rate=16000 channels=2 bits=16 frames=%u bytes=%u",
                         (unsigned)frame_count, (unsigned)decoded_size);
                logged_format = true;
            }
            if (rtc.group_mode) {
                const float rms = pcm_rms(decoded_pcm, sample_count);
                portENTER_CRITICAL(&group_state_lock);
                group_stats.decoded_samples += (uint64_t)frame_count;
                if (rms > group_stats.remote_rms_peak) {
                    group_stats.remote_rms_peak = rms;
                }
                portEXIT_CRITICAL(&group_state_lock);
            }
            const TickType_t frame_ticks = pdMS_TO_TICKS(20);
            if (!rtc.group_mode) {
                const TickType_t now = xTaskGetTickCount();
                if (next_playback_tick == 0 ||
                    (int32_t)(now - next_playback_tick) > (int32_t)frame_ticks) {
                    if (next_playback_tick != 0) {
                        ++late_resets;
                    }
                    next_playback_tick = now;
                } else if ((int32_t)(next_playback_tick - now) > 0) {
                    vTaskDelay(next_playback_tick - now);
                }
            }
            // Group packets can contain 2.5..120 ms of PCM. Blocking codec/I2S
            // writes provide the sample clock; a fixed 20 ms task delay would
            // stretch short packets and overlap long packets.
            queue_playback_reference((const int16_t *)pcm, (size_t)frame_count,
                                     RTC_CAPTURE_CHANNELS, reference_mono);
            if (esp_codec_dev_write(rtc.audio.speaker, pcm, (int)decoded_size) != 0) {
                ESP_LOGW(TAG, "speaker write failed");
            }
            rtc.playback_guard_until_tick =
                xTaskGetTickCount() + pdMS_TO_TICKS(RTC_PLAYBACK_TAIL_MS);
            next_playback_tick += frame_ticks;
            ++decoded_frames;
            if (decode_fec && !fec_fallback_plc) {
                ++fec_decode_frames;
            }
            if (conceal || fec_fallback_plc) {
                ++concealed_frames;
            }
            if (decoded_frames % RTC_PLAYBACK_STATS_INTERVAL == 0) {
                ESP_LOGI(TAG,
                         "playback stats: frames=%" PRIu32 " queue=%u underflow=%" PRIu32
                         " fec_decode=%" PRIu32 " fec_fail=%" PRIu32
                         " plc=%" PRIu32 " plc_fail=%" PRIu32
                         " input_gap=%" PRIu32 " late_reset=%" PRIu32
                         " peak=%u clipped=%" PRIu32 "/%" PRIu32
                         " max_queue_age=%" PRId64 "us",
                         decoded_frames,
                         (unsigned)uxQueueMessagesWaiting(rtc.playback_queue),
                         underflows, fec_decode_frames, fec_decode_failures,
                         concealed_frames, concealment_failures,
                         rtc.playback_input_gaps, late_resets,
                         pcm_peak, clipped_samples, measured_samples, max_queue_age_us);
                clipped_samples = 0;
                measured_samples = 0;
                pcm_peak = 0;
                max_queue_age_us = 0;
            }
            if (decoded_frames % RTC_STACK_LOG_INTERVAL == 0) {
                ESP_LOGI(TAG, "playback stack remaining: %u bytes",
                         (unsigned)uxTaskGetStackHighWaterMark(NULL));
            }
        }
    }
    free(pcm);
    free(reference_mono);
    free(packet);
    finish_worker(RTC_EVENT_PLAYBACK_DONE);
}

static esp_err_t start_audio(const board_audio_t *audio)
{
    rtc.audio = *audio;
    esp_opus_enc_config_t encoder_cfg = ESP_OPUS_ENC_CONFIG_DEFAULT();
    encoder_cfg.sample_rate = 16000;
    encoder_cfg.channel = 1;
    encoder_cfg.bitrate = 40000;
    encoder_cfg.frame_duration = ESP_OPUS_ENC_FRAME_DURATION_20_MS;
    encoder_cfg.application_mode = ESP_OPUS_ENC_APPLICATION_VOIP;
    encoder_cfg.complexity = 2;
    encoder_cfg.enable_vbr = true;
    if (esp_opus_enc_open(&encoder_cfg, sizeof(encoder_cfg), &rtc.opus_encoder) != ESP_AUDIO_ERR_OK ||
        esp_opus_enc_get_frame_size(rtc.opus_encoder, &rtc.encoder_input_size,
                                    &rtc.encoder_output_size) != ESP_AUDIO_ERR_OK) {
        ESP_LOGE(TAG, "could not initialize Opus encoder");
        return ESP_FAIL;
    }

    int decoder_error = 0;
    rtc.opus_decoder = opus_decoder_create(16000, RTC_CAPTURE_CHANNELS, &decoder_error);
    if (rtc.opus_decoder == NULL || decoder_error != 0) {
        ESP_LOGE(TAG, "could not initialize Opus decoder: %d", decoder_error);
        return ESP_FAIL;
    }

#if CONFIG_MIKU_RTI_CAPTURE_DIAGNOSTIC
    rtc.aec = NULL;
    rtc.aec_frame_samples = 0;
    rtc.aec_enabled = false;
    rtc.reference_stream = xStreamBufferCreateWithCaps(
        RTC_DIAG_REFERENCE_SAMPLES * sizeof(int16_t), 1,
        MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (rtc.reference_stream == NULL) {
        ESP_LOGE(TAG, "capture diagnostic reference buffer unavailable");
        return ESP_ERR_NO_MEM;
    }
    ESP_LOGW(TAG,
             "CAPTURE_DIAG active: raw ES8389 stereo ADC, AEC bypassed, "
             "uplink gated during playback");
#elif CONFIG_MIKU_RTI_OFFICIAL_CAPTURE_AEC
    rtc.aec = NULL;
    rtc.aec_frame_samples = 0;
    rtc.aec_enabled = false;
    const esp_err_t capture_err = official_capture_aec_start(audio);
    if (capture_err != ESP_OK) {
        return capture_err;
    }
#elif CONFIG_MIKU_RTI_AEC_RUNTIME
    if (persistent_aec == NULL) {
        persistent_aec = create_full_duplex_aec();
        if (persistent_aec != NULL) {
            persistent_aec_frame_samples = aec_get_chunksize(persistent_aec);
        }
    }
    rtc.aec = persistent_aec;
    rtc.aec_frame_samples = persistent_aec_frame_samples;
    if (rtc.aec != NULL && rtc.aec_frame_samples > 0 &&
        rtc.aec_frame_samples <= RTC_AEC_MAX_FRAME_SAMPLES) {
        const size_t reference_bytes = (size_t)rtc.aec_frame_samples * sizeof(int16_t) *
                                       RTC_AEC_REFERENCE_FRAMES;
        rtc.reference_stream = xStreamBufferCreateWithCaps(
            reference_bytes, 1, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    }
    if (rtc.aec == NULL || rtc.aec_frame_samples <= 0 ||
        rtc.aec_frame_samples > RTC_AEC_MAX_FRAME_SAMPLES || rtc.reference_stream == NULL) {
        if (rtc.aec != NULL) {
            aec_destroy(rtc.aec);
            if (rtc.aec == persistent_aec) {
                persistent_aec = NULL;
                persistent_aec_frame_samples = 0;
            }
        }
        rtc.aec = NULL;
        rtc.aec_frame_samples = 0;
        ESP_LOGW(TAG, "AEC unavailable; using half-duplex microphone gating");
    } else {
        rtc.aec_enabled = true;
        ESP_LOGI(TAG, "AEC active: %s, frame=%d samples, NLP=%s",
                 aec_get_mode_string(RTC_AEC_MODE), rtc.aec_frame_samples,
                 aec_get_nlp_string(AEC_NLP_LEVEL_AGGR));
    }
#else
    if (persistent_aec != NULL) {
        aec_destroy(persistent_aec);
        persistent_aec = NULL;
        persistent_aec_frame_samples = 0;
    }
    rtc.aec = NULL;
    rtc.aec_frame_samples = 0;
    rtc.aec_enabled = false;
    ESP_LOGW(TAG, "AEC disabled for receive/playback A/B test; using half-duplex microphone gating");
#endif

    rtc.playback_queue = xQueueCreateWithCaps(rtc.group_mode ? RTC_GROUP_PLAYBACK_QUEUE_FRAMES
                                                            : RTC_PLAYBACK_QUEUE_FRAMES,
                                              sizeof(opus_packet_t),
                                              MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (rtc.playback_queue == NULL) {
        return ESP_ERR_NO_MEM;
    }
    rtc.audio_running = true;
    ESP_LOGI(TAG, "Opus frame buffers: mono input=%d output=%d", rtc.encoder_input_size,
             rtc.encoder_output_size);
    const UBaseType_t audio_stack_caps = MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT;
    if (xTaskCreateWithCaps(playback_task, "rti_playback", RTC_PLAYBACK_STACK_SIZE, NULL,
                            RTC_PLAYBACK_TASK_PRIORITY,
                            &rtc.playback_task, audio_stack_caps) != pdPASS ||
#if CONFIG_MIKU_RTI_OFFICIAL_CAPTURE_AEC
        xTaskCreatePinnedToCoreWithCaps(microphone_task, "rti_microphone",
                                        RTC_MIC_STACK_SIZE, NULL, 10,
                                        &rtc.mic_task, 1,
                                        audio_stack_caps) != pdPASS) {
#else
        xTaskCreateWithCaps(microphone_task, "rti_microphone", RTC_MIC_STACK_SIZE, NULL, 6,
                            &rtc.mic_task, audio_stack_caps) != pdPASS) {
#endif
        rtc.audio_running = false;
        return ESP_ERR_NO_MEM;
    }
    ESP_LOGI(TAG, "Opus audio active: 16 kHz mono uplink/stereo playback, 20 ms frames, "
                  "PSRAM stacks");
    return ESP_OK;
}

static void peer_loop_task(void *arg)
{
    (void)arg;
    uint32_t calls = 0;
    uint32_t slow_calls = 0;
    int64_t total_us = 0;
    int64_t max_call_us = 0;
    int64_t max_start_gap_us = 0;
    int64_t last_start_us = 0;
    int64_t report_at_us = esp_timer_get_time() + RTC_PIPELINE_STATS_INTERVAL_US;
    while (rtc.running) {
        const int64_t start_us = esp_timer_get_time();
        if (last_start_us != 0 && start_us - last_start_us > max_start_gap_us) {
            max_start_gap_us = start_us - last_start_us;
        }
        last_start_us = start_us;
        esp_peer_main_loop(rtc.peer);
        const int64_t elapsed_us = esp_timer_get_time() - start_us;
        ++calls;
        total_us += elapsed_us;
        if (elapsed_us > max_call_us) {
            max_call_us = elapsed_us;
        }
        if (elapsed_us >= 30000) {
            ++slow_calls;
        }
        if (esp_timer_get_time() >= report_at_us) {
            ESP_LOGI(TAG,
                     "peer loop stats: calls=%" PRIu32 " avg=%" PRId64
                     "us max=%" PRId64 "us slow30=%" PRIu32
                     " max_start_gap=%" PRId64 "us data_queue=%u audio_gaps=%" PRIu32,
                     calls, calls == 0 ? 0 : total_us / calls, max_call_us,
                     slow_calls, max_start_gap_us,
                     rtc.data_queue == NULL
                         ? 0U
                         : (unsigned)uxQueueMessagesWaiting(rtc.data_queue),
                     rtc.playback_input_gaps);
            calls = 0;
            slow_calls = 0;
            total_us = 0;
            max_call_us = 0;
            max_start_gap_us = 0;
            report_at_us = esp_timer_get_time() + RTC_PIPELINE_STATS_INTERVAL_US;
        }
        vTaskDelay(pdMS_TO_TICKS(10));
    }
    finish_worker(RTC_EVENT_LOOP_DONE);
}

static void data_event_task(void *arg)
{
    (void)arg;
    uint32_t processed = 0;
    uint32_t slow_events = 0;
    int64_t total_us = 0;
    int64_t max_event_us = 0;
    int64_t report_at_us = esp_timer_get_time() + RTC_PIPELINE_STATS_INTERVAL_US;
    while (rtc.running) {
        data_event_t *event = NULL;
        if (xQueueReceive(rtc.data_queue, &event, pdMS_TO_TICKS(20)) == pdTRUE &&
            event != NULL) {
            const int64_t start_us = esp_timer_get_time();
            handle_realtime_event(event->data, event->size);
            const int64_t elapsed_us = esp_timer_get_time() - start_us;
            free(event);
            ++processed;
            total_us += elapsed_us;
            if (elapsed_us > max_event_us) {
                max_event_us = elapsed_us;
            }
            if (elapsed_us >= 20000) {
                ++slow_events;
            }
        }
        flush_pending_assistant_transcript();
        if (esp_timer_get_time() >= report_at_us) {
            ESP_LOGI(TAG,
                     "DataChannel worker stats: events=%" PRIu32
                     " avg=%" PRId64 "us max=%" PRId64
                     "us slow20=%" PRIu32 " queue=%u drops=%" PRIu32,
                     processed, processed == 0 ? 0 : total_us / processed,
                     max_event_us, slow_events,
                     (unsigned)uxQueueMessagesWaiting(rtc.data_queue),
                     rtc.data_event_drops);
            processed = 0;
            slow_events = 0;
            total_us = 0;
            max_event_us = 0;
            report_at_us = esp_timer_get_time() + RTC_PIPELINE_STATS_INTERVAL_US;
        }
    }
    data_event_t *event = NULL;
    while (xQueueReceive(rtc.data_queue, &event, 0) == pdTRUE) {
        free(event);
    }
    finish_worker(RTC_EVENT_DATA_DONE);
}

void webrtc_peer_stop(void)
{
    webrtc_peer_group_set_sending(false, 0);
    group_mark_unavailable();
    if (rtc.peer == NULL && rtc.events == NULL && rtc.lock == NULL && rtc.data_queue == NULL) {
        portENTER_CRITICAL(&group_state_lock);
        group_mode_active = false;
        portEXIT_CRITICAL(&group_state_lock);
        return;
    }
    const uint32_t cycle = peer_cycle;
    log_heap_state("stop_entry", cycle);
    bool shutdown_complete = true;
    EventBits_t expected_retired = 0;
    rtc.audio_running = false;
    if (rtc.events != NULL) {
        EventBits_t wait_for = 0;
        if (rtc.mic_task != NULL) {
            wait_for |= RTC_EVENT_MIC_DONE;
        }
        if (rtc.playback_task != NULL) {
            wait_for |= RTC_EVENT_PLAYBACK_DONE;
        }
        expected_retired |= wait_for;
        if (wait_for != 0) {
            const EventBits_t stopped = xEventGroupWaitBits(
                rtc.events, wait_for, pdFALSE, pdTRUE, pdMS_TO_TICKS(3000));
            ESP_LOGI(TAG,
                     "RTC stop cycle=%" PRIu32 " audio task bits expected=0x%02x observed=0x%02x",
                     cycle, (unsigned)wait_for, (unsigned)(stopped & wait_for));
            if ((stopped & wait_for) != wait_for) {
                ESP_LOGE(TAG, "RTC stop cycle=%" PRIu32 " audio task shutdown timed out", cycle);
                shutdown_complete = false;
            }
        }
    }
    rtc.running = false;
    if ((rtc.loop_task != NULL || rtc.data_task != NULL) && rtc.events != NULL) {
        EventBits_t wait_for = 0;
        if (rtc.loop_task != NULL) {
            wait_for |= RTC_EVENT_LOOP_DONE;
        }
        if (rtc.data_task != NULL) {
            wait_for |= RTC_EVENT_DATA_DONE;
        }
        expected_retired |= wait_for;
        const EventBits_t stopped = xEventGroupWaitBits(
            rtc.events, wait_for, pdFALSE, pdTRUE, pdMS_TO_TICKS(3000));
        ESP_LOGI(TAG,
                 "RTC stop cycle=%" PRIu32
                 " worker bits expected=0x%02x observed=0x%02x",
                 cycle, (unsigned)wait_for, (unsigned)(stopped & wait_for));
        if ((stopped & wait_for) != wait_for) {
            ESP_LOGE(TAG, "RTC stop cycle=%" PRIu32 " worker shutdown timed out", cycle);
            shutdown_complete = false;
        }
    }
    const TickType_t retirement_started = xTaskGetTickCount();
    while (shutdown_complete && expected_retired != 0) {
        portENTER_CRITICAL(&group_state_lock);
        const EventBits_t retired = rtc.retired_workers;
        portEXIT_CRITICAL(&group_state_lock);
        if ((retired & expected_retired) == expected_retired) {
            break;
        }
        if (xTaskGetTickCount() - retirement_started >= pdMS_TO_TICKS(3000)) {
            shutdown_complete = false;
            break;
        }
        vTaskDelay(1);
    }
    if (!shutdown_complete) {
        // Keep the peer, events, locks, queues, and codecs alive until every
        // owner has acknowledged shutdown. start rejects this retained context.
        ESP_LOGE(TAG, "RTC cleanup deferred: tasks are still using the retained context");
        return;
    }
#if CONFIG_MIKU_RTI_OFFICIAL_CAPTURE_AEC
    official_capture_aec_stop();
#endif
    if (rtc.peer != NULL) {
        const int close_ret = esp_peer_close(rtc.peer);
        ESP_LOGI(TAG, "RTC stop cycle=%" PRIu32 " esp_peer_close=%d", cycle, close_ret);
        log_heap_state("after_peer_close", cycle);
    }
    if (rtc.opus_encoder != NULL) {
        esp_opus_enc_close(rtc.opus_encoder);
    }
    if (rtc.opus_decoder != NULL) {
        opus_decoder_destroy(rtc.opus_decoder);
    }
    if (rtc.aec != NULL && rtc.aec != persistent_aec) {
        aec_destroy(rtc.aec);
    }
    if (rtc.reference_stream != NULL) {
        vStreamBufferDeleteWithCaps(rtc.reference_stream);
    }
    if (rtc.playback_queue != NULL) {
        vQueueDeleteWithCaps(rtc.playback_queue);
    }
    if (rtc.data_queue != NULL) {
        data_event_t *event = NULL;
        while (xQueueReceive(rtc.data_queue, &event, 0) == pdTRUE) {
            free(event);
        }
        vQueueDeleteWithCaps(rtc.data_queue);
    }
    free(rtc.local_sdp);
    if (rtc.lock != NULL) {
        vSemaphoreDelete(rtc.lock);
    }
    if (rtc.events != NULL) {
        vEventGroupDelete(rtc.events);
    }
    memset(&rtc, 0, sizeof(rtc));
    portENTER_CRITICAL(&group_state_lock);
    group_mode_active = false;
    portEXIT_CRITICAL(&group_state_lock);
    log_heap_state("stop_complete", cycle);
    ESP_LOGI(TAG, "PeerConnection closed");
}

static esp_err_t start_peer(const rti_session_t *session, const board_audio_t *audio,
                            webrtc_group_exchange_cb_t exchange, void *exchange_context)
{
    if ((session == NULL && exchange == NULL) || audio == NULL ||
        audio->microphone == NULL || audio->speaker == NULL ||
        rtc.peer != NULL || rtc.events != NULL || rtc.lock != NULL) {
        return ESP_ERR_INVALID_STATE;
    }
    const uint32_t cycle = ++peer_cycle;
    log_heap_state("start_entry", cycle);
    rtc.group_mode = exchange != NULL;
    if (rtc.group_mode) {
        portENTER_CRITICAL(&group_state_lock);
        group_mode_active = true;
        group_transport_connected = false;
        group_audio_ready = false;
        group_send_enabled = false;
        group_send_expires_at_us = 0;
        ++group_send_epoch;
        memset(&group_stats, 0, sizeof(group_stats));
        portEXIT_CRITICAL(&group_state_lock);
    }
    rtc.events = xEventGroupCreate();
    rtc.lock = xSemaphoreCreateMutex();
    if (!rtc.group_mode) {
        rtc.data_queue = xQueueCreateWithCaps(
            RTC_DATA_EVENT_QUEUE_LENGTH, sizeof(data_event_t *),
            MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    }
    if (rtc.events == NULL || rtc.lock == NULL || (!rtc.group_mode && rtc.data_queue == NULL)) {
        webrtc_peer_stop();
        return ESP_ERR_NO_MEM;
    }

    esp_peer_default_cfg_t default_cfg = {
        .agent_recv_timeout = 100,
        .data_ch_cfg = {
            .recv_cache_size = 16 * 1024,
            .send_cache_size = 4 * 1024,
        },
        .rtp_cfg = {
            .audio_recv_jitter = {
                .cache_timeout = 100,
                .resend_delay = 20,
                // Keep the official audio cache size so the peer stack can
                // retain enough RTP metadata for NACK/retransmission. This
                // allocation is larger than SPIRAM_MALLOC_ALWAYSINTERNAL and
                // therefore lives in PSRAM instead of scarce internal SRAM.
                .cache_size = 100 * 1024,
            },
            .send_pool_size = 4 * 1024,
            .send_queue_num = 12,
        },
        .max_candidates = 32,
    };
    esp_peer_cfg_t peer_cfg = {
        .role = ESP_PEER_ROLE_CONTROLLING,
        .audio_dir = ESP_PEER_MEDIA_DIR_SEND_RECV,
        .audio_info = {
            .codec = ESP_PEER_AUDIO_CODEC_OPUS,
            .sample_rate = 48000,
            .channel = 2,
        },
        .video_dir = ESP_PEER_MEDIA_DIR_NONE,
        .enable_data_channel = !rtc.group_mode,
        .manual_ch_create = !rtc.group_mode,
        .no_auto_reconnect = true,
        .on_state = peer_state_handler,
        .on_msg = peer_msg_handler,
        .on_audio_info = peer_audio_info_handler,
        .on_audio_data = peer_audio_data_handler,
        .on_channel_open = peer_channel_open_handler,
        .on_data = peer_data_handler,
        .on_channel_close = peer_channel_close_handler,
        .ctx = &rtc,
        .extra_cfg = &default_cfg,
        .extra_size = sizeof(default_cfg),
    };
    int ret = esp_peer_open(&peer_cfg, esp_peer_get_default_impl(), &rtc.peer);
    if (ret != ESP_PEER_ERR_NONE) {
        ESP_LOGE(TAG, "PeerConnection open failed: %d", ret);
        webrtc_peer_stop();
        return ESP_FAIL;
    }
    log_heap_state("after_peer_open", cycle);

    rtc.running = true;
    if ((!rtc.group_mode && xTaskCreateWithCaps(
            data_event_task, "rti_events", RTC_DATA_EVENT_STACK_SIZE, NULL,
            RTC_DATA_EVENT_TASK_PRIORITY, &rtc.data_task,
            MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT) != pdPASS) ||
        xTaskCreateWithCaps(peer_loop_task, "rti_peer", 20 * 1024, NULL,
                            RTC_PEER_TASK_PRIORITY,
                            &rtc.loop_task,
                            MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT) != pdPASS) {
        webrtc_peer_stop();
        return ESP_ERR_NO_MEM;
    }
    ret = esp_peer_new_connection(rtc.peer);
    if (ret != ESP_PEER_ERR_NONE) {
        ESP_LOGE(TAG, "new PeerConnection failed: %d", ret);
        webrtc_peer_stop();
        return ESP_FAIL;
    }

    EventBits_t bits = xEventGroupWaitBits(rtc.events, RTC_EVENT_LOCAL_SDP | RTC_EVENT_FAILED,
                                           pdFALSE, pdFALSE, pdMS_TO_TICKS(RTC_SIGNAL_TIMEOUT_MS));
    if ((bits & RTC_EVENT_LOCAL_SDP) == 0 || (bits & RTC_EVENT_FAILED) != 0) {
        ESP_LOGE(TAG, "timed out gathering local SDP");
        webrtc_peer_stop();
        return ESP_ERR_TIMEOUT;
    }

    xSemaphoreTake(rtc.lock, portMAX_DELAY);
    char *offer = rtc.local_sdp == NULL ? NULL : strdup(rtc.local_sdp);
    xSemaphoreGive(rtc.lock);
    if (offer == NULL) {
        webrtc_peer_stop();
        return ESP_ERR_NO_MEM;
    }
    if (rtc.group_mode && (strstr(offer, "a=candidate:") == NULL ||
                           strstr(offer, "a=ice-ufrag:") == NULL ||
                           strstr(offer, "a=ice-pwd:") == NULL)) {
        // LAN signaling is non-trickle: never submit an empty/partial offer.
        ESP_LOGE(TAG, "group SDP is missing gathered ICE candidates or credentials");
        free(offer);
        webrtc_peer_stop();
        return ESP_ERR_INVALID_RESPONSE;
    }
    const int setup_replacements = prefer_dtls_client_role(offer);
    if (setup_replacements == 0) {
        ESP_LOGE(TAG, "local SDP has no actpass DTLS setup attribute");
        free(offer);
        webrtc_peer_stop();
        return ESP_ERR_INVALID_RESPONSE;
    }
    ESP_LOGI(TAG, "requesting DTLS client role in %d SDP media section(s)", setup_replacements);
    esp_err_t recovery_err = advertise_audio_recovery(&offer);
    if (recovery_err != ESP_OK) {
        ESP_LOGE(TAG, "could not advertise audio recovery: %s",
                 esp_err_to_name(recovery_err));
        free(offer);
        webrtc_peer_stop();
        return recovery_err;
    }
    log_audio_sdp_features("offer", offer);
    char *answer = NULL;
    esp_err_t err = rtc.group_mode ? exchange(offer, &answer, exchange_context)
                                   : rti_session_exchange_sdp(session, offer, &answer);
    free(offer);
    if (err != ESP_OK || answer == NULL || answer[0] == '\0') {
        free(answer);
        webrtc_peer_stop();
        return err == ESP_OK ? ESP_ERR_INVALID_RESPONSE : err;
    }
    log_audio_sdp_features("answer", answer);
    esp_peer_msg_t answer_msg = {
        .type = ESP_PEER_MSG_TYPE_SDP,
        .data = (uint8_t *)answer,
        .size = (int)strlen(answer),
    };
    ret = esp_peer_send_msg(rtc.peer, &answer_msg);
    free(answer);
    if (ret != ESP_PEER_ERR_NONE) {
        ESP_LOGE(TAG, "apply remote SDP failed: %d", ret);
        webrtc_peer_stop();
        return ESP_FAIL;
    }

    const EventBits_t ready_event = rtc.group_mode ? RTC_EVENT_CONNECTED : RTC_EVENT_DATA_OPEN;
    bits = xEventGroupWaitBits(rtc.events, ready_event | RTC_EVENT_FAILED,
                               pdFALSE, pdFALSE, pdMS_TO_TICKS(RTC_SIGNAL_TIMEOUT_MS));
    if ((bits & ready_event) == 0 || (bits & RTC_EVENT_FAILED) != 0) {
        ESP_LOGE(TAG, "timed out waiting for %s", rtc.group_mode ? "group peer" : "oai-events DataChannel");
        webrtc_peer_stop();
        return ESP_ERR_TIMEOUT;
    }
    err = start_audio(audio);
    if (err != ESP_OK) {
        webrtc_peer_stop();
        return err;
    }
    log_heap_state("audio_active", cycle);
    if (rtc.group_mode) {
        portENTER_CRITICAL(&group_state_lock);
        const bool connected = group_transport_connected && rtc.audio_running;
        group_audio_ready = connected;
        portEXIT_CRITICAL(&group_state_lock);
        if (!connected) {
            webrtc_peer_stop();
            return ESP_FAIL;
        }
        ESP_LOGI(TAG, "group PeerConnection audio active; uplink admission closed");
        return ESP_OK;
    }
    err = send_welcome_prompt();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "could not request welcome response");
        webrtc_peer_stop();
        return err;
    }
    ESP_LOGI(TAG, "RTI PeerConnection and DataChannel are active");
    return ESP_OK;
}


esp_err_t webrtc_peer_start(const rti_session_t *session, const board_audio_t *audio)
{
    return start_peer(session, audio, NULL, NULL);
}

esp_err_t webrtc_peer_start_group(const board_audio_t *audio,
                                   webrtc_group_exchange_cb_t exchange, void *context)
{
    if (exchange == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    return start_peer(NULL, audio, exchange, context);
}
