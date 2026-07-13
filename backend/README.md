# mr-greeny backend

FastAPI hub that the board's firmware streams raw mic audio to over a single
WebSocket (`/ws/stream`). See [app.py](app.py) and the root
[README.md](../README.md) for the protocol and overall architecture.

## Setup

```bash
cd backend
python3 -m venv .venv
source .venv/bin/activate
pip install -r requirements.txt
cp .env.example .env    # then fill in GEMINI_API_KEY
```

First run downloads openWakeWord's pretrained models automatically (needs
network access once).

`.env` is git-ignored on purpose — never commit real API keys.

## Run

```bash
uvicorn app:app --host 0.0.0.0 --port 8000 --reload
```

Point the firmware's `MRGREENY_BACKEND_WS_URL` (`idf.py menuconfig` -> Mr.
Greeny Configuration) at `ws://<this-machine's-LAN-IP>:8000/ws/stream`.

## Status of this scaffold

- Wake-word detection (openWakeWord) gates when a Gemini **Live** session
  opens for a connection — see `WakeGate` in `app.py`.
- Once awake, mic PCM streams continuously into the Live session
  (`LiveSession.feed_pcm`); Gemini's own server-side VAD handles turn-taking,
  so there's no more manual "collect until silence" step.
- Output transcription text comes back over the socket as
  `{"status": "success", "reply": "..."}` JSON, same as before.
- Output *audio* (24kHz PCM) is sent back as a new `0x20`-prefixed binary
  packet, but firmware doesn't play it through the speaker yet — that's the
  next piece to build (ES8311 playback via `bsp_audio_codec_speaker_init()`).
- Falls back to openWakeWord's bundled pretrained wake words until you train
  and drop in a custom one under `wakeword_models/`.
- No sleep-timeout yet: once a connection wakes up, it stays streaming into
  Gemini Live for the life of that WebSocket connection (see the `WakeGate`
  docstring in `app.py`).
