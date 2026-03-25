#pragma once

#include "app.h"
#include "json.h"
#include "model.h"

#include <cctype>
#include <deque>
#include <fstream>
#include <set>

class TdApp : public AppBase {
    // ── Display Item ────────────────────────────────────────
    struct DisplayItem {
        bool is_folder = false;
        std::string folder_name;
        Task* task = nullptr;
        int indent = 0;
        int folder_count = 0;
    };

    State state;
    std::string path;
    std::deque<State> undo_stack;

    enum Mode { LIST, DONE, ARCHIVE, DETAIL, HELP, FILTER };
    Mode mode = LIST;
    int cursor = 0;
    int scroll = 0;
    std::set<std::string> filter_include;
    std::set<std::string> filter_exclude;
    int filter_cursor = 0;
    std::string search;
    std::set<int> expanded;
    std::set<std::string> expanded_folders;

    static constexpr int MAX_UNDO = 20;
    static constexpr int PREFIX_W = 7;
    static constexpr int FOLDER_INDENT = 2;

    // ── Persistence ─────────────────────────────────────────

    static std::string data_path() {
        const char* home = std::getenv("HOME");
        return std::string(home ? home : ".") + "/.td.json";
    }

    void load() {
        std::ifstream f(path);
        if (!f.is_open()) { init_state(); return; }
        std::string content((std::istreambuf_iterator<char>(f)), {});
        if (content.empty()) { init_state(); return; }
        try {
            state = state_from_json(json::parse(content));
        } catch (...) {
            init_state();
            flash("Error reading ~/.td.json");
        }
        for (auto& t : state.tasks)
            if (!t.folder.empty()) expanded_folders.insert(t.folder);
        for (auto& t : state.archive)
            if (!t.folder.empty()) expanded_folders.insert(t.folder);
    }

    void init_state() {
        state.created = now_iso();
        state.status_defs = StatusDef::defaults();
    }

    void save() {
        std::set<std::string> fl;
        for (auto& f : state.folders) fl.insert(f);
        for (auto& t : state.tasks) if (!t.folder.empty()) fl.insert(t.folder);
        for (auto& t : state.archive) if (!t.folder.empty()) fl.insert(t.folder);
        state.folders.assign(fl.begin(), fl.end());
        std::ofstream f(path);
        f << json::serialize(state_to_json(state));
    }

    void push_undo() {
        undo_stack.push_back(state);
        if ((int)undo_stack.size() > MAX_UNDO) undo_stack.pop_front();
    }

    void undo() {
        if (undo_stack.empty()) { flash("Nothing to undo"); return; }
        state = undo_stack.back();
        undo_stack.pop_back();
        save();
        flash("Undone");
    }

    // ── Task Filtering ──────────────────────────────────────

    std::vector<std::string> all_tags() {
        std::set<std::string> tags;
        for (auto& t : state.tasks)
            for (auto& tag : t.tags) tags.insert(tag);
        return std::vector<std::string>(tags.begin(), tags.end());
    }

    bool has_active_filters() {
        return !filter_include.empty() || !filter_exclude.empty() || !search.empty();
    }

    bool passes_filter(const Task& t) {
        if (mode == DONE && !t.done) return false;
        if (!filter_include.empty()) {
            bool found = false;
            for (auto& tag : t.tags)
                if (filter_include.count(tag)) { found = true; break; }
            if (!found) return false;
        }
        if (!filter_exclude.empty()) {
            for (auto& tag : t.tags)
                if (filter_exclude.count(tag)) return false;
        }
        if (!search.empty()) {
            std::string lt = t.text, lq = search;
            std::transform(lt.begin(), lt.end(), lt.begin(), ::tolower);
            std::transform(lq.begin(), lq.end(), lq.begin(), ::tolower);
            if (lt.find(lq) == std::string::npos) return false;
        }
        return true;
    }

    // ── Display List ────────────────────────────────────────

    std::vector<DisplayItem> build_display() {
        auto& src = (mode == ARCHIVE) ? state.archive : state.tasks;
        std::map<std::string, std::vector<Task*>> by_folder;
        std::vector<Task*> unfiled;
        for (auto& t : src) {
            if (!passes_filter(t)) continue;
            if (t.folder.empty()) unfiled.push_back(&t);
            else by_folder[t.folder].push_back(&t);
        }

        std::vector<DisplayItem> items;
        for (auto& [fname, ftasks] : by_folder) {
            DisplayItem fi;
            fi.is_folder = true;
            fi.folder_name = fname;
            fi.folder_count = (int)ftasks.size();
            items.push_back(fi);
            if (expanded_folders.count(fname)) {
                for (auto* t : ftasks) {
                    DisplayItem ti;
                    ti.task = t;
                    ti.indent = FOLDER_INDENT;
                    items.push_back(ti);
                }
            }
        }
        for (auto* t : unfiled) {
            DisplayItem ti;
            ti.task = t;
            items.push_back(ti);
        }
        return items;
    }

    int item_height(const DisplayItem& item) {
        if (item.is_folder) return 1;
        Task* t = item.task;
        if (!t) return 1;

        auto inds = build_indicators(*t);
        int ind_w = indicators_width(inds);
        int prefix = 1 + item.indent + 3 + 1 + 1 + 1;
        int sep_w = (ind_w > 0) ? 3 : 0;
        int text_avail = COLS - prefix - ind_w - sep_w;

        std::string display = t->text;
        for (auto& tag : t->tags) display += " +" + tag;
        auto lines = word_wrap(display, std::max(text_avail, 5));
        int h = (int)lines.size();

        if (expanded.count(t->id)) {
            if (!t->description.empty()) {
                auto dl = word_wrap(t->description, std::max(COLS - prefix - 2, 5));
                h += (int)dl.size();
            }
            h += (int)t->subtasks.size() + (int)t->notes.size();
        }
        return h;
    }

    void clamp_cursor(int count) {
        if (count <= 0) cursor = 0;
        else cursor = std::clamp(cursor, 0, count - 1);
    }

    void ensure_visible(const std::vector<DisplayItem>& items, int content_h) {
        if (items.empty()) return;
        if (cursor < scroll) { scroll = cursor; return; }
        int y = 0;
        for (int i = scroll; i <= cursor && i < (int)items.size(); ++i) {
            if (i == cursor && y + item_height(items[i]) <= content_h) return;
            y += item_height(items[i]);
        }
        int h = item_height(items[cursor]);
        scroll = cursor;
        for (int i = cursor - 1; i >= 0; --i) {
            if (h + item_height(items[i]) > content_h) break;
            h += item_height(items[i]);
            scroll = i;
        }
    }

