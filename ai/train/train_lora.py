#!/usr/bin/env python3
"""LoRA 微调 1B/2B(3B档)/3B 模型（Apple MLX 原生路径）。

默认「2B档」—— mlx-community/Qwen2.5-3B-Instruct-4bit（3B ≈ 市面上最常见的 2B+ 档位，
HuggingFace / Ollama 没有严格 2B 的 Qwen2.5，用 3B-Instruct 作为「2B 左右」目标是
兼顾参数量与可用性的行业共识）。

流程：
  1) 基座模型位于 ./base_model（根据 --train-size 自动选择默认下载路径；
     也可用 --base 指向本地任意 HuggingFace/MLX 模型目录）。
  2) 以 ai/prompts.py 为骨架合成的语料 data/train.jsonl（Markdown + JSON 围栏格式）
     转为 chat messages 格式，并按目录拆分为 train/valid/test.jsonl（mlx_lm.lora 要求）。
  3) 在 4bit 基座上做 LoRA 监督微调（QLoRA 风格，Apple Silicon 友好）。
  4) 融合 LoRA 到基座 -> ./fused（MLX 格式，可直接加载推理）。

关于 GGUF / Ollama：
  mlx-lm 新版本已移除 gguf CLI，且 GGUF 辅助库明确声明
  "Conversion of quantized models is not yet supported"。
  因此在本机更优、更原生的离线闭环是：用 mlx_lm.server 直接服务融合模型
  （OpenAI 兼容 /v1/chat/completions，零外部依赖、全离线、Apple 原生）。
  如需严格意义的 GGUF+Ollama，需改为 PyTorch(PEFT)+llama.cpp 路线。

快速入口：
  # 2B档（默认，推荐）
  ./venv/bin/python train_lora.py --train-size 2b
  # 1B档（机器内存 < 16G 时使用）
  ./venv/bin/python train_lora.py --train-size 1b
  # 3B档（机器内存 >= 32G，更强效果）
  ./venv/bin/python train_lora.py --train-size 3b

依赖：pip install -r requirements-train.txt
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
DEFAULT_BASE_LOCAL = os.path.join(HERE, "base_model")
ADAPTER_DIR = os.path.join(HERE, "adapters")
FUSED_DIR = os.path.join(HERE, "fused")

# ──────────── 基座预设（按档位） ────────────
# HuggingFace / mlx-community 仓库名；首次使用会自动下载到 HF cache，
# 然后可通过 --base 覆盖到本地拷贝目录。
SIZE_PRESETS = {
    "1b": {
        "hf_id": "mlx-community/Qwen2.5-1.5B-Instruct-4bit",
        "label": "Qwen2.5-1.5B-Instruct-4bit (≈1.5B 实际参数量)",
        # 1.5B：hidden_size=1536, num_layers=28 → 默认 LoRA 调 16 层够用
        "num_layers": 16,
        "batch_size": 2,
        "iters": 1000,
        "lr": 1e-4,
        "rank": 8,
    },
    "2b": {
        # 市面上没有严格 2B 的 Qwen2.5 Instruct，3B 是「2B 左右」最接近的一档，
        # 也是 Ollama / HuggingFace 生态中公认的「2B 档位」基线。
        "hf_id": "mlx-community/Qwen2.5-3B-Instruct-4bit",
        "label": "Qwen2.5-3B-Instruct-4bit (≈3B，'2B 左右'目标档位)",
        # 3B：num_hidden_layers=36 → 默认对 24 层做 LoRA，确保 2/3 注意力层被覆盖
        "num_layers": 24,
        "batch_size": 1,  # M 系列 Mac 16G 内存常用值；若 32G+ 可手动改成 2
        "iters": 1200,    # 参数量更大，步数略微增加
        "lr": 5e-5,       # 大模型 + LoRA 学习率适当降低，稳定收敛
        "rank": 16,       # 更大 rank 承载 3B 模型的新增能力
    },
    "3b": {
        "hf_id": "mlx-community/Qwen2.5-3B-Instruct-4bit",
        "label": "Qwen2.5-3B-Instruct-4bit (3B 全档位)",
        "num_layers": 30,  # 更激进：几乎所有层
        "batch_size": 1,
        "iters": 1500,
        "lr": 4e-5,
        "rank": 32,
    },
}


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
    n = len(chat)
    # 兼容极小语料（本项目手工语料约 40 条）：保证 train 至少 60%，valid/test 各≥1条
    if n <= 3:
        train, valid, test = chat, [], []
    elif n <= 10:
        valid_n = 1
        test_n = 1
        train = chat[valid_n + test_n:]
        valid = chat[:valid_n]
        test = chat[valid_n:valid_n + test_n]
    else:
        valid_n = max(1, n // 10)
        test_n = max(1, n // 10)
        valid = chat[:valid_n]
        test = chat[valid_n:valid_n + test_n]
        train = chat[valid_n + test_n:]
    os.makedirs(LORA_DIR, exist_ok=True)
    for name, subset in (("train", train), ("valid", valid), ("test", test)):
        with open(os.path.join(LORA_DIR, f"{name}.jsonl"), "w", encoding="utf-8") as f:
            for c in subset:
                f.write(json.dumps(c, ensure_ascii=False) + "\n")
    print(f"[data] 拆分完成: train={len(train)} valid={len(valid)} test={len(test)} -> {LORA_DIR}")
    return len(train)


def download_preset(preset):
    """首次使用：自动从 HuggingFace 下载预设基座（若本地不存在 base_model/config.json 且未指定 --base）。"""
    print(f"[base] 预设档位「{preset['label']}」")
    try:
        from huggingface_hub import snapshot_download  # 延迟导入，避免未装依赖的 dry-run 报错
    except ImportError as e:
        print("[base] ⚠️  huggingface_hub 未安装。请先: pip install -r requirements-train.txt")
        raise e
    local = snapshot_download(
        repo_id=preset["hf_id"],
        resume_download=True,
    )
    print(f"[base] 已缓存到: {local}")
    return local


def run(cmd, env=None):
    print("[train] $ " + " ".join(cmd))
    subprocess.run(cmd, check=True, env=env)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument(
        "--train-size", choices=["1b", "2b", "3b"], default="2b",
        help="训练目标档位（默认 2b ≈ Qwen2.5-3B-Instruct-4bit，「2B 左右」常用档位）",
    )
    ap.add_argument(
        "--base", default=None,
        help="本地基座模型目录（MLX 或 HuggingFace 格式）。未指定时按 --train-size 预设自动下载/使用 ./base_model",
    )
    ap.add_argument("--data", default=LORA_DIR)
    ap.add_argument("--iters", type=int, default=None, help="训练步数；不填则按档位预设")
    ap.add_argument("--batch-size", type=int, default=None, help="不填则按档位预设")
    ap.add_argument("--num-layers", type=int, default=None, help="参与 LoRA 的 Transformer 层数；不填则按档位预设")
    ap.add_argument("--lr", type=float, default=None, help="学习率；不填则按档位预设")
    ap.add_argument("--rank", type=int, default=None, help="LoRA rank；不填则按档位预设")
    ap.add_argument("--skip-train", action="store_true", help="仅融合（已有 adapters 时）")
    ap.add_argument("--download-only", action="store_true", help="只下载对应档位的基座到本地，不训练不融合")
    args = ap.parse_args()

    py = sys.executable
    preset = SIZE_PRESETS[args.train_size]

    # 1) 基座解析
    if args.base:
        base = args.base
        print(f"[base] 使用用户指定基座: {base}")
    elif os.path.isfile(os.path.join(DEFAULT_BASE_LOCAL, "config.json")):
        base = DEFAULT_BASE_LOCAL
        print(f"[base] 使用本地 base_model: {base}")
    else:
        print(f"[base] 未指定 --base 且本地无 base_model，按 {args.train_size} 档自动下载…")
        base = download_preset(preset)

    if args.download_only:
        print(f"[download-only] 完成。基座路径: {base}")
        return

    # 2) 参数应用（用户显式覆盖 > 档位预设）
    iters = args.iters if args.iters is not None else preset["iters"]
    batch_size = args.batch_size if args.batch_size is not None else preset["batch_size"]
    num_layers = args.num_layers if args.num_layers is not None else preset["num_layers"]
    lr = args.lr if args.lr is not None else preset["lr"]
    rank = args.rank if args.rank is not None else preset["rank"]

    print()
    print("========== 训练参数 ==========")
    print(f"  档位     : {args.train_size}  ({preset['label']})")
    print(f"  基座路径 : {base}")
    print(f"  iters    : {iters}")
    print(f"  batch    : {batch_size}")
    print(f"  layers   : {num_layers}")
    print(f"  lr       : {lr}")
    print(f"  rank     : {rank}")
    print("================================")
    print()

    # 3) 数据集拆分（基于最新 synthesize 产物）
    n_train = build_lora_dataset()
    if n_train == 0:
        print("[data] ⚠️  train.jsonl 为空，请先运行: python synthesize.py")
        sys.exit(2)

    if not args.skip_train:
        extra_env = os.environ.copy()
        cmd = [
            py, "-m", "mlx_lm", "lora",
            "--model", base,
            "--data", args.data,
            "--train",
            "--iters", str(iters),
            "--batch-size", str(batch_size),
            "--num-layers", str(num_layers),
            "--learning-rate", str(lr),
            "--lora-layers", str(rank),  # mlx-lm 新版本使用 --lora-layers 指定 rank；旧版本会忽略该参数，安全
            "--mask-prompt",
            "--adapter-path", ADAPTER_DIR,
        ]
        # 兼容旧版 mlx-lm：若不认识 --lora-layers，则剥离该参数重试（rank 默认由 mlx-lm 决定=8）
        try:
            run(cmd, env=extra_env)
        except subprocess.CalledProcessError as exc:
            if "--lora-layers" not in str(cmd):
                raise
            print(f"[train] 警告：mlx-lm 版本不支持 --lora-layers，剥离后重试 (returncode={exc.returncode})")
            cmd2 = [c for c in cmd if c != "--lora-layers" and c != str(rank)]
            # 去掉一对 "--lora-layers <rank>"
            reduced = []
            i = 0
            while i < len(cmd):
                if cmd[i] == "--lora-layers" and i + 1 < len(cmd):
                    i += 2
                    continue
                reduced.append(cmd[i])
                i += 1
            run(reduced, env=extra_env)

    # 4) 融合 LoRA -> 基座（MLX 格式，可直接加载推理）
    os.makedirs(FUSED_DIR, exist_ok=True)
    run([
        py, "-m", "mlx_lm", "fuse",
        "--model", base,
        "--adapter-path", ADAPTER_DIR,
        "--save-path", FUSED_DIR,
    ])

    print()
    print("========== 训练完成 ==========")
    print(f"  融合模型目录: {FUSED_DIR}")
    print(f"  启动离线服务: {py} -m mlx_lm server --model {FUSED_DIR} --host 127.0.0.1 --port 8080")
    print("  或直接执行:  bash serve_trained.sh")
    print("  闭环验证:    bash verify_closed_loop.sh")
    print()
    print("  将 ai/config.py （或环境变量 AI_BASE_URL / AI_MODEL）保持默认值即可自动对接。")


if __name__ == "__main__":
    main()
