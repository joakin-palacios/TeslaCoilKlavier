#include "teckla_audio.hpp"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>

extern "C" {
#include "driver/gpio.h"
#include "driver/i2c.h"
#include "driver/i2s_std.h"
#include "esp_check.h"
#include "esp_log.h"
#include "es8311.h"
#include "freertos/FreeRTOS.h"
#include "freertos/stream_buffer.h"
#include "freertos/task.h"
}

namespace {

constexpr const char *TAG = "TecklaAudio";

constexpr i2c_port_t I2C_PORT = I2C_NUM_0;
constexpr gpio_num_t I2C_SDA = GPIO_NUM_7;
constexpr gpio_num_t I2C_SCL = GPIO_NUM_8;

constexpr int ES8311_I2S_PORT = 0;
constexpr gpio_num_t ES8311_I2S_MCLK = GPIO_NUM_13;
constexpr gpio_num_t ES8311_I2S_BCLK = GPIO_NUM_12;
constexpr gpio_num_t ES8311_I2S_WS = GPIO_NUM_10;
constexpr gpio_num_t ES8311_I2S_DOUT = GPIO_NUM_9;
constexpr gpio_num_t ES8311_I2S_DIN = GPIO_NUM_11;

constexpr gpio_num_t PCM5102_I2S_BCLK = GPIO_NUM_46;
constexpr gpio_num_t PCM5102_I2S_WS = GPIO_NUM_27;
constexpr gpio_num_t PCM5102_I2S_DOUT = GPIO_NUM_45;
constexpr gpio_num_t PA_ENABLE = GPIO_NUM_53;

constexpr uint32_t SAMPLE_RATE = 16000;
constexpr uint32_t MCLK_MULTIPLE = 384;
constexpr uint32_t MCLK_FREQ_HZ = SAMPLE_RATE * MCLK_MULTIPLE;
constexpr int VOLUME_PERCENT = 70;
constexpr float PCM5102_GAIN = 2.0f;
constexpr float PI = 3.14159265358979323846f;
constexpr size_t AUDIO_FRAMES_PER_BUFFER = 128;
constexpr uint32_t PCM5102_STARTUP_TONE_MS = 500;
constexpr float PCM5102_STARTUP_TONE_HZ = 523.25f;
constexpr float PCM5102_STARTUP_TONE_GAIN = 26000.0f;
constexpr size_t MAX_MIDI_NOTES = 128;
constexpr size_t TTS_STREAM_BYTES = SAMPLE_RATE * sizeof(int16_t) * 3;
constexpr float MIDI_RELEASE_STEP = 1.0f / 960.0f;
constexpr float MIDI_ATTACK_STEP = 1.0f / 32.0f;
constexpr float MIDI_BASE_GAIN = 16000.0f;
constexpr float MIDI_MIN_VELOCITY_SCALE = 0.65f;
constexpr float MIDI_SECOND_HARMONIC_GAIN = 0.35f;

i2s_chan_handle_t es8311_tx_handle = nullptr;
i2s_chan_handle_t es8311_rx_handle = nullptr;
i2s_chan_handle_t pcm5102_tx_handle = nullptr;
es8311_handle_t codec_handle = nullptr;
StreamBufferHandle_t tts_stream = nullptr;
TaskHandle_t audio_task_handle = nullptr;
portMUX_TYPE audio_lock = portMUX_INITIALIZER_UNLOCKED;

struct ActiveNote {
    bool active;
    bool releasing;
    uint8_t velocity;
    float frequency_hz;
    float phase;
    float amplitude;
};

struct FeedbackState {
    bool active;
    TecklaFeedbackSound sound;
    uint32_t frame;
    uint32_t total_frames;
    float phase;
};

ActiveNote notes[MAX_MIDI_NOTES] = {};
FeedbackState feedback = {};

float midi_note_frequency(uint8_t note)
{
    return 440.0f * std::pow(2.0f, (static_cast<float>(note) - 69.0f) / 12.0f);
}

int16_t clamp_to_i16(float sample)
{
    if (sample > 32767.0f) {
        return 32767;
    }
    if (sample < -32768.0f) {
        return -32768;
    }
    return static_cast<int16_t>(sample);
}

esp_err_t init_power_amp()
{
    gpio_config_t io_conf = {};
    io_conf.pin_bit_mask = 1ULL << PA_ENABLE;
    io_conf.mode = GPIO_MODE_OUTPUT;
    io_conf.pull_down_en = GPIO_PULLDOWN_DISABLE;
    io_conf.pull_up_en = GPIO_PULLUP_DISABLE;
    io_conf.intr_type = GPIO_INTR_DISABLE;
    ESP_RETURN_ON_ERROR(gpio_config(&io_conf), TAG, "configure PA enable GPIO failed");
    ESP_RETURN_ON_ERROR(gpio_set_level(PA_ENABLE, 1), TAG, "enable PA failed");
    return ESP_OK;
}

esp_err_t init_i2c()
{
    i2c_config_t cfg = {};
    cfg.mode = I2C_MODE_MASTER;
    cfg.sda_io_num = I2C_SDA;
    cfg.scl_io_num = I2C_SCL;
    cfg.sda_pullup_en = GPIO_PULLUP_ENABLE;
    cfg.scl_pullup_en = GPIO_PULLUP_ENABLE;
    cfg.master.clk_speed = 100000;

    ESP_RETURN_ON_ERROR(i2c_param_config(I2C_PORT, &cfg), TAG, "configure I2C failed");
    ESP_RETURN_ON_ERROR(i2c_driver_install(I2C_PORT, cfg.mode, 0, 0, 0), TAG, "install I2C driver failed");
    return ESP_OK;
}

esp_err_t init_es8311_i2s()
{
    i2s_chan_config_t chan_cfg = I2S_CHANNEL_DEFAULT_CONFIG(ES8311_I2S_PORT, I2S_ROLE_MASTER);
    chan_cfg.auto_clear = true;
    ESP_RETURN_ON_ERROR(i2s_new_channel(&chan_cfg, &es8311_tx_handle, &es8311_rx_handle), TAG, "create ES8311 I2S channel failed");

    i2s_std_config_t std_cfg = {};
    std_cfg.clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG(SAMPLE_RATE);
    std_cfg.clk_cfg.mclk_multiple = static_cast<i2s_mclk_multiple_t>(MCLK_MULTIPLE);
    std_cfg.slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_16BIT, I2S_SLOT_MODE_STEREO);
    std_cfg.gpio_cfg.mclk = ES8311_I2S_MCLK;
    std_cfg.gpio_cfg.bclk = ES8311_I2S_BCLK;
    std_cfg.gpio_cfg.ws = ES8311_I2S_WS;
    std_cfg.gpio_cfg.dout = ES8311_I2S_DOUT;
    std_cfg.gpio_cfg.din = ES8311_I2S_DIN;
    std_cfg.gpio_cfg.invert_flags.mclk_inv = false;
    std_cfg.gpio_cfg.invert_flags.bclk_inv = false;
    std_cfg.gpio_cfg.invert_flags.ws_inv = false;