    // ── Drawing ─────────────────────────────────────────────

    void draw_header() {
        attron(COLOR_PAIR(CP_HEADER) | A_BOLD);
        move(top_y, 0);
        for (int i = 0; i < COLS; ++i) addch(' ');
        move(top_y, 1);

        switch (mode) {
            case LIST:    addstr("td"); break;
            case DONE:    addstr("td  DONE"); break;
            case ARCHIVE: addstr("td  ARCHIVE"); break;
            case DETAIL:  addstr("TASK DETAIL"); break;
            case HELP:    addstr("HELP"); break;
            case FILTER:  addstr("FILTER BY TAG"); break;
        }

        std::string right;
        for (auto& tag : filter_include) right += " +" + tag;
        for (auto& tag : filter_exclude) right += " !" + tag;
        if (!search.empty()) right += " \"" + search + "\"";
        if (!right.empty()) {
            right = " [" + right.substr(1) + "]";
            int rx = COLS - (int)right.size() - 1;
            if (rx > 0) { move(top_y, rx); addstr(right.c_str()); }
        }
        attroff(COLOR_PAIR(CP_HEADER) | A_BOLD);
    }

    // ── Indicators (right side of task line) ────────────────

    struct Indicator { std::string text; int cp; int attr; };

    std::vector<Indicator> build_indicators(const Task& t) {
        std::vector<Indicator> inds;
        if (!t.description.empty())
            inds.push_back({"D", CP_HINT, A_DIM});
        if (!t.subtasks.empty()) {
            int done = 0;
            for (auto& s : t.subtasks) if (s.done) ++done;
            inds.push_back({std::to_string(done) + "/" + std::to_string(t.subtasks.size()),
                           CP_SELECTED, 0});
        }
        if (!t.notes.empty()) {
            std::string n = expanded.count(t.id) ? "-" : "+";
            n += std::to_string(t.notes.size());
            inds.push_back({n, CP_NOTE, A_DIM});
        }
        for (auto& sd : state.status_defs)
            if (t.statuses.count(sd.name))
                inds.push_back({sd.label, status_cp(sd.color), A_BOLD});
        for (auto& [name, val] : t.statuses) {
            bool has_def = false;
            for (auto& sd : state.status_defs)
                if (sd.name == name) { has_def = true; break; }
            if (!has_def)
                inds.push_back({name.substr(0, 1), CP_HINT, A_DIM});
        }
        return inds;
    }

    int indicators_width(const std::vector<Indicator>& inds) {
        if (inds.empty()) return 0;
        int w = 0;
        for (auto& i : inds) w += (int)i.text.size();
        w += (int)inds.size();
        return w;
    }

    void render_indicators(const std::vector<Indicator>& inds, int y, int right_x) {
        int total = indicators_width(inds);
        int x = right_x - total;
        for (auto& ind : inds) {
            if (x >= 0) move(y, x);
            addch(' ');
            ++x;
            attron(COLOR_PAIR(ind.cp) | ind.attr);
            addstr(ind.text.c_str());
            attroff(COLOR_PAIR(ind.cp) | ind.attr);
            x += (int)ind.text.size();
        }
    }

    // ── List Rendering ──────────────────────────────────────

    void draw_list(const std::vector<DisplayItem>& items, int start_y, int ch) {
        if (items.empty()) {
            move(start_y + ch / 2, 0);
            attron(A_DIM);
            const char* msg = (mode == LIST)    ? "  No tasks. Press 'a' to add one."
                            : (mode == DONE)    ? "  No completed tasks."
                            :                     "  Archive is empty.";
            addstr(msg);
            attroff(A_DIM);
            return;
        }

        int y = start_y;
        for (int i = scroll; i < (int)items.size() && y < start_y + ch; ++i) {
            auto& item = items[i];
            bool sel = (i == cursor);
            if (item.is_folder) {
                draw_folder_row(item, y, sel);
                ++y;
            } else {
                draw_task_row(*item.task, y, sel, item.indent);
            }
        }
    }

    void draw_folder_row(const DisplayItem& item, int y, bool sel) {
        move(y, 0);
        if (sel) attron(COLOR_PAIR(CP_SELECTED) | A_BOLD);
        addch(sel ? '>' : ' ');

        bool exp = expanded_folders.count(item.folder_name);
        attron(COLOR_PAIR(CP_FOLDER));
        addstr(exp ? "[-]" : "[+]");
        attroff(COLOR_PAIR(CP_FOLDER));
        addch(' ');

        attron(COLOR_PAIR(CP_FOLDER));
        std::string lbl = item.folder_name + " (" + std::to_string(item.folder_count) + ")";
        addstr(trunc(lbl, COLS - 6).c_str());
        attroff(COLOR_PAIR(CP_FOLDER));

        if (sel) attroff(COLOR_PAIR(CP_SELECTED) | A_BOLD);
    }

