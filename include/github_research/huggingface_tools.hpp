#pragma once

#include <string>
#include <nlohmann/json.hpp>
#include "github_research/browser_session.hpp"

namespace github_research {

using json = nlohmann::json;

json ToolHfSearchModels(IBrowserSession& session, const json& args);
json ToolHfGetModelInfo(IBrowserSession& session, const json& args);
json ToolHfGetModelReadme(IBrowserSession& session, const json& args);
json ToolHfSearchDatasets(IBrowserSession& session, const json& args);
json ToolHfGetDatasetInfo(IBrowserSession& session, const json& args);
json ToolHfGetTrendingModels(IBrowserSession& session, const json& args);
json ToolHfSearchSpaces(IBrowserSession& session, const json& args);

// 分层工具: 缓存 + entity_mapper
//   - ToolHfFetchModelDetail:    cache_key=hf:model:{id},       TTL=12h, entity=model + derived_from 关系
//   - ToolHfFetchDatasetDetail:  cache_key=hf:dataset:{id},     TTL=24h, entity=dataset
json ToolHfFetchModelDetail(IBrowserSession& session, const json& args);
json ToolHfFetchDatasetDetail(IBrowserSession& session, const json& args);

} // namespace github_research
