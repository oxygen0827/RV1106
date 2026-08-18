import asyncio
import websockets
import json
from contextlib import suppress
from handle.text_handler import TextHandler
from handle.audio_handler import AudioHandler
from handle.auth_handler import AuthHandler
from service_manager import ServiceManager

import sys
sys.path.append("..")
from tools.logger import logger

class WebSocketServer:
    def __init__(
        self,
        host="0.0.0.0",
        port=8000,
        access_token=None,
        device_id="00:11:22:33:44:55",
        protocol_version=2,
        service_manager: ServiceManager = None,
    ):
        if not access_token:
            raise ValueError("AIChat access token is required")

        self.host = host
        self.port = port

        # 初始化vad asr chat intent tts等服务
        self.service_manager = service_manager
        # 初始化文本和音频处理器
        self.text_handler = TextHandler(self.service_manager)
        self.audio_handler = AudioHandler(self.service_manager)
        # 初始化鉴权处理器
        self.auth_handler = AuthHandler(access_token, device_id, protocol_version)
        self._connection_lock = asyncio.Lock()
        self._active_websocket = None

    async def _claim_client(self, websocket):
        async with self._connection_lock:
            previous_websocket = self._active_websocket
            self._active_websocket = websocket
            self.service_manager.reset_services()

        if previous_websocket is not None and previous_websocket is not websocket:
            with suppress(Exception):
                await previous_websocket.close(reason="Replaced by a new session")

    async def _release_client(self, websocket):
        async with self._connection_lock:
            if self._active_websocket is not websocket:
                return False
            self._active_websocket = None
            self.service_manager.reset_services()
            return True

    async def process_send_queue(self, websocket):
        """
        异步任务：从发送队列中取出数据并发送
        """
        while self._active_websocket is websocket:
            try:
                # 检查队列是否为空
                if not self.service_manager.ws_send_queue.empty():
                    # 队列不为空时获取数据
                    data = self.service_manager.ws_send_queue.get_nowait()  # 非阻塞获取数据
                    if (
                        isinstance(data, tuple)
                        and len(data) == 2
                        and isinstance(data[0], int)
                    ):
                        session_generation, data = data
                        if session_generation != getattr(
                            self.service_manager, "session_generation", 0
                        ):
                            continue
                    # 通过 WebSocket 发送数据
                    await websocket.send(data)
                    # logger.info(f"发送数据到客户端: {len(data)} bytes")
                else:
                    # 如果队列为空，稍作等待
                    await asyncio.sleep(0.1)
            except Exception as e:
                logger.error(f"发送队列处理错误: {e}")
                return

    async def handle_client(self, websocket):
        """
        处理客户端连接
        """
        # connected
        logger.info("Client connected")
        authenticated = False
        process_task = None
        try:
            # 获取连接时的请求头
            headers = websocket.request_headers

            # 执行鉴权
            if not self.auth_handler.authenticate(headers):
                await websocket.send(json.dumps({"type": "auth", "message": "Authentication failed"}))
                await websocket.close(reason="Authentication failed")
                logger.error("Authentication failed for client")
                return

            await self._claim_client(websocket)
            authenticated = True

            # 鉴权通过后，向客户端发送成功响应
            response = {
                "type": "auth",
                "message": "Client authenticated",
                "mode": self.service_manager.mode,
                "audio_params": {
                    "format": "opus",
                    "sample_rate": 16000,
                    "channels": 1,
                    "frame_duration": 40,
                },
            }
            await websocket.send(json.dumps(response))

            # Only the active authenticated client may consume the send queue.
            if self._active_websocket is not websocket:
                return
            process_task = asyncio.create_task(self.process_send_queue(websocket))

            # 开始接收和处理客户端消息
            async for message in websocket:
                if isinstance(message, bytes):
                    # 处理音频消息
                    await self.audio_handler.handle_audio_message(message)
                else:
                    # 处理 JSON 文本消息
                    text = json.loads(message)
                    await self.text_handler.handle_text_message(text)

        except websockets.exceptions.ConnectionClosed as e:
            logger.warning(f"Connection closed: {e}")
        finally:
            if process_task:
                process_task.cancel()
                with suppress(asyncio.CancelledError):
                    await process_task
            logger.info("Client disconnected")
            if authenticated:
                await self._release_client(websocket)

    async def start_server(self):
        """
        启动 WebSocket 服务器
        """
        async with websockets.serve(self.handle_client, self.host, self.port):
            logger.info(f"WebSocket server started on {self.host}:{self.port}")
            await asyncio.Future()  # 保持服务器运行
