#pragma once

#include <nlohmann/json.hpp>
#include <string>

namespace github_research {

using json = nlohmann::json;

// ═════════════════════════════════════════════════════════════
//  定向知识雷达 — Focus MCP 工具集 (next.txt)
// ═════════════════════════════════════════════════════════════

// Focus 管理
json ToolFocusCreate(const json& args);
json ToolFocusList(const json& args);
json ToolFocusGet(const json& args);
json ToolFocusDelete(const json& args);

// 成员 & 缺口
json ToolFocusMembers(const json& args);
json ToolFocusGaps(const json& args);
json ToolFocusStats(const json& args);

// 实体属性查询
json ToolEntityAttrs(const json& args);

// 人工干预
json ToolFocusPrune(const json& args);
json ToolFocusPromote(const json& args);

} // namespace github_research
