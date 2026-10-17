"""Regressions for the 2026-10-09 implementation; no external provider calls."""
import hashlib
import time
import pytest
from app.errors import JobFailure
from app.providers import base
from app.services import audio_spec


def spec(codec="mp3", rate="24000", channels=1, bitrate="64000", duration="1"):
    return {"streams": [{"codec_type": "audio", "codec_name": codec,
        "sample_rate": rate, "channels": channels, "bit_rate": bitrate}],
        "format": {"duration": duration, "bit_rate": bitrate}}


def finalize():
    return audio_spec.finalize_audio(b"source", 2 * 1024 * 1024, 240000,
                                    lambda b: hashlib.sha256(b).hexdigest())


@pytest.mark.parametrize("duration", ["nan", "inf", "-1", "0", "invalid"])
def test_invalid_duration_fails_closed(monkeypatch, duration):
    monkeypatch.setattr(audio_spec, "probe", lambda *a, **kw: spec(duration=duration))
    with pytest.raises(JobFailure) as error:
        finalize()
    assert error.value.code == "TTS_FAILED"


@pytest.mark.parametrize("after", [None, spec(rate="16000"), spec(channels=2), spec(bitrate="128000")])
def test_transcode_output_is_revalidated(monkeypatch, after):
    probes = iter([spec(codec="pcm_s16le", rate="16000", channels=2), after])
    monkeypatch.setattr(audio_spec, "probe", lambda *a, **kw: next(probes))
    def convert(src, dst, **kw):
        dst.write_bytes(b"unverified conversion")
        return True
    monkeypatch.setattr(audio_spec, "transcode_to_contract", convert)
    with pytest.raises(JobFailure) as error:
        finalize()
    assert error.value.code == "TTS_FAILED"


def test_expired_deadline_never_calls_provider():
    calls = []
    with pytest.raises(base.ProviderCallFailed):
        base.with_retries(lambda: calls.append(1), retries=2, base_delay_s=1,
                          deadline_monotonic=time.monotonic() - 1)
    assert not calls


def test_backoff_including_jitter_cannot_cross_deadline(monkeypatch):
    calls, sleeps = [], []
    monkeypatch.setattr(base.time, "monotonic", lambda: 10.0)
    monkeypatch.setattr(base.time, "sleep", sleeps.append)
    import random
    monkeypatch.setattr(random, "uniform", lambda a, b: 0.3)
    def provider():
        calls.append(1)
        raise base.RetryableHTTP("temporary")
    with pytest.raises(base.ProviderCallFailed):
        base.with_retries(provider, retries=2, base_delay_s=1, deadline_monotonic=11.1)
    assert calls == [1] and not sleeps


def test_remaining_timeout_is_bounded(monkeypatch):
    monkeypatch.setattr(base.time, "monotonic", lambda: 10.0)
    assert base.remaining_timeout(30, 10.25) == 0.25
    with pytest.raises(base.ProviderCallFailed):
        base.remaining_timeout(30, 10)


def test_failed_state_advances_revision(client):
    from conftest import post_job, wait_terminal
    class BrokenTTS:
        def synthesize(self, *a, **kw): return b"invalid mp3"
    client.app.state.providers.tts = BrokenTTS()
    response, _ = post_job(client)
    job_id = response.json()["job_id"]
    terminal = wait_terminal(client, job_id)
    assert terminal["status"] == "failed"
    assert terminal["stage"] == "tts_generating"
    # Accept + running + understanding + story + tts + failure.
    assert terminal["revision"] >= 6

@pytest.mark.parametrize("expected", ["garbage", "1", True, 0, -1, 1.5, [], {}])
def test_retry_attempt_type_is_validated(client, expected):
    from conftest import post_job, wait_terminal, auth
    response, _ = post_job(client)
    job_id = response.json()["job_id"]
    wait_terminal(client, job_id)
    headers = auth() | {"Idempotency-Key": "validation-command"}
    result = client.post(f"/v1/jobs/{job_id}/retry", headers=headers, json={"expected_attempt": expected})
    assert result.status_code == 422
    assert result.json()["error"]["code"] == "INVALID_REQUEST"


def test_recovery_key_cannot_target_another_job(client):
    from conftest import post_job, wait_terminal, auth, DEVICE_A
    first, _ = post_job(client)
    first_id = first.json()["job_id"]
    wait_terminal(client, first_id)
    second, _ = post_job(client)
    second_id = second.json()["job_id"]
    wait_terminal(client, second_id)
    client.app.state.db.put_recovery(DEVICE_A, "bound-command", first_id)
    result = client.post(f"/v1/jobs/{second_id}/retry",
                         headers=auth() | {"Idempotency-Key": "bound-command"}, json={})
    assert result.status_code == 409
    assert result.json()["error"]["code"] == "IDEMPOTENCY_CONFLICT"
