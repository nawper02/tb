#pragma once

#include "app.h"
#include "json.h"
#include "model.h" // for now_iso, format_date

#include <algorithm>
#include <fstream>
#include <set>

class PasteApp : public AppBase {
    struct Snippet {
        int id = 0;
        std::string name;
        std::string content;
        std::string created;
    };

    std::string path;
    int next_id = 1;
    std::vector<Snippet> snippets;
    int cursor = 0;
    int scroll = 0;
    std::set<int> expanded;
    std::string search;
    bool show_help = false;

    static std::string data_path() {
        const char* home = std::getenv("HOME");
        return std::string(home ? home : ".") + "/.pb.json";
    }

    // ── Persistence ─────────────────────────────────────────

    void load() {
        std::ifstream f(path);
        if (!f.is_open()) return;
        std::string content((std::istreambuf_iterator<char>(f)), {});
        if (content.empty()) return;
        try {
            auto v = json::parse(content);
            next_id = v.int_or("next_id", 1);
            if (v.has("snippets") && v.at("snippets").is_arr()) {
                for (auto& s : v.at("snippets").as_arr()) {
                    Snippet sn;
                    sn.id = s.int_or("id", 0);
                    sn.name = s.str_or("name", "");
                    sn.content = s.str_or("content", "");
                    sn.created = s.str_or("created", "");
                    snippets.push_back(sn);
                }
            }
        } catch (...) {
            flash("Error reading ~/.pb.json");
        }
    }

    void save() {
        json::Object o;
        o["next_id"] = next_id;
        json::Array arr;
        for (auto& s : snippets) {
            json::Object so;
            so["id"] = s.id;
            so["name"] = s.name;
            so["content"] = s.content;
            so["created"] = s.created;
            arr.push_back(std::move(so));
        }
        o["snippets"] = std::move(arr);
        std::ofstream f(path);
        f << json::serialize(json::Value(std::move(o)));
    }

    // ── Filtering ───────────────────────────────────────────

    std::vector<int> visible() {
        std::vector<int> vis;
        for (int i = 0; i < (int)snippets.size(); ++i) {
            if (!search.empty()) {
                std::string ln = snippets[i].name, lq = search;
                std::transform(ln.begin(), ln.end(), ln.begin(), ::tolower);
                std::transform(lq.begin(), lq.end(), lq.begin(), ::tolower);
                if (ln.find(lq) == std::string::npos) {
                    std::string lc = snippets[i].content;
                    std::transform(lc.begin(), lc.end(), lc.begin(), ::tolower);
                    if (lc.find(lq) == std::string::npos) continue;
                }
            }
            vis.push_back(i);
        }
        return vis;
    }

    int item_height(int idx) {
        if (!expanded.count(snippets[idx].id)) return 1;
        // Count content lines
        auto& c = snippets[idx].content;
        if (c.empty()) return 2; // name + "(empty)"
        int lines = 1;
        for (char ch : c) if (ch == '\n') ++lines;
        return 1 + lines;
    }

    void clamp_cursor(int count) {
        if (count <= 0) cursor = 0;
        else cursor = std::clamp(cursor, 0, count - 1);
    }

    // ── Drawing ─────────────────────────────────────────────

    void draw_header() {
        attron(COLOR_PAIR(CP_HEADER) | A_BOLD);
        move(top_y, 0);
        for (int i = 0; i < COLS; ++i) addch(' ');
        move(top_y, 1);
        addstr("pb");

        if (!search.empty()) {
            std::string right = " [\"" + search + "\"]";
            int rx = COLS - (int)right.size() - 1;
            if (rx > 0) { move(top_y, rx); addstr(right.c_str()); }
        }
        attroff(COLOR_PAIR(CP_HEADER) | A_BOLD);
    }

