import asyncio
import audioop  # stdlib; deprecated since 3.11, removed in 3.13 -- swap for
                # e.g. scipy.signal.resample_poly if/when this venv upgrades past 3.12
import json
import logging
import os
import sys
import types as pytypes

import numpy as np
from dotenv import load_dotenv
from fastapi import FastAPI, WebSocket, WebSocketDisconnect
from google import genai
from google.genai import types

# openWakeWord only knows how to import the tflite backend via the
# `tflite_runtime` package, which has no wheel for this platform/Python combo.
# `ai_edge_litert` (Google's actively-maintained successor, much lighter than
# full tensorflow) provides a drop-in-compatible Interpreter, so we shim a
# fake `tflite_runtime.interpreter` module pointing at it before importing
# openwakeword. This matters: the bundled `onnx` models were tested here and
# scored ~0 on both a clean synthesized "Alexa" clip and real speech (feature
# extraction looked healthy, VAD/windowing/warm-up all ruled out) -- the
# tflite models scored a clean 1.0 on the same clip. This looks like a real
# bug in this openwakeword release's onnx models/backend, not our
# integration, so tflite is the one to actually use.
from ai_edge_litert.interpreter import Interpreter as _LiteRTInterpreter

_tflite_runtime = pytypes.ModuleType("tflite_runtime")
_tflite_runtime_interpreter = pytypes.ModuleType("tflite_runtime.interpreter")
_tflite_runtime_interpreter.Interpreter = _LiteRTInterpreter
_tflite_runtime.interpreter = _tflite_runtime_interpreter
sys.modules["tflite_runtime"] = _tflite_runtime
sys.modules["tflite_runtime.interpreter"] = _tflite_runtime_interpreter

from openwakeword.model import Model as WakeWordModel
from openwakeword.utils import download_models as download_wakeword_models

load_dotenv()

logging.basicConfig(level=logging.INFO)
log = logging.getLogger("mr-greeny")

app = FastAPI()

SAMPLE_RATE_IN = 16000  # board mic, matches firmware/main/main.c
FRAME_TYPE_MIC_PCM = 0x10  # board -> backend
FRAME_TYPE_REPLY_PCM = 0x20  # backend -> board, 16kHz PCM after resampling (see below)

# Gemini Live's native output audio is 24kHz, but the board's ES7210 mic and
# ES8311 speaker share one I2S bus in duplex mode, which must run RX/TX at
# the same rate -- and the mic side is fixed at 16kHz (wake word + Gemini
# input both expect it). Resampling here keeps the board's I2S bus at one
# consistent rate instead of needing two separate duplex configurations.
LIVE_OUTPUT_SAMPLE_RATE = 24000
BOARD_OUTPUT_SAMPLE_RATE = 16000

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

download_wakeword_models()  # no-op if already downloaded; model files aren't bundled in the pip package
wakeword_model = WakeWordModel(wakeword_models=WAKEWORD_MODEL_PATHS, inference_framework="tflite")
WAKE_SCORE_THRESHOLD = 0.5

# How long to keep the Live session open with no new turn after the last
# reply finishes, before closing it and re-arming the wake word. Mirrors
# Pixel's short "still listening" follow-up window -- long enough for a
# natural pause in conversation, short enough not to bill/stream forever.
IDLE_TIMEOUT_SECONDS = 10.0


