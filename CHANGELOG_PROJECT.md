# CHANGELOG_PROJECT

## 2026-11-03 — Gemini Live realtime audio tuning

- Increased microphone-to-TX audio command size from 640 bytes (20 ms) to 1280 bytes (40 ms) to halve Base64/JSON/WebSocket send-call frequency. This reduces per-message overhead; it does not guarantee that a slow underlying WebSocket send is fixed.
- Disabled temporary mbedTLS debug logging after runtime logs confirmed TLS handshake, WebSocket connection, and Gemini setup completion.
- Rate-limited application-level AFE feed/fetch and PCM-fallback warnings (first failure and every 50th failure) to reduce repeated logging in the realtime path. This does not suppress ESP-SR's own internal warnings.
- Updated the TX queue headroom comment to reflect 1280-byte microphone frames.
- Validation: source edits reviewed against the current branch. Firmware build and on-device audio validation have not yet been run; collect fresh TX duration, queue-full, AFE, and playback-underrun logs after flashing.
## 2026-11-03 — Protect Gemini TX control commands

- Matched Xiaozhi's realtime-queue principle: audio is disposable under backpressure, but control messages must not be discarded as if they were audio.
- Changed the full TX queue policy so an incoming microphone frame may evict an old audio frame, but does not evict queued Gemini setup or text/prompt commands.
- Architecture audit: Repo5 already separates WebSocket TX, WebSocket RX, and speaker playback into independent tasks. Microphone capture, WakeNet processing, and AFE feed/fetch still share `audio_task` / the synchronous `audio_read_mic()` path; splitting that safely requires a dedicated bounded frame queue and lifecycle changes, so it is not being done blindly in this change.
- Validation: source-level change only; firmware build and device testing still required.

## 2026-11-03 — Separate microphone capture from AFE/network work

- Added a dedicated pinned-core microphone capture task that continuously reads I2S into a bounded 8-frame PCM queue.
- WakeNet/raw and Gemini/AFE consumers now read captured PCM from that queue, so AFE processing and TX queue backpressure no longer block the I2S capture task directly.
- Queue overflow drops the newest capture frame and emits rate-limited diagnostics; this is bounded behavior, not a guarantee that no audio is lost under sustained overload.
- Validation: source-level changes only. Build and on-device checks are required, especially to confirm the ESP-SR AFE feed frame size matches the 512-sample capture frame and to tune task priorities/queue depth.
- Follow-up hardening: microphone capture now accumulates partial I2S DMA reads into full 512-sample frames before enqueueing, rather than incorrectly dropping every short driver read.
- Follow-up hardening: PCM reads now wait to fill the requested sample count across capture-frame boundaries, preserving exact AFE/WakeNet frame sizes when those sizes are not 512 samples.
- Follow-up hardening: clear queued and partially consumed microphone PCM whenever playback mutes/unmutes the microphone, preventing stale pre-playback audio from leaking into a later Gemini turn.
