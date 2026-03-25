#pragma once

#include <ncurses.h>
#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <sstream>
#include <string>
#include <vector>

// ── Color Pairs ─────────────────────────────────────────────

enum CP {
    CP_HEADER = 1, CP_DONE, CP_SELECTED, CP_PRI_LOW, CP_PRI_HIGH,
    CP_PRI_URGENT, CP_TAG, CP_NOTE, CP_HINT, CP_FOLDER,
    CP_RED, CP_GREEN, CP_YELLOW, CP_BLUE, CP_MAGENTA, CP_CYAN,
    CP_TAB_ACTIVE, CP_TAB_INACTIVE,
};

inline void init_colors() {
    start_color();
    use_default_colors();
    init_pair(CP_HEADER,       COLOR_WHITE,   COLOR_BLUE);
    init_pair(CP_DONE,         COLOR_GREEN,   -1);
    init_pair(CP_SELECTED,     COLOR_CYAN,    -1);
    init_pair(CP_PRI_LOW,      COLOR_BLUE,    -1);
    init_pair(CP_PRI_HIGH,     COLOR_YELLOW,  -1);
    init_pair(CP_PRI_URGENT,   COLOR_RED,     -1);
    init_pair(CP_TAG,          COLOR_CYAN,    -1);
    init_pair(CP_NOTE,         COLOR_MAGENTA, -1);
    init_pair(CP_HINT,         COLOR_WHITE,   -1);
    init_pair(CP_FOLDER,       COLOR_YELLOW,  -1);
    init_pair(CP_RED,          COLOR_RED,     -1);
    init_pair(CP_GREEN,        COLOR_GREEN,   -1);
    init_pair(CP_YELLOW,       COLOR_YELLOW,  -1);
    init_pair(CP_BLUE,         COLOR_BLUE,    -1);
    init_pair(CP_MAGENTA,      COLOR_MAGENTA, -1);
    init_pair(CP_CYAN,         COLOR_CYAN,    -1);
    init_pair(CP_TAB_ACTIVE,   COLOR_WHITE,   COLOR_BLUE);
    init_pair(CP_TAB_INACTIVE, COLOR_WHITE,   -1);
}

inline int status_cp(int color) {
    if (color >= 1 && color <= 6) return CP_RED + color - 1;
    return CP_HINT;
}

// ── Shared Utilities ────────────────────────────────────────

inline std::string trunc(const std::string& s, int w) {
    if (w <= 0) return "";
    if ((int)s.size() <= w) return s;
    if (w <= 1) return ".";
    return s.substr(0, w - 1) + "~";
}

inline std::vector<std::string> word_wrap(const std::string& text, int width) {
    std::vector<std::string> lines;
    if (width <= 0 || text.empty()) return {text};
    std::istringstream words(text);
    std::string word, line;
    while (words >> word) {
        if (line.empty()) line = word;
        else if ((int)(line.size() + 1 + word.size()) <= width) line += " " + word;
        else { lines.push_back(line); line = word; }
    }
    if (!line.empty()) lines.push_back(line);
    if (lines.empty()) lines.push_back("");
    return lines;
}

// ── Clipboard ───────────────────────────────────────────────

inline bool clipboard_copy(const std::string& text) {
    FILE* pipe = popen("xclip -selection clipboard 2>/dev/null", "w");
    if (!pipe) return false;
    fwrite(text.c_str(), 1, text.size(), pipe);
    return pclose(pipe) == 0;
}

inline std::string clipboard_paste() {
    FILE* pipe = popen("xclip -selection clipboard -o 2>/dev/null", "r");
    if (!pipe) return "";
    std::string result;
    char buf[4096];
    size_t n;
    while ((n = fread(buf, 1, sizeof(buf), pipe)) > 0)
        result.append(buf, n);
    pclose(pipe);
    return result;
}

// ── App Base Class ──────────────────────────────────────────

class AppBase {
public:
    int top_y = 1;  // First row the app can draw on (below tab bar)
    bool reload_requested = false;  // Set true to trigger init() on all apps

