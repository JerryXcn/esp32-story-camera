# Assets / 界面素材

| Folder | Content | Origin |
|---|---|---|
| `v0.2/` | Home tiles, fox illustrations, home background (`src/` = full-size originals) | AI-generated for this project |
| `v0.2/icons/` | Line icons, rendered from `firmware/components/app_ui/tools/icons/icons.html` | Drawn for this project |
| `mascot/`, `background/`, `decoration/`, `icon/` | v0.1 fox scenes, stars and settings icons still used by the firmware | Sliced from the project's original UI design |

Convert to LVGL C arrays (output: `firmware/components/app_ui/generated/`):

```bash
python3 firmware/components/app_ui/tools/icons/render_icons.py   # icons.html -> PNG (needs Google Chrome)
python3 firmware/components/app_ui/tools/convert_assets.py       # PNG -> RGB565 / RGB565A8 / A8
```

素材均为本项目原创（含 AI 生成），随项目以 Apache-2.0 发布。
