# mr-greeny — agent context

Voice assistant on a Waveshare ESP32-S3-Touch-AMOLED-1.75 board (round 466x466
AMOLED, ES8311 speaker + ES7210 mic, no camera): say a wake word, talk to
Gemini Live, hear it reply through the onboard speaker. Loosely modeled on
OmniBot's Pixel bot, simplified since this board has no camera.

## Architecture

```
firmware/   ESP-IDF + LVGL, runs on the board
backend/    Python FastAPI hub, runs on a machine on the same LAN (e.g. this Mac)
```

Single persistent WebSocket, `/ws/stream`, board as client. Binary frames are
`[1-byte type][payload]`:
- `0x10` board→backend: mic PCM, 16kHz mono 16-bit, streamed continuously
- `0x20` backend→board: Gemini's reply audio, resampled server-side from its
  native 24kHz down to 16kHz (the mic/speaker share one duplex I2S bus that
  must run both directions at the same rate)

JSON text frames, backend→board:
- `{"type": "assistant_speaking", "state": "start"|"stop"}` — drives the
  on-screen mouth animation
- `{"status": "success", "reply": "..."}` — transcript text

## Current status: what works

- Wi-Fi + WebSocket connect/reconnect, with UI feedback ("connecting...",
  "reconnecting...")
- Wake word detection (openWakeWord, **tflite backend only**, see gotcha
  below) — bundled pretrained words only: "alexa", "hey jarvis", "hey
  mycroft", "hey rhasspy", "weather". No custom-trained word yet.
- Gemini Live session: opens on wake word, persists across multiple
  conversational turns, auto-closes after `IDLE_TIMEOUT_SECONDS` (10s, in
  `backend/app.py`) of no genuine activity, re-arms the wake word
- Speaker playback of Gemini's replies through the board's ES8311 (volume
  explicitly set to 80 — was missing entirely before, silent failure)
- Mic mutes automatically while the speaker is playing (no AEC on this
  board, so without muting the mic hears its own speaker output and feeds it
  back to Gemini, causing self-interruption/self-answering)
- UI: blinking eyes (idle), pulsing mouth (while Gemini is replying), status
  text (connecting/reconnecting)
- Backend correctly handles Gemini's `go_away` and other Live API signal
  messages (`session_resumption_update`, `voice_activity`, etc.)

## Known open items

1. **Minor cosmetic display glitch**: stray pixels near the mouth's right
   edge while talking, don't clear. Root cause: SPI DMA queue contention
   between the display and concurrent mic/network/Gemini-Live activity (the
   BSP's SPI `trans_queue_depth` is Kconfig-capped at 10, not raisable
   through normal config). Already improved substantially (moved a 32KB
   reassembly buffer to PSRAM, pinned LVGL to core 1 vs. mic/playback tasks
   on core 0) but not fully eliminated. **User explicitly deprioritized this
   for now** — don't re-chase it unprompted. Full history in Claude's
   persistent memory (`mrgreeny_known_display_glitch.md`) if picking it back
   up later.
2. **No barge-in**: muting the mic during playback means you can't interrupt
   the assistant mid-reply by talking over it. Deliberate tradeoff, not a bug.
3. **No custom wake word trained yet** — relies on openWakeWord's bundled
   pretrained words. Training notebook is in the openWakeWord repo
   (github.com/dscripka/openWakeWord); drop the trained file in
   `backend/wakeword_models/` and list it in `WAKEWORD_MODEL_PATHS` in
   `backend/app.py`.
4. **Backend's LAN IP is hardcoded** into `firmware/sdkconfig`
   (`CONFIG_MRGREENY_BACKEND_WS_URL`) — if the backend machine's DHCP IP
   changes, the board silently fails to connect until sdkconfig is updated
   and firmware reflashed. No mDNS/hostname resolution set up. This has
   already happened once during development.
5. `play_test_tone()` in `firmware/main/main.c` (`app_main`) is a diagnostic
   leftover — plays a 440Hz tone at boot to test speaker hardware in
   isolation from the network/Gemini pipeline. Safe to remove now that
   speaker output is confirmed working, or keep as a boot self-test.

## Non-obvious gotchas — read before touching related code

- **openWakeWord's ONNX backend is broken in this environment.** A clean
  synthesized "Alexa" clip scored ~0 on onnx but a clean 1.0 on tflite,
  verified directly against `app.py`'s actual model object. The backend
  shims `tflite_runtime.interpreter` to point at `ai_edge_litert.Interpreter`
  (real `tflite-runtime` has no wheel for this Python/macOS combo) and uses
  `inference_framework="tflite"`. Do not switch back to onnx.
- **`esp_websocket_client` must stay pinned below 1.6.0** (`~1.5` in
  `firmware/main/idf_component.yml`) — 1.6.0+ needs a newer `tcp_transport`
  API than ships in this ESP-IDF v5.5.0 checkout; the build fails otherwise.
- **The audio reassembly buffer must live in PSRAM**
  (`init_reply_audio_buffer()` in `main.c`, `heap_caps_malloc(...,
  MALLOC_CAP_SPIRAM)`), never a plain `static` array — a 32KB static buffer
  measurably starved the display's DMA-capable internal RAM pool and
  corrupted the whole screen continuously (confirmed via `idf.py size` +
  bisection). Any future large buffer should default to PSRAM too.
- **The speaker's I2S channel opens lazily and closes after an idle gap**
  (`PLAYBACK_IDLE_CLOSE_MS = 800` in `main.c`), never held open continuously
  — an always-open channel contends with the display's own SPI DMA even
  while silent.
- **LVGL is pinned to core 1; `mic_stream_task`/`playback_task` are pinned
  to core 0** (`start_display()`'s custom `bsp_display_cfg_t`,
  `xTaskCreatePinnedToCore` calls) — reduces (doesn't fully eliminate)
  display SPI contention with network/audio activity.
- **Gemini Live sends `go_away` and other signal-only messages
  (`session_resumption_update`, `voice_activity`, `usage_metadata`) as
  top-level fields on `LiveServerMessage`, not nested in `server_content`.**
  The idle timer must only reset on genuine `server_content` (model_turn /
  transcription / turn_complete) — resetting on every message including
  these meant the session never actually went idle (confirmed bug, fixed in
  `LiveSession._receiver_loop`).
- **`WakeGate.check()` calls `wakeword_model.predict()` on every incoming
  chunk regardless of awake/asleep state** — skipping it while awake let the
  model's internal sliding-window buffer go stale. The actual root cause of
  "wake word instantly re-triggers after every idle-close" turned out to be
  mic/speaker audio feedback (the mic heard the assistant's own voice for
  the whole reply, before mic-muting existed) rather than this specifically,
  but keeping `predict()` continuously fed is still correct practice and
  worth keeping either way.
- Reflashing or restarting the backend drops the board's WebSocket
  connection, and the board's own reconnect logic can be slow to notice a
  dead connection on its own. `esptool --after hard_reset run` (a reset, not
  a reflash) reliably forces an immediate reconnect attempt. Use
  `backend/run.sh` to restart the backend itself.

## Repo layout

- `firmware/main/main.c` — all firmware logic: UI (eyes/mouth/status text),
  mic capture, speaker playback, WebSocket client, wake-word-adjacent state
- `backend/app.py` — FastAPI backend; `WakeGate` (openWakeWord) and
  `LiveSession` (Gemini Live) classes
- `backend/test_wakeword_mic.py` — standalone tool: tests openWakeWord
  against this machine's own mic, fully independent of the board
- `backend/run.sh` — starts the backend (`./run.sh` from `backend/`)
