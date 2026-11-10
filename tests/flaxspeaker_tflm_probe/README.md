# FlaxSpeaker TFLite Micro Probe

ESP32-S3 runtime smoke test for the FlaxSpeaker TFLite Micro model.

## Model requirement

Place the verified model artifact at `main/model_int8.tflite` before building. The probe expects input `float32[1,180,80]` and output `float32[1,192]`. A different tensor contract must be investigated rather than bypassed.

## Build

Run from this directory in an ESP-IDF environment:

```sh
idf.py set-target esp32s3
idf.py build
```

This is a zero-input runtime smoke test only. A pass does not prove real-audio frontend parity or speaker-identification accuracy. Do not flash this probe over the robot's production firmware.
