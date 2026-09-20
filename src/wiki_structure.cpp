#include "github_research/wiki_structure.hpp"
#include "github_research/kiwix_source.hpp"
#include <algorithm>
#include <cctype>
#include <cmath>
#include <deque>
#include <regex>
#include <sstream>
#include <unordered_map>

namespace github_research {

WikiStructure::WikiStructure(DataSourceRegistry& registry)
    : registry_(registry) {}

// =============================================================
// 辅助: HTML / 链接提取
// =============================================================

std::optional<std::string> WikiStructure::fetchHtml(const std::string& canonical_uri) {
    auto* src = registry_.get("kiwix_local");
    if (!src) return std::nullopt;
    auto* kiwix = dynamic_cast<KiwixSource*>(src);
    if (!kiwix) return std::nullopt;
    return kiwix->fetchRawHtml(canonical_uri);
}

std::string WikiStructure::extractTitleFromHtml(const std::string& html) {
    // Try <h1 id="firstHeading"> (Wikipedia standard)
    size_t pos = html.find("id=\"firstHeading\"");
    if (pos != std::string::npos) {
        size_t tag_start = html.rfind("<h1", pos);
        if (tag_start != std::string::npos) {
            size_t body_start = html.find('>', tag_start);
            if (body_start != std::string::npos) {
                size_t body_end = html.find("</h1>", body_start);
                if (body_end != std::string::npos) {
                    std::string raw = html.substr(body_start + 1, body_end - body_start - 1);
                    // Strip inner tags like <span>, <i>
                    std::string clean;
                    clean.reserve(raw.size());
                    bool in_tag = false;
                    for (char c : raw) {
                        if (c == '<') in_tag = true;
                        else if (c == '>') { in_tag = false; continue; }
                        else if (!in_tag) clean += c;
                    }
                    if (!clean.empty()) return clean;
                }
            }
        }
    }
    // Fallback: <title>
    pos = html.find("<title>");
    if (pos != std::string::npos) {
        size_t end = html.find("</title>", pos);
        if (end != std::string::npos) {
            std::string t = html.substr(pos + 7, end - pos - 7);
            // Wikipedia title format: "XXX - Wikipedia" → strip suffix
            size_t dash = t.find(" - Wikipedia");
            if (dash != std::string::npos) t = t.substr(0, dash);
            else {
                dash = t.find(" - 维基百科");
                if (dash != std::string::npos) t = t.substr(0, dash);
            }
            if (!t.empty()) return t;
        }
    }
    return "";
}

std::vector<std::pair<std::string, std::string>> WikiStructure::extractWikiLinks(
    const std::string& html,
    const std::string& base_url,
    const std::string& content_prefix)
{
    std::vector<std::pair<std::string, std::string>> links;
    std::set<std::string> seen;

    // Find all <a href="..."> tags
    size_t pos = 0;
    while ((pos = html.find("<a ", pos)) != std::string::npos) {
        // Extract href
        size_t href_pos = html.find("href=\"", pos);
        if (href_pos == std::string::npos || href_pos > pos + 50) { pos++; continue; }
        size_t start = href_pos + 6;
        size_t end = html.find('"', start);
        if (end == std::string::npos) { pos++; continue; }
        std::string href = html.substr(start, end - start);

        // Must be a local content link
        size_t cpos = href.find(content_prefix);
        if (cpos == std::string::npos) { pos++; continue; }

        std::string linked_path = href.substr(cpos + content_prefix.size());
        size_t q = linked_path.find('?');
        if (q != std::string::npos) linked_path = linked_path.substr(0, q);
        size_t f = linked_path.find('#');
        if (f != std::string::npos) linked_path = linked_path.substr(0, f);
        if (linked_path.empty()) { pos++; continue; }

        // Extract link text (between > and </a>)
        size_t close_tag = html.find('>', end);
        if (close_tag == std::string::npos) { pos++; continue; }
        size_t close_a = html.find("</a>", close_tag);
        if (close_a == std::string::npos) { pos++; continue; }
        std::string link_text = html.substr(close_tag + 1, close_a - close_tag - 1);
        // Strip inner tags from text
        std::string clean_text;
        clean_text.reserve(link_text.size());
        bool in_tag = false;
        for (char c : link_text) {
            if (c == '<') in_tag = true;
            else if (c == '>') { in_tag = false; continue; }
            else if (!in_tag) clean_text += c;
        }

        if (seen.insert(linked_path).second) {
            links.emplace_back(linked_path, clean_text);
        }

        pos = close_a + 4;
    }

    return links;
}

bool WikiStructure::isCategoryPath(const std::string& path) {
    if (path.compare(0, strlen(CATEGORY_PREFIX_EN), CATEGORY_PREFIX_EN) == 0) return true;
    if (path.compare(0, strlen(CATEGORY_PREFIX_ZH), CATEGORY_PREFIX_ZH) == 0) return true;
    return false;
}

// =============================================================
// 分类树递归
// =============================================================

void WikiStructure::crawlCategory(const std::string& category_uri,
                                   const std::string& zim_id,
                                   int current_depth,
                                   int max_depth,
                                   CategoryGraph& out,
                                   std::set<std::string>& visited)
{
    if (current_depth > max_depth) return;
    if (!visited.insert(category_uri).second) return;

    out.max_depth_reached = std::max(out.max_depth_reached, current_depth);

    auto html_opt = fetchHtml(category_uri);
    if (!html_opt) return;
    const std::string& html = *html_opt;

    std::string title = extractTitleFromHtml(html);
    if (title.empty()) {
        // Derive from URI
        size_t last_slash = category_uri.rfind('/');
        if (last_slash != std::string::npos) {
            title = category_uri.substr(last_slash + 1);
        }
    }

    GraphNode cat_node;
    cat_node.canonical_uri = category_uri;
    cat_node.title = title;
    cat_node.node_type = "category";
    cat_node.depth = current_depth;
    out.nodes.push_back(cat_node);
    out.category_count++;

    // Build content_prefix for this ZIM
    std::string base_url = "";  // not needed for path extraction
    std::string content_prefix = "/content/" + zim_id + "/";

    auto wiki_links = extractWikiLinks(html, base_url, content_prefix);

    for (const auto& [linked_path, link_text] : wiki_links) {
        // Skip special pages (File:, Template:, Help:, Wikipedia:, Portal:, MediaWiki:, Draft:, Module:, Gadget:, TimedText:)
        static const std::vector<std::string> special_prefixes = {
            "File:", "Template:", "Help:", "Wikipedia:", "Portal:",
            "MediaWiki:", "Draft:", "Module:", "Gadget:", "TimedText:",
            "Image:", "Media:", "Special:", "Book:", "Index:"
        };
        bool skip = false;
        std::string lower_path = linked_path;
        for (auto& c : lower_path) c = static_cast<char>(tolower(static_cast<unsigned char>(c)));
        for (const auto& sp : special_prefixes) {
            if (lower_path.compare(0, sp.size(), sp) == 0) { skip = true; break; }
        }
        if (skip) continue;

        std::string child_uri = "kiwix://" + zim_id + "/" + linked_path;

        // Add is_a edge: child IS_A category
        GraphEdge edge;
        edge.from = child_uri;   // child
        edge.to = category_uri; // parent category
        edge.edge_type = "is_a";
        out.edges.push_back(edge);

        if (isCategoryPath(linked_path)) {
            // Recurse into subcategory
            crawlCategory(child_uri, zim_id, current_depth + 1, max_depth, out, visited);
        } else {
            // Article leaf
            if (visited.insert(child_uri).second) {
                GraphNode art_node;
                art_node.canonical_uri = child_uri;
                art_node.title = link_text.empty() ? linked_path : link_text;
                art_node.node_type = "article";
                art_node.depth = current_depth + 1;
                out.nodes.push_back(art_node);
                out.article_count++;
            }
        }
    }
}

CategoryGraph WikiStructure::categoryGraph(const std::string& root_category,
                                            const std::string& zim_id,
                                            int max_depth)
{
    CategoryGraph result;
    if (zim_id.empty() || root_category.empty()) return result;

    // Normalize root category path
    std::string cat_path = root_category;
    // Strip Category: prefix if user provided it
    if (cat_path.compare(0, strlen(CATEGORY_PREFIX_EN), CATEGORY_PREFIX_EN) == 0) {
        // keep as-is
    } else if (cat_path.compare(0, strlen(CATEGORY_PREFIX_ZH), CATEGORY_PREFIX_ZH) == 0) {
        // keep as-is
    } else {
        cat_path = std::string(CATEGORY_PREFIX_EN) + cat_path;
    }

    std::string root_uri = "kiwix://" + zim_id + "/" + cat_path;
    result.root_category = root_category;

    std::set<std::string> visited;
    crawlCategory(root_uri, zim_id, 0, max_depth, result, visited);

    return result;
}

// =============================================================
// 内部链接网络 + 核心度
// =============================================================

LinkGraph WikiStructure::linkGraph(const std::string& canonical_uri,
                                    int depth,
                                    int max_nodes)
{
    LinkGraph result;
    result.center_uri = canonical_uri;
    result.depth = depth;

    auto html_opt = fetchHtml(canonical_uri);
    if (!html_opt) return result;

    // Extract zim_id from canonical_uri
    std::string zim_id;
    static const std::string scheme = "kiwix://";
    if (canonical_uri.compare(0, scheme.size(), scheme) == 0) {
        std::string rest = canonical_uri.substr(scheme.size());
        size_t slash = rest.find('/');
        if (slash != std::string::npos) zim_id = rest.substr(0, slash);
    }
    if (zim_id.empty()) return result;

    std::string content_prefix = "/content/" + zim_id + "/";

    // BFS
    std::deque<std::pair<std::string, int>> queue;  // (uri, current_depth)
    std::set<std::string> visited;
    std::unordered_map<std::string, std::vector<std::string>> adj;  // uri -> links from it

    queue.push_back({canonical_uri, 0});
    visited.insert(canonical_uri);

    // Seed center node
    {
        GraphNode center;
        center.canonical_uri = canonical_uri;
        center.title = extractTitleFromHtml(*html_opt);
        center.node_type = "article";
        center.depth = 0;
        result.nodes.push_back(center);
    }

    while (!queue.empty() && static_cast<int>(result.nodes.size()) < max_nodes) {
        auto [current_uri, cur_depth] = queue.front();
        queue.pop_front();

        // Only expand if within depth limit and we have its HTML
        if (cur_depth >= depth) continue;

        std::string current_html;
        if (current_uri == canonical_uri) {
            current_html = *html_opt;
        } else {
            auto h = fetchHtml(current_uri);
            if (!h) continue;
            current_html = *h;
        }

        auto wiki_links = extractWikiLinks(current_html, "", content_prefix);

        for (const auto& [linked_path, link_text] : wiki_links) {
            // Skip special pages (same filter as category)
            static const std::vector<std::string> special_prefixes = {
                "file:", "template:", "help:", "wikipedia:", "portal:",
                "mediawiki:", "draft:", "module:", "gadget:", "timedtext:",
                "image:", "media:", "special:", "book:", "index:"
            };
            std::string lower_path = linked_path;
            for (auto& c : lower_path) c = static_cast<char>(tolower(static_cast<unsigned char>(c)));
            bool skip = false;
            for (const auto& sp : special_prefixes) {
                if (lower_path.compare(0, sp.size(), sp) == 0) { skip = true; break; }
            }
            if (skip) continue;

            std::string target_uri = "kiwix://" + zim_id + "/" + linked_path;

            // Add edge
            GraphEdge e;
            e.from = current_uri;
            e.to = target_uri;
            e.edge_type = "link";
            result.edges.push_back(e);
            adj[current_uri].push_back(target_uri);

            if (visited.insert(target_uri).second) {
                GraphNode n;
                n.canonical_uri = target_uri;
                n.title = link_text.empty() ? linked_path : link_text;
                n.node_type = isCategoryPath(linked_path) ? "category" : "article";
                n.depth = cur_depth + 1;
                result.nodes.push_back(n);
                queue.push_back({target_uri, cur_depth + 1});
            }

            if (static_cast<int>(result.nodes.size()) >= max_nodes) break;
        }
    }

    // Compute degrees
    std::unordered_map<std::string, int> in_deg, out_deg;
    for (const auto& e : result.edges) {
        out_deg[e.from]++;
        in_deg[e.to]++;
    }
    for (auto& n : result.nodes) {
        n.in_degree = in_deg.count(n.canonical_uri) ? in_deg[n.canonical_uri] : 0;
        n.out_degree = out_deg.count(n.canonical_uri) ? out_deg[n.canonical_uri] : 0;
    }

    // Core score = normalized in-degree + PageRank-lite
    computeCoreScores(result);

    return result;
}

void WikiStructure::computeCoreScores(LinkGraph& graph) {
    if (graph.nodes.empty()) return;
    const int N = static_cast<int>(graph.nodes.size());
    const double d = 0.85;  // damping factor
    const int ITER = 20;

    // Build node index map
    std::unordered_map<std::string, int> idx;
    idx.reserve(N);
    for (int i = 0; i < N; i++) idx[graph.nodes[i].canonical_uri] = i;

    // Build out-link list
    std::vector<std::vector<int>> out_links(N);
    std::vector<int> out_count(N, 0);
    for (const auto& e : graph.edges) {
        auto fi = idx.find(e.from);
        auto ti = idx.find(e.to);
        if (fi == idx.end() || ti == idx.end()) continue;
        out_links[fi->second].push_back(ti->second);
        out_count[fi->second]++;
    }

    // PageRank
    std::vector<double> pr(N, 1.0 / N);
    std::vector<double> new_pr(N, 0.0);

    for (int it = 0; it < ITER; it++) {
        double dangling_sum = 0.0;
        for (int i = 0; i < N; i++) {
            if (out_count[i] == 0) dangling_sum += pr[i];
        }
        double dangling_contrib = dangling_sum / N;

        for (int i = 0; i < N; i++) new_pr[i] = 0.0;

        for (int i = 0; i < N; i++) {
            if (out_count[i] == 0) continue;
            double share = pr[i] / out_count[i];
            for (int j : out_links[i]) new_pr[j] += share;
        }

        double base = (1.0 - d) / N + d * dangling_contrib;
        for (int i = 0; i < N; i++) {
            new_pr[i] = base + d * new_pr[i];
        }
        pr = new_pr;
    }

    // Normalize to [0, 1]
    double max_pr = 0.0;
    for (double v : pr) max_pr = std::max(max_pr, v);
    if (max_pr <= 0) max_pr = 1.0;

    for (int i = 0; i < N; i++) {
        double pagerank_norm = pr[i] / max_pr;
        double id_norm = N > 1 ? static_cast<double>(graph.nodes[i].in_degree) / (N - 1) : 0.0;
        // 70% PageRank + 30% normalized in-degree
        graph.nodes[i].core_score = 0.7 * pagerank_norm + 0.3 * id_norm;
    }

    // Sort nodes by core_score descending (in-place, but keep stable)
    std::stable_sort(graph.nodes.begin(), graph.nodes.end(),
        [](const GraphNode& a, const GraphNode& b) {
            return a.core_score > b.core_score;
        });
}

// =============================================================
// 重定向 + 消歧义检测
// =============================================================

RedirectInfo WikiStructure::redirectInfo(const std::string& canonical_uri) {
    RedirectInfo result;

    auto html_opt = fetchHtml(canonical_uri);
    if (!html_opt) return result;
    const std::string& html = *html_opt;

    // 1. Detect #REDIRECT [[Target]] (wikitext may leak into rendered HTML)
    //    Also check mw-redirect class on wrapper div
    std::string lower_html = html;
    for (auto& c : lower_html) c = static_cast<char>(tolower(static_cast<unsigned char>(c)));

    bool has_redirect_marker = lower_html.find(REDIRECT_MARKER) != std::string::npos;
    bool has_redirect_class = lower_html.find(MW_REDIRECT_CLASS) != std::string::npos;

    if (has_redirect_marker || has_redirect_class) {
        result.is_redirect = true;
        if (has_redirect_marker) result.detected_from_html = "#REDIRECT marker";
        else result.detected_from_html = "mw-redirect class";

        // Try to extract target from #REDIRECT [[Target]]
        // In rendered HTML this may appear as: #REDIRECT <a href="/content/.../Target">Target</a>
        // Or in a special redirect box
        // Strategy: find hrefs in first 2000 chars that are NOT the current page, pick the first
        size_t search_region = std::min(html.size(), (size_t)4000);
        std::string content_prefix;
        static const std::string scheme = "kiwix://";
        if (canonical_uri.compare(0, scheme.size(), scheme) == 0) {
            std::string rest = canonical_uri.substr(scheme.size());
            size_t slash = rest.find('/');
            if (slash != std::string::npos) {
                content_prefix = "/content/" + rest.substr(0, slash) + "/";
            }
        }

        if (!content_prefix.empty()) {
            std::string region = html.substr(0, search_region);
            auto links = extractWikiLinks(region, "", content_prefix);
            for (const auto& [path, text] : links) {
                // Skip the current page itself
                std::string candidate = "kiwix://" + content_prefix.substr(9) + path;  // reconstruct
                // Actually simpler: just check path
                size_t last_slash = canonical_uri.rfind('/');
                std::string cur_path = (last_slash != std::string::npos) ? canonical_uri.substr(last_slash + 1) : "";
                if (path != cur_path) {
                    result.redirect_target = "kiwix://" + content_prefix.substr(9) + path;
                    break;
                }
            }
        }
    }

    // 2. Detect disambiguation page
    //    Wikipedia disambiguation pages have "disambiguation" in categories
    //    or a template like {{disambiguation}} rendered as a special box
    bool has_disambig_cat = false;
    // Check if any link in the page points to Category:Disambiguation_pages or similar
    // Also check for disambiguation-related class/id
    if (lower_html.find("disambiguation") != std::string::npos) {
        // Verify: a disambiguation page has multiple links with "(disambiguation)" suffix
        // or contains text like "may refer to:" / "may stand for:"
        size_t may_refer = lower_html.find("may refer to");
        size_t may_stand = lower_html.find("may stand for");
        if (may_refer != std::string::npos || may_stand != std::string::npos) {
            result.is_disambiguation = true;
            result.detected_from_html += (result.detected_from_html.empty() ? std::string("") : std::string(" + ")) + "disambiguation template";

            // Collect disambiguation targets: wiki links in the page body
            static const std::string scheme = "kiwix://";
            std::string zim_id;
            if (canonical_uri.compare(0, scheme.size(), scheme) == 0) {
                std::string rest = canonical_uri.substr(scheme.size());
                size_t slash = rest.find('/');
                if (slash != std::string::npos) zim_id = rest.substr(0, slash);
            }
            std::string cp = "/content/" + zim_id + "/";
            auto all_links = extractWikiLinks(html, "", cp);
            for (const auto& [path, text] : all_links) {
                // Skip self
                size_t last_slash = canonical_uri.rfind('/');
                std::string cur_path = (last_slash != std::string::npos) ? canonical_uri.substr(last_slash + 1) : "";
                if (path == cur_path) continue;
                // Skip special pages
                std::string lp = path;
                for (auto& c : lp) c = static_cast<char>(tolower(static_cast<unsigned char>(c)));
                static const std::vector<std::string> sp = {
                    "category:", "file:", "template:", "help:", "wikipedia:",
                    "portal:", "mediawiki:", "draft:", "module:", "gadget:",
                    "timedtext:", "image:", "media:", "special:", "book:",
                    "index:", "disambiguation"
                };
                bool skip = false;
                for (const auto& s : sp) {
                    if (lp.compare(0, s.size(), s) == 0) { skip = true; break; }
                }
                if (skip) continue;

                std::string target_uri = "kiwix://" + zim_id + "/" + path;
                if (std::find(result.disambig_targets.begin(),
                              result.disambig_targets.end(),
                              target_uri) == result.disambig_targets.end()) {
                    result.disambig_targets.push_back(target_uri);
                }
                if (result.disambig_targets.size() >= 20) break;
            }
        }
    }

    return result;
}

// =============================================================
// JSON 输出
// =============================================================

json WikiStructure::toJson(const CategoryGraph& g) const {
    json j;
    j["root_category"] = g.root_category;
    j["max_depth_reached"] = g.max_depth_reached;
    j["category_count"] = g.category_count;
    j["article_count"] = g.article_count;
    j["node_count"] = g.nodes.size();
    j["edge_count"] = g.edges.size();

    json nodes_arr = json::array();
    for (const auto& n : g.nodes) {
        json nj;
        nj["canonical_uri"] = n.canonical_uri;
        nj["title"] = n.title;
        nj["node_type"] = n.node_type;
        nj["depth"] = n.depth;
        nodes_arr.push_back(nj);
    }
    j["nodes"] = nodes_arr;

    json edges_arr = json::array();
    for (const auto& e : g.edges) {
        json ej;
        ej["from"] = e.from;
        ej["to"] = e.to;
        ej["edge_type"] = e.edge_type;
        edges_arr.push_back(ej);
    }
    j["edges"] = edges_arr;

    return j;
}

json WikiStructure::toJson(const LinkGraph& g) const {
    json j;
    j["center_uri"] = g.center_uri;
    j["depth"] = g.depth;
    j["node_count"] = g.nodes.size();
    j["edge_count"] = g.edges.size();

    // Top 10 by core_score
    json top_arr = json::array();
    int top_n = std::min(10, static_cast<int>(g.nodes.size()));
    for (int i = 0; i < top_n; i++) {
        const auto& n = g.nodes[i];
        json nj;
        nj["rank"] = i + 1;
        nj["canonical_uri"] = n.canonical_uri;
        nj["title"] = n.title;
        nj["node_type"] = n.node_type;
        nj["core_score"] = std::round(n.core_score * 1000.0) / 1000.0;
        nj["in_degree"] = n.in_degree;
        nj["out_degree"] = n.out_degree;
        nj["depth"] = n.depth;
        top_arr.push_back(nj);
    }
    j["top_nodes_by_core"] = top_arr;

    json nodes_arr = json::array();
    for (const auto& n : g.nodes) {
        json nj;
        nj["canonical_uri"] = n.canonical_uri;
        nj["title"] = n.title;
        nj["node_type"] = n.node_type;
        nj["core_score"] = std::round(n.core_score * 1000.0) / 1000.0;
        nj["in_degree"] = n.in_degree;
        nj["out_degree"] = n.out_degree;
        nj["depth"] = n.depth;
        nodes_arr.push_back(nj);
    }
    j["nodes"] = nodes_arr;

    json edges_arr = json::array();
    for (const auto& e : g.edges) {
        json ej;
        ej["from"] = e.from;
        ej["to"] = e.to;
        ej["edge_type"] = e.edge_type;
        edges_arr.push_back(ej);
    }
    j["edges"] = edges_arr;

    return j;
}

json WikiStructure::toJson(const RedirectInfo& r) const {
    json j;
    j["is_redirect"] = r.is_redirect;
    j["redirect_target"] = r.redirect_target;
    j["is_disambiguation"] = r.is_disambiguation;
    j["disambig_targets"] = r.disambig_targets;
    j["detected_from_html"] = r.detected_from_html;
    return j;
}

} // namespace github_research
