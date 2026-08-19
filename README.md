# TeslaCoilKlavier / TeCKla

ESP-IDF firmware for an ESP32-P4 Waveshare-style module dev kit.

- USB host MIDI keyboard input.
- Onboard ES8311 speaker output.
- External PCM5102A I2S DAC output with the same mixed audio.
- PicoTTS spoken assistant output.
- One central software mixer for MIDI tones, feedback sounds, and TTS samples.

No Tesla coil, motor, or external actuator output is driven by this firmware.

## Features

- Plays synthesized MIDI tones from a connected USB MIDI keyboard.
- Mixes MIDI tones, assistant feedback sounds, and PicoTTS speech through one I2S audio task.
- Uses the onboard ES8311 codec/speaker path and an optional external PCM5102A DAC output.
- Uses robust USB MIDI input handling with multiple transfers in flight and a parser task.
- Clears all held MIDI notes on USB disconnect, USB transfer errors, and MIDI all-notes-off style controller messages.

## Assistant Controls

TeCKla tracks the currently held MIDI keys and listens for a secret activation gesture:

- Activation: hold exactly five white keys and three black keys, three times within five seconds.
- Tutorial: after the spoken menu, release all keys, then hold three white keys.
- Tesla coil physics: hold four white keys.
- xstage story: hold any black-key choice that is not the joke gesture.
- Joke: hold one white key and one black key.
- Repeat menu: press one low key, note `36` or below.
- Exit: press one high key, note `96` or above.

After TeCKla speaks, menu input must be fresh: release all keys once, then press a choice within five seconds. This prevents held or stale keys from repeating TTS.

## MIDI Robustness

USB MIDI receive is designed to avoid missed note-off bursts during chord playing:

- Four USB IN transfers are kept in flight.
- The USB transfer callback only copies received bytes to a queue and resubmits the transfer.
- MIDI packet parsing and note dispatch run in a separate parser task.
- USB-MIDI Code Index Number values are validated before parsing note/control messages.
- MIDI Control Change `120`, `121`, and `123` clear all active notes.
- MIDI messages are logged at info level from a dedicated logger task, not from USB callbacks.
- MIDI logging is non-blocking for parsing. If logging cannot keep up, log events are dropped and a warning reports the dropped count.

## Audio Notes

- Codec output volume is currently set to `70` in `main/teckla_audio.cpp`.
- MIDI tones use a fundamental plus second harmonic. `MIDI_SECOND_HARMONIC_GAIN` is currently `0.35f`.
- `audio_task()` is the only audio writer. TTS, MIDI, and feedback audio must go through the mixer.
- The onboard ES8311 uses I2S port `0`: `MCLK=GPIO13`, `BCLK=GPIO12`, `WS=GPIO10`, `DOUT=GPIO9`, `DIN=GPIO11`.
- The external PCM5102A output uses I2S port `1`: `BCLK=GPIO46`, `WS/LRCK=GPIO27`, `DOUT=GPIO45`. Connect PCM5102A `DIN` to ESP32 `DOUT`.
- The PCM5102A output receives the same mixed stereo audio as the onboard speaker path.

## Build

Use an ESP-IDF shell with ESP-IDF 6.0.1 or newer:

```powershell
idf.py set-target esp32p4
idf.py build
```

## Flash

The board has been tested on `COM10`:

```powershell
idf.py -p COM10 flash
```

To monitor with debug-level MIDI note logs, enable a debug log level in ESP-IDF configuration or adjust the MIDI note log level in `main/usb_midi_host.cpp`.
