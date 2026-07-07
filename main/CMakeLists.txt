idf_component_register(
    SRCS
        "main.cpp"
        "usb_midi_host.cpp"
        "teckla_audio.cpp"
        "teckla_tts.cpp"
        "teckla_assistant.cpp"
        "teckla_content.cpp"
    INCLUDE_DIRS "."
    REQUIRES
        esp_driver_gpio
        esp_driver_i2c
        esp_driver_i2s
        esp_timer
        es8311
        picotts
        usb
)
