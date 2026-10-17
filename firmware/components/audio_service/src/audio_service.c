/* One worker owns the file, decoder and codec. UI commands never free worker
 * resources or wait for I2S. The newest command replaces pending commands. */
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include "sdkconfig.h"
#include "audio_service.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "esp_audio_simple_dec.h"
#include "esp_audio_dec_default.h"
#include "esp_audio_simple_dec_default.h"
#include "esp_codec_dev.h"
#include "board_adapter.h"
#define INPUT_BYTES 4096
#define PCM_BYTES (1152 * 2 * 2 * 2)
typedef struct { uint32_t serial; char path[192]; } audio_command_t;
static const char *TAG = "audio";
static QueueHandle_t s_commands;
static SemaphoreHandle_t s_command_lock;
static portMUX_TYPE s_lock = portMUX_INITIALIZER_UNLOCKED;
static audio_state_t s_state = AUDIO_STOPPED;
static uint32_t s_serial, s_total_ms;
static uint64_t s_samples;
static bool s_worker_busy;
static uint16_t s_pcm_peak;  /* Only the audio worker reads/writes this diagnostic. */
static uint8_t s_volume = 60;
static audio_state_cb s_cb;
static void *s_cb_user;
extern const uint8_t reference_mp3_start[] asm("_binary_reference_mp3_start");
extern const uint8_t reference_mp3_end[] asm("_binary_reference_mp3_end");
/* 界面音效与语音提示（clips 目录下的 mp3，契约格式 24kHz/mono/64kbps） */
#define CLIP_SYMS(n) extern const uint8_t n##_mp3_start[] asm("_binary_" #n "_mp3_start"); \
                     extern const uint8_t n##_mp3_end[] asm("_binary_" #n "_mp3_end");
CLIP_SYMS(click) CLIP_SYMS(shutter) CLIP_SYMS(prompt_looking) CLIP_SYMS(prompt_ready)
CLIP_SYMS(prompt_done) CLIP_SYMS(prompt_rejected)
static const struct { const uint8_t *start, *end; } k_clips[AUDIO_CLIP_COUNT] = {
    [AUDIO_CLIP_CLICK] = { click_mp3_start, click_mp3_end },
    [AUDIO_CLIP_SHUTTER] = { shutter_mp3_start, shutter_mp3_end },
    [AUDIO_CLIP_PROMPT_LOOKING] = { prompt_looking_mp3_start, prompt_looking_mp3_end },
    [AUDIO_CLIP_PROMPT_READY] = { prompt_ready_mp3_start, prompt_ready_mp3_end },
    [AUDIO_CLIP_PROMPT_DONE] = { prompt_done_mp3_start, prompt_done_mp3_end },
    [AUDIO_CLIP_PROMPT_REJECTED] = { prompt_rejected_mp3_start, prompt_rejected_mp3_end },
};
static uint32_t s_clip_serial;   /* 最近一次片段命令的 serial（0 = 无） */
static int s_clip_id = -1;       /* 该片段的 ID */
static bool current(uint32_t serial)
{
    portENTER_CRITICAL(&s_lock);
    bool yes = serial == s_serial;
    portEXIT_CRITICAL(&s_lock);
    return yes;
}
static void set_state(uint32_t serial, audio_state_t state)
{
    portENTER_CRITICAL(&s_lock);
    bool changed = serial == s_serial && s_state != state;
    if (changed) s_state = state;
    portEXIT_CRITICAL(&s_lock);
    if (changed && s_cb) s_cb(state, s_cb_user);
}
static bool checkpoint(uint32_t serial)
{
    while (current(serial)) {
        if (audio_get_state() != AUDIO_PAUSED) return true;
        vTaskDelay(pdMS_TO_TICKS(10));
    }
    return false;
}
static bool output_pcm(uint32_t serial, esp_codec_dev_handle_t codec,
                       esp_audio_simple_dec_handle_t dec, uint8_t *pcm, uint32_t bytes)
{
    if (!bytes) return true;
    esp_audio_simple_dec_info_t info = {0};
    if (esp_audio_simple_dec_get_info(dec, &info) != ESP_AUDIO_ERR_OK ||
        info.sample_rate != 24000 || info.channel != 1 || info.bits_per_sample != 16 ||
        bytes > PCM_BYTES / 2 || bytes % 2) {
        ESP_LOGE(TAG, "MP3 must be mono 24kHz/16bit, decoded bytes=%lu", (unsigned long)bytes);
        return false;
    }
    int16_t *samples = (int16_t *)pcm;
    for (uint32_t i=0; i<bytes/2; ++i) {
        int value=samples[i];
        uint16_t magnitude=(uint16_t)(value < 0 ? -value : value);
        if (magnitude > s_pcm_peak) s_pcm_peak=magnitude;
    }
    for (int i = (int)(bytes / 2) - 1; i >= 0; --i) {
        samples[2 * i] = samples[i];
        samples[2 * i + 1] = samples[i];
    }
    bytes *= 2;
    for (uint32_t offset = 0; offset < bytes;) {
        if (!checkpoint(serial)) return true;
        portENTER_CRITICAL(&s_lock);
        uint8_t volume = s_volume;
        portEXIT_CRITICAL(&s_lock);
        if (esp_codec_dev_set_out_vol(codec, volume) != ESP_CODEC_DEV_OK) {
            ESP_LOGE(TAG, "codec volume update failed");
            return false;
        }
        uint32_t count = bytes - offset > 1024 ? 1024 : bytes - offset;
        /* esp_codec_dev_write returns a status, never a byte count. */
        int status = esp_codec_dev_write(codec, pcm + offset, (int)count);
        if (status != ESP_CODEC_DEV_OK) {
            ESP_LOGE(TAG, "codec write failed: %d", status);
            return false;
        }
        portENTER_CRITICAL(&s_lock);
        if (serial == s_serial) s_samples += count / 4;
        portEXIT_CRITICAL(&s_lock);
        offset += count;
    }
    return true;
}
/* 静音调试构建：codec 每次打开后 DAC 静音（功放另由板级常关），双重保险 */
static void silence_if_debug(esp_codec_dev_handle_t codec)
{
#if CONFIG_STORY_DEBUG_SILENT
    esp_codec_dev_set_out_mute(codec, true);
#else
    (void)codec;
#endif
}

