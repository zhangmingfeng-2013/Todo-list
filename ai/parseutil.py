"""容错 JSON / Markdown 抽取工具。

- extract_json：保留旧行为——从模型可能夹带说明文字/代码块的输出中提取首个 JSON 对象或数组（向后兼容）。
- split_markdown_and_json：适用于新 prompt，模型先输出可读 Markdown，最后带一个 ```json ``` 围栏。
  返回 (markdown_text, json_obj)，缺任一则对应为 None。
- _repair_json：容错修复 LLM 常见输出缺陷（缺逗号/缺括号/尾逗号等）。
"""
import json
import re


def _repair_json(text):
    """尝试修复 LLM 常见 JSON 输出缺陷，返回修复后的字符串；无法修复返回原文本。

    修复策略（按序尝试，逐层修复）：
    1. 清除字符串中的多余转义
    2. 补齐 对象/数组元素之间缺失的 {（},"key" → },"{key"）
    3. 补齐缺失的逗号（}→}, 等）
    4. 移除尾部多余逗号
    5. 自动补全缺失的闭合括号（根据未闭合深度追加 } 或 ]）
    """
    if not text:
        return text

    s = text

    # 策略 1: 处理 LLM 偶发输出的 \" 转义
    if '\\"' in s:
        s = s.replace('\\"', '"')

    # 策略 2: 补齐对象数组中缺失的 {
    # 场景 A: 数组元素的最后一个字段是数字，后面紧跟着对象闭合 } 和下一个 key
    #   例: [0]},"title" → [0]},{"title"
    s = re.sub(r'(\d)\s*\]\s*\}\s*,\s*"', r'\1]},{"', s)
    # 场景 B: 字段值为对象，} 后直接跟 ,"key 缺少 {
    #   例: {"depends_on":[0]}},"title" → {"depends_on":[0]}},{"title"
    s = re.sub(r'(\})\s*,\s*(")', r'\1, {\2', s)

    # 策略 3: 补齐缺失的逗号
    s = re.sub(r'}\s*\{', r'}, {', s)
    s = re.sub(r']\s*\{', r']}, {', s)

    # 策略 4: 移除尾部逗号
    s = re.sub(r',\s*]', ']', s)
    s = re.sub(r',\s*}', '}', s)

    # 策略 5: 自动补全缺失的闭合括号
    # 统计未闭合的括号（忽略字符串内的括号）
    open_count = 0  # { 未闭合数
    close_count = 0  # } 未闭合数
    bracket_pairs = {'{': '}', '[': ']', '}': '{', ']': '['}
    # 使用栈来追踪未闭合的括号
    stack = []
    in_string = False
    escape = False
    for ch in s:
        if escape:
            escape = False
            continue
        if ch == '\\':
            escape = True
            continue
        if ch == '"':
            in_string = not in_string
            continue
        if in_string:
            continue
        if ch in ('{', '['):
            stack.append(ch)
        elif ch in ('}', ']'):
            if stack and bracket_pairs.get(ch) == stack[-1]:
                stack.pop()
            else:
                # 多余的闭合括号，忽略
                pass

    # 栈中剩余的就是未闭合的开括号，需要逆序补全对应的闭括号
    if stack:
        missing = ''.join(bracket_pairs[b] for b in reversed(stack))
        s = s.rstrip()
        # 如果末尾已有空白或换行，直接追加
        s += missing

    return s


def _try_parse_json(text):
    """尝试解析 JSON，失败时返回 (None, None)，成功返回 (parsed, repaired_text)。"""
    if not text:
        return None, None
    # 直接解析
    try:
        return json.loads(text), text
    except Exception:
        pass
    # 修复后解析
    repaired = _repair_json(text)
    if repaired != text:
        try:
            return json.loads(repaired), repaired
        except Exception:
            pass
    return None, repaired


