#!/usr/bin/env python3
"""Persistent UART session; JSON lines on stdin, screenshots and logs go where you point them.

Unlike the one-shot tools, opening the port once preserves UI and audio state.
Examples: {"cmd":"info","until":"INFO "}, {"screenshot":"camera.png"},
{"wait":5}, {"cmd":"tap 40 280","until":"TAP_OK","after":1}.
"""
import argparse
import base64
import json
import queue
import threading
import time
import sys
from pathlib import Path

import serial
from serial_port import default_port
from PIL import Image


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--port", default=default_port())
    ap.add_argument("--output", required=True)
    args = ap.parse_args()
    project = Path(__file__).resolve().parents[1]
    output = Path(args.output).resolve()
    output.relative_to(project)
    output.mkdir(parents=True, exist_ok=True)
    ser = serial.Serial(port=None, baudrate=115200, timeout=0.1)
    ser.port = args.port
    ser.dtr = False
    ser.rts = False
    ser.open()
    incoming = queue.Queue()
    closing = threading.Event()
    started = time.monotonic()
    raw = (output / "uart.log").open("ab", buffering=0)
    events = (output / "commands.jsonl").open("a", buffering=1)

    def reader():
        pending = bytearray()
        while not closing.is_set():
            block = ser.read(ser.in_waiting or 1)
            if not block:
                continue
            raw.write(block)
            pending.extend(block)
            while b"\n" in pending:
                line, _, pending = pending.partition(b"\n")
                incoming.put(line.decode("utf-8", "replace").strip())

    thread = threading.Thread(target=reader, daemon=True)
    thread.start()
    terminal_settings = None
    if sys.stdin.isatty():
        import termios
        terminal_settings = termios.tcgetattr(sys.stdin.fileno())
        quiet = terminal_settings.copy()
        quiet[3] &= ~termios.ECHO
        termios.tcsetattr(sys.stdin.fileno(), termios.TCSANOW, quiet)

    def collect(seconds):
        lines = []
        deadline = time.monotonic() + seconds
        while time.monotonic() < deadline:
            try:
                lines.append(incoming.get(timeout=min(0.2, deadline-time.monotonic())))
            except queue.Empty:
                pass
        return lines

    def run(req):
        lines = collect(0.05)
        if "wait" in req:
            lines += collect(float(req["wait"]))
            return {"lines":lines[-150:]}
        picture = req.get("screenshot") or req.get("camera_dump")
        file_dump = req.get("file_dump")
        cmd = "screenshot" if "screenshot" in req else "camera dump" if "camera_dump" in req else req["cmd"]
        ser.write(("\n" + cmd + "\n").encode())
        ser.flush()
        deadline = time.monotonic() + float(req.get("timeout", 110 if picture else 8))
        prefix = "FILE" if file_dump else "SCREEN" if "screenshot" in req else "CAM"
        data_prefix = "FX " if file_dump else "PX " if prefix == "SCREEN" else "CX "
        header = None
        data = bytearray()
        complete = False
        while time.monotonic() < deadline:
            try:
                line = incoming.get(timeout=0.2)
            except queue.Empty:
                continue
            if (picture or file_dump) and line.startswith(data_prefix):
                data.extend(base64.b64decode(line[len(data_prefix):], validate=True))
                continue
            lines.append(line)
            if picture or file_dump:
                if line.startswith(prefix + "_BEGIN "):
                    header = int(line.split()[2]) if file_dump else list(map(int, line.split()[1:4]))
                elif line == prefix + "_END":
                    complete = True
                    break
                elif line.startswith(prefix + "_ERROR"):
                    break
            elif line.startswith(req.get("until", "INFO ")):
                complete = True
                break
        lines += collect(float(req.get("after", 0.15)))
        result = {"complete":complete,"lines":lines[-150:]}
        if file_dump:
            if not complete or header is None or len(data) != header:
                raise RuntimeError("Incomplete file response")
            target = (output / file_dump).resolve()
            target.relative_to(output)
            target.write_bytes(data)
            result.update(path=str(target),bytes=len(data))
        if picture:
            if not complete or header is None:
                raise RuntimeError("Incomplete image response")
            w,h,n = header
            stride = n if prefix == "SCREEN" else w*2
            if len(data) != stride*h:
                raise RuntimeError(f"Image length {len(data)} != {stride*h}")
            target = (output / picture).resolve()
            target.relative_to(output)
            rgb = bytearray()
            for y in range(h):
                row = data[y*stride:y*stride+w*2]
                for i in range(0,len(row),2):
                    v = row[i] | (row[i+1]<<8)
                    r,g,b = (v>>11)&31,(v>>5)&63,v&31
                    rgb.extend(((r<<3)|(r>>2),(g<<2)|(g>>4),(b<<3)|(b>>2)))
            Image.frombytes("RGB",(w,h),bytes(rgb)).save(target)
            result.update(path=str(target),width=w,height=h,bytes=len(data))
        return result

    print(json.dumps({"ready":True,"port":args.port,"output":str(output)}),flush=True)
    try:
        for line in sys.stdin:
            try:
                req = json.loads(line)
                if req.get("close"):
                    break
                result = run(req)
            except Exception as exc:
                result = {"error":str(exc)}
            record = {"elapsed_s":round(time.monotonic()-started,3),"request":line.strip(),"result":result}
            events.write(json.dumps(record,ensure_ascii=False)+"\n")
            print(json.dumps(result,ensure_ascii=False),flush=True)
    finally:
        if terminal_settings is not None:
            termios.tcsetattr(sys.stdin.fileno(), termios.TCSANOW, terminal_settings)
        closing.set()
        thread.join(timeout=1)
        ser.close()
        raw.close()
        events.close()


if __name__ == "__main__":
    main()