    void draw_task_row(Task& t, int& y, bool sel, int indent) {
        auto inds = build_indicators(t);
        int ind_w = indicators_width(inds);
        int prefix = 1 + indent + 3 + 1 + 1 + 1;
        int sep_w = (ind_w > 0) ? 3 : 0;
        int text_avail = COLS - prefix - ind_w - sep_w;

        std::string display = t.text;
        for (auto& tag : t.tags) display += " +" + tag;
        auto lines = word_wrap(display, std::max(text_avail, 5));

        // ── First line ──
        move(y, 0);
        if (sel) attron(COLOR_PAIR(CP_SELECTED) | A_BOLD);
        addch(sel ? '>' : ' ');

        for (int i = 0; i < indent; ++i) addch(' ');

        if (t.done) {
            attron(COLOR_PAIR(CP_DONE));
            addstr("[x]");
            attroff(COLOR_PAIR(CP_DONE));
        } else {
            addstr("[ ]");
        }
        addch(' ');

        switch (t.priority) {
            case 1: attron(COLOR_PAIR(CP_PRI_LOW));    addch('~'); attroff(COLOR_PAIR(CP_PRI_LOW)); break;
            case 2: attron(COLOR_PAIR(CP_PRI_HIGH));   addch('!'); attroff(COLOR_PAIR(CP_PRI_HIGH)); break;
            case 3: attron(COLOR_PAIR(CP_PRI_URGENT)); addch('*'); attroff(COLOR_PAIR(CP_PRI_URGENT)); break;
            default: addch(' '); break;
        }
        if (sel) attron(COLOR_PAIR(CP_SELECTED) | A_BOLD);
        addch(' ');

        std::string first_line = lines.empty() ? "" : lines[0];
        int tag_start = (int)t.text.size();
        int main_len = std::min(tag_start, (int)first_line.size());

        if (t.done) attron(COLOR_PAIR(CP_DONE));
        addnstr(first_line.c_str(), main_len);
        if (t.done) attroff(COLOR_PAIR(CP_DONE));

        if ((int)first_line.size() > main_len) {
            if (!sel) attron(COLOR_PAIR(CP_TAG) | A_DIM);
            else attron(COLOR_PAIR(CP_TAG));
            addstr(first_line.c_str() + main_len);
            if (!sel) attroff(COLOR_PAIR(CP_TAG) | A_DIM);
            else attroff(COLOR_PAIR(CP_TAG));
        }

        // Dashed separator (selected line only)
        if (ind_w > 0) {
            if (sel) {
                int text_end_x = prefix + (int)first_line.size();
                int ind_start_x = COLS - ind_w;
                int dash_count = ind_start_x - text_end_x - 2;
                attroff(COLOR_PAIR(CP_SELECTED) | A_BOLD);
                if (dash_count > 1) {
                    attron(COLOR_PAIR(CP_HINT) | A_DIM);
                    move(y, text_end_x + 1);
                    for (int d = 0; d < dash_count; ++d) addch('-');
                    attroff(COLOR_PAIR(CP_HINT) | A_DIM);
                }
            }
            render_indicators(inds, y, COLS);
        }
        if (sel) attroff(COLOR_PAIR(CP_SELECTED) | A_BOLD);
        ++y;

        // ── Continuation lines ──
        for (int li = 1; li < (int)lines.size(); ++li) {
            if (y >= LINES - 3) break;
            move(y, prefix);
            if (sel) attron(COLOR_PAIR(CP_SELECTED) | A_BOLD);

            int chars_before = 0;
            for (int pi = 0; pi < li; ++pi) chars_before += (int)lines[pi].size() + 1;
            int remain_main = tag_start - chars_before;

            if (remain_main >= (int)lines[li].size()) {
                if (t.done) attron(COLOR_PAIR(CP_DONE));
                addstr(lines[li].c_str());
                if (t.done) attroff(COLOR_PAIR(CP_DONE));
            } else if (remain_main > 0) {
                if (t.done) attron(COLOR_PAIR(CP_DONE));
                addnstr(lines[li].c_str(), remain_main);
                if (t.done) attroff(COLOR_PAIR(CP_DONE));
                if (!sel) attron(COLOR_PAIR(CP_TAG) | A_DIM);
                else attron(COLOR_PAIR(CP_TAG));
                addstr(lines[li].c_str() + remain_main);
                if (!sel) attroff(COLOR_PAIR(CP_TAG) | A_DIM);
                else attroff(COLOR_PAIR(CP_TAG));
            } else {
                if (!sel) attron(COLOR_PAIR(CP_TAG) | A_DIM);
                else attron(COLOR_PAIR(CP_TAG));
                addstr(lines[li].c_str());
                if (!sel) attroff(COLOR_PAIR(CP_TAG) | A_DIM);
                else attroff(COLOR_PAIR(CP_TAG));
            }

            if (sel) attroff(COLOR_PAIR(CP_SELECTED) | A_BOLD);
            ++y;
        }

        // ── Expanded: description, subtasks, notes ──
        if (expanded.count(t.id)) {
            int sub_indent = prefix;

            if (!t.description.empty()) {
                auto desc_lines = word_wrap(t.description, std::max(COLS - sub_indent - 2, 5));
                for (auto& dl : desc_lines) {
                    if (y >= LINES - 3) break;
                    move(y, sub_indent);
                    attron(COLOR_PAIR(CP_HINT));
                    addstr(dl.c_str());
                    attroff(COLOR_PAIR(CP_HINT));
                    ++y;
                }
            }

            for (auto& sub : t.subtasks) {
                if (y >= LINES - 3) break;
                move(y, sub_indent);
                if (sub.done) {
                    attron(COLOR_PAIR(CP_DONE) | A_DIM);
                    addstr(("[x] " + trunc(sub.text, COLS - sub_indent - 5)).c_str());
                    attroff(COLOR_PAIR(CP_DONE) | A_DIM);
                } else {
                    attron(A_DIM);
                    addstr(("[ ] " + trunc(sub.text, COLS - sub_indent - 5)).c_str());
                    attroff(A_DIM);
                }
                ++y;
            }
            for (auto& note : t.notes) {
                if (y >= LINES - 3) break;
                move(y, sub_indent);
                attron(COLOR_PAIR(CP_NOTE) | A_DIM);
                std::string nl = format_short(note.timestamp) + " " + note.text;
                addstr(trunc(nl, COLS - sub_indent - 1).c_str());
                attroff(COLOR_PAIR(CP_NOTE) | A_DIM);
                ++y;
            }
        }
    }

    // ── Detail View ─────────────────────────────────────────

