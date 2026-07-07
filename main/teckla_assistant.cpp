#include "teckla_assistant.hpp"

#include <cstdint>
#include <cstring>

#include "teckla_audio.hpp"
#include "teckla_content.hpp"
#include "teckla_tts.hpp"

extern "C" {
#include "esp_check.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
}

namespace {

constexpr const char *TAG = "TecklaAssistant";
constexpr int64_t ACTIVATION_WINDOW_US = 5LL * 1000LL * 1000LL;
constexpr int64_t CHOICE_STABLE_US = 200LL * 1000LL;
constexpr int64_t CHOICE_TIMEOUT_US = 5LL * 1000LL * 1000LL;
constexpr uint8_t LOWEST_MENU_NOTE_MAX = 36;
constexpr uint8_t HIGHEST_MENU_NOTE_MIN = 96;

enum class AssistantState {
    Idle,
    Speaking,
    WaitingForChoice,
};

enum class Choice {
    None,
    Tutorial,
    Physics,
    Xstage,
    Joke,
    RepeatMenu,
    Exit,
    Invalid,
};

struct KeySnapshot {
    bool down[128];
    uint8_t white_count;
    uint8_t black_count;
    uint8_t total_count;
    uint8_t lowest_note;
    uint8_t highest_note;
};

TaskHandle_t assistant_task_handle = nullptr;
portMUX_TYPE key_lock = portMUX_INITIALIZER_UNLOCKED;
bool key_down[128] = {};
AssistantState state = AssistantState::Idle;
bool activation_combo_down = false;
uint8_t activation_count = 0;
int64_t first_activation_us = 0;
Choice stable_choice = Choice::None;
int64_t stable_since_us = 0;
int64_t choice_wait_started_us = 0;
bool exit_after_speech = false;
bool choice_input_ready = false;

bool is_white_key(uint8_t note)
{
    switch (note % 12U) {
    case 0:
    case 2:
    case 4:
    case 5:
    case 7:
    case 9:
    case 11:
        return true;
    default:
        return false;
    }
}

KeySnapshot snapshot_keys()
{
    KeySnapshot snapshot = {};
    snapshot.lowest_note = 127;
    snapshot.highest_note = 0;

    portENTER_CRITICAL(&key_lock);
    std::memcpy(snapshot.down, key_down, sizeof(snapshot.down));
    portEXIT_CRITICAL(&key_lock);

    for (uint8_t note = 0; note < 128; ++note) {
        if (!snapshot.down[note]) {
            continue;
        }
        ++snapshot.total_count;
        if (is_white_key(note)) {
            ++snapshot.white_count;
        } else {
            ++snapshot.black_count;
        }
        if (note < snapshot.lowest_note) {
            snapshot.lowest_note = note;
        }
        if (note > snapshot.highest_note) {
            snapshot.highest_note = note;
        }
    }
    return snapshot;
}

void reset_choice_stability()
{
    stable_choice = Choice::None;
    stable_since_us = 0;
}

bool activation_combo_is_down(const KeySnapshot &keys)
{
    return keys.white_count == 5 && keys.black_count == 3;
}

void reset_session_to_idle(bool activation_combo_currently_down)
{
    state = AssistantState::Idle;
    exit_after_speech = false;
    activation_count = 0;
    first_activation_us = 0;
    activation_combo_down = activation_combo_currently_down;
    choice_wait_started_us = 0;
    choice_input_ready = false;
    reset_choice_stability();
}

void enter_waiting_for_choice(int64_t now_us)
{
    state = AssistantState::WaitingForChoice;
    exit_after_speech = false;
    choice_wait_started_us = now_us;
    choice_input_ready = false;
    reset_choice_stability();
    ESP_LOGI(TAG, "Assistant waiting for menu choice");
}

void enter_speaking(bool exit_when_done)
{
    state = AssistantState::Speaking;
    exit_after_speech = exit_when_done;
    reset_choice_stability();
}

void speak_or_wait(const char *text, bool exit_when_done, int64_t now_us)
{
    const esp_err_t err = teckla_tts_say(text);
    if (err == ESP_OK) {
        enter_speaking(exit_when_done);
    } else {
        ESP_LOGW(TAG, "Could not start TTS: %s", esp_err_to_name(err));
        if (exit_when_done) {
            reset_session_to_idle(false);
            ESP_LOGI(TAG, "Assistant exited to idle");
        } else {
            enter_waiting_for_choice(now_us);
        }
    }
}

void activate(int64_t now_us)
{
    ESP_LOGI(TAG, "Activation gesture accepted");
    teckla_audio_play_feedback(TecklaFeedbackSound::Activation);
    activation_count = 0;
    first_activation_us = 0;
    activation_combo_down = true;
    speak_or_wait(teckla_intro_text(), false, now_us);
}

void update_activation(const KeySnapshot &keys, int64_t now_us)
{
    const bool combo = activation_combo_is_down(keys);
    if (!combo) {
        activation_combo_down = false;
        return;
    }
    if (activation_combo_down) {
        return;
    }

    activation_combo_down = true;
    if (activation_count == 0 || now_us - first_activation_us > ACTIVATION_WINDOW_US) {
        activation_count = 0;
        first_activation_us = now_us;
    }

    ++activation_count;
    ESP_LOGI(TAG, "Activation gesture repetition %u/3", activation_count);
    if (activation_count >= 3 && now_us - first_activation_us <= ACTIVATION_WINDOW_US) {
        activate(now_us);
    }
}

Choice current_choice(const KeySnapshot &keys)
{
    if (keys.total_count == 0) {
        return Choice::None;
    }
    if (keys.total_count == 1 && keys.lowest_note <= LOWEST_MENU_NOTE_MAX) {
        return Choice::RepeatMenu;
    }
    if (keys.total_count == 1 && keys.highest_note >= HIGHEST_MENU_NOTE_MIN) {
        return Choice::Exit;
    }
    if (keys.white_count == 1 && keys.black_count == 1 && keys.total_count == 2) {
        return Choice::Joke;
    }
    if (keys.black_count > 0) {
        return Choice::Xstage;
    }
    if (keys.white_count == 3 && keys.total_count == 3) {
        return Choice::Tutorial;
    }
    if (keys.white_count == 4 && keys.total_count == 4) {
        return Choice::Physics;
    }
    return Choice::Invalid;
}

void accept_choice(Choice choice, int64_t now_us)
{
    switch (choice) {
    case Choice::Tutorial:
        teckla_audio_play_feedback(TecklaFeedbackSound::ValidChoice);
        speak_or_wait(teckla_tutorial_text(), false, now_us);
        break;
    case Choice::Physics:
        teckla_audio_play_feedback(TecklaFeedbackSound::ValidChoice);
        speak_or_wait(teckla_physics_text(), false, now_us);
        break;
    case Choice::Xstage:
        teckla_audio_play_feedback(TecklaFeedbackSound::ValidChoice);
        speak_or_wait(teckla_xstage_text(), false, now_us);
        break;
    case Choice::Joke:
        teckla_audio_play_feedback(TecklaFeedbackSound::ValidChoice);
        speak_or_wait(teckla_random_joke_text(), false, now_us);
        break;
    case Choice::RepeatMenu:
        teckla_audio_play_feedback(TecklaFeedbackSound::ValidChoice);
        speak_or_wait(teckla_intro_text(), false, now_us);
        break;
    case Choice::Exit:
        teckla_audio_play_feedback(TecklaFeedbackSound::Exit);
        speak_or_wait(teckla_goodbye_text(), true, now_us);
        break;
    case Choice::Invalid:
        teckla_audio_play_feedback(TecklaFeedbackSound::InvalidChoice);
        reset_choice_stability();
        break;
    case Choice::None:
        break;
    }
}

void update_waiting_choice(const KeySnapshot &keys, int64_t now_us)
{
    const bool timed_out = choice_wait_started_us != 0 && now_us - choice_wait_started_us >= CHOICE_TIMEOUT_US;
    if (!choice_input_ready) {
        if (timed_out) {
            reset_session_to_idle(activation_combo_is_down(keys));
            ESP_LOGI(TAG, "Assistant choice timeout; returning to idle");
            return;
        }
        if (keys.total_count == 0) {
            choice_input_ready = true;
            reset_choice_stability();
        }
        return;
    }

    const Choice choice = current_choice(keys);
    if (choice == Choice::None) {
        reset_choice_stability();
        if (timed_out) {
            reset_session_to_idle(activation_combo_is_down(keys));
            ESP_LOGI(TAG, "Assistant choice timeout; returning to idle");
        }
        return;
    }

    if (choice != stable_choice) {
        stable_choice = choice;
        stable_since_us = now_us;
        return;
    }

    if (stable_since_us != 0 && now_us - stable_since_us >= CHOICE_STABLE_US) {
        choice_input_ready = false;
        accept_choice(choice, now_us);
        reset_choice_stability();
    }
}

void assistant_task(void *)
{
    while (true) {
        ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(25));

        const int64_t now_us = esp_timer_get_time();
        const KeySnapshot keys = snapshot_keys();

        switch (state) {
        case AssistantState::Idle:
            update_activation(keys, now_us);
            break;
        case AssistantState::Speaking:
            if (!teckla_tts_is_busy()) {
                if (exit_after_speech) {
                    reset_session_to_idle(activation_combo_is_down(keys));
                    ESP_LOGI(TAG, "Assistant exited to idle");
                } else {
                    enter_waiting_for_choice(now_us);
                }
            }
            break;
        case AssistantState::WaitingForChoice:
            if (!teckla_tts_is_busy()) {
                update_waiting_choice(keys, now_us);
            }
            break;
        }
    }
}

} // namespace

esp_err_t teckla_assistant_init()
{
    if (assistant_task_handle != nullptr) {
        return ESP_OK;
    }

    const BaseType_t task_result = xTaskCreate(assistant_task, "teckla_assistant", 4096, nullptr, 6, &assistant_task_handle);
    ESP_RETURN_ON_FALSE(task_result == pdPASS, ESP_ERR_NO_MEM, TAG, "create assistant task failed");
    ESP_LOGI(TAG, "Assistant initialized");
    return ESP_OK;
}

void teckla_assistant_handle_note(const MidiNoteEvent &event)
{
    if (event.note >= 128) {
        return;
    }

    portENTER_CRITICAL(&key_lock);
    key_down[event.note] = event.type == MidiNoteEventType::NoteOn && event.velocity > 0;
    portEXIT_CRITICAL(&key_lock);

    if (assistant_task_handle != nullptr) {
        xTaskNotifyGive(assistant_task_handle);
    }
}

void teckla_assistant_all_notes_off()
{
    portENTER_CRITICAL(&key_lock);
    std::memset(key_down, 0, sizeof(key_down));
    portEXIT_CRITICAL(&key_lock);

    if (assistant_task_handle != nullptr) {
        xTaskNotifyGive(assistant_task_handle);
    }
}
