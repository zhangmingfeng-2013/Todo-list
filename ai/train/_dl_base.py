import os
from huggingface_hub import snapshot_download
repo = "mlx-community/Qwen2.5-1.5B-Instruct-4bit"
out = os.path.abspath("./base_model")
print(f"[dl] target={repo} -> {out}")
path = snapshot_download(repo_id=repo, local_dir=out, local_dir_use_symlinks=False)
print("[dl] DONE:", path)
for f in sorted(os.listdir(out)):
    fp = os.path.join(out, f)
    if os.path.isfile(fp):
        print(f"  {f}  {os.path.getsize(fp)//1024} KB")