    void draw_detail(int start_y, int ch) {
        auto display = build_display();
        Task* tp = nullptr;
        if (cursor >= 0 && cursor < (int)display.size() && !display[cursor].is_folder)
            tp = display[cursor].task;
        if (!tp) { mode = LIST; return; }
        Task& t = *tp;

        int y = start_y;
        int w = COLS - 2;

        auto line = [&](const std::string& s, int cp = 0, int attr = 0) {
            if (y >= start_y + ch) return;
            move(y, 1);
            if (cp || attr) attron(COLOR_PAIR(cp) | attr);
            addstr(trunc(s, w).c_str());
            if (cp || attr) attroff(COLOR_PAIR(cp) | attr);
            ++y;
        };

        auto wrapped = [&](const std::string& s, int indent, int cp = 0) {
            for (auto& l : word_wrap(s, w - indent)) {
                if (y >= start_y + ch) return;
                move(y, 1 + indent);
                if (cp) attron(COLOR_PAIR(cp));
                addstr(trunc(l, w - indent).c_str());
                if (cp) attroff(COLOR_PAIR(cp));
                ++y;
            }
        };

        std::string sep(std::min(w, 40), '-');
        line(sep, CP_HINT, A_DIM);
        ++y;

        wrapped(t.text, 1, t.done ? CP_DONE : 0);

        if (!t.description.empty()) {
            ++y;
            wrapped(t.description, 2, CP_HINT);
        }
        ++y;

        const char* pri_names[] = {"normal", "low", "high", "urgent"};
        if (t.priority > 0 && t.priority <= 3) {
            int pcp[] = {0, CP_PRI_LOW, CP_PRI_HIGH, CP_PRI_URGENT};
            line(std::string("Priority: ") + pri_names[t.priority], pcp[t.priority]);
        }

        if (!t.folder.empty())
            line("Folder: " + t.folder, CP_FOLDER);

        if (!t.tags.empty()) {
            std::string tl = "Tags:";
            for (auto& tag : t.tags) tl += " +" + tag;
            line(tl, CP_TAG);
        }

        if (t.done && !t.completed_at.empty())
            line("Completed: " + format_date(t.completed_at), CP_DONE);
        line("Created: " + format_date(t.created), CP_HINT, A_DIM);

        if (!t.subtasks.empty()) {
            ++y;
            int sub_done = 0;
            for (auto& s : t.subtasks) if (s.done) ++sub_done;
            line("Subtasks (" + std::to_string(sub_done) + "/" +
                 std::to_string(t.subtasks.size()) + "):", CP_HINT, A_DIM);
            for (int si = 0; si < (int)t.subtasks.size(); ++si) {
                if (y >= start_y + ch) break;
                auto& sub = t.subtasks[si];
                std::string sl = "  " + std::to_string(si + 1) + ". ";
                sl += sub.done ? "[x] " : "[ ] ";
                sl += sub.text;
                line(sl, sub.done ? CP_DONE : 0);
            }
        }

        if (!t.statuses.empty()) {
            ++y;
            for (auto& [name, val] : t.statuses) {
                int cp = CP_HINT;
                for (auto& sd : state.status_defs)
                    if (sd.name == name) { cp = status_cp(sd.color); break; }
                wrapped(name + ": " + val, 1, cp);
            }
        }

        if (!t.notes.empty()) {
            ++y;
            line("Notes:", CP_HINT, A_DIM);
            for (int ni = 0; ni < (int)t.notes.size(); ++ni) {
                if (y >= start_y + ch) break;
                auto& note = t.notes[ni];
                std::string nl = "  " + std::to_string(ni + 1) + ". " +
                    format_short(note.timestamp) + " " + note.text;
                wrapped(nl, 0, CP_NOTE);
            }
        }

        ++y;
        line(sep, CP_HINT, A_DIM);
    }

    // ── Help View ───────────────────────────────────────────

    void draw_help(int start_y, int ch) {
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

        section("Navigation");
        bind("j/k", "Move up/down");
        bind("g/G", "Top/bottom");
        bind("^D/^U", "Page down/up");
        bind("Enter", "Open / expand folder");
        bind("Esc", "Back / clear filter");

        section("Tasks");
        bind("a", "Add task");
        bind("e", "Edit title");
        bind("D", "Edit description");
        bind("x / Space", "Toggle done");
        bind("d", "Delete (detail: menu)");
        bind("p", "Cycle priority");
        bind("Shift+Arr", "Reorder task");

        section("Organize");
        bind("t", "Add/toggle tag");
        bind("c", "Add subtask");
        bind("1-9", "Toggle subtask (detail)");
        bind("m", "Move to folder");
        bind("n", "Add note");
        bind("N", "Delete last note");
        bind("o", "Expand inline");
        bind("O", "Expand/collapse all");
        bind("s", "Set/clear status");

        section("View");
        bind("f", "Filter by tag");
        bind("/", "Search");
        bind("Tab", "Show completed");
        bind("A", "Show archive");
        bind("S", "Archive all done");
        bind("u", "Undo");
        bind("?", "This help");
        bind("q", "Quit / back");
    }

    // ── Filter View ─────────────────────────────────────────

    void draw_filter(int start_y, int ch) {
        auto tags = all_tags();
        if (tags.empty()) { mode = LIST; return; }
        if (filter_cursor >= (int)tags.size()) filter_cursor = (int)tags.size() - 1;

        int y = start_y;

        move(y, 1);
        attron(A_DIM);
        addstr("Space=include  !=exclude  Enter/q=apply");
        attroff(A_DIM);
        y += 2;

        for (int i = 0; i < (int)tags.size() && y < start_y + ch; ++i) {
            move(y, 2);
            bool sel = (i == filter_cursor);
            if (sel) attron(COLOR_PAIR(CP_SELECTED) | A_BOLD);

            if (filter_include.count(tags[i])) {
                attron(COLOR_PAIR(CP_GREEN));
                addstr("[+] ");
                attroff(COLOR_PAIR(CP_GREEN));
            } else if (filter_exclude.count(tags[i])) {
                attron(COLOR_PAIR(CP_RED));
                addstr("[!] ");
                attroff(COLOR_PAIR(CP_RED));
            } else {
                addstr("[ ] ");
            }

            if (sel) attron(COLOR_PAIR(CP_SELECTED) | A_BOLD);
            addstr(trunc(tags[i], COLS - 12).c_str());

            int tag_count = 0;
            for (auto& t : state.tasks)
                for (auto& tt : t.tags) if (tt == tags[i]) { ++tag_count; break; }
            std::string cnt = " (" + std::to_string(tag_count) + ")";
            attron(A_DIM);
            addstr(cnt.c_str());
            attroff(A_DIM);

            if (sel) attroff(COLOR_PAIR(CP_SELECTED) | A_BOLD);
            ++y;
        }
    }

    // ── Footer ──────────────────────────────────────────────

    void draw_footer() {
        std::string hints;
        switch (mode) {
            case LIST:
                hints = "a:add e:edit x:done d:del p:pri t:tag c:sub D:desc";
                hints += " n:note o:expand O:all s:status m:move";
                hints += " /:find f:filter Tab:done A:arch S:archAll u:undo ?:help q:quit";
                if (has_active_filters()) hints = "Esc:clear " + hints;
                break;
            case DONE:
                hints = "x:reopen d:del S:archive Tab:back";
                if (has_active_filters()) hints = "Esc:clear " + hints;
                hints += " q:back";
                break;
            case ARCHIVE:
                hints = "x:unarchive d:del q:back";
                break;
            case DETAIL:
                hints = "e:edit D:desc x:done p:pri t:tag c:sub 1-9:subtask";
                hints += " n:note N:del-note s:status m:move d:delete ?:help q:back";
                break;
            case HELP:
                hints = "q:back";
                break;
            case FILTER:
                hints = "j/k:navigate Space:include !:exclude Enter/q:done";
                break;
        }
        draw_status_and_hints(hints);
    }

