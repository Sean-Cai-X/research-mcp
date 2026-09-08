#pragma once

#include <string>
#include <nlohmann/json.hpp>
#include "github_research/browser_session.hpp"

namespace github_research {

using json = nlohmann::json;

json ToolPwcSearchPapers(IBrowserSession& session, const json& args);
json ToolPwcGetPaperDetail(IBrowserSession& session, const json& args);
json ToolPwcGetSota(IBrowserSession& session, const json& args);
json ToolPwcSearchTasks(IBrowserSession& session, const json& args);
json ToolPwcSearchDatasets(IBrowserSession& session, const json& args);

// 分层工具: 缓存 + entity_mapper (cache_key=pwc:{paper_id}, TTL=72h)
// entity: paper 实体 + evaluated_on(task) 关系
json ToolPwcFetchPaperDetail(IBrowserSession& session, const json& args);

} // namespace github_research
