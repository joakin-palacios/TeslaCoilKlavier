#include "usb_midi_host.hpp"

#include <algorithm>
#include <cstring>

#include "esp_check.h"
#include "esp_log.h"
#include "esp_timer.h"
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
constexpr size_t MIDI_LOG_QUEUE_LEN = 128;

enum class MidiLogMessageType : uint8_t {
    NoteOff,
    NoteOn,
    PolyPressure,
    ControlChange,
    ProgramChange,
    ChannelPressure,
    PitchBend,
    SysEx,
    SystemCommon,
    SystemRealtime,
    Unknown,
};

struct MidiPacketBatch {
    int length;
    uint8_t data[MIDI_TRANSFER_BYTES];
};

struct MidiLogEvent {
    MidiLogMessageType type;
    uint32_t time_ms;
    uint16_t value;
    uint8_t cable;
    uint8_t cin;
    uint8_t status;
    uint8_t channel;
    uint8_t data1;
    uint8_t data2;
};

struct UsbMidiHostState {
    usb_host_client_handle_t client = nullptr;
    usb_device_handle_t device = nullptr;
    usb_transfer_t *transfers[MIDI_TRANSFER_COUNT] = {};
    QueueHandle_t packet_queue = nullptr;
    QueueHandle_t log_queue = nullptr;
    TaskHandle_t daemon_task = nullptr;
    TaskHandle_t client_task = nullptr;
    TaskHandle_t parser_task = nullptr;
    TaskHandle_t logger_task = nullptr;
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
    uint32_t dropped_log_count = 0;
};

UsbMidiHostState state;
portMUX_TYPE log_count_lock = portMUX_INITIALIZER_UNLOCKED;

void emit_panic(const char *reason)
{
    ESP_LOGW(TAG, "Clearing MIDI note state: %s", reason);
    if (state.panic_callback != nullptr) {
        state.panic_callback(state.panic_callback_user_data);
    }
}

MidiLogMessageType decode_midi_log_type(uint8_t cin, uint8_t status, uint8_t data2)
{
    if (status >= 0xF8) {
        return MidiLogMessageType::SystemRealtime;
    }
    if (status >= 0xF0) {
        switch (cin) {
        case 0x4:
        case 0x5:
        case 0x6:
        case 0x7:
            return MidiLogMessageType::SysEx;
        case 0x2:
        case 0x3:
        case 0xF:
            return MidiLogMessageType::SystemCommon;
        default:
            return MidiLogMessageType::Unknown;
        }
    }

    switch (status & 0xF0) {
    case 0x80:
        return MidiLogMessageType::NoteOff;
    case 0x90:
        return data2 == 0 ? MidiLogMessageType::NoteOff : MidiLogMessageType::NoteOn;
    case 0xA0:
        return MidiLogMessageType::PolyPressure;
    case 0xB0:
        return MidiLogMessageType::ControlChange;
    case 0xC0:
        return MidiLogMessageType::ProgramChange;
    case 0xD0:
        return MidiLogMessageType::ChannelPressure;
    case 0xE0:
        return MidiLogMessageType::PitchBend;
    default:
        return MidiLogMessageType::Unknown;
    }
}

void increment_dropped_log_count()
{
    portENTER_CRITICAL(&log_count_lock);
    ++state.dropped_log_count;
    portEXIT_CRITICAL(&log_count_lock);
}

uint32_t take_dropped_log_count()
{
    portENTER_CRITICAL(&log_count_lock);
    const uint32_t count = state.dropped_log_count;
    state.dropped_log_count = 0;
    portEXIT_CRITICAL(&log_count_lock);
    return count;
}

void queue_midi_log_event(const uint8_t *packet)
{
    if (state.log_queue == nullptr) {
        return;
    }

    const uint8_t cin = packet[0] & 0x0F;
    const uint8_t status = packet[1];
    MidiLogEvent event = {
        .type = decode_midi_log_type(cin, status, packet[3]),
        .time_ms = static_cast<uint32_t>(esp_timer_get_time() / 1000ULL),
        .value = static_cast<uint16_t>(packet[2] | (packet[3] << 7)),
        .cable = static_cast<uint8_t>((packet[0] >> 4) & 0x0F),
        .cin = cin,
        .status = status,
        .channel = static_cast<uint8_t>(status & 0x0F),
        .data1 = packet[2],
        .data2 = packet[3],
    };

    if (xQueueSend(state.log_queue, &event, 0) != pdTRUE) {
        increment_dropped_log_count();
    }
}

