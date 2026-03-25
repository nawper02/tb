/*
 * ┌─────────────────────────────────┐
 * │  td — a terminal to-do manager  │
 * └─────────────────────────────────┘
 *
 * A fast, keyboard-driven TUI for managing tasks.
 * Persistent JSON state. Rich terminal UI via ncurses.
 * State saved to ~/.td.json
 *
 * Build:  g++ -std=c++17 -O2 -o td td.cpp -lncursesw
 * Run:    ./td
 */

#include <ncurses.h>
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <deque>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iomanip>
#include <iostream>
#include <optional>
#include <regex>
#include <sstream>
#include <string>
#include <vector>
#include <set>

// ── Minimal JSON (self-contained, no deps) ─────────────────────────────────
// We roll a tiny JSON reader/writer so there's zero dependency beyond ncurses.

namespace json {

enum Type { Null, Bool, Int, String, Array, Object };

struct Value {
    Type type = Null;
    bool bool_val = false;
    long long int_val = 0;
    std::string str_val;
    std::vector<Value> arr;
    std::vector<std::pair<std::string, Value>> obj;

    Value() = default;
    static Value make_null() { return {}; }
    static Value make_bool(bool b) { Value v; v.type = Bool; v.bool_val = b; return v; }
    static Value make_int(long long i) { Value v; v.type = Int; v.int_val = i; return v; }
    static Value make_string(const std::string& s) { Value v; v.type = String; v.str_val = s; return v; }
    static Value make_array() { Value v; v.type = Array; return v; }
    static Value make_object() { Value v; v.type = Object; return v; }

    Value& operator[](const std::string& key) {
        for (auto& [k, v] : obj) if (k == key) return v;
        obj.emplace_back(key, Value{});
        return obj.back().second;
    }
    const Value* get(const std::string& key) const {
        for (auto& [k, v] : obj) if (k == key) return &v;
        return nullptr;
    }
    bool has(const std::string& key) const { return get(key) != nullptr; }
    void push(Value v) { arr.push_back(std::move(v)); }

    std::string to_str() const { return (type == String) ? str_val : ""; }
    long long to_int() const { return (type == Int) ? int_val : 0; }
    bool to_bool() const { return (type == Bool) ? bool_val : false; }
};

// Writer
static void write_escaped(std::ostream& os, const std::string& s) {
    os << '"';
    for (char c : s) {
        switch (c) {
            case '"':  os << "\\\""; break;
            case '\\': os << "\\\\"; break;
            case '\n': os << "\\n"; break;
            case '\r': os << "\\r"; break;
            case '\t': os << "\\t"; break;
            default:   os << c;
        }
    }
    os << '"';
}

static void write_json(std::ostream& os, const Value& v, int indent = 0) {
    auto pad = [&](int n) { for (int i = 0; i < n; i++) os << ' '; };
    switch (v.type) {
        case Null:   os << "null"; break;
        case Bool:   os << (v.bool_val ? "true" : "false"); break;
        case Int:    os << v.int_val; break;
        case String: write_escaped(os, v.str_val); break;
        case Array:
            if (v.arr.empty()) { os << "[]"; break; }
            os << "[\n";
            for (size_t i = 0; i < v.arr.size(); i++) {
                pad(indent + 2);
                write_json(os, v.arr[i], indent + 2);
                if (i + 1 < v.arr.size()) os << ',';
                os << '\n';
            }
            pad(indent); os << ']';
            break;
        case Object:
            if (v.obj.empty()) { os << "{}"; break; }
            os << "{\n";
            for (size_t i = 0; i < v.obj.size(); i++) {
                pad(indent + 2);
                write_escaped(os, v.obj[i].first);
                os << ": ";
                write_json(os, v.obj[i].second, indent + 2);
                if (i + 1 < v.obj.size()) os << ',';
                os << '\n';
            }
            pad(indent); os << '}';
            break;
    }
}

// Parser
struct Parser {
    const std::string& src;
    size_t pos = 0;

    void skip_ws() {
        while (pos < src.size() && (src[pos] == ' ' || src[pos] == '\n' || src[pos] == '\r' || src[pos] == '\t'))
            pos++;
    }
    char peek() { skip_ws(); return pos < src.size() ? src[pos] : '\0'; }
    char next() { skip_ws(); return pos < src.size() ? src[pos++] : '\0'; }
    bool match(const char* s) {
        skip_ws();
        size_t len = strlen(s);
        if (pos + len <= src.size() && src.compare(pos, len, s) == 0) {
            pos += len; return true;
        }
        return false;
    }

    std::string parse_string() {
        next(); // skip "
        std::string r;
        while (pos < src.size() && src[pos] != '"') {
            if (src[pos] == '\\') {
                pos++;
                if (pos >= src.size()) break;
                switch (src[pos]) {
                    case '"': r += '"'; break;
                    case '\\': r += '\\'; break;
                    case 'n': r += '\n'; break;
                    case 'r': r += '\r'; break;
                    case 't': r += '\t'; break;
                    case '/': r += '/'; break;
                    case 'u': r += '?'; pos += 4; break; // crude
                    default: r += src[pos];
                }
            } else {
                r += src[pos];
            }
            pos++;
        }
        if (pos < src.size()) pos++; // skip closing "
        return r;
    }

