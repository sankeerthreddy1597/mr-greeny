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
```

First run downloads openWakeWord's pretrained models automatically (needs
network access once).

## Run

```bash
uvicorn app:app --host 0.0.0.0 --port 8000 --reload
```

Point the firmware's `MRGREENY_BACKEND_WS_URL` (`idf.py menuconfig` -> Mr.
Greeny Configuration) at `ws://<this-machine's-LAN-IP>:8000/ws/stream`.

## Status of this scaffold

- Wake-word detection and a naive silence-based utterance boundary are wired
  up and will log when they fire.
- The actual AI speech call (`run_ai_speech_turn` in `app.py`) is a stub —
  no LLM/model credentials are wired in yet, on purpose.
- Falls back to openWakeWord's bundled pretrained wake words until you train
  and drop in a custom one under `wakeword_models/`.
