#!/usr/bin/env python3
"""PNG -> LVGL v9 C 数组转换（实施计划 4.2）。

在锁定 LVGL 版本（当前面向 v9 的 lv_image_dsc_t / LV_COLOR_FORMAT_*）后运行：

    python3 firmware/components/app_ui/tools/convert_assets.py

- 输入：仓库根 assets/ 的 P0 白名单 PNG（不包含 reference/、review/、demo/）
- 输出：firmware/components/app_ui/generated/{assets.h,assets.c,ASSETS.md}
- 不透明图 -> RGB565（字节序与 LVGL v9 默认一致，实机色序验收在 M0）
- 透明图   -> ARGB8888（v0.1 素材）；v0.2 素材显式指定格式：
  RGB565A8（透明插画，比 ARGB8888 省 25%）、A8（单色线性图标，运行时用 image_recolor 着色）
- assets/v0.2/icons/*.png 由 tools/icons/render_icons.py 生成，自动全部收录为 A8
- ASSETS.md 记录每个文件的输入 SHA-256、尺寸、色彩格式与解码内存估算；
  输入文件变更后必须重新运行（构建不做自动依赖追踪）。
"""
from __future__ import annotations

import hashlib
import struct
import sys
from pathlib import Path

try:
    from PIL import Image
except ImportError:
    sys.exit("需要 Pillow：pip install Pillow")

ROOT = Path(__file__).resolve().parents[4]          # 仓库根目录
ASSETS = ROOT / "assets"
OUT = ROOT / "firmware" / "components" / "app_ui" / "generated"

WHITELIST = {
    "mascot/fox_reading_scene.png": "img_fox_reading_scene",
    "mascot/fox_empty_library.png": "img_fox_empty_library",
    "mascot/fox_wifi.png": "img_fox_wifi",
    "mascot/fox_error.png": "img_fox_error",
    "background/boot_fox_scene.png": "img_boot_fox_scene",
    "decoration/star_large.png": "img_star_large",
    "decoration/star_small.png": "img_star_small",
    "icon/back.png": "img_icon_back",
    "icon/library.png": "img_icon_library",
    "icon/settings.png": "img_icon_settings",
    "icon/regenerate.png": "img_icon_regenerate",
    "icon/delete.png": "img_icon_delete",
    "icon/step_done.png": "img_icon_step_done",
    "icon/step_pending.png": "img_icon_step_pending",
    "icon/volume.png": "img_icon_volume",
    "icon/wifi.png": "img_icon_wifi",
    "icon/age.png": "img_icon_age",
    "icon/duration.png": "img_icon_duration",
    "icon/voice.png": "img_icon_voice",
    "icon/storage.png": "img_icon_storage",
    # v0.2（Codex 生成插画，源图在 assets/v0.2/src/）
    "v0.2/home_bg.png": ("img_home_bg", "RGB565"),
    "v0.2/tile_camera.png": ("img_tile_camera", "RGB565A8"),
    "v0.2/tile_library.png": ("img_tile_library", "RGB565A8"),
    "v0.2/tile_favorite.png": ("img_tile_favorite", "RGB565A8"),
    "v0.2/tile_settings.png": ("img_tile_settings", "RGB565A8"),
    "v0.2/fox_clap.png": ("img_fox_clap", "RGB565A8"),
    "v0.2/fox_reading.png": ("img_fox_reading_t", "RGB565A8"),
}
for _icon in sorted((ASSETS / "v0.2" / "icons").glob("*.png")):
    WHITELIST[f"v0.2/icons/{_icon.name}"] = (f"img_ic_{_icon.stem}", "A8")


def to_rgb565(img: Image.Image) -> bytes:
    px = img.convert("RGB")
    out = bytearray()
    for r, g, b in px.getdata():
        v = ((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3)
        out += struct.pack("<H", v)
    return bytes(out)


def to_argb8888(img: Image.Image) -> bytes:
    px = img.convert("RGBA")
    out = bytearray()
    for r, g, b, a in px.getdata():
        out += struct.pack("<I", (a << 24) | (r << 16) | (g << 8) | b)
    return bytes(out)


def to_rgb565a8(img: Image.Image) -> bytes:
    px = img.convert("RGBA")
    return to_rgb565(px) + bytes(a for *_, a in px.getdata())


def to_a8(img: Image.Image) -> bytes:
    px = img.convert("RGBA") if img.mode in ("RGBA", "LA", "P") else img.convert("L")
    return bytes(px.getchannel("A").getdata()) if px.mode == "RGBA" else bytes(px.getdata())


def c_name(var: str) -> str:
    return var


def main() -> None:
    OUT.mkdir(parents=True, exist_ok=True)
    decls, defs, report = [], [], []
    for rel, spec in WHITELIST.items():
        var, fmt = spec if isinstance(spec, tuple) else (spec, None)
        path = ASSETS / rel
        if not path.exists():
            print(f"跳过缺失素材: {rel}")
            continue
        img = Image.open(path)
        has_alpha = img.mode in ("RGBA", "LA") or (img.mode == "P" and "transparency" in img.info)
        if fmt is None:
            fmt = "ARGB8888" if has_alpha else "RGB565"
        conv = {"ARGB8888": (to_argb8888, 4, 4), "RGB565": (to_rgb565, 2, 2),
                "RGB565A8": (to_rgb565a8, 3, 2), "A8": (to_a8, 1, 1)}[fmt]
        data, cf = conv[0](img), f"LV_COLOR_FORMAT_{fmt}"
        mem = img.width * img.height * conv[1]
        stride = img.width * conv[2]
        sha = hashlib.sha256(path.read_bytes()).hexdigest()
        arr = ", ".join(f"0x{b:02x}" for b in data)
        defs.append(
            f"static const uint8_t {var}_data[] = {{\n    {arr}\n}};\n"
            f"const lv_image_dsc_t {var} = {{\n"
            f"    .header = {{ .magic = LV_IMAGE_HEADER_MAGIC, .cf = {cf}, .w = {img.width}, .h = {img.height}, .stride = {stride} }},\n"
            f"    .data_size = sizeof({var}_data),\n"
            f"    .data = {var}_data,\n}};\n"
        )
        decls.append(f"extern const lv_image_dsc_t {var};")
        report.append(f"| {rel} | {img.width}x{img.height} | {cf} | {sha[:16]}… | {mem/1024:.1f} KiB |")

    (OUT / "assets.h").write_text(
        "#pragma once\n// 由 convert_assets.py 生成，勿手改\n#include \"lvgl.h\"\n\n"
        + "\n".join(decls) + "\n", encoding="utf-8")
    (OUT / "assets.c").write_text(
        "#include \"assets.h\"\n// 由 convert_assets.py 生成，勿手改\n\n"
        + "\n".join(defs), encoding="utf-8")
    (OUT / "ASSETS.md").write_text(
        "# 生成资源清单\n\n| 来源 | 尺寸 | 格式 | 输入SHA-256 | 解码内存 |\n|---|---|---|---|---|\n"
        + "\n".join(report) + "\n", encoding="utf-8")
    print(f"生成 {len(defs)} 张资源 -> {OUT}")


if __name__ == "__main__":
    main()