    ESP_RETURN_ON_ERROR(i2s_channel_init_std_mode(es8311_tx_handle, &std_cfg), TAG, "init ES8311 I2S TX failed");
    ESP_RETURN_ON_ERROR(i2s_channel_init_std_mode(es8311_rx_handle, &std_cfg), TAG, "init ES8311 I2S RX failed");
    ESP_RETURN_ON_ERROR(i2s_channel_enable(es8311_tx_handle), TAG, "enable ES8311 I2S TX failed");
    ESP_RETURN_ON_ERROR(i2s_channel_enable(es8311_rx_handle), TAG, "enable ES8311 I2S RX failed");
    return ESP_OK;
}

esp_err_t init_pcm5102_i2s()
{
    i2s_chan_config_t chan_cfg = I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_AUTO, I2S_ROLE_MASTER);
    chan_cfg.auto_clear = true;
    ESP_RETURN_ON_ERROR(i2s_new_channel(&chan_cfg, &pcm5102_tx_handle, nullptr), TAG, "create PCM5102A I2S channel failed");

    i2s_std_config_t std_cfg = {};
    std_cfg.clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG(SAMPLE_RATE);
    std_cfg.slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_16BIT, I2S_SLOT_MODE_STEREO);
    std_cfg.gpio_cfg.mclk = I2S_GPIO_UNUSED;
    std_cfg.gpio_cfg.bclk = PCM5102_I2S_BCLK;
    std_cfg.gpio_cfg.ws = PCM5102_I2S_WS;
    std_cfg.gpio_cfg.dout = PCM5102_I2S_DOUT;
    std_cfg.gpio_cfg.din = I2S_GPIO_UNUSED;
    std_cfg.gpio_cfg.invert_flags.mclk_inv = false;
    std_cfg.gpio_cfg.invert_flags.bclk_inv = false;
    std_cfg.gpio_cfg.invert_flags.ws_inv = false;

    ESP_RETURN_ON_ERROR(i2s_channel_init_std_mode(pcm5102_tx_handle, &std_cfg), TAG, "init PCM5102A I2S TX failed");
    ESP_RETURN_ON_ERROR(i2s_channel_enable(pcm5102_tx_handle), TAG, "enable PCM5102A I2S TX failed");
    ESP_LOGI(TAG, "PCM5102A I2S initialized: BCLK=GPIO%d WS=GPIO%d DOUT=GPIO%d", PCM5102_I2S_BCLK, PCM5102_I2S_WS, PCM5102_I2S_DOUT);
    return ESP_OK;
}