    void draw_list(const std::vector<int>& vis, int start_y, int ch) {
        if (vis.empty()) {
            move(start_y + ch / 2, 0);
            attron(A_DIM);
            addstr("  No snippets. 'a' to add, 'p' to paste from clipboard.");
            attroff(A_DIM);
            return;
        }

        int y = start_y;
        for (int vi = scroll; vi < (int)vis.size() && y < start_y + ch; ++vi) {
            int idx = vis[vi];
            auto& sn = snippets[idx];
            bool sel = (vi == cursor);
            bool exp = expanded.count(sn.id);

            move(y, 0);
            if (sel) attron(COLOR_PAIR(CP_SELECTED) | A_BOLD);
            addch(sel ? '>' : ' ');

            // Name
            int prefix = 2;
            int avail = COLS - prefix;

            // Show first line of content inline if not expanded and content exists
            std::string display_name = sn.name;
            if (!exp && !sn.content.empty()) {
                // Get first line of content
                std::string first;
                auto nl = sn.content.find('\n');
                first = (nl != std::string::npos) ? sn.content.substr(0, nl) : sn.content;

                int name_w = (int)display_name.size();
                int content_avail = avail - name_w - 3; // " | "

                if (content_avail > 10) {
                    attron(COLOR_PAIR(CP_SELECTED) | A_BOLD);
                    addstr(trunc(display_name, avail).c_str());

                    // Dashed separator (selected line only)
                    if (sel) {
                        int text_end_x = prefix + (int)display_name.size();
                        int content_start = COLS - std::min(content_avail, (int)first.size()) - 1;
                        int dash_count = content_start - text_end_x - 1;
                        attroff(COLOR_PAIR(CP_SELECTED) | A_BOLD);
                        if (dash_count > 1) {
                            attron(COLOR_PAIR(CP_HINT) | A_DIM);
                            move(y, text_end_x + 1);
                            for (int d = 0; d < dash_count; ++d) addch('-');
                            attroff(COLOR_PAIR(CP_HINT) | A_DIM);
                        }
                    } else {
                        attroff(COLOR_PAIR(CP_SELECTED) | A_BOLD);
                    }

                    // Content preview (right-aligned)
                    move(y, COLS - std::min(content_avail, (int)first.size()) - 1);
                    attron(COLOR_PAIR(CP_HINT) | A_DIM);
                    addstr(trunc(first, content_avail).c_str());
                    attroff(COLOR_PAIR(CP_HINT) | A_DIM);
                } else {
                    addstr(trunc(display_name, avail).c_str());
                }
            } else {
                addstr(trunc(display_name, avail).c_str());
            }

            if (sel) attroff(COLOR_PAIR(CP_SELECTED) | A_BOLD);
            ++y;

            // Expanded content
            if (exp) {
                auto& c = sn.content;
                if (c.empty()) {
                    if (y < start_y + ch) {
                        move(y, 4);
                        attron(A_DIM);
                        addstr("(empty)");
                        attroff(A_DIM);
                        ++y;
                    }
                } else {
                    std::istringstream ss(c);
                    std::string line;
                    while (std::getline(ss, line) && y < start_y + ch) {
                        move(y, 4);
                        attron(COLOR_PAIR(CP_HINT));
                        addstr(trunc(line, COLS - 5).c_str());
                        attroff(COLOR_PAIR(CP_HINT));
                        ++y;
                    }
                }
            }
        }
    }

    void draw_help_view(int start_y, int ch) {
        int y = start_y;
        int w = COLS - 2;

        auto section = [&](const char* title) {
            if (y >= start_y + ch) return;
            ++y;
            move(y, 1);
            attron(A_BOLD | COLOR_PAIR(CP_SELECTED));
            addstr(trunc(title, w).c_str());
            attroff(A_BOLD | COLOR_PAIR(CP_SELECTED));
            ++y;
        };

        auto bind = [&](const char* key, const char* desc) {
            if (y >= start_y + ch) return;
            move(y, 2);
            attron(A_BOLD);
            addstr(key);
            attroff(A_BOLD);
            int kw = (int)strlen(key);
            move(y, 2 + std::max(kw + 1, 10));
            attron(A_DIM);
            addstr(trunc(desc, w - 12).c_str());
            attroff(A_DIM);
            ++y;
        };

        section("Snippets");
        bind("a", "Add snippet (name + content)");
        bind("p", "Paste from clipboard as new snippet");
        bind("y/Enter", "Copy content to clipboard");
        bind("e", "Edit name");
        bind("E", "Edit content");
        bind("d", "Delete snippet");

        section("View");
        bind("o", "Expand/collapse content");
        bind("O", "Expand/collapse all");
        bind("/", "Search");
        bind("Esc", "Clear search");
        bind("?", "Toggle help");
        bind("q", "Quit");
    }

    void draw_footer() {
        std::string hints;
        if (show_help) {
            hints = "q/?:back";
        } else {
            hints = "a:add p:paste y:copy e:name E:content d:del o:expand O:all /:find ?:help q:quit";
            if (!search.empty()) hints = "Esc:clear " + hints;
        }
        draw_status_and_hints(hints);
    }

public:
    const char* id() override { return "pb"; }
    const char* label() override { return "paste"; }

    void init() override {
        path = data_path();
        load();
    }

    void draw() override {
        draw_header();
        int ch = LINES - top_y - 4;
        if (ch < 1) ch = 1;

        if (show_help) {
            draw_help_view(top_y + 1, ch);
        } else {
            auto vis = visible();
            clamp_cursor((int)vis.size());
            // Simple scroll
            if (cursor < scroll) scroll = cursor;
            int y = 0;
            for (int i = scroll; i <= cursor && i < (int)vis.size(); ++i)
                y += item_height(vis[i]);
            if (y > ch) {
                scroll = cursor;
                int h = item_height(vis[cursor]);
                for (int i = cursor - 1; i >= 0; --i) {
                    if (h + item_height(vis[i]) > ch) break;
                    h += item_height(vis[i]);
                    scroll = i;
                }
            }
            draw_list(vis, top_y + 1, ch);
        }
        draw_footer();
    }

