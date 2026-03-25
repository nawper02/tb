#pragma once

#include "app.h"
#include "json.h"
#include "model.h"

#include <cstdio>
#include <cstdlib>
#include <deque>
#include <fstream>
#include <sstream>
#include <unistd.h>

// ── Console App ──────────────────────────────────────────────
//
// A command-line interface inside the TUI. Type 'help' to start.
//
// Built-in commands: push, pull, status, key, password, clear, help
//
// To add a new command:
//   1. Write a cmd_foo(args) method
//   2. Add "else if (cmd == "foo") cmd_foo(args);" in execute()
//   3. Add a line in cmd_help()

class ConsoleApp : public AppBase {

    // ── Output buffer ────────────────────────────────────────

    enum LineType { L_OUT, L_CMD, L_OK, L_ERR, L_DIM, L_HEAD };

    struct Line {
        LineType    type = L_OUT;
        std::string text;
        std::string ts;   // timestamp, shown right-aligned on CMD lines
    };

    std::deque<Line>     buf;
    int                  scroll = 0;   // lines scrolled up from bottom
    static constexpr int BUFMAX = 2000;

    void emit(const std::string& text, LineType type = L_OUT, bool ts = false) {
        Line l{ type, text, ts ? format_short(now_iso()) : "" };
        buf.push_back(l);
        while ((int)buf.size() > BUFMAX) buf.pop_front();
        if (scroll > 0) ++scroll; // keep viewport if scrolled up
    }

    // ── Input ────────────────────────────────────────────────

    std::string              input;
    int                      input_pos = 0;
    std::vector<std::string> history;
    int                      history_idx = -1;
    std::string              history_saved;

    // ── Sync state ───────────────────────────────────────────

    struct SyncEntry {
        std::string label;
        std::string file;
        std::string last_push;
        std::string last_pull;
    };

    std::string             cfg_path;
    std::string             bucket_id;
    std::string             session_pw;
    std::vector<SyncEntry>  entries;

    // ── Paths ────────────────────────────────────────────────

    static std::string home(const char* name) {
        const char* h = std::getenv("HOME");
        return std::string(h ? h : ".") + "/" + name;
    }

    // ── Config ───────────────────────────────────────────────

    void load_cfg() {
        std::ifstream f(cfg_path);
        if (!f.is_open()) return;
        std::string s((std::istreambuf_iterator<char>(f)), {});
        try {
            auto v = json::parse(s);
            bucket_id = v.str_or("bucket_id", "");
            for (auto& e : entries) {
                e.last_push = v.str_or(e.label + "_last_push", "");
                e.last_pull = v.str_or(e.label + "_last_pull", "");
            }
        } catch (...) {}
    }

    void save_cfg() {
        json::Object o;
        o["bucket_id"] = bucket_id;
        for (auto& e : entries) {
            o[e.label + "_last_push"] = e.last_push;
            o[e.label + "_last_pull"] = e.last_pull;
        }
        std::ofstream f(cfg_path);
        f << json::serialize(json::Value(std::move(o)));
    }

    // ── Shell utils ──────────────────────────────────────────

    static std::string sq(const std::string& s) {
        std::string r = "'";
        for (char c : s) { if (c == '\'') r += "'\\''"; else r += c; }
        return r + "'";
    }

    static std::string run(const std::string& cmd) {
        FILE* p = popen(cmd.c_str(), "r");
        if (!p) return "";
        std::string out; char tmp[4096]; size_t n;
        while ((n = fread(tmp, 1, sizeof(tmp), p)) > 0) out.append(tmp, n);
        pclose(p);
        return out;
    }

    static std::vector<std::string> tokenize(const std::string& s) {
        std::vector<std::string> v;
        std::istringstream ss(s);
        std::string t;
        while (ss >> t) v.push_back(t);
        return v;
    }

    // ── Crypto ───────────────────────────────────────────────

    std::string encrypt(const std::string& data) {
        std::string tmp = "/tmp/.tb_enc_" + std::to_string(getpid());
        { std::ofstream f(tmp); f << data; }
        std::string out = run("openssl enc -aes-256-cbc -pbkdf2 -base64 -A"
                              " -in " + sq(tmp) +
                              " -pass " + sq("pass:" + session_pw) + " 2>/dev/null");
        std::remove(tmp.c_str());
        while (!out.empty() && (out.back() == '\n' || out.back() == '\r')) out.pop_back();
        return out;
    }