    // ── Input Handling ──────────────────────────────────────

    bool handle_list(int ch) {
        auto display = build_display();
        int count = (int)display.size();

        switch (ch) {
            case 'j': case KEY_DOWN: if (cursor < count - 1) ++cursor; break;
            case 'k': case KEY_UP:   if (cursor > 0) --cursor; break;
            case 'g': case KEY_HOME: cursor = 0; break;
            case 'G': case KEY_END:  cursor = std::max(0, count - 1); break;
            case 4:  cursor = std::min(cursor + 10, std::max(0, count - 1)); break;
            case 21: cursor = std::max(cursor - 10, 0); break;

            case '\n': case KEY_ENTER: {
                if (count == 0) break;
                auto& item = display[cursor];
                if (item.is_folder) {
                    if (expanded_folders.count(item.folder_name))
                        expanded_folders.erase(item.folder_name);
                    else
                        expanded_folders.insert(item.folder_name);
                } else {
                    mode = DETAIL;
                }
                break;
            }

            case 'a': {
                if (mode != LIST) break;
                std::string text = text_input("Add: ");
                if (text.empty()) break;
                push_undo();
                Task t;
                t.id = state.next_id++;
                t.tags = extract_tags(text);
                t.text = text;
                t.created = now_iso();
                if (cursor >= 0 && cursor < count) {
                    auto& item = display[cursor];
                    if (item.is_folder) {
                        t.folder = item.folder_name;
                        expanded_folders.insert(item.folder_name);
                    } else if (item.task && !item.task->folder.empty()) {
                        t.folder = item.task->folder;
                    }
                }
                state.tasks.push_back(t);
                save();
                flash("Added: " + t.text);
                auto disp2 = build_display();
                for (int i = 0; i < (int)disp2.size(); ++i)
                    if (!disp2[i].is_folder && disp2[i].task && disp2[i].task->id == t.id)
                        { cursor = i; break; }
                break;
            }

            case 'e': {
                if (count == 0 || mode == ARCHIVE) break;
                auto& item = display[cursor];
                if (item.is_folder) break;
                Task* t = item.task;
                std::string edit = t->text;
                for (auto& tag : t->tags) edit += " +" + tag;
                std::string result = text_input("Edit: ", edit);
                if (result.empty()) break;
                push_undo();
                t->tags = extract_tags(result);
                t->text = result;
                save();
                flash("Edited");
                break;
            }

            case 'D': {
                if (count == 0 || mode == ARCHIVE) break;
                auto& item = display[cursor];
                if (item.is_folder) break;
                std::string result = text_input("Desc: ", item.task->description);
                push_undo();
                item.task->description = result;
                save();
                flash(result.empty() ? "Description cleared" : "Description set");
                break;
            }

            case 'x': case ' ': {
                if (count == 0) break;
                auto& item = display[cursor];
                if (item.is_folder) break;
                // Unarchive: move task back to active list
                if (mode == ARCHIVE) {
                    push_undo();
                    Task* t = item.task;
                    int id = t->id;
                    t->done = false;
                    t->completed_at.clear();
                    state.tasks.push_back(*t);
                    state.archive.erase(std::remove_if(state.archive.begin(), state.archive.end(),
                        [id](const Task& task) { return task.id == id; }), state.archive.end());
                    save();
                    clamp_cursor((int)build_display().size());
                    flash("Unarchived");
                    break;
                }
                push_undo();
                Task* t = item.task;
                t->done = !t->done;
                if (t->done) {
                    t->completed_at = now_iso();
                    std::string note = text_input("Completion note (optional): ");
                    if (!note.empty()) t->notes.push_back({note, now_iso()});
                    flash("Done: " + t->text);
                } else {
                    t->completed_at.clear();
                    flash("Reopened: " + t->text);
                }
                save();
                break;
            }

            case 'd': {
                if (count == 0) break;
                auto& item = display[cursor];
                if (item.is_folder) {
                    if (!confirm("Remove folder? Tasks become unfiled.")) break;
                    push_undo();
                    for (auto& t : state.tasks)
                        if (t.folder == item.folder_name) t.folder.clear();
                    expanded_folders.erase(item.folder_name);
                    save();
                    flash("Removed folder: " + item.folder_name);
                } else {
                    Task* t = item.task;
                    if (!confirm("Delete \"" + trunc(t->text, 30) + "\"?")) break;
                    push_undo();
                    int id = t->id;
                    auto& src = (mode == ARCHIVE) ? state.archive : state.tasks;
                    src.erase(std::remove_if(src.begin(), src.end(),
                        [id](const Task& task) { return task.id == id; }), src.end());
                    save();
                    flash("Deleted");
                }
                clamp_cursor((int)build_display().size());
                break;
            }

            case 'p': {
                if (count == 0 || mode == ARCHIVE) break;
                auto& item = display[cursor];
                if (item.is_folder) break;
                push_undo();
                item.task->priority = (item.task->priority + 1) % 4;
                const char* names[] = {"normal", "low", "high", "urgent"};
                flash(std::string("Priority: ") + names[item.task->priority]);
                save();
                break;
            }

            case 't': {
                if (count == 0 || mode == ARCHIVE) break;
                auto& item = display[cursor];
                if (item.is_folder) break;
                Task* task = item.task;
                std::string prompt = "Tag";
                if (!task->tags.empty()) {
                    prompt += " (current:";
                    for (auto& tg : task->tags) prompt += " " + tg;
                    prompt += ")";
                }
                prompt += ": ";
                std::string tag = text_input(prompt);
                if (tag.empty()) break;
                push_undo();
                auto it = std::find(task->tags.begin(), task->tags.end(), tag);
                if (it != task->tags.end()) {
                    task->tags.erase(it);
                    flash("Removed tag: " + tag);
                } else {
                    task->tags.push_back(tag);
                    flash("Added tag: " + tag);
                }
                save();
                break;
            }

            case 'c': {
                if (count == 0 || mode == ARCHIVE) break;
                auto& item = display[cursor];
                if (item.is_folder) break;
                std::string text = text_input("Subtask: ");
                if (text.empty()) break;
                push_undo();
                Subtask sub;
                sub.id = state.next_id++;
                sub.text = text;
                item.task->subtasks.push_back(sub);
                save();
                flash("Subtask added");
                break;
            }

            case 'n': {
                if (count == 0 || mode == ARCHIVE) break;
                auto& item = display[cursor];
                if (item.is_folder) break;
                std::string text = text_input("Note: ");
                if (text.empty()) break;
                push_undo();
                item.task->notes.push_back({text, now_iso()});
                save();
                flash("Note added");
                break;
            }

            case 'N': {
                if (count == 0 || mode == ARCHIVE) break;
                auto& item = display[cursor];
                if (item.is_folder) break;
                if (item.task->notes.empty()) { flash("No notes"); break; }
                if (!confirm("Delete last note?")) break;
                push_undo();
                item.task->notes.pop_back();
                save();
                flash("Note deleted");
                break;
            }

            case 'o': {
                if (count == 0) break;
                auto& item = display[cursor];
                if (item.is_folder) {
                    if (expanded_folders.count(item.folder_name))
                        expanded_folders.erase(item.folder_name);
                    else
                        expanded_folders.insert(item.folder_name);
                } else {
                    int id = item.task->id;
                    if (expanded.count(id)) expanded.erase(id);
                    else expanded.insert(id);
                }
                break;
            }

            case 'O': {
                auto& src = (mode == ARCHIVE) ? state.archive : state.tasks;
                bool all_expanded = true;
                for (auto& t : src) {
                    if (!passes_filter(t)) continue;
                    if (!t.folder.empty() && !expanded_folders.count(t.folder))
                        { all_expanded = false; break; }
                    if (!expanded.count(t.id))
                        { all_expanded = false; break; }
                }
                if (all_expanded) {
                    expanded.clear();
                    expanded_folders.clear();
                    flash("Collapsed all");
                } else {
                    for (auto& t : src) {
                        if (!passes_filter(t)) continue;
                        expanded.insert(t.id);
                        if (!t.folder.empty())
                            expanded_folders.insert(t.folder);
                    }
                    flash("Expanded all");
                }
                break;
            }

            case 's': {
                if (count == 0 || mode == ARCHIVE) break;
                auto& item = display[cursor];
                if (item.is_folder) break;
                handle_status(item.task);
                break;
            }

            case 'm': {
                if (count == 0 || mode == ARCHIVE) break;
                auto& item = display[cursor];
                if (item.is_folder) break;
                std::set<std::string> fl;
                for (auto& t : state.tasks) if (!t.folder.empty()) fl.insert(t.folder);
                std::string prompt = "Move to folder (";
                int fn = 0;
                for (auto& f : fl) {
                    if (fn++ > 0) prompt += "/";
                    prompt += f;
                    if (fn > 4) { prompt += "/..."; break; }
                }
                if (fn > 0) prompt += ", ";
                prompt += "empty=unfiled): ";
                std::string folder = text_input(prompt, item.task->folder);
                push_undo();
                item.task->folder = folder;
                if (!folder.empty()) expanded_folders.insert(folder);
                save();
                flash(folder.empty() ? "Moved to unfiled" : "Moved to: " + folder);
                break;
            }

            case 'J': case KEY_SF: {
                if (mode != LIST || count < 2) break;
                if (cursor >= count - 1) break;
                auto& cur = display[cursor];
                auto& nxt = display[cursor + 1];
                if (cur.is_folder || nxt.is_folder) break;
                if (!cur.task || !nxt.task) break;
                if (cur.task->folder != nxt.task->folder) break;
                if (has_active_filters()) break;
                push_undo();
                int idx1 = -1, idx2 = -1;
                for (int i = 0; i < (int)state.tasks.size(); ++i) {
                    if (&state.tasks[i] == cur.task) idx1 = i;
                    if (&state.tasks[i] == nxt.task) idx2 = i;
                }
                if (idx1 >= 0 && idx2 >= 0) {
                    std::swap(state.tasks[idx1], state.tasks[idx2]);
                    ++cursor;
                    save();
                }
                break;
            }

            case 'K': case KEY_SR: {
                if (mode != LIST || count < 2) break;
                if (cursor <= 0) break;
                auto& cur = display[cursor];
                auto& prv = display[cursor - 1];
                if (cur.is_folder || prv.is_folder) break;
                if (!cur.task || !prv.task) break;
                if (cur.task->folder != prv.task->folder) break;
                if (has_active_filters()) break;
                push_undo();
                int idx1 = -1, idx2 = -1;
                for (int i = 0; i < (int)state.tasks.size(); ++i) {
                    if (&state.tasks[i] == cur.task) idx1 = i;
                    if (&state.tasks[i] == prv.task) idx2 = i;
                }
                if (idx1 >= 0 && idx2 >= 0) {
                    std::swap(state.tasks[idx1], state.tasks[idx2]);
                    --cursor;
                    save();
                }
                break;
            }

            case 'f': {
                auto tags = all_tags();
                if (tags.empty()) { flash("No tags to filter"); break; }
                filter_cursor = 0;
                mode = FILTER;
                break;
            }

            case '/': {
                std::string q = text_input("Search (Esc=cancel): ", search);
                search = q;
                cursor = 0; scroll = 0;
                break;
            }

            case 27:
                if (has_active_filters()) {
                    filter_include.clear();
                    filter_exclude.clear();
                    search.clear();
                    cursor = 0; scroll = 0;
                    flash("Filters cleared");
                } else if (mode == DONE || mode == ARCHIVE) {
                    mode = LIST; cursor = 0; scroll = 0;
                }
                break;

            case '\t':
                mode = (mode == DONE) ? LIST : DONE;
                cursor = 0; scroll = 0;
                break;

            case 'A':
                mode = (mode == ARCHIVE) ? LIST : ARCHIVE;
                cursor = 0; scroll = 0;
                break;

            case 'S': {
                push_undo();
                int archived = 0;
                for (auto it = state.tasks.begin(); it != state.tasks.end();) {
                    if (it->done) {
                        state.archive.push_back(std::move(*it));
                        it = state.tasks.erase(it);
                        ++archived;
                    } else ++it;
                }
                if (archived > 0) {
                    save();
                    flash("Archived " + std::to_string(archived) + " task(s)");
                    if (mode == DONE) { mode = LIST; cursor = 0; scroll = 0; }
                } else {
                    flash("No completed tasks to archive");
                }
                break;
            }

            case 'u': undo(); break;
            case '?': mode = HELP; break;

            case 'q':
                if (mode == DONE || mode == ARCHIVE) {
                    mode = LIST; cursor = 0; scroll = 0;
                } else {
                    return false;
                }
                break;
        }
        return true;
    }

