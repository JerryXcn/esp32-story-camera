#!/usr/bin/env python3
"""渲染 icons.html 中的线性图标为 A8（仅 alpha）PNG：assets/v0.2/icons/<name>_<size>.png。

依赖：Google Chrome（无头截图）与 Pillow。图标在 192px 下渲染再缩小，保证小尺寸边缘平滑。
运行后再执行 convert_assets.py 生成 C 数组。
"""
import re
import subprocess
import sys
import tempfile
from pathlib import Path
from PIL import Image

HERE = Path(__file__).resolve().parent
ROOT = HERE.parents[4]
OUT = ROOT / "assets" / "v0.2" / "icons"
CHROME = "/Applications/Google Chrome.app/Contents/MacOS/Google Chrome"
SIZES = (20, 24, 28, 40)
CELL, COLS = 192, 10

names = re.findall(r'^\s+(\w+):\s*\'', (HERE / "icons.html").read_text(), re.M)
rows = (len(names) + COLS - 1) // COLS
with tempfile.TemporaryDirectory() as td:
    shot = Path(td) / "icons.png"
    subprocess.run([CHROME, "--headless=new", "--disable-gpu", "--hide-scrollbars",
                    "--default-background-color=00000000", f"--window-size={CELL*COLS},{CELL*rows}",
                    f"--screenshot={shot}", (HERE / "icons.html").as_uri()],
                   check=True, capture_output=True)
    sheet = Image.open(shot).convert("RGBA")
OUT.mkdir(parents=True, exist_ok=True)
for i, name in enumerate(names):
    x, y = (i % COLS) * CELL, (i // COLS) * CELL
    alpha = sheet.crop((x, y, x + CELL, y + CELL)).getchannel("A")
    for size in SIZES:
        alpha.resize((size, size), Image.LANCZOS).save(OUT / f"{name}_{size}.png")
print(f"{len(names)} icons x {len(SIZES)} sizes -> {OUT}", file=sys.stderr)
