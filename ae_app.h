#pragma once

#include "app.h"
#include "json.h"
#include "model.h"

#include <algorithm>
#include <chrono>
#include <cstring>
#include <deque>
#include <fstream>
#include <poll.h>
#include <pty.h>
#include <regex>
#include <set>
#include <signal.h>
#include <sstream>
#include <sys/ioctl.h>
#include <sys/wait.h>
#include <termios.h>
#include <unistd.h>

// ── Base64 encode/decode (for binary keystroke data) ────────

namespace b64 {

static const char TABLE[] =
    "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

inline std::string encode(const std::string& in) {
    std::string out;
    out.reserve((in.size() + 2) / 3 * 4);
    int val = 0, bits = -6;
    for (unsigned char c : in) {
        val = (val << 8) + c;
        bits += 8;
        while (bits >= 0) {
            out.push_back(TABLE[(val >> bits) & 0x3F]);
            bits -= 6;
        }
    }
    if (bits > -6) out.push_back(TABLE[((val << 8) >> (bits + 8)) & 0x3F]);
    while (out.size() % 4) out.push_back('=');
    return out;
}

inline int decode_char(unsigned char c) {
    if (c >= 'A' && c <= 'Z') return c - 'A';
    if (c >= 'a' && c <= 'z') return c - 'a' + 26;
    if (c >= '0' && c <= '9') return c - '0' + 52;
    if (c == '+') return 62;
    if (c == '/') return 63;
    return -1;
}

inline std::string decode(const std::string& in) {
    std::string out;
    int val = 0, bits = -8;
    for (unsigned char c : in) {
        int d = decode_char(c);
        if (d == -1) continue;
        val = (val << 6) + d;
        bits += 6;
        if (bits >= 0) {
            out.push_back((char)((val >> bits) & 0xFF));
            bits -= 8;
        }
    }
    return out;
}

} // namespace b64

// ── Automation Engine App ───────────────────────────────────

class AeApp : public AppBase {

    // ── Data model ──────────────────────────────────────────

    enum StepType { ST_SEND, ST_WAIT, ST_PAUSE };

    struct Step {
        StepType    type = ST_SEND;
        std::string data;           // raw bytes for ST_SEND
        std::string pattern;        // regex for ST_WAIT
        int         delay_ms = 0;   // inter-byte delay for ST_SEND, duration for ST_PAUSE
        int         timeout_ms = 10000; // for ST_WAIT
    };

    struct Recording {
        int         id = 0;
        std::string name;
        std::string created;
        std::vector<std::string> params;
        std::vector<Step> steps;
    };

    // ── Raw keystroke captured during recording ─────────────

    struct RawKey {
        char    byte;
        int64_t time_us; // microseconds since recording start
    };

    // ── State ───────────────────────────────────────────────

    std::string path;
    int next_id = 1;
    std::vector<Recording> recordings;
    int send_delay_ms = 10;      // configurable inter-byte delay for new SEND steps
    bool skip_pauses = false;    // when true, don't record pause steps

    enum Mode { LIST, EDIT, HELP };
    Mode mode = LIST;
    int cursor = 0;
    int scroll = 0;
    std::set<int> expanded;
    std::string search;

    // Edit mode
    int edit_idx = -1;  // index into recordings
    int edit_cursor = 0;
    int edit_scroll = 0;

    // ── Persistence ─────────────────────────────────────────

    static std::string data_path() {
        const char* home = std::getenv("HOME");
        return std::string(home ? home : ".") + "/.ae.json";
    }

    static std::string step_type_str(StepType t) {
        switch (t) {
            case ST_SEND:  return "send";
            case ST_WAIT:  return "wait";
            case ST_PAUSE: return "pause";
        }
        return "send";
    }

    static StepType step_type_from_str(const std::string& s) {
        if (s == "wait")  return ST_WAIT;
        if (s == "pause") return ST_PAUSE;
        return ST_SEND;
    }

    void load() {
        std::ifstream f(path);
        if (!f.is_open()) return;
        std::string content((std::istreambuf_iterator<char>(f)), {});
        if (content.empty()) return;
        try {
            auto v = json::parse(content);
            next_id = v.int_or("next_id", 1);
            send_delay_ms = v.int_or("send_delay_ms", 10);
            skip_pauses = v.bool_or("skip_pauses", false);
            recordings.clear();
            if (v.has("recordings") && v.at("recordings").is_arr()) {
                for (auto& rv : v.at("recordings").as_arr()) {
                    Recording rec;
                    rec.id = rv.int_or("id", 0);
                    rec.name = rv.str_or("name", "");
                    rec.created = rv.str_or("created", "");

                    if (rv.has("params") && rv.at("params").is_arr())
                        for (auto& pv : rv.at("params").as_arr())
                            if (pv.is_str()) rec.params.push_back(pv.as_str());

                    if (rv.has("steps") && rv.at("steps").is_arr()) {
                        for (auto& sv : rv.at("steps").as_arr()) {
                            Step s;
                            s.type = step_type_from_str(sv.str_or("type", "send"));
                            if (s.type == ST_SEND)
                                s.data = b64::decode(sv.str_or("data", ""));
                            s.pattern = sv.str_or("pattern", "");
                            s.delay_ms = sv.int_or("delay_ms", 0);
                            s.timeout_ms = sv.int_or("timeout_ms", 10000);
                            rec.steps.push_back(s);
                        }
                    }
                    recordings.push_back(std::move(rec));
                }
            }
        } catch (...) {
            flash("Error reading ~/.ae.json");
        }
    }

