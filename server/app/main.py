"""FastAPI 应用装配：路由、异常信封、生命周期（设备注册、启动恢复、
调度器与 TTL 清理线程）。"""
from __future__ import annotations

from contextlib import asynccontextmanager

from fastapi import FastAPI, Request
from fastapi.responses import JSONResponse

from .api.jobs import router as jobs_router
from .config import get_settings
from .db import Database
from .errors import ApiError
from .providers.factory import build_providers
from .services.auth import token_sha256
from .services.media_store import MediaStore
from .workers.executor import CleanupLoop, JobExecutor


def create_app() -> FastAPI:
    s = get_settings()

    @asynccontextmanager
    async def lifespan(app: FastAPI):
        db = Database(f"{s.data_dir}/server.db")
        media = MediaStore(s.data_dir)
        # 设备令牌注册（env 为准；旋转令牌即更新映射）
        for pair in s.device_tokens_raw.split(","):
            if ":" in pair:
                token, device_id = pair.split(":", 1)
                db.register_device(token_sha256(token.strip()), device_id.strip())
        providers = build_providers(s)
        executor = JobExecutor(db, media, providers, s)
        cleanup = CleanupLoop(db, media, s)
        app.state.db = db
        app.state.media = media
        app.state.providers = providers
        app.state.executor = executor
        app.state.cleanup = cleanup
        executor.start()
        cleanup.start()
        try:
            yield
        finally:
            executor.stop()
            cleanup.stop()
            db.close()

    app = FastAPI(title="esp32-story-camera server", version="1.0.0", lifespan=lifespan)

    @app.exception_handler(ApiError)
    async def api_error_handler(request: Request, exc: ApiError):
        return JSONResponse(status_code=exc.status, content=exc.payload())

    app.include_router(jobs_router)
    return app


app = create_app()
