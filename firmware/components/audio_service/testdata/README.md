# UART 音频参考样本

`reference.mp3` 是本地生成的 6 秒双音参考样本，前 3 秒 440 Hz，后 3 秒 660 Hz。24 kHz / mono / CBR 64 kbit/s，起止各 20 ms 淡入淡出。主机解码峰值 11488，RMS 约 8043，长度 144000 样本。无真实语音、供应商调用或授权素材。

使用 `audio mp3` 从 UART 请求播放。样本只读嵌入 Flash，经同一个音频任务、文件读取、MP3 解码和 PCM 输出函数播放；不写入 TF，不进入故事库。该测试确认解码与硬件听音链路，不能替代真实 TTS 朗读及长音频验收。

在仓库根目录重新生成：

```sh
ffmpeg -hide_banner -loglevel error \
  -f lavfi -i sine=frequency=440:duration=3:sample_rate=24000 \
  -f lavfi -i sine=frequency=660:duration=3:sample_rate=24000 \
  -filter_complex '[0:a][1:a]concat=n=2:v=0:a=1,volume=2.93,afade=t=in:d=0.02,afade=t=out:st=5.98:d=0.02' \
  -ar 24000 -ac 1 -c:a libmp3lame -b:a 64k -y \
  firmware/components/audio_service/testdata/reference.mp3
```
