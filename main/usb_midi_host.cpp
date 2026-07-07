#include "usb_midi_host.hpp"

#include <algorithm>
#include <cstring>

#include "esp_check.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include "usb/usb_host.h"

namespace {

constexpr const char *TAG = "UsbMidiHost";
constexpr uint8_t MIDI_USB_CLASS_AUDIO = 0x01;
constexpr uint8_t USB_SUBCLASS_MIDISTREAMING = 0x03;
constexpr uint8_t USB_DESCRIPTOR_TYPE_INTERFACE = 0x04;
constexpr uint8_t USB_DESCRIPTOR_TYPE_ENDPOINT = 0x05;
constexpr uint8_t USB_ENDPOINT_DIR_IN = 0x80;
constexpr uint8_t USB_TRANSFER_TYPE_BULK = 0x02;
constexpr size_t MIDI_TRANSFER_BYTES = 64;
constexpr size_t MIDI_TRANSFER_COUNT = 4;
constexpr size_t MIDI_PACKET_QUEUE_LEN = 32;

struct MidiPacketBatch {
    int length;
    uint8_t data[MIDI_TRANSFER_BYTES];
};

struct UsbMidiHostState {
    usb_host_client_handle_t client = nullptr;
    usb_device_handle_t device = nullptr;
    usb_transfer_t *transfers[MIDI_TRANSFER_COUNT] = {};
    QueueHandle_t packet_queue = nullptr;
    TaskHandle_t daemon_task = nullptr;
    TaskHandle_t client_task = nullptr;
    TaskHandle_t parser_task = nullptr;
    midi_note_callback_t note_callback = nullptr;
    void *note_callback_user_data = nullptr;
    midi_panic_callback_t panic_callback = nullptr;
    void *panic_callback_user_data = nullptr;
    uint8_t pending_device_address = 0;
    uint8_t interface_number = 0;
    uint8_t endpoint_address = 0;
    bool initialized = false;
    bool started = false;
    bool device_open = false;
    bool interface_claimed = false;
};

UsbMidiHostState state;

void emit_panic(const char *reason)
{
    ESP_LOGW(TAG, "Clearing MIDI note state: %s", reason);
    if (state.panic_callback != nullptr) {
        state.panic_callback(state.panic_callback_user_data);
    }
}

void parse_usb_midi_packet(const uint8_t *packet)
{
    const uint8_t cin = packet[0] & 0x0F;
    const uint8_t status = packet[1];
    const uint8_t message_type = status & 0xF0;

    if (cin == 0x0B && message_type == 0xB0) {
        const uint8_t controller = packet[2];
        if (controller == 120 || controller == 121 || controller == 123) {
            emit_panic("MIDI all-notes/all-sound-off controller");
        }
        return;
    }

    if (!((cin == 0x08 && message_type == 0x80) || (cin == 0x09 && message_type == 0x90))) {
        return;
    }

    MidiNoteEvent event = {
        .type = MidiNoteEventType::NoteOff,
        .channel = static_cast<uint8_t>(status & 0x0F),
        .note = packet[2],
        .velocity = packet[3],
    };

    if (message_type == 0x90 && event.velocity > 0) {
        event.type = MidiNoteEventType::NoteOn;
    }

    ESP_LOGD(TAG, "MIDI note %s: channel=%u note=%u velocity=%u",
             event.type == MidiNoteEventType::NoteOn ? "on" : "off", event.channel + 1, event.note, event.velocity);

    if (state.note_callback != nullptr) {
        state.note_callback(event, state.note_callback_user_data);
    }
}

void parse_usb_midi_transfer(const uint8_t *data, int length)
{
    for (int offset = 0; offset + 3 < length; offset += 4) {
        parse_usb_midi_packet(&data[offset]);
    }
}

void midi_transfer_callback(usb_transfer_t *transfer)
{
    if (transfer->status == USB_TRANSFER_STATUS_COMPLETED && transfer->actual_num_bytes > 0) {
        MidiPacketBatch batch = {};
        batch.length = std::min(static_cast<int>(transfer->actual_num_bytes), static_cast<int>(MIDI_TRANSFER_BYTES));
        std::memcpy(batch.data, transfer->data_buffer, batch.length);
        if (state.packet_queue != nullptr && xQueueSend(state.packet_queue, &batch, 0) != pdTRUE) {
            emit_panic("USB MIDI packet queue full");
        }
    } else if (transfer->status != USB_TRANSFER_STATUS_COMPLETED) {
        emit_panic("USB MIDI transfer error");
    }

    if (state.device_open) {
        const esp_err_t err = usb_host_transfer_submit(transfer);
        if (err != ESP_OK) {
            ESP_LOGW(TAG, "Failed to resubmit MIDI transfer: %s", esp_err_to_name(err));
        }
    }
}

void midi_parser_task(void *)
{
    MidiPacketBatch batch = {};
    while (true) {
        if (xQueueReceive(state.packet_queue, &batch, portMAX_DELAY) == pdTRUE) {
            parse_usb_midi_transfer(batch.data, batch.length);
        }
    }
}

void client_event_callback(const usb_host_client_event_msg_t *event, void *)
{
    switch (event->event) {
    case USB_HOST_CLIENT_EVENT_NEW_DEV:
        state.pending_device_address = event->new_dev.address;
        break;
    case USB_HOST_CLIENT_EVENT_DEV_GONE:
        ESP_LOGI(TAG, "USB MIDI device disconnected");
        emit_panic("USB MIDI device disconnected");
        state.device_open = false;
        break;
    default:
        break;
    }
}

bool find_midi_input_endpoint(const usb_config_desc_t *config, uint8_t *interface_number, uint8_t *endpoint_address)
{
    const auto *descriptor = reinterpret_cast<const uint8_t *>(config);
    const uint8_t *end = descriptor + config->wTotalLength;
    bool in_midi_streaming_interface = false;

    while (descriptor + 2 <= end && descriptor[0] > 0 && descriptor + descriptor[0] <= end) {
        const uint8_t descriptor_length = descriptor[0];
        const uint8_t descriptor_type = descriptor[1];

        if (descriptor_type == USB_DESCRIPTOR_TYPE_INTERFACE && descriptor_length >= 9) {
            in_midi_streaming_interface = descriptor[5] == MIDI_USB_CLASS_AUDIO && descriptor[6] == USB_SUBCLASS_MIDISTREAMING;
            if (in_midi_streaming_interface) {
                *interface_number = descriptor[2];
            }
        } else if (descriptor_type == USB_DESCRIPTOR_TYPE_ENDPOINT && descriptor_length >= 7 && in_midi_streaming_interface) {
            const uint8_t address = descriptor[2];
            const uint8_t attributes = descriptor[3] & 0x03;

            if ((address & USB_ENDPOINT_DIR_IN) != 0 && attributes == USB_TRANSFER_TYPE_BULK) {
                *endpoint_address = address;
                return true;
            }
        }

        descriptor += descriptor_length;
    }

    return false;
}

void close_device()
{
    for (usb_transfer_t *&transfer : state.transfers) {
        if (transfer != nullptr) {
            usb_host_transfer_free(transfer);
            transfer = nullptr;
        }
    }

    if (state.interface_claimed) {
        usb_host_interface_release(state.client, state.device, state.interface_number);
        state.interface_claimed = false;
    }

    if (state.device != nullptr) {
        usb_host_device_close(state.client, state.device);
        state.device = nullptr;
    }

    state.device_open = false;
}

esp_err_t open_device(uint8_t device_address)
{
    esp_err_t ret = ESP_OK;

    ESP_LOGI(TAG, "Opening USB device at address %u", device_address);

    ESP_RETURN_ON_ERROR(usb_host_device_open(state.client, device_address, &state.device), TAG, "Failed to open USB device");
    state.device_open = true;

    const usb_config_desc_t *config = nullptr;
    ESP_GOTO_ON_ERROR(usb_host_get_active_config_descriptor(state.device, &config), fail, TAG,
                      "Failed to get active USB config descriptor");

    if (!find_midi_input_endpoint(config, &state.interface_number, &state.endpoint_address)) {
        ESP_LOGW(TAG, "Connected USB device is not a supported MIDI input device");
        goto fail;
    }

    ESP_LOGI(TAG, "Found USB MIDI IN endpoint 0x%02x on interface %u", state.endpoint_address, state.interface_number);

    ESP_GOTO_ON_ERROR(usb_host_interface_claim(state.client, state.device, state.interface_number, 0), fail, TAG,
                      "Failed to claim MIDI interface");
    state.interface_claimed = true;

    for (usb_transfer_t *&transfer : state.transfers) {
        ESP_GOTO_ON_ERROR(usb_host_transfer_alloc(MIDI_TRANSFER_BYTES, 0, &transfer), fail, TAG,
                          "Failed to allocate MIDI transfer");

        transfer->device_handle = state.device;
        transfer->bEndpointAddress = state.endpoint_address;
        transfer->callback = midi_transfer_callback;
        transfer->context = nullptr;
        transfer->num_bytes = MIDI_TRANSFER_BYTES;
    }

    for (usb_transfer_t *transfer : state.transfers) {
        ESP_GOTO_ON_ERROR(usb_host_transfer_submit(transfer), fail, TAG, "Failed to submit MIDI transfer");
    }
    ESP_LOGI(TAG, "USB MIDI host is listening for note on/off messages");

    return ESP_OK;

fail:
    close_device();
    return ret == ESP_OK ? ESP_FAIL : ret;
}

void usb_daemon_task(void *)
{
    while (true) {
        uint32_t event_flags = 0;
        const esp_err_t err = usb_host_lib_handle_events(portMAX_DELAY, &event_flags);
        if (err != ESP_OK) {
            ESP_LOGW(TAG, "USB host daemon event error: %s", esp_err_to_name(err));
        }
    }
}

void usb_client_task(void *)
{
    usb_host_client_config_t client_config = {};
    client_config.is_synchronous = false;
    client_config.max_num_event_msg = 5;
    client_config.async.client_event_callback = client_event_callback;
    client_config.async.callback_arg = nullptr;

    esp_err_t err = usb_host_client_register(&client_config, &state.client);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to register USB host client: %s", esp_err_to_name(err));
        vTaskDelete(nullptr);
        return;
    }

