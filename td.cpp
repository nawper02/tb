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
#include <set>
#include <sstream>
#include <string>
#include <vector>

// ── Minimal JSON (self-contained, no deps) ─────────────────────────────────

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
        next();
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
                    case 'u': r += '?'; pos += 4; break;
                    default: r += src[pos];
                }
            } else {
                r += src[pos];
            }
            pos++;
        }
        if (pos < src.size()) pos++;
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
        skip_ws();
        size_t start = pos;
        if (pos < src.size() && src[pos] == '-') pos++;
        while (pos < src.size() && src[pos] >= '0' && src[pos] <= '9') pos++;
        if (pos < src.size() && src[pos] == '.') {
            pos++;
            while (pos < src.size() && src[pos] >= '0' && src[pos] <= '9') pos++;
        }
        std::string num = src.substr(start, pos - start);
        return Value::make_int(std::atoll(num.c_str()));
    }

    Value parse_object() {
        next();
        auto v = Value::make_object();
        if (peek() == '}') { next(); return v; }
        while (true) {
            std::string key = parse_string();
            next();
            v.obj.emplace_back(key, parse_value());
            if (peek() != ',') break;
            next();
        }
        next();
        return v;
    }

    Value parse_array() {
        next();
        auto v = Value::make_array();
        if (peek() == ']') { next(); return v; }
        while (true) {
            v.arr.push_back(parse_value());
            if (peek() != ',') break;
            next();
        }
        next();
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

struct NoteEntry {
    std::string text;
    std::string timestamp;

    json::Value to_json() const {
        auto v = json::Value::make_object();
        v["text"] = json::Value::make_string(text);
        v["timestamp"] = json::Value::make_string(timestamp);
        return v;
    }
    static NoteEntry from_json(const json::Value& v) {
        NoteEntry n;
        if (auto p = v.get("text")) n.text = p->to_str();
        if (auto p = v.get("timestamp")) n.timestamp = p->to_str();
        return n;
    }
};

struct Task {
    int id = 0;
    std::string text;
    bool done = false;
    int priority = 0; // 0=normal, 1=low, 2=high, 3=urgent, 4=backlog
    std::string created;
    std::string completed_at;
    std::string note;
    std::vector<NoteEntry> notes;
    std::vector<std::string> tags;
    std::string waiting_on;
    std::string blocked_by;
    bool backlog = false;
    bool notes_expanded = false; // UI-only

    json::Value to_json() const {
        auto v = json::Value::make_object();
        v["id"] = json::Value::make_int(id);
        v["text"] = json::Value::make_string(text);
        v["done"] = json::Value::make_bool(done);
        v["priority"] = json::Value::make_int(priority);
        v["created"] = json::Value::make_string(created);
        v["completed_at"] = completed_at.empty() ? json::Value::make_null() : json::Value::make_string(completed_at);
        v["note"] = note.empty() ? json::Value::make_null() : json::Value::make_string(note);
        auto na = json::Value::make_array();
        for (auto& n : notes) na.push(n.to_json());
        v["notes"] = na;
        auto ta = json::Value::make_array();
        for (auto& t : tags) ta.push(json::Value::make_string(t));
        v["tags"] = ta;
        v["waiting_on"] = waiting_on.empty() ? json::Value::make_null() : json::Value::make_string(waiting_on);
        v["blocked_by"] = blocked_by.empty() ? json::Value::make_null() : json::Value::make_string(blocked_by);
        v["backlog"] = json::Value::make_bool(backlog);
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
        if (auto p = v.get("notes")) {
            for (auto& e : p->arr) t.notes.push_back(NoteEntry::from_json(e));
        }
        if (auto p = v.get("tags")) {
            for (auto& e : p->arr) t.tags.push_back(e.to_str());
        }
        if (auto p = v.get("waiting_on")) t.waiting_on = p->to_str();
        if (auto p = v.get("blocked_by")) t.blocked_by = p->to_str();
        if (auto p = v.get("backlog")) t.backlog = p->to_bool();
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
    std::tm tm{};
    if (strptime(iso.c_str(), "%Y-%m-%dT%H:%M:%S", &tm)) {
        char buf[32];
        std::strftime(buf, sizeof(buf), "%Y-%m-%d %H:%M", &tm);
        return buf;
    }
    return iso;
}

static std::string format_date_short(const std::string& iso) {
    if (iso.empty()) return "";
    std::tm tm{};
    if (strptime(iso.c_str(), "%Y-%m-%dT%H:%M:%S", &tm)) {
        char buf[32];
        std::strftime(buf, sizeof(buf), "%b %d %H:%M", &tm);
        return buf;
    }
    return iso;
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
    std::regex quoted_tag_re(R"(\+\"([^\"]+)\")");
    std::smatch m;
    std::string tmp = text;
    while (std::regex_search(tmp, m, quoted_tag_re)) {
        tags.push_back(m[1].str());
        tmp = m.suffix().str();
    }
    text = std::regex_replace(text, quoted_tag_re, "");
    std::regex word_tag_re(R"(\+(\w+))");
    tmp = text;
    while (std::regex_search(tmp, m, word_tag_re)) {
        tags.push_back(m[1].str());
        tmp = m.suffix().str();
    }
    text = std::regex_replace(text, word_tag_re, "");
    text.erase(text.find_last_not_of(" \t") + 1);
    text.erase(0, text.find_first_not_of(" \t"));
    std::regex multi_space(R"(\s{2,})");
    text = std::regex_replace(text, multi_space, " ");
    return tags;
}

static std::vector<std::string> word_wrap(const std::string& text, int width) {
    std::vector<std::string> lines;
    if (width <= 0) { lines.push_back(text); return lines; }
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
    C_LOW,
    C_BLOCKED,
    C_BACKLOG,
};

// ── TUI App ────────────────────────────────────────────────────────────────

// priority: 0=normal, 1=low, 2=high, 3=urgent, 4=backlog
static const char* PRIORITY_LABELS[] = { "", "~ ", "!! ", "** ", "... " };
static const char* PRIORITY_NAMES[]  = { "normal", "low", "high", "urgent", "backlog" };
static const int NUM_PRIORITIES = 5;

class TodoApp {
public:
    State state;
    std::deque<State> undo_stack;
    int cursor = 0;
    int scroll_offset = 0;
    std::string mode = "list";
    std::string filter_tag;
    std::string search_query;
    std::string message;
    std::string message_style;
    time_t message_time = 0;
    bool show_backlog = false;

    // Hint bar scrolling
    int hint_scroll_offset = 0;
    int hint_frame_counter = 0;

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
            if (!show_backlog && t.backlog && !t.done) continue;
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
                auto s = buf;
                s.erase(0, s.find_first_not_of(" \t"));
                if (!s.empty()) s.erase(s.find_last_not_of(" \t") + 1);
                return s;
            }
            if (ch == 27) {
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
            } else if (ch == KEY_HOME || ch == 1) {
                pos = 0;
            } else if (ch == KEY_END || ch == 5) {
                pos = (int)buf.size();
            } else if (ch == 21) {
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
        int backlog_count = 0;
        for (auto& t : state.tasks) if (t.backlog && !t.done) backlog_count++;

        std::string left, right_str;

        if (mode == "archive") {
            left = " ARCHIVE (" + std::to_string(state.archive.size()) + " items)";
        } else if (mode == "done") {
            left = " COMPLETED (" + std::to_string(done_count) + ")";
        } else if (mode == "detail") {
            left = " TASK DETAIL";
        } else if (mode == "help") {
            left = " KEYBOARD SHORTCUTS";
        } else {
            left = " todo";
            right_str = std::to_string(pending) + " pending";
            if (done_count) right_str += "  " + std::to_string(done_count) + " done";
            if (backlog_count && !show_backlog)
                right_str += "  " + std::to_string(backlog_count) + " backlog";
            if (show_backlog)
                right_str += "  [backlog visible]";
            right_str += " ";
        }

        if (!filter_tag.empty()) {
            std::string tag_info = "  [+" + filter_tag + "]";
            if (!right_str.empty()) right_str = tag_info + "  " + right_str;
            else left += tag_info;
        }
        if (!search_query.empty()) {
            std::string search_info = "  [\"" + search_query + "\"]";
            if (!right_str.empty()) right_str = search_info + "  " + right_str;
            else left += search_info;
        }

        std::string bar;
        if (!right_str.empty()) {
            std::string sep = "::";
            int content_len = (int)left.size() + 2 + (int)sep.size() + 2 + (int)right_str.size();
            if (content_len <= w) {
                int gap = w - (int)left.size() - (int)right_str.size();
                int sep_pos = gap / 2;
                bar = left;
                int before_sep = sep_pos - (int)sep.size() / 2;
                if (before_sep > 0) bar += std::string(before_sep, ' ');
                bar += sep;
                int after_len = w - (int)bar.size() - (int)right_str.size();
                if (after_len > 0) bar += std::string(after_len, ' ');
                bar += right_str;
            } else {
                bar = left;
                bar.resize(w, ' ');
            }
        } else {
            bar = left;
            bar.resize(w, ' ');
        }

        if ((int)bar.size() > w) bar.resize(w);
        safe_addnstr(0, 0, bar, w, COLOR_PAIR(C_HEADER) | A_BOLD);
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
        // We can't easily pre-calculate multi-line heights for scroll clamping,
        // so we just draw and let it naturally fill

        if (tasks.empty()) {
            std::string msg = state.tasks.empty() ? "No tasks yet -- press 'a' to add one" : "No matching tasks";
            safe_addnstr(2, 2, msg, w - 3, A_DIM);
            return;
        }

        int y = 1;

        // First pass: if cursor has scrolled off, adjust scroll_offset
        // We need to figure out how many rows each task before cursor takes
        {
            int test_y = 0;
            for (int idx = scroll_offset; idx < cursor && idx < (int)tasks.size(); idx++) {
                Task* task = tasks[idx];
                int fixed_left = 6; // gutter + checkbox
                int text_w = w - fixed_left - 1;
                if (text_w < 10) text_w = 10;

                std::string pri_str;
                if (task->priority > 0 && task->priority < NUM_PRIORITIES)
                    pri_str = PRIORITY_LABELS[task->priority];
                std::string full_text = pri_str + task->text;

                std::string tags_str;
                for (auto& t : task->tags) {
                    if (t.find(' ') != std::string::npos)
                        tags_str += " +\"" + t + "\"";
                    else
                        tags_str += " +" + t;
                }

                std::string content = full_text + tags_str;
                auto lines = word_wrap(content, text_w);
                int task_lines = (int)lines.size();

                // expanded notes
                if (task->notes_expanded && !task->notes.empty()) {
                    int note_indent = 5;
                    int note_w = w - note_indent - 2;
                    if (note_w < 10) note_w = 10;
                    for (auto& n : task->notes) {
                        std::string ts = format_date_short(n.timestamp);
                        std::string prefix = ts + ": ";
                        auto wrapped = word_wrap(n.text, note_w - (int)prefix.size());
                        task_lines += (int)wrapped.size();
                    }
                }

                test_y += task_lines;
            }
            // If the items before cursor take more than avail, bump scroll_offset
            while (test_y >= avail && scroll_offset < cursor) {
                // remove first visible task's lines
                Task* task = tasks[scroll_offset];
                int fixed_left = 6;
                int text_w = w - fixed_left - 1;
                if (text_w < 10) text_w = 10;
                std::string pri_str;
                if (task->priority > 0 && task->priority < NUM_PRIORITIES)
                    pri_str = PRIORITY_LABELS[task->priority];
                std::string content = pri_str + task->text;
                for (auto& t : task->tags) {
                    if (t.find(' ') != std::string::npos)
                        content += " +\"" + t + "\"";
                    else
                        content += " +" + t;
                }
                auto lines = word_wrap(content, text_w);
                int rm = (int)lines.size();
                if (task->notes_expanded && !task->notes.empty()) {
                    int note_indent = 5;
                    int note_w = w - note_indent - 2;
                    if (note_w < 10) note_w = 10;
                    for (auto& n : task->notes) {
                        std::string ts = format_date_short(n.timestamp);
                        std::string prefix = ts + ": ";
                        auto wrapped = word_wrap(n.text, note_w - (int)prefix.size());
                        rm += (int)wrapped.size();
                    }
                }
                test_y -= rm;
                scroll_offset++;
            }
        }

        for (int idx = scroll_offset; idx < (int)tasks.size() && y < h - 2; idx++) {
            Task* task = tasks[idx];
            bool selected = (idx == cursor);

            std::string gutter = selected ? ">" : " ";
            std::string checkbox = task->done ? " [x] " : " [ ] ";
            std::string pri_str;
            if (task->priority > 0 && task->priority < NUM_PRIORITIES)
                pri_str = PRIORITY_LABELS[task->priority];

            std::string full_text = pri_str + task->text;

            std::string tags_str;
            for (auto& t : task->tags) {
                if (t.find(' ') != std::string::npos)
                    tags_str += " +\"" + t + "\"";
                else
                    tags_str += " +" + t;
            }

            // Right side: pinned status indicators
            std::string right;
            bool has_status = false;
            if (!task->waiting_on.empty()) { right += " [w:" + task->waiting_on + "]"; has_status = true; }
            if (!task->blocked_by.empty()) { right += " [b:" + task->blocked_by + "]"; has_status = true; }
            if (task->backlog)             { right += " [backlog]"; has_status = true; }

            std::string note_ind;
            if (!task->notes.empty()) {
                note_ind = task->notes_expanded
                    ? " [-" + std::to_string(task->notes.size()) + "]"
                    : " [+" + std::to_string(task->notes.size()) + "]";
            } else if (!task->note.empty()) {
                note_ind = " [n]";
            }
            right += note_ind;
            if (!right.empty()) right += " ";

            int fixed_left = 1 + (int)checkbox.size(); // gutter(1) + checkbox(5) = 6
            int fixed_right = (int)right.size();
            int text_area_w = w - fixed_left - fixed_right - 1;
            if (text_area_w < 10) { text_area_w = w - fixed_left - 2; tags_str.clear(); right.clear(); fixed_right = 0; }

            std::string content = full_text + tags_str;
            auto wrapped_lines = word_wrap(content, text_area_w);

            // Determine priority color
            attr_t pri_attr = 0;
            if (task->done) {
                pri_attr = COLOR_PAIR(C_DONE) | A_DIM;
            } else if (task->priority == 4) {
                pri_attr = COLOR_PAIR(C_BACKLOG) | A_DIM;
            } else if (task->priority == 3) {
                pri_attr = COLOR_PAIR(C_URGENT) | A_BOLD;
            } else if (task->priority == 2) {
                pri_attr = COLOR_PAIR(C_HIGH);
            } else if (task->priority == 1) {
                pri_attr = COLOR_PAIR(C_LOW);
            }

            for (int li = 0; li < (int)wrapped_lines.size() && y < h - 2; li++) {
                const std::string& line_text = wrapped_lines[li];

                if (li == 0) {
                    // First line: gutter + checkbox + text + right indicators
                    if (selected) {
                        safe_addnstr(y, 0, gutter, 1, COLOR_PAIR(C_ACCENT) | A_BOLD);
                        safe_addnstr(y, 1, checkbox, w - 1, A_BOLD);
                    } else {
                        safe_addnstr(y, 0, gutter + checkbox, w - 1, task->done ? (COLOR_PAIR(C_DONE) | A_DIM) : (attr_t)0);
                    }

                    // Figure out where full_text ends and tags begin in this line
                    // On the first line, full_text starts at char 0 of content
                    int ft_remaining = (int)full_text.size();
                    int vis_text_chars = std::min(ft_remaining, (int)line_text.size());

                    attr_t text_attr = selected ? (pri_attr | A_BOLD) : pri_attr;
                    if (task->done) text_attr = COLOR_PAIR(C_DONE) | A_DIM;
                    if (task->priority == 4 && !task->done) text_attr = COLOR_PAIR(C_BACKLOG) | A_DIM;

                    if (vis_text_chars > 0) {
                        safe_addnstr(y, fixed_left, line_text.substr(0, vis_text_chars), text_area_w, text_attr);
                    }
                    if (vis_text_chars < (int)line_text.size()) {
                        std::string tag_part = line_text.substr(vis_text_chars);
                        attr_t tag_attr = selected ? COLOR_PAIR(C_ACCENT) : (COLOR_PAIR(C_ACCENT) | A_DIM);
                        safe_addnstr(y, fixed_left + vis_text_chars, tag_part, text_area_w - vis_text_chars, tag_attr);
                    }

                    // Right-side indicators on first line only
                    if (!right.empty() && fixed_right > 0) {
                        int rx = w - fixed_right;
                        if (rx >= fixed_left) {
                            if (has_status)
                                safe_addnstr(y, rx, right, fixed_right, selected ? (COLOR_PAIR(C_BLOCKED) | A_BOLD) : COLOR_PAIR(C_BLOCKED));
                            else
                                safe_addnstr(y, rx, right, fixed_right, A_DIM);
                        }
                    }
                } else {
                    // Continuation lines: indented to align with text start
                    // Figure out how much of full_text has been consumed by previous lines
                    int chars_before = 0;
                    for (int pi = 0; pi < li; pi++) chars_before += (int)wrapped_lines[pi].size();
                    // Account for the space that word_wrap removes between words
                    // Actually word_wrap preserves content length roughly, so:
                    int ft_remaining = (int)full_text.size() - chars_before;
                    if (ft_remaining < 0) ft_remaining = 0;
                    int vis_text_chars = std::min(ft_remaining, (int)line_text.size());

                    attr_t text_attr = selected ? (pri_attr | A_BOLD) : pri_attr;
                    if (task->done) text_attr = COLOR_PAIR(C_DONE) | A_DIM;
                    if (task->priority == 4 && !task->done) text_attr = COLOR_PAIR(C_BACKLOG) | A_DIM;

                    if (vis_text_chars > 0) {
                        safe_addnstr(y, fixed_left, line_text.substr(0, vis_text_chars), text_area_w, text_attr);
                    }
                    if (vis_text_chars < (int)line_text.size()) {
                        std::string tag_part = line_text.substr(vis_text_chars);
                        attr_t tag_attr = selected ? COLOR_PAIR(C_ACCENT) : (COLOR_PAIR(C_ACCENT) | A_DIM);
                        safe_addnstr(y, fixed_left + vis_text_chars, tag_part, text_area_w - vis_text_chars, tag_attr);
                    }
                }

                y++;
            }

            // Expanded notes
            if (task->notes_expanded && !task->notes.empty() && y < h - 2) {
                int note_indent = 5;
                int note_w = w - note_indent - 2;
                if (note_w < 10) note_w = 10;
                for (auto& n : task->notes) {
                    if (y >= h - 2) break;
                    std::string ts = format_date_short(n.timestamp);
                    std::string prefix = ts + ": ";
                    auto wrapped = word_wrap(n.text, note_w - (int)prefix.size());
                    for (int nli = 0; nli < (int)wrapped.size(); nli++) {
                        if (y >= h - 2) break;
                        if (nli == 0) {
                            safe_addnstr(y, note_indent, prefix, w - note_indent - 1, COLOR_PAIR(C_DIM));
                            safe_addnstr(y, note_indent + (int)prefix.size(), wrapped[nli],
                                         w - note_indent - (int)prefix.size() - 1, COLOR_PAIR(C_NOTE));
                        } else {
                            safe_addnstr(y, note_indent + (int)prefix.size(), wrapped[nli],
                                         w - note_indent - (int)prefix.size() - 1, COLOR_PAIR(C_NOTE));
                        }
                        y++;
                    }
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
            for (auto& tg : task.tags) {
                if (tg.find(' ') != std::string::npos)
                    t += "+\"" + tg + "\" ";
                else
                    t += "+" + tg + " ";
            }
            fields.push_back({"Tags", t});
        }
        if (!task.waiting_on.empty()) fields.push_back({"Waiting on", task.waiting_on});
        if (!task.blocked_by.empty()) fields.push_back({"Blocked by", task.blocked_by});
        if (task.backlog) fields.push_back({"Backlog", "yes"});
        for (auto& [label, value] : fields) {
            if (y >= h - 3) break;
            safe_addnstr(y, indent, (label + ":"), 15, A_DIM);
            safe_addnstr(y, indent + 15, value, w - indent - 16, COLOR_PAIR(C_ACCENT));
            y++;
        }

        if (!task.note.empty()) {
            y++;
            if (y < h - 3) {
                safe_addnstr(y, indent, "Completion note:", w - indent - 1, A_DIM);
                y++;
                auto lines = word_wrap(task.note, w - indent - 4);
                for (auto& nl : lines) {
                    if (y >= h - 3) break;
                    safe_addnstr(y, indent + 2, nl, w - indent - 3, COLOR_PAIR(C_NOTE));
                    y++;
                }
            }
        }

        if (!task.notes.empty()) {
            y++;
            if (y < h - 3) {
                safe_addnstr(y, indent, "Notes (" + std::to_string(task.notes.size()) + "):", w - indent - 1, A_DIM);
                y++;
                for (auto& n : task.notes) {
                    if (y >= h - 3) break;
                    std::string ts = format_date_short(n.timestamp);
                    safe_addnstr(y, indent + 2, ts + ":", w - indent - 3, COLOR_PAIR(C_DIM));
                    y++;
                    auto lines = word_wrap(n.text, w - indent - 6);
                    for (auto& nl : lines) {
                        if (y >= h - 3) break;
                        safe_addnstr(y, indent + 4, nl, w - indent - 5, COLOR_PAIR(C_NOTE));
                        y++;
                    }
                }
            }
        }

        y += 2;
        if (y < h - 3) {
            safe_addnstr(y, indent, "q:back  n:add note  e:edit  w:waiting  b:blocked  B:backlog", w - indent - 1, A_DIM);
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
            bool sel = (idx == cursor);

            if (sel) {
                safe_addnstr(y, 0, ">", 1, COLOR_PAIR(C_ACCENT) | A_BOLD);
                safe_addnstr(y, 1, " [x] ", w - 1, A_BOLD);
                safe_addnstr(y, 6, task.text, w - 7, A_BOLD | COLOR_PAIR(C_DONE));
            } else {
                safe_addnstr(y, 0, "  [x] " + task.text, w - 1, COLOR_PAIR(C_DONE) | A_DIM);
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
                {"Ctrl-D", "Jump down 10"},
                {"Ctrl-U", "Jump up 10"},
                {"g / Home", "Jump to top"},
                {"G / End", "Jump to bottom"},
                {"Enter", "View task detail"},
            }},
            {"Actions", {
                {"a", "Add new task"},
                {"x / Space", "Toggle done (prompts for note)"},
                {"e", "Edit task text"},
                {"n", "Add a timestamped note"},
                {"N", "Delete last note"},
                {"o", "Expand/collapse notes inline"},
                {"p", "Cycle priority (normal > low > high > urgent > backlog)"},
                {"w", "Set/clear 'waiting on'"},
                {"b", "Set/clear 'blocked by'"},
                {"B", "Toggle backlog flag"},
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
                {"~", "Toggle showing backlog items"},
                {"A", "View archive"},
                {"S", "Archive all completed tasks"},
                {"?", "This help screen"},
                {"q", "Quit"},
            }},
            {"Tips", {
                {"+tag", "Add single-word tag inline"},
                {"+\"my tag\"", "Add multi-word tag with quotes"},
            }},
        };

        int max_key_w = 0;
        for (auto& sec : sections) {
            for (auto& [key, desc] : sec.keys) {
                if ((int)key.size() > max_key_w) max_key_w = (int)key.size();
            }
        }
        int key_col = 5;
        int desc_col = key_col + max_key_w + 3;

        int y = 2;
        for (auto& sec : sections) {
            if (y >= h - 3) break;
            safe_addnstr(y, 3, sec.title, w - 4, A_BOLD | COLOR_PAIR(C_ACCENT));
            y++;
            for (auto& [key, desc] : sec.keys) {
                if (y >= h - 3) break;
                safe_addnstr(y, key_col, key, max_key_w, A_BOLD);
                safe_addnstr(y, desc_col, desc, w - desc_col - 1, A_DIM);
                y++;
            }
            y++;
        }
    }

    void draw_status(int h, int w) {
        int y = h - 2;
        if (!message.empty() && (time(nullptr) - message_time < 3)) {
            attr_t attr = (message_style == "success") ? COLOR_PAIR(C_SUCCESS) : COLOR_PAIR(C_MSG);
            std::string bar = " " + message;
            bar.resize(w, ' ');
            safe_addnstr(y, 0, bar, w, attr);
        } else {
            message.clear();
        }

        y = h - 1;
        std::string hints;
        if (mode == "list")
            hints = "a:add  x:done  e:edit  n:note  o:expand  p:priority  w:wait  b:block  "
                    "B:backlog  ~:show backlog  J/K:reorder  t:tag filter  /:search  "
                    "Tab:completed  A:archive  S:sweep done  ?:help  q:quit";
        else if (mode == "done")
            hints = "Tab:back  S:archive completed  x:undo completion  q:back";
        else if (mode == "archive")
            hints = "q:back  j/k:scroll";
        else if (mode == "help")
            hints = "q:back";
        else if (mode == "detail")
            hints = "q:back  n:add note  e:edit  w:waiting  b:blocked  B:backlog";

        int display_w = w - 2;
        if (display_w < 1) display_w = 1;

        if ((int)hints.size() <= display_w) {
            safe_addnstr(y, 1, hints, display_w, A_DIM);
        } else {
            std::string gap = "     ";
            std::string ticker = hints + gap;
            int ticker_len = (int)ticker.size();

            // Wall-clock based ticker: one char every 150ms
            static auto last_hint_advance = std::chrono::steady_clock::now();
            auto now_tp = std::chrono::steady_clock::now();
            auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(now_tp - last_hint_advance).count();
            if (ms >= 150) {
                int steps = (int)(ms / 150);
                hint_scroll_offset += steps;
                if (hint_scroll_offset >= ticker_len)
                    hint_scroll_offset %= ticker_len;
                last_hint_advance = now_tp;
            }

            std::string visible;
            visible.reserve(display_w);
            for (int i = 0; i < display_w; i++) {
                int ci = (hint_scroll_offset + i) % ticker_len;
                visible += ticker[ci];
            }
            safe_addnstr(y, 1, visible, display_w, A_DIM);
        }
    }

    // ── Input ──────────────────────────────────────────────────────────

    bool handle_input(int ch) {
        if (ch == ERR) return true;

        // Any keypress resets the hint ticker
        hint_scroll_offset = 0;
        hint_frame_counter = 0;

        if (ch == 'q') {
            if (mode == "help" || mode == "detail" || mode == "archive" || mode == "done") {
                mode = "list"; clamp_cursor(); return true;
            }
            return false;
        }
        if (ch == 27) {
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
                auto note_text = text_input("Add note: ");
                if (note_text.has_value() && !note_text->empty()) {
                    save_undo();
                    NoteEntry entry;
                    entry.text = note_text.value();
                    entry.timestamp = now_iso();
                    task.notes.push_back(entry);
                    save_state(state);
                    flash("Note added", "success");
                }
            } else if (ch == 'e') {
                auto new_text = text_input("Edit: ", task.text);
                if (new_text.has_value() && !new_text->empty()) {
                    save_undo();
                    task.text = new_text.value();
                    save_state(state);
                    flash("Task updated", "success");
                }
            } else if (ch == 'w') {
                auto val = text_input("Waiting on (empty to clear): ", task.waiting_on);
                if (val.has_value()) {
                    save_undo();
                    task.waiting_on = val.value();
                    save_state(state);
                    flash(val->empty() ? "Cleared waiting" : "Waiting on: " + *val, "success");
                }
            } else if (ch == 'b') {
                auto val = text_input("Blocked by (empty to clear): ", task.blocked_by);
                if (val.has_value()) {
                    save_undo();
                    task.blocked_by = val.value();
                    save_state(state);
                    flash(val->empty() ? "Cleared blocked" : "Blocked by: " + *val, "success");
                }
            } else if (ch == 'B') {
                save_undo();
                task.backlog = !task.backlog;
                save_state(state);
                flash(task.backlog ? "Moved to backlog" : "Removed from backlog", "success");
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

        // Fast scroll
        else if (ch == 4)  cursor = std::min(cursor + 10, std::max((int)tasks.size() - 1, 0)); // Ctrl-D
        else if (ch == 21) cursor = std::max(cursor - 10, 0); // Ctrl-U

        // Reorder
        else if (ch == 'J' || ch == 336) {
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
        else if (ch == 'K' || ch == 337) {
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

        // Add timestamped note
        else if (ch == 'n') {
            if (!tasks.empty()) {
                Task* task = tasks[cursor];
                auto note_text = text_input("Add note: ");
                if (note_text.has_value() && !note_text->empty()) {
                    save_undo();
                    NoteEntry entry;
                    entry.text = note_text.value();
                    entry.timestamp = now_iso();
                    task->notes.push_back(entry);
                    task->notes_expanded = true;
                    save_state(state);
                    flash("Note added (" + std::to_string(task->notes.size()) + " total)", "success");
                }
            }
        }

        // Delete last note
        else if (ch == 'N') {
            if (!tasks.empty()) {
                Task* task = tasks[cursor];
                if (!task->notes.empty()) {
                    if (confirm("Delete last note?")) {
                        save_undo();
                        task->notes.pop_back();
                        save_state(state);
                        flash("Note removed", "success");
                    }
                } else {
                    flash("No notes to delete");
                }
            }
        }

        // Toggle notes expand
        else if (ch == 'o') {
            if (!tasks.empty()) {
                Task* task = tasks[cursor];
                if (!task->notes.empty()) {
                    task->notes_expanded = !task->notes_expanded;
                } else {
                    flash("No notes to show. Press 'n' to add one.");
                }
            }
        }

        // Priority cycle (5 levels)
        else if (ch == 'p') {
            if (!tasks.empty()) {
                Task* task = tasks[cursor];
                save_undo();
                task->priority = (task->priority + 1) % NUM_PRIORITIES;
                save_state(state);
                flash(std::string("Priority: ") + PRIORITY_NAMES[task->priority], "success");
            }
        }

        // Waiting on
        else if (ch == 'w') {
            if (!tasks.empty()) {
                Task* task = tasks[cursor];
                auto val = text_input("Waiting on (empty to clear): ", task->waiting_on);
                if (val.has_value()) {
                    save_undo();
                    task->waiting_on = val.value();
                    save_state(state);
                    flash(val->empty() ? "Cleared waiting" : "Waiting on: " + *val, "success");
                }
            }
        }

        // Blocked by
        else if (ch == 'b') {
            if (!tasks.empty()) {
                Task* task = tasks[cursor];
                auto val = text_input("Blocked by (empty to clear): ", task->blocked_by);
                if (val.has_value()) {
                    save_undo();
                    task->blocked_by = val.value();
                    save_state(state);
                    flash(val->empty() ? "Cleared blocked" : "Blocked by: " + *val, "success");
                }
            }
        }

        // Toggle backlog flag
        else if (ch == 'B') {
            if (!tasks.empty()) {
                Task* task = tasks[cursor];
                save_undo();
                task->backlog = !task->backlog;
                save_state(state);
                flash(task->backlog ? "Moved to backlog" : "Removed from backlog", "success");
                clamp_cursor();
            }
        }

        // Toggle backlog visibility
        else if (ch == '~') {
            show_backlog = !show_backlog;
            flash(show_backlog ? "Showing backlog items" : "Hiding backlog items", "success");
            clamp_cursor();
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

        // Detail view
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
                flash("No tags found. Add tags with +tag or +\"multi word\"");
            } else {
                std::string tag_list;
                for (auto& t : all_tags) tag_list += t + ", ";
                if (tag_list.size() > 2) tag_list.resize(tag_list.size() - 2);
                auto tag = text_input("Filter tag (" + tag_list + "): ");
                if (tag.has_value() && !tag->empty()) {
                    filter_tag = tag.value();
                    if (!filter_tag.empty() && filter_tag[0] == '+') filter_tag.erase(0, 1);
                    if (filter_tag.size() >= 2 && filter_tag.front() == '"' && filter_tag.back() == '"') {
                        filter_tag = filter_tag.substr(1, filter_tag.size() - 2);
                    }
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
        initscr();
        cbreak();
        noecho();
        keypad(stdscr, TRUE);
        curs_set(0);
        start_color();
        use_default_colors();
        timeout(50);

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
        init_pair(C_LOW,       COLOR_BLUE,    -1);
        init_pair(C_BLOCKED,   COLOR_RED,     -1);
        init_pair(C_BACKLOG,   COLOR_WHITE,   -1); // grey/dim

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
