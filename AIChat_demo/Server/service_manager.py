import json
import queue
import threading

from services.vad_service import VADService
from services.asr_service import ASRService
from services.voice_service import VoiceService
from threads.task_manager import TaskManager
from tools.audio_processor import AudioProcessor
from tools.logger import logger
from tools.registry import global_registry
from config.settings import global_settings


class ServiceManager:
    """Own the per-client voice session and the WebSocket output queues."""

    def __init__(self):
        self.audio_processor = AudioProcessor()
        self.mode = global_settings.mode if global_settings.mode in ("voice", "asr") else "voice"
        self.vad_service = VADService()
        self.asr_service = ASRService()
        self._session_lock = threading.RLock()
        self._voice_service_factory = VoiceService
        self.voice_service = None
        self.is_vad = False
        self.voice_audio_buffer = bytearray()
        self.session_generation = 0

        # audio_queue contains (session_generation, PCM bytes), followed by
        # (session_generation, None) to serialize the voice end marker after
        # every encoded Opus frame has entered ws_send_queue.
        self.audio_queue = queue.Queue()
        self.ws_send_queue = queue.Queue()
        self.stop_event = threading.Event()
        self.task_manager = TaskManager()

        def continue_chat():
            return "继续聊天..."

        def handle_exit_intent():
            return "再见！"

        global_registry.register_function("continue_chat", "继续聊天意图", {}, continue_chat)
        global_registry.register_function("exit_chat", "结束对话意图", {}, handle_exit_intent)

    def reset_services(self):
        """Invalidate outstanding cloud work and clear the old voice turn."""
        with self._get_session_lock():
            self.session_generation = getattr(self, "session_generation", 0) + 1
            old_voice_service = getattr(self, "voice_service", None)
            self.voice_service = None
            self.is_vad = False
            self.voice_audio_buffer.clear()

        if old_voice_service:
            try:
                old_voice_service.reset()
            except Exception:
                pass
        self.vad_service.reset()
        self.asr_service.reset()
        for pending_queue in (self.audio_queue, self.ws_send_queue):
            self._drain_queue(pending_queue)

    @staticmethod
    def _drain_queue(pending_queue):
        while True:
            try:
                pending_queue.get_nowait()
            except queue.Empty:
                return

    def _get_session_lock(self):
        lock = getattr(self, "_session_lock", None)
        if lock is None:
            lock = threading.RLock()
            self._session_lock = lock
        return lock

    def prepare_voice_session(self):
        """Create a clean multimodal conversation for the current listening turn."""
        with self._get_session_lock():
            session_generation = getattr(self, "session_generation", 0)
            if getattr(self, "voice_service", None) is not None:
                return True

        try:
            voice_service = getattr(self, "_voice_service_factory", VoiceService)()
        except Exception as exc:
            logger.warning(f"GLM-4-Voice unavailable: {exc}")
            return False

        with self._get_session_lock():
            if not self._is_current_session(session_generation):
                return False
            self.voice_service = voice_service
        return True

    def append_voice_audio(self, pcm_data: bytes):
        if pcm_data and not self.is_vad:
            self.voice_audio_buffer.extend(pcm_data)

    def take_voice_audio(self) -> bytes:
        audio_data = bytes(self.voice_audio_buffer)
        self.voice_audio_buffer.clear()
        return audio_data

    def _is_current_session(self, session_generation):
        return session_generation == getattr(self, "session_generation", 0)

    def queue_ws_message(self, message, session_generation=None):
        if session_generation is None:
            self.ws_send_queue.put(message)
            return True
        if not self._is_current_session(session_generation):
            return False
        self.ws_send_queue.put((session_generation, message))
        return True

    def _queue_error(self, code, message, session_generation=None):
        if session_generation is not None and not self._is_current_session(session_generation):
            return
        self.queue_ws_message(
            json.dumps({"type": "error", "code": code, "message": message}),
            session_generation,
        )

    def voice_start_task(self, pcm_data: bytes, session_generation=None):
        """Call GLM-4-Voice for one VAD-delimited utterance."""
        if session_generation is None:
            session_generation = getattr(self, "session_generation", 0)
        with self._get_session_lock():
            if not self._is_current_session(session_generation):
                logger.info("Discarding voice task from a closed session")
                return -1
            voice_service = self.voice_service

        if voice_service is None:
            self._queue_error(
                "voice_unavailable",
                "GLM-4-Voice is not configured",
                session_generation,
            )
            return -1
        if not pcm_data:
            self._queue_error(
                "voice_unavailable",
                "Voice input is empty",
                session_generation,
            )
            return -1

        try:
            result = voice_service.generate_voice(pcm_data)
        except Exception as exc:
            logger.error(f"GLM-4-Voice generation failed: {exc}")
            self._queue_error(
                "voice_unavailable",
                "Voice conversation failed",
                session_generation,
            )
            return -1

        if not self._is_current_session(session_generation):
            logger.info("Discarding voice response from a closed session")
            return -1
        if not result.pcm16:
            self._queue_error(
                "voice_unavailable",
                "GLM-4-Voice returned empty audio",
                session_generation,
            )
            return -1

        if result.text:
            self.queue_ws_message(
                json.dumps(
                    {"type": "voice", "state": "text", "text": result.text},
                    ensure_ascii=False,
                ),
                session_generation,
            )
        # The audio sender emits the end marker only after all PCM has been
        # converted to individual 40 ms Opus packets.
        self.audio_queue.put((session_generation, result.pcm16))
        self.audio_queue.put((session_generation, None))
        logger.info(
            "GLM-4-Voice response queued: pcm_bytes=%d text=%s",
            len(result.pcm16),
            bool(result.text),
        )
        return None

    def asr_start_task(self, pcm_data: bytes, session_generation=None):
        """Transcribe one VAD-delimited utterance with GLM ASR."""
        if session_generation is None:
            session_generation = getattr(self, "session_generation", 0)
        if not self._is_current_session(session_generation):
            return -1
        if not pcm_data:
            self._queue_error("asr_empty", "Voice input is empty", session_generation)
            return -1
        try:
            self.asr_service.asr_add_audio_buffer(pcm_data)
            text = self.asr_service.asr_generate_text()
        except Exception as exc:
            logger.error("GLM ASR generation failed: %s", exc)
            self._queue_error("asr_unavailable", "ASR request failed", session_generation)
            return -1
        if not self._is_current_session(session_generation):
            return -1
        if not text:
            self._queue_error("asr_empty", "No speech recognized", session_generation)
            return -1
        self.queue_ws_message(
            json.dumps({"type": "asr", "state": "text", "text": text}, ensure_ascii=False),
            session_generation,
        )
        self.queue_ws_message(
            json.dumps({"type": "asr", "state": "end"}), session_generation
        )
        logger.info("GLM ASR result: %s", text)
        return None
