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
Scores print continuously; anything above ~0.5 is a detection (same
threshold app.py uses). Ctrl+C to stop.
"""

import numpy as np
import sounddevice as sd
from openwakeword.model import Model
from openwakeword.utils import download_models

RATE = 16000
CHUNK = 1280  # ~80ms, openWakeWord's typical frame size
PRINT_THRESHOLD = 0.05  # show near-misses too, not just full detections

download_models()  # no-op if already cached
model = Model(wakeword_models=[], inference_framework="onnx")  # bundled pretrained models

print("Listening on the default input device. Say: alexa / hey jarvis / hey mycroft / hey rhasspy / weather")
print("Ctrl+C to stop.\n")


def callback(indata, frames, time_info, status):
    if status:
        print("stream status:", status)
    audio = indata[:, 0]
    predictions = model.predict(audio)
    hits = {name: round(score, 3) for name, score in predictions.items() if score > PRINT_THRESHOLD}
    if hits:
        marker = " <-- DETECTED" if any(s > 0.5 for s in hits.values()) else ""
        print(hits, marker)


with sd.InputStream(channels=1, samplerate=RATE, blocksize=CHUNK, dtype="int16", callback=callback):
    while True:
        sd.sleep(1000)
