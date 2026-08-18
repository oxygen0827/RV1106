from dataclasses import dataclass

from config.settings import global_settings
from models.voice_model import VoiceModel
from tools.audio_converter import pcm16_to_wav, wav_to_pcm16


@dataclass(frozen=True)
class VoiceResult:
    pcm16: bytes
    text: str


class VoiceService:
    """Turn one VAD-delimited 16 kHz utterance into 16 kHz playback PCM."""

    def __init__(self):
        self.voice_model = VoiceModel()

    def reset(self):
        self.voice_model.clear_messages()

    def generate_voice(self, pcm_data: bytes) -> VoiceResult:
        wav_data = pcm16_to_wav(
            pcm_data,
            sample_rate=global_settings.AUDIO_SAMPLE_RATE,
            channels=global_settings.AUDIO_CHANNELS,
        )
        response = self.voice_model.generate_voice(wav_data)
        normalized_pcm = wav_to_pcm16(
            response.audio_wav,
            target_rate=global_settings.AUDIO_SAMPLE_RATE,
        )
        return VoiceResult(pcm16=normalized_pcm, text=response.text)