    void save() {
        json::Object o;
        o["next_id"] = next_id;
        o["send_delay_ms"] = send_delay_ms;
        o["skip_pauses"] = skip_pauses;
        json::Array arr;
        for (auto& rec : recordings) {
            json::Object ro;
            ro["id"] = rec.id;
            ro["name"] = rec.name;
            ro["created"] = rec.created;

            json::Array pa;
            for (auto& p : rec.params) pa.push_back(p);
            ro["params"] = std::move(pa);

            json::Array sa;
            for (auto& s : rec.steps) {
                json::Object so;
                so["type"] = step_type_str(s.type);
                if (s.type == ST_SEND)
                    so["data"] = b64::encode(s.data);
                if (s.type == ST_WAIT) {
                    so["pattern"] = s.pattern;
                    so["timeout_ms"] = s.timeout_ms;
                }
                so["delay_ms"] = s.delay_ms;
                sa.push_back(std::move(so));
            }
            ro["steps"] = std::move(sa);
            arr.push_back(std::move(ro));
        }
        o["recordings"] = std::move(arr);
        std::ofstream f(path);
        f << json::serialize(json::Value(std::move(o)));
    }

    // ── Filtering ───────────────────────────────────────────

    std::vector<int> visible() {
        std::vector<int> vis;
        for (int i = 0; i < (int)recordings.size(); ++i) {
            if (!search.empty()) {
                std::string ln = recordings[i].name, lq = search;
                std::transform(ln.begin(), ln.end(), ln.begin(), ::tolower);
                std::transform(lq.begin(), lq.end(), lq.begin(), ::tolower);
                if (ln.find(lq) == std::string::npos) continue;
            }
            vis.push_back(i);
        }
        return vis;
    }

    void clamp_cursor(int count) {
        if (count <= 0) cursor = 0;
        else cursor = std::clamp(cursor, 0, count - 1);
    }

    // ── Item height for scroll ──────────────────────────────

    int item_height(int idx) {
        int h = 1; // name line
        if (expanded.count(recordings[idx].id)) {
            h += (int)recordings[idx].steps.size();
            if (recordings[idx].steps.empty()) h += 1; // "(no steps)"
        }
        return h;
    }

    // ── PTY session ─────────────────────────────────────────
    //
    // Suspends ncurses, forks a PTY, runs a live terminal session.
    // In record mode: captures all keystrokes with timestamps.
    // In play mode: feeds steps to the PTY.
    //
    // Ctrl+] (0x1d) exits the session.

    static volatile sig_atomic_t winch_flag;

    static void winch_handler(int) { winch_flag = 1; }

    // Suspend ncurses and prepare terminal for raw PTY I/O
    struct TermState {
        struct termios orig;
        bool saved = false;
    };

    void suspend_ncurses(TermState& ts) {
        def_prog_mode();
        endwin();
        if (tcgetattr(STDIN_FILENO, &ts.orig) == 0) ts.saved = true;
        struct termios raw_term = ts.orig;
        cfmakeraw(&raw_term);
        tcsetattr(STDIN_FILENO, TCSANOW, &raw_term);
    }

    void restore_ncurses(TermState& ts) {
        if (ts.saved) tcsetattr(STDIN_FILENO, TCSANOW, &ts.orig);
        reset_prog_mode();
        refresh();
        curs_set(0);
    }

    void propagate_winsize(int master_fd) {
        struct winsize ws;
        if (ioctl(STDIN_FILENO, TIOCGWINSZ, &ws) == 0)
            ioctl(master_fd, TIOCSWINSZ, &ws);
    }

    // ── Record session ──────────────────────────────────────

