"""OpenAI 兼容的 LLM 客户端（仅标准库）。

支持任意实现了 /v1/chat/completions 的后端：
- 本地：llama.cpp server / vLLM / LM Studio（base_url 指向其 /v1）
- 云端：OpenAI / 通义 / DeepSeek 等（填对应 base_url 与 api_key）
"""
import json
import urllib.request
import urllib.error


class LLMClient:
    def __init__(self, base_url, api_key, model, temperature=0.3):
        self.base_url = base_url.rstrip("/")
        self.api_key = api_key
        self.model = model
        self.temperature = float(temperature)

    def chat(self, system, user, max_tokens=1024):
        """返回助手消息文本内容（str）。"""
        payload = {
            "model": self.model,
            "messages": [
                {"role": "system", "content": system},
                {"role": "user", "content": user},
            ],
            "temperature": self.temperature,
            "max_tokens": max_tokens,
        }
        resp = self._post(payload)
        try:
            return resp["choices"][0]["message"]["content"]
        except (KeyError, IndexError, TypeError) as e:
            raise RuntimeError(f"LLM 返回结构异常: {e} | {str(resp)[:200]}")

    def raw_chat(self, payload):
        """透传完整请求体（代理模式），返回解析后的 dict。"""
        payload = dict(payload)
        payload.setdefault("model", self.model)
        return self._post(payload)

    def _post(self, payload):
        url = f"{self.base_url}/chat/completions"
        data = json.dumps(payload, ensure_ascii=False).encode("utf-8")
        req = urllib.request.Request(url, data=data, method="POST")
        req.add_header("Content-Type", "application/json")
        req.add_header("Authorization", f"Bearer {self.api_key}")
        try:
            with urllib.request.urlopen(req, timeout=180) as resp:
                return json.loads(resp.read().decode("utf-8"))
        except urllib.error.HTTPError as e:
            raise RuntimeError(
                f"LLM HTTP {e.code}: {e.read().decode('utf-8', 'ignore')}"
            )
        except urllib.error.URLError as e:
            raise RuntimeError(f"LLM connection failed: {e}")
