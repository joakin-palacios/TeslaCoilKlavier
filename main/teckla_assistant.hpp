#pragma once

#include "esp_err.h"
#include "usb_midi_host.hpp"

esp_err_t teckla_assistant_init();
void teckla_assistant_handle_note(const MidiNoteEvent &event);
void teckla_assistant_all_notes_off();