    std::vector<RawKey> run_record_session() {
        std::vector<RawKey> keys;
        TermState ts;
        suspend_ncurses(ts);

        // Print banner
        const char* banner =
            "\r\n\033[1;36m── ae: recording ──────────────────────────"
            "──────────────────\033[0m\r\n"
            "\033[2m   Ctrl+]  stop recording\033[0m\r\n\r\n";
        (void)!write(STDOUT_FILENO, banner, strlen(banner));

        int master_fd;
        pid_t child = forkpty(&master_fd, nullptr, nullptr, nullptr);
        if (child < 0) {
            restore_ncurses(ts);
            flash("forkpty failed");
            return keys;
        }

        if (child == 0) {
            // Child: exec shell
            const char* sh = std::getenv("SHELL");
            if (!sh) sh = "/bin/bash";
            execlp(sh, sh, nullptr);
            _exit(127);
        }

        // Parent: relay I/O and capture keystrokes
        propagate_winsize(master_fd);

        struct sigaction sa = {};
        sa.sa_handler = winch_handler;
        sigaction(SIGWINCH, &sa, nullptr);
        winch_flag = 0;

        auto start = std::chrono::steady_clock::now();
        bool running = true;
        char buf[4096];

        while (running) {
            if (winch_flag) {
                winch_flag = 0;
                propagate_winsize(master_fd);
            }

            struct pollfd fds[2];
            fds[0] = { STDIN_FILENO, POLLIN, 0 };
            fds[1] = { master_fd, POLLIN, 0 };

            int ret = poll(fds, 2, 100);
            if (ret < 0) {
                if (errno == EINTR) continue;
                break;
            }

            // User input → PTY (and capture)
            if (fds[0].revents & POLLIN) {
                ssize_t n = read(STDIN_FILENO, buf, sizeof(buf));
                if (n <= 0) break;

                auto now = std::chrono::steady_clock::now();
                int64_t us = std::chrono::duration_cast<std::chrono::microseconds>(
                    now - start).count();

                for (ssize_t i = 0; i < n; ++i) {
                    if (buf[i] == 0x1d) { // Ctrl+]
                        running = false;
                        break;
                    }
                    keys.push_back({ buf[i], us });
                }

                if (running) {
                    // Strip Ctrl+] bytes before writing to PTY
                    (void)!write(master_fd, buf, n);
                }
            }

            // PTY output → user
            if (fds[1].revents & (POLLIN | POLLHUP)) {
                ssize_t n = read(master_fd, buf, sizeof(buf));
                if (n <= 0) break;
                (void)!write(STDOUT_FILENO, buf, n);
            }
        }

        // Cleanup child
        kill(child, SIGHUP);
        int status;
        waitpid(child, &status, WNOHANG);
        close(master_fd);

        // Restore
        struct sigaction sa_dfl = {};
        sa_dfl.sa_handler = SIG_DFL;
        sigaction(SIGWINCH, &sa_dfl, nullptr);

        const char* end_banner =
            "\r\n\033[1;36m── recording stopped ──\033[0m\r\n";
        (void)!write(STDOUT_FILENO, end_banner, strlen(end_banner));

        // Brief pause so user sees the message
        usleep(400000);

        restore_ncurses(ts);
        return keys;
    }

    // ── Post-process raw keys into steps ────────────────────
    //
    // Coalesce rapid keystrokes (< 80ms gap) into single send steps.
    // Long gaps become pause steps.

    std::vector<Step> postprocess(const std::vector<RawKey>& keys) {
        if (keys.empty()) return {};

        std::vector<Step> steps;
        static constexpr int64_t COALESCE_US = 80000;   // 80ms
        static constexpr int64_t PAUSE_THRESH_US = 800000; // 800ms

        std::string accum;
        int64_t last_time = keys[0].time_us;

        for (size_t i = 0; i < keys.size(); ++i) {
            int64_t gap = keys[i].time_us - last_time;

            if (i > 0 && gap > PAUSE_THRESH_US) {
                // Flush accumulated send
                if (!accum.empty()) {
                    Step s;
                    s.type = ST_SEND;
                    s.data = accum;
                    s.delay_ms = send_delay_ms;
                    steps.push_back(s);
                    accum.clear();
                }
                // Insert pause (unless skip_pauses is on)
                if (!skip_pauses) {
                    Step p;
                    p.type = ST_PAUSE;
                    p.delay_ms = (int)(gap / 1000);
                    steps.push_back(p);
                }
            } else if (i > 0 && gap > COALESCE_US && !accum.empty()) {
                // Moderate gap: flush current batch as a step
                Step s;
                s.type = ST_SEND;
                s.data = accum;
                s.delay_ms = send_delay_ms;
                steps.push_back(s);
                accum.clear();
            }

            accum += keys[i].byte;
            last_time = keys[i].time_us;
        }

        if (!accum.empty()) {
            Step s;
            s.type = ST_SEND;
            s.data = accum;
            s.delay_ms = send_delay_ms;
            steps.push_back(s);
        }

        return steps;
    }

    // ── Play session ────────────────────────────────────────