    // ── Detail View Input ───────────────────────────────────

    bool handle_detail(int ch) {
        auto display = build_display();
        Task* t = nullptr;
        if (cursor >= 0 && cursor < (int)display.size() && !display[cursor].is_folder)
            t = display[cursor].task;
        if (!t) { mode = LIST; return true; }

        if (ch >= '1' && ch <= '9') {
            int idx = ch - '1';
            if (idx < (int)t->subtasks.size()) {
                push_undo();
                t->subtasks[idx].done = !t->subtasks[idx].done;
                save();
                flash(t->subtasks[idx].done ? "Subtask done" : "Subtask reopened");
            }
            return true;
        }

        switch (ch) {
            case 'q': case 27: mode = LIST; break;

            case 'e': {
                std::string edit = t->text;
                for (auto& tag : t->tags) edit += " +" + tag;
                std::string result = text_input("Edit: ", edit);
                if (result.empty()) break;
                push_undo();
                t->tags = extract_tags(result);
                t->text = result;
                save();
                flash("Edited");
                break;
            }

            case 'D': {
                std::string result = text_input("Desc: ", t->description);
                push_undo();
                t->description = result;
                save();
                flash(result.empty() ? "Description cleared" : "Description set");
                break;
            }

            case 'x': case ' ': {
                push_undo();
                t->done = !t->done;
                if (t->done) {
                    t->completed_at = now_iso();
                    std::string note = text_input("Completion note (optional): ");
                    if (!note.empty()) t->notes.push_back({note, now_iso()});
                } else {
                    t->completed_at.clear();
                }
                save();
                flash(t->done ? "Done" : "Reopened");
                break;
            }

            case 'p': {
                push_undo();
                t->priority = (t->priority + 1) % 4;
                const char* names[] = {"normal", "low", "high", "urgent"};
                flash(std::string("Priority: ") + names[t->priority]);
                save();
                break;
            }

            case 't': {
                std::string prompt = "Tag";
                if (!t->tags.empty()) {
                    prompt += " (current:";
                    for (auto& tg : t->tags) prompt += " " + tg;
                    prompt += ")";
                }
                prompt += ": ";
                std::string tag = text_input(prompt);
                if (tag.empty()) break;
                push_undo();
                auto it = std::find(t->tags.begin(), t->tags.end(), tag);
                if (it != t->tags.end()) {
                    t->tags.erase(it);
                    flash("Removed tag: " + tag);
                } else {
                    t->tags.push_back(tag);
                    flash("Added tag: " + tag);
                }
                save();
                break;
            }

            case 'c': {
                std::string text = text_input("Subtask: ");
                if (text.empty()) break;
                push_undo();
                Subtask sub;
                sub.id = state.next_id++;
                sub.text = text;
                t->subtasks.push_back(sub);
                save();
                flash("Subtask added");
                break;
            }

            case 'n': {
                std::string text = text_input("Note: ");
                if (text.empty()) break;
                push_undo();
                t->notes.push_back({text, now_iso()});
                save();
                flash("Note added");
                break;
            }

            case 'N': {
                if (t->notes.empty()) { flash("No notes"); break; }
                if (!confirm("Delete last note?")) break;
                push_undo();
                t->notes.pop_back();
                save();
                flash("Note deleted");
                break;
            }

            case 'm': {
                std::set<std::string> fl;
                for (auto& task : state.tasks) if (!task.folder.empty()) fl.insert(task.folder);
                std::string prompt = "Move to folder (";
                int fn = 0;
                for (auto& f : fl) {
                    if (fn++ > 0) prompt += "/";
                    prompt += f;
                    if (fn > 4) { prompt += "/..."; break; }
                }
                if (fn > 0) prompt += ", ";
                prompt += "empty=unfiled): ";
                std::string folder = text_input(prompt, t->folder);
                push_undo();
                t->folder = folder;
                if (!folder.empty()) expanded_folders.insert(folder);
                save();
                flash(folder.empty() ? "Moved to unfiled" : "Moved to: " + folder);
                break;
            }

            case 's': handle_status(t); break;
            case 'd': handle_delete_menu(t); break;
            case '?': mode = HELP; break;
        }
        return true;
    }

