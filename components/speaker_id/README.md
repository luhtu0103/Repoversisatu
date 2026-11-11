# Speaker ID prototype: MFCC + DTW (ESP32-S3 / ESP-IDF)

Prototype speaker-comparison component for mono signed PCM16 at 16 kHz.
It uses no neural-network model and does not need an internet connection.

## Files
- `components/speaker_id/include/speaker_id.h`: public C API
- `components/speaker_id/speaker_id.cpp`: MFCC feature extraction and DTW comparison
- `components/speaker_id/CMakeLists.txt`: ESP-IDF component registration

## Input requirements
- Mono, signed PCM16
- 16,000 samples/second
- Prefer 1-3 seconds of speech per recording
- Avoid clipping, silence, music and strong background noise

## API usage

Call these functions from the existing application capture flow:

- `speaker_id_init()` loads the saved named template from NVS during startup.
- `speaker_id_enroll_named_pcm16(pcm, sample_count, name)` extracts MFCC features and commits the named template to NVS.
- `speaker_id_compare_pcm16(...)` remains available for experimental comparison; its score is not a calibrated identity decision.

## Explicit Gemini enrollment

In an active Gemini Live session, say something explicit such as:

> “Daftarkan suaraku, namaku Bos.”

Gemini invokes the blocking `speaker_enroll` tool only after the transcript is recognized as an explicit enrollment request. The device captures approximately 1.2 seconds from the already-running microphone path, extracts MFCC features, and stores the name and template in NVS. Gemini receives success only after the NVS commit succeeds.

BOOT is not an enrollment or comparison trigger. A short BOOT press retains its existing behavior of starting a Gemini session. The application must not start a second I2S reader.

## Important limitations

- This is a baseline MFCC + DTW experiment, not a validated speaker-verification model.
- The DTW distance is uncalibrated and does not prove identity. Do not use it alone for access control.
- Test different recordings, distances, languages, and background-noise conditions before relying on comparisons.
- The direct DFT is CPU-intensive compared with an optimized FFT implementation.
- Firmware compilation and hardware testing are still required.