    std::string decrypt(const std::string& data) {
        std::string tmp = "/tmp/.tb_enc_" + std::to_string(getpid());
        { std::ofstream f(tmp); f << data; }
        std::string out = run("openssl enc -d -aes-256-cbc -pbkdf2 -base64 -A"
                              " -in " + sq(tmp) +
                              " -pass " + sq("pass:" + session_pw) + " 2>/dev/null");
        std::remove(tmp.c_str());
        return out;
    }

    // ── kvdb.io ──────────────────────────────────────────────

    // Returns "" on success, error string on failure
    std::string kv_put(const std::string& key, const std::string& value) {
        std::string url = "https://kvdb.io/" + bucket_id + "/" + key;
        std::string resp = run("curl -s -w '\\n%{http_code}'"
                               " -X POST " + sq(url) + " -d " + sq(value));
        auto sep  = resp.rfind('\n');
        std::string code = (sep != std::string::npos) ? resp.substr(sep + 1) : "";
        if (code == "200" || code == "201") return "";
        return "HTTP " + code + (sep != std::string::npos ? ": " + resp.substr(0, sep) : "");
    }

    // Returns value string, or "" on 404/error
    std::string kv_get(const std::string& key) {
        std::string url = "https://kvdb.io/" + bucket_id + "/" + key;
        std::string resp = run("curl -s -w '\\n%{http_code}'"
                               " -X GET " + sq(url));
        auto sep  = resp.rfind('\n');
        std::string code = (sep != std::string::npos) ? resp.substr(sep + 1) : "";
        if (code == "200") return (sep != std::string::npos) ? resp.substr(0, sep) : resp;
        return "";
    }

    // ── Progress display (non-buffered, during blocking ops) ─

    void progress(const std::string& msg) {
        move(LINES - 3, 0); clrtoeol();
        attron(COLOR_PAIR(CP_YELLOW) | A_DIM);
        addstr((" " + msg).c_str());
        attroff(COLOR_PAIR(CP_YELLOW) | A_DIM);
        refresh();
    }

    // ── Sync helpers ─────────────────────────────────────────

    SyncEntry* find_entry(const std::string& label) {
        for (auto& e : entries) if (e.label == label) return &e;
        return nullptr;
    }

    bool do_push(SyncEntry& e) {
        std::ifstream f(e.file);
        if (!f.is_open()) {
            emit("  [" + e.label + "] file not found: " + e.file, L_ERR);
            return false;
        }
        std::string data((std::istreambuf_iterator<char>(f)), {});

        progress("[" + e.label + "] encrypting...");
        std::string enc = encrypt(data);
        if (enc.empty()) { emit("  [" + e.label + "] encryption failed (openssl installed?)", L_ERR); return false; }

        progress("[" + e.label + "] uploading...");
        std::string err = kv_put(e.label, enc);
        if (!err.empty()) { emit("  [" + e.label + "] " + err, L_ERR); return false; }

        e.last_push = now_iso();
        save_cfg();
        emit("  [" + e.label + "] pushed  " + format_short(e.last_push), L_OK);
        return true;
    }

    bool do_pull(SyncEntry& e) {
        progress("[" + e.label + "] downloading...");
        std::string enc = kv_get(e.label);
        if (enc.empty()) { emit("  [" + e.label + "] not found (push first from another machine)", L_ERR); return false; }

        progress("[" + e.label + "] decrypting...");
        std::string data = decrypt(enc);
        if (data.empty()) { emit("  [" + e.label + "] decrypt failed - wrong encryption password?", L_ERR); return false; }

        // Validate it's actually JSON before touching the file
        try { json::parse(data); }
        catch (...) {
            emit("  [" + e.label + "] decrypted data is not valid JSON - wrong encryption password?", L_ERR);
            return false;
        }

        std::rename(e.file.c_str(), (e.file + ".bak").c_str());
        std::ofstream out(e.file);
        if (!out) { emit("  [" + e.label + "] cannot write: " + e.file, L_ERR); return false; }
        out << data;

        e.last_pull = now_iso();
        save_cfg();
        emit("  [" + e.label + "] pulled  " + format_short(e.last_pull), L_OK);
        return true;
    }

