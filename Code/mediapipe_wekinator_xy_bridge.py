"""
mediapipe_wekinator_xy_bridge.py

Webcam -> MediaPipe (index fingertip) -> OSC
  /wek/inputs x y   -> Wekinator   (127.0.0.1:6448)
  /hand x y         -> Processing  (127.0.0.1:9000)

Run this in PowerShell (NOT in Processing):
    python .\\mediapipe_wekinator_xy_bridge.py
Press 'q' in the preview window to quit.
"""

import os
import time
import urllib.request

import cv2
import numpy as np
import mediapipe as mp
from pythonosc.udp_client import SimpleUDPClient

# ---------------- Settings you may want to change ----------------
CAMERA_INDEX = 0            # try 1 if the webcam is not found
WEKINATOR_HOST = "127.0.0.1"
WEKINATOR_PORT = 6448       # Wekinator INPUT port
PROCESSING_HOST = "127.0.0.1"
PROCESSING_PORT = 9000      # Processing listens here
PRINT_EVERY_N_FRAMES = 15   # how often to print x/y while a hand is visible
DOT_RADIUS = 40             # size of the red dot in the preview
# ------------------------------------------------------------------

MODEL_URL = (
    "https://storage.googleapis.com/mediapipe-models/hand_landmarker/"
    "hand_landmarker/float16/latest/hand_landmarker.task"
)
MODEL_PATH = "hand_landmarker.task"
INDEX_FINGERTIP = 8         # MediaPipe landmark number for the index fingertip
WIDTH, HEIGHT = 640, 480

# --- Download the hand model on first run -------------------------
if not os.path.exists(MODEL_PATH):
    print("[SETUP] Downloading MediaPipe hand model (first run only)...")
    urllib.request.urlretrieve(MODEL_URL, MODEL_PATH)
    print("[SETUP] Model downloaded.")

# --- OSC clients ---------------------------------------------------
wekinator = SimpleUDPClient(WEKINATOR_HOST, WEKINATOR_PORT)
processing = SimpleUDPClient(PROCESSING_HOST, PROCESSING_PORT)
print(f"[OSC] Sending /wek/inputs x y  -> Wekinator  {WEKINATOR_HOST}:{WEKINATOR_PORT}")
print(f"[OSC] Sending /hand x y        -> Processing {PROCESSING_HOST}:{PROCESSING_PORT}")

# --- MediaPipe setup -----------------------------------------------
BaseOptions = mp.tasks.BaseOptions
HandLandmarker = mp.tasks.vision.HandLandmarker
HandLandmarkerOptions = mp.tasks.vision.HandLandmarkerOptions
RunningMode = mp.tasks.vision.RunningMode

options = HandLandmarkerOptions(
    base_options=BaseOptions(model_asset_path=MODEL_PATH),
    running_mode=RunningMode.VIDEO,
    num_hands=1,
    min_hand_detection_confidence=0.5,
    min_hand_presence_confidence=0.5,
    min_tracking_confidence=0.5,
)


def open_camera(index):
    """Try several Windows camera backends until one works."""
    for name, backend in (("MSMF", cv2.CAP_MSMF),
                          ("DSHOW", cv2.CAP_DSHOW),
                          ("ANY", cv2.CAP_ANY)):
        cam = cv2.VideoCapture(index, backend)
        if cam.isOpened():
            cam.set(cv2.CAP_PROP_FRAME_WIDTH, WIDTH)
            cam.set(cv2.CAP_PROP_FRAME_HEIGHT, HEIGHT)
            ok, frame = cam.read()
            if ok and frame is not None and frame.size:
                print(f"[CAMERA] Webcam {index} opened OK using backend {name}.")
                return cam
        cam.release()
    raise RuntimeError(
        f"[CAMERA] Could not open webcam {index}. "
        "Close Zoom/Teams/browser tabs using the camera, or set CAMERA_INDEX = 1."
    )


camera = open_camera(CAMERA_INDEX)

WINDOW_NAME = "MediaPipe - red dot only (press q to quit)"
cv2.namedWindow(WINDOW_NAME, cv2.WINDOW_NORMAL)
cv2.resizeWindow(WINDOW_NAME, WIDTH, HEIGHT)

start_time = time.monotonic()
last_timestamp = -1
frame_count = 0
hand_was_visible = False

print("[RUN] Running. Show one hand to the camera. Press 'q' in the preview window to quit.")

with HandLandmarker.create_from_options(options) as landmarker:
    while True:
        ok, frame = camera.read()
        if not ok:
            print("[CAMERA] Lost the webcam frame. Stopping.")
            break

        # Mirror the image so moving your hand right moves the dot right.
        # The webcam image is used ONLY for detection; it is never shown or sent.
        frame = cv2.flip(frame, 1)
        rgb = cv2.cvtColor(frame, cv2.COLOR_BGR2RGB)
        mp_image = mp.Image(image_format=mp.ImageFormat.SRGB, data=rgb)

        # VIDEO mode needs strictly increasing timestamps
        timestamp_ms = int((time.monotonic() - start_time) * 1000)
        if timestamp_ms <= last_timestamp:
            timestamp_ms = last_timestamp + 1
        last_timestamp = timestamp_ms

        result = landmarker.detect_for_video(mp_image, timestamp_ms)

        # Plain white preview
        display = np.full((HEIGHT, WIDTH, 3), 255, dtype=np.uint8)

        if result.hand_landmarks:
            if not hand_was_visible:
                print("[MEDIAPIPE] Hand DETECTED.")
                hand_was_visible = True

            tip = result.hand_landmarks[0][INDEX_FINGERTIP]
            # Normalized 0..1, clamped in case the tip is slightly off-screen
            x = float(min(max(tip.x, 0.0), 1.0))
            y = float(min(max(tip.y, 0.0), 1.0))

            # Exactly two inputs to Wekinator
            wekinator.send_message("/wek/inputs", [x, y])
            # Same point to Processing
            processing.send_message("/hand", [x, y])

            frame_count += 1
            if frame_count % PRINT_EVERY_N_FRAMES == 0:
                print(f"[SEND] /wek/inputs -> Wekinator:{WEKINATOR_PORT}  "
                      f"x={x:.3f} y={y:.3f}   |   "
                      f"/hand -> Processing:{PROCESSING_PORT}")

            cv2.circle(display, (int(x * WIDTH), int(y * HEIGHT)),
                       DOT_RADIUS, (0, 0, 255), -1)   # BGR: red
        else:
            if hand_was_visible:
                print("[MEDIAPIPE] Hand LOST (nothing is being sent).")
                hand_was_visible = False

        cv2.imshow(WINDOW_NAME, display)
        if cv2.waitKey(1) & 0xFF == ord("q"):
            break

camera.release()
cv2.destroyAllWindows()
print("[RUN] Stopped.")