esp_err_t write_all_i2s(i2s_chan_handle_t handle, const void *buffer, size_t byte_count, const char *name)
{
    const uint8_t *data = static_cast<const uint8_t *>(buffer);
    size_t remaining = byte_count;
    while (remaining > 0) {
        size_t bytes_written = 0;
        const esp_err_t err = i2s_channel_write(handle, data, remaining, &bytes_written, portMAX_DELAY);
        if (err != ESP_OK) {
            ESP_LOGW(TAG, "%s I2S write failed: %s bytes=%u", name, esp_err_to_name(err), static_cast<unsigned>(bytes_written));
            return err;
        }
        if (bytes_written == 0) {
            ESP_LOGW(TAG, "%s I2S write made no progress", name);
            return ESP_ERR_TIMEOUT;
        }
        data += bytes_written;
        remaining -= bytes_written;
    }
    return ESP_OK;
}

void play_pcm5102_startup_tone()
{
    int16_t stereo[AUDIO_FRAMES_PER_BUFFER * 2] = {};
    const uint32_t total_frames = SAMPLE_RATE * PCM5102_STARTUP_TONE_MS / 1000U;
    const float phase_step = 2.0f * PI * PCM5102_STARTUP_TONE_HZ / static_cast<float>(SAMPLE_RATE);
    float phase = 0.0f;

    ESP_LOGI(TAG, "Playing PCM5102A startup test tone");
    for (uint32_t generated = 0; generated < total_frames;) {
        const size_t frame_count = std::min<size_t>(AUDIO_FRAMES_PER_BUFFER, total_frames - generated);
        for (size_t i = 0; i < frame_count; ++i) {
            const int16_t sample = clamp_to_i16(std::sinf(phase) * PCM5102_STARTUP_TONE_GAIN);
            stereo[i * 2] = sample;
            stereo[i * 2 + 1] = sample;

            phase += phase_step;
            if (phase >= 2.0f * PI) {
                phase -= 2.0f * PI;
            }
        }
        write_all_i2s(pcm5102_tx_handle, stereo, frame_count * 2 * sizeof(int16_t), "PCM5102A startup tone");
        generated += frame_count;
    }
}

esp_err_t init_codec()
{
    codec_handle = es8311_create(I2C_PORT, ES8311_ADDRRES_0);
    ESP_RETURN_ON_FALSE(codec_handle != nullptr, ESP_FAIL, TAG, "create ES8311 handle failed");

    es8311_clock_config_t clk_cfg = {};
    clk_cfg.mclk_inverted = false;
    clk_cfg.sclk_inverted = false;
    clk_cfg.mclk_from_mclk_pin = true;
    clk_cfg.mclk_frequency = MCLK_FREQ_HZ;
    clk_cfg.sample_frequency = SAMPLE_RATE;

    ESP_RETURN_ON_ERROR(es8311_init(codec_handle, &clk_cfg, ES8311_RESOLUTION_16, ES8311_RESOLUTION_16), TAG, "init ES8311 failed");
    ESP_RETURN_ON_ERROR(es8311_sample_frequency_config(codec_handle, MCLK_FREQ_HZ, SAMPLE_RATE), TAG, "configure ES8311 sample rate failed");
    ESP_RETURN_ON_ERROR(es8311_voice_volume_set(codec_handle, VOLUME_PERCENT, nullptr), TAG, "set ES8311 volume failed");
    ESP_RETURN_ON_ERROR(es8311_microphone_config(codec_handle, false), TAG, "disable ES8311 microphone path failed");
    ESP_RETURN_ON_ERROR(es8311_microphone_gain_set(codec_handle, ES8311_MIC_GAIN_30DB), TAG, "set ES8311 microphone gain failed");
    return ESP_OK;
}

