# UI 字库（生成产物，勿手改）

由 `../tools/gen_fonts.sh` 用 lv_font_conv 生成，4bpp。字体均为开源可商用，许可证在 `licenses/`。

| 文件 | 字号 | 来源字体 | 字符集 | 用途 |
|---|---|---|---|---|
| `ui_font_16.c` / `ui_font_22.c` | 16 / 22 px | 源圆黑体 Medium（Resource Han Rounded CN v0.990，MIT + OFL） | UI 文案 + 文档措辞 + 高频常用字 + GB2312 一级 3755 字 | 正文、按钮、AI 生成的故事标题 |
| `ui_kid_24.c` / `ui_kid_32.c` / `ui_kid_44.c` | 24 / 32 / 44 px | 站酷快乐体（ZCOOL KuaiLe，OFL） | `../tools/kid_texts.txt` 中的固定文案 | 首页入口、页面标题、弹窗标题等大字；缺字回退到 22px 正文 |

字体文件约 15MB，不入库。重新生成：

```bash
# 源圆黑体：https://github.com/CyanoHao/Resource-Han-Rounded/releases （RHR-CN-*.7z，取 ResourceHanRoundedCN-Medium.ttf）
# 站酷快乐体：https://github.com/google/fonts/tree/main/ofl/zcoolkuaile
sh ../tools/gen_fonts.sh <ResourceHanRoundedCN-Medium.ttf> <ZCOOLKuaiLe-Regular.ttf>
```

v0.1.0 及以前使用 macOS 系统黑体（STHeiti），其许可不允许随硬件分发，v0.2 起已替换。
