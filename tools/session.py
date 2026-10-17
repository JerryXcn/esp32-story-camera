#!/usr/bin/env python3
"""设备验证会话：打开串口（会触发复位）→ 等待启动完成 → 执行命令序列。"""
import serial
import sys
import time

from serial_port import default_port

PORT = default_port()


class Dev:
    def __init__(self, port=PORT):
        self.ser = serial.Serial(port, 115200, timeout=0.5)
        self.ser.setDTR(False)
        self.ser.setRTS(False)
        self.log = open("device-session.log", "ab")
        # 等待启动完成（打开串口会复位芯片）：等到网络校时完成后控制台
        # 一定已开始读取；超时则退化为固定等待。
        deadline = time.time() + 40
        booted = False
        while time.time() < deadline:
            line = self.ser.readline().decode("utf-8", "replace").strip()
            if line:
                self.log.write((line + "\n").encode())
            if "time synced" in line:
                booted = True
                break
        if not booted:
            print("! boot wait timeout（再等 10s 兜底）")
            time.sleep(10)
        time.sleep(1.0)
        self.ser.reset_input_buffer()
        # 预热：确认控制台就绪（截图路径依赖 LVGL 锁正常释放）
        if self.send("info", ["INFO"], timeout=8, quiet=True) is None:
            print("! console warmup 失败")

    def send(self, cmd, markers, timeout=25, quiet=False):
        for attempt in range(3):
            self.ser.reset_input_buffer()
            self.ser.write(("\n" + cmd + "\n").encode())   # 前导换行冲掉残留行
            deadline = time.time() + timeout
            saw_unknown = False
            while time.time() < deadline:
                line = self.ser.readline().decode("utf-8", "replace").strip()
                if not line:
                    continue
                self.log.write((line + "\n").encode())
                if not quiet:
                    print("  ", line)
                if line == "UNKNOWN_COMMAND":
                    saw_unknown = True
                    break
                for m in markers:
                    if line.startswith(m):
                        return line
            if not saw_unknown:
                break
        return None

    def screenshot(self, out):
        for attempt in range(3):
            if self._screenshot_once(out):
                return
            print(f"  ! screenshot 重试 {attempt + 1}")
        raise SystemExit("screenshot failed after retries")

    def _screenshot_once(self, out):
        self.ser.reset_input_buffer()
        self.ser.write(b"\nscreenshot\n")
        width = height = stride = None
        data = bytearray()
        deadline = time.time() + 60
        while time.time() < deadline:
            line = self.ser.readline().decode("utf-8", "replace").strip()
            if not line:
                continue
            if line.startswith("SCREEN_BEGIN"):
                _, w, h, st = line.split()[:4]
                width, height, stride = int(w), int(h), int(st)
            elif line.startswith("PX "):
                import base64
                data.extend(base64.b64decode(line[3:]))
            elif line == "SCREEN_END":
                break
            elif line.startswith("SCREEN_ERROR"):
                print("  device:", line)
                return False
        else:
            return False
        expected = stride * height
        assert len(data) >= expected, f"short {len(data)}<{expected}"
        data = data[:expected]
        self._save_png(data, width, height, stride, out)
        return True

    @staticmethod
    def _save_png(data, width, height, stride, out):
        from PIL import Image
        pixels = bytearray()
        for y in range(height):
            row = data[y * stride: y * stride + width * 2]
            for i in range(0, len(row), 2):
                v = row[i] | (row[i + 1] << 8)
                pixels += bytes((((v >> 11) & 0x1F) << 3,
                                 ((v >> 5) & 0x3F) << 2,
                                 (v & 0x1F) << 3))
        Image.frombytes("RGB", (width, height), bytes(pixels)).save(out)
        print(f"saved {out} {width}x{height}")


if __name__ == "__main__":
    d = Dev()
    for cmd in sys.argv[1:]:
        print(f"$ {cmd}")
        d.send(cmd, ["OK", "CAMERA_FRAME", "CAMERA_GRAB", "CAMERA_JPEG",
                     "TAP_OK", "GOTO", "UI_RESET", "CTRL_DONE", "ENUM_DONE",
                     "ISP_DONE", "TOUCHSTATE", "INFO", "REG ", "WREG"],
               timeout=30)
