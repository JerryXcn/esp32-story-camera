"""Measure audio before and after normalization. Unverified output is rejected."""
from __future__ import annotations
import json
import math
import shutil
import subprocess
import tempfile
import time
from dataclasses import dataclass
from pathlib import Path
from ..errors import JobFailure

@dataclass
class AudioSpec:
    format: str = "mp3"
    sample_rate_hz: int = 24000
    channels: int = 1
    bit_rate_bps: int = 64000
    duration_ms: int = 0
    bytes_len: int = 0
    sha256: str = ""
    transcoded: bool = False

def _budget(deadline: float | None, cap: float) -> float:
    remaining = cap if deadline is None else min(cap, deadline - time.monotonic())
    if remaining <= 0:
        raise JobFailure("TTS_FAILED", True, "tts_generating")
    return remaining

def probe(path: Path, timeout_s: float = 30) -> dict | None:
    if not shutil.which("ffprobe"):
        return None
    try:
        out = subprocess.run(
            ["ffprobe", "-v", "error", "-print_format", "json", "-show_streams", "-show_format", str(path)],
            capture_output=True, text=True, timeout=timeout_s,
        )
        return json.loads(out.stdout) if out.returncode == 0 else None
    except (OSError, subprocess.SubprocessError, ValueError):
        return None

# 供应商 TTS 实测约 -24 LUFS（广播电平），设备小喇叭上偏轻且各篇不一致。
# 统一做响度归一到 -16 LUFS（语音内容常用电平），真峰值限 -1.5 dBTP 防削波。
LOUDNORM_FILTER = "loudnorm=I=-16:TP=-1.5:LRA=11"


def transcode_to_contract(src: Path, dst: Path, timeout_s: float = 120) -> bool:
    if not shutil.which("ffmpeg"):
        return False
    try:
        out = subprocess.run(
            ["ffmpeg", "-v", "error", "-y", "-i", str(src), "-map", "0:a:0",
             "-af", LOUDNORM_FILTER,
             "-codec:a", "libmp3lame", "-b:a", "64k", "-ar", "24000", "-ac", "1",
             "-write_xing", "1", str(dst)],
            capture_output=True, text=True, timeout=timeout_s,
        )
        return out.returncode == 0 and dst.is_file()
    except (OSError, subprocess.SubprocessError):
        return False

def _measure(info: dict | None) -> tuple[str, int, int, int, int]:
    try:
        streams = [s for s in info["streams"] if s.get("codec_type") == "audio"]
        if len(streams) != 1:
            raise ValueError("expected one audio stream")
        stream = streams[0]
        duration = float(info["format"]["duration"])
        if not math.isfinite(duration) or duration <= 0:
            raise ValueError("invalid duration")
        rate = int(stream["sample_rate"])
        channels = int(stream["channels"])
        bitrate = int(stream.get("bit_rate") or info["format"].get("bit_rate") or 0)
        if rate <= 0 or channels <= 0 or bitrate <= 0:
            raise ValueError("invalid stream parameters")
        return stream["codec_name"], rate, channels, bitrate, max(1, int(duration * 1000))
    except (TypeError, KeyError, ValueError, OverflowError):
        raise JobFailure("TTS_FAILED", True, "tts_generating") from None

def _contract(measured: tuple) -> bool:
    codec, rate, channels, bitrate, _ = measured
    return codec == "mp3" and rate == 24000 and channels == 1 and bitrate == 64000

def finalize_audio(raw: bytes, audio_max_bytes: int, audio_max_duration_ms: int,
                   sha256_fn, deadline: float | None = None) -> tuple[bytes, AudioSpec]:
    if not raw:
        raise JobFailure("TTS_FAILED", True, "tts_generating")
    if len(raw) > audio_max_bytes:
        raise JobFailure("AUDIO_TOO_LONG", False, "tts_generating")
    with tempfile.TemporaryDirectory() as td:
        src = Path(td) / "raw_audio"
        src.write_bytes(raw)
        measured = _measure(probe(src, timeout_s=_budget(deadline, 30)))
        if measured[4] > audio_max_duration_ms:
            raise JobFailure("AUDIO_TOO_LONG", False, "tts_generating")
        transcoded = False
        final = raw
        # 有 ffmpeg 时总是转码以完成响度归一；没有时契约格式原样通过
        if shutil.which("ffmpeg") or not _contract(measured):
            dst = Path(td) / "contract.mp3"
            if not transcode_to_contract(src, dst, timeout_s=_budget(deadline, 120)):
                raise JobFailure("TTS_FAILED", True, "tts_generating")
            if not dst.is_file() or dst.stat().st_size > audio_max_bytes:
                raise JobFailure("AUDIO_TOO_LONG", False, "tts_generating")
            measured = _measure(probe(dst, timeout_s=_budget(deadline, 30)))
            if not _contract(measured):
                raise JobFailure("TTS_FAILED", True, "tts_generating")
            final = dst.read_bytes()
            transcoded = True
        _budget(deadline, 1)
        if not final or len(final) > audio_max_bytes or measured[4] > audio_max_duration_ms:
            raise JobFailure("AUDIO_TOO_LONG", False, "tts_generating")
        codec, rate, channels, bitrate, duration = measured
        return final, AudioSpec(format=codec, sample_rate_hz=rate, channels=channels,
            bit_rate_bps=bitrate, duration_ms=duration, bytes_len=len(final),
            sha256=sha256_fn(final), transcoded=transcoded)
