#include "teckla_tts.hpp"

#include <cstring>

#include "teckla_audio.hpp"

extern "C" {
#include "esp_check.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "picotts.h"
}

namespace {

constexpr const char *TAG = "TecklaTTS";
constexpr size_t MAX_UTTERANCE_BYTES = 1024;

struct Utterance {
    char text[MAX_UTTERANCE_BYTES];
};

QueueHandle_t utterance_queue = nullptr;
SemaphoreHandle_t tts_done = nullptr;
TaskHandle_t tts_task_handle = nullptr;
portMUX_TYPE tts_lock = portMUX_INITIALIZER_UNLOCKED;
bool tts_busy = false;

void set_busy(bool busy)
{
    portENTER_CRITICAL(&tts_lock);
    tts_busy = busy;
    portEXIT_CRITICAL(&tts_lock);
}

void tts_samples_callback(int16_t *samples, unsigned count)
{
    teckla_audio_push_tts_samples(samples, count);
}

void tts_idle_callback()
{
    if (tts_done != nullptr) {
        xSemaphoreGive(tts_done);
    }
}

void tts_error_callback()
{
    ESP_LOGE(TAG, "PicoTTS stopped after an internal error");
    if (tts_done != nullptr) {
        xSemaphoreGive(tts_done);
    }
}

void speak_text(const char *text)
{
    picotts_shutdown();
    picotts_set_language(PICOTTS_LANG_EN_US);

    if (!picotts_init(5, tts_samples_callback, 1)) {
        ESP_LOGE(TAG, "Failed to initialize PicoTTS");
        return;
    }

    picotts_set_idle_notify(tts_idle_callback);
    picotts_set_error_notify(tts_error_callback);

    xSemaphoreTake(tts_done, 0);
    picotts_add(text, std::strlen(text));
    picotts_add("\0", 1);
    xSemaphoreTake(tts_done, portMAX_DELAY);
    picotts_shutdown();
}

void tts_task(void *)
{
    Utterance utterance = {};
    while (true) {
        if (xQueueReceive(utterance_queue, &utterance, portMAX_DELAY) == pdTRUE) {
            ESP_LOGI(TAG, "Speaking: %s", utterance.text);
            speak_text(utterance.text);
            set_busy(false);
        }
    }
}

} // namespace

esp_err_t teckla_tts_init()
{
    if (tts_task_handle != nullptr) {
        return ESP_OK;
    }

    utterance_queue = xQueueCreate(1, sizeof(Utterance));
    ESP_RETURN_ON_FALSE(utterance_queue != nullptr, ESP_ERR_NO_MEM, TAG, "create utterance queue failed");

    tts_done = xSemaphoreCreateBinary();
    ESP_RETURN_ON_FALSE(tts_done != nullptr, ESP_ERR_NO_MEM, TAG, "create TTS semaphore failed");

    const BaseType_t task_result = xTaskCreate(tts_task, "teckla_tts", 8192, nullptr, 5, &tts_task_handle);
    ESP_RETURN_ON_FALSE(task_result == pdPASS, ESP_ERR_NO_MEM, TAG, "create TTS task failed");

    ESP_LOGI(TAG, "PicoTTS wrapper initialized");
    return ESP_OK;
}

esp_err_t teckla_tts_say(const char *text)
{
    ESP_RETURN_ON_FALSE(text != nullptr, ESP_ERR_INVALID_ARG, TAG, "null TTS text");
    ESP_RETURN_ON_FALSE(utterance_queue != nullptr, ESP_ERR_INVALID_STATE, TAG, "TTS is not initialized");

    portENTER_CRITICAL(&tts_lock);
    const bool already_busy = tts_busy;
    if (!already_busy) {
        tts_busy = true;
    }
    portEXIT_CRITICAL(&tts_lock);
    ESP_RETURN_ON_FALSE(!already_busy, ESP_ERR_INVALID_STATE, TAG, "TTS is busy");

    Utterance utterance = {};
    std::strncpy(utterance.text, text, sizeof(utterance.text) - 1);

    if (xQueueSend(utterance_queue, &utterance, 0) != pdTRUE) {
        set_busy(false);
        return ESP_ERR_INVALID_STATE;
    }
    return ESP_OK;
}

bool teckla_tts_is_busy()
{
    portENTER_CRITICAL(&tts_lock);
    const bool busy = tts_busy;
    portEXIT_CRITICAL(&tts_lock);
    return busy;
}