def _find_last_fenced_block(text, info="json"):
    """返回 text 中最后一个 ```<info>? ... ``` 围栏的 (start_idx, end_idx, content)，找不到返回 None。"""
    if not text:
        return None
    lines = text.splitlines(keepends=True)
    fence_open = None  # (i, info_stripped)
    candidates = []
    for i, line in enumerate(lines):
        stripped = line.lstrip()
        if stripped.startswith("```"):
            info_str = stripped[3:].rstrip("\r\n")
            if fence_open is None:
                fence_open = (i, info_str.strip().lower())
            else:
                open_info = fence_open[1]
                close_info = info_str.strip().lower()
                info_ok = (not open_info) or (not close_info) or (open_info == close_info)
                if info_ok:
                    start_line = fence_open[0]
                    end_line = i
                    content = "".join(lines[start_line + 1 : end_line])
                    candidates.append((start_line, end_line, content))
                    fence_open = None
                else:
                    fence_open = (i, close_info)
    if not candidates:
        return None
    start_line, end_line, content = candidates[-1]
    fence_start = sum(len(l) for l in lines[:start_line])
    fence_end = sum(len(l) for l in lines[: end_line + 1])
    return fence_start, fence_end, content


def extract_json(text):
    """从文本中提取并解析 JSON，包含容错修复。找不到返回 None。

    容错策略：
    1. 先按标准括号匹配找到候选 JSON 片段
    2. 若解析失败，扫描所有可能的结束位置（从后往前），逐个尝试
    3. 若仍失败，回退到「首括号 → 末括号」兜底策略
    """
    if not text:
        return None
    t = text.strip()
    # 去掉 ```json ... ``` 围栏（若整段是）
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

    open_ch = t[start]
    close_ch = "}" if open_ch == "{" else "]"

    # 收集所有 depth 归零的位置（可能有多个——当 JSON 中间缺 { 时会提前归零）
    depth = 0
    in_string = False
    escape = False
    end_positions = []
    for i in range(start, len(t)):
        c = t[i]
        if escape:
            escape = False
            continue
        if c == "\\":
            escape = True
            continue
        if c == '"':
            in_string = not in_string
            continue
        if in_string:
            continue
        if c == open_ch:
            depth += 1
        elif c == close_ch:
            depth -= 1
            if depth == 0:
                end_positions.append(i)
                # 不 break，继续收集所有可能的结束位置

    if not end_positions:
        # bracket 匹配找不到结束位置——可能缺少闭合括号
        # 直接尝试解析完整文本（_repair_json 会自动补全缺失的闭合括号）
        parsed, _ = _try_parse_json(t)
        if parsed is not None:
            return parsed
        return None

    # 从最后一个结束位置开始尝试（最可能包含完整 JSON）
    for end in reversed(end_positions):
        candidate = t[start : end + 1]
        parsed, _ = _try_parse_json(candidate)
        if parsed is not None:
            return parsed

    # 兜底：先尝试 rfind 最后一个 close_ch
    last_close = t.rfind(close_ch)
    if last_close > start:
        candidate = t[start : last_close + 1]
        parsed, _ = _try_parse_json(candidate)
        if parsed is not None:
            return parsed

    # 最终兜底：尝试从 start 开始的完整文本（_repair_json 会自动补全）
    full_candidate = t[start:]
    parsed, _ = _try_parse_json(full_candidate)
    if parsed is not None:
        return parsed

    return None


def split_markdown_and_json(text):
    """从「Markdown + 末尾 JSON 围栏」格式中分离展示文本与结构化 JSON。

    返回 (markdown_text, parsed_json)：
      - markdown_text：围栏之前的文本（保留前后空白，若无围栏则为整段 text 去除首尾空白）
      - parsed_json：围栏内的 JSON 解析结果；若没围栏 / 解析失败 则为 None
    """
    if not text:
        return "", None
    fence = _find_last_fenced_block(text, info="json")
    if fence is None:
        # 退化：尝试从全文 extract_json；Markdown = 全文（剥离 JSON）
        parsed = extract_json(text)
        if parsed is None:
            return text.strip(), None
        # 找到 JSON 对象/数组的范围，从文本中剔除后作为 markdown
        t = text
        start = None
        for i, ch in enumerate(t):
            if ch in "{[":
                start = i
                break
        if start is None:
            return t.strip(), parsed
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
            return t.strip(), parsed
        md_part = (t[:start] + t[end + 1 :]).strip()
        return md_part, parsed

    fence_start, fence_end, json_content = fence
    md_text = text[:fence_start].rstrip()
    # 若围栏前面还有 ``` 开栏（上一段不是 Markdown），保持原样即可
    parsed = extract_json(json_content)
    return md_text, parsed


