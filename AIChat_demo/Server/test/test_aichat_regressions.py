import asyncio
import json
import queue
import sys
import threading
import unittest
from pathlib import Path
from unittest.mock import Mock

import numpy as np


SERVER_DIR = Path(__file__).resolve().parents[1]
if str(SERVER_DIR) not in sys.path:
    sys.path.insert(0, str(SERVER_DIR))

from config.settings import access_token_is_valid, global_settings
from handle.audio_handler import AudioHandler
from handle.text_handler import TextHandler
from service_manager import ServiceManager
from threads.audio_send_thread import AudioSendThread


def dequeue_payload(pending_queue):
    item = pending_queue.get_nowait()
    if isinstance(item, tuple) and len(item) == 2 and isinstance(item[0], int):
        return item[1]
    return item


class FakeAudioProcessor:
    sample_rate = 16000
    frame_duration_ms = 40

    def unpack_bin_frame(self, _message):
        return global_settings.protocol_version, 0, b"opus"

    def decode_audio(self, _payload):
        return np.ones(320, dtype=np.int16).tobytes()

    def encode_audio(self, _pcm):
        return b"opus-response"

    def pack_bin_frame(self, **kwargs):
        return b"bin:" + kwargs["payload"]


class FakeVadService:
    def __init__(self, result=1):
        self.result = result
        self.reset_count = 0

    def process_audio_frame(self, _audio):
        return self.result

    def reset(self):
        self.reset_count += 1


class FakeTaskManager:
    def __init__(self):
        self.calls = []

    def submit_task(self, *args):
        self.calls.append(args)


class DummyVoiceManager:
    def __init__(self, vad_result=1):
        self.audio_processor = FakeAudioProcessor()
        self.vad_service = FakeVadService(vad_result)
        self.task_manager = FakeTaskManager()
        self.ws_send_queue = queue.Queue()
        self.voice_audio_buffer = bytearray()
        self.is_vad = False
        self.session_generation = 4
        def voice_start_task(*args):
            self.task_manager.calls.append(args)
        self.voice_start_task = voice_start_task

    def append_voice_audio(self, pcm):
        self.voice_audio_buffer.extend(pcm)

    def take_voice_audio(self):
        data = bytes(self.voice_audio_buffer)
        self.voice_audio_buffer.clear()
        return data

    def queue_ws_message(self, message, session_generation=None):
        if session_generation is not None:
            message = (session_generation, message)
        self.ws_send_queue.put(message)
        return True


class VoiceHandlerTests(unittest.TestCase):
    def test_vad_end_submits_pcm_without_asr(self):
        manager = DummyVoiceManager(vad_result=1)
        asyncio.run(AudioHandler(manager).handle_audio_message(b"audio"))

        self.assertTrue(manager.is_vad)
        self.assertEqual(len(manager.task_manager.calls), 1)
        func, pcm, generation = manager.task_manager.calls[0]
        self.assertEqual(func.__name__, "voice_start_task")
        self.assertEqual(pcm, np.ones(320, dtype=np.int16).tobytes())
        self.assertEqual(generation, 4)
        message = json.loads(dequeue_payload(manager.ws_send_queue))
        self.assertEqual(message, {"type": "voice", "state": "processing"})

    def test_no_speech_discards_buffer_and_returns_to_idle_signal(self):
        manager = DummyVoiceManager(vad_result=2)
        asyncio.run(AudioHandler(manager).handle_audio_message(b"audio"))

        self.assertTrue(manager.is_vad)
        self.assertEqual(bytes(manager.voice_audio_buffer), b"")
        message = json.loads(dequeue_payload(manager.ws_send_queue))
        self.assertEqual(message, {"type": "voice", "state": "no_speech"})


