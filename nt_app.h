#pragma once

#include "app.h"
#include "json.h"
#include "model.h" // for now_iso, format_date

#include <algorithm>
#include <fstream>
#include <set>
#include <sstream>

// ── Notes App ──────────────────────────────────────────────
//
// Each note has a title and a description composed of blocks.
// Blocks are either paragraph text or copyable snippets
// (rendered distinctly, copied to clipboard with Enter/y).

class NtApp : public AppBase {

    // ── Data model ──────────────────────────────────────────

    enum BlockType { BLK_TEXT, BLK_COPY };

    struct Block {
        BlockType   type = BLK_TEXT;
        std::string content;
    };

    struct NoteEntry {
        int         id = 0;
        std::string title;
        std::string created;
        std::vector<Block> blocks;
    };

    // ── State ───────────────────────────────────────────────

    std::string path;
    int next_id = 1;
    std::vector<NoteEntry> notes;

    enum Mode { LIST, VIEW, HELP };
    Mode mode = LIST;
    int cursor = 0;
    int scroll = 0;
    std::set<int> expanded;
    std::string search;

    // View mode: browsing blocks within a note
    int view_idx = -1;
    int view_cursor = 0;
    int view_scroll = 0;

    // ── Persistence ─────────────────────────────────────────

    static std::string data_path() {
        const char* home = std::getenv("HOME");
        return std::string(home ? home : ".") + "/.nt.json";
    }

    void load() {
        std::ifstream f(path);
        if (!f.is_open()) return;
        std::string content((std::istreambuf_iterator<char>(f)), {});
        if (content.empty()) return;
        try {
            auto v = json::parse(content);
            next_id = v.int_or("next_id", 1);
            notes.clear();
            if (v.has("notes") && v.at("notes").is_arr()) {
                for (auto& nv : v.at("notes").as_arr()) {
                    NoteEntry ne;
                    ne.id = nv.int_or("id", 0);
                    ne.title = nv.str_or("title", "");
                    ne.created = nv.str_or("created", "");
                    if (nv.has("blocks") && nv.at("blocks").is_arr()) {
                        for (auto& bv : nv.at("blocks").as_arr()) {
                            Block b;
                            std::string t = bv.str_or("type", "text");
                            b.type = (t == "copy") ? BLK_COPY : BLK_TEXT;
                            b.content = bv.str_or("content", "");
                            ne.blocks.push_back(b);
                        }
                    }
                    notes.push_back(std::move(ne));
                }
            }
        } catch (...) {
            flash("Error reading ~/.nt.json");
        }
    }

    void save() {
        json::Object o;
        o["next_id"] = next_id;
        json::Array arr;
        for (auto& ne : notes) {
            json::Object no;
            no["id"] = ne.id;
            no["title"] = ne.title;
            no["created"] = ne.created;
            json::Array ba;
            for (auto& b : ne.blocks) {
                json::Object bo;
                bo["type"] = (b.type == BLK_COPY) ? "copy" : "text";
                bo["content"] = b.content;
                ba.push_back(std::move(bo));
            }
            no["blocks"] = std::move(ba);
            arr.push_back(std::move(no));
        }
        o["notes"] = std::move(arr);
        std::ofstream f(path);
        f << json::serialize(json::Value(std::move(o)));
    }

    // ── Filtering ───────────────────────────────────────────

    std::vector<int> visible() {
        std::vector<int> vis;
        for (int i = 0; i < (int)notes.size(); ++i) {
            if (!search.empty()) {
                std::string lt = notes[i].title, lq = search;
                std::transform(lt.begin(), lt.end(), lt.begin(), ::tolower);
                std::transform(lq.begin(), lq.end(), lq.begin(), ::tolower);
                if (lt.find(lq) == std::string::npos) {
                    // Also search block content
                    bool found = false;
                    for (auto& b : notes[i].blocks) {
                        std::string lc = b.content;
                        std::transform(lc.begin(), lc.end(), lc.begin(), ::tolower);
                        if (lc.find(lq) != std::string::npos) { found = true; break; }
                    }
                    if (!found) continue;
                }
            }
            vis.push_back(i);
        }
        return vis;
    }

    void clamp_cursor(int count) {
        if (count <= 0) cursor = 0;
        else cursor = std::clamp(cursor, 0, count - 1);
    }

