import json

import numpy as np

from config.settings import global_settings
from service_manager import ServiceManager
from tools.logger import logger


class AudioHandler:
    """Decode 16 kHz Opus frames and submit VAD-delimited voice turns."""

    def __init__(self, service_manager: ServiceManager):
        self.service_manager = service_manager
        self.service_manager.is_vad = False

    def _submit_voice_turn(self, reason: str):
        session_generation = getattr(self.service_manager, "session_generation", 0)
        pcm_data = self.service_manager.take_voice_audio()
        self.service_manager.is_vad = True
        logger.info("Voice turn ended (%s), pcm_bytes=%d", reason, len(pcm_data))
        self.service_manager.queue_ws_message(
            json.dumps({"type": "voice", "state": "processing"}),
            session_generation,
        )
        task = (
            self.service_manager.asr_start_task
            if getattr(self.service_manager, "mode", "voice") == "asr"
            else self.service_manager.voice_start_task
        )
        self.service_manager.task_manager.submit_task(task, pcm_data, session_generation)

    async def handle_audio_message(self, msg):
        bin_protocol = self.service_manager.audio_processor.unpack_bin_frame(msg)
        if not bin_protocol:
            return

        protocol_version, message_type, payload = bin_protocol
        if (
            message_type != 0
            or protocol_version != global_settings.protocol_version
            or self.service_manager.is_vad
        ):
            return

        pcm_data = self.service_manager.audio_processor.decode_audio(payload)
        if not pcm_data:
            return
        self.service_manager.append_voice_audio(pcm_data)
        vad_result = self.service_manager.vad_service.process_audio_frame(
            np.frombuffer(pcm_data, dtype=np.int16)
        )

        if vad_result == 1:
            self._submit_voice_turn("speech_end")
        elif vad_result == 2:
            session_generation = getattr(self.service_manager, "session_generation", 0)
            self.service_manager.take_voice_audio()
            self.service_manager.is_vad = True
            self.service_manager.queue_ws_message(
                json.dumps({"type": "voice", "state": "no_speech"}),
                session_generation,
            )
            logger.info("Voice turn dropped: no speech")
        elif vad_result == 3:
            self._submit_voice_turn("buffer_full")
