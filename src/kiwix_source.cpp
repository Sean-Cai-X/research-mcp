#include "github_research/kiwix_source.hpp"
#include "github_research/string_utils.hpp"
#include <algorithm>
#include <cctype>
#include <set>
#include <sstream>
#include <vector>
namespace github_research {
namespace {
std::string toLowerStr(const std::string& s) {
    std::string r;
    r.reserve(s.size());
    for (char c : s) r += static_cast<char>(tolower(static_cast<unsigned char>(c)));
    return r;
}
std::string resolveUrl(const std::string& url, const std::string& base_url) {
    if (url.empty()) return url;
    if (url.compare(0, 7, "http://") == 0 || url.compare(0, 8, "https://") == 0) return url;
    if (url.size() >= 2 && url[0] == '/' && url[1] == '/') {
        if (base_url.compare(0, 5, "http:") == 0) return "http:" + url;
        return "https:" + url;
    }
    size_t scheme_end = base_url.find("://");
    if (scheme_end == std::string::npos) return base_url + "/" + url;
    size_t host_start = scheme_end + 3;
    size_t host_end = base_url.find('/', host_start);
    std::string scheme_host = (host_end == std::string::npos) ? base_url : base_url.substr(0, host_end);
    if (url[0] == '/') return scheme_host + url;
    if (host_end == std::string::npos) return scheme_host + "/" + url;
    std::string base_path = base_url.substr(host_end);
    size_t q = base_path.find('?');
    if (q != std::string::npos) base_path = base_path.substr(0, q);
    size_t f = base_path.find('#');
    if (f != std::string::npos) base_path = base_path.substr(0, f);
    size_t last_slash = base_path.find_last_of('/');
    if (last_slash != std::string::npos) base_path = base_path.substr(0, last_slash + 1);
    else base_path = "/";
    std::string u = url;
    if (u.size() >= 2 && u[0] == '.' && u[1] == '/') u = u.substr(2);
    return scheme_host + base_path + u;
}
std::string extractTitle(const std::string& html) {
    if (html.empty()) return "";
    std::string lower = toLowerStr(html);
    size_t start = lower.find("<title");
    if (start == std::string::npos) return "";
    size_t tag_end = html.find('>', start);
    if (tag_end == std::string::npos) return "";
    size_t close = lower.find("</title>", tag_end + 1);
    if (close == std::string::npos) return "";
    std::string title = html.substr(tag_end + 1, close - tag_end - 1);
    size_t s = title.find_first_not_of(" \t\r\n");
    size_t e = title.find_last_not_of(" \t\r\n");
    if (s == std::string::npos) return "";
    return title.substr(s, e - s + 1);
}
}
KiwixSource::KiwixSource(std::string base_url, IHttpClient* http_client)
    : base_url_(std::move(base_url)), http_client_(http_client) {}
bool KiwixSource::healthCheck() {
    if (health_checked_) return healthy_;
    health_checked_ = true;
    if (!http_client_ || !http_client_->is_ready()) { healthy_ = false; return false; }
    HttpResponse resp = http_client_->get(base_url_ + "/");
    healthy_ = (resp.status_code == 200);
    return healthy_;
}

std::vector<SearchResult> KiwixSource::search(const SearchQuery& query) {
    std::vector<SearchResult> results;
    if (!http_client_ || !http_client_->is_ready()) return results;
    if (query.query.empty()) return results;

    std::string url = base_url_ + "/search?pattern=" + url_encode(query.query) + "&format=json";
    HttpResponse resp = http_client_->get(url);
    if (resp.status_code != 200 || resp.body.empty()) return results;

    try {
        json j = json::parse(resp.body);
        json items;
        if (j.is_array()) {
            items = j;
        } else if (j.is_object()) {
            if (j.contains("results") && j["results"].is_array()) items = j["results"];
            else if (j.contains("items") && j["items"].is_array()) items = j["items"];
            else if (j.contains("data") && j["data"].is_array()) items = j["data"];
        }

        int count = 0;
        for (auto& item : items) {
            if (!item.is_object()) continue;
            if (count >= query.max_results) break;

            SearchResult sr;
            sr.source_id = sourceId();
            sr.resource_kind = ResourceKind::KIWIX_ARTICLE;
            sr.title = item.value("title", "");
            sr.snippet = item.value("snippet", "");

            std::string zim_id = item.value("zim_id", "");
            if (zim_id.empty()) zim_id = item.value("content_id", "");
            std::string article_path = item.value("path", "");

            if (zim_id.empty() || article_path.empty()) {
                std::string item_url = item.value("url", "");
                if (!item_url.empty()) {
                    static const std::string marker = "/content/";
                    size_t cpos = item_url.find(marker);
                    if (cpos != std::string::npos) {
                        std::string rest = item_url.substr(cpos + marker.size());
                        size_t slash = rest.find('/');
                        if (slash != std::string::npos) {
                            if (zim_id.empty()) zim_id = rest.substr(0, slash);
                            if (article_path.empty()) article_path = rest.substr(slash + 1);
                        } else {
                            if (zim_id.empty()) zim_id = rest;
                        }
                    }
                }
            }

            if (zim_id.empty() || article_path.empty()) continue;
            sr.canonical_uri = "kiwix://" + zim_id + "/" + article_path;
            results.push_back(sr);
            count++;
        }
        if (!results.empty()) return results;
    } catch (...) {
    }

    auto links = extractLinks(resp.body, base_url_);
    std::set<std::string> seen;
    int count = 0;
    static const std::string marker = "/content/";
    for (auto& link : links) {
        if (count >= query.max_results) break;
        size_t cpos = link.find(marker);
        if (cpos == std::string::npos) continue;
        std::string rest = link.substr(cpos + marker.size());
        size_t slash = rest.find('/');
        if (slash == std::string::npos) continue;
        std::string zim_id = rest.substr(0, slash);
        std::string article_path = rest.substr(slash + 1);
        if (article_path.empty()) continue;
        size_t q = article_path.find('?');
        if (q != std::string::npos) article_path = article_path.substr(0, q);
        size_t f = article_path.find('#');
        if (f != std::string::npos) article_path = article_path.substr(0, f);
        if (article_path.empty()) continue;

        std::string canonical = "kiwix://" + zim_id + "/" + article_path;
        if (seen.insert(canonical).second) {
            SearchResult sr;
            sr.canonical_uri = canonical;
            sr.resource_kind = ResourceKind::KIWIX_ARTICLE;
            sr.source_id = sourceId();
            sr.title = article_path;
            results.push_back(sr);
            count++;
        }
    }
    return results;
}

std::optional<FetchResult> KiwixSource::fetch(const std::string& canonical_uri) {
    if (!http_client_ || !http_client_->is_ready()) return std::nullopt;

    static const std::string scheme = "kiwix://";
    if (canonical_uri.compare(0, scheme.size(), scheme) != 0) return std::nullopt;
    std::string uri_path = canonical_uri.substr(scheme.size());

    size_t slash = uri_path.find('/');
    if (slash == std::string::npos) return std::nullopt;
    std::string zim_id = uri_path.substr(0, slash);
    std::string article_path = uri_path.substr(slash + 1);
    if (zim_id.empty() || article_path.empty()) return std::nullopt;

    std::string url = base_url_ + "/content/" + zim_id + "/" + article_path;
    HttpResponse resp = http_client_->get(url);
    if (resp.status_code != 200 || resp.body.empty()) return std::nullopt;

    FetchResult fr;
    fr.canonical_uri = canonical_uri;
    fr.resource_kind = ResourceKind::KIWIX_ARTICLE;
    fr.source_id = sourceId();
    fr.content_markdown = htmlToMarkdown(resp.body);
    fr.title = extractTitle(resp.body);
    if (fr.title.empty()) fr.title = article_path;
    return fr;
}

std::vector<std::string> KiwixSource::expand(const std::string& root_uri,
                                             const std::string& sub_path,
                                             int max_depth) {
    (void)sub_path;
    std::vector<std::string> result;
    if (max_depth <= 0) return result;
    if (!http_client_ || !http_client_->is_ready()) return result;

    static const std::string scheme = "kiwix://";
    if (root_uri.compare(0, scheme.size(), scheme) != 0) return result;
    std::string uri_path = root_uri.substr(scheme.size());

    size_t slash = uri_path.find('/');
    if (slash == std::string::npos) return result;
    std::string zim_id = uri_path.substr(0, slash);
    std::string article_path = uri_path.substr(slash + 1);
    if (zim_id.empty() || article_path.empty()) return result;

    std::string article_url = base_url_ + "/content/" + zim_id + "/" + article_path;
    HttpResponse resp = http_client_->get(article_url);
    if (resp.status_code != 200 || resp.body.empty()) return result;

    auto links = extractLinks(resp.body, article_url);
    std::string content_prefix = "/content/" + zim_id + "/";
    std::set<std::string> seen;

    for (auto& link : links) {
        size_t cpos = link.find(content_prefix);
        if (cpos == std::string::npos) continue;
        std::string linked_path = link.substr(cpos + content_prefix.size());
        if (linked_path.empty()) continue;
        size_t q = linked_path.find('?');
        if (q != std::string::npos) linked_path = linked_path.substr(0, q);
        size_t f = linked_path.find('#');
        if (f != std::string::npos) linked_path = linked_path.substr(0, f);
        if (linked_path.empty()) continue;

        std::string canonical = "kiwix://" + zim_id + "/" + linked_path;
        if (seen.insert(canonical).second) {
            result.push_back(canonical);
            if (static_cast<int>(result.size()) >= 50) break;
        }
    }
    return result;
}

std::string KiwixSource::htmlToMarkdown(const std::string& html) {
    if (html.empty()) return "";

    auto to_lower_str = [](const std::string& s2) {
        std::string r;
        r.reserve(s2.size());
        for (char c : s2) r += static_cast<char>(tolower(static_cast<unsigned char>(c)));
        return r;
    };

    std::string lower = to_lower_str(html);
    std::string s;
    s.reserve(html.size());
    {
        size_t i = 0;
        while (i < html.size()) {
            if (lower[i] == '<') {
                if (i + 7 <= lower.size() && lower.compare(i, 7, "<script") == 0) {
                    size_t end = lower.find("</script>", i);
                    if (end == std::string::npos) { i = html.size(); break; }
                    i = end + 9;
                    continue;
                }
                if (i + 6 <= lower.size() && lower.compare(i, 6, "<style") == 0) {
                    size_t end = lower.find("</style>", i);
                    if (end == std::string::npos) { i = html.size(); break; }
                    i = end + 8;
                    continue;
                }
            }
            s += html[i];
            i++;
        }
    }

    lower = to_lower_str(s);

    std::string out;
    out.reserve(s.size());
    size_t i = 0;
    while (i < s.size()) {
        if (s[i] != '<') { out += s[i]; i++; continue; }
        size_t tag_end = s.find('>', i);
        if (tag_end == std::string::npos) { out += s.substr(i); break; }
        std::string tag = s.substr(i, tag_end - i + 1);
        std::string tag_l = lower.substr(i, tag_end - i + 1);

        if (tag_l.size() >= 3 && tag_l.compare(0, 2, "<a") == 0 &&
            (tag_l[2] == ' ' || tag_l[2] == '\t' || tag_l[2] == '\n' ||
             tag_l[2] == '\r' || tag_l[2] == '>')) {
            std::string href;
            size_t hp = tag_l.find("href");
            if (hp != std::string::npos) {
                size_t eq = tag_l.find('=', hp);
                if (eq != std::string::npos) {
                    size_t p = eq + 1;
                    while (p < tag_l.size() && (tag_l[p] == ' ' || tag_l[p] == '\t')) p++;
                    if (p < tag_l.size() && (tag_l[p] == '"' || tag_l[p] == '\'')) {
                        char q = tag_l[p];
                        size_t qe = tag_l.find(q, p + 1);
                        if (qe != std::string::npos) href = tag.substr(p + 1, qe - p - 1);
                    } else if (p < tag_l.size()) {
                        size_t p2 = p;
                        while (p2 < tag_l.size() && tag_l[p2] != ' ' && tag_l[p2] != '>') p2++;
                        href = tag.substr(p, p2 - p);
                    }
                }
            }
            size_t close = lower.find("</a>", tag_end + 1);
            if (close != std::string::npos) {
                std::string link_text = s.substr(tag_end + 1, close - tag_end - 1);
                std::string clean_text;
                for (size_t k = 0; k < link_text.size();) {
                    if (link_text[k] == '<') {
                        size_t te = link_text.find('>', k);
                        if (te == std::string::npos) break;
                        k = te + 1;
                    } else { clean_text += link_text[k]; k++; }
                }
                size_t ts = clean_text.find_first_not_of(" \t\r\n");
                size_t te2 = clean_text.find_last_not_of(" \t\r\n");
                if (ts != std::string::npos) clean_text = clean_text.substr(ts, te2 - ts + 1);
                else clean_text = "";
                if (href.empty()) out += clean_text;
                else out += "[" + clean_text + "](" + href + ")";
                i = close + 4;
                continue;
            }
            i = tag_end + 1;
            continue;
        }

        if (tag_l.size() >= 3 && tag_l[1] == 'h' && tag_l[2] >= '1' && tag_l[2] <= '6' &&
            (tag_l.size() == 3 || tag_l[3] == '>' || tag_l[3] == ' ' ||
             tag_l[3] == '\t' || tag_l[3] == '\n' || tag_l[3] == '\r')) {
            int level = tag_l[2] - '0';
            out += "\n";
            for (int k = 0; k < level; k++) out += '#';
            out += ' ';
            i = tag_end + 1;
            continue;
        }
        if (tag_l.size() >= 4 && tag_l[1] == '/' && tag_l[2] == 'h' &&
            tag_l[3] >= '1' && tag_l[3] <= '6') {
            out += "\n"; i = tag_end + 1; continue;
        }
        if (tag_l.size() >= 2 && tag_l.compare(0, 2, "<p") == 0 &&
            (tag_l.size() == 2 || tag_l[2] == '>' || tag_l[2] == ' ' ||
             tag_l[2] == '\t' || tag_l[2] == '\n' || tag_l[2] == '\r')) {
            out += "\n\n"; i = tag_end + 1; continue;
        }
        if (tag_l.size() >= 3 && tag_l.compare(0, 3, "</p") == 0) {
            out += "\n"; i = tag_end + 1; continue;
        }
        if (tag_l.size() >= 3 && tag_l.compare(0, 3, "<br") == 0) {
            out += "\n"; i = tag_end + 1; continue;
        }
        if (tag_l.size() >= 3 && tag_l.compare(0, 3, "<li") == 0 &&
            (tag_l.size() == 3 || tag_l[3] == '>' || tag_l[3] == ' ' ||
             tag_l[3] == '\t' || tag_l[3] == '\n' || tag_l[3] == '\r')) {
            out += "\n- "; i = tag_end + 1; continue;
        }
        if ((tag_l.size() >= 3 && tag_l.compare(0, 3, "<ul") == 0 &&
             (tag_l.size() == 3 || tag_l[3] == '>' || tag_l[3] == ' ' ||
              tag_l[3] == '\t' || tag_l[3] == '\n' || tag_l[3] == '\r')) ||
            (tag_l.size() >= 3 && tag_l.compare(0, 3, "<ol") == 0 &&
             (tag_l.size() == 3 || tag_l[3] == '>' || tag_l[3] == ' ' ||
              tag_l[3] == '\t' || tag_l[3] == '\n' || tag_l[3] == '\r'))) {
            out += "\n"; i = tag_end + 1; continue;
        }
        if ((tag_l.size() >= 4 && tag_l.compare(0, 4, "</ul") == 0) ||
            (tag_l.size() >= 4 && tag_l.compare(0, 4, "</ol") == 0)) {
            out += "\n"; i = tag_end + 1; continue;
        }

        i = tag_end + 1;
    }

    auto replace_all = [](std::string& str, const std::string& from, const std::string& to) {
        if (from.empty()) return;
        size_t pos = 0;
        while ((pos = str.find(from, pos)) != std::string::npos) {
            str.replace(pos, from.size(), to);
            pos += to.size();
        }
    };
    replace_all(out, "&amp;", "&");
    replace_all(out, "&lt;", "<");
    replace_all(out, "&gt;", ">");
    replace_all(out, "&quot;", "\"");
    replace_all(out, "&#39;", "'");
    replace_all(out, "&nbsp;", " ");

    std::string final_out;
    final_out.reserve(out.size());
    int blank = 0;
    for (char c : out) {
        if (c == '\n') { blank++; if (blank <= 2) final_out += c; }
        else if (c == '\r') {}
        else { blank = 0; final_out += c; }
    }
    size_t start = final_out.find_first_not_of(" \t\n\r");
    size_t end = final_out.find_last_not_of(" \t\n\r");
    if (start == std::string::npos) return "";
    return final_out.substr(start, end - start + 1);
}

std::vector<std::string> KiwixSource::extractLinks(const std::string& html,
                                                    const std::string& base_url) {
    std::vector<std::string> links;
    if (html.empty()) return links;

    std::string lower = toLowerStr(html);
    size_t pos = 0;
    while ((pos = lower.find("href", pos)) != std::string::npos) {
        pos += 4;
        while (pos < lower.size() && (lower[pos] == ' ' || lower[pos] == '\t')) pos++;
        if (pos >= lower.size() || lower[pos] != '=') continue;
        pos++;
        while (pos < lower.size() && (lower[pos] == ' ' || lower[pos] == '\t')) pos++;
        if (pos >= html.size()) break;

        std::string href;
        if (html[pos] == '"' || html[pos] == '\'') {
            char quote = html[pos];
            size_t end = html.find(quote, pos + 1);
            if (end == std::string::npos) break;
            href = html.substr(pos + 1, end - pos - 1);
            pos = end + 1;
        } else {
            size_t start = pos;
            while (pos < html.size() && html[pos] != ' ' && html[pos] != '>' &&
                   html[pos] != '\t' && html[pos] != '\n' && html[pos] != '\r') pos++;
            href = html.substr(start, pos - start);
        }
        if (!href.empty()) links.push_back(resolveUrl(href, base_url));
    }
    return links;
}

} // namespace github_research
