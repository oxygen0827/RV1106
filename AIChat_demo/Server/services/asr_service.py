import numpy as np
from config.settings import global_settings
from models.asr_model import ASRModel


class ASRService:
    def __init__(self):
        self.asr_model = ASRModel(device=global_settings.ASR_DEVICE)
        self.asr_model.clear_audio_buffer()

    def reset(self):
        """重置 ASR 状态"""
        self.asr_model.clear_audio_buffer()

    def asr_add_audio_buffer(self, audio_data):
        """
        添加PCM音频数据到缓冲区
        """
        # 将音频数据转换为numpy数组并添加到缓冲区
        self.asr_model.add_audio_buffer(audio_data)

    def asr_take_audio_buffer(self):
        """Detach the current session audio before a blocking cloud request."""
        audio_buffer = self.asr_model.audio_buffer
        self.asr_model.clear_audio_buffer()
        return audio_buffer

    def asr_generate_text(self, audio_buffer=None):
        """
        使用 ASR 模型进行语音识别，生成文本, 然后清空音频缓冲区

        :return: 识别结果文本（字符串）。
                - 如果识别成功，返回转录后的文本。
                - 如果识别失败或没有检测到语音，返回 None。
        """
        if audio_buffer is None:
            audio_buffer = self.asr_take_audio_buffer()
        return self.asr_model.ASR_generate_text(audio_buffer)
