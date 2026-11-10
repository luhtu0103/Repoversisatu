# FlaxSpeaker TFLite Micro Probe

ESP32-S3 runtime smoke test for the FlaxSpeaker TFLite Micro model.

## Model requirement

CI stages the pinned FP32 export as main/model_fp32.tflite before building. The probe expects input float32[1,180,80] and output float32[1,192]. The INT8 export currently fails during AllocateTensors() because its FULLY_CONNECTED node mixes FLOAT32 input with INT8 weights in a path rejected by the tested runtime. The FP32 export is the baseline compatibility test; it avoids changing quantization or model weights.

## Build

Run from this directory in an ESP-IDF environment:

```sh
idf.py set-target esp32s3
idf.py build
```

This is a zero-input runtime smoke test only. A pass does not prove real-audio frontend parity or speaker-identification accuracy. Do not flash this probe over the robot's production firmware.
