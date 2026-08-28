"""测试 _repair_json 修复逻辑。"""
import json
import re
import sys
sys.path.insert(0, '/Users/zhangmingfeng/Projects/TODO-list/ai')
from parseutil import _repair_json, _try_parse_json, extract_json

# 模拟模型实际输出的 JSON（缺少 { 的缺陷场景）
test_cases = [
    # 场景 A: 数字 → ] → } → ,"key (最常见的缺陷)
    ('{"steps":[{"title":"test","estimated_minutes":30,"depends_on":[0]},"title":"test2","estimated_minutes":15,"depends_on":[1]}]}',
     '{"steps":[{"title":"test","estimated_minutes":30,"depends_on":[0]},{"title":"test2","estimated_minutes":15,"depends_on":[1]}]}'),
    # 场景 B: 对象值 } → ,"key 缺少 {
    ('{"steps":[{"title":"a","meta":{"x":1}},"title":"b"}]}',
     '{"steps":[{"title":"a","meta":{"x":1}},{"title":"b"}]}'),
    # 场景 C: 尾部逗号
    ('{"steps":[{"title":"a",},]}',
     '{"steps":[{"title":"a"}]}'),
    # 正常 JSON (不应被破坏)
    ('{"steps":[{"title":"a","depends_on":[0]},{"title":"b","depends_on":[1]}]}',
     '{"steps":[{"title":"a","depends_on":[0]},{"title":"b","depends_on":[1]}]}'),
]

all_pass = True
for i, (raw, expected_valid) in enumerate(test_cases):
    print(f'=== 测试 {i+1} ===')
    print(f'原始: {raw[:120]}')
    try:
        json.loads(raw)
        direct_ok = True
        print('直接解析: ✓ 成功')
    except Exception as e:
        direct_ok = False
        print(f'直接解析: ✗ 失败 - {e}')

    repaired = _repair_json(raw)
    print(f'修复后: {repaired[:120]}')
    try:
        result = json.loads(repaired)
        print(f'修复后解析: ✓ 成功')
        # 如果原本就可以解析，检查是否被破坏
        if direct_ok:
            expected = json.loads(raw)
            if result == expected:
                print('  未破坏正常 JSON ✓')
            else:
                print('  ✗ 正常 JSON 被破坏了!')
                all_pass = False
    except Exception as e:
        print(f'修复后解析: ✗ 失败 - {e}')
        all_pass = False
    print()

# 测试 extract_json 完整流程
print("=== extract_json 完整流程测试 ===")
# 模拟包含 markdown + ```json 围栏的完整模型输出
full_output = """## 目标：准备一场公司年会
1. **制定年会策划方案**
2. **邀请嘉宾**

```json
{"steps":[{"title":"制定年会策划方案","estimated_minutes":30,"depends_on":[0]},"title":"邀请嘉宾","estimated_minutes":15,"depends_on":[1]}]}
```"""

# 测试 extract_json 完整流程
print("=== extract_json 完整流程测试 ===")
# 模拟模型输出 JSON 内容（含缺陷）
json_content = '{"steps":[{"title":"制定年会策划方案","estimated_minutes":30,"depends_on":[0]},"title":"邀请嘉宾","estimated_minutes":15,"depends_on":[1]}]}'
result = extract_json(json_content)
if result and isinstance(result, dict) and "steps" in result:
    print(f'extract_json: ✓ 成功, steps={len(result["steps"])} 个')
else:
    print(f'extract_json: ✗ 失败, result={result}')
    all_pass = False

print()
if all_pass:
    print("🎉 所有测试通过！")
else:
    print("❌ 部分测试失败")