#pragma once

#include <string>
#include <nlohmann/json.hpp>
#include "github_research/browser_session.hpp"

namespace github_research {

using json = nlohmann::json;

json ToolSoSearchQuestions(IBrowserSession& session, const json& args);
json ToolSoGetQuestionDetail(IBrowserSession& session, const json& args);
json ToolSoGetTopAnswers(IBrowserSession& session, const json& args);
json ToolSoSearchByTags(IBrowserSession& session, const json& args);
json ToolSoGetSimilar(IBrowserSession& session, const json& args);

// 分层工具: 缓存 + entity_mapper
//   cache_key=so:question:{id}, TTL=24h, entity=question + answered_by 关系
json ToolSoFetchQuestionDetail(IBrowserSession& session, const json& args);

} // namespace github_research
