#pragma once

#include "json.h"

#include <algorithm>
#include <ctime>
#include <map>
#include <regex>
#include <set>
#include <string>
#include <vector>

// ── Time ────────────────────────────────────────────────────

inline std::string now_iso() {
    auto t = std::time(nullptr);
    char buf[32];
    std::strftime(buf, sizeof(buf), "%Y-%m-%dT%H:%M:%S", std::localtime(&t));
    return buf;
}

inline std::string format_date(const std::string& iso) {
    if (iso.size() < 10) return iso;
    return iso.substr(0, 10);
}

inline std::string format_short(const std::string& iso) {
    if (iso.size() < 16) return iso;
    return iso.substr(5, 5) + " " + iso.substr(11, 5); // MM-DD HH:MM
}

// ── Data Structures ─────────────────────────────────────────

struct Note {
    std::string text;
    std::string timestamp;
};

struct StatusDef {
    std::string name;   // "waiting", "blocked", etc.
    std::string label;  // single char shown in list view
    int color;          // 1=red 2=green 3=yellow 4=blue 5=magenta 6=cyan

    static std::vector<StatusDef> defaults() {
        return {
            {"waiting", "w", 3},
            {"blocked", "b", 1},
        };
    }
};

struct Subtask {
    int id = 0;
    std::string text;
    bool done = false;
};

struct Task {
    int id = 0;
    std::string text;
    std::string description;
    std::string folder;
    bool done = false;
    int priority = 0; // 0=normal 1=low 2=high 3=urgent
    std::string created;
    std::string completed_at;
    std::vector<std::string> tags;
    std::vector<Note> notes;
    std::vector<Subtask> subtasks;
    std::map<std::string, std::string> statuses;
};

struct State {
    int next_id = 1;
    std::string created;
    std::vector<Task> tasks;
    std::vector<Task> archive;
    std::vector<StatusDef> status_defs;
    std::vector<std::string> folders;
};

// ── Tags ────────────────────────────────────────────────────

inline std::vector<std::string> extract_tags(std::string& text) {
    std::vector<std::string> tags;
    // Quoted: +"multi word tag"
    std::regex qre(R"(\+\"([^\"]+)\")");
    std::smatch m;
    std::string tmp = text;
    while (std::regex_search(tmp, m, qre)) {
        tags.push_back(m[1].str());
        tmp = m.suffix().str();
    }
    text = std::regex_replace(text, qre, "");
    // Simple: +tag
    std::regex sre(R"(\+(\w+))");
    tmp = text;
    while (std::regex_search(tmp, m, sre)) {
        tags.push_back(m[1].str());
        tmp = m.suffix().str();
    }
    text = std::regex_replace(text, sre, "");
    // Trim
    while (!text.empty() && text.back() == ' ') text.pop_back();
    while (!text.empty() && text.front() == ' ') text.erase(text.begin());
    // Deduplicate
    std::set<std::string> seen;
    std::vector<std::string> unique;
    for (auto& t : tags)
        if (seen.insert(t).second) unique.push_back(t);
    return unique;
}

// ── Serialization ───────────────────────────────────────────

inline json::Value task_to_json(const Task& t) {
    json::Object o;
    o["id"] = t.id;
    o["text"] = t.text;
    o["done"] = t.done;
    o["priority"] = t.priority;
    o["created"] = t.created;
    if (!t.completed_at.empty()) o["completed_at"] = t.completed_at;

    json::Array tags;
    for (auto& tag : t.tags) tags.push_back(tag);
    o["tags"] = std::move(tags);

    json::Array notes;
    for (auto& n : t.notes) {
        json::Object no;
        no["text"] = n.text;
        no["timestamp"] = n.timestamp;
        notes.push_back(std::move(no));
    }
    o["notes"] = std::move(notes);

    if (!t.description.empty()) o["description"] = t.description;
    if (!t.folder.empty()) o["folder"] = t.folder;

    if (!t.subtasks.empty()) {
        json::Array subs;
        for (auto& s : t.subtasks) {
            json::Object so;
            so["id"] = s.id;
            so["text"] = s.text;
            so["done"] = s.done;
            subs.push_back(std::move(so));
        }
        o["subtasks"] = std::move(subs);
    }

    if (!t.statuses.empty()) {
        json::Object st;
        for (auto& [k, v] : t.statuses) st[k] = v;
        o["statuses"] = std::move(st);
    }
    return json::Value(std::move(o));
}

