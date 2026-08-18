import dashscope
from dashscope.api_entities.dashscope_response import SpeechSynthesisResponse
from dashscope.audio.tts_v2 import *
import os
import time
from tools.logger import logger


class TTSModel:

    def __init__(self):
        api_key = os.getenv("DASHSCOPE_API_KEY", "").strip()
        if not api_key:
            raise RuntimeError("DASHSCOPE_API_KEY is not configured")
        dashscope.api_key = api_key
        # 初始化 TTS 回调函数
        self.callback = None
        # 初始化 TTS 生成器
        self.synthesizer = SpeechSynthesizer(
            model="cosyvoice-v1",
            voice="longxiaochun",
            format=AudioFormat.PCM_16000HZ_MONO_16BIT,
            callback=self.callback
        )

    class __tts_callback(ResultCallback):

        def __init__(self, on_open=None, on_complete=None, on_error=None, on_close=None, on_data=None):
            self._on_open = on_open or (lambda: logger.info("tts server-WS is open."))
            self._on_complete = on_complete or (lambda: logger.info("Speech synthesis task completed successfully."))
            self._on_error = on_error or (lambda message: logger.info(f"Speech synthesis task failed, {message}"))
            self._on_close = on_close or (lambda: logger.info("tts server-WS is closed."))
            self._on_data = on_data or (lambda data: logger.info(f"Audio result length: {len(data)}"))

        # 实现 ResultCallback 必需的方法
        def on_open(self):
            self._on_open()

        def on_complete(self):
            self._on_complete()

        def on_error(self, message: str):
            self._on_error(message)

        def on_close(self):
            self._on_close()

        def on_data(self, data: bytes) -> None:
            self._on_data(data)

    def tts_stream_set(self, on_open=None, on_complete=None, on_error=None, on_close=None, on_data=None):
        '''设置TTS回调函数, 提前打开ws连接'''
        self.callback = self.__tts_callback(on_open=on_open, on_complete=on_complete,
                                      on_error=on_error, on_close=on_close, on_data=on_data)
        self.synthesizer=SpeechSynthesizer(
            model="cosyvoice-v1",
            voice="longxiaochun",
            format=AudioFormat.PCM_16000HZ_MONO_16BIT,
            callback=self.callback
        )
        try:
            self.synthesizer.streaming_call('') # 先提前打开ws连接
            time.sleep(0.1)
            return True
        except Exception as e:
            logger.error(f"TTS stream setup failed: {e}")
            return False

    def tts_stream_close(self):
        try:
            self.synthesizer.streaming_complete()
            logger.info(f"Request ID: {self.synthesizer.get_last_request_id()}")
            return True
        except Exception as e:
            logger.error(f"TTS stream close failed: {e}")
            return False

    def tts_stream_speech_synthesis(self, text_chunk):
        '''流式语音合成

        :param: text_chunk: 文本片
        :return: 合成是否成功'''

        if text_chunk:
            try:
                self.synthesizer.streaming_call(text_chunk)
            except Exception as e:
                logger.error(f"TTS synthesis failed: {e}")
                return False
        return True