    // ── Masked password prompt ───────────────────────────────

    std::string pw_input(const std::string& prompt) {
        std::string buf2;
        while (true) {
            move(LINES - 2, 0); clrtoeol();
            attron(COLOR_PAIR(CP_CYAN) | A_BOLD); addstr((prompt + " ").c_str()); attroff(COLOR_PAIR(CP_CYAN) | A_BOLD);
            addstr(std::string(buf2.size(), '*').c_str());
            move(LINES - 2, (int)prompt.size() + 1 + (int)buf2.size());
            curs_set(1); refresh();
            int ch = getch();
            if (ch == ERR) continue;
            if (ch == '\n' || ch == KEY_ENTER)                     { curs_set(0); return buf2; }
            if (ch == 27)                                           { curs_set(0); return ""; }
            if (ch == KEY_BACKSPACE || ch == 127 || ch == 8)       { if (!buf2.empty()) buf2.pop_back(); }
            else if (ch >= 32 && ch < 127)                         buf2 += (char)ch;
        }
    }

    // ── Commands ─────────────────────────────────────────────

    void cmd_push(const std::vector<std::string>& args) {
        if (bucket_id.empty()) { emit("  bucket ID not set  ->  run: key <id>", L_ERR); return; }
        if (session_pw.empty()) { emit("  encryption password not set  ->  run: password", L_ERR); return; }

        std::vector<SyncEntry*> targets;
        if (args.empty()) {
            for (auto& e : entries) targets.push_back(&e);
        } else {
            for (auto& a : args) {
                auto* ep = find_entry(a);
                if (!ep) { emit("  unknown target: " + a, L_ERR); return; }
                targets.push_back(ep);
            }
        }

        int ok = 0;
        for (auto* ep : targets) if (do_push(*ep)) ++ok;
        emit("  " + std::to_string(ok) + "/" + std::to_string((int)targets.size()) + " pushed",
             ok == (int)targets.size() ? L_OK : L_ERR);
        if (ok > 0) reload_requested = true;
    }

    void cmd_pull(const std::vector<std::string>& args) {
        if (bucket_id.empty()) { emit("  bucket ID not set  ->  run: key <id>", L_ERR); return; }
        if (session_pw.empty()) { emit("  encryption password not set  ->  run: password", L_ERR); return; }

        std::vector<SyncEntry*> targets;
        if (args.empty()) {
            for (auto& e : entries) targets.push_back(&e);
        } else {
            for (auto& a : args) {
                auto* ep = find_entry(a);
                if (!ep) { emit("  unknown target: " + a, L_ERR); return; }
                targets.push_back(ep);
            }
        }

        int ok = 0;
        for (auto* ep : targets) if (do_pull(*ep)) ++ok;
        emit("  " + std::to_string(ok) + "/" + std::to_string((int)targets.size()) + " pulled",
             ok == (int)targets.size() ? L_OK : L_ERR);
        if (ok > 0) reload_requested = true;
    }

    void cmd_status() {
        emit("  bucket   " + (bucket_id.empty() ? "[not set]" : bucket_id));
        emit("  enc password " + std::string(session_pw.empty() ? "[not set]" : "[set for session]"));
        for (auto& e : entries) {
            std::string ps = e.last_push.empty() ? "never" : format_short(e.last_push);
            std::string pl = e.last_pull.empty() ? "never" : format_short(e.last_pull);
            emit("  " + e.label + "       push:" + ps + "  pull:" + pl);
        }
    }

    void cmd_key(const std::vector<std::string>& args) {
        if (args.empty()) { emit("  usage: key <bucket_id>", L_DIM); return; }
        bucket_id = args[0];
        save_cfg();
        emit("  bucket ID saved", L_OK);
    }

    void cmd_password() {
        std::string pw = pw_input("new encryption password:");
        if (pw.empty()) { emit("  cancelled", L_DIM); return; }
        std::string pw2 = pw_input("confirm:");
        if (pw != pw2) { emit("  passwords don't match", L_ERR); return; }
        session_pw = pw;
        emit("  encryption password set for session", L_OK);
    }

