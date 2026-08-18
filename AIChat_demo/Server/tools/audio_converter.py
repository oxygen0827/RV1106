"""Small, dependency-light PCM/WAV conversion helpers for voice sessions."""

import io
import wave

import numpy as np


def pcm16_to_wav(pcm_data: bytes, sample_rate: int = 16000, channels: int = 1) -> bytes:
    """Wrap little-endian signed 16-bit PCM in a canonical WAV container."""
    if not pcm_data:
        raise ValueError("PCM audio is empty")
    if channels != 1:
        raise ValueError("Only mono PCM is supported")
    if len(pcm_data) % 2:
        raise ValueError("16-bit PCM must contain an even number of bytes")

    output = io.BytesIO()
    with wave.open(output, "wb") as wav_file:
        wav_file.setnchannels(channels)
        wav_file.setsampwidth(2)
        wav_file.setframerate(sample_rate)
        wav_file.writeframes(pcm_data)
    return output.getvalue()


def _resample_mono_pcm16(samples: np.ndarray, source_rate: int, target_rate: int) -> np.ndarray:
    if source_rate <= 0 or target_rate <= 0:
        raise ValueError("Sample rates must be positive")
    if source_rate == target_rate or samples.size == 0:
        return samples.astype(np.int16, copy=False)

    target_length = max(1, int(round(samples.size * target_rate / source_rate)))
    source_positions = np.arange(samples.size, dtype=np.float64)
    target_positions = np.linspace(
        0.0,
        float(samples.size - 1),
        num=target_length,
        dtype=np.float64,
    )
    converted = np.interp(target_positions, source_positions, samples.astype(np.float64))
    return np.clip(np.rint(converted), -32768, 32767).astype(np.int16)


def wav_to_pcm16(wav_data: bytes, target_rate: int = 16000) -> bytes:
    """Decode a PCM WAV response and normalize it to mono 16 kHz PCM."""
    if not wav_data:
        raise ValueError("WAV audio is empty")

    try:
        wav_stream = wave.open(io.BytesIO(wav_data), "rb")
    except (wave.Error, EOFError) as exc:
        raise ValueError(f"Voice response is not a WAV file: {exc}") from exc

    with wav_stream:
        channels = wav_stream.getnchannels()
        sample_width = wav_stream.getsampwidth()
        source_rate = wav_stream.getframerate()
        compression = wav_stream.getcomptype()
        raw_data = wav_stream.readframes(wav_stream.getnframes())

    if sample_width != 2 or compression != "NONE":
        raise ValueError("Voice response must be uncompressed 16-bit PCM WAV")
    if channels < 1:
        raise ValueError("Voice response has no channels")

    samples = np.frombuffer(raw_data, dtype="<i2")
    if channels > 1:
        samples = samples[: samples.size - (samples.size % channels)]
        samples = samples.reshape(-1, channels).mean(axis=1)
    return _resample_mono_pcm16(samples, source_rate, target_rate).astype("<i2").tobytes()
