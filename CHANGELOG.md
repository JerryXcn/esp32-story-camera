# Changelog / 更新日志

## 0.4.0 — first public release / 首次开源发布

- Story library: favorites, bulk favorite / unfavorite / delete (enter by holding a card for 6 s),
  background deletion with progress and a confirmation toast.
  故事库：收藏；按住卡片 6 秒进入选择模式，批量收藏 / 取消收藏 / 删除；删除在后台进行，显示进度与完成提示。
- Player: previous / next buttons and swipe, end-of-story card with Replay / Next (default, auto after a
  countdown) / Back to library.
  播放页：上一个 / 下一个与左右滑动；播完可选重新播放 / 下一个（默认，倒计时后自动）/ 返回故事库。
- Camera preview ~28 fps (direct PPA compositing, async display flush, -O2); lower sensor gain indoors;
  configurable camera orientation.
  拍照预览约 28 帧/秒；室内增益降低、噪点减少；摄像头方向可配置。
- Backend address (domain or IP + port) editable on the device; TLS uses the public CA bundle with
  cross-signed chain verification; nginx + systemd deployment templates.
  设备上可修改后端地址；TLS 走公共根证书包并支持交叉签名链；附公网部署模板。

## 0.2.0 / 0.3.0

- Kid-friendly UI redesign: home screen with four entries, full-screen 3:2 camera and player,
  sound feedback, open-source fonts. 童趣界面改版。

## 0.1.0

- End-to-end loop on real hardware: Wi-Fi setup → capture → story + TTS generation → download,
  verify, commit → auto-play → offline library → regenerate with versions → resumable failures.
  实机闭环跑通。