    bool handle_filter(int ch) {
        auto tags = all_tags();
        if (tags.empty()) { mode = LIST; return true; }
        int count = (int)tags.size();

        switch (ch) {
            case 'j': case KEY_DOWN:
                if (filter_cursor < count - 1) ++filter_cursor;
                break;
            case 'k': case KEY_UP:
                if (filter_cursor > 0) --filter_cursor;
                break;
            case 'g': case KEY_HOME:
                filter_cursor = 0;
                break;
            case 'G': case KEY_END:
                filter_cursor = std::max(0, count - 1);
                break;

            case ' ': {
                auto& tag = tags[filter_cursor];
                filter_exclude.erase(tag);
                if (filter_include.count(tag))
                    filter_include.erase(tag);
                else
                    filter_include.insert(tag);
                break;
            }

            case '!': {
                auto& tag = tags[filter_cursor];
                filter_include.erase(tag);
                if (filter_exclude.count(tag))
                    filter_exclude.erase(tag);
                else
                    filter_exclude.insert(tag);
                break;
            }

            case '\n': case KEY_ENTER: case 'q': case 27:
                mode = LIST;
                cursor = 0;
                scroll = 0;
                break;
        }
        return true;
    }

    // ── Delete Menu ─────────────────────────────────────────

    void handle_delete_menu(Task* t) {
        std::string prompt = "Delete: [t]ask";
        if (!t->subtasks.empty()) prompt += " [c]subtask";
        if (!t->notes.empty()) prompt += " [n]ote";
        if (!t->description.empty()) prompt += " [D]esc";
        if (!t->statuses.empty()) prompt += " [s]tatus";
        if (!t->tags.empty()) prompt += " [f]tag";
        prompt += ": ";

        move(LINES - 1, 0); clrtoeol();
        attron(A_BOLD);
        addstr(trunc(prompt, COLS - 1).c_str());
        attroff(A_BOLD);
        refresh();

        int ch;
        while (true) { ch = getch(); if (ch != ERR) break; }
        if (ch == 27) return;

        switch (ch) {
            case 't': {
                if (!confirm("Delete this task?")) break;
                push_undo();
                int id = t->id;
                state.tasks.erase(std::remove_if(state.tasks.begin(), state.tasks.end(),
                    [id](const Task& task) { return task.id == id; }), state.tasks.end());
                save();
                mode = LIST;
                clamp_cursor((int)build_display().size());
                flash("Deleted");
                break;
            }
            case 'c': {
                if (t->subtasks.empty()) break;
                int idx = pick_number("Delete subtask (1-" +
                    std::to_string(t->subtasks.size()) + "): ", (int)t->subtasks.size());
                if (idx < 0) break;
                push_undo();
                t->subtasks.erase(t->subtasks.begin() + idx);
                save();
                flash("Subtask deleted");
                break;
            }
            case 'n': {
                if (t->notes.empty()) break;
                int idx = pick_number("Delete note (1-" +
                    std::to_string(t->notes.size()) + "): ", (int)t->notes.size());
                if (idx < 0) break;
                push_undo();
                t->notes.erase(t->notes.begin() + idx);
                save();
                flash("Note deleted");
                break;
            }
            case 'D': {
                if (t->description.empty()) break;
                push_undo();
                t->description.clear();
                save();
                flash("Description cleared");
                break;
            }
            case 's': {
                if (t->statuses.empty()) break;
                std::string p2 = "Clear status: ";
                for (auto& [name, val] : t->statuses)
                    p2 += "[" + name.substr(0,1) + "]" + name + " ";
                move(LINES - 1, 0); clrtoeol();
                attron(A_BOLD);
                addstr(trunc(p2, COLS - 1).c_str());
                attroff(A_BOLD);
                refresh();
                int ch2;
                while (true) { ch2 = getch(); if (ch2 != ERR) break; }
                for (auto it = t->statuses.begin(); it != t->statuses.end(); ++it) {
                    if (!it->first.empty() && ch2 == it->first[0]) {
                        push_undo();
                        std::string removed = it->first;
                        t->statuses.erase(it);
                        save();
                        flash("Cleared: " + removed);
                        return;
                    }
                }
                break;
            }
            case 'f': {
                if (t->tags.empty()) break;
                std::string p2 = "Remove tag: ";
                for (int i = 0; i < (int)t->tags.size(); ++i)
                    p2 += std::to_string(i+1) + ":" + t->tags[i] + " ";
                int idx = pick_number(p2, (int)t->tags.size());
                if (idx < 0) break;
                push_undo();
                t->tags.erase(t->tags.begin() + idx);
                save();
                flash("Tag removed");
                break;
            }
        }
    }