class WakeGate:
    """Runs openWakeWord on incoming PCM until the wake word fires once.

    After that this connection is considered "awake" -- all further mic audio
    streams straight into the Gemini Live session, which does its own
    turn-taking/VAD server-side. Re-armed by LiveSession's idle timeout (see
    `rearm`) once a conversation goes quiet for IDLE_TIMEOUT_SECONDS.
    """

    def __init__(self):
        self.awake = False

    def rearm(self):
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

    def __init__(self, client: genai.Client, send_text, send_binary, on_idle_close):
        self._client = client
        self._send_text = send_text
        self._send_binary = send_binary
        self._on_idle_close = on_idle_close
        self._session_cm = None
        self._session = None
        self._pcm_queue: asyncio.Queue = asyncio.Queue(maxsize=256)
        self._sender_task: asyncio.Task | None = None
        self._receiver_task: asyncio.Task | None = None
        self._idle_timer_task: asyncio.Task | None = None
        self._speaking = False
        self._resample_state = None  # audioop.ratecv continuity across chunks in a turn

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
        self._start_idle_timer()  # safety net: still closes even if Gemini never sends anything at all
        log.info("Gemini Live session opened")

    def feed_pcm(self, chunk: bytes) -> None:
        try:
            self._pcm_queue.put_nowait(chunk)
        except asyncio.QueueFull:
            pass

    def _cancel_idle_timer(self):
        # _idle_timeout() calls close() -> _cancel_idle_timer() on itself when
        # it fires; cancelling the currently-running task here would abort
        # close() partway through at its next await, so skip self-cancellation.
        task = self._idle_timer_task
        self._idle_timer_task = None
        if task is not None and task is not asyncio.current_task():
            task.cancel()

    def _start_idle_timer(self):
        self._cancel_idle_timer()
        self._idle_timer_task = asyncio.create_task(self._idle_timeout())

    async def _idle_timeout(self):
        try:
            await asyncio.sleep(IDLE_TIMEOUT_SECONDS)
        except asyncio.CancelledError:
            return
        log.info("Live session idle for %.0fs, closing and re-arming wake word", IDLE_TIMEOUT_SECONDS)
        await self.close()
        self._on_idle_close()

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
        # Every termination path here (go_away, the generator ending on its
        # own, or an unexpected exception) needs the same cleanup: close the
        # session and re-arm the wake gate. Previously none of these paths did
        # that -- session state (self._session etc.) was left stale, so a
        # later ensure_started() silently no-op'd forever, thinking a session
        # was still open. That's the likely explanation for "works once, then
        # the second turn just doesn't, then it goes idle" -- whatever ended
        # the first turn's session (very possibly a GoAway, which Gemini Live
        # sends shortly before force-closing a session -- these preview models
        # have short max session durations) went completely unnoticed.
        try:
            async for msg in self._session.receive():
                # go_away is a sibling of server_content, not nested inside it.
                if msg.go_away is not None:
                    log.warning("Gemini Live GoAway received (time_left=%s) -- closing session",
                                getattr(msg.go_away, "time_left", None))
                    break

                sc = msg.server_content
                if sc is None:
                    # Gemini sends other top-level signal types with no
                    # server_content at all (voice_activity, usage_metadata,
                    # etc.) -- these can arrive continuously/periodically
                    # even during silence, e.g. as VAD housekeeping. An
                    # earlier version reset the idle countdown on ANY
                    # message including these, which meant the session
                    # basically never actually went idle. Only genuine
                    # server_content below counts as "activity".
                    log.info("Live message with no server_content (set fields: %s) -- not activity",
                             msg.model_fields_set)
                    continue

                # Real conversational content -- safe to treat as activity
                # and reset the idle countdown. This also covers the case
                # where turn_complete itself is delayed/never fires cleanly
                # (per the Live API docs, it can be held back pending an
                # assumed "realtime playback finished" signal we never send):
                # the countdown still starts from whenever the last genuine
                # content message arrived, not from a specific flag.
                self._start_idle_timer()

                if sc.model_turn and not self._speaking:
                    self._speaking = True
                    await self._send_text(json.dumps({"type": "assistant_speaking", "state": "start"}))

                if sc.output_transcription and sc.output_transcription.text:
                    await self._send_text(json.dumps({
                        "status": "success",
                        "reply": sc.output_transcription.text,
                    }))

                if sc.model_turn:
                    for part in sc.model_turn.parts:
                        inline = getattr(part, "inline_data", None)
                        if inline and inline.data:
                            resampled, self._resample_state = audioop.ratecv(
                                inline.data, 2, 1,
                                LIVE_OUTPUT_SAMPLE_RATE, BOARD_OUTPUT_SAMPLE_RATE,
                                self._resample_state,
                            )
                            await self._send_binary(bytes([FRAME_TYPE_REPLY_PCM]) + resampled)

                if (sc.turn_complete or sc.interrupted) and self._speaking:
                    self._speaking = False
                    await self._send_text(json.dumps({"type": "assistant_speaking", "state": "stop"}))
        except asyncio.CancelledError:
            # An external close() (shutdown, idle timeout) is already tearing
            # this down -- don't re-run cleanup on top of that.
            return
        except Exception:
            log.exception("Live receive loop ended")

        # Reached for every other termination path (go_away, or the receive()
        # generator just ending on its own) -- always clean up and re-arm.
        log.info("Live session receive loop ended -- closing and re-arming wake word")
        await self.close()
        self._on_idle_close()

    async def close(self):
        self._cancel_idle_timer()
        for t in (self._sender_task, self._receiver_task):
            if t is not None and t is not asyncio.current_task():
                t.cancel()
        self._sender_task = None
        self._receiver_task = None
        if self._session_cm is not None:
            try:
                await self._session_cm.__aexit__(None, None, None)
            except Exception:
                log.debug("Live session __aexit__ failed", exc_info=True)
        # Reset so a later ensure_started() (re-armed wake word -> spoken
        # again) opens a fresh session instead of thinking one is still open.
        self._session_cm = None
        self._session = None
        self._speaking = False
        self._resample_state = None


@app.websocket("/ws/stream")
async def stream_endpoint(websocket: WebSocket):
    await websocket.accept()
    log.info("board connected")

    gate = WakeGate()
    live = (
        LiveSession(genai_client, websocket.send_text, websocket.send_bytes, gate.rearm)
        if genai_client
        else None
    )

    try:
        while True:
            message = await websocket.receive()

            # The raw receive() API returns this as a normal message rather
            # than raising WebSocketDisconnect (that's only done by the
            # receive_bytes()/receive_text()/receive_json() wrappers) --
            # calling receive() again after this arrives raises a RuntimeError
            # and was silently killing every connection before this check
            # existed.
            if message["type"] == "websocket.disconnect":
                break

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
