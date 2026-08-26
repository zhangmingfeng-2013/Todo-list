"""容错 JSON 抽取：从模型可能夹带说明文字/代码块的输出中提取首个 JSON 对象或数组。"""
import json


def extract_json(text):
    if not text:
        return None
    t = text.strip()
    # 去掉 ```json ... ``` 围栏
    if t.startswith("```"):
        lines = t.splitlines()
        if lines and lines[0].startswith("```"):
            lines = lines[1:]
        if lines and lines[-1].strip().startswith("```"):
            lines = lines[:-1]
        t = "\n".join(lines).strip()
    # 定位首个 { 或 [
    start = None
    for i, ch in enumerate(t):
        if ch in "{[":
            start = i
            break
    if start is None:
        return None
    open_ch, close_ch = t[start], ("}" if t[start] == "{" else "]")
    depth = 0
    end = None
    for i in range(start, len(t)):
        if t[i] == open_ch:
            depth += 1
        elif t[i] == close_ch:
            depth -= 1
            if depth == 0:
                end = i
                break
    if end is None:
        return None
    try:
        return json.loads(t[start:end + 1])
    except Exception:
        return None
