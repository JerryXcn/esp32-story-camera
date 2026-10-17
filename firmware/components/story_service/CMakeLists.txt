# 故事服务：单任务编排与状态机
idf_component_register(
    SRCS "src/story_service.c"
    INCLUDE_DIRS "include"
    REQUIRES storage_service api_client settings_service network_service camera_service audio_service system_service
    PRIV_REQUIRES
)