    // ── Item height (for list scroll) ───────────────────────

    int block_lines(const Block& b, int width) const {
        if (b.content.empty()) return 1;
        int lines = 1;
        for (char c : b.content) if (c == '\n') ++lines;
        return lines;
    }

    int item_height(int idx) {
        int h = 1; // title line
        if (expanded.count(notes[idx].id)) {
            auto& ne = notes[idx];
            if (ne.blocks.empty()) {
                h += 1; // "(no content)"
            } else {
                for (auto& b : ne.blocks)
                    h += block_lines(b, COLS - 6);
            }
        }
        return h;
    }

    // ── Drawing: header ─────────────────────────────────────

    void draw_header() {
        attron(COLOR_PAIR(CP_HEADER) | A_BOLD);
        move(top_y, 0);
        for (int i = 0; i < COLS; ++i) addch(' ');
        move(top_y, 1);
        if (mode == VIEW && view_idx >= 0 && view_idx < (int)notes.size())
            addstr(("nt > " + trunc(notes[view_idx].title, COLS - 20)).c_str());
        else
            addstr("nt");

        if (!search.empty()) {
            std::string right = " [\"" + search + "\"]";
            int rx = COLS - (int)right.size() - 1;
            if (rx > 0) { move(top_y, rx); addstr(right.c_str()); }
        }
        attroff(COLOR_PAIR(CP_HEADER) | A_BOLD);
    }

    // ── Drawing: list mode ──────────────────────────────────

    void draw_list_view(const std::vector<int>& vis, int start_y, int ch) {
        if (vis.empty()) {
            move(start_y + ch / 2, 0);
            attron(A_DIM);
            addstr("  No notes. 'a' to add a new note.");
            attroff(A_DIM);
            return;
        }

        int y = start_y;
        for (int vi = scroll; vi < (int)vis.size() && y < start_y + ch; ++vi) {
            int idx = vis[vi];
            auto& ne = notes[idx];
            bool sel = (vi == cursor);
            bool exp = expanded.count(ne.id);

            // ── Title line ──
            move(y, 0);
            if (sel) attron(COLOR_PAIR(CP_SELECTED) | A_BOLD);
            addch(sel ? '>' : ' ');
            addch(' ');
            addstr(trunc(ne.title, COLS - 2).c_str());

            // Right-aligned: block count + date
            std::string ind;
            int ncopy = 0;
            for (auto& b : ne.blocks) if (b.type == BLK_COPY) ++ncopy;
            ind += std::to_string(ne.blocks.size()) + "b";
            if (ncopy > 0) ind += " " + std::to_string(ncopy) + "c";
            ind += " " + format_short(ne.created);

            int ind_w = (int)ind.size();
            int text_end_x = 2 + std::min((int)ne.title.size(), COLS - 2);
            int ind_start_x = COLS - ind_w - 1;

            if (sel && ind_start_x > text_end_x + 2) {
                int dash_count = ind_start_x - text_end_x - 1;
                attroff(COLOR_PAIR(CP_SELECTED) | A_BOLD);
                if (dash_count > 1) {
                    attron(COLOR_PAIR(CP_HINT) | A_DIM);
                    move(y, text_end_x + 1);
                    for (int d = 0; d < dash_count; ++d) addch('-');
                    attroff(COLOR_PAIR(CP_HINT) | A_DIM);
                }
                move(y, ind_start_x);
                attron(COLOR_PAIR(CP_HINT) | A_DIM);
                addstr(ind.c_str());
                attroff(COLOR_PAIR(CP_HINT) | A_DIM);
            } else if (ind_start_x > text_end_x + 1) {
                attroff(COLOR_PAIR(CP_SELECTED) | A_BOLD);
                move(y, ind_start_x);
                attron(COLOR_PAIR(CP_HINT) | A_DIM);
                addstr(ind.c_str());
                attroff(COLOR_PAIR(CP_HINT) | A_DIM);
            }

            if (sel) attroff(COLOR_PAIR(CP_SELECTED) | A_BOLD);
            ++y;

            // ── Expanded blocks ──
            if (exp) {
                int sub_indent = 4;
                if (ne.blocks.empty()) {
                    if (y < start_y + ch) {
                        move(y, sub_indent);
                        attron(A_DIM);
                        addstr("(no content)");
                        attroff(A_DIM);
                        ++y;
                    }
                } else {
                    for (auto& b : ne.blocks) {
                        bool is_copy = (b.type == BLK_COPY);
                        int cp = is_copy ? CP_CYAN : CP_HINT;
                        int attr = is_copy ? A_BOLD : A_DIM;
                        std::istringstream ss(b.content);
                        std::string line;
                        bool first = true;
                        if (b.content.empty()) {
                            if (y < start_y + ch) {
                                move(y, sub_indent);
                                attron(COLOR_PAIR(cp) | A_DIM);
                                addstr(is_copy ? "[copy] (empty)" : "(empty)");
                                attroff(COLOR_PAIR(cp) | A_DIM);
                                ++y;
                            }
                        } else {
                            while (std::getline(ss, line) && y < start_y + ch) {
                                move(y, sub_indent);
                                attron(COLOR_PAIR(cp) | attr);
                                if (first && is_copy) {
                                    addstr(trunc("[copy] " + line, COLS - sub_indent - 1).c_str());
                                    first = false;
                                } else {
                                    // continuation: indent to align with content after "[copy] "
                                    std::string pad = is_copy ? "       " : "";
                                    addstr(trunc(pad + line, COLS - sub_indent - 1).c_str());
                                }
                                attroff(COLOR_PAIR(cp) | attr);
                                ++y;
                            }
                        }
                    }
                }
            }
        }
    }

