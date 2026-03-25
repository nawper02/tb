#pragma once

#include "app.h"
#include "json.h"
#include "model.h"

#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <unistd.h>

// ── Console / Sync App ───────────────────────────────────────
//
// Encrypts ~/.td.json and ~/.pb.json with AES-256-CBC (via openssl)
// and stores them in private jsonbin.io bins.
//
// Setup:
//   1. Create a free account at https://jsonbin.io
//   2. Copy your Master Key from the dashboard
//   3. Press 'k' in this app to enter it (saved to ~/.tb_console.json)
//   4. Press 'p' to set an encryption password (session-only, never saved)
//   5. Press 'u' to push, 'd' to pull
//
// On another machine: same steps 1-4, but step 1 reuses the same account.
// The bin IDs are stored in ~/.tb_console.json so you can copy that file
// across machines to wire them up to the same bins.

class ConsoleApp : public AppBase {
    struct SyncEntry {
        std::string label;       // "td" or "pb"
        std::string file;        // ~/.td.json etc.
        std::string bin_id;      // jsonbin.io bin ID
        std::string last_push;
        std::string last_pull;
    };

    std::string cfg_path;
    std::string api_key;
    std::string session_pw;
    std::vector<SyncEntry> entries;
    int cursor = 0;

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
            api_key = v.str_or("api_key", "");
            for (auto& e : entries) {
                e.bin_id    = v.str_or(e.label + "_bin_id",    "");
                e.last_push = v.str_or(e.label + "_last_push", "");
                e.last_pull = v.str_or(e.label + "_last_pull", "");
            }
        } catch (...) {}
    }

    void save_cfg() {
        json::Object o;
        o["api_key"] = api_key;
        for (auto& e : entries) {
            o[e.label + "_bin_id"]    = e.bin_id;
            o[e.label + "_last_push"] = e.last_push;
            o[e.label + "_last_pull"] = e.last_pull;
        }
        std::ofstream f(cfg_path);
        f << json::serialize(json::Value(std::move(o)));
    }

    // ── Shell ────────────────────────────────────────────────

    // Single-quote escape for passing arguments to shell safely
    static std::string sq(const std::string& s) {
        std::string r = "'";
        for (char c : s) { if (c == '\'') r += "'\\''"; else r += c; }
        return r + "'";
    }

    static std::string run(const std::string& cmd) {
        FILE* p = popen(cmd.c_str(), "r");
        if (!p) return "";
        std::string out; char buf[4096]; size_t n;
        while ((n = fread(buf, 1, sizeof(buf), p)) > 0) out.append(buf, n);
        pclose(p);
        return out;
    }

    // ── Crypto ───────────────────────────────────────────────

    std::string encrypt(const std::string& data) {
        std::string tmp = "/tmp/.tb_sync_" + std::to_string(getpid());
        { std::ofstream f(tmp); f << data; }
        std::string out = run("openssl enc -aes-256-cbc -pbkdf2 -base64 -A"
                              " -in " + sq(tmp) +
                              " -pass " + sq("pass:" + session_pw) + " 2>/dev/null");
        std::remove(tmp.c_str());
        while (!out.empty() && (out.back() == '\n' || out.back() == '\r')) out.pop_back();
        return out;
    }

    std::string decrypt(const std::string& data) {
        std::string tmp = "/tmp/.tb_sync_" + std::to_string(getpid());
        { std::ofstream f(tmp); f << data; }
        std::string out = run("openssl enc -d -aes-256-cbc -pbkdf2 -base64 -A"
                              " -in " + sq(tmp) +
                              " -pass " + sq("pass:" + session_pw) + " 2>/dev/null");
        std::remove(tmp.c_str());
        return out;
    }

    // ── jsonbin.io REST API ──────────────────────────────────

    // Minimal JSON string escape (encrypted content is base64 so mostly safe,
    // but we escape properly regardless)
    static std::string jstr(const std::string& s) {
        std::string r = "\"";
        for (unsigned char c : s) {
            if      (c == '"')  r += "\\\"";
            else if (c == '\\') r += "\\\\";
            else if (c == '\n') r += "\\n";
            else if (c == '\r') r += "\\r";
            else if (c == '\t') r += "\\t";
            else if (c < 32)    { char b[8]; snprintf(b, sizeof(b), "\\u%04x", c); r += b; }
            else r += c;
        }
        return r + "\"";
    }

    // Create a new private bin; returns bin ID or "" on failure
    std::string api_create(const std::string& enc) {
        std::string body = "{\"d\":" + jstr(enc) + "}";
        std::string resp = run(
            "curl -s -X POST https://api.jsonbin.io/v3/b"
            " -H 'Content-Type: application/json'"
            " -H 'X-Bin-Private: true'"
            " -H " + sq("X-Master-Key: " + api_key) +
            " -d " + sq(body));
        try {
            auto v = json::parse(resp);
            if (v.has("metadata") && v.at("metadata").has("id"))
                return v.at("metadata").at("id").as_str();
        } catch (...) {}
        return "";
    }

    // Update an existing bin; returns true on success
    bool api_put(const std::string& id, const std::string& enc) {
        std::string body = "{\"d\":" + jstr(enc) + "}";
        std::string resp = run(
            "curl -s -X PUT " + sq("https://api.jsonbin.io/v3/b/" + id) +
            " -H 'Content-Type: application/json'"
            " -H " + sq("X-Master-Key: " + api_key) +
            " -d " + sq(body));
        try { auto v = json::parse(resp); return v.has("record"); } catch (...) {}
        return false;
    }

    // Fetch latest content of a bin; returns encrypted string or "" on failure
    std::string api_get(const std::string& id) {
        std::string resp = run(
            "curl -s " + sq("https://api.jsonbin.io/v3/b/" + id + "/latest") +
            " -H " + sq("X-Master-Key: " + api_key));
        try {
            auto v = json::parse(resp);
            if (v.has("record") && v.at("record").has("d"))
                return v.at("record").at("d").as_str();
        } catch (...) {}
        return "";
    }

    // ── Sync operations ──────────────────────────────────────

    // Show an in-progress message immediately (before the blocking call)
    void progress(const std::string& msg) {
        move(LINES - 3, 0); clrtoeol();
        attron(A_BOLD | COLOR_PAIR(CP_YELLOW));
        addstr((" " + msg).c_str());
        attroff(A_BOLD | COLOR_PAIR(CP_YELLOW));
        refresh();
    }

    bool do_push(SyncEntry& e) {
        std::ifstream f(e.file);
        if (!f.is_open()) { flash("Not found: " + e.file); return false; }
        std::string data((std::istreambuf_iterator<char>(f)), {});

        progress("Encrypting " + e.label + "...");
        std::string enc = encrypt(data);
        if (enc.empty()) { flash("Encryption failed (is openssl installed?)"); return false; }

        progress("Uploading " + e.label + "...");
        if (e.bin_id.empty()) {
            e.bin_id = api_create(enc);
            if (e.bin_id.empty()) { flash("Create failed — check API key"); return false; }
        } else {
            if (!api_put(e.bin_id, enc)) { flash("Upload failed — check API key / network"); return false; }
        }

        e.last_push = now_iso();
        save_cfg();
        return true;
    }

    bool do_pull(SyncEntry& e) {
        if (e.bin_id.empty()) { flash(e.label + ": no bin yet — push first"); return false; }

        progress("Downloading " + e.label + "...");
        std::string enc = api_get(e.bin_id);
        if (enc.empty()) { flash("Download failed — check API key / network"); return false; }

        progress("Decrypting " + e.label + "...");
        std::string data = decrypt(enc);
        if (data.empty()) { flash("Decrypt failed (wrong password?)"); return false; }

        // Keep a .bak of whatever was there before
        std::rename(e.file.c_str(), (e.file + ".bak").c_str());

        std::ofstream out(e.file);
        if (!out) { flash("Cannot write: " + e.file); return false; }
        out << data;

        e.last_pull = now_iso();
        save_cfg();
        return true;
    }

    // ── Password input (masked) ──────────────────────────────

    std::string pw_input(const std::string& prompt) {
        std::string buf;
        while (true) {
            move(LINES - 1, 0); clrtoeol();
            attron(A_BOLD); addstr(prompt.c_str()); attroff(A_BOLD);
            addstr(std::string(buf.size(), '*').c_str());
            move(LINES - 1, (int)prompt.size() + (int)buf.size());
            curs_set(1); refresh();
            int ch = getch();
            if (ch == ERR) continue;
            if (ch == '\n' || ch == KEY_ENTER) { curs_set(0); return buf; }
            if (ch == 27)                      { curs_set(0); return ""; }
            if (ch == KEY_BACKSPACE || ch == 127 || ch == 8) { if (!buf.empty()) buf.pop_back(); }
            else if (ch >= 32 && ch < 127) buf += (char)ch;
        }
    }

    // ── Drawing ──────────────────────────────────────────────

    void draw_header() {
        attron(COLOR_PAIR(CP_HEADER) | A_BOLD);
        move(top_y, 0);
        for (int i = 0; i < COLS; ++i) addch(' ');
        move(top_y, 1); addstr("cn - sync console");
        attroff(COLOR_PAIR(CP_HEADER) | A_BOLD);
    }

    void draw_body() {
        int y = top_y + 2;

        auto status_badge = [&](bool ok, const char* yes, const char* no) {
            if (ok) { attron(COLOR_PAIR(CP_DONE));         addstr(yes); attroff(COLOR_PAIR(CP_DONE)); }
            else    { attron(COLOR_PAIR(CP_PRI_URGENT));   addstr(no);  attroff(COLOR_PAIR(CP_PRI_URGENT)); }
        };

        move(y, 2); attron(A_DIM); addstr("API Key:  "); attroff(A_DIM);
        status_badge(!api_key.empty(), "[configured]", "[not set — press 'k']");
        ++y;

        move(y, 2); attron(A_DIM); addstr("Password: "); attroff(A_DIM);
        status_badge(!session_pw.empty(), "[set for session]", "[not set — press 'p']");
        y += 2;

        // Sync entries
        for (int i = 0; i < (int)entries.size(); ++i) {
            auto& e = entries[i];
            bool sel = (i == cursor);
            move(y, 0);
            if (sel) attron(COLOR_PAIR(CP_SELECTED) | A_BOLD);
            addch(sel ? '>' : ' ');
            addch(' ');
            attron(A_BOLD); addstr(e.label.c_str()); attroff(A_BOLD);
            addstr("  ");

            if (e.bin_id.empty()) {
                attron(A_DIM); addstr("[no bin]"); attroff(A_DIM);
            } else {
                attron(A_DIM); addstr(("bin:" + e.bin_id.substr(0, 10) + "..").c_str()); attroff(A_DIM);
            }

            // Timestamps at a fixed column
            std::string ps = e.last_push.empty() ? "never" : format_short(e.last_push);
            std::string pl = e.last_pull.empty() ? "never" : format_short(e.last_pull);
            if (COLS > 50) {
                move(y, 30);
                attron(A_DIM);
                addstr(("push:" + ps + "  pull:" + pl).c_str());
                attroff(A_DIM);
            }

            if (sel) attroff(COLOR_PAIR(CP_SELECTED) | A_BOLD);
            ++y;
        }

        // Hint about bin IDs
        if (!api_key.empty()) {
            y += 1;
            move(y, 2); attron(A_DIM);
            addstr("Tip: copy ~/.tb_console.json to other machines to share bin IDs.");
            attroff(A_DIM);
        }
    }

