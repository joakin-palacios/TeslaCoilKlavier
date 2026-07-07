#pragma once

#include <cstddef>
#include <cstdint>

#include "esp_err.h"
#include "usb_midi_host.hpp"

enum class TecklaFeedbackSound {
    Activation,
    ValidChoice,
    InvalidChoice,
    Exit,
};

esp_err_t teckla_audio_init();
void teckla_audio_handle_midi_note(const MidiNoteEvent &event);
void teckla_audio_all_notes_off();
void teckla_audio_push_tts_samples(const int16_t *samples, size_t count);
void teckla_audio_play_feedback(TecklaFeedbackSound sound);