    void cmd_help() {
        emit("  commands", L_HEAD);
        emit("    push [td|ae|nt]  encrypt + upload", L_DIM);
        emit("    pull [td|ae|nt]  download + decrypt", L_DIM);
        emit("    status              bucket, encryption password, and last sync times", L_DIM);
        emit("    key <bucket_id>     set kvdb.io bucket ID", L_DIM);
        emit("    password            set encryption password (session only)", L_DIM);
        emit("    clear               clear output", L_DIM);
        emit("    help                show this", L_DIM);
    }

    // ── Command execution ────────────────────────────────────

    void execute(const std::string& raw) {
        std::string trimmed = raw;
        while (!trimmed.empty() && trimmed.front() == ' ') trimmed.erase(trimmed.begin());
        while (!trimmed.empty() && trimmed.back()  == ' ') trimmed.pop_back();
        if (trimmed.empty()) return;

        if (history.empty() || history.back() != trimmed)
            history.push_back(trimmed);
        history_idx = -1;
        history_saved.clear();

        emit("> " + trimmed, L_CMD, true);
        scroll = 0;

        auto tokens = tokenize(trimmed);
        const std::string& cmd = tokens[0];
        std::vector<std::string> args(tokens.begin() + 1, tokens.end());

        if      (cmd == "push")     cmd_push(args);
        else if (cmd == "pull")     cmd_pull(args);
        else if (cmd == "status")   cmd_status();
        else if (cmd == "key")      cmd_key(args);
        else if (cmd == "password") cmd_password();
        else if (cmd == "clear")    { buf.clear(); return; }
        else if (cmd == "help")     cmd_help();
        else emit("  unknown command: " + cmd + "  (try 'help')", L_ERR);

        emit("");
    }

    // ── Drawing ──────────────────────────────────────────────

    int output_lines() const {
        // header(1) + output(N) + status(1) + input(1) + hint(1) = LINES - top_y
        return LINES - top_y - 4;
    }

    void draw_header() {
        attron(COLOR_PAIR(CP_HEADER) | A_BOLD);
        move(top_y, 0);
        for (int i = 0; i < COLS; ++i) addch(' ');
        move(top_y, 1); addstr("cn");
        attroff(COLOR_PAIR(CP_HEADER) | A_BOLD);
    }

    void draw_output() {
        int avail    = output_lines();
        int start_y  = top_y + 1;
        int total    = (int)buf.size();
        int from     = std::max(0, total - avail - scroll);
        int to       = std::max(0, total - scroll);

        int y = start_y;
        for (int i = from; i < to && y < start_y + avail; ++i, ++y) {
            auto& l = buf[i];
            move(y, 0);

            switch (l.type) {
                case L_CMD:
                    attron(COLOR_PAIR(CP_CYAN) | A_BOLD);
                    addstr(trunc(l.text, COLS - (int)l.ts.size() - 2).c_str());
                    attroff(COLOR_PAIR(CP_CYAN) | A_BOLD);
                    if (!l.ts.empty() && COLS > 30) {
                        move(y, COLS - (int)l.ts.size() - 1);
                        attron(A_DIM); addstr(l.ts.c_str()); attroff(A_DIM);
                    }
                    break;
                case L_OK:
                    attron(COLOR_PAIR(CP_DONE));
                    addstr(trunc(l.text, COLS - 1).c_str());
                    attroff(COLOR_PAIR(CP_DONE));
                    break;
                case L_ERR:
                    attron(COLOR_PAIR(CP_PRI_URGENT));
                    addstr(trunc(l.text, COLS - 1).c_str());
                    attroff(COLOR_PAIR(CP_PRI_URGENT));
                    break;
                case L_HEAD:
                    attron(A_BOLD | COLOR_PAIR(CP_SELECTED));
                    addstr(trunc(l.text, COLS - 1).c_str());
                    attroff(A_BOLD | COLOR_PAIR(CP_SELECTED));
                    break;
                case L_DIM:
                    attron(A_DIM);
                    addstr(trunc(l.text, COLS - 1).c_str());
                    attroff(A_DIM);
                    break;
                case L_OUT:
                    addstr(trunc(l.text, COLS - 1).c_str());
                    break;
            }
        }

        if (scroll > 0) {
            move(start_y, COLS - 10);
            attron(COLOR_PAIR(CP_YELLOW) | A_DIM);
            addstr("(scrolled)");
            attroff(COLOR_PAIR(CP_YELLOW) | A_DIM);
        }
    }