    bool handle(int ch) override {
        if (show_help) {
            if (ch == 'q' || ch == '?' || ch == 27) show_help = false;
            return true;
        }

        auto vis = visible();
        int count = (int)vis.size();

        switch (ch) {
            case 'j': case KEY_DOWN: if (cursor < count - 1) ++cursor; break;
            case 'k': case KEY_UP:   if (cursor > 0) --cursor; break;
            case 'g': case KEY_HOME: cursor = 0; break;
            case 'G': case KEY_END:  cursor = std::max(0, count - 1); break;
            case 4:  cursor = std::min(cursor + 10, std::max(0, count - 1)); break;
            case 21: cursor = std::max(cursor - 10, 0); break;

            // Add snippet
            case 'a': {
                std::string name = text_input("Name: ");
                if (name.empty()) break;
                std::string content = text_input("Content: ");
                Snippet sn;
                sn.id = next_id++;
                sn.name = name;
                sn.content = content;
                sn.created = now_iso();
                snippets.push_back(sn);
                save();
                cursor = (int)visible().size() - 1;
                flash("Added: " + name);
                break;
            }

            // Paste from clipboard
            case 'p': {
                std::string content = clipboard_paste();
                if (content.empty()) { flash("Clipboard empty or xclip not found"); break; }
                // Trim trailing newline
                while (!content.empty() && content.back() == '\n') content.pop_back();
                std::string name = text_input("Name for snippet: ");
                if (name.empty()) break;
                Snippet sn;
                sn.id = next_id++;
                sn.name = name;
                sn.content = content;
                sn.created = now_iso();
                snippets.push_back(sn);
                save();
                cursor = (int)visible().size() - 1;
                flash("Pasted: " + name);
                break;
            }

            // Copy to clipboard
            case 'y': case '\n': case KEY_ENTER: {
                if (count == 0) break;
                auto& sn = snippets[vis[cursor]];
                if (sn.content.empty()) { flash("No content to copy"); break; }
                if (clipboard_copy(sn.content))
                    flash("Copied: " + sn.name);
                else
                    flash("Copy failed (install xclip)");
                break;
            }

            // Edit name
            case 'e': {
                if (count == 0) break;
                auto& sn = snippets[vis[cursor]];
                std::string result = text_input("Name: ", sn.name);
                if (result.empty()) break;
                sn.name = result;
                save();
                flash("Renamed");
                break;
            }

            // Edit content
            case 'E': {
                if (count == 0) break;
                auto& sn = snippets[vis[cursor]];
                std::string result = text_input("Content: ", sn.content);
                if (!result.empty() || confirm("Clear content?")) {
                    sn.content = result;
                    save();
                    flash(result.empty() ? "Content cleared" : "Content updated");
                }
                break;
            }

            // Delete
            case 'd': {
                if (count == 0) break;
                auto& sn = snippets[vis[cursor]];
                if (!confirm("Delete \"" + trunc(sn.name, 30) + "\"?")) break;
                int id = sn.id;
                expanded.erase(id);
                snippets.erase(std::remove_if(snippets.begin(), snippets.end(),
                    [id](const Snippet& s) { return s.id == id; }), snippets.end());
                save();
                clamp_cursor((int)visible().size());
                flash("Deleted");
                break;
            }

            // Expand/collapse
            case 'o': {
                if (count == 0) break;
                int id = snippets[vis[cursor]].id;
                if (expanded.count(id)) expanded.erase(id);
                else expanded.insert(id);
                break;
            }

            // Expand/collapse all
            case 'O': {
                bool all_exp = true;
                for (auto vi : vis)
                    if (!expanded.count(snippets[vi].id)) { all_exp = false; break; }
                if (all_exp) {
                    expanded.clear();
                    flash("Collapsed all");
                } else {
                    for (auto vi : vis) expanded.insert(snippets[vi].id);
                    flash("Expanded all");
                }
                break;
            }

            // Search
            case '/': {
                std::string q = text_input("Search (Esc=cancel): ", search);
                search = q;
                cursor = 0; scroll = 0;
                break;
            }

            // Esc: clear search
            case 27:
                if (!search.empty()) {
                    search.clear();
                    cursor = 0; scroll = 0;
                    flash("Search cleared");
                }
                break;

            case '?': show_help = true; break;

            case 'q': return false;
        }
        return true;
    }
};
