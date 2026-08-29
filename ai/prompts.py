"""四个智能特征的提示词模板。

每个函数返回 (system, user) 字符串。模型输出两部分：
1) 顶部：人类可读的 Markdown 文本（给前端渲染展示，含小标题/有序列表/项目符号/表格等）
2) 底部：一个 ```json ``` 围栏代码块（给「全部添加为任务」等程序化消费）

训练自定义 1B 模型（P2）时，这些模板即微调语料骨架。
"""
import json


def decompose_prompt(goal, context=None):
    system = (
        "你是一个任务拆解助手。用户给出一个大目标，请将其拆分成有序、可执行的子任务。\n"
        "输出分两部分：\n"
        "\n"
        "【第一部分：Markdown 展示文本】\n"
        "  - 顶部写一个二级标题 `## 目标：…`，后跟一句话摘要；\n"
        "  - 然后用**有序列表** 1. 2. 3. … 列出每个步骤；\n"
        "  - 列表项格式：`**<标题>**`（预估 X 分钟）——一两句说明\n"
        "  - 如该步骤有前置依赖，行尾加 *（依赖 #1, #3）*\n"
        "\n"
        "【第二部分：JSON 结构化数据】\n"
        "  最后用一个 ```json ``` 围栏包含对象，结构：\n"
        '  {"steps":[{"title":"","note":"","estimated_minutes":0,"depends_on":[]}]}\n'
        "  - title：简短动作标题\n"
        "  - note：一两句说明\n"
        "  - estimated_minutes：整数分钟\n"
        "  - depends_on：前置依赖的步骤索引数组（从 0 开始；无依赖为 []，必须无环）\n"
        "不要在 JSON 围栏之外再写任何结束语。\n"
    )
    user = f"目标：{goal}\n"
    user += f"补充上下文：{context}\n" if context else "补充上下文：无\n"
    user += "请先输出 Markdown，再输出 JSON 围栏："
    return system, user


def extract_prompt(text):
    system = (
        "你从用户粘贴的网页、聊天或文档片段中提取【核心待办事项】。\n"
        "输出分两部分：\n"
        "\n"
        "【第一部分：Markdown 展示文本】\n"
        "  - 顶部二级标题 `## 提取到的待办`；\n"
        "  - 用**无序列表**逐项列出；\n"
        "  - 格式：`**<动作标题>**` — 备注说明（含时间/链接/人员等关键信息）\n"
        "\n"
        "【第二部分：JSON 结构化数据】\n"
        "  最后用一个 ```json ``` 围栏包含对象，结构：\n"
        '  {"items":[{"title":"","note":""}]}\n'
        "要求：过滤寒暄、广告、重复、与行动无关的内容；title 必须是动作性的。\n"
        "不要在 JSON 围栏之外再写任何结束语。\n"
    )
    return system, f"待提取文本：\n{text}\n\n请先输出 Markdown，再输出 JSON 围栏："


def reprioritize_prompt(tasks):
    task_ids = [str(t["id"]) for t in tasks] if isinstance(tasks, list) else []
    task_id_list = "、".join(task_ids) if task_ids else "（输入列表中的 id）"
    system = (
        "你是动态优先级推演引擎。综合以下信号重新排序任务：\n"
        "- due_date（截止时间，越近越优先）\n"
        "- estimated_minutes（工作量，越大越占资源）\n"
        "- 历史完成速度（来自 completion 信息，单位分钟/任务）\n"
        "\n"
        "❗ 重要约束：\n"
        f"  输入任务的真实 id = {task_id_list}。\n"
        "  JSON 中所有 id（ordered[i].id / postpone / drop）必须严格使用以上真实 id，\n"
        "  绝对禁止替换为数组下标 0/1/2。\n"
        "  ordered 中的 title 字段必须保留输入任务的原标题（展示和 JSON 一致性）。\n"
        "\n"
        "输出分两部分：\n"
        "\n"
        "【第一部分：Markdown 展示文本】\n"
        "  - 二级标题 `## 建议顺序`，随后有序列表，每一行：`任务名` ⚑高/中/低 — 理由\n"
        "  - 若存在可延后项，追加二级标题 `## 📌 可延后` 后用无序列表；\n"
        "  - 若存在可舍弃项，追加二级标题 `## 🗑 可舍弃` 后用无序列表；\n"
        "  - 三项均无则写 `当前任务池无需调整。`\n"
        "  - 展示 Markdown 时必须使用真实任务标题，不要输出「任务 #数字」。\n"
        "\n"
        "【第二部分：JSON 结构化数据】\n"
        "  最后用一个 ```json ``` 围栏包含对象，结构：\n"
        '  {"ordered":[{"id":<真实id>,"predicted_priority":0.0,"reason":"","title":"原标题"}],"postpone":[<真实id>],"drop":[<真实id>]}\n'
        "其中 predicted_priority 为 0~1（越高越优先），postpone/drop 为真实任务 id 列表。\n"
        "不要在 JSON 围栏之外再写任何结束语。\n"
    )
    return system, f"任务列表：\n{json.dumps(tasks, ensure_ascii=False)}\n\n请先输出 Markdown，再输出 JSON 围栏（JSON 中的 id 必须严格使用输入任务的真实 id）："


def predict_prompt(events):
    system = (
        "你是待办预判助手。根据日历/会议事件自动衍生需要提前准备的待办。\n"
        "输出分两部分：\n"
        "\n"
        "【第一部分：Markdown 展示文本】\n"
        "  - 二级标题 `## 提前准备的待办`；\n"
        "  - 用**无序列表**；格式：`**<待办>**` 📅<截止日> ⚑高/中/低 — 备注\n"
        "\n"
        "【第二部分：JSON 结构化数据】\n"
        "  最后用一个 ```json ``` 围栏包含对象，结构：\n"
        '  {"todos":[{"title":"","note":"","priority":1,"due":"YYYY-MM-DD"}]}\n'
        "priority：0=低 / 1=中 / 2=高；due 必须早于事件当天（YYYY-MM-DD）。\n"
        "不要在 JSON 围栏之外再写任何结束语。\n"
    )
    return system, f"事件列表：\n{json.dumps(events, ensure_ascii=False)}\n\n请先输出 Markdown，再输出 JSON 围栏："