    bool run_play_session(Recording& rec, const std::map<std::string, std::string>& param_vals) {
        TermState ts;
        suspend_ncurses(ts);

        const char* banner =
            "\r\n\033[1;32m── ae: playing ────────────────────────────"
            "──────────────────\033[0m\r\n"
            "\033[2m   Ctrl+]  abort playback\033[0m\r\n\r\n";
        (void)!write(STDOUT_FILENO, banner, strlen(banner));

        int master_fd;
        pid_t child = forkpty(&master_fd, nullptr, nullptr, nullptr);
        if (child < 0) {
            restore_ncurses(ts);
            flash("forkpty failed");
            return false;
        }

        if (child == 0) {
            const char* sh = std::getenv("SHELL");
            if (!sh) sh = "/bin/bash";
            execlp(sh, sh, nullptr);
            _exit(127);
        }

        propagate_winsize(master_fd);

        struct sigaction sa = {};
        sa.sa_handler = winch_handler;
        sigaction(SIGWINCH, &sa, nullptr);
        winch_flag = 0;

        bool aborted = false;
        bool ok = true;
        char buf[4096];

        // Helper: drain PTY output to screen, check for abort
        auto drain_pty = [&](int timeout_ms) -> bool {
            struct pollfd fds[2];
            fds[0] = { STDIN_FILENO, POLLIN, 0 };
            fds[1] = { master_fd, POLLIN, 0 };
            int ret = poll(fds, 2, timeout_ms);
            if (ret < 0 && errno != EINTR) return false;

            if (fds[0].revents & POLLIN) {
                ssize_t n = read(STDIN_FILENO, buf, sizeof(buf));
                for (ssize_t i = 0; i < n; ++i)
                    if (buf[i] == 0x1d) { aborted = true; return false; }
            }
            if (fds[1].revents & (POLLIN | POLLHUP)) {
                ssize_t n = read(master_fd, buf, sizeof(buf));
                if (n > 0) (void)!write(STDOUT_FILENO, buf, n);
                else if (n <= 0) return false;
            }
            return true;
        };

        // Helper: drain for a duration, continuously showing output
        auto drain_for_ms = [&](int ms) {
            auto deadline = std::chrono::steady_clock::now() +
                            std::chrono::milliseconds(ms);
            while (!aborted && std::chrono::steady_clock::now() < deadline) {
                auto remain = std::chrono::duration_cast<std::chrono::milliseconds>(
                    deadline - std::chrono::steady_clock::now()).count();
                if (remain <= 0) break;
                drain_pty(std::min((int)remain, 50));
            }
        };

        // Helper: substitute params in data
        auto substitute = [&](const std::string& data) -> std::string {
            std::string result = data;
            for (auto& [key, val] : param_vals) {
                std::string token = "${" + key + "}";
                size_t pos = 0;
                while ((pos = result.find(token, pos)) != std::string::npos) {
                    result.replace(pos, token.size(), val);
                    pos += val.size();
                }
            }
            return result;
        };

        // Execute steps
        for (auto& step : rec.steps) {
            if (aborted) break;

            if (winch_flag) {
                winch_flag = 0;
                propagate_winsize(master_fd);
            }

            switch (step.type) {
                case ST_SEND: {
                    std::string data = substitute(step.data);
                    for (size_t i = 0; i < data.size(); ++i) {
                        if (aborted) break;
                        (void)!write(master_fd, &data[i], 1);
                        if (step.delay_ms > 0)
                            drain_for_ms(step.delay_ms);
                        else
                            drain_pty(5);
                    }
                    // Let output settle
                    drain_for_ms(50);
                    break;
                }

                case ST_WAIT: {
                    std::string match_buf;
                    std::regex rx;
                    try { rx = std::regex(step.pattern); }
                    catch (...) {
                        // Bad regex, just pause
                        drain_for_ms(step.timeout_ms);
                        break;
                    }

                    auto deadline = std::chrono::steady_clock::now() +
                                    std::chrono::milliseconds(step.timeout_ms);
                    bool matched = false;

                    while (!aborted && !matched) {
                        auto remain = std::chrono::duration_cast<std::chrono::milliseconds>(
                            deadline - std::chrono::steady_clock::now()).count();
                        if (remain <= 0) break;

                        struct pollfd fds[2];
                        fds[0] = { STDIN_FILENO, POLLIN, 0 };
                        fds[1] = { master_fd, POLLIN, 0 };
                        int ret = poll(fds, 2, std::min((int)remain, 100));
                        if (ret < 0 && errno != EINTR) break;

                        if (fds[0].revents & POLLIN) {
                            ssize_t n = read(STDIN_FILENO, buf, sizeof(buf));
                            for (ssize_t i = 0; i < n; ++i)
                                if (buf[i] == 0x1d) aborted = true;
                        }
                        if (fds[1].revents & (POLLIN | POLLHUP)) {
                            ssize_t n = read(master_fd, buf, sizeof(buf));
                            if (n > 0) {
                                (void)!write(STDOUT_FILENO, buf, n);
                                match_buf.append(buf, n);
                                // Keep buffer bounded
                                if (match_buf.size() > 8192)
                                    match_buf = match_buf.substr(match_buf.size() - 4096);
                                if (std::regex_search(match_buf, rx))
                                    matched = true;
                            } else if (n <= 0) break;
                        }
                    }
                    break;
                }

                case ST_PAUSE: {
                    drain_for_ms(step.delay_ms);
                    break;
                }
            }
        }

        // Final drain to show remaining output
        if (!aborted) drain_for_ms(500);

        // Cleanup
        kill(child, SIGHUP);
        int status;
        waitpid(child, &status, WNOHANG);
        close(master_fd);

        struct sigaction sa_dfl = {};
        sa_dfl.sa_handler = SIG_DFL;
        sigaction(SIGWINCH, &sa_dfl, nullptr);

        const char* end_banner = aborted
            ? "\r\n\033[1;33m── playback aborted ──\033[0m\r\n"
            : "\r\n\033[1;32m── playback complete ──\033[0m\r\n";
        (void)!write(STDOUT_FILENO, end_banner, strlen(end_banner));
        usleep(400000);

        restore_ncurses(ts);
        return !aborted;
    }