def build_fallback_markdown(feature, parsed, fallback_title="AI 输出"):
    """当模型未输出 markdown（只输出 JSON）时，依据 JSON 结构回退生成一份像样的 Markdown 文本。

    feature ∈ {"decompose","extract","predict","reprioritize","unknown"}
    """
    if not isinstance(parsed, dict):
        return f"## {fallback_title}\n\n```\n{parsed}\n```"

    if feature == "decompose":
        lines = [f"## 目标：{parsed.get('goal') or '（未提供）'}", ""]
        steps = parsed.get("steps") or []
        if not steps:
            lines.append("_（模型未返回子步骤）_")
        for i, s in enumerate(steps):
            title = s.get("title") or "（未命名）"
            mins = s.get("estimated_minutes")
            note = s.get("note") or ""
            dep = s.get("depends_on") or []
            line = f"{i+1}. **{title}**"
            if mins is not None:
                line += f"（预估 {mins} 分钟）"
            if note:
                line += f" —— {note}"
            if dep:
                line += " _（依赖 " + " ".join(f"#{d+1}" for d in dep) + "）_"
            lines.append(line)
        return "\n".join(lines)

    if feature == "extract":
        lines = ["## 提取到的待办", ""]
        items = parsed.get("items") or []
        if not items:
            lines.append("_（未提取到待办）_")
        for it in items:
            title = it.get("title") or "（未命名）"
            note = it.get("note") or ""
            line = f"- **{title}**"
            if note:
                line += f" —— {note}"
            lines.append(line)
        return "\n".join(lines)

    if feature == "predict":
        lines = ["## 提前准备的待办", ""]
        todos = parsed.get("todos") or []
        if not todos:
            lines.append("_（未生成待办）_")
        for it in todos:
            title = it.get("title") or "（未命名）"
            note = it.get("note") or ""
            due = it.get("due") or "无日期"
            pri = {2: "高", 1: "中", 0: "低"}.get(it.get("priority"), "中")
            line = f"- **{title}** 📅{due} ⚑{pri}"
            if note:
                line += f" —— {note}"
            lines.append(line)
        return "\n".join(lines)

    if feature == "reprioritize":
        lines = []
        ordered = parsed.get("ordered") or []
        # 可用标题映射：优先 _titles（app.py 注入），其次 o.title
        titles = parsed.get("_titles") or {}

        def _title_of(tid):
            if tid is None:
                return "任务"
            if str(tid) in titles:
                return titles[str(tid)]
            return f"任务 #{tid}"

        if ordered:
            lines.append("## 建议顺序")
            for i, o in enumerate(ordered):
                if not isinstance(o, dict):
                    continue
                pri = o.get("predicted_priority")
                pri_cn = "高" if (pri is not None and pri >= 0.8) else "中" if (pri is not None and pri >= 0.4) else "低"
                tid = o.get("id")
                # ordered 对象自身 title 优先于 _titles 映射
                title = o.get("title") if isinstance(o.get("title"), str) and o.get("title").strip() else _title_of(tid)
                reason = o.get("reason") or ""
                # 根据 pri 粗算简单默认理由（仅当模型完全没给时）
                if not reason:
                    if pri is not None and pri >= 0.8:
                        reason = "模型判定优先级较高（截止近/工作量大）"
                    elif pri is not None and pri >= 0.4:
                        reason = "模型判定优先级中等"
                    else:
                        reason = "模型判定可延后处理"
                line = f"{i+1}. **{title}** ⚑{pri_cn}"
                if reason:
                    line += f" —— {reason}"
                lines.append(line)
        postpone = parsed.get("postpone") or []
        if postpone:
            lines.append("")
            lines.append("## 📌 可延后")
            for tid in postpone:
                lines.append(f"- {_title_of(tid)}")
        drop = parsed.get("drop") or []
        if drop:
            lines.append("")
            lines.append("## 🗑 可舍弃")
            for tid in drop:
                lines.append(f"- {_title_of(tid)}")
        if not lines:
            return "## 优先级推演\n\n_当前任务池无需调整。_"
        return "\n".join(lines)

    # unknown
    return f"## {fallback_title}\n\n```json\n{json.dumps(parsed, ensure_ascii=False, indent=2)}\n```"
