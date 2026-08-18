import requests
import io
import wave
import numpy as np
from config.settings import global_settings
from tools.logger import logger

class ASRModel:
    def __init__(self, device="cpu"):
        self.audio_buffer = np.array([], dtype=np.int16)

    def clear_audio_buffer(self):
        self.audio_buffer = np.array([], dtype=np.int16)

    def add_audio_buffer(self, pcm_data):
        audio_data_array = np.frombuffer(pcm_data, dtype=np.int16)
        self.audio_buffer = np.append(self.audio_buffer, audio_data_array)

    def get_audio_buffer_length(self):
        return len(self.audio_buffer)

    def ASR_generate_text(self, audio_buffer):
        if len(audio_buffer) == 0:
            return None
        if not global_settings.zhipu_api_key:
            logger.error("ZHIPU_API_KEY is not configured; ASR request skipped")
            return None

        wav_buf = io.BytesIO()
        w = wave.open(wav_buf, "wb")
        w.setnchannels(1)
        w.setsampwidth(2)
        w.setframerate(16000)
        if audio_buffer.dtype == np.float32:
            audio_buffer = (audio_buffer * 32767).astype(np.int16)
        w.writeframes(audio_buffer.tobytes())
        w.close()

        try:
            resp = requests.post(
                "https://open.bigmodel.cn/api/paas/v4/audio/transcriptions",
                headers={
                    "Authorization": f"Bearer {global_settings.zhipu_api_key}"
                },
                files={"file": ("speech.wav", wav_buf.getvalue(), "audio/wav")},
                data={"model": global_settings.ASR_MODEL},
                timeout=global_settings.API_TIMEOUT,
            )
            if resp.status_code == 200:
                data = resp.json()
                text = data.get("text", "")
                if text and text != "无":
                    return text
            else:
                logger.error(f"GLM ASR error: {resp.status_code} {resp.text[:200]}")
        except Exception as e:
            logger.error(f"GLM ASR exception: {e}")
        return None
