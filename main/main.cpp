#include "esp_log.h"

#include "teckla_assistant.hpp"
#include "teckla_audio.hpp"
#include "teckla_tts.hpp"
#include "usb_midi_host.hpp"

namespace {

constexpr const char *TAG = "TeCKla";

void midi_note_callback(const MidiNoteEvent &event, void *)
{
    teckla_audio_handle_midi_note(event);
    teckla_assistant_handle_note(event);
}

void midi_panic_callback(void *)
{
    teckla_audio_all_notes_off();
    teckla_assistant_all_notes_off();
}

} // namespace

extern "C" void app_main(void)
{
    ESP_LOGI(TAG, "Starting TeCKla");

    ESP_ERROR_CHECK(teckla_audio_init());
    ESP_ERROR_CHECK(teckla_tts_init());
    ESP_ERROR_CHECK(teckla_assistant_init());

    ESP_ERROR_CHECK(usb_midi_host_init());
    ESP_ERROR_CHECK(usb_midi_host_set_note_callback(midi_note_callback, nullptr));
    ESP_ERROR_CHECK(usb_midi_host_set_panic_callback(midi_panic_callback, nullptr));
    ESP_ERROR_CHECK(usb_midi_host_start());
}