float feedback_frequency(TecklaFeedbackSound sound, uint32_t frame, uint32_t total_frames)
{
    const uint32_t segment = total_frames / 4U;
    switch (sound) {
    case TecklaFeedbackSound::Activation:
        if (frame < segment) {
            return 523.25f;
        }
        if (frame < segment * 2U) {
            return 659.25f;
        }
        if (frame < segment * 3U) {
            return 783.99f;
        }
        return 1046.50f;
    case TecklaFeedbackSound::ValidChoice:
        return frame < total_frames / 2U ? 880.0f : 1174.66f;
    case TecklaFeedbackSound::InvalidChoice:
        return 146.83f;
    case TecklaFeedbackSound::Exit:
        if (frame < segment) {
            return 783.99f;
        }
        if (frame < segment * 2U) {
            return 659.25f;
        }
        if (frame < segment * 3U) {
            return 523.25f;
        }
        return 392.0f;
    }
    return 440.0f;
}

float feedback_sample_locked()
{
    if (!feedback.active) {
        return 0.0f;
    }

    if (feedback.frame >= feedback.total_frames) {
        feedback.active = false;
        feedback.frame = 0;
        feedback.phase = 0.0f;
        return 0.0f;
    }

    const float attack_frames = 160.0f;
    const float release_frames = 640.0f;
    float envelope = 1.0f;
    if (feedback.frame < static_cast<uint32_t>(attack_frames)) {
        envelope = static_cast<float>(feedback.frame) / attack_frames;
    } else if (feedback.total_frames - feedback.frame < static_cast<uint32_t>(release_frames)) {
        envelope = static_cast<float>(feedback.total_frames - feedback.frame) / release_frames;
    }

    const float frequency = feedback_frequency(feedback.sound, feedback.frame, feedback.total_frames);
    const float sample = std::sinf(feedback.phase) * envelope * 7000.0f;
    feedback.phase += 2.0f * PI * frequency / static_cast<float>(SAMPLE_RATE);
    if (feedback.phase >= 2.0f * PI) {
        feedback.phase -= 2.0f * PI;
    }
    ++feedback.frame;
    return sample;
}

float midi_sample_locked()
{
    size_t active_count = 0;
    for (const ActiveNote &note : notes) {
        if (note.active) {
            ++active_count;
        }
    }
    if (active_count == 0) {
        return 0.0f;
    }

    const float gain = MIDI_BASE_GAIN / std::sqrt(static_cast<float>(active_count));
    float mixed = 0.0f;
    for (ActiveNote &note : notes) {
        if (!note.active) {
            continue;
        }

        if (note.releasing) {
            note.amplitude -= MIDI_RELEASE_STEP;
            if (note.amplitude <= 0.0f) {
                note = {};
                continue;
            }
        } else if (note.amplitude < 1.0f) {
            note.amplitude = std::min(1.0f, note.amplitude + MIDI_ATTACK_STEP);
        }

        const float velocity_scale = std::max(MIDI_MIN_VELOCITY_SCALE, static_cast<float>(note.velocity) / 127.0f);
        const float tone = std::sinf(note.phase) + MIDI_SECOND_HARMONIC_GAIN * std::sinf(note.phase * 2.0f);
        mixed += tone * note.amplitude * velocity_scale * gain;
        note.phase += 2.0f * PI * note.frequency_hz / static_cast<float>(SAMPLE_RATE);
        if (note.phase >= 2.0f * PI) {
            note.phase -= 2.0f * PI;
        }
    }
    return mixed;
}