    // ── High-level record/play commands ─────────────────────

    void do_record() {
        auto keys = run_record_session();
        if (keys.empty()) {
            flash("Empty recording (nothing captured)");
            return;
        }

        std::string name = text_input("Name: ");
        if (name.empty()) {
            flash("Cancelled");
            return;
        }

        auto steps = postprocess(keys);

        Recording rec;
        rec.id = next_id++;
        rec.name = name;
        rec.created = now_iso();
        rec.steps = std::move(steps);
        recordings.push_back(std::move(rec));
        save();

        auto vis = visible();
        cursor = std::max(0, (int)vis.size() - 1);
        flash("Recorded: " + name + " (" + std::to_string(recordings.back().steps.size()) + " steps)");
    }

    void do_play(int idx) {
        auto& rec = recordings[idx];
        if (rec.steps.empty()) {
            flash("No steps to play");
            return;
        }

        // Prompt for params
        std::map<std::string, std::string> param_vals;
        for (auto& p : rec.params) {
            std::string val = text_input("${" + p + "}: ");
            if (val.empty() && !confirm("Leave ${" + p + "} empty?")) {
                flash("Cancelled");
                return;
            }
            param_vals[p] = val;
        }

        bool ok = run_play_session(rec, param_vals);
        flash(ok ? "Playback complete" : "Playback aborted");
    }

    // ── Drawing helpers ─────────────────────────────────────

    // Human-readable step summary
    static std::string step_summary(const Step& s) {
        switch (s.type) {
            case ST_SEND: {
                std::string preview;
                for (char c : s.data) {
                    if (preview.size() >= 40) { preview += "..."; break; }
                    if (c == '\n') preview += "\\n";
                    else if (c == '\r') preview += "\\r";
                    else if (c == '\t') preview += "\\t";
                    else if (c == 0x1b) preview += "\\e";
                    else if (c < 32) {
                        preview += "^";
                        preview += (char)(c + 64);
                    } else {
                        preview += c;
                    }
                }
                return "send  " + preview;
            }
            case ST_WAIT:
                return "wait  /" + s.pattern + "/  " +
                       std::to_string(s.timeout_ms / 1000) + "s";
            case ST_PAUSE:
                return "pause " + std::to_string(s.delay_ms) + "ms";
        }
        return "";
    }

    static std::string step_type_label(StepType t) {
        switch (t) {
            case ST_SEND:  return "send";
            case ST_WAIT:  return "wait";
            case ST_PAUSE: return "pause";
        }
        return "?";
    }

    // ── Drawing: header ─────────────────────────────────────

    void draw_header() {
        attron(COLOR_PAIR(CP_HEADER) | A_BOLD);
        move(top_y, 0);
        for (int i = 0; i < COLS; ++i) addch(' ');
        move(top_y, 1);
        if (mode == EDIT && edit_idx >= 0 && edit_idx < (int)recordings.size())
            addstr(("ae > " + trunc(recordings[edit_idx].name, COLS - 20)).c_str());
        else
            addstr("ae");

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
            addstr("  No recordings. 'r' to start recording.");
            attroff(A_DIM);
            return;
        }

