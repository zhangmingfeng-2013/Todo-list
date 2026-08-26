#!/usr/bin/env python3
"""LoRA 微调 1B 模型（Apple MLX 原生路径）。

流程：
  1) 确保基座模型（默认 mlx-community/Qwen2.5-1.5B-Instruct-4bit，~1GB）。
  2) 用 ai/train/data/train.jsonl 做 LoRA 监督微调。
  3) 融合 LoRA 到基座并量化导出 GGUF。
  4) 注册进 Ollama，供 AI 服务本地离线调用。

依赖：pip install mlx-lm huggingface_hub
基座可换：Qwen2.5-0.5B-Instruct-4bit（严格≤1B）/ Qwen2.5-3B-Instruct-4bit（更强）。

注意：本脚本在训练机上运行（Mac Apple Silicon），不在 cpp-todo 进程内。
"""
import argparse
import json
import os
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
DATA = os.path.join(HERE, "data", "train.jsonl")
DEFAULT_BASE = "mlx-community/Qwen2.5-1.5B-Instruct-4bit"
ADAPTER_DIR = os.path.join(HERE, "adapters")
FUSED_DIR = os.path.join(HERE, "fused")
GGUF_PATH = os.path.join(HERE, "model.gguf")


def run(cmd):
    print("[train] $ " + " ".join(cmd))
    subprocess.run(cmd, check=True)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--base", default=DEFAULT_BASE)
    ap.add_argument("--data", default=DATA)
    ap.add_argument("--iters", type=int, default=1000)
    ap.add_argument("--batch-size", type=int, default=4)
    ap.add_argument("--lora-layers", type=int, default=16)
    ap.add_argument("--lr", type=float, default=1e-4)
    ap.add_argument("--skip-train", action="store_true", help="仅导出（已有 adapters 时）")
    args = ap.parse_args()

    py = sys.executable

    if not args.skip_train:
        os.makedirs(ADAPTER_DIR, exist_ok=True)
        run([
            py, "-m", "mlx_lm.lora",
            "--model", args.base,
            "--data", args.data,
            "--iters", str(args.iters),
            "--batch-size", str(args.batch_size),
            "--lora-layers", str(args.lora_layers),
            "--learning-rate", str(args.lr),
            "--adapter-path", ADAPTER_DIR,
        ])

    # 融合 LoRA → 基座（生成 HF/MLX 合并权重）
    os.makedirs(FUSED_DIR, exist_ok=True)
    run([
        py, "-m", "mlx_lm.fuse",
        "--model", args.base,
        "--adapter-path", ADAPTER_DIR,
        "--save-path", FUSED_DIR,
    ])

    # 导出 GGUF（mlx 提供 gguf 导出）
    run([
        py, "-m", "mlx_lm.gguf",
        "--model", FUSED_DIR,
        "--out", GGUF_PATH,
        "--quantize", "q4_0",
    ])
    print(f"[train] GGUF 已导出: {GGUF_PATH}")
    print(f"[train] 注册到 Ollama: ollama create cpp-todo-ai -f Modelfile")


if __name__ == "__main__":
    main()
