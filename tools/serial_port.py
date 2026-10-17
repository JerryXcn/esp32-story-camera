"""串口默认值：环境变量 STORY_CAMERA_PORT，否则自动找第一个 USB 串口（macOS / Linux）。"""
import glob
import os


def default_port() -> str:
    env = os.environ.get("STORY_CAMERA_PORT")
    if env:
        return env
    for pattern in ("/dev/cu.usbmodem*", "/dev/ttyACM*", "/dev/ttyUSB*"):
        found = sorted(glob.glob(pattern))
        if found:
            return found[0]
    return "/dev/ttyACM0"
