import json
import requests
from config.settings import global_settings
from tools.logger import logger

ZHIPU_API_URL = "https://open.bigmodel.cn/api/paas/v4/chat/completions"

class LLMModel:
    def __init__(self, model_name: str = "GLM-5.1"):
        self.model_name = model_name
        self.messages = [
            {"role": "system", "content": "你是一个桌面机器人, 名为Echo, 快速地回复我."}
        ]

    def _headers(self):
        return {
            "Authorization": f"Bearer {global_settings.zhipu_api_key}",
            "Content-Type": "application/json"
        }

    def set_model_sys_content(self, content: str):
        self.messages[0]["content"] = content

    def add_message(self, role: str, content: str):
        self.messages.append({"role": role, "content": content})

    def clear_messages(self):
        self.messages = [
            {"role": "system", "content": "你是一个桌面机器人, 名为Echo, 全程请快速地回复我. 同时你还有函数执行的功能, 可以根据函数来回复我. "}
        ]

    def get_LLM_response(self, question: str) -> str:
        if question:
            self.add_message("user", question)

        if not global_settings.zhipu_api_key:
            logger.error("ZHIPU_API_KEY is not configured; LLM request skipped")
            return "抱歉，AI 服务尚未配置。"

        try:
            resp = requests.post(
                ZHIPU_API_URL,
                headers=self._headers(),
                json={
                    "model": self.model_name,
                    "messages": self.messages,
                    "stream": False
                },
                timeout=global_settings.API_TIMEOUT
            )
            if resp.status_code == 200:
                data = resp.json()
                content = data["choices"][0]["message"]["content"]
                self.add_message("assistant", content)
                return content
            else:
                logger.error(f"GLM non-stream error: {resp.status_code} {resp.text}")
                return "抱歉，我暂时无法处理您的请求。"
        except Exception as e:
            logger.error(f"GLM non-stream exception: {str(e)}")
            return "抱歉，我暂时无法处理您的请求。"

    def get_LLM_response_stream(self, question):
        if question:
            self.add_message("user", question)

        if not global_settings.zhipu_api_key:
            logger.error("ZHIPU_API_KEY is not configured; streaming LLM request skipped")
            yield -1
            return

        try:
            resp = requests.post(
                ZHIPU_API_URL,
                headers=self._headers(),
                json={
                    "model": self.model_name,
                    "messages": self.messages,
                    "stream": True
                },
                timeout=global_settings.API_TIMEOUT,
                stream=True
            )
            if resp.status_code != 200:
                logger.error(f"GLM stream error: {resp.status_code} {resp.text}")
                yield -1
                return

            full_text = ""
            for line in resp.iter_lines():
                if not line:
                    continue
                line = line.decode("utf-8")
                if line.startswith("data: "):
                    data_str = line[6:]
                    if data_str == "[DONE]":
                        break
                    try:
                        chunk = json.loads(data_str)
                        delta = chunk["choices"][0].get("delta", {})
                        content = delta.get("content", "")
                        if content:
                            full_text += content
                            yield content
                    except Exception:
                        continue
            self.add_message("assistant", full_text)
        except Exception as e:
            logger.error(f"GLM stream exception: {str(e)}")
            yield -1