    void draw_status() {
        if (status_ttl > 0 && !status_msg.empty()) {
            move(LINES - 3, 0);
            attron(A_BOLD);
            addstr((" " + trunc(status_msg, COLS - 2)).c_str());
            attroff(A_BOLD);
            --status_ttl;
        }
    }

    void draw_input() {
        move(LINES - 2, 0); clrtoeol();
        attron(COLOR_PAIR(CP_CYAN) | A_BOLD); addstr("> "); attroff(COLOR_PAIR(CP_CYAN) | A_BOLD);
        int avail = COLS - 3;
        int view_start = 0;
        if (input_pos > avail - 1) view_start = input_pos - avail + 1;
        addstr(input.substr(view_start, avail).c_str());
        move(LINES - 2, 2 + input_pos - view_start);
        curs_set(1);
    }

    void draw_hint() {
        move(LINES - 1, 0); clrtoeol();
        attron(COLOR_PAIR(CP_HINT) | A_DIM);
        addstr(" PgUp/PgDn:scroll  Up/Down:history  Esc:clear  q:quit (when input empty)");
        attroff(COLOR_PAIR(CP_HINT) | A_DIM);
    }

public:
    const char* id()    override { return "cn"; }
    const char* label() override { return "console"; }

    void init() override {
        cfg_path = home(".tb_console.json");
        if (entries.empty()) {
            entries = {
                {"td", home(".td.json"), "", ""},
                {"ae", home(".ae.json"), "", ""},
                {"nt", home(".nt.json"), "", ""},
            };
        }
        load_cfg();
        (void)buf; // no startup messages
    }

    void draw() override {
        draw_header();
        draw_output();
        draw_status();
        draw_input();
        draw_hint();
    }

    bool handle(int ch) override {
        switch (ch) {

            case '\n': case KEY_ENTER: {
                std::string cmd = input;
                input.clear(); input_pos = 0;
                execute(cmd);
                break;
            }

            // Editing
            case KEY_BACKSPACE: case 127: case 8:
                if (input_pos > 0) input.erase(--input_pos, 1);
                break;
            case KEY_DC:
                if (input_pos < (int)input.size()) input.erase(input_pos, 1);
                break;
            case KEY_LEFT:  if (input_pos > 0)               --input_pos; break;
            case KEY_RIGHT: if (input_pos < (int)input.size()) ++input_pos; break;
            case KEY_HOME: case 1: input_pos = 0;                    break;
            case KEY_END:  case 5: input_pos = (int)input.size();    break;
            case 21: input.clear(); input_pos = 0;                   break; // Ctrl+U

            // History
            case KEY_UP: {
                if (history.empty()) break;
                if (history_idx == -1) { history_saved = input; history_idx = (int)history.size() - 1; }
                else if (history_idx > 0) --history_idx;
                input = history[history_idx]; input_pos = (int)input.size();
                break;
            }
            case KEY_DOWN: {
                if (history_idx == -1) break;
                if (history_idx < (int)history.size() - 1) { ++history_idx; input = history[history_idx]; }
                else { history_idx = -1; input = history_saved; }
                input_pos = (int)input.size();
                break;
            }

            // Scroll
            case KEY_PPAGE: {
                int step = std::max(1, output_lines() / 2);
                int max_scroll = std::max(0, (int)buf.size() - output_lines());
                scroll = std::min(scroll + step, max_scroll);
                break;
            }
            case KEY_NPAGE:
                scroll = std::max(0, scroll - std::max(1, output_lines() / 2));
                break;

            // Esc: clear input, or clear screen if already empty
            case 27:
                if (!input.empty()) { input.clear(); input_pos = 0; }
                else { buf.clear(); scroll = 0; }
                break;

            // q only quits when input line is empty
            case 'q':
                if (input.empty()) return false;
                input.insert(input_pos++, 1, 'q');
                break;

            default:
                if (ch >= 32 && ch < 127)
                    input.insert(input_pos++, 1, (char)ch);
                break;
        }
        return true;
    }
};
