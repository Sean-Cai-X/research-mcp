#pragma once

// Stack Overflow MCP 工具集(6 个 so_* 工具)
// 架构:纯 HTTP REST (api.stackexchange.com/2.3/)
// 零浏览器依赖,启动即用

#include <string>
#include <nlohmann/json.hpp>

namespace github_research {

using json = nlohmann::json;

json ToolSoSearchQuestions(const json& args);
json ToolSoGetQuestionDetail(const json& args);
json ToolSoGetTopAnswers(const json& args);
json ToolSoSearchByTags(const json& args);
json ToolSoGetSimilar(const json& args);

// 分层工具: 缓存 + entity_mapper
//   cache_key=so:question:{id}, TTL=24h, entity=question + answered_by 关系
json ToolSoFetchQuestionDetail(const json& args);

} // namespace github_research
