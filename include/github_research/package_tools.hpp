#pragma once

// Package Registry MCP 工具集(5 个 pkg_* 工具)
// 架构:纯 HTTP REST
//   - npm:  registry.npmjs.org API (JSON)
//   - pypi: pypi.org API (JSON)
// 零浏览器依赖,启动即用

#include <string>
#include <nlohmann/json.hpp>

namespace github_research {

using json = nlohmann::json;

json ToolPkgSearchNpm(const json& args);
json ToolPkgGetNpmDetail(const json& args);
json ToolPkgSearchPypi(const json& args);
json ToolPkgGetPypiDetail(const json& args);

// 分层工具: 缓存 + entity_mapper (cache_key=pkg:{registry}:{name}, TTL=24h)
// entity: package 实体 + depends_on/derived_from 关系
json ToolPkgFetchDetail(const json& args);

} // namespace github_research