inline Task task_from_json(const json::Value& v) {
    Task t;
    t.id = v.int_or("id", 0);
    t.text = v.str_or("text", "");
    t.done = v.bool_or("done", false);
    t.priority = v.int_or("priority", 0);
    // Clamp priority to valid range (old data had priority=4 for backlog)
    if (t.priority > 3) t.priority = 0;
    t.created = v.str_or("created", "");
    t.completed_at = v.str_or("completed_at", "");

    if (v.has("tags") && v.at("tags").is_arr())
        for (auto& tag : v.at("tags").as_arr())
            if (tag.is_str()) t.tags.push_back(tag.as_str());

    if (v.has("notes") && v.at("notes").is_arr())
        for (auto& n : v.at("notes").as_arr())
            t.notes.push_back({n.str_or("text", ""), n.str_or("timestamp", "")});

    t.description = v.str_or("description", "");
    t.folder = v.str_or("folder", "");

    if (v.has("subtasks") && v.at("subtasks").is_arr())
        for (auto& s : v.at("subtasks").as_arr())
            t.subtasks.push_back({s.int_or("id",0), s.str_or("text",""), s.bool_or("done",false)});

    if (v.has("statuses") && v.at("statuses").is_obj())
        for (auto& [k, val] : v.at("statuses").as_obj())
            if (val.is_str()) t.statuses[k] = val.as_str();

    // Migrate old fields
    if (v.has("waiting_on") && v.at("waiting_on").is_str()) {
        auto val = v.at("waiting_on").as_str();
        if (!val.empty() && !t.statuses.count("waiting")) t.statuses["waiting"] = val;
    }
    if (v.has("blocked_by") && v.at("blocked_by").is_str()) {
        auto val = v.at("blocked_by").as_str();
        if (!val.empty() && !t.statuses.count("blocked")) t.statuses["blocked"] = val;
    }
    if (v.bool_or("backlog", false) && !t.statuses.count("backlog"))
        t.statuses["backlog"] = "yes";

    return t;
}

inline json::Value state_to_json(const State& s) {
    json::Object o;
    o["next_id"] = s.next_id;
    o["created"] = s.created;

    json::Array tasks, archive;
    for (auto& t : s.tasks) tasks.push_back(task_to_json(t));
    for (auto& t : s.archive) archive.push_back(task_to_json(t));
    o["tasks"] = std::move(tasks);
    o["archive"] = std::move(archive);

    json::Array defs;
    for (auto& d : s.status_defs) {
        json::Object sd;
        sd["name"] = d.name;
        sd["label"] = d.label;
        sd["color"] = d.color;
        defs.push_back(std::move(sd));
    }
    o["status_defs"] = std::move(defs);

    if (!s.folders.empty()) {
        json::Array fl;
        for (auto& f : s.folders) fl.push_back(f);
        o["folders"] = std::move(fl);
    }

    return json::Value(std::move(o));
}

inline State state_from_json(const json::Value& v) {
    State s;
    s.next_id = v.int_or("next_id", 1);
    s.created = v.str_or("created", now_iso());

    if (v.has("tasks") && v.at("tasks").is_arr())
        for (auto& t : v.at("tasks").as_arr())
            s.tasks.push_back(task_from_json(t));

    if (v.has("archive") && v.at("archive").is_arr())
        for (auto& t : v.at("archive").as_arr())
            s.archive.push_back(task_from_json(t));

    if (v.has("status_defs") && v.at("status_defs").is_arr())
        for (auto& d : v.at("status_defs").as_arr())
            s.status_defs.push_back({d.str_or("name",""), d.str_or("label",""), d.int_or("color",0)});

    if (v.has("folders") && v.at("folders").is_arr())
        for (auto& f : v.at("folders").as_arr())
            if (f.is_str()) s.folders.push_back(f.as_str());

    if (s.status_defs.empty())
        s.status_defs = StatusDef::defaults();
    return s;
}
