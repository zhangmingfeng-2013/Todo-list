"""OpenAI 兼容的 LLM 客户端（仅标准库）。

支持任意实现了 /v1/chat/completions 的后端：
- 本地：llama.cpp server / vLLM / LM Studio（base_url 指向其 /v1）
- 云端：OpenAI / 通义 / DeepSeek 等（填对应 base_url 与 api_key）
"""
import json
import os
import urllib.request
import urllib.error

# 始终直连后端，绕过系统 HTTP(S)_PROXY / ALL_PROXY。
# 本服务的后端（mlx_lm.server / llama.cpp / Ollama）均在 127.0.0.1，
# 一旦请求经透明代理转发到 localhost 会失败（404/502）。
_no_proxy_opener = urllib.request.build_opener(urllib.request.ProxyHandler({}))


class LLMClient:
    def __init__(self, base_url, api_key, model, temperature=0.3):
        self.base_url = base_url.rstrip("/")
        self.api_key = api_key
        self.model = model
        self.temperature = float(temperature)
        self._resolved = None  # 懒加载：从 /v1/models 发现后端真实模型名
        self._discover_error = None

    def _discover_model(self):
        """向 /v1/models 查询后端实际模型 id（避免 model 名不匹配导致 404）。"""
        url = f"{self.base_url}/models"
        try:
            req = urllib.request.Request(url, method="GET")
            req.add_header("Authorization", f"Bearer {self.api_key}")
            with _no_proxy_opener.open(req, timeout=10) as resp:
                data = json.loads(resp.read().decode("utf-8"))
            models = (data or {}).get("data") or []
            if models:
                mid = models[0].get("id")
                if mid:
                    # mlx_lm.server 把加载路径作为 model id 严格校验，必须原样返回；
                    # Ollama / vLLM 等返回的模型名本身就不是路径，原样同样可用。
                    print(f"[llm] discovered model='{mid}' from {url}")
                    return mid
            print(f"[llm] warn: /v1/models returned empty list, fallback to '{self.model}'")
            return None
        except Exception as e:
            self._discover_error = str(e)
            print(f"[llm] warn: model discovery failed: {e}")
            return None

    def _model(self):
        if self._resolved is None:
            self._resolved = self._discover_model() or self.model
        return self._resolved

    def resolved_model(self):
        """触发并返回解析后的模型名（供 health 展示）。"""
        return self._model()

    def chat(self, system, user, max_tokens=1024):
        """返回助手消息文本内容（str）。"""
        payload = {
            "model": self._model(),
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
        payload.setdefault("model", self._model())
        return self._post(payload)

    def _post(self, payload):
        url = f"{self.base_url}/chat/completions"
        data = json.dumps(payload, ensure_ascii=False).encode("utf-8")
        req = urllib.request.Request(url, data=data, method="POST")
        req.add_header("Content-Type", "application/json")
        req.add_header("Authorization", f"Bearer {self.api_key}")
        try:
            with _no_proxy_opener.open(req, timeout=180) as resp:
                return json.loads(resp.read().decode("utf-8"))
        except urllib.error.HTTPError as e:
            raise RuntimeError(
                f"LLM HTTP {e.code}: {e.read().decode('utf-8', 'ignore')}"
            )
        except urllib.error.URLError as e:
            raise RuntimeError(f"LLM connection failed: {e}")
