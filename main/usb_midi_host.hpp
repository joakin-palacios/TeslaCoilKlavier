#pragma once

#include <cstdint>

#include "esp_err.h"

enum class MidiNoteEventType : uint8_t {
    NoteOff,
    NoteOn,
};

struct MidiNoteEvent {
    MidiNoteEventType type;
    uint8_t channel;
    uint8_t note;
    uint8_t velocity;
};

using midi_note_callback_t = void (*)(const MidiNoteEvent &event, void *user_data);
using midi_panic_callback_t = void (*)(void *user_data);

esp_err_t usb_midi_host_init();
esp_err_t usb_midi_host_start();
esp_err_t usb_midi_host_set_note_callback(midi_note_callback_t callback, void *user_data);
esp_err_t usb_midi_host_set_panic_callback(midi_panic_callback_t callback, void *user_data);