    // ── Drawing: view mode (browse blocks) ──────────────────

    void draw_view(int start_y, int ch) {
        if (view_idx < 0 || view_idx >= (int)notes.size()) {
            mode = LIST;
            return;
        }
        auto& ne = notes[view_idx];

        if (ne.blocks.empty()) {
            move(start_y + ch / 2, 0);
            attron(A_DIM);
            addstr("  No blocks. 't' to add text, 'c' to add copyable.");
            attroff(A_DIM);
            return;
        }

        if (view_cursor >= (int)ne.blocks.size())
            view_cursor = std::max(0, (int)ne.blocks.size() - 1);

        // Compute row offsets per block for scrolling (+1 per block for blank spacer)
        std::vector<int> block_start_row;
        int total_rows = 0;
        for (auto& b : ne.blocks) {
            block_start_row.push_back(total_rows);
            total_rows += block_lines(b, COLS - 3) + 1;
        }

        // Scroll so current block is visible
        int cur_row = block_start_row[view_cursor];
        int cur_h = block_lines(ne.blocks[view_cursor], COLS - 3) + 1;
        if (cur_row < view_scroll) view_scroll = cur_row;
        if (cur_row + cur_h > view_scroll + ch) view_scroll = cur_row + cur_h - ch;
        if (view_scroll < 0) view_scroll = 0;

        int y = start_y;
        int row = 0;

        for (int bi = 0; bi < (int)ne.blocks.size(); ++bi) {
            auto& b = ne.blocks[bi];
            bool sel = (bi == view_cursor);
            bool is_copy = (b.type == BLK_COPY);
            int bh = block_lines(b, COLS - 3) + 1; // +1 blank spacer row

            // Selected: bold (not dim). Unselected copy: cyan bold. Unselected text: hint dim.
            int cp   = is_copy ? CP_CYAN : CP_HINT;
            int attr = sel ? A_BOLD : (is_copy ? A_BOLD : A_DIM);

            for (int lr = 0; lr < bh; ++lr) {
                if (row >= view_scroll && y < start_y + ch) {
                    if (lr == bh - 1) {
                        // blank spacer line
                        ++y;
                    } else {
                        move(y, 0);

                        // Selector column: '>' in block's own color, not blue
                        if (lr == 0) {
                            attron(COLOR_PAIR(cp) | A_BOLD);
                            addstr(sel ? "> " : "  ");
                            attroff(COLOR_PAIR(cp) | A_BOLD);
                        } else {
                            addstr("  ");
                        }

                        // Get the lr-th content line
                        std::string line;
                        if (b.content.empty()) {
                            line = "(empty)";
                        } else {
                            std::istringstream ss(b.content);
                            for (int skip = 0; skip <= lr; ++skip)
                                if (!std::getline(ss, line)) line.clear();
                        }

                        attron(COLOR_PAIR(cp) | attr);
                        if (lr == 0 && is_copy) {
                            addstr(trunc("[copy] " + line, COLS - 3).c_str());
                        } else if (is_copy) {
                            addstr(trunc("       " + line, COLS - 3).c_str());
                        } else {
                            addstr(trunc(line, COLS - 3).c_str());
                        }
                        attroff(COLOR_PAIR(cp) | attr);

                        ++y;
                    }
                }
                ++row;
            }
        }
    }

