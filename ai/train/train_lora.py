#!/usr/bin/env python3
"""LoRA 微调 1B 模型（Apple MLX 原生路径）。

流程：
  1) 基座模型位于 ./base_model（默认 mlx-community/Qwen2.5-1.5B-Instruct-4bit，已预下载）。
  2) 以 ai/prompts.py 为骨架合成的语料 data/train.jsonl（Alpaca 格式）转为 chat
     messages 格式，并按目录拆分为 train/valid/test.jsonl（mlx_lm.lora 要求）。
  3) 在 4bit 基座上做 LoRA 监督微调（QLoRA 风格，Apple Silicon 友好）。
  4) 融合 LoRA 到基座 -> ./fused（MLX 格式，可直接加载推理）。

关于 GGUF / Ollama：
  mlx-lm 0.31.3 已移除 gguf CLI，且其 GGUF 辅助库明确声明
  "Conversion of quantized models is not yet supported"。
  因此在本机更优、更原生的离线闭环是：用 mlx_lm.server 直接服务融合模型
  （OpenAI 兼容 /v1/chat/completions，零外部依赖、全离线、Apple 原生）。
  这同样满足「替换后端 + 离线/隐私闭环」目标。
  如需严格意义的 GGUF+Ollama，需改为 PyTorch(PEFT)+llama.cpp 路线（见 README）。

依赖：pip install mlx-lm huggingface_hub
基座可换：Qwen2.5-0.5B-Instruct-4bit（严格≤1B）/ Qwen2.5-3B-Instruct-4bit（更强）。
"""
import argparse
import json
import os
import random
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
ALPACA = os.path.join(HERE, "data", "train.jsonl")
LORA_DIR = os.path.join(HERE, "data", "lora")
DEFAULT_BASE = os.path.join(HERE, "base_model")
ADAPTER_DIR = os.path.join(HERE, "adapters")
FUSED_DIR = os.path.join(HERE, "fused")


def build_lora_dataset():
    """Alpaca -> chat messages 格式，并按目录拆分为 train/valid/test。"""
    with open(ALPACA, encoding="utf-8") as f:
        records = [json.loads(l) for l in f if l.strip()]
    chat = []
    for r in records:
        chat.append({
            "messages": [
                {"role": "system", "content": r["instruction"]},
                {"role": "user", "content": r["input"]},
                {"role": "assistant", "content": r["output"]},
            ]
        })
    random.seed(42)
    random.shuffle(chat)
    valid = chat[:3]
    test = chat[3:6]
    train = chat[6:]
    os.makedirs(LORA_DIR, exist_ok=True)
    for name, subset in (("train", train), ("valid", valid), ("test", test)):
        with open(os.path.join(LORA_DIR, f"{name}.jsonl"), "w", encoding="utf-8") as f:
            for c in subset:
                f.write(json.dumps(c, ensure_ascii=False) + "\n")
    print(f"[data] 拆分完成: train={len(train)} valid={len(valid)} test={len(test)} -> {LORA_DIR}")
    return len(train)


def run(cmd):
    print("[train] $ " + " ".join(cmd))
    subprocess.run(cmd, check=True)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--base", default=DEFAULT_BASE)
    ap.add_argument("--data", default=LORA_DIR)
    ap.add_argument("--iters", type=int, default=1000)
    ap.add_argument("--batch-size", type=int, default=2)
    ap.add_argument("--num-layers", type=int, default=16)
    ap.add_argument("--lr", type=float, default=1e-4)
    ap.add_argument("--skip-train", action="store_true", help="仅融合（已有 adapters 时）")
    args = ap.parse_args()

    py = sys.executable

    n_train = build_lora_dataset()

    if not args.skip_train:
        run([
            py, "-m", "mlx_lm", "lora",
            "--model", args.base,
            "--data", args.data,
            "--train",
            "--iters", str(args.iters),
            "--batch-size", str(args.batch_size),
            "--num-layers", str(args.num_layers),
            "--learning-rate", str(args.lr),
            "--mask-prompt",
            "--adapter-path", ADAPTER_DIR,
        ])

    # 融合 LoRA -> 基座（MLX 格式，可直接加载推理）
    os.makedirs(FUSED_DIR, exist_ok=True)
    run([
        py, "-m", "mlx_lm", "fuse",
        "--model", args.base,
        "--adapter-path", ADAPTER_DIR,
        "--save-path", FUSED_DIR,
    ])

    print(f"[train] 融合模型已生成: {FUSED_DIR}")
    print("[train] 本地服务（离线闭环）：")
    print(f"        {py} -m mlx_lm server --model {FUSED_DIR} --port 8080")
    print("[train] 将 ai/config.py 的 base_url 指向 http://127.0.0.1:8080/v1 即完成「替换后端」。")


if __name__ == "__main__":
    main()
