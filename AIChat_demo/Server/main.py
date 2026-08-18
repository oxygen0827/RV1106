import asyncio
from ws_server import WebSocketServer
from threads.audio_send_thread import AudioSendThread
from tools.logger import logger
from service_manager import ServiceManager
from config.settings import access_token_is_valid, global_settings
import sys
sys.path.append("..")

async def main():

    if not access_token_is_valid(global_settings.access_token):
        raise RuntimeError(
            "AICHAT_ACCESS_TOKEN must be 32-64 URL-safe characters"
        )
    if not global_settings.zhipu_api_key:
        logger.warning("ZHIPU_API_KEY is not configured; cloud requests are disabled")

    # 初始化 VAD 和 GLM-4-Voice 会话服务
    service_manager = ServiceManager()

    # 启动语音响应音频发送线程
    audio_send_thread = AudioSendThread(service_manager)
    audio_send_thread.start()

    # 启动 WebSocket 服务器
    server = WebSocketServer(
        host="0.0.0.0",
        port=8000,
        access_token=global_settings.access_token,
        protocol_version=global_settings.protocol_version,
        service_manager=service_manager,
    )
    try:
        await server.start_server()
    except KeyboardInterrupt:
        logger.info("\n服务器正在关闭...")
    finally:
        # 停止线程
        service_manager.stop_event.set()  # 设置停止事件
        audio_send_thread.join()
        logger.info("服务器已关闭。")

if __name__ == "__main__":
    try:
        asyncio.run(main())
    except KeyboardInterrupt:
        logger.info("程序已被用户中断")
    finally:
        # 确保事件循环关闭
        try:
            loop = asyncio.get_event_loop()
            if loop.is_running():
                loop.stop()
        except RuntimeError:
            pass
        logger.info("事件循环已关闭")
