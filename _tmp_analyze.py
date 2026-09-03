import json, re

FILE = r'D:\2026-08-20_03-59-27_conv_c13e9529__github_sean_cai_x_c.jsonl'

with open(FILE, encoding='utf-8') as f:
    lines = f.readlines()

# 详细打印每行
for idx, line in enumerate(lines):
    if not line.strip(): continue
    obj = json.loads(line)
    t = obj.get('type', '?')
    msg = obj.get('message') if t == 'message' else None
    if not msg:
        if t == 'session':
            print(f'=== line {idx}: SESSION ===')
            print(f'  name={obj.get("name")} harness={obj.get("harness")}')
            print(f'  reasoningEffort={obj.get("reasoningEffort")}')
        continue
    role = msg.get('role', '?')
    print(f'\n=== line {idx}: role={role} ===')
    content = msg.get('content', '')
    if content:
        print(f'  content: {content[:2000]}')
    tc = msg.get('toolCalls') or msg.get('modelToolCalls') or []
    if tc:
        print(f'  toolCalls ({len(tc)}):')
        for i, t2 in enumerate(tc):
            fn = t2.get('function', {}) if isinstance(t2, dict) else {}
            name = fn.get('name', '?')
            args = fn.get('arguments', '{}')
            print(f'    [{i}] {name}: {args[:500]}')
    tr = msg.get('modelToolResults') or []
    if tr:
        print(f'  toolResults ({len(tr)}):')
        for i, r in enumerate(tr):
            print(f'    [{i}] {json.dumps(r, ensure_ascii=False)[:500]}')