    virtual ~AppBase() = default;
    virtual const char* id() = 0;
    virtual const char* label() = 0;
    virtual void init() {}
    virtual void draw() = 0;
    virtual bool handle(int ch) = 0;

protected:
    std::string status_msg;
    int status_ttl = 0;

    void flash(const std::string& msg) { status_msg = msg; status_ttl = 40; }

    std::string text_input(const std::string& prompt, const std::string& initial = "") {
        std::string buf = initial;
        int pos = (int)buf.size();
        while (true) {
            move(LINES - 1, 0);
            clrtoeol();
            attron(A_BOLD);
            addstr(prompt.c_str());
            attroff(A_BOLD);
            int input_x = (int)prompt.size();
            int avail = COLS - input_x;
            int view_start = 0;
            if (pos > avail - 1) view_start = pos - avail + 1;
            addstr(buf.substr(view_start, avail).c_str());
            move(LINES - 1, input_x + pos - view_start);
            curs_set(1);
            refresh();

            int ch = getch();
            if (ch == ERR) continue;
            if (ch == '\n' || ch == KEY_ENTER) { curs_set(0); return buf; }
            if (ch == 27) { curs_set(0); return ""; }
            if (ch == KEY_BACKSPACE || ch == 127 || ch == 8) {
                if (pos > 0) buf.erase(--pos, 1);
            } else if (ch == KEY_DC) {
                if (pos < (int)buf.size()) buf.erase(pos, 1);
            } else if (ch == KEY_LEFT) {
                if (pos > 0) --pos;
            } else if (ch == KEY_RIGHT) {
                if (pos < (int)buf.size()) ++pos;
            } else if (ch == KEY_HOME || ch == 1) {
                pos = 0;
            } else if (ch == KEY_END || ch == 5) {
                pos = (int)buf.size();
            } else if (ch == 21) {
                buf.clear(); pos = 0;
            } else if (ch >= 32 && ch < 127) {
                buf.insert(pos++, 1, (char)ch);
            }
        }
    }

    bool confirm(const std::string& prompt) {
        move(LINES - 1, 0);
        clrtoeol();
        attron(A_BOLD);
        addstr(prompt.c_str());
        attroff(A_BOLD);
        addstr(" (y/n) ");
        refresh();
        while (true) {
            int ch = getch();
            if (ch == ERR) continue;
            return ch == 'y' || ch == 'Y';
        }
    }

    int pick_number(const std::string& prompt, int max) {
        std::string input = text_input(prompt);
        if (input.empty()) return -1;
        try {
            int n = std::stoi(input);
            if (n >= 1 && n <= max) return n - 1;
        } catch (...) {}
        flash("Invalid selection");
        return -1;
    }

    void draw_status_and_hints(const std::string& hints) {
        // Status message
        if (status_ttl > 0 && !status_msg.empty()) {
            move(LINES - 3, 0);
            attron(A_BOLD);
            addstr((" " + trunc(status_msg, COLS - 2)).c_str());
            attroff(A_BOLD);
            --status_ttl;
        }

        // Word-wrap hints into 2 lines
        int max_w = COLS - 2;
        std::string line1, line2;
        std::istringstream iss(hints);
        std::string word;
        while (iss >> word) {
            if ((int)(line1.size() + word.size() + 1) <= max_w || line1.empty()) {
                if (!line1.empty()) line1 += " ";
                line1 += word;
            } else {
                if (!line2.empty()) line2 += " ";
                line2 += word;
            }
        }

        attron(COLOR_PAIR(CP_HINT) | A_DIM);
        move(LINES - 2, 0);
        clrtoeol();
        addstr((" " + trunc(line1, COLS - 2)).c_str());
        move(LINES - 1, 0);
        clrtoeol();
        if (!line2.empty())
            addstr((" " + trunc(line2, COLS - 2)).c_str());
        attroff(COLOR_PAIR(CP_HINT) | A_DIM);
    }
};
