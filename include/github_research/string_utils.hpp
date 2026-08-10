#pragma once

#include <string>
#include <map>
#include <vector>

namespace github_research {

// UTF-8 <-> UTF-16 转换(Windows COM API 需要)
std::wstring to_wstring(const std::string& utf8);
std::string to_utf8(const std::wstring& wide);

// URL 编码(application/x-www-form-urlencoded 风格,但保留 - _ . ~)
std::string url_encode(const std::string& value);

// 字符串小写转换
std::string to_lower(const std::string& s);

// 字符串比较(大小写不敏感)
bool iequals(const std::string& a, const std::string& b);

// 拼接 query string,params 已 URL 编码
std::string build_query(const std::map<std::string, std::string>& params);

// Base64 解码(用于 GitHub Contents API 返回的 base64 编码文件内容)
std::string base64_decode(const std::string& encoded);

// 从文本中提取所有 URL(http/https/ftp 协议)
// 返回去重后的 URL 列表,按首次出现顺序
std::vector<std::string> extract_urls(const std::string& text);

// URL 归一化(去 fragment、去尾斜杠、统一小写 scheme/host)
std::string normalize_url(const std::string& url);

// 判断 URL 是否为"低价值"过滤目标(HN 站内、短链、登录页等)
bool is_low_value_url(const std::string& url);

} // namespace github_research
