"""四个智能特征的提示词模板。

每个函数返回 (system, user) 字符串。模型被强制要求只输出指定结构的 JSON，
以便 cpp-todo 侧稳定解析。训练自定义 1B 模型（P2）时，这些模板即微调语料骨架。
"""
import json


def decompose_prompt(goal, context=None):
    system = (
        "你是一个任务拆解助手。用户给出一个大目标，请将其拆分成有序、可执行的子任务。\n"
        "要求：\n"
        "1. 每个子任务含 title（简短动作标题）、note（一两句说明）、estimated_minutes（预估耗时，整数分钟）。\n"
        "2. depends_on 用【步骤索引数组】表示前置依赖，索引从 0 开始；无依赖则为空数组 []。\n"
        "3. 依赖必须无环：被依赖的步骤索引必须严格早于当前步骤。\n"
        "4. 只输出一个 JSON 对象，不要解释或 markdown 代码块，结构如下：\n"
        '{"steps":[{"title":"","note":"","estimated_minutes":0,"depends_on":[]}]}'
    )
    user = f"目标：{goal}\n"
    user += f"补充上下文：{context}\n" if context else "补充上下文：无\n"
    user += "请输出拆解 JSON："
    return system, user


def extract_prompt(text):
    system = (
        "你从用户粘贴的网页、聊天或文档片段中提取【核心待办事项】。\n"
        "要求：\n"
        "1. 过滤寒暄、广告、重复、与行动无关的内容。\n"
        "2. 每条待办含 title（动作性标题）和 note（关键背景/链接/时间等简要备注）。\n"
        "3. 只输出一个 JSON 对象，结构如下：\n"
        '{"items":[{"title":"","note":""}]}'
    )
    return system, f"待提取文本：\n{text}\n\n请输出提取 JSON："


def reprioritize_prompt(tasks):
    system = (
        "你是动态优先级推演引擎。综合以下信号重新排序任务：\n"
        "- due_date（截止时间，越近越优先）\n"
        "- estimated_minutes（工作量，越大越占资源）\n"
        "- 历史完成速度（来自 completion 信息，单位分钟/任务）\n"
        "要求：\n"
        "1. 输出 ordered 数组，按推荐执行顺序排列，含 id 与 predicted_priority（0~1 越高越优先）及简短 reason。\n"
        "2. 当任务明显超出可用时间，给出 postpone（可延后 id 列表）与 drop（可舍弃 id 列表）。\n"
        '输出结构：{"ordered":[{"id":0,"predicted_priority":0.0,"reason":""}],"postpone":[],"drop":[]}'
    )
    return system, f"任务列表：\n{json.dumps(tasks, ensure_ascii=False)}\n\n请输出推演 JSON："


def predict_prompt(events):
    system = (
        "你是待办预判助手。根据日历/会议事件自动衍生需要提前准备的待办。\n"
        "要求：\n"
        "1. 每条 todo 含 title、note、priority（0低/1中/2高）、due（应完成日期 YYYY-MM-DD，早于事件当天）。\n"
        "2. 只输出 JSON 对象，结构如下：\n"
        '{"todos":[{"title":"","note":"","priority":1,"due":"YYYY-MM-DD"}]}'
    )
    return system, f"事件列表：\n{json.dumps(events, ensure_ascii=False)}\n\n请输出预判 JSON："
