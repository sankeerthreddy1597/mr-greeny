# mr-greeny

A voice-assistant-style companion for the Waveshare ESP32-S3-Touch-AMOLED-1.75
board (round 466x466 AMOLED, ES8311 speaker + ES7210 mic codecs, no camera).
Modeled on the client/server split used by OmniBot's "Pixel" bot, simplified
since this board has no camera and doesn't need video/vision handling.

## Architecture

```
firmware/  ESP-IDF + LVGL app running on the board
backend/   Python FastAPI hub running on your machine (or a Pi, etc.)
```

The board is a thin capture/render client; the backend does all the
heavy/compute work. Single persistent WebSocket, board as client:

```
board mic (ES7210, 16kHz PCM)
  --0x10 binary chunks-->  backend /ws/stream
                              -> wake-word detection (openWakeWord)
                              -> silence-based utterance boundary
                              -> [TODO] LLM/speech call
  <--JSON {status, reply}--  backend
board renders reply / speaks it back (not wired up yet)
```

This mirrors Pixel's `[1-byte type][payload]` binary framing convention
(`0x10` = PCM chunk) so the two sides have an unambiguous shared contract,
but drops everything camera/vision-related (`0x02/0x06/0x07` packet types,
JPEG handling, face recognition) since this board has no camera.

Wake-word detection runs **server-side**, same as Pixel: the board streams
mic audio continuously rather than running a keyword spotter on-device. This
was the deliberate tradeoff made for this scaffold — simpler firmware, at the
cost of constant audio upload and it being less private than an on-device
wake word. If that tradeoff stops making sense later, the alternative is
Espressif's own **ESP-SR** component (WakeNet), which runs the wake-word
model on the ESP32-S3 itself and only streams audio after a hit.

## Is a custom wake word possible?

Yes. The backend uses **openWakeWord**, which — unlike vendor-locked options
like Picovoice Porcupine — is fully open source and supports training your
own wake-word model from your own recordings via the training notebook in
its repo (github.com/dscripka/openWakeWord). The trained model file drops
into `backend/wakeword_models/` and gets listed in `WAKEWORD_MODEL_PATHS` in
`backend/app.py`. Until you do that, the backend falls back to openWakeWord's
bundled pretrained wake words so you can verify the pipeline end-to-end first.

## What's scaffolded vs. still a TODO

Done:
- Firmware: Wi-Fi connect, WebSocket client, mic capture via the board's
  ES7210 codec, streaming raw PCM to the backend, basic LVGL status text.
- Backend: WebSocket endpoint, wake-word detection wired to openWakeWord,
  naive silence-based utterance boundary, WAV packaging.

Not done yet (intentionally left as stubs, see inline `TODO`s):
- The actual AI/speech call (`run_ai_speech_turn` in `backend/app.py`) —
  no LLM credentials or provider choice baked in.
- Playing the reply back through the board's speaker (ES8311) — currently
  only a JSON `{status, reply}` message goes back over the socket.
- Any on-screen face/UI beyond a plain status label.
- Real VAD (currently a simple RMS-based silence heuristic).

## Build & flash the firmware

Same workflow as the other ESP-IDF examples in `waveshare-examples`:

```bash
source ~/esp/idf-env.sh
cd firmware
idf.py menuconfig   # set Wi-Fi SSID/password (Example Connection Configuration)
                    # and the backend WebSocket URL (Mr. Greeny Configuration)
idf.py build
idf.py -p /dev/cu.usbmodem1101 flash monitor
```

## Run the backend

```bash
cd backend
python3 -m venv .venv && source .venv/bin/activate
pip install -r requirements.txt
uvicorn app:app --host 0.0.0.0 --port 8000 --reload
```
