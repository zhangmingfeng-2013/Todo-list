"""AI 服务配置加载。

优先级：环境变量 AI_*  >  ~/.cpp-todo.conf（key=value，可无段头）  >  内置默认值。
兼容标准 INI（含 [section]）与本项目 cpp-todo.conf 的极简格式。

默认模型档位说明：
  内置默认 model = "default_model" —— 指向本机离线融合的「2B 档」模型：
    mlx-community/Qwen2.5-3B-Instruct-4bit 作为基座经 LoRA 微调后的融合权重。
  参数量约 3B（行业内常作为"2B 左右"的主流档位，因为严格 2B 的公开通用模型较少）。
  "default_model" 是 mlx_lm.server 的内置映射名，指向 --model 启动参数指定的路径。
  若内存紧张可降至 1B 档：使用 Ollama 后端（AI_BACKEND=ollama）并设 qwen2.5:1.5b。
"""
import os

DEFAULTS = {
    "base_url": "http://127.0.0.1:8080/v1",  # 指向本地 mlx_lm.server / llama.cpp / vLLM / LM Studio
    "api_key": "sk-no-key",
    "model": "default_model",                 # 默认「2B 档」：Qwen2.5-3B-Instruct-4bit 微调融合版（mlx_lm.server 的 default_model 即 --model 指定的路径）
    "host": "127.0.0.1",
    "port": "8777",
    "temperature": "0.3",
}


def _read_kv(path):
    out = {}
    try:
        with open(path, "r", encoding="utf-8") as f:
            for line in f:
                line = line.strip()
                if not line or line.startswith("#") or line.startswith("["):
                    continue
                if "=" not in line:
                    continue
                k, v = line.split("=", 1)
                out[k.strip()] = v.strip()
    except FileNotFoundError:
        pass
    return out


def load_config():
    cfg = dict(DEFAULTS)
    # Ollama 本地后端预设：AI_BACKEND=ollama 时指向本机 Ollama（离线/隐私）
    if os.environ.get("AI_BACKEND", "").lower() == "ollama":
        cfg["base_url"] = "http://localhost:11434/v1"
        # 默认「2B 档」：qwen2.5:3b 约 3B 参数量，视作市面上最常见的"2B 左右"档位。
        # 16G 内存以下机器可改用 qwen2.5:1.5b（约 1.5B）。
        cfg["model"] = os.environ.get("AI_MODEL", "qwen2.5:3b")
        cfg["api_key"] = os.environ.get("AI_API_KEY", "ollama")
    conf = os.path.expanduser("~/.cpp-todo.conf")
    kv = _read_kv(conf)
    for k in cfg:
        if k in kv:
            cfg[k] = kv[k]
    # 环境变量覆盖（便于容器/临时切换）
    for k in cfg:
        env = os.environ.get("AI_" + k.upper())
        if env is not None:
            cfg[k] = env
    return cfg


if __name__ == "__main__":
    import json
    print(json.dumps(load_config(), ensure_ascii=False, indent=2))
