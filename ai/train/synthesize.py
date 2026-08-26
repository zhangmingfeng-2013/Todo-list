#!/usr/bin/env python3
"""以 prompts.py 为骨架，生成 4 类特征的指令微调语料（Alpaca 格式 JSONL）。

设计：
- 复用 ai/prompts.py 的 system/user 模板，保证与线上推理提示词一致。
- 用「领域剧本」(DOMAIN_PLAYBOOK) 实例化大量结构合理的拆解样本；
  用少量手工种子覆盖 抽取 / 重排 / 预判。
- 输出 ai/train/data/train.jsonl，每行：
  {"instruction": <system 提示>, "input": <user 内容>, "output": <模型应输出的 JSON 字符串>}

后续可用强模型（教师）对本语料做蒸馏扩充，或直接用真实历史 tasks 蒸馏。
"""
import json
import os
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.join(HERE, ".."))  # 使 `import prompts` 可用
from prompts import (  # noqa: E402
    decompose_prompt, extract_prompt, reprioritize_prompt, predict_prompt,
)

OUT_DIR = os.path.join(HERE, "data")
OUT_FILE = os.path.join(OUT_DIR, "train.jsonl")

# ---------- 拆解：领域剧本 ----------
# 每个领域一组步骤模板：(title, note, est_min, depends_on 索引列表)
DOMAIN_PLAYBOOK = {
    "备考": [
        ("整理笔记", "汇总各章节重点与公式", 120, []),
        ("刷选择题", "完成近三年真题选择题", 90, [0]),
        ("错题复盘", "重做并讲解错题", 60, [1]),
        ("做模拟卷", "全真计时模拟", 120, [2]),
        ("查漏补缺", "针对薄弱点专项练习", 80, [3]),
    ],
    "报告": [
        ("收集素材", "整理数据与访谈记录", 90, []),
        ("列大纲", "确定章节结构与论点", 60, [0]),
        ("写初稿", "完成全文草稿", 180, [1]),
        ("配图表", "制作关键图表", 90, [2]),
        ("校对润色", "通读并修正", 60, [3]),
    ],
    "出行": [
        ("定行程", "规划目的地与天数", 60, []),
        ("订机票", "比价并购买机票", 45, [0]),
        ("订住宿", "预订酒店", 45, [0]),
        ("做攻略", "整理景点与美食清单", 90, [1, 2]),
        ("打包", "按清单收拾行李", 60, [3]),
    ],
    "开发": [
        ("需求拆解", "明确功能点与验收标准", 90, []),
        ("搭骨架", "初始化工程与目录", 60, [0]),
        ("写核心逻辑", "实现主流程", 180, [1]),
        ("接测试", "补充单元测试", 120, [2]),
        ("联调部署", "集成并上线", 90, [3]),
    ],
    "健身": [
        ("评估体测", "记录体重与围度", 30, []),
        ("定计划", "安排每周训练分化", 45, [0]),
        ("力量训练", "完成推/拉/腿循环", 120, [1]),
        ("有氧", "跑步或骑行", 60, [1]),
        ("饮食控制", "规划高蛋白餐单", 45, [1]),
    ],
}

GOALS_BY_DOMAIN = {
    "备考": ["备考期末", "备考研究生考试", "备考雅思", "准备软考"],
    "报告": ["完成项目报告", "写季度复盘", "产出周报", "撰写技术方案"],
    "出行": ["计划日本旅行", "安排周末露营", "规划毕业旅行", "出差上海"],
    "开发": ["开发待办小程序", "做一个爬虫", "搭个人博客", "实现登录模块"],
    "健身": ["三个月减脂", "练出马甲线", "增肌计划", "备战半马"],
}


def gen_decompose():
    out = []
    for domain, goals in GOALS_BY_DOMAIN.items():
        playbook = DOMAIN_PLAYBOOK[domain]
        for goal in goals:
            steps = [
                {"title": t, "note": n, "estimated_minutes": e, "depends_on": d}
                for (t, n, e, d) in playbook
            ]
            sys_p, user_p = decompose_prompt(goal)
            out.append({
                "instruction": sys_p,
                "input": user_p,
                "output": json.dumps({"steps": steps}, ensure_ascii=False),
            })
    return out


