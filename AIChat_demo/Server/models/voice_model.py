import base64
from dataclasses import dataclass

import requests

from config.settings import global_settings
from tools.logger import logger


VOICE_API_URL = "https://open.bigmodel.cn/api/paas/v4/chat/completions"


@dataclass(frozen=True)
class VoiceModelResponse:
    audio_wav: bytes
    text: str


class VoiceModel:
    """HTTP adapter for the end-to-end GLM-4-Voice model."""

    def __init__(self, model_name: str | None = None):
        self.model_name = model_name or global_settings.VOICE_MODEL
        self.messages = [
            {
                "role": "system",
                "content": (
                    "身份规则：你是 Echo-Mate 桌面机器人，名称严格为 Echo。"
                    "禁止自称小智、智谱清言、ChatGLM 或任何其他助手名称。"
                    "当用户询问你是谁、叫什么或要求自我介绍时，必须直接回答：我是 Echo，你的桌面机器人助手。"
                    "请理解用户的语音并用自然、简短、友好的中文语音回答。"
                    "不要描述内部处理过程，不要输出 JSON 或代码。"
                ),
            }
        ]

    def clear_messages(self):
        self.messages = self.messages[:1]

    def _headers(self):
        return {
            "Authorization": f"Bearer {global_settings.zhipu_api_key}",
            "Content-Type": "application/json",
        }

    @staticmethod
    def _response_text(message: dict) -> str:
        content = message.get("content", "")
        if isinstance(content, str):
            return content.strip()
        if isinstance(content, list):
            parts = []
            for part in content:
                if isinstance(part, dict) and isinstance(part.get("text"), str):
                    parts.append(part["text"])
            return "".join(parts).strip()
        return ""

    def generate_voice(self, wav_data: bytes) -> VoiceModelResponse:
        if not wav_data:
            raise ValueError("Voice input is empty")
        if not global_settings.zhipu_api_key:
            raise RuntimeError("ZHIPU_API_KEY is not configured")

        user_message = {
            "role": "user",
            "content": [
                {
                    "type": "text",
                    "text": (
                        "请听懂这段语音并直接回答用户。"
                        "严格遵守系统身份规则：你只能自称 Echo。"
                    ),
                },
                {
                    "type": "input_audio",
                    "input_audio": {
                        "data": base64.b64encode(wav_data).decode("ascii"),
                        "format": "wav",
                    },
                },
            ],
        }
        request_messages = self.messages + [user_message]

        try:
            response = requests.post(
                VOICE_API_URL,
                headers=self._headers(),
                json={
                    "model": self.model_name,
                    "messages": request_messages,
                    "stream": False,
                },
                timeout=global_settings.API_TIMEOUT,
            )
        except Exception as exc:
            logger.error(f"GLM-4-Voice request failed: {exc}")
            raise RuntimeError("GLM-4-Voice request failed") from exc

        if response.status_code != 200:
            logger.error(
                "GLM-4-Voice error: %s %s",
                response.status_code,
                response.text[:300],
            )
            raise RuntimeError(f"GLM-4-Voice returned HTTP {response.status_code}")

        try:
            message = response.json()["choices"][0]["message"]
            audio = message["audio"]
            audio_id = audio["id"]
            audio_data = audio["data"]
            if not isinstance(audio_id, str) or not audio_id:
                raise ValueError("assistant audio id is empty")
            audio_wav = base64.b64decode(audio_data, validate=True)
        except (KeyError, IndexError, TypeError, ValueError) as exc:
            logger.error("GLM-4-Voice returned an invalid audio response")
            raise RuntimeError("GLM-4-Voice returned invalid audio") from exc

        response_text = self._response_text(message)
        # GLM-4-Voice requires the assistant audio id in later turns. Keep the
        # id without replaying the previous base64 audio payload.
        assistant_message = {
            "role": "assistant",
            "content": response_text,
            "audio": {"id": audio_id},
        }
        self.messages.extend([user_message, assistant_message])
        if len(self.messages) > 1 + (global_settings.VOICE_HISTORY_TURNS * 2):
            self.messages = [self.messages[0]] + self.messages[-(global_settings.VOICE_HISTORY_TURNS * 2) :]

        return VoiceModelResponse(audio_wav=audio_wav, text=response_text)
