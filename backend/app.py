import asyncio
import json
import logging
import os

import numpy as np
from dotenv import load_dotenv
from fastapi import FastAPI, WebSocket, WebSocketDisconnect
from google import genai
from google.genai import types
from openwakeword.model import Model as WakeWordModel
from openwakeword.utils import download_models as download_wakeword_models

load_dotenv()

logging.basicConfig(level=logging.INFO)
log = logging.getLogger("mr-greeny")

app = FastAPI()

SAMPLE_RATE_IN = 16000  # board mic, matches firmware/main/main.c
FRAME_TYPE_MIC_PCM = 0x10  # board -> backend
FRAME_TYPE_REPLY_PCM = 0x20  # backend -> board, 24kHz PCM (not yet consumed by firmware)

# Preview model name taken from a known-working Gemini Live integration
# (OmniBot's Pixel bot). Preview model names/availability can shift over
# time -- check https://ai.google.dev/gemini-api/docs/live-api if this
# stops working.
LIVE_MODEL = "gemini-3.1-flash-live-preview"

GEMINI_API_KEY = os.environ.get("GEMINI_API_KEY")
genai_client = genai.Client(api_key=GEMINI_API_KEY) if GEMINI_API_KEY else None
if genai_client is None:
    log.warning("GEMINI_API_KEY not set (see backend/.env) -- Live sessions will not start")

# openWakeWord ships pretrained models and also supports training a fully
# custom wake word from your own recordings (fully open source, no vendor
# account needed) -- see the training notebook in the openWakeWord repo:
# https://github.com/dscripka/openWakeWord
# Drop your trained .onnx/.tflite file under ./wakeword_models/ and list its
# path here. Leaving this empty falls back to openWakeWord's bundled
# pretrained models (useful to confirm the pipeline works end-to-end first).
WAKEWORD_MODEL_PATHS: list[str] = [
    # "wakeword_models/hey_greeny.onnx",
]

download_wakeword_models()  # no-op if already downloaded; onnx models aren't bundled in the pip package
wakeword_model = WakeWordModel(wakeword_models=WAKEWORD_MODEL_PATHS, inference_framework="onnx")
WAKE_SCORE_THRESHOLD = 0.5


class WakeGate:
    """Runs openWakeWord on incoming PCM until the wake word fires once.

    After that this connection is considered "awake" for good -- all further
    mic audio streams straight into the Gemini Live session, which does its
    own turn-taking/VAD server-side. TODO: add a sleep-timeout (like Pixel's
    runtime_sleep_timeout) to re-arm the wake word after a period of silence.
    """

    def __init__(self):
        self.awake = False

    def check(self, pcm_bytes: bytes) -> bool:
        if self.awake:
            return False
        samples = np.frombuffer(pcm_bytes, dtype=np.int16)
        predictions = wakeword_model.predict(samples)
        if any(score > WAKE_SCORE_THRESHOLD for score in predictions.values()):
            self.awake = True
            return True
        return False


class LiveSession:
    """One Gemini Live API session for a connected board: PCM in, audio+text out."""

    def __init__(self, client: genai.Client, send_text, send_binary):
        self._client = client
        self._send_text = send_text
        self._send_binary = send_binary
        self._session_cm = None
        self._session = None
        self._pcm_queue: asyncio.Queue = asyncio.Queue(maxsize=256)
        self._sender_task: asyncio.Task | None = None
        self._receiver_task: asyncio.Task | None = None

    async def ensure_started(self):
        if self._session is not None:
            return
        config = types.LiveConnectConfig(
            response_modalities=[types.Modality.AUDIO],
            output_audio_transcription=types.AudioTranscriptionConfig(),
            system_instruction=(
                "You are Mr. Greeny, a friendly voice assistant living inside a small "
                "desk gadget. Keep replies brief and conversational."
            ),
        )
        self._session_cm = self._client.aio.live.connect(model=LIVE_MODEL, config=config)
        self._session = await self._session_cm.__aenter__()
        self._sender_task = asyncio.create_task(self._sender_loop())
        self._receiver_task = asyncio.create_task(self._receiver_loop())
        log.info("Gemini Live session opened")

    def feed_pcm(self, chunk: bytes) -> None:
        try:
            self._pcm_queue.put_nowait(chunk)
        except asyncio.QueueFull:
            pass

    async def _sender_loop(self):
        while True:
            chunk = await self._pcm_queue.get()
            if chunk is None:
                break
            try:
                await self._session.send_realtime_input(
                    audio=types.Blob(data=chunk, mime_type=f"audio/pcm;rate={SAMPLE_RATE_IN}")
                )
            except Exception:
                log.exception("Live audio send failed")

    async def _receiver_loop(self):
        try:
            async for msg in self._session.receive():
                sc = msg.server_content
                if sc is None:
                    continue

                if sc.output_transcription and sc.output_transcription.text:
                    await self._send_text(json.dumps({
                        "status": "success",
                        "reply": sc.output_transcription.text,
                    }))

                if sc.model_turn:
                    for part in sc.model_turn.parts:
                        inline = getattr(part, "inline_data", None)
                        if inline and inline.data:
                            await self._send_binary(bytes([FRAME_TYPE_REPLY_PCM]) + inline.data)
        except asyncio.CancelledError:
            pass
        except Exception:
            log.exception("Live receive loop ended")

    async def close(self):
        for t in (self._sender_task, self._receiver_task):
            if t is not None:
                t.cancel()
        if self._session_cm is not None:
            try:
                await self._session_cm.__aexit__(None, None, None)
            except Exception:
                log.debug("Live session __aexit__ failed", exc_info=True)


@app.websocket("/ws/stream")
async def stream_endpoint(websocket: WebSocket):
    await websocket.accept()
    log.info("board connected")

    gate = WakeGate()
    live = LiveSession(genai_client, websocket.send_text, websocket.send_bytes) if genai_client else None

    try:
        while True:
            message = await websocket.receive()

            data = message.get("bytes")
            if data is not None:
                packet_type, payload = data[0], data[1:]
                if packet_type != FRAME_TYPE_MIC_PCM:
                    log.warning("unknown packet type: 0x%02x", packet_type)
                    continue

                if not gate.awake:
                    if gate.check(payload):
                        log.info("wake word detected")
                        if live is None:
                            log.warning("no GEMINI_API_KEY configured, can't start Live session")
                        else:
                            await live.ensure_started()
                elif live is not None:
                    live.feed_pcm(payload)
                continue

            text = message.get("text")
            if text is not None:
                log.info("text message from board: %s", text)

    except WebSocketDisconnect:
        log.info("board disconnected")
    finally:
        if live is not None:
            await live.close()