void log_midi_event(const MidiLogEvent &event)
{
    switch (event.type) {
    case MidiLogMessageType::NoteOff:
        ESP_LOGI(TAG, "MIDI Note Off t=%ums cable=%u ch=%u note=%u velocity=%u", event.time_ms, event.cable, event.channel + 1, event.data1, event.data2);
        break;
    case MidiLogMessageType::NoteOn:
        ESP_LOGI(TAG, "MIDI Note On t=%ums cable=%u ch=%u note=%u velocity=%u", event.time_ms, event.cable, event.channel + 1, event.data1, event.data2);
        break;
    case MidiLogMessageType::PolyPressure:
        ESP_LOGI(TAG, "MIDI Poly Pressure t=%ums cable=%u ch=%u note=%u pressure=%u", event.time_ms, event.cable, event.channel + 1, event.data1, event.data2);
        break;
    case MidiLogMessageType::ControlChange:
        ESP_LOGI(TAG, "MIDI Control Change t=%ums cable=%u ch=%u controller=%u value=%u", event.time_ms, event.cable, event.channel + 1, event.data1, event.data2);
        break;
    case MidiLogMessageType::ProgramChange:
        ESP_LOGI(TAG, "MIDI Program Change t=%ums cable=%u ch=%u program=%u", event.time_ms, event.cable, event.channel + 1, event.data1);
        break;
    case MidiLogMessageType::ChannelPressure:
        ESP_LOGI(TAG, "MIDI Channel Pressure t=%ums cable=%u ch=%u pressure=%u", event.time_ms, event.cable, event.channel + 1, event.data1);
        break;
    case MidiLogMessageType::PitchBend:
        ESP_LOGI(TAG, "MIDI Pitch Bend t=%ums cable=%u ch=%u value=%u", event.time_ms, event.cable, event.channel + 1, event.value);
        break;
    case MidiLogMessageType::SysEx:
        ESP_LOGI(TAG, "MIDI SysEx t=%ums cable=%u cin=0x%01x data=%02x %02x %02x", event.time_ms, event.cable, event.cin, event.status, event.data1, event.data2);
        break;
    case MidiLogMessageType::SystemCommon:
        ESP_LOGI(TAG, "MIDI System Common t=%ums cable=%u cin=0x%01x status=0x%02x data=%02x %02x", event.time_ms, event.cable, event.cin, event.status, event.data1, event.data2);
        break;
    case MidiLogMessageType::SystemRealtime:
        ESP_LOGI(TAG, "MIDI System Realtime t=%ums cable=%u status=0x%02x", event.time_ms, event.cable, event.status);
        break;
    case MidiLogMessageType::Unknown:
        ESP_LOGI(TAG, "MIDI Unknown t=%ums cable=%u cin=0x%01x packet=%02x %02x %02x %02x", event.time_ms, event.cable, event.cin,
                 static_cast<uint8_t>((event.cable << 4) | event.cin), event.status, event.data1, event.data2);
        break;
    }
}

void parse_usb_midi_packet(const uint8_t *packet)
{
    queue_midi_log_event(packet);

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

void midi_logger_task(void *)
{
    MidiLogEvent event = {};
    while (true) {
        const BaseType_t received = xQueueReceive(state.log_queue, &event, pdMS_TO_TICKS(1000));
        const uint32_t dropped = take_dropped_log_count();
        if (dropped > 0) {
            ESP_LOGW(TAG, "MIDI log dropped %u messages", static_cast<unsigned>(dropped));
        }
        if (received == pdTRUE) {
            log_midi_event(event);
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

    state.log_queue = xQueueCreate(MIDI_LOG_QUEUE_LEN, sizeof(MidiLogEvent));
    ESP_RETURN_ON_FALSE(state.log_queue != nullptr, ESP_ERR_NO_MEM, TAG, "Failed to create USB MIDI log queue");

    BaseType_t result = xTaskCreate(midi_parser_task, "usb_midi_parser", 4096, nullptr, 12, &state.parser_task);
    ESP_RETURN_ON_FALSE(result == pdPASS, ESP_ERR_NO_MEM, TAG, "Failed to create USB MIDI parser task");

    result = xTaskCreate(midi_logger_task, "usb_midi_logger", 4096, nullptr, 8, &state.logger_task);
    ESP_RETURN_ON_FALSE(result == pdPASS, ESP_ERR_NO_MEM, TAG, "Failed to create USB MIDI logger task");

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
