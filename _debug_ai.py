#!/usr/bin/env python3
"""Debug script to trace the AI service pipeline."""
import sys, json
sys.path.insert(0, './ai')
from llm import LLMClient
from config import load_config
from prompts import decompose_prompt
from parseutil import split_markdown_and_json, extract_json

cfg = load_config()
llm = LLMClient(cfg['base_url'], cfg['api_key'], cfg['model'], float(cfg.get('temperature', 0.3)))

system, user = decompose_prompt('准备一场公司年会')
text = llm.chat(system, user)

# 保存原始输出到文件
with open('/tmp/ai_raw_output.txt', 'w') as f:
    f.write(text)

# 逐行分析 _find_last_fenced_block
lines = text.splitlines(keepends=True)
fence_open = None
candidates = []
for i, line in enumerate(lines):
    stripped = line.lstrip()
    if stripped.startswith("```"):
        info_str = stripped[3:].rstrip("\r\n")
        if fence_open is None:
            fence_open = (i, info_str.strip().lower())
            print(f"  Line {i}: OPEN fence info='{info_str.strip().lower()}'")
        else:
            open_info = fence_open[1]
            close_info = info_str.strip().lower()
            info_ok = (not open_info) or (not close_info) or (open_info == close_info)
            print(f"  Line {i}: CLOSE fence info='{close_info}' (open='{open_info}', ok={info_ok})")
            if info_ok:
                start_line = fence_open[0]
                end_line = i
                content = "".join(lines[start_line + 1 : end_line])
                candidates.append((start_line, end_line, content))
                fence_open = None
            else:
                fence_open = (i, close_info)

print(f"\n候选围栏数: {len(candidates)}")
if candidates:
    start_line, end_line, content = candidates[-1]
    print(f"选中: lines [{start_line}..{end_line}]")
    print(f"content repr (前200): {repr(content[:200])}")
    
    # 尝试直接解析
    try:
        obj = json.loads(content.strip())
        print(f"直接解析成功: {list(obj.keys())}")
    except Exception as e:
        print(f"直接解析失败: {e}")
        # 尝试 extract_json
        obj2 = extract_json(content)
        print(f"extract_json 结果: {type(obj2).__name__} = {str(obj2)[:100] if obj2 else 'None'}")

md, parsed = split_markdown_and_json(text)
print(f"\n最终结果:")
print(f"  md 长度: {len(md) if md else 0}")
print(f"  parsed 类型: {type(parsed).__name__}")
print(f"  has 'steps': {'steps' in parsed if isinstance(parsed, dict) else 'N/A'}")
