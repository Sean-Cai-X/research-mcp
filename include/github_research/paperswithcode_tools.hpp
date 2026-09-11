#pragma once

// Papers with Code MCP 工具集(6 个 pwc_* 工具)
// 架构:纯 HTTP REST (paperswithcode.com/api/v1/)
// 零浏览器依赖,启动即用

#include <string>
#include <nlohmann/json.hpp>

namespace github_research {

using json = nlohmann::json;

json ToolPwcSearchPapers(const json& args);
json ToolPwcGetPaperDetail(const json& args);
json ToolPwcGetSota(const json& args);
json ToolPwcSearchTasks(const json& args);
json ToolPwcSearchDatasets(const json& args);

// 分层工具: 缓存 + entity_mapper (cache_key=pwc:{paper_id}, TTL=72h)
// entity: paper 实体 + evaluated_on(task) 关系
json ToolPwcFetchPaperDetail(const json& args);

} // namespace github_research
