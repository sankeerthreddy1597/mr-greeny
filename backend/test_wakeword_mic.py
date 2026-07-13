"""Standalone openWakeWord test -- completely independent of the board,
WiFi, and the /ws/stream backend. Uses this machine's own mic so you can
verify the wake-word library itself actually works before debugging
anything about the ESP32 pipeline.

Setup (one-time):
    brew install portaudio
    pip install sounddevice

Run:
    python test_wakeword_mic.py

Say one of: "alexa", "hey jarvis", "hey mycroft", "hey rhasspy", "weather".
Prints a heartbeat (best score + audio RMS) roughly twice a second even
below the detection threshold, so you can tell "audio isn't reaching the
model at all" apart from "audio arrives but scores stay low" -- those are
different problems. A real detection (score > 0.5, same threshold app.py
uses) prints on its own line. Ctrl+C to stop.
"""

import time

import numpy as np
import sounddevice as sd
from openwakeword.model import Model
from openwakeword.utils import download_models

RATE = 16000
CHUNK = 1280  # ~80ms, openWakeWord's typical frame size
DETECT_THRESHOLD = 0.5
HEARTBEAT_SECONDS = 0.5

download_models()  # no-op if already cached
model = Model(wakeword_models=[], inference_framework="onnx")  # bundled pretrained models

print("Listening on the default input device. Say: alexa / hey jarvis / hey mycroft / hey rhasspy / weather")
print("Printing a heartbeat (best score + audio rms) so you can see it's alive even below threshold.")
print("Ctrl+C to stop.\n")

_last_print = 0.0


def callback(indata, frames, time_info, status):
    global _last_print
    if status:
        print("stream status:", status)

    audio = indata[:, 0]
    predictions = model.predict(audio)
    best_name = max(predictions, key=predictions.get)
    best_score = predictions[best_name]
    rms = int(np.sqrt(np.mean(audio.astype(np.float32) ** 2)))

    now = time.monotonic()
    if best_score > DETECT_THRESHOLD:
        print(f"DETECTED {best_name}={best_score:.3f} (audio rms={rms})")
        _last_print = now
    elif now - _last_print > HEARTBEAT_SECONDS:
        print(f"...listening: best={best_name}={best_score:.3f}  audio rms={rms}")
        _last_print = now


with sd.InputStream(channels=1, samplerate=RATE, blocksize=CHUNK, dtype="int16", callback=callback):
    while True:
        sd.sleep(1000)
