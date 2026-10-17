#!/usr/bin/env python3
"""常驻 UART 守护：串口只打开一次（避免每条命令都复位设备）。

所有串口输出追加到 LOG；向 FIFO 写入的每一行原样发送给设备控制台。
  python3 tools/uartd.py --log /path/uart.log --fifo /path/uart.in &
  echo "info" > /path/uart.in ; tail /path/uart.log
特殊行：`!reset` 拉一次 RTS 复位芯片；`!quit` 退出守护。
"""
import argparse
import os
import threading
import time

import serial
from serial_port import default_port


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--port", default=default_port())
    ap.add_argument("--log", required=True)
    ap.add_argument("--fifo", required=True)
    args = ap.parse_args()

    if not os.path.exists(args.fifo):
        os.mkfifo(args.fifo)
    ser = serial.Serial(port=None, baudrate=115200, timeout=0.1)
    ser.port = args.port
    ser.dtr = False
    ser.rts = False
    ser.open()
    log = open(args.log, "ab", buffering=0)
    stop = threading.Event()

    def reader():
        while not stop.is_set():
            try:
                block = ser.read(ser.in_waiting or 1)
            except serial.SerialException as e:
                log.write(f"\n[uartd] serial error: {e}\n".encode())
                time.sleep(1)
                continue
            if block:
                log.write(block)

    threading.Thread(target=reader, daemon=True).start()
    while not stop.is_set():
        with open(args.fifo, "r") as fifo:      # 阻塞直到有写入者
            for line in fifo:
                line = line.rstrip("\n")
                if line == "!quit":
                    stop.set()
                    break
                if line == "!reset":
                    ser.rts = True
                    time.sleep(0.1)
                    ser.rts = False
                    continue
                log.write(f"\n[uartd] >>> {line}\n".encode())
                ser.write(("\n" + line + "\n").encode())
                ser.flush()
    ser.close()


if __name__ == "__main__":
    main()