void audio_task(void *)
{
    int16_t stereo[AUDIO_FRAMES_PER_BUFFER * 2] = {};
    int16_t pcm5102_stereo[AUDIO_FRAMES_PER_BUFFER * 2] = {};
    int16_t tts_samples[AUDIO_FRAMES_PER_BUFFER] = {};

    while (true) {
        std::memset(tts_samples, 0, sizeof(tts_samples));
        size_t tts_bytes = 0;
        if (tts_stream != nullptr) {
            tts_bytes = xStreamBufferReceive(tts_stream, tts_samples, sizeof(tts_samples), 0);
        }
        const size_t tts_count = tts_bytes / sizeof(int16_t);

        for (size_t i = 0; i < AUDIO_FRAMES_PER_BUFFER; ++i) {
            float mixed = i < tts_count ? static_cast<float>(tts_samples[i]) : 0.0f;

            portENTER_CRITICAL(&audio_lock);
            mixed += midi_sample_locked();
            mixed += feedback_sample_locked();
            portEXIT_CRITICAL(&audio_lock);

            const int16_t sample = clamp_to_i16(mixed);
            stereo[i * 2] = sample;
            stereo[i * 2 + 1] = sample;

            const int16_t pcm5102_sample = clamp_to_i16(mixed * PCM5102_GAIN);
            pcm5102_stereo[i * 2] = pcm5102_sample;
            pcm5102_stereo[i * 2 + 1] = pcm5102_sample;
        }

        write_all_i2s(es8311_tx_handle, stereo, sizeof(stereo), "ES8311");
        write_all_i2s(pcm5102_tx_handle, pcm5102_stereo, sizeof(pcm5102_stereo), "PCM5102A");
    }
}

} // namespace

esp_err_t teckla_audio_init()
{
    if (audio_task_handle != nullptr) {
        return ESP_OK;
    }

    ESP_RETURN_ON_ERROR(init_power_amp(), TAG, "power amp init failed");
    ESP_RETURN_ON_ERROR(init_i2c(), TAG, "I2C init failed");
    ESP_RETURN_ON_ERROR(init_es8311_i2s(), TAG, "ES8311 I2S init failed");
    ESP_RETURN_ON_ERROR(init_pcm5102_i2s(), TAG, "PCM5102A I2S init failed");
    ESP_RETURN_ON_ERROR(init_codec(), TAG, "codec init failed");
    play_pcm5102_startup_tone();

    tts_stream = xStreamBufferCreate(TTS_STREAM_BYTES, sizeof(int16_t));
    ESP_RETURN_ON_FALSE(tts_stream != nullptr, ESP_ERR_NO_MEM, TAG, "create TTS stream failed");

    const BaseType_t task_result = xTaskCreate(audio_task, "teckla_audio", 4096, nullptr, 18, &audio_task_handle);
    ESP_RETURN_ON_FALSE(task_result == pdPASS, ESP_ERR_NO_MEM, TAG, "create audio task failed");

    ESP_LOGI(TAG, "Audio mixer initialized");
    return ESP_OK;
}

void teckla_audio_handle_midi_note(const MidiNoteEvent &event)
{
    if (event.note >= MAX_MIDI_NOTES) {
        return;
    }

    portENTER_CRITICAL(&audio_lock);
    ActiveNote &note = notes[event.note];
    if (event.type == MidiNoteEventType::NoteOn && event.velocity > 0) {
        note.active = true;
        note.releasing = false;
        note.velocity = event.velocity;
        note.frequency_hz = midi_note_frequency(event.note);
        note.phase = 0.0f;
        note.amplitude = 0.0f;
    } else if (note.active) {
        note.releasing = true;
    }
    portEXIT_CRITICAL(&audio_lock);
}

void teckla_audio_all_notes_off()
{
    portENTER_CRITICAL(&audio_lock);
    std::memset(notes, 0, sizeof(notes));
    portEXIT_CRITICAL(&audio_lock);
}

void teckla_audio_push_tts_samples(const int16_t *samples, size_t count)
{
    if (tts_stream == nullptr || samples == nullptr || count == 0) {
        return;
    }

    const uint8_t *bytes = reinterpret_cast<const uint8_t *>(samples);
    size_t remaining = count * sizeof(int16_t);
    while (remaining > 0) {
        const size_t sent = xStreamBufferSend(tts_stream, bytes, remaining, portMAX_DELAY);
        bytes += sent;
        remaining -= sent;
    }
}

void teckla_audio_play_feedback(TecklaFeedbackSound sound)
{
    uint32_t duration_ms = 180;
    switch (sound) {
    case TecklaFeedbackSound::Activation:
        duration_ms = 520;
        break;
    case TecklaFeedbackSound::ValidChoice:
        duration_ms = 180;
        break;
    case TecklaFeedbackSound::InvalidChoice:
        duration_ms = 160;
        break;
    case TecklaFeedbackSound::Exit:
        duration_ms = 420;
        break;
    }

    portENTER_CRITICAL(&audio_lock);
    feedback.active = true;
    feedback.sound = sound;
    feedback.frame = 0;
    feedback.total_frames = SAMPLE_RATE * duration_ms / 1000U;
    feedback.phase = 0.0f;
    portEXIT_CRITICAL(&audio_lock);
}
