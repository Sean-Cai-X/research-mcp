#pragma once

// Hugging Face MCP 工具集(9 个 hf_* 工具)
// 架构:纯 HTTP REST (huggingface.co/api)
// 零浏览器依赖,启动即用

#include <string>
#include <nlohmann/json.hpp>

namespace github_research {

using json = nlohmann::json;

json ToolHfSearchModels(const json& args);
json ToolHfGetModelInfo(const json& args);
json ToolHfGetModelReadme(const json& args);
json ToolHfSearchDatasets(const json& args);
json ToolHfGetDatasetInfo(const json& args);
json ToolHfGetTrendingModels(const json& args);
json ToolHfSearchSpaces(const json& args);

// 分层工具: 缓存 + entity_mapper
//   - ToolHfFetchModelDetail:    cache_key=hf:model:{id},       TTL=12h, entity=model + derived_from 关系
//   - ToolHfFetchDatasetDetail:  cache_key=hf:dataset:{id},     TTL=24h, entity=dataset
json ToolHfFetchModelDetail(const json& args);
json ToolHfFetchDatasetDetail(const json& args);

} // namespace github_research
