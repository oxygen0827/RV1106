import os
import re


ACCESS_TOKEN_PATTERN = re.compile(r"^[A-Za-z0-9_-]{32,64}$")


class Settings:
    def __init__(self):
        self.protocol_version = int(os.getenv("AICHAT_PROTOCOL_VERSION", "2"))
        self.access_token = os.getenv("AICHAT_ACCESS_TOKEN", "").strip()
        self.zhipu_api_key = os.getenv("ZHIPU_API_KEY", "").strip()
        self.mode = os.getenv("AICHAT_MODE", "voice").strip().lower()
        self.ASR_MODEL = os.getenv("ZHIPU_ASR_MODEL", "glm-asr-2512").strip()

        self.VOICE_MODEL = os.getenv("ZHIPU_VOICE_MODEL", "glm-4-voice").strip()
        self.VOICE_HISTORY_TURNS = max(
            0, int(os.getenv("ZHIPU_VOICE_HISTORY_TURNS", "3"))
        )

        self.AUDIO_SAMPLE_RATE = 16000
        self.AUDIO_CHANNELS = 1
        self.AUDIO_FRAME_DURATION_MS = 40

        self.ASR_DEVICE = os.getenv("AICHAT_ASR_DEVICE", "cpu")
        self.VAD_DEVICE = os.getenv("AICHAT_VAD_DEVICE", "cpu")

        self.API_TIMEOUT = int(os.getenv("AICHAT_API_TIMEOUT", "30"))


global_settings = Settings()


def access_token_is_valid(token):
    return bool(ACCESS_TOKEN_PATTERN.fullmatch(token))
