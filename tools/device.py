#!/usr/bin/env python3
"""esp32-story-camera UART-C 调试工具（与 firmware/components/debug_console 协议一致）。

命令：
  info                 设备一行状态
  screenshot out.png   LVGL 快照 → PNG
  tap X Y              模拟点击
  send CMD             发送任意控制台命令并回显响应（如 camera grab / goto 3）
  monitor [SECONDS]    透传串口日志
"""

import argparse
import base64
import binascii
import sys
import time

import serial

from serial_port import default_port

PORT_DEFAULT = default_port()
BAUD = 115200


def open_serial(port, baud=BAUD):
    return serial.Serial(port, baud, timeout=0.2)


def send_line(ser, line):
    ser.write((line + "\n").encode())
    ser.flush()


def read_lines(ser, seconds):
    deadline = time.time() + seconds
    while time.time() < deadline:
        raw = ser.readline()
        if raw:
            yield raw.decode("utf-8", "replace").rstrip("\r\n")
        deadline = time.time() + seconds if False else deadline


def cmd_info(ser):
    send_line(ser, "info")
    deadline = time.time() + 5
    while time.time() < deadline:
        line = ser.readline().decode("utf-8", "replace").strip()
        if line.startswith("INFO "):
            print(line)
            return
        if line:
            print("#", line)
    raise SystemExit("timeout waiting INFO")


def cmd_screenshot(ser, out_png):
    send_line(ser, "screenshot")
    width = height = stride = None
    data = bytearray()
    deadline = time.time() + 60
    while time.time() < deadline:
        line = ser.readline().decode("utf-8", "replace").strip()
        if not line:
            continue
        if line.startswith("SCREEN_BEGIN"):
            parts = line.split()
            _, w, h, st = parts[:4]
            width, height, stride = int(w), int(h), int(st)
        elif line.startswith("PX "):
            data.extend(base64.b64decode(line[3:], validate=False))
        elif line == "SCREEN_END":
            break
        elif line.startswith("SCREEN_ERROR"):
            raise SystemExit("device: " + line)
    else:
        raise SystemExit("timeout waiting SCREEN_END")

    expected = stride * height
    if len(data) < expected:
        raise SystemExit(f"short frame: {len(data)} < {expected}")
    data = data[:expected]

    from PIL import Image

    pixels = bytearray()
    for y in range(height):
        row = data[y * stride : y * stride + width * 2]
        for i in range(0, len(row), 2):
            v = row[i] | (row[i + 1] << 8)
            r = ((v >> 11) & 0x1F) << 3
            g = ((v >> 5) & 0x3F) << 2
            b = (v & 0x1F) << 3
            pixels += bytes((r, g, b))
    img = Image.frombytes("RGB", (width, height), bytes(pixels))
    img.save(out_png)
    print(f"saved {out_png} ({width}x{height})")


def cmd_camera_dump(ser, out_raw):
    send_line(ser, "camera dump")
    width = height = stride = None
    data = bytearray()
    deadline = time.time() + 60
    while time.time() < deadline:
        line = ser.readline().decode("utf-8", "replace").strip()
        if not line:
            continue
        if line.startswith("CAM_BEGIN"):
            _, w, h, st = line.split()[:4]
            width, height, stride = int(w), int(h), int(st)
        elif line.startswith("CX "):
            data.extend(base64.b64decode(line[3:], validate=False))
        elif line == "CAM_END":
            break
        elif line.startswith("CAM_ERROR"):
            raise SystemExit("device: " + line)
    else:
        raise SystemExit("timeout waiting CAM_END")
    with open(out_raw, "wb") as fh:
        fh.write(data)
    print(f"saved {out_raw} ({width}x{height} rgb565, {len(data)} bytes)")


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("-p", "--port", default=PORT_DEFAULT)
    sub = ap.add_subparsers(dest="cmd", required=True)
    sub.add_parser("info")
    sp = sub.add_parser("screenshot")
    sp.add_argument("out")
    sp = sub.add_parser("tap")
    sp.add_argument("x", type=int)
    sp.add_argument("y", type=int)
    sp = sub.add_parser("send")
    sp.add_argument("command")
    sp = sub.add_parser("monitor")
    sp.add_argument("seconds", nargs="?", type=float, default=15)
    sp = sub.add_parser("camdump")
    sp.add_argument("out")
    args = ap.parse_args()

    ser = open_serial(args.port)
    time.sleep(0.3)
    ser.reset_input_buffer()

    if args.cmd == "info":
        cmd_info(ser)
    elif args.cmd == "screenshot":
        cmd_screenshot(ser, args.out)
    elif args.cmd == "tap":
        send_line(ser, f"tap {args.x} {args.y}")
        deadline = time.time() + 5
        while time.time() < deadline:
            line = ser.readline().decode("utf-8", "replace").strip()
            if line == "TAP_OK":
                print("TAP_OK")
                return
            if line:
                print("#", line)
        raise SystemExit("timeout waiting TAP_OK")
    elif args.cmd == "send":
        send_line(ser, args.command)
        deadline = time.time() + 15
        while time.time() < deadline:
            line = ser.readline().decode("utf-8", "replace").rstrip()
            if line:
                print(line)
                if line in ("UNKNOWN_COMMAND", "COMMAND_TOO_LONG"):
                    break
        # 透传模式下多行响应由超时截断
    elif args.cmd == "monitor":
        for line in read_lines(ser, args.seconds):
            print(line)
    elif args.cmd == "camdump":
        cmd_camera_dump(ser, args.out)


if __name__ == "__main__":
    main()