static void play(const audio_command_t *cmd)
{
    FILE *file = NULL;
    uint8_t *input = NULL, *pcm = NULL;
    esp_audio_simple_dec_handle_t dec = NULL;
    esp_codec_dev_handle_t codec = board_adapter_audio_codec();
    bool opened = false, failed = false, decoded = false;
    uint64_t submitted = 0;
    esp_audio_simple_dec_cfg_t cfg = { .dec_type = ESP_AUDIO_SIMPLE_DEC_TYPE_MP3 };
    if (!current(cmd->serial)) return;
    s_pcm_peak = 0;
    input = malloc(INPUT_BYTES);
    pcm = malloc(PCM_BYTES);
    if (!strncmp(cmd->path, "@clip:", 6)) {
        int id = atoi(cmd->path + 6);
        if (id >= 0 && id < AUDIO_CLIP_COUNT)
            file = fmemopen((void *)k_clips[id].start, (size_t)(k_clips[id].end - k_clips[id].start), "rb");
    } else if (!strcmp(cmd->path, "@diagnostic-mp3")) {
        /* The generated fixture is read-only in Flash. Use the exact same
         * fread/decode/output path as a story, without writing to the TF card. */
        file = fmemopen((void *)reference_mp3_start,
                       (size_t)(reference_mp3_end-reference_mp3_start), "rb");
    } else file = fopen(cmd->path, "rb");
    if (!codec || !input || !pcm || !file ||
        esp_audio_simple_dec_open(&cfg, &dec) != ESP_AUDIO_ERR_OK) {
        failed = true;
        goto done;
    }
    esp_codec_dev_sample_info_t format = { .sample_rate = 24000, .channel = 2, .bits_per_sample = 16 };
    if (esp_codec_dev_open(codec, &format) != ESP_CODEC_DEV_OK) { failed = true; goto done; }
    opened = true;
    silence_if_debug(codec);
    if (!current(cmd->serial)) goto done;
    if (board_adapter_speaker_enable(true) != ESP_OK) { failed = true; goto done; }
    set_state(cmd->serial, AUDIO_PLAYING);
    size_t pending = 0;
    for (;;) {
        if (!checkpoint(cmd->serial)) break;
        size_t n = fread(input + pending, 1, INPUT_BYTES - pending, file);
        if (ferror(file)) { failed = true; break; }
        bool eos = feof(file);
        esp_audio_simple_dec_raw_t raw = { .buffer = input, .len = pending + n, .eos = eos };
        unsigned flush_count = 0;
        for (;;) {
            if (!checkpoint(cmd->serial)) goto done;
            esp_audio_simple_dec_out_t out = { .buffer = pcm, .len = PCM_BYTES / 2 };
            raw.consumed = 0;
            esp_audio_err_t status = esp_audio_simple_dec_process(dec, &raw, &out);
            if (status == ESP_AUDIO_ERR_DATA_LACK && !out.decoded_size && !raw.len && eos && decoded) break;
            if (status != ESP_AUDIO_ERR_OK ||
                raw.consumed > raw.len || out.decoded_size > PCM_BYTES / 2) {
                ESP_LOGE(TAG, "decode error=%d input=%lu consumed=%lu output=%lu eos=%d",
                         status, (unsigned long)raw.len, (unsigned long)raw.consumed,
                         (unsigned long)out.decoded_size, eos);
                failed = true; goto done;
            }
            if (out.decoded_size && !decoded) {
                esp_audio_simple_dec_info_t info = {0};
                esp_audio_simple_dec_get_info(dec, &info);
                ESP_LOGI(TAG, "first PCM serial=%lu rate=%lu channels=%u bits=%u",
                         (unsigned long)cmd->serial, (unsigned long)info.sample_rate,
                         (unsigned)info.channel, (unsigned)info.bits_per_sample);
                decoded = true;
            }
            if (!output_pcm(cmd->serial, codec, dec, pcm, out.decoded_size)) { failed = true; goto done; }
            raw.buffer += raw.consumed;
            raw.len -= raw.consumed;
            if (!raw.consumed && !out.decoded_size) break;
            if (!raw.consumed && ++flush_count > 32) { failed = true; goto done; }
            if (!raw.len && !eos) break;
        }
        pending = raw.len;
        if (eos) { if (pending) failed = true; break; }
        if (pending == INPUT_BYTES) { failed = true; break; }
        memmove(input, raw.buffer, pending);
    }
done:
    portENTER_CRITICAL(&s_lock);
    if (cmd->serial == s_serial) submitted = s_samples;
    portEXIT_CRITICAL(&s_lock);
    board_adapter_speaker_enable(false);
    if (opened) esp_codec_dev_close(codec);
    if (dec) esp_audio_simple_dec_close(dec);
    if (file) fclose(file);
    free(input);
    free(pcm);
    set_state(cmd->serial, failed || !decoded ? AUDIO_ERROR : AUDIO_STOPPED);
    ESP_LOGI(TAG, "session end serial=%lu failed=%d cancelled=%d samples=%llu pcm_peak=%u",
             (unsigned long)cmd->serial, failed || !decoded, !current(cmd->serial),
             (unsigned long long)submitted, (unsigned)s_pcm_peak);
}
static void play_test_tone(const audio_command_t *cmd)
{
    esp_codec_dev_handle_t codec = board_adapter_audio_codec();
    esp_codec_dev_sample_info_t format = { .sample_rate=24000, .channel=2, .bits_per_sample=16 };
    if (!current(cmd->serial)) return;
    if (!codec || esp_codec_dev_open(codec, &format) != ESP_CODEC_DEV_OK) {
        set_state(cmd->serial, AUDIO_ERROR); return;
    }
    silence_if_debug(codec);
    bool failed = false;
    int16_t pcm[256 * 2];
    portENTER_CRITICAL(&s_lock);
    uint8_t initial_volume = s_volume;
    portEXIT_CRITICAL(&s_lock);
    if (esp_codec_dev_set_out_vol(codec, initial_volume) != ESP_CODEC_DEV_OK ||
        board_adapter_speaker_enable(true) != ESP_OK) {
        board_adapter_speaker_enable(false);
        esp_codec_dev_close(codec);
        set_state(cmd->serial, AUDIO_ERROR); return;
    }
    set_state(cmd->serial, AUDIO_PLAYING);
    ESP_LOGI(TAG, "reference tone serial=%lu 24kHz/stereo/16bit peak=12000 duration=3000ms",
             (unsigned long)cmd->serial);
    ESP_LOGI(TAG, "PA GPIO53 readback=%d", board_adapter_speaker_level());
    /* Read-only codec evidence runs on its owning task, never from the UI. */
    const uint8_t diagnostic_regs[] = {0x00,0x01,0x09,0x0d,0x0e,0x12,0x31,0x32,0x37};
    for (unsigned i=0; i<sizeof(diagnostic_regs); ++i) {
        int value=0;
        int status=esp_codec_dev_read_reg(codec, diagnostic_regs[i], &value);
        ESP_LOGI(TAG, "ES8311 reg=0x%02x value=0x%02x status=%d", diagnostic_regs[i], value, status);
    }
    for (uint32_t sample = 0; sample < 72000 && checkpoint(cmd->serial); sample += 256) {
        uint32_t count = 72000 - sample > 256 ? 256 : 72000 - sample;
        for (uint32_t i=0; i<count; ++i) {
            float frequency = (sample+i)/24000 < 2 ? 440.0f : 660.0f;
            int16_t value = (int16_t)(12000 * sinf(6.2831853f * frequency * (sample+i) / 24000.0f));
            pcm[2*i] = pcm[2*i+1] = value;
        }
        portENTER_CRITICAL(&s_lock);
        uint8_t volume = s_volume;
        portEXIT_CRITICAL(&s_lock);
        if (esp_codec_dev_set_out_vol(codec, volume) != ESP_CODEC_DEV_OK ||
            esp_codec_dev_write(codec, pcm, count * 4) != ESP_CODEC_DEV_OK) { failed=true; break; }
        portENTER_CRITICAL(&s_lock);
        if (cmd->serial == s_serial) s_samples += count;
        portEXIT_CRITICAL(&s_lock);
    }
    board_adapter_speaker_enable(false);
    esp_codec_dev_close(codec);
    set_state(cmd->serial, failed ? AUDIO_ERROR : AUDIO_STOPPED);
    ESP_LOGI(TAG, "reference tone end failed=%d cancelled=%d", failed, !current(cmd->serial));
}

