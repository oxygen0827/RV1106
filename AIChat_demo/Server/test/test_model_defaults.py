import base64
import io
import os
import sys
import unittest
import wave
from pathlib import Path
from unittest.mock import Mock, patch

import numpy as np


SERVER_DIR = Path(__file__).resolve().parents[1]
DEMO_ROOT = SERVER_DIR.parents[1]
if str(SERVER_DIR) not in sys.path:
    sys.path.insert(0, str(SERVER_DIR))

from config.settings import Settings, global_settings
from models.voice_model import VoiceModel, VoiceModelResponse
from services.voice_service import VoiceService
from tools.audio_converter import pcm16_to_wav, wav_to_pcm16


class ModelDefaultsTests(unittest.TestCase):
    def test_voice_defaults_match_deployed_model_id(self):
        with patch.dict(os.environ, {}, clear=True):
            settings = Settings()
        self.assertEqual(settings.VOICE_MODEL, "glm-4-voice")
        self.assertEqual(settings.AUDIO_SAMPLE_RATE, 16000)
        self.assertEqual(settings.AUDIO_CHANNELS, 1)

    def test_voice_identity_prompt_locks_the_name_to_echo(self):
        prompt = VoiceModel().messages[0]["content"]
        self.assertIn("名称严格为 Echo", prompt)
        self.assertIn("禁止自称小智", prompt)
        self.assertIn("我是 Echo，你的桌面机器人助手", prompt)

    def test_voice_request_contains_wav_audio_and_model(self):
        input_wav = pcm16_to_wav(np.zeros(1600, dtype=np.int16).tobytes())
        output_wav = pcm16_to_wav(np.zeros(1600, dtype=np.int16).tobytes(), 22050)
        response = Mock(status_code=200)
        response.json.return_value = {
            "choices": [{
                "message": {
                    "role": "assistant",
                    "content": "你好。",
                    "audio": {
                        "id": "audio-turn-1",
                        "data": base64.b64encode(output_wav).decode("ascii"),
                    },
                }
            }]
        }

        with patch.object(global_settings, "zhipu_api_key", "server-secret"), patch(
            "models.voice_model.requests.post", return_value=response
        ) as post:
            result = VoiceModel().generate_voice(input_wav)

        request = post.call_args.kwargs["json"]
        self.assertEqual(request["model"], "glm-4-voice")
        prompt_part = request["messages"][-1]["content"][0]
        self.assertIn("严格遵守系统身份规则", prompt_part["text"])
        self.assertIn("只能自称 Echo", prompt_part["text"])
        audio_part = request["messages"][-1]["content"][-1]
        self.assertEqual(audio_part["type"], "input_audio")
        self.assertEqual(audio_part["input_audio"]["format"], "wav")
        self.assertEqual(base64.b64decode(audio_part["input_audio"]["data"]), input_wav)
        self.assertEqual(result.text, "你好。")

    def test_voice_history_keeps_assistant_audio_id_for_next_turn(self):
        input_wav = pcm16_to_wav(np.zeros(1600, dtype=np.int16).tobytes())
        output_wav = pcm16_to_wav(np.zeros(1600, dtype=np.int16).tobytes(), 22050)
        response = Mock(status_code=200)
        response.json.side_effect = [
            {
                "choices": [{
                    "message": {
                        "role": "assistant",
                        "content": "第一句。",
                        "audio": {
                            "id": "audio-turn-1",
                            "data": base64.b64encode(output_wav).decode("ascii"),
                        },
                    }
                }]
            },
            {
                "choices": [{
                    "message": {
                        "role": "assistant",
                        "content": "第二句。",
                        "audio": {
                            "id": "audio-turn-2",
                            "data": base64.b64encode(output_wav).decode("ascii"),
                        },
                    }
                }]
            },
        ]

        with patch.object(global_settings, "zhipu_api_key", "server-secret"), patch(
            "models.voice_model.requests.post", return_value=response
        ) as post:
            model = VoiceModel()
            model.generate_voice(input_wav)
            model.generate_voice(input_wav)

        second_request = post.call_args_list[1].kwargs["json"]
        assistant_history = second_request["messages"][-2]
        self.assertEqual(assistant_history["role"], "assistant")
        self.assertEqual(assistant_history["audio"]["id"], "audio-turn-1")

    def test_voice_request_requires_server_key(self):
        with patch.object(global_settings, "zhipu_api_key", ""):
            with self.assertRaisesRegex(RuntimeError, "ZHIPU_API_KEY"):
                VoiceModel().generate_voice(b"wav")

    def test_voice_service_normalizes_provider_wav_to_16khz_pcm(self):
        source_wav = pcm16_to_wav(np.zeros(22050, dtype=np.int16).tobytes(), 22050)
        service = VoiceService()
        service.voice_model.generate_voice = Mock(
            return_value=VoiceModelResponse(audio_wav=source_wav, text="reply")
        )

        result = service.generate_voice(np.zeros(16000, dtype=np.int16).tobytes())

        self.assertEqual(len(result.pcm16), 16000 * 2)
        self.assertEqual(result.text, "reply")

    def test_wav_converter_rejects_non_pcm_response(self):
        output = io.BytesIO()
        with wave.open(output, "wb") as wav_file:
            wav_file.setnchannels(1)
            wav_file.setsampwidth(1)
            wav_file.setframerate(22050)
            wav_file.writeframes(b"\x80" * 100)
        with self.assertRaisesRegex(ValueError, "16-bit"):
            wav_to_pcm16(output.getvalue())


class SourceBoundaryTests(unittest.TestCase):
    def test_client_and_deskbot_do_not_store_cloud_llm_credentials(self):
        source_paths = [
            DEMO_ROOT / "AIChat_demo/Client/Application/Application.h",
            DEMO_ROOT / "AIChat_demo/Client/Application/Application.cc",
            DEMO_ROOT / "AIChat_demo/Client/c_interface/AIchat_c_interface.h",
            DEMO_ROOT / "AIChat_demo/Client/c_interface/AIchat_c_interface.cc",
            DEMO_ROOT / "AIChat_demo/Client/main.cc",
            DEMO_ROOT / "DeskBot_demo/common/sys_manager/sys_manager.h",
            DEMO_ROOT / "DeskBot_demo/common/sys_manager/sys_manager.c",
            DEMO_ROOT / "DeskBot_demo/gui_app/ui.c",
            DEMO_ROOT / "DeskBot_demo/utils/system_para.conf",
        ]

        for source_path in source_paths:
            with self.subTest(source_path=source_path):
                self.assertNotIn("aliyun_api_key", source_path.read_text())


if __name__ == "__main__":
    unittest.main()
