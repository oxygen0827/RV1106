import threading
import queue
import json
from tools.logger import logger
from service_manager import ServiceManager
from config.settings import global_settings

class AudioSendThread(threading.Thread):
    def __init__(self, sevice_manager: ServiceManager):
        super().__init__(daemon=True)
        self.sevice_manager = sevice_manager

    def run(self):
        remain_data = b''
        active_generation = None
        while not self.sevice_manager.stop_event.is_set():  # 检查 stop_event 是否被设置
            try:
                # 从语音队列中获取语音数据
                queued_audio = self.sevice_manager.audio_queue.get(timeout=1)  # 设置超时时间，避免阻塞
                if isinstance(queued_audio, tuple) and len(queued_audio) == 2 and isinstance(queued_audio[0], int):
                    session_generation, audio_data = queued_audio
                else:
                    session_generation = getattr(
                        self.sevice_manager, "session_generation", 0
                    )
                    audio_data = queued_audio

                if session_generation != getattr(
                    self.sevice_manager, "session_generation", 0
                ):
                    continue
                if active_generation != session_generation:
                    active_generation = session_generation
                    remain_data = b''

                if audio_data is None:
                    # The sentinel is enqueued after the complete PCM result;
                    # this keeps the JSON end marker behind every Opus packet.
                    if remain_data:
                        frame_bytes = (
                            self.sevice_manager.audio_processor.frame_duration_ms
                            * self.sevice_manager.audio_processor.sample_rate
                            // 1000
                            * 2
                        )
                        padded_frame = remain_data.ljust(frame_bytes, b"\0")
                        opus_data = self.sevice_manager.audio_processor.encode_audio(
                            padded_frame
                        )
                        if opus_data:
                            bin_data = self.sevice_manager.audio_processor.pack_bin_frame(
                                type=0,
                                version=global_settings.protocol_version,
                                payload=opus_data,
                            )
                            self.sevice_manager.queue_ws_message(
                                bin_data, session_generation
                            )
                    remain_data = b''
                    self.sevice_manager.queue_ws_message(
                        json.dumps({"type": "voice", "state": "end"}),
                        session_generation,
                    )
                    continue
                # 调用发送回调函数，将语音数据发送给客户端
                # 最开始的数据，需要大于一定值，才开始发送出去，防止断续
                # if len(audio_data) < 1000:
                #     remain_data += audio_data
                #     continue
                # 二进制数据: PCM-16bit 音频数据
                if isinstance(audio_data, bytes):
                    samples_per_frame = int(self.sevice_manager.audio_processor.frame_duration_ms * self.sevice_manager.audio_processor.sample_rate / 1000)*2
                    audio_data = remain_data + audio_data
                    complete_length = len(audio_data) // samples_per_frame * samples_per_frame
                    remain_data = audio_data[complete_length:]
                    # 切片, 编码, 打包, 发送
                    for i in range(0, complete_length, samples_per_frame):
                        frame_slice = audio_data[i:i + samples_per_frame]
                        # 编码当前帧并发送
                        opus_data = self.sevice_manager.audio_processor.encode_audio(frame_slice)
                        if not opus_data:
                            continue
                        bin_data = self.sevice_manager.audio_processor.pack_bin_frame(type=0, version=global_settings.protocol_version, payload=opus_data)
                        self.sevice_manager.queue_ws_message(
                            bin_data, session_generation
                        )
            except queue.Empty:
                # 如果队列为空，继续检查 stop_event
                continue
            except Exception as e:
                logger.error(f"TTS发送线程发生错误: {e}")