static void audio_worker(void *arg)
{
    (void)arg;
    audio_command_t cmd;
    for (;;) {
        if (xQueueReceive(s_commands, &cmd, portMAX_DELAY) == pdTRUE && cmd.path[0]) {
            portENTER_CRITICAL(&s_lock); s_worker_busy = true; portEXIT_CRITICAL(&s_lock);
            if (!strcmp(cmd.path, "@diagnostic-tone")) play_test_tone(&cmd);
            else play(&cmd);
            portENTER_CRITICAL(&s_lock); s_worker_busy = false; portEXIT_CRITICAL(&s_lock);
        }
    }
}
esp_err_t audio_init(audio_state_cb cb, void *user)
{
    if (s_commands) return ESP_OK;
    s_cb = cb;
    s_cb_user = user;
    s_commands = xQueueCreate(1, sizeof(audio_command_t));
    s_command_lock = xSemaphoreCreateMutex();
    if (!s_commands || !s_command_lock) goto fail;
    if (esp_audio_dec_register_default() != ESP_AUDIO_ERR_OK ||
        esp_audio_simple_dec_register_default() != ESP_AUDIO_ERR_OK) goto fail;
    if (xTaskCreate(audio_worker, "audio", 6144, NULL, 10, NULL) != pdPASS) goto fail;
    return board_adapter_speaker_enable(false);
fail:
    if (s_commands) vQueueDelete(s_commands);
    if (s_command_lock) vSemaphoreDelete(s_command_lock);
    s_commands = NULL;
    s_command_lock = NULL;
    return ESP_ERR_NO_MEM;
}
esp_err_t audio_play_file_with_duration(const char *path, uint32_t duration_ms)
{
    if (!path || !path[0] || strlen(path) >= sizeof(((audio_command_t *)0)->path)) return ESP_ERR_INVALID_ARG;
    if (!s_commands || !s_command_lock) return ESP_ERR_INVALID_STATE;
    if (xSemaphoreTake(s_command_lock, pdMS_TO_TICKS(10)) != pdTRUE) return ESP_ERR_TIMEOUT;
    audio_command_t cmd = {0};
    strlcpy(cmd.path, path, sizeof(cmd.path));
    portENTER_CRITICAL(&s_lock);
    cmd.serial = ++s_serial;
    s_total_ms = duration_ms;
    s_samples = 0;
    s_state = AUDIO_LOADING;
    portEXIT_CRITICAL(&s_lock);
    xQueueOverwrite(s_commands, &cmd);
    xSemaphoreGive(s_command_lock);
    ESP_LOGI(TAG, "queue serial=%lu %s", (unsigned long)cmd.serial, path[0] == '@' ? path : "story");
    return ESP_OK;
}
esp_err_t audio_play_file(const char *path) { return audio_play_file_with_duration(path, 0); }
esp_err_t audio_play_clip(audio_clip_t clip)
{
    if (clip < 0 || clip >= AUDIO_CLIP_COUNT) return ESP_ERR_INVALID_ARG;
    char path[16];
    snprintf(path, sizeof(path), "@clip:%d", (int)clip);
    esp_err_t err = audio_play_file_with_duration(path, 0);
    if (err == ESP_OK) {
        portENTER_CRITICAL(&s_lock);
        s_clip_serial = s_serial;
        s_clip_id = (int)clip;
        portEXIT_CRITICAL(&s_lock);
    }
    return err;
}
bool audio_clip_active(void)
{
    portENTER_CRITICAL(&s_lock);
    bool active = s_clip_serial && s_clip_serial == s_serial &&
                  (s_state == AUDIO_LOADING || s_state == AUDIO_PLAYING);
    portEXIT_CRITICAL(&s_lock);
    return active;
}
esp_err_t audio_play_test_tone(void) { return audio_play_file_with_duration("@diagnostic-tone", 3000); }
esp_err_t audio_play_test_mp3(void) { return audio_play_file_with_duration("@diagnostic-mp3", 6000); }
int audio_prompt_active(void)
{
    portENTER_CRITICAL(&s_lock);
    bool active = s_clip_serial && s_clip_serial == s_serial &&
                  (s_state == AUDIO_LOADING || s_state == AUDIO_PLAYING);
    int id = active && s_clip_id >= AUDIO_CLIP_PROMPT_LOOKING ? s_clip_id : -1;
    portEXIT_CRITICAL(&s_lock);
    return id;
}
audio_state_t audio_get_state(void)
{
    portENTER_CRITICAL(&s_lock);
    audio_state_t state = s_state;
    portEXIT_CRITICAL(&s_lock);
    return state;
}
static void change_play_state(audio_state_t from, audio_state_t to)
{
    portENTER_CRITICAL(&s_lock);
    bool changed = s_state == from;
    if (changed) s_state = to;
    portEXIT_CRITICAL(&s_lock);
    if (changed && s_cb) s_cb(to, s_cb_user);
}
void audio_pause(void) { change_play_state(AUDIO_PLAYING, AUDIO_PAUSED); }
void audio_resume(void) { change_play_state(AUDIO_PAUSED, AUDIO_PLAYING); }
void audio_stop(void)
{
    if (!s_commands || xSemaphoreTake(s_command_lock, pdMS_TO_TICKS(10)) != pdTRUE) return;
    audio_command_t cmd = {0};
    portENTER_CRITICAL(&s_lock);
    cmd.serial = ++s_serial;
    s_state = AUDIO_STOPPED;
    s_samples = 0;
    s_total_ms = 0;
    portEXIT_CRITICAL(&s_lock);
    xQueueOverwrite(s_commands, &cmd);
    xSemaphoreGive(s_command_lock);
}
int audio_progress_permille(void)
{
    portENTER_CRITICAL(&s_lock);
    uint64_t samples = s_samples;
    uint32_t total = s_total_ms;
    portEXIT_CRITICAL(&s_lock);
    if (!total) return 0;
    uint64_t progress = samples * 1000ULL / 24ULL / total;
    return progress > 1000 ? 1000 : (int)progress;
}
uint32_t audio_duration_ms(void)
{
    portENTER_CRITICAL(&s_lock);
    uint32_t ms = s_total_ms;
    portEXIT_CRITICAL(&s_lock);
    return ms;
}
void audio_set_duration_ms(uint32_t ms)
{
    portENTER_CRITICAL(&s_lock);
    s_total_ms = ms;
    portEXIT_CRITICAL(&s_lock);
}
void audio_set_volume(uint8_t volume)
{
    portENTER_CRITICAL(&s_lock);
    s_volume = volume > 100 ? 100 : volume;
    portEXIT_CRITICAL(&s_lock);
}

bool audio_is_quiescent(void)
{
    portENTER_CRITICAL(&s_lock);
    bool quiet = !s_worker_busy && s_state == AUDIO_STOPPED;
    portEXIT_CRITICAL(&s_lock);
    return quiet;
}
