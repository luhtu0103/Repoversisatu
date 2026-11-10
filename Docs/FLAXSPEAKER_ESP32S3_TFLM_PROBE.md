# FlaxSpeaker ESP32-S3 TFLite Micro probe

Branch: feature/flaxspeaker-local-id

This is an isolated test application. It does not add TFLite Micro or FlaxSpeaker to the Repo5 production firmware and does not alter the Gemini audio path.

## Automated CI gate

The workflow downloads both exports from the pinned Hugging Face revision and stages the FP32 export in the isolated probe. The probe builds using ESP-IDF 6.0.1 and espressif/esp-tflite-micro 1.4.2. Build success checks compilation only; it is not a hardware inference result.

## Why FP32 first

The INT8 export failed on hardware during AllocateTensors() at FULLY_CONNECTED: the runtime reported `Input type: FLOAT32 with filter type : INT8 not supported.` This is a hybrid-quantization compatibility issue, not a flash or PSRAM detection failure. The next test uses the FP32 export as a baseline, avoiding an unvalidated change to quantization or accuracy. The FP32 model may increase firmware size and inference memory/latency; CI and hardware results must confirm it fits.

## Hardware test

After CI produces the probe binary, flash it only to a spare/test ESP32-S3 board, not the robot's production firmware. Open serial monitor at 115200 baud and look for FLAX_TFLM_PROBE logs:

- `allocate_tensors status=OK` means the model operators were accepted and tensors fit the 4 MiB PSRAM arena.
- `ZERO_INPUT_INVOKE_PASS` means a zero-input Invoke completed and produced finite output.
- `FAIL` indicates the model could not be loaded/allocated/invoked or the tensor contract differs.
- Record latency_ms, arena_used, internal heap, and PSRAM before/after.

The test embeds the model in the test app and requests a 4 MiB PSRAM arena. Actual available PSRAM depends on the board/configuration. The model input is float32 [1,180,80], so this probe does not validate the audio frontend, real PCM recordings, identity thresholds, or false accept/reject rates. Do not merge into main based on a successful build alone.

## CI dependency notes

The probe component explicitly declares its direct ESP-IDF dependencies (esp_timer, heap, and log) alongside esp-tflite-micro. It uses MicroMutableOpResolver and explicitly registers built-in ops instead of relying on an all-ops resolver. This may expose unsupported graph operators during AllocateTensors(); it does not claim the complete FlaxSpeaker graph is supported.

The Espressif component package may not expose tensorflow/lite/schema/schema_generated.h. CI therefore fetches only that generated header from a pinned TensorFlow Lite Micro commit into the test-only compatibility include directory. This is a build-system workaround, not proof that the model's operators are supported.

## Automated operator inventory

CI emits a static operator inventory for the pinned FP32 model. It lists operator counts and compares them with operators explicitly registered by the isolated probe. A clean list still does not replace a real ESP32-S3 AllocateTensors() and Invoke() test.