        int y = start_y;
        for (int vi = scroll; vi < (int)vis.size() && y < start_y + ch; ++vi) {
            int idx = vis[vi];
            auto& rec = recordings[idx];
            bool sel = (vi == cursor);
            bool exp = expanded.count(rec.id);

            // ── Name line ──
            move(y, 0);
            if (sel) attron(COLOR_PAIR(CP_SELECTED) | A_BOLD);
            addch(sel ? '>' : ' ');
            addch(' ');

            std::string display = rec.name;
            addstr(trunc(display, COLS - 2).c_str());

            // Right-aligned indicators: step count + param count
            std::string ind;
            ind += std::to_string(rec.steps.size()) + "s";
            if (!rec.params.empty())
                ind += " " + std::to_string(rec.params.size()) + "p";
            ind += " " + format_short(rec.created);

            int ind_w = (int)ind.size();
            int text_end_x = 2 + std::min((int)display.size(), COLS - 2);
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

            // ── Expanded steps ──
            if (exp) {
                if (rec.steps.empty()) {
                    if (y < start_y + ch) {
                        move(y, 4);
                        attron(A_DIM);
                        addstr("(no steps)");
                        attroff(A_DIM);
                        ++y;
                    }
                } else {
                    for (int si = 0; si < (int)rec.steps.size() && y < start_y + ch; ++si) {
                        move(y, 4);
                        int cp = CP_HINT;
                        switch (rec.steps[si].type) {
                            case ST_SEND:  cp = CP_CYAN; break;
                            case ST_WAIT:  cp = CP_YELLOW; break;
                            case ST_PAUSE: cp = CP_HINT; break;
                        }
                        attron(COLOR_PAIR(cp) | A_DIM);
                        std::string sum = step_summary(rec.steps[si]);
                        addstr(trunc(sum, COLS - 5).c_str());
                        attroff(COLOR_PAIR(cp) | A_DIM);
                        ++y;
                    }
                }
            }
        }
    }

    // ── Drawing: edit mode ──────────────────────────────────

    void draw_edit_view(int start_y, int ch) {
        if (edit_idx < 0 || edit_idx >= (int)recordings.size()) {
            mode = LIST;
            return;
        }
        auto& rec = recordings[edit_idx];

        if (rec.steps.empty()) {
            move(start_y + ch / 2, 0);
            attron(A_DIM);
            addstr("  No steps. 'w' to add wait, 'p' to add pause, 's' to add send.");
            attroff(A_DIM);
            return;
        }

        if (edit_cursor >= (int)rec.steps.size())
            edit_cursor = std::max(0, (int)rec.steps.size() - 1);
        if (edit_cursor < edit_scroll) edit_scroll = edit_cursor;
        if (edit_cursor >= edit_scroll + ch) edit_scroll = edit_cursor - ch + 1;

        int y = start_y;
        for (int i = edit_scroll; i < (int)rec.steps.size() && y < start_y + ch; ++i) {
            bool sel = (i == edit_cursor);
            auto& step = rec.steps[i];

            move(y, 0);
            if (sel) attron(COLOR_PAIR(CP_SELECTED) | A_BOLD);
            addch(sel ? '>' : ' ');
            addch(' ');

            // Step number
            std::string num = std::to_string(i + 1) + ".";
            while ((int)num.size() < 4) num += ' ';

            // Type badge
            int tcp = CP_HINT;
            switch (step.type) {
                case ST_SEND:  tcp = CP_CYAN; break;
                case ST_WAIT:  tcp = CP_YELLOW; break;
                case ST_PAUSE: tcp = CP_HINT; break;
            }

            attron(A_DIM);
            addstr(num.c_str());
            attroff(A_DIM);

            if (!sel) attron(COLOR_PAIR(tcp));
            std::string label = step_type_label(step.type);
            attron(A_BOLD);
            addstr(label.c_str());
            attroff(A_BOLD);
            addch(' ');

            // Details
            std::string detail;
            switch (step.type) {
                case ST_SEND:
                    detail = step_summary(step).substr(6); // skip "send  "
                    break;
                case ST_WAIT:
                    detail = "/" + step.pattern + "/  timeout:" +
                             std::to_string(step.timeout_ms / 1000) + "s";
                    break;
                case ST_PAUSE:
                    detail = std::to_string(step.delay_ms) + "ms";
                    break;
            }

            int avail = COLS - 2 - (int)num.size() - (int)label.size() - 1;
            addstr(trunc(detail, std::max(avail, 5)).c_str());
            if (!sel) attroff(COLOR_PAIR(tcp));

            if (sel) attroff(COLOR_PAIR(CP_SELECTED) | A_BOLD);
            ++y;
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

        section("Recording");
        bind("r", "Record new automation (opens live terminal)");
        bind("Ctrl+]", "Stop recording (inside the terminal)");

        section("Playback");
        bind("Enter/p", "Play selected recording");
        bind("Ctrl+]", "Abort playback (inside the terminal)");

        section("Recordings");
        bind("e", "Rename recording");
        bind("d", "Delete recording");
        bind("D", "Duplicate recording");
        bind("o", "Expand/collapse steps");
        bind("O", "Expand/collapse all");
        bind("E", "Edit steps");

        section("Step Editor (E)");
        bind("d", "Delete step");
        bind("w", "Insert wait step (regex match on output)");
        bind("s", "Insert send step");
        bind("P", "Insert pause step");
        bind("t", "Edit step delay/timeout");
        bind("e", "Edit step data/pattern");
        bind("J/K", "Move step down/up");
        bind("v", "Extract parameter from send step");
        bind("Esc/q", "Back to list");

        section("Settings");
        bind("T", ("Set send delay (currently " + std::to_string(send_delay_ms) + "ms)").c_str());
        bind("S", (std::string("Toggle skip pauses (currently ") + (skip_pauses ? "ON" : "OFF") + ")").c_str());

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
        } else if (mode == EDIT) {
            hints = "d:del w:wait s:send P:pause t:timing e:edit J/K:move v:param Esc:back";
        } else {
            hints = "r:record p:play e:name d:del D:dup E:edit o:expand T:delay S:pauses /:find ?:help q:quit";
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

            // Record
            case 'r': {
                do_record();
                break;
            }

            // Play
            case 'p': case '\n': case KEY_ENTER: {
                if (count == 0) break;
                do_play(vis[cursor]);
                break;
            }

            // Rename
            case 'e': {
                if (count == 0) break;
                auto& rec = recordings[vis[cursor]];
                std::string result = text_input("Name: ", rec.name);
                if (result.empty()) break;
                rec.name = result;
                save();
                flash("Renamed");
                break;
            }

            // Delete
            case 'd': {
                if (count == 0) break;
                auto& rec = recordings[vis[cursor]];
                if (!confirm("Delete \"" + trunc(rec.name, 30) + "\"?")) break;
                int id = rec.id;
                expanded.erase(id);
                recordings.erase(std::remove_if(recordings.begin(), recordings.end(),
                    [id](const Recording& r) { return r.id == id; }), recordings.end());
                save();
                clamp_cursor((int)visible().size());
                flash("Deleted");
                break;
            }

            // Duplicate
            case 'D': {
                if (count == 0) break;
                Recording dup = recordings[vis[cursor]];
                dup.id = next_id++;
                dup.name += " (copy)";
                dup.created = now_iso();
                recordings.push_back(std::move(dup));
                save();
                cursor = (int)visible().size() - 1;
                flash("Duplicated");
                break;
            }

            // Expand/collapse
            case 'o': {
                if (count == 0) break;
                int id = recordings[vis[cursor]].id;
                if (expanded.count(id)) expanded.erase(id);
                else expanded.insert(id);
                break;
            }

            // Expand/collapse all
            case 'O': {
                bool all_exp = true;
                for (auto vi : vis)
                    if (!expanded.count(recordings[vi].id)) { all_exp = false; break; }
                if (all_exp) {
                    expanded.clear();
                    flash("Collapsed all");
                } else {
                    for (auto vi : vis) expanded.insert(recordings[vi].id);
                    flash("Expanded all");
                }
                break;
            }

            // Edit steps
            case 'E': {
                if (count == 0) break;
                edit_idx = vis[cursor];
                edit_cursor = 0;
                edit_scroll = 0;
                mode = EDIT;
                break;
            }

            // Configure send delay
            case 'T': {
                std::string result = text_input("Send delay ms: ",
                    std::to_string(send_delay_ms));
                if (!result.empty()) {
                    try { send_delay_ms = std::max(0, std::stoi(result)); } catch (...) {}
                    save();
                    flash("Send delay: " + std::to_string(send_delay_ms) + "ms");
                }
                break;
            }

            // Toggle skip pauses during recording
            case 'S': {
                skip_pauses = !skip_pauses;
                save();
                flash(skip_pauses ? "Skip pauses: ON" : "Skip pauses: OFF");
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

    // ── Handle: edit mode ───────────────────────────────────

    bool handle_edit(int ch) {
        if (edit_idx < 0 || edit_idx >= (int)recordings.size()) {
            mode = LIST;
            return true;
        }
        auto& rec = recordings[edit_idx];
        int count = (int)rec.steps.size();

        switch (ch) {
            case 'j': case KEY_DOWN: if (edit_cursor < count - 1) ++edit_cursor; break;
            case 'k': case KEY_UP:   if (edit_cursor > 0) --edit_cursor; break;
            case 'g': case KEY_HOME: edit_cursor = 0; break;
            case 'G': case KEY_END:  edit_cursor = std::max(0, count - 1); break;

            // Delete step
            case 'd': {
                if (count == 0) break;
                rec.steps.erase(rec.steps.begin() + edit_cursor);
                if (edit_cursor >= (int)rec.steps.size())
                    edit_cursor = std::max(0, (int)rec.steps.size() - 1);
                save();
                flash("Step deleted");
                break;
            }

            // Insert wait step
            case 'w': {
                std::string pat = text_input("Wait pattern (regex): ");
                if (pat.empty()) break;
                std::string timeout = text_input("Timeout ms [10000]: ", "10000");
                Step s;
                s.type = ST_WAIT;
                s.pattern = pat;
                try { s.timeout_ms = std::stoi(timeout); } catch (...) { s.timeout_ms = 10000; }
                int insert_at = count > 0 ? edit_cursor + 1 : 0;
                rec.steps.insert(rec.steps.begin() + insert_at, s);
                edit_cursor = insert_at;
                save();
                flash("Wait step added");
                break;
            }

            // Insert send step
            case 's': {
                std::string data = text_input("Send data (\\n=newline, \\e=escape): ");
                if (data.empty()) break;
                // Process escape sequences
                std::string processed;
                for (size_t i = 0; i < data.size(); ++i) {
                    if (data[i] == '\\' && i + 1 < data.size()) {
                        switch (data[i + 1]) {
                            case 'n': processed += '\n'; ++i; break;
                            case 'r': processed += '\r'; ++i; break;
                            case 't': processed += '\t'; ++i; break;
                            case 'e': processed += '\x1b'; ++i; break;
                            case '\\': processed += '\\'; ++i; break;
                            default: processed += data[i]; break;
                        }
                    } else {
                        processed += data[i];
                    }
                }
                Step s;
                s.type = ST_SEND;
                s.data = processed;
                s.delay_ms = send_delay_ms;
                int insert_at = count > 0 ? edit_cursor + 1 : 0;
                rec.steps.insert(rec.steps.begin() + insert_at, s);
                edit_cursor = insert_at;
                save();
                flash("Send step added");
                break;
            }

            // Insert pause step
            case 'P': {
                std::string ms = text_input("Pause ms: ", "500");
                Step s;
                s.type = ST_PAUSE;
                try { s.delay_ms = std::stoi(ms); } catch (...) { s.delay_ms = 500; }
                int insert_at = count > 0 ? edit_cursor + 1 : 0;
                rec.steps.insert(rec.steps.begin() + insert_at, s);
                edit_cursor = insert_at;
                save();
                flash("Pause step added");
                break;
            }

            // Edit step data/pattern
            case 'e': {
                if (count == 0) break;
                auto& step = rec.steps[edit_cursor];
                switch (step.type) {
                    case ST_SEND: {
                        // Show current data with escapes
                        std::string escaped;
                        for (char c : step.data) {
                            if (c == '\n') escaped += "\\n";
                            else if (c == '\r') escaped += "\\r";
                            else if (c == '\t') escaped += "\\t";
                            else if (c == 0x1b) escaped += "\\e";
                            else if (c < 32) { escaped += "^"; escaped += (char)(c + 64); }
                            else escaped += c;
                        }
                        std::string result = text_input("Data: ", escaped);
                        if (result.empty() && !confirm("Clear data?")) break;
                        // Process escapes
                        std::string processed;
                        for (size_t i = 0; i < result.size(); ++i) {
                            if (result[i] == '\\' && i + 1 < result.size()) {
                                switch (result[i + 1]) {
                                    case 'n': processed += '\n'; ++i; break;
                                    case 'r': processed += '\r'; ++i; break;
                                    case 't': processed += '\t'; ++i; break;
                                    case 'e': processed += '\x1b'; ++i; break;
                                    case '\\': processed += '\\'; ++i; break;
                                    default: processed += result[i]; break;
                                }
                            } else {
                                processed += result[i];
                            }
                        }
                        step.data = processed;
                        save();
                        flash("Updated");
                        break;
                    }
                    case ST_WAIT: {
                        std::string result = text_input("Pattern: ", step.pattern);
                        if (!result.empty()) {
                            step.pattern = result;
                            save();
                            flash("Updated");
                        }
                        break;
                    }
                    case ST_PAUSE:
                        // Use 't' for timing
                        break;
                }
                break;
            }

            // Edit timing
            case 't': {
                if (count == 0) break;
                auto& step = rec.steps[edit_cursor];
                if (step.type == ST_WAIT) {
                    std::string result = text_input("Timeout ms: ",
                        std::to_string(step.timeout_ms));
                    try { step.timeout_ms = std::stoi(result); } catch (...) {}
                } else {
                    std::string result = text_input("Delay ms: ",
                        std::to_string(step.delay_ms));
                    try { step.delay_ms = std::stoi(result); } catch (...) {}
                }
                save();
                flash("Timing updated");
                break;
            }

            // Move step down
            case 'J': {
                if (edit_cursor < count - 1) {
                    std::swap(rec.steps[edit_cursor], rec.steps[edit_cursor + 1]);
                    ++edit_cursor;
                    save();
                }
                break;
            }

            // Move step up
            case 'K': {
                if (edit_cursor > 0) {
                    std::swap(rec.steps[edit_cursor], rec.steps[edit_cursor - 1]);
                    --edit_cursor;
                    save();
                }
                break;
            }

            // Extract parameter from send step
            case 'v': {
                if (count == 0) break;
                auto& step = rec.steps[edit_cursor];
                if (step.type != ST_SEND) {
                    flash("Can only parameterize send steps");
                    break;
                }
                std::string val = text_input("Value to replace: ");
                if (val.empty()) break;
                std::string param_name = text_input("Parameter name: ");
                if (param_name.empty()) break;

                // Replace value with ${param_name} in the step data
                std::string token = "${" + param_name + "}";
                size_t pos = step.data.find(val);
                if (pos == std::string::npos) {
                    flash("Value not found in step data");
                    break;
                }
                step.data.replace(pos, val.size(), token);

                // Add param to recording if not already there
                if (std::find(rec.params.begin(), rec.params.end(), param_name) == rec.params.end())
                    rec.params.push_back(param_name);

                save();
                flash("Parameter ${" + param_name + "} created");
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
    const char* id() override { return "ae"; }
    const char* label() override { return "auto"; }

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
            case EDIT:
                draw_edit_view(top_y + 1, ch);
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
        if (mode == EDIT) return handle_edit(ch);
        return handle_list(ch);
    }
};

volatile sig_atomic_t AeApp::winch_flag = 0;