public:
    const char* id()    override { return "cn"; }
    const char* label() override { return "console"; }

    void init() override {
        cfg_path = home(".tb_console.json");
        entries = {
            {"td", home(".td.json"), "", "", ""},
            {"pb", home(".pb.json"), "", "", ""},
        };
        load_cfg();
    }

    void draw() override {
        draw_header();
        draw_body();
        draw_status_and_hints("u:push-all d:pull-all U:push-sel D:pull-sel K:api-key p:password q:quit");
    }

    bool handle(int ch) override {
        int count = (int)entries.size();
        switch (ch) {
            case 'j': case KEY_DOWN: if (cursor < count - 1) ++cursor; break;
            case 'k': case KEY_UP:   if (cursor > 0)         --cursor; break;

            case 'p': {
                std::string pw = pw_input("Password: ");
                if (pw.empty()) break;
                std::string pw2 = pw_input("Confirm:  ");
                if (pw == pw2) { session_pw = pw; flash("Password set for session"); }
                else flash("Passwords don't match");
                break;
            }

            case 'K': {
                std::string key = text_input("jsonbin.io master key: ", api_key);
                if (!key.empty()) { api_key = key; save_cfg(); flash("API key saved"); }
                break;
            }

            case 'u': {
                if (!ready()) break;
                int ok = 0;
                for (auto& e : entries) if (do_push(e)) ++ok;
                flash("Pushed " + std::to_string(ok) + "/" + std::to_string(count));
                break;
            }

            case 'd': {
                if (!ready()) break;
                int ok = 0;
                for (auto& e : entries) if (do_pull(e)) ++ok;
                flash("Pulled " + std::to_string(ok) + "/" + std::to_string(count));
                break;
            }

            case 'U': {
                if (!ready()) break;
                if (do_push(entries[cursor])) flash("Pushed " + entries[cursor].label);
                break;
            }

            case 'D': {
                if (!ready()) break;
                if (do_pull(entries[cursor])) flash("Pulled " + entries[cursor].label);
                break;
            }

            case 'q': return false;
        }
        return true;
    }

private:
    bool ready() {
        if (api_key.empty())    { flash("Set API key first (press 'K')"); return false; }
        if (session_pw.empty()) { flash("Set password first (press 'p')"); return false; }
        return true;
    }
};