    while (true) {
        err = usb_host_client_handle_events(state.client, pdMS_TO_TICKS(50));
        if (err != ESP_OK && err != ESP_ERR_TIMEOUT) {
            ESP_LOGW(TAG, "USB host client event error: %s", esp_err_to_name(err));
        }

        if (state.pending_device_address != 0) {
            if (state.device_open) {
                close_device();
            }

            const uint8_t address = state.pending_device_address;
            state.pending_device_address = 0;
            open_device(address);
        }
    }
}

} // namespace

esp_err_t usb_midi_host_init()
{
    if (state.initialized) {
        return ESP_OK;
    }

    usb_host_config_t host_config = {};
    host_config.skip_phy_setup = false;
    host_config.root_port_unpowered = false;
    host_config.intr_flags = ESP_INTR_FLAG_LEVEL1;

    ESP_RETURN_ON_ERROR(usb_host_install(&host_config), TAG, "Failed to install USB host");

    state.initialized = true;
    return ESP_OK;
}

esp_err_t usb_midi_host_start()
{
    ESP_RETURN_ON_FALSE(state.initialized, ESP_ERR_INVALID_STATE, TAG, "USB MIDI host is not initialized");

    if (state.started) {
        return ESP_OK;
    }

    state.packet_queue = xQueueCreate(MIDI_PACKET_QUEUE_LEN, sizeof(MidiPacketBatch));
    ESP_RETURN_ON_FALSE(state.packet_queue != nullptr, ESP_ERR_NO_MEM, TAG, "Failed to create USB MIDI packet queue");

    BaseType_t result = xTaskCreate(midi_parser_task, "usb_midi_parser", 4096, nullptr, 12, &state.parser_task);
    ESP_RETURN_ON_FALSE(result == pdPASS, ESP_ERR_NO_MEM, TAG, "Failed to create USB MIDI parser task");

    result = xTaskCreate(usb_daemon_task, "usb_host_daemon", 4096, nullptr, 20, &state.daemon_task);
    ESP_RETURN_ON_FALSE(result == pdPASS, ESP_ERR_NO_MEM, TAG, "Failed to create USB host daemon task");

    result = xTaskCreate(usb_client_task, "usb_midi_client", 4096, nullptr, 10, &state.client_task);
    ESP_RETURN_ON_FALSE(result == pdPASS, ESP_ERR_NO_MEM, TAG, "Failed to create USB MIDI client task");

    state.started = true;
    return ESP_OK;
}

esp_err_t usb_midi_host_set_note_callback(midi_note_callback_t callback, void *user_data)
{
    state.note_callback = callback;
    state.note_callback_user_data = user_data;
    return ESP_OK;
}

esp_err_t usb_midi_host_set_panic_callback(midi_panic_callback_t callback, void *user_data)
{
    state.panic_callback = callback;
    state.panic_callback_user_data = user_data;
    return ESP_OK;
}
