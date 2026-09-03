import json, urllib.request

BASE = "http://127.0.0.1:8770/mcp"

def call_tool(name, params, rid=1, timeout=60):
    body = json.dumps({
        "jsonrpc": "2.0", "id": rid,
        "method": "tools/call",
        "params": {"name": name, "arguments": params}
    }).encode('utf-8')
    req = urllib.request.Request(BASE, data=body, headers={"Content-Type":"application/json"}, method="POST")
    with urllib.request.urlopen(req, timeout=timeout) as resp:
        return json.loads(resp.read().decode('utf-8'))

def get_text(obj):
    content = obj.get("result", {}).get("content") or []
    for c in content:
        if c.get("type") == "text":
            return c["text"]
    return None

# Test 1: 原始失败场景 - branch="cxcore-integration" (实际是 codex/cxcore-integration)
print("=== Test 1: branch='cxcore-integration' (需要后缀模糊匹配) ===")
r = call_tool("github_get_tree", {"owner":"Sean-Cai-X","repo":"cxvision","branch":"cxcore-integration","max_depth":3}, rid=1, timeout=60)
text = get_text(r) or ""
isError = r.get("result", {}).get("isError", False)
if "error" in text.lower() or isError:
    print(f"  FAIL: isError={isError}, text={text[:300]}")
else:
    # 成功了,看看树内容
    lines = text.strip().split('\n')
    print(f"  OK! tree lines={len(lines)}")
    for l in lines[:20]:
        print(f"    {l[:100]}")
    if len(lines) > 20:
        print(f"    ... ({len(lines)-20} more lines)")

# Test 2: 正确分支名
print("\n=== Test 2: branch='codex/cxcore-integration' (精确匹配) ===")
r2 = call_tool("github_get_tree", {"owner":"Sean-Cai-X","repo":"cxvision","branch":"codex/cxcore-integration","max_depth":3}, rid=2, timeout=60)
text2 = get_text(r2) or ""
isError2 = r2.get("result", {}).get("isError", False)
if "error" in text2.lower() or isError2:
    print(f"  FAIL: isError={isError2}, text={text2[:300]}")
else:
    lines2 = text2.strip().split('\n')
    print(f"  OK! tree lines={len(lines2)}")
    for l in lines2[:10]:
        print(f"    {l[:100]}")

# Test 3: master 分支
print("\n=== Test 3: branch='master' ===")
r3 = call_tool("github_get_tree", {"owner":"Sean-Cai-X","repo":"cxvision","branch":"master","max_depth":2}, rid=3, timeout=60)
text3 = get_text(r3) or ""
isError3 = r3.get("result", {}).get("isError", False)
if "error" in text3.lower() or isError3:
    print(f"  FAIL: isError={isError3}, text={text3[:300]}")
else:
    lines3 = text3.strip().split('\n')
    print(f"  OK! tree lines={len(lines3)}")
    for l in lines3[:10]:
        print(f"    {l[:100]}")

# Test 4: 不存在的分支(验证错误信息改善)
print("\n=== Test 4: branch='nonexistent-branch-xyz' (验证错误信息) ===")
r4 = call_tool("github_get_tree", {"owner":"Sean-Cai-X","repo":"cxvision","branch":"nonexistent-branch-xyz","max_depth":3}, rid=4, timeout=60)
text4 = get_text(r4) or ""
isError4 = r4.get("result", {}).get("isError", False)
print(f"  isError={isError4}")
print(f"  text={text4[:500]}")
