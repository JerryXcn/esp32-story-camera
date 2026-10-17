# 音频服务：单任务 MP3 解码与 I2S 输出
idf_component_register(
    SRCS "src/audio_service.c"
    INCLUDE_DIRS "include"
    REQUIRES board_adapter
    REQUIRES esp_ringbuf
    PRIV_REQUIRES esp_codec_dev esp_audio_codec
    EMBED_FILES "testdata/reference.mp3"
                "clips/click.mp3" "clips/shutter.mp3" "clips/prompt_looking.mp3"
                "clips/prompt_ready.mp3" "clips/prompt_done.mp3" "clips/prompt_rejected.mp3"
)
