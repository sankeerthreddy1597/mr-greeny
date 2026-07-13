import io
import json
import logging
import wave

import numpy as np
from fastapi import FastAPI, WebSocket, WebSocketDisconnect
from openwakeword.model import Model as WakeWordModel

logging.basicConfig(level=logging.INFO)
log = logging.getLogger("mr-greeny")

app = FastAPI()

SAMPLE_RATE = 16000
FRAME_TYPE_MIC_PCM = 0x10  # matches the framing used in firmware/main/main.c

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

wakeword_model = WakeWordModel(wakeword_models=WAKEWORD_MODEL_PATHS or None)

WAKE_SCORE_THRESHOLD = 0.5
SILENCE_RMS_THRESHOLD = 200
SILENCE_CHUNKS_TO_END_UTTERANCE = 15  # ~1s at the firmware's ~64ms/chunk rate


class Session:
    """Per-connection state: wake-word gating + a naive silence-based utterance boundary.

    TODO: swap the RMS silence heuristic for real VAD (e.g. webrtcvad, same
    library OmniBot's Pixel backend uses) once the wake-word path is verified
    against real hardware.
    """

    def __init__(self):
        self.awake = False
        self.utterance_frames: list[bytes] = []
        self.silence_chunks = 0

    def feed(self, pcm_bytes: bytes) -> bytes | None:
        samples = np.frombuffer(pcm_bytes, dtype=np.int16)

        if not self.awake:
            predictions = wakeword_model.predict(samples)
            if any(score > WAKE_SCORE_THRESHOLD for score in predictions.values()):
                log.info("wake word detected")
                self.awake = True
                self.utterance_frames = []
                self.silence_chunks = 0
            return None

        self.utterance_frames.append(pcm_bytes)
        rms = float(np.sqrt(np.mean(samples.astype(np.float32) ** 2)))
        self.silence_chunks = self.silence_chunks + 1 if rms < SILENCE_RMS_THRESHOLD else 0

        if self.silence_chunks > SILENCE_CHUNKS_TO_END_UTTERANCE:
            utterance = b"".join(self.utterance_frames)
            self.awake = False
            self.utterance_frames = []
            self.silence_chunks = 0
            return utterance

        return None


def pcm_to_wav_bytes(pcm: bytes) -> bytes:
    buf = io.BytesIO()
    with wave.open(buf, "wb") as wf:
        wf.setnchannels(1)
        wf.setsampwidth(2)
        wf.setframerate(SAMPLE_RATE)
        wf.writeframes(pcm)
    return buf.getvalue()


async def run_ai_speech_turn(wav_bytes: bytes) -> str:
    """TODO: send `wav_bytes` to whichever speech/LLM API you pick (e.g.
    Gemini's audio input) and return the reply text. Left as a stub so this
    scaffold doesn't bake in unverified credentials/model choices."""
    log.info("captured utterance of %d bytes (AI call not wired up yet)", len(wav_bytes))
    return "AI response goes here"


@app.websocket("/ws/stream")
async def stream_endpoint(websocket: WebSocket):
    await websocket.accept()
    session = Session()
    log.info("board connected")

    try:
        while True:
            message = await websocket.receive()

            data = message.get("bytes")
            if data is not None:
                packet_type, payload = data[0], data[1:]
                if packet_type == FRAME_TYPE_MIC_PCM:
                    utterance = session.feed(payload)
                    if utterance is not None:
                        reply = await run_ai_speech_turn(pcm_to_wav_bytes(utterance))
                        await websocket.send_text(json.dumps({"status": "success", "reply": reply}))
                else:
                    log.warning("unknown packet type: 0x%02x", packet_type)
                continue

            text = message.get("text")
            if text is not None:
                log.info("text message from board: %s", text)

    except WebSocketDisconnect:
        log.info("board disconnected")