class ServiceManagerTests(unittest.TestCase):
    def _manager(self, voice_service):
        manager = ServiceManager.__new__(ServiceManager)
        manager._session_lock = threading.RLock()
        manager.session_generation = 7
        manager.voice_service = voice_service
        manager.audio_queue = queue.Queue()
        manager.ws_send_queue = queue.Queue()
        return manager

    def test_voice_response_queues_audio_then_end_sentinel(self):
        class FakeVoiceService:
            def generate_voice(self, pcm):
                return type("Result", (), {"pcm16": b"pcm-response", "text": "回答"})()

        manager = self._manager(FakeVoiceService())
        self.assertIsNone(manager.voice_start_task(b"pcm-input", session_generation=7))

        self.assertEqual(manager.audio_queue.get_nowait(), (7, b"pcm-response"))
        self.assertEqual(manager.audio_queue.get_nowait(), (7, None))
        generation, message = manager.ws_send_queue.get_nowait()
        self.assertEqual(generation, 7)
        self.assertEqual(json.loads(message), {"type": "voice", "state": "text", "text": "回答"})

    def test_stale_voice_response_is_discarded(self):
        class FakeVoiceService:
            def generate_voice(self, _pcm):
                return type("Result", (), {"pcm16": b"pcm", "text": ""})()

        manager = self._manager(FakeVoiceService())
        self.assertEqual(manager.voice_start_task(b"pcm", session_generation=6), -1)
        self.assertTrue(manager.audio_queue.empty())
        self.assertTrue(manager.ws_send_queue.empty())

    def test_voice_error_is_reported_without_audio(self):
        class FailingVoiceService:
            def generate_voice(self, _pcm):
                raise RuntimeError("provider failed")

        manager = self._manager(FailingVoiceService())
        self.assertEqual(manager.voice_start_task(b"pcm", session_generation=7), -1)
        message = json.loads(dequeue_payload(manager.ws_send_queue))
        self.assertEqual(message["type"], "error")
        self.assertEqual(message["code"], "voice_unavailable")
        self.assertTrue(manager.audio_queue.empty())


class AudioSendThreadTests(unittest.TestCase):
    def test_voice_end_marker_follows_all_opus_packets(self):
        manager = type("Manager", (), {})()
        manager.audio_processor = FakeAudioProcessor()
        manager.audio_queue = queue.Queue()
        manager.ws_send_queue = queue.Queue()
        manager.session_generation = 1
        manager.stop_event = threading.Event()

        def queue_ws_message(message, session_generation=None):
            manager.ws_send_queue.put((session_generation, message))

        manager.queue_ws_message = queue_ws_message
        manager.audio_queue.put((1, np.ones(1280, dtype=np.int16).tobytes()))
        manager.audio_queue.put((1, None))
        sender = threading.Thread(target=AudioSendThread(manager).run)
        sender.start()
        sender.join(timeout=0.2)
        manager.stop_event.set()
        sender.join(timeout=1.5)

        packets = []
        while not manager.ws_send_queue.empty():
            packets.append(manager.ws_send_queue.get_nowait()[1])
        self.assertEqual(packets[:-1], [b"bin:opus-response", b"bin:opus-response"])
        self.assertEqual(json.loads(packets[-1]), {"type": "voice", "state": "end"})

    def test_voice_end_marker_flushes_the_partial_final_pcm_frame(self):
        manager = type("Manager", (), {})()
        manager.audio_processor = FakeAudioProcessor()
        manager.audio_queue = queue.Queue()
        manager.ws_send_queue = queue.Queue()
        manager.session_generation = 1
        manager.stop_event = threading.Event()

        def queue_ws_message(message, session_generation=None):
            manager.ws_send_queue.put((session_generation, message))

        manager.queue_ws_message = queue_ws_message
        # 2 complete 40 ms frames (2 * 1280 bytes) plus a 40-byte tail.
        manager.audio_queue.put((1, np.ones(1300, dtype=np.int16).tobytes()))
        manager.audio_queue.put((1, None))
        sender = threading.Thread(target=AudioSendThread(manager).run)
        sender.start()
        sender.join(timeout=0.2)
        manager.stop_event.set()
        sender.join(timeout=1.5)

        packets = []
        while not manager.ws_send_queue.empty():
            packets.append(manager.ws_send_queue.get_nowait()[1])
        self.assertEqual(
            packets[:-1],
            [b"bin:opus-response", b"bin:opus-response", b"bin:opus-response"],
        )
        self.assertEqual(json.loads(packets[-1]), {"type": "voice", "state": "end"})


class SettingsTests(unittest.TestCase):
    def test_access_token_policy(self):
        self.assertTrue(access_token_is_valid("a" * 32))
        self.assertTrue(access_token_is_valid("A_" + "b" * 62))
        self.assertFalse(access_token_is_valid("short-token"))
        self.assertFalse(access_token_is_valid("a" * 65))


if __name__ == "__main__":
    unittest.main()