    // ── Drawing: help view ──────────────────────────────────

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
            move(y, 2 + std::max(kw + 1, 12));
            attron(A_DIM);
            addstr(trunc(desc, w - 14).c_str());
            attroff(A_DIM);
            ++y;
        };

        section("Notes");
        bind("a", "Add new note");
        bind("Enter", "View/edit note blocks");
        bind("e", "Edit title");
        bind("d", "Delete note");
        bind("o", "Expand/collapse preview");
        bind("O", "Expand/collapse all");

        section("View Mode (Enter)");
        bind("t", "Add text block (paragraph)");
        bind("c", "Add copyable block (snippet)");
        bind("Enter", "Copy selected copyable block to clipboard");
        bind("e", "Edit block content");
        bind("d", "Delete block");
        bind("T", "Toggle block type (text/copy)");
        bind("Shift+↑/↓", "Move block up/down");
        bind("Esc/q", "Back to list");

        section("Navigation");
        bind("j/k", "Move cursor");
        bind("g/G", "Top / bottom");
        bind("/", "Search");
        bind("?", "Toggle help");
        bind("q", "Quit");
    }

    // ── Drawing: footer ─────────────────────────────────────

    void draw_footer() {
        std::string hints;
        if (mode == HELP) {
            hints = "q/?:back";
        } else if (mode == VIEW) {
            hints = "t:text c:copy Enter:copy e:edit d:del T:toggle S+↑↓:move Esc:back";
        } else {
            hints = "a:add Enter:view e:title d:del o:expand /:find ?:help q:quit";
            if (!search.empty()) hints = "Esc:clear " + hints;
        }
        draw_status_and_hints(hints);
    }

    // ── Scroll management ───────────────────────────────────

    void fix_scroll(const std::vector<int>& vis, int content_h) {
        int count = (int)vis.size();
        if (count == 0) { scroll = 0; return; }
        if (cursor < scroll) { scroll = cursor; return; }
        int y = 0;
        for (int i = scroll; i <= cursor && i < count; ++i) {
            if (i == cursor && y + item_height(vis[i]) <= content_h) return;
            y += item_height(vis[i]);
        }
        int h = item_height(vis[cursor]);
        scroll = cursor;
        for (int i = cursor - 1; i >= 0; --i) {
            if (h + item_height(vis[i]) > content_h) break;
            h += item_height(vis[i]);
            scroll = i;
        }
    }

    // ── Mini text editor ────────────────────────────────────
    // Modal panel editor. Returns {confirmed, text}.
    // confirmed=false means user cancelled (Esc).

    std::pair<bool, std::string> text_editor(const std::string& title,
                                              const std::string& initial = "") {
        // Split initial text into lines
        std::vector<std::string> lines;
        {
            std::istringstream ss(initial);
            std::string ln;
            while (std::getline(ss, ln)) lines.push_back(ln);
            if (lines.empty()) lines.push_back("");
        }

        int cur_row = (int)lines.size() - 1;
        int cur_col = (int)lines[cur_row].size();
        int scroll_y = 0;

        curs_set(1);
        while (true) {
            erase();
            int ph = std::max(6, LINES - 4);   // panel height
            int pw = std::max(20, COLS - 4);    // panel width
            int py = (LINES - ph) / 2;          // panel top
            int px = (COLS - pw) / 2;           // panel left
            int edit_h = ph - 3;                // editable rows (below title, above hint+border)
            int content_w = pw - 2;             // usable width inside borders

            // Scroll to keep cursor visible
            if (cur_row < scroll_y) scroll_y = cur_row;
            if (cur_row >= scroll_y + edit_h) scroll_y = cur_row - edit_h + 1;

            // Background fill + vertical borders
            for (int r = py + 1; r < py + ph - 1; ++r) {
                move(r, px);
                attron(A_BOLD); addch(ACS_VLINE); attroff(A_BOLD);
                for (int c = 1; c < pw - 1; ++c) addch(' ');
                attron(A_BOLD); addch(ACS_VLINE); attroff(A_BOLD);
            }

            // Top border + title
            move(py, px);
            attron(A_BOLD);
            addch(ACS_ULCORNER);
            std::string ttl = " " + title + " ";
            int ttl_w = std::min((int)ttl.size(), pw - 2);
            addstr(ttl.substr(0, ttl_w).c_str());
            for (int c = px + 1 + ttl_w; c < px + pw - 1; ++c) addch(ACS_HLINE);
            addch(ACS_URCORNER);
            attroff(A_BOLD);

            // Bottom border
            move(py + ph - 1, px);
            attron(A_BOLD);
            addch(ACS_LLCORNER);
            for (int c = 0; c < pw - 2; ++c) addch(ACS_HLINE);
            addch(ACS_LRCORNER);
            attroff(A_BOLD);

            // Hint line (row above bottom border)
            move(py + ph - 2, px + 1);
            attron(A_DIM);
            addstr(trunc("Ctrl+S:save  Ctrl+V:paste  Esc:cancel", content_w).c_str());
            attroff(A_DIM);

            // Horizontal scroll offset for current line
            int hscroll = 0;
            if (cur_col > content_w - 1) hscroll = cur_col - content_w + 1;

            // Text content
            for (int r = 0; r < edit_h; ++r) {
                int li = scroll_y + r;
                move(py + 1 + r, px + 1);
                if (li < (int)lines.size()) {
                    int hs = (li == cur_row) ? hscroll : 0;
                    std::string disp = lines[li].substr(hs);
                    addstr(trunc(disp, content_w).c_str());
                }
            }

            // Terminal cursor position
            move(py + 1 + (cur_row - scroll_y), px + 1 + (cur_col - hscroll));
            refresh();

            int ch = getch();
            if (ch == ERR) continue;

            if (ch == 19) { // Ctrl+S — save
                curs_set(0);
                std::string result;
                for (int i = 0; i < (int)lines.size(); ++i) {
                    if (i) result += '\n';
                    result += lines[i];
                }
                return {true, result};
            }
            if (ch == 27) { // Esc — cancel
                curs_set(0);
                return {false, ""};
            }
            if (ch == 22) { // Ctrl+V — paste from clipboard
                std::string clip = clipboard_paste();
                for (char c : clip) {
                    if (c == '\n') {
                        std::string tail = lines[cur_row].substr(cur_col);
                        lines[cur_row] = lines[cur_row].substr(0, cur_col);
                        lines.insert(lines.begin() + cur_row + 1, tail);
                        ++cur_row; cur_col = 0;
                    } else if (c >= 32 && c < 127) {
                        lines[cur_row].insert(cur_col++, 1, c);
                    }
                }
            } else if (ch == '\n' || ch == KEY_ENTER) {
                std::string tail = lines[cur_row].substr(cur_col);
                lines[cur_row] = lines[cur_row].substr(0, cur_col);
                lines.insert(lines.begin() + cur_row + 1, tail);
                ++cur_row; cur_col = 0;
            } else if (ch == KEY_BACKSPACE || ch == 127 || ch == 8) {
                if (cur_col > 0) {
                    lines[cur_row].erase(--cur_col, 1);
                } else if (cur_row > 0) {
                    cur_col = (int)lines[cur_row - 1].size();
                    lines[cur_row - 1] += lines[cur_row];
                    lines.erase(lines.begin() + cur_row--);
                }
            } else if (ch == KEY_DC) {
                if (cur_col < (int)lines[cur_row].size()) {
                    lines[cur_row].erase(cur_col, 1);
                } else if (cur_row < (int)lines.size() - 1) {
                    lines[cur_row] += lines[cur_row + 1];
                    lines.erase(lines.begin() + cur_row + 1);
                }
            } else if (ch == KEY_UP) {
                if (cur_row > 0) {
                    --cur_row;
                    cur_col = std::min(cur_col, (int)lines[cur_row].size());
                }
            } else if (ch == KEY_DOWN) {
                if (cur_row < (int)lines.size() - 1) {
                    ++cur_row;
                    cur_col = std::min(cur_col, (int)lines[cur_row].size());
                }
            } else if (ch == KEY_LEFT) {
                if (cur_col > 0) --cur_col;
                else if (cur_row > 0) { --cur_row; cur_col = (int)lines[cur_row].size(); }
            } else if (ch == KEY_RIGHT) {
                if (cur_col < (int)lines[cur_row].size()) ++cur_col;
                else if (cur_row < (int)lines.size() - 1) { ++cur_row; cur_col = 0; }
            } else if (ch == KEY_HOME || ch == 1) {
                cur_col = 0;
            } else if (ch == KEY_END || ch == 5) {
                cur_col = (int)lines[cur_row].size();
            } else if (ch >= 32 && ch < 127) {
                lines[cur_row].insert(cur_col++, 1, (char)ch);
            }
        }
    }

    // ── Handle: list mode ───────────────────────────────────

    bool handle_list(int ch) {
        auto vis = visible();
        int count = (int)vis.size();

        switch (ch) {
            case 'j': case KEY_DOWN: if (cursor < count - 1) ++cursor; break;
            case 'k': case KEY_UP:   if (cursor > 0) --cursor; break;
            case 'g': case KEY_HOME: cursor = 0; break;
            case 'G': case KEY_END:  cursor = std::max(0, count - 1); break;
            case 4:  cursor = std::min(cursor + 10, std::max(0, count - 1)); break;
            case 21: cursor = std::max(cursor - 10, 0); break;

            // Add note
            case 'a': {
                std::string title = text_input("Title: ");
                if (title.empty()) break;
                NoteEntry ne;
                ne.id = next_id++;
                ne.title = title;
                ne.created = now_iso();
                notes.push_back(std::move(ne));
                save();
                cursor = (int)visible().size() - 1;
                flash("Added: " + title);
                break;
            }

            // View/edit blocks
            case '\n': case KEY_ENTER: {
                if (count == 0) break;
                view_idx = vis[cursor];
                view_cursor = 0;
                view_scroll = 0;
                mode = VIEW;
                break;
            }

            // Edit title
            case 'e': {
                if (count == 0) break;
                auto& ne = notes[vis[cursor]];
                std::string result = text_input("Title: ", ne.title);
                if (result.empty()) break;
                ne.title = result;
                save();
                flash("Renamed");
                break;
            }

            // Delete
            case 'd': {
                if (count == 0) break;
                auto& ne = notes[vis[cursor]];
                if (!confirm("Delete \"" + trunc(ne.title, 30) + "\"?")) break;
                int id = ne.id;
                expanded.erase(id);
                notes.erase(std::remove_if(notes.begin(), notes.end(),
                    [id](const NoteEntry& n) { return n.id == id; }), notes.end());
                save();
                clamp_cursor((int)visible().size());
                flash("Deleted");
                break;
            }

            // Expand/collapse
            case 'o': {
                if (count == 0) break;
                int id = notes[vis[cursor]].id;
                if (expanded.count(id)) expanded.erase(id);
                else expanded.insert(id);
                break;
            }

            // Expand/collapse all
            case 'O': {
                bool all_exp = true;
                for (auto vi : vis)
                    if (!expanded.count(notes[vi].id)) { all_exp = false; break; }
                if (all_exp) {
                    expanded.clear();
                    flash("Collapsed all");
                } else {
                    for (auto vi : vis) expanded.insert(notes[vi].id);
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

            case 27:
                if (!search.empty()) {
                    search.clear();
                    cursor = 0; scroll = 0;
                    flash("Search cleared");
                }
                break;

            case '?': mode = HELP; break;
            case 'q': return false;
        }
        return true;
    }

    // ── Handle: view mode (block editor) ────────────────────

    bool handle_view(int ch) {
        if (view_idx < 0 || view_idx >= (int)notes.size()) {
            mode = LIST;
            return true;
        }
        auto& ne = notes[view_idx];
        int count = (int)ne.blocks.size();

        switch (ch) {
            case 'j': case KEY_DOWN: if (view_cursor < count - 1) ++view_cursor; break;
            case 'k': case KEY_UP:   if (view_cursor > 0) --view_cursor; break;
            case 'g': case KEY_HOME: view_cursor = 0; break;
            case 'G': case KEY_END:  view_cursor = std::max(0, count - 1); break;

            // Add text block
            case 't': {
                auto [ok, content] = text_editor("Text block");
                if (!ok) break;
                Block b;
                b.type = BLK_TEXT;
                b.content = content;
                int insert_at = count > 0 ? view_cursor + 1 : 0;
                ne.blocks.insert(ne.blocks.begin() + insert_at, b);
                view_cursor = insert_at;
                save();
                flash("Text block added");
                break;
            }

            // Add copyable block
            case 'c': {
                auto [ok, content] = text_editor("Copyable block");
                if (!ok) break;
                Block b;
                b.type = BLK_COPY;
                b.content = content;
                int insert_at = count > 0 ? view_cursor + 1 : 0;
                ne.blocks.insert(ne.blocks.begin() + insert_at, b);
                view_cursor = insert_at;
                save();
                flash("Copyable block added");
                break;
            }

            // Paste from clipboard as copyable block
            case 'p': {
                std::string content = clipboard_paste();
                if (content.empty()) { flash("Clipboard empty or xclip not found"); break; }
                while (!content.empty() && content.back() == '\n') content.pop_back();
                Block b;
                b.type = BLK_COPY;
                b.content = content;
                int insert_at = count > 0 ? view_cursor + 1 : 0;
                ne.blocks.insert(ne.blocks.begin() + insert_at, b);
                view_cursor = insert_at;
                save();
                flash("Pasted as copyable block");
                break;
            }

            // Copy copyable block to clipboard
            case '\n': case KEY_ENTER: {
                if (count == 0) break;
                auto& b = ne.blocks[view_cursor];
                if (b.type != BLK_COPY) break;
                if (b.content.empty()) { flash("No content to copy"); break; }
                if (clipboard_copy(b.content))
                    flash("Copied to clipboard");
                else
                    flash("Copy failed (install xclip)");
                break;
            }

            // Edit block content
            case 'e': {
                if (count == 0) break;
                auto& b = ne.blocks[view_cursor];
                auto [ok, result] = text_editor(
                    b.type == BLK_COPY ? "Edit Copyable" : "Edit Text", b.content);
                if (!ok) break;
                b.content = result;
                save();
                flash("Updated");
                break;
            }

            // Delete block
            case 'd': {
                if (count == 0) break;
                ne.blocks.erase(ne.blocks.begin() + view_cursor);
                if (view_cursor >= (int)ne.blocks.size())
                    view_cursor = std::max(0, (int)ne.blocks.size() - 1);
                save();
                flash("Block deleted");
                break;
            }

            // Toggle block type
            case 'T': {
                if (count == 0) break;
                auto& b = ne.blocks[view_cursor];
                b.type = (b.type == BLK_COPY) ? BLK_TEXT : BLK_COPY;
                save();
                flash(b.type == BLK_COPY ? "Now copyable" : "Now text");
                break;
            }

            // Move block down
            case KEY_SF: {
                if (view_cursor < count - 1) {
                    std::swap(ne.blocks[view_cursor], ne.blocks[view_cursor + 1]);
                    ++view_cursor;
                    save();
                }
                break;
            }

            // Move block up
            case KEY_SR: {
                if (view_cursor > 0) {
                    std::swap(ne.blocks[view_cursor], ne.blocks[view_cursor - 1]);
                    --view_cursor;
                    save();
                }
                break;
            }

            // Back to list
            case 27: case 'q':
                mode = LIST;
                break;
        }
        return true;
    }

public:
    const char* id() override { return "nt"; }
    const char* label() override { return "notes"; }

    void init() override {
        path = data_path();
        load();
    }

    void draw() override {
        draw_header();
        int ch = LINES - top_y - 4;
        if (ch < 1) ch = 1;

        switch (mode) {
            case LIST: {
                auto vis = visible();
                clamp_cursor((int)vis.size());
                fix_scroll(vis, ch);
                draw_list_view(vis, top_y + 1, ch);
                break;
            }
            case VIEW:
                draw_view(top_y + 1, ch);
                break;
            case HELP:
                draw_help_view(top_y + 1, ch);
                break;
        }
        draw_footer();
    }

    bool handle(int ch) override {
        if (mode == HELP) {
            if (ch == 'q' || ch == '?' || ch == 27) mode = LIST;
            return true;
        }
        if (mode == VIEW) return handle_view(ch);
        return handle_list(ch);
    }
};