    Value parse_value() {
        char c = peek();
        if (c == '"') return Value::make_string(parse_string());
        if (c == '{') return parse_object();
        if (c == '[') return parse_array();
        if (match("true")) return Value::make_bool(true);
        if (match("false")) return Value::make_bool(false);
        if (match("null")) return Value::make_null();
        // number
        skip_ws();
        size_t start = pos;
        if (pos < src.size() && src[pos] == '-') pos++;
        while (pos < src.size() && src[pos] >= '0' && src[pos] <= '9') pos++;
        // skip fractional part
        if (pos < src.size() && src[pos] == '.') {
            pos++;
            while (pos < src.size() && src[pos] >= '0' && src[pos] <= '9') pos++;
        }
        std::string num = src.substr(start, pos - start);
        return Value::make_int(std::atoll(num.c_str()));
    }

    Value parse_object() {
        next(); // {
        auto v = Value::make_object();
        if (peek() == '}') { next(); return v; }
        while (true) {
            std::string key = parse_string();
            next(); // :
            v.obj.emplace_back(key, parse_value());
            if (peek() != ',') break;
            next();
        }
        next(); // }
        return v;
    }

    Value parse_array() {
        next(); // [
        auto v = Value::make_array();
        if (peek() == ']') { next(); return v; }
        while (true) {
            v.arr.push_back(parse_value());
            if (peek() != ',') break;
            next();
        }
        next(); // ]
        return v;
    }
};

Value parse(const std::string& s) {
    Parser p{s};
    return p.parse_value();
}

std::string serialize(const Value& v) {
    std::ostringstream os;
    write_json(os, v);
    return os.str();
}

} // namespace json

// ── Data Types ─────────────────────────────────────────────────────────────

struct Task {
    int id = 0;
    std::string text;
    bool done = false;
    int priority = 0; // 0=normal, 1=high, 2=urgent
    std::string created;
    std::string completed_at;
    std::string note;
    std::vector<std::string> tags;

    json::Value to_json() const {
        auto v = json::Value::make_object();
        v["id"] = json::Value::make_int(id);
        v["text"] = json::Value::make_string(text);
        v["done"] = json::Value::make_bool(done);
        v["priority"] = json::Value::make_int(priority);
        v["created"] = json::Value::make_string(created);
        v["completed_at"] = completed_at.empty() ? json::Value::make_null() : json::Value::make_string(completed_at);
        v["note"] = note.empty() ? json::Value::make_null() : json::Value::make_string(note);
        auto ta = json::Value::make_array();
        for (auto& t : tags) ta.push(json::Value::make_string(t));
        v["tags"] = ta;
        return v;
    }

    static Task from_json(const json::Value& v) {
        Task t;
        if (auto p = v.get("id")) t.id = (int)p->to_int();
        if (auto p = v.get("text")) t.text = p->to_str();
        if (auto p = v.get("done")) t.done = p->to_bool();
        if (auto p = v.get("priority")) t.priority = (int)p->to_int();
        if (auto p = v.get("created")) t.created = p->to_str();
        if (auto p = v.get("completed_at")) t.completed_at = p->to_str();
        if (auto p = v.get("note")) t.note = p->to_str();
        if (auto p = v.get("tags")) {
            for (auto& e : p->arr) t.tags.push_back(e.to_str());
        }
        return t;
    }
};

struct State {
    std::vector<Task> tasks;
    std::vector<Task> archive;
    int next_id = 1;
    std::string created;
};

// ── Time Helpers ───────────────────────────────────────────────────────────

static std::string now_iso() {
    auto now = std::chrono::system_clock::now();
    auto t = std::chrono::system_clock::to_time_t(now);
    std::tm tm{};
    localtime_r(&t, &tm);
    char buf[64];
    std::strftime(buf, sizeof(buf), "%Y-%m-%dT%H:%M:%S", &tm);
    return buf;
}

static std::string format_date(const std::string& iso) {
    if (iso.empty()) return "";
    // parse YYYY-MM-DDTHH:MM:SS
    std::tm tm{};
    if (strptime(iso.c_str(), "%Y-%m-%dT%H:%M:%S", &tm)) {
        char buf[32];
        std::strftime(buf, sizeof(buf), "%Y-%m-%d %H:%M", &tm);
        return buf;
    }
    return iso;
}