# ---------- 抽取：手工种子 ----------
EXTRACT_SEEDS = [
    ("在吗？记得回复客户邮件关于报价，还有周五前提交周报，顺便把发票搞定",
     [{"title": "回复客户邮件", "note": "关于报价"},
      {"title": "提交周报", "note": "周五前"},
      {"title": "处理发票", "note": ""}]),
    ("【会议通知】明早 9 点评审，请提前准备演示账号与测试用例",
     [{"title": "准备演示账号", "note": "明早 9 点评审"},
      {"title": "准备测试用例", "note": "明早 9 点评审"}]),
    ("刚看了这篇文章 https://example.com/x 核心是缓存穿透，总结一下要点并加进知识库",
     [{"title": "总结缓存穿透要点", "note": "来源 https://example.com/x"},
      {"title": "更新知识库", "note": "缓存穿透"}]),
    ("提醒我买菜、取快递、给妈妈打电话",
     [{"title": "买菜", "note": ""}, {"title": "取快递", "note": ""},
      {"title": "给妈妈打电话", "note": ""}]),
]


def gen_extract():
    out = []
    for text, items in EXTRACT_SEEDS:
        sys_p, user_p = extract_prompt(text)
        out.append({
            "instruction": sys_p,
            "input": user_p,
            "output": json.dumps({"items": items}, ensure_ascii=False),
        })
    return out


# ---------- 重排：手工种子 ----------
REPRIOR_SEEDS = [
    ([{"id": 1, "title": "A", "due_date": "2026-08-27", "estimated_minutes": 30},
      {"id": 2, "title": "B", "due_date": "2026-09-01", "estimated_minutes": 200},
      {"id": 3, "title": "C", "due_date": "2026-12-01", "estimated_minutes": 10}],
     [1, 3, 2], [2], []),
    ([{"id": 10, "title": "X", "due_date": "2026-08-26", "estimated_minutes": 120},
      {"id": 11, "title": "Y", "due_date": "2026-08-30", "estimated_minutes": 60},
      {"id": 12, "title": "Z", "due_date": "2026-10-01", "estimated_minutes": 240}],
     [10, 11, 12], [12], []),
]


def gen_reprioritize():
    out = []
    for tasks, ordered, postpone, drop in REPRIOR_SEEDS:
        sys_p, user_p = reprioritize_prompt(tasks)
        result = {
            "ordered": [
                {"id": i, "predicted_priority": round(1 - idx / len(ordered), 2),
                 "reason": "截止更近且工作量适配" if idx == 0 else "按可用时间排序"}
                for idx, i in enumerate(ordered)
            ],
            "postpone": postpone,
            "drop": drop,
        }
        out.append({
            "instruction": sys_p,
            "input": user_p,
            "output": json.dumps(result, ensure_ascii=False),
        })
    return out


# ---------- 预判：手工种子 ----------
PREDICT_SEEDS = [
    ([{"title": "Q3 复盘会", "date": "2026-08-28"}],
     [{"title": "准备会议材料", "note": "Q3 复盘", "priority": 2, "due": "2026-08-27"}]),
    ([{"title": "客户演示", "date": "2026-09-05"}],
     [{"title": "准备演示环境", "note": "客户演示", "priority": 2, "due": "2026-09-04"},
      {"title": "演练讲解", "note": "客户演示", "priority": 1, "due": "2026-09-03"}]),
]


def gen_predict():
    out = []
    for events, todos in PREDICT_SEEDS:
        sys_p, user_p = predict_prompt(events)
        out.append({
            "instruction": sys_p,
            "input": user_p,
            "output": json.dumps({"todos": todos}, ensure_ascii=False),
        })
    return out


def main():
    os.makedirs(OUT_DIR, exist_ok=True)
    records = []
    records += gen_decompose()
    records += gen_extract()
    records += gen_reprioritize()
    records += gen_predict()

    with open(OUT_FILE, "w", encoding="utf-8") as f:
        for r in records:
            f.write(json.dumps(r, ensure_ascii=False) + "\n")

    # 校验
    n = 0
    with open(OUT_FILE, encoding="utf-8") as f:
        for line in f:
            obj = json.loads(line)
            assert {"instruction", "input", "output"} <= set(obj), "格式缺失"
            json.loads(obj["output"])  # 确保 output 本身是可解析 JSON
            n += 1
    print(f"[synthesize] 已生成 {n} 条语料 -> {OUT_FILE}")
    print(f"  拆解 {len(gen_decompose())} | 抽取 {len(gen_extract())} | "
          f"重排 {len(gen_reprioritize())} | 预判 {len(gen_predict())}")


if __name__ == "__main__":
    main()