    // ── Status Menu ─────────────────────────────────────────

    void handle_status(Task* t) {
        std::string prompt = "Status: ";
        for (auto& sd : state.status_defs)
            prompt += "[" + sd.label + "]" + sd.name + " ";
        prompt += "[+]new [-]del: ";

        move(LINES - 1, 0); clrtoeol();
        attron(A_BOLD);
        addstr(trunc(prompt, COLS - 1).c_str());
        attroff(A_BOLD);
        refresh();

        int ch;
        while (true) { ch = getch(); if (ch != ERR) break; }
        if (ch == 27) return;

        for (auto& sd : state.status_defs) {
            if (!sd.label.empty() && ch == sd.label[0]) {
                std::string current;
                if (t->statuses.count(sd.name)) current = t->statuses[sd.name];
                std::string val = text_input(sd.name + " (empty to clear): ", current);
                push_undo();
                if (val.empty()) t->statuses.erase(sd.name);
                else t->statuses[sd.name] = val;
                save();
                flash(val.empty() ? "Cleared " + sd.name : sd.name + ": " + val);
                return;
            }
        }

        if (ch == '+') {
            std::string name = text_input("Status name: ");
            if (name.empty()) return;
            std::string lbl = auto_label(name);
            int color = 1 + ((int)state.status_defs.size() % 6);
            state.status_defs.push_back({name, lbl, color});
            save();
            flash("Added status: " + name + " [" + lbl + "]");
            return;
        }

        if (ch == '-') {
            if (state.status_defs.empty()) return;
            std::string p2 = "Remove: ";
            for (auto& sd : state.status_defs) p2 += "[" + sd.label + "]" + sd.name + " ";
            move(LINES - 1, 0); clrtoeol();
            attron(A_BOLD);
            addstr(trunc(p2, COLS - 1).c_str());
            attroff(A_BOLD);
            refresh();
            int ch2;
            while (true) { ch2 = getch(); if (ch2 != ERR) break; }
            for (auto it = state.status_defs.begin(); it != state.status_defs.end(); ++it) {
                if (!it->label.empty() && ch2 == it->label[0]) {
                    std::string removed = it->name;
                    state.status_defs.erase(it);
                    save();
                    flash("Removed status: " + removed);
                    return;
                }
            }
        }
    }

    std::string auto_label(const std::string& name) {
        std::set<std::string> used;
        for (auto& d : state.status_defs) used.insert(d.label);
        if (!name.empty()) {
            std::string l(1, std::tolower(name[0]));
            if (!used.count(l)) return l;
        }
        for (char c : name) {
            if (!std::isalpha(c)) continue;
            std::string l(1, std::tolower(c));
            if (!used.count(l)) return l;
        }
        for (char c = 'a'; c <= 'z'; ++c) {
            std::string l(1, c);
            if (!used.count(l)) return l;
        }
        return "?";
    }

public:
    const char* id() override { return "td"; }
    const char* label() override { return "todo"; }

    void init() override {
        path = data_path();
        load();
    }

    void draw() override {
        draw_header();
        int ch = LINES - top_y - 4;
        if (ch < 1) ch = 1;

        switch (mode) {
            case LIST: case DONE: case ARCHIVE: {
                auto display = build_display();
                clamp_cursor((int)display.size());
                ensure_visible(display, ch);
                draw_list(display, top_y + 1, ch);
                break;
            }
            case DETAIL: draw_detail(top_y + 1, ch); break;
            case HELP:   draw_help(top_y + 1, ch); break;
            case FILTER: draw_filter(top_y + 1, ch); break;
        }
        draw_footer();
    }

    bool handle(int ch) override {
        switch (mode) {
            case LIST: case DONE: case ARCHIVE: return handle_list(ch);
            case DETAIL: return handle_detail(ch);
            case FILTER: return handle_filter(ch);
            case HELP:
                if (ch == 'q' || ch == '?' || ch == 27) mode = LIST;
                return true;
        }
        return true;
    }
};