static std::string relative_time(const std::string& iso) {
    if (iso.empty()) return "";
    std::tm tm{};
    if (!strptime(iso.c_str(), "%Y-%m-%dT%H:%M:%S", &tm)) return "";
    time_t then = mktime(&tm);
    time_t now = time(nullptr);
    double secs = difftime(now, then);
    if (secs < 60) return "just now";
    if (secs < 3600) return std::to_string((int)(secs / 60)) + "m ago";
    if (secs < 86400) return std::to_string((int)(secs / 3600)) + "h ago";
    if (secs < 604800) return std::to_string((int)(secs / 86400)) + "d ago";
    char buf[16];
    std::strftime(buf, sizeof(buf), "%b %d", &tm);
    return buf;
}

// ── Persistence ────────────────────────────────────────────────────────────

static std::string data_path() {
    const char* home = getenv("HOME");
    if (!home) home = "/tmp";
    return std::string(home) + "/.td.json";
}

static State load_state() {
    State s;
    s.created = now_iso();
    std::ifstream f(data_path());
    if (!f.is_open()) return s;
    std::string content((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
    f.close();
    if (content.empty()) return s;
    try {
        auto root = json::parse(content);
        if (auto p = root.get("next_id")) s.next_id = (int)p->to_int();
        if (auto p = root.get("created")) s.created = p->to_str();
        if (auto p = root.get("tasks")) {
            for (auto& e : p->arr) s.tasks.push_back(Task::from_json(e));
        }
        if (auto p = root.get("archive")) {
            for (auto& e : p->arr) s.archive.push_back(Task::from_json(e));
        }
    } catch (...) {}
    return s;
}

static void save_state(const State& s) {
    auto root = json::Value::make_object();
    root["next_id"] = json::Value::make_int(s.next_id);
    root["created"] = json::Value::make_string(s.created);
    auto tasks = json::Value::make_array();
    for (auto& t : s.tasks) tasks.push(t.to_json());
    root["tasks"] = tasks;
    auto archive = json::Value::make_array();
    for (auto& t : s.archive) archive.push(t.to_json());
    root["archive"] = archive;
    std::ofstream f(data_path());
    f << json::serialize(root);
}

// ── String Helpers ─────────────────────────────────────────────────────────

static std::string str_lower(const std::string& s) {
    std::string r = s;
    std::transform(r.begin(), r.end(), r.begin(), ::tolower);
    return r;
}

static std::vector<std::string> extract_tags(std::string& text) {
    std::vector<std::string> tags;
    std::regex tag_re(R"(\+(\w+))");
    std::smatch m;
    std::string tmp = text;
    while (std::regex_search(tmp, m, tag_re)) {
        tags.push_back(m[1].str());
        tmp = m.suffix().str();
    }
    if (!tags.empty()) {
        text = std::regex_replace(text, tag_re, "");
        // trim
        text.erase(text.find_last_not_of(" \t") + 1);
        text.erase(0, text.find_first_not_of(" \t"));
    }
    return tags;
}

static std::vector<std::string> word_wrap(const std::string& text, int width) {
    std::vector<std::string> lines;
    std::istringstream iss(text);
    std::string word, line;
    while (iss >> word) {
        if (line.empty()) {
            line = word;
        } else if ((int)(line.size() + 1 + word.size()) <= width) {
            line += " " + word;
        } else {
            lines.push_back(line);
            line = word;
        }
    }
    if (!line.empty()) lines.push_back(line);
    if (lines.empty()) lines.push_back("");
    return lines;
}

// ── Color Pairs ────────────────────────────────────────────────────────────

enum Colors {
    C_DONE = 1,
    C_HIGH,
    C_URGENT,
    C_ACCENT,
    C_SELECTION,
    C_DIM,
    C_NOTE,
    C_MSG,
    C_SUCCESS,
    C_HEADER,
};

// ── TUI App ────────────────────────────────────────────────────────────────

static const char* PRIORITY_LABELS[] = { "", "!! ", "** " };
static const char* PRIORITY_NAMES[]  = { "normal", "high", "urgent" };

class TodoApp {
public:
    State state;
    std::deque<State> undo_stack;
    int cursor = 0;
    int scroll_offset = 0;
    std::string mode = "list"; // list, done, detail, help, archive
    std::string filter_tag;
    std::string search_query;
    std::string message;
    std::string message_style;
    time_t message_time = 0;

    void save_undo() {
        undo_stack.push_back(state);
        if (undo_stack.size() > 20) undo_stack.pop_front();
    }

    void undo() {
        if (!undo_stack.empty()) {
            state = undo_stack.back();
            undo_stack.pop_back();
            save_state(state);
            flash("Undone", "success");
        } else {
            flash("Nothing to undo", "info");
        }
    }

    void flash(const std::string& msg, const std::string& style = "info") {
        message = msg;
        message_style = style;
        message_time = time(nullptr);
    }

    std::vector<Task*> visible_tasks() {
        std::vector<Task*> result;
        for (auto& t : state.tasks) {
            if (mode == "done" && !t.done) continue;
            if (!filter_tag.empty()) {
                bool has_tag = false;
                for (auto& tg : t.tags) if (tg == filter_tag) { has_tag = true; break; }
                if (!has_tag) continue;
            }
            if (!search_query.empty()) {
                if (str_lower(t.text).find(str_lower(search_query)) == std::string::npos)
                    continue;
            }
            result.push_back(&t);
        }
        return result;
    }

    void clamp_cursor() {
        auto tasks = visible_tasks();
        int n = (int)tasks.size();
        if (n == 0) cursor = 0;
        else if (cursor >= n) cursor = n - 1;
        else if (cursor < 0) cursor = 0;
    }

    // ── Text Input ─────────────────────────────────────────────────────

    std::optional<std::string> text_input(const std::string& prompt, const std::string& prefill = "") {
        int h, w;
        getmaxyx(stdscr, h, w);
        int y = h - 2;
        curs_set(1);
        std::string buf = prefill;
        int pos = (int)buf.size();

        while (true) {
            move(y, 0);
            clrtoeol();
            attron(A_BOLD | COLOR_PAIR(C_ACCENT));
            addnstr(prompt.c_str(), w - 1);
            attroff(A_BOLD | COLOR_PAIR(C_ACCENT));
            int max_disp = w - (int)prompt.size() - 1;
            if (max_disp > 0) {
                addnstr(buf.c_str(), max_disp);
            }
            int cx = std::min((int)prompt.size() + pos, w - 1);
            move(y, cx);
            refresh();

            int ch = getch();
            if (ch == '\n' || ch == '\r' || ch == KEY_ENTER) {
                curs_set(0);
                // trim
                auto s = buf;
                s.erase(0, s.find_first_not_of(" \t"));
                if (!s.empty()) s.erase(s.find_last_not_of(" \t") + 1);
                return s;
            }
            if (ch == 27) { // ESC
                curs_set(0);
                return std::nullopt;
            }
            if (ch == KEY_BACKSPACE || ch == 127 || ch == 8) {
                if (pos > 0) { buf.erase(pos - 1, 1); pos--; }
            } else if (ch == KEY_DC) {
                if (pos < (int)buf.size()) buf.erase(pos, 1);
            } else if (ch == KEY_LEFT) {
                if (pos > 0) pos--;
            } else if (ch == KEY_RIGHT) {
                if (pos < (int)buf.size()) pos++;
            } else if (ch == KEY_HOME || ch == 1) { // Ctrl-A
                pos = 0;
            } else if (ch == KEY_END || ch == 5) { // Ctrl-E
                pos = (int)buf.size();
            } else if (ch == 21) { // Ctrl-U
                buf.clear(); pos = 0;
            } else if (ch >= 32 && ch <= 126) {
                buf.insert(buf.begin() + pos, (char)ch);
                pos++;
            }
        }
    }

    bool confirm(const std::string& prompt) {
        int h, w;
        getmaxyx(stdscr, h, w);
        int y = h - 2;
        move(y, 0); clrtoeol();
        attron(A_BOLD | COLOR_PAIR(C_URGENT));
        std::string msg = prompt + " (y/n) ";
        addnstr(msg.c_str(), w - 1);
        attroff(A_BOLD | COLOR_PAIR(C_URGENT));
        refresh();
        while (true) {
            int ch = getch();
            if (ch == 'y' || ch == 'Y') return true;
            if (ch == 'n' || ch == 'N' || ch == 27) return false;
        }
    }

    // ── Drawing ────────────────────────────────────────────────────────

    void safe_addnstr(int y, int x, const std::string& s, int maxlen, attr_t attr = 0) {
        int h, w;
        getmaxyx(stdscr, h, w);
        if (y < 0 || y >= h || x < 0 || x >= w) return;
        int avail = std::min(maxlen, w - x);
        if (avail <= 0) return;
        if (attr) attron(attr);
        mvaddnstr(y, x, s.c_str(), avail);
        if (attr) attroff(attr);
    }

    void draw() {
        erase();
        int h, w;
        getmaxyx(stdscr, h, w);
        if (h < 5 || w < 30) {
            mvaddstr(0, 0, "Terminal too small");
            refresh();
            return;
        }
        draw_header(w);
        draw_content(h, w);
        draw_status(h, w);
        refresh();
    }

    void draw_header(int w) {
        int done_count = 0, total = (int)state.tasks.size();
        for (auto& t : state.tasks) if (t.done) done_count++;
        int pending = total - done_count;

        std::string title;
        if (mode == "archive") {
            title = " ARCHIVE (" + std::to_string(state.archive.size()) + " items)";
        } else if (mode == "done") {
            title = " COMPLETED (" + std::to_string(done_count) + ")";
        } else if (mode == "detail") {
            title = " TASK DETAIL";
        } else if (mode == "help") {
            title = " KEYBOARD SHORTCUTS";
        } else {
            title = " " + std::to_string(pending) + " pending";
            if (done_count) title += "  " + std::to_string(done_count) + " done";
        }
        if (!filter_tag.empty()) title += "  [+" + filter_tag + "]";
        if (!search_query.empty()) title += "  [\"" + search_query + "\"]";

        title.resize(w, ' ');
        safe_addnstr(0, 0, title, w, COLOR_PAIR(C_HEADER) | A_BOLD);
    }

    void draw_content(int h, int w) {
        if (mode == "help") { draw_help(h, w); return; }
        if (mode == "detail") { draw_detail(h, w); return; }
        if (mode == "archive") { draw_archive(h, w); return; }
        draw_task_list(h, w);
    }

    void draw_task_list(int h, int w) {
        auto tasks = visible_tasks();
        clamp_cursor();
        int avail = h - 3;

        if (scroll_offset > cursor) scroll_offset = cursor;
        if (cursor >= scroll_offset + avail) scroll_offset = cursor - avail + 1;

        if (tasks.empty()) {
            std::string msg = state.tasks.empty() ? "No tasks yet -- press 'a' to add one" : "No matching tasks";
            safe_addnstr(2, 2, msg, w - 3, A_DIM);
            return;
        }

        for (int i = 0; i < avail; i++) {
            int idx = i + scroll_offset;
            if (idx >= (int)tasks.size()) break;
            Task* task = tasks[idx];
            int y = i + 1;
            bool selected = (idx == cursor);

            std::string checkbox = task->done ? " [x] " : " [ ] ";
            std::string pri_str = (task->priority > 0 && task->priority <= 2) ? PRIORITY_LABELS[task->priority] : "";
            std::string text = pri_str + task->text;

            std::string tags_str;
            for (auto& t : task->tags) tags_str += " +" + t;

            std::string age = relative_time(task->done ? task->completed_at : task->created);
            std::string note_ind = task->note.empty() ? "" : " [n]";
            std::string right = " " + age + note_ind + " ";

            int max_text_w = w - (int)checkbox.size() - (int)right.size() - (int)tags_str.size() - 1;
            if (max_text_w < 10) { max_text_w = w - (int)checkbox.size() - 2; tags_str.clear(); right.clear(); }
            if ((int)text.size() > max_text_w && max_text_w > 1) {
                text = text.substr(0, max_text_w - 1) + "~";
            }

            std::string line = checkbox + text;

            if (selected) {
                std::string full(w, ' ');
                safe_addnstr(y, 0, full, w, COLOR_PAIR(C_SELECTION));
                safe_addnstr(y, 0, line, w - 1, COLOR_PAIR(C_SELECTION) | A_BOLD);
                if (!tags_str.empty() && (int)(line.size() + tags_str.size()) < w - (int)right.size()) {
                    safe_addnstr(y, (int)line.size(), tags_str, w - (int)line.size() - 1, COLOR_PAIR(C_SELECTION));
                }
                if (!right.empty()) {
                    int rx = std::max((int)(line.size() + tags_str.size()), w - (int)right.size());
                    if (rx < w) safe_addnstr(y, rx, right, w - rx, COLOR_PAIR(C_SELECTION));
                }
            } else {
                attr_t attr = 0;
                if (task->done) {
                    attr = COLOR_PAIR(C_DONE) | A_DIM;
                    safe_addnstr(y, 0, line, w - 1, attr);
                } else {
                    safe_addnstr(y, 0, checkbox, w - 1, 0);
                    if (task->priority == 2) attr = COLOR_PAIR(C_URGENT) | A_BOLD;
                    else if (task->priority == 1) attr = COLOR_PAIR(C_HIGH);
                    safe_addnstr(y, (int)checkbox.size(), text, w - (int)checkbox.size() - 1, attr);
                }
                if (!tags_str.empty() && (int)(line.size() + tags_str.size()) < w - (int)right.size()) {
                    safe_addnstr(y, (int)line.size(), tags_str, w - (int)line.size() - 1, COLOR_PAIR(C_ACCENT) | A_DIM);
                }
                if (!right.empty()) {
                    int rx = std::max((int)(line.size() + tags_str.size()), w - (int)right.size());
                    if (rx < w) safe_addnstr(y, rx, right, w - rx, A_DIM);
                }
            }
        }
    }

    void draw_detail(int h, int w) {
        if (cursor >= (int)state.tasks.size()) { mode = "list"; return; }
        Task& task = state.tasks[cursor];
        int y = 2, indent = 3;

        safe_addnstr(y, indent, task.text, w - indent - 1, A_BOLD);
        y += 2;

        struct Field { std::string label, value; };
        std::vector<Field> fields = {
            {"Status", task.done ? "[x] Done" : "[ ] Pending"},
            {"Priority", PRIORITY_NAMES[task.priority]},
            {"Created", format_date(task.created)},
        };
        if (task.done) fields.push_back({"Completed", format_date(task.completed_at)});
        if (!task.tags.empty()) {
            std::string t;
            for (auto& tg : task.tags) t += "+" + tg + " ";
            fields.push_back({"Tags", t});
        }
        for (auto& [label, value] : fields) {
            if (y >= h - 3) break;
            safe_addnstr(y, indent, (label + ":"), 13, A_DIM);
            safe_addnstr(y, indent + 13, value, w - indent - 14, COLOR_PAIR(C_ACCENT));
            y++;
        }
        if (!task.note.empty()) {
            y++;
            if (y < h - 3) {
                safe_addnstr(y, indent, "Note:", 12, A_DIM);
                y++;
                auto lines = word_wrap(task.note, w - indent - 4);
                for (auto& nl : lines) {
                    if (y >= h - 3) break;
                    safe_addnstr(y, indent + 2, nl, w - indent - 3, COLOR_PAIR(C_NOTE));
                    y++;
                }
            }
        }
        y += 2;
        if (y < h - 3) {
            safe_addnstr(y, indent, "Press 'q' or ESC to go back, 'n' to edit note", w - indent - 1, A_DIM);
        }
    }

    void draw_archive(int h, int w) {
        if (state.archive.empty()) {
            safe_addnstr(2, 2, "Archive is empty", w - 3, A_DIM);
            return;
        }
        int avail = h - 3;
        int start = std::max(0, cursor - avail + 1);
        for (int i = 0; i < avail; i++) {
            int idx = start + i;
            if (idx >= (int)state.archive.size()) break;
            Task& task = state.archive[idx];
            int y = i + 1;
            std::string line = " [x] " + task.text;
            std::string right = " " + relative_time(task.completed_at) + " ";
            bool sel = (idx == cursor);

            if (sel) {
                std::string full(w, ' ');
                safe_addnstr(y, 0, full, w, COLOR_PAIR(C_SELECTION));
                safe_addnstr(y, 0, line, w - (int)right.size() - 1, COLOR_PAIR(C_SELECTION));
                int rx = std::max((int)line.size(), w - (int)right.size());
                safe_addnstr(y, rx, right, w - rx, COLOR_PAIR(C_SELECTION));
            } else {
                safe_addnstr(y, 0, line, w - (int)right.size() - 1, COLOR_PAIR(C_DONE) | A_DIM);
                int rx = std::max((int)line.size(), w - (int)right.size());
                safe_addnstr(y, rx, right, w - rx, A_DIM);
            }
        }
    }

    void draw_help(int h, int w) {
        struct Section {
            std::string title;
            std::vector<std::pair<std::string, std::string>> keys;
        };
        std::vector<Section> sections = {
            {"Navigation", {
                {"j / Down", "Move down"},
                {"k / Up", "Move up"},
                {"g / Home", "Jump to top"},
                {"G / End", "Jump to bottom"},
                {"Enter", "View task detail"},
            }},
            {"Actions", {
                {"a", "Add new task"},
                {"x / Space", "Toggle done (prompts for note)"},
                {"e", "Edit task text"},
                {"n", "Add/edit note"},
                {"p", "Cycle priority (normal > high > urgent)"},
                {"d", "Delete task"},
                {"u", "Undo last action"},
            }},
            {"Organization", {
                {"J / Shift+Down", "Move task down"},
                {"K / Shift+Up", "Move task up"},
                {"t", "Filter by tag"},
                {"/", "Search tasks"},
                {"Esc", "Clear filter/search"},
            }},
            {"Views", {
                {"Tab", "Toggle showing completed"},
                {"A", "View archive"},
                {"S", "Archive all completed tasks"},
                {"?", "This help screen"},
                {"q", "Quit"},
            }},
            {"Tips", {
                {"+tag", "Add tags inline: 'Buy milk +groceries'"},
            }},
        };
        int y = 2, indent = 3;
        for (auto& sec : sections) {
            if (y >= h - 3) break;
            safe_addnstr(y, indent, sec.title, w - indent - 1, A_BOLD | COLOR_PAIR(C_ACCENT));
            y++;
            for (auto& [key, desc] : sec.keys) {
                if (y >= h - 3) break;
                std::string k = key;
                while ((int)k.size() < 14) k = " " + k;
                safe_addnstr(y, indent + 1, k, 15, A_BOLD);
                safe_addnstr(y, indent + 16, desc, w - indent - 17, A_DIM);
                y++;
            }
            y++;
        }
    }

    void draw_status(int h, int w) {
        // message bar
        int y = h - 2;
        if (!message.empty() && (time(nullptr) - message_time < 3)) {
            attr_t attr = (message_style == "success") ? COLOR_PAIR(C_SUCCESS) : COLOR_PAIR(C_MSG);
            std::string bar = " " + message;
            bar.resize(w, ' ');
            safe_addnstr(y, 0, bar, w, attr);
        } else {
            message.clear();
        }

        // hint bar
        y = h - 1;
        std::string hints;
        if (mode == "list") hints = "a:add  x:done  e:edit  p:priority  /:search  ?:help  q:quit";
        else if (mode == "done") hints = "Tab:back  S:archive completed  x:undo completion";
        else if (mode == "archive") hints = "q:back  j/k:scroll";
        else if (mode == "help") hints = "q:back";
        else if (mode == "detail") hints = "q:back  n:edit note  e:edit text";
        safe_addnstr(y, 0, " " + hints, w - 1, A_DIM);
    }

    // ── Input ──────────────────────────────────────────────────────────

    bool handle_input(int ch) {
        if (ch == ERR) return true; // timeout

        // Global escape routes
        if (ch == 'q') {
            if (mode == "help" || mode == "detail" || mode == "archive" || mode == "done") {
                mode = "list"; clamp_cursor(); return true;
            }
            return false; // quit
        }
        if (ch == 27) { // ESC
            if (mode != "list") { mode = "list"; clamp_cursor(); return true; }
            if (!filter_tag.empty() || !search_query.empty()) {
                filter_tag.clear(); search_query.clear();
                flash("Filters cleared"); return true;
            }
            return true;
        }

        if (mode == "help") return true;

        if (mode == "archive") {
            if (ch == 'j' || ch == KEY_DOWN) cursor = std::min(cursor + 1, (int)state.archive.size() - 1);
            else if (ch == 'k' || ch == KEY_UP) cursor = std::max(cursor - 1, 0);
            return true;
        }

        if (mode == "detail") {
            if (cursor >= (int)state.tasks.size()) { mode = "list"; return true; }
            Task& task = state.tasks[cursor];
            if (ch == 'n') {
                auto note = text_input("Note: ", task.note);
                if (note.has_value()) {
                    save_undo();
                    task.note = note.value();
                    save_state(state);
                    flash("Note updated", "success");
                }
            } else if (ch == 'e') {
                auto new_text = text_input("Edit: ", task.text);
                if (new_text.has_value() && !new_text->empty()) {
                    save_undo();
                    task.text = new_text.value();
                    save_state(state);
                    flash("Task updated", "success");
                }
            }
            return true;
        }

        // ── List / Done mode ───────────────────────────────────────────

        auto tasks = visible_tasks();
        clamp_cursor();

        // Navigation
        if (ch == 'j' || ch == KEY_DOWN)      cursor = std::min(cursor + 1, std::max((int)tasks.size() - 1, 0));
        else if (ch == 'k' || ch == KEY_UP)   cursor = std::max(cursor - 1, 0);
        else if (ch == 'g' || ch == KEY_HOME) cursor = 0;
        else if (ch == 'G' || ch == KEY_END)  cursor = std::max((int)tasks.size() - 1, 0);

        // Reorder
        else if (ch == 'J' || ch == 336) { // Shift+Down
            if (!tasks.empty() && mode == "list" && filter_tag.empty() && search_query.empty()) {
                auto it = std::find_if(state.tasks.begin(), state.tasks.end(),
                    [&](auto& t) { return &t == tasks[cursor]; });
                if (it != state.tasks.end()) {
                    int ri = (int)(it - state.tasks.begin());
                    if (ri + 1 < (int)state.tasks.size()) {
                        std::swap(state.tasks[ri], state.tasks[ri + 1]);
                        save_state(state);
                        cursor = std::min(cursor + 1, (int)tasks.size() - 1);
                    }
                }
            }
        }
        else if (ch == 'K' || ch == 337) { // Shift+Up
            if (!tasks.empty() && mode == "list" && filter_tag.empty() && search_query.empty()) {
                auto it = std::find_if(state.tasks.begin(), state.tasks.end(),
                    [&](auto& t) { return &t == tasks[cursor]; });
                if (it != state.tasks.end()) {
                    int ri = (int)(it - state.tasks.begin());
                    if (ri > 0) {
                        std::swap(state.tasks[ri], state.tasks[ri - 1]);
                        save_state(state);
                        cursor = std::max(cursor - 1, 0);
                    }
                }
            }
        }

        // Add
        else if (ch == 'a') {
            auto text = text_input("New task: ");
            if (text.has_value() && !text->empty()) {
                save_undo();
                Task t;
                t.id = state.next_id++;
                t.text = text.value();
                t.created = now_iso();
                t.tags = extract_tags(t.text);
                state.tasks.push_back(t);
                save_state(state);
                cursor = (int)visible_tasks().size() - 1;
                flash("Added: " + t.text, "success");
            }
        }

        // Toggle done
        else if (ch == 'x' || ch == ' ') {
            if (!tasks.empty()) {
                Task* task = tasks[cursor];
                save_undo();
                if (task->done) {
                    task->done = false;
                    task->completed_at.clear();
                    task->note.clear();
                    save_state(state);
                    flash("Reopened task", "success");
                } else {
                    auto note = text_input("Completion note (optional, Enter to skip): ");
                    task->done = true;
                    task->completed_at = now_iso();
                    if (note.has_value() && !note->empty()) task->note = note.value();
                    save_state(state);
                    flash("Done!" + (task->note.empty() ? std::string("") : " -- " + task->note), "success");
                }
            }
        }

        // Edit
        else if (ch == 'e') {
            if (!tasks.empty()) {
                Task* task = tasks[cursor];
                auto new_text = text_input("Edit: ", task->text);
                if (new_text.has_value() && !new_text->empty()) {
                    save_undo();
                    task->text = new_text.value();
                    save_state(state);
                    flash("Updated", "success");
                }
            }
        }

        // Note
        else if (ch == 'n') {
            if (!tasks.empty()) {
                Task* task = tasks[cursor];
                auto note = text_input("Note: ", task->note);
                if (note.has_value()) {
                    save_undo();
                    task->note = note.value();
                    save_state(state);
                    flash("Note saved", "success");
                }
            }
        }

        // Priority
        else if (ch == 'p') {
            if (!tasks.empty()) {
                Task* task = tasks[cursor];
                save_undo();
                task->priority = (task->priority + 1) % 3;
                save_state(state);
                flash(std::string("Priority: ") + PRIORITY_NAMES[task->priority], "success");
            }
        }

        // Delete
        else if (ch == 'd') {
            if (!tasks.empty()) {
                Task* task = tasks[cursor];
                if (confirm("Delete '" + task->text + "'?")) {
                    save_undo();
                    state.tasks.erase(
                        std::remove_if(state.tasks.begin(), state.tasks.end(),
                            [&](auto& t) { return t.id == task->id; }),
                        state.tasks.end());
                    save_state(state);
                    clamp_cursor();
                    flash("Deleted", "success");
                }
            }
        }

        // Detail
        else if (ch == '\n' || ch == '\r' || ch == KEY_ENTER) {
            if (!tasks.empty()) {
                Task* task = tasks[cursor];
                auto it = std::find_if(state.tasks.begin(), state.tasks.end(),
                    [&](auto& t) { return t.id == task->id; });
                if (it != state.tasks.end()) cursor = (int)(it - state.tasks.begin());
                mode = "detail";
            }
        }

        // Tab: toggle done view
        else if (ch == 9) {
            mode = (mode == "done") ? "list" : "done";
            cursor = 0; scroll_offset = 0;
        }

        // Archive
        else if (ch == 'A') { mode = "archive"; cursor = 0; }
        else if (ch == 'S') {
            int count = 0;
            for (auto it = state.tasks.begin(); it != state.tasks.end(); ) {
                if (it->done) {
                    state.archive.push_back(*it);
                    it = state.tasks.erase(it);
                    count++;
                } else {
                    ++it;
                }
            }
            if (count) {
                save_state(state);
                flash("Archived " + std::to_string(count) + " completed tasks", "success");
            } else {
                flash("No completed tasks to archive");
            }
            clamp_cursor();
        }

        // Search
        else if (ch == '/') {
            auto query = text_input("Search: ");
            if (query.has_value() && !query->empty()) {
                search_query = query.value();
                cursor = 0; scroll_offset = 0;
            } else {
                search_query.clear();
            }
        }

        // Tag filter
        else if (ch == 't') {
            std::set<std::string> all_tags;
            for (auto& t : state.tasks) for (auto& tg : t.tags) all_tags.insert(tg);
            if (all_tags.empty()) {
                flash("No tags found. Add tags with +tagname");
            } else {
                std::string tag_list;
                for (auto& t : all_tags) tag_list += t + ", ";
                if (tag_list.size() > 2) tag_list.resize(tag_list.size() - 2);
                auto tag = text_input("Filter tag (" + tag_list + "): ");
                if (tag.has_value() && !tag->empty()) {
                    filter_tag = tag.value();
                    if (!filter_tag.empty() && filter_tag[0] == '+') filter_tag.erase(0, 1);
                    cursor = 0;
                } else {
                    filter_tag.clear();
                }
            }
        }

        // Undo
        else if (ch == 'u') { undo(); }

        // Help
        else if (ch == '?') { mode = "help"; }

        return true;
    }

    // ── Main Loop ──────────────────────────────────────────────────────

    void run() {
        state = load_state();
        // curses init
        initscr();
        cbreak();
        noecho();
        keypad(stdscr, TRUE);
        curs_set(0);
        start_color();
        use_default_colors();
        timeout(100);

        init_pair(C_DONE,      COLOR_GREEN,   -1);
        init_pair(C_HIGH,      COLOR_YELLOW,  -1);
        init_pair(C_URGENT,    COLOR_RED,     -1);
        init_pair(C_ACCENT,    COLOR_CYAN,    -1);
        init_pair(C_SELECTION, COLOR_BLACK,   COLOR_WHITE);
        init_pair(C_DIM,       COLOR_WHITE,   -1);
        init_pair(C_NOTE,      COLOR_MAGENTA, -1);
        init_pair(C_MSG,       COLOR_BLACK,   COLOR_YELLOW);
        init_pair(C_SUCCESS,   COLOR_BLACK,   COLOR_GREEN);
        init_pair(C_HEADER,    COLOR_WHITE,   COLOR_BLUE);

        while (true) {
            draw();
            int ch = getch();
            if (!handle_input(ch)) break;
        }

        endwin();
    }
};

// ── Entry Point ────────────────────────────────────────────────────────────

int main() {
    TodoApp app;
    app.run();
    return 0;
}
