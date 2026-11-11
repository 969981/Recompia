// SPDX-License-Identifier: GPL-3.0-or-later
// The Markdown and JSON forms of a table of ini settings, see nxrt/setting_doc.h.
#include "nxrt/setting_doc.h"

#include <cctype>
#include <cstdio>
#include <string>
#include <vector>

namespace nxrt::setting_doc {
namespace {

std::string number(double v) { return number_text(v); }

std::string code(const std::string& s) { return "`" + s + "`"; }

// A cell of a Markdown table: '|' would end the cell.
std::string cell(const std::string& s) {
    std::string r;
    for (char c : s) {
        if (c == '|') r += "\\|";
        else if (c == '\n') r += ' ';
        else r += c;
    }
    return r;
}

std::string json_text(const std::string& s) {
    std::string r = "\"";
    for (unsigned char c : s) {
        switch (c) {
            case '"': r += "\\\""; break;
            case '\\': r += "\\\\"; break;
            case '\n': r += "\\n"; break;
            case '\r': r += "\\r"; break;
            case '\t': r += "\\t"; break;
            default:
                if (c < 0x20) {
                    char buf[8];
                    std::snprintf(buf, sizeof buf, "\\u%04x", c);
                    r += buf;
                } else {
                    r += static_cast<char>(c);
                }
        }
    }
    return r + "\"";
}

std::string json_list(const std::vector<std::string>& items) {
    std::string r = "[";
    for (size_t i = 0; i < items.size(); ++i) r += (i ? ", " : "") + json_text(items[i]);
    return r + "]";
}

// "Values" column: what the file accepts.
std::string values_text(const Row& r) {
    std::vector<std::string> parts;
    if (!r.auto_word.empty()) parts.push_back(code(r.auto_word));
    if (r.zero_ok) parts.push_back(code("0"));
    if (!r.choices.empty()) {
        std::string c;
        for (size_t i = 0; i < r.choices.size(); ++i) c += (i ? " " : "") + code(r.choices[i]);
        parts.push_back(c);
    } else if (r.has_min && r.has_max) {
        parts.push_back(code(number(r.min)) + " to " + code(number(r.max)) + (r.clamped ? " (clamped into it)" : ""));
        if (r.type == "ratio") parts.push_back("W:H (" + code("16:9") + ")");
    } else if (r.type == "bool") {
        parts.push_back(code("1") + " " + code("0"));
    } else if (r.type == "int" || r.type == "real") {
        parts.push_back(r.type == "int" ? "a whole number" : "a number");
    } else if (r.type == "text") {
        parts.push_back("text");
    } else if (r.type == "key") {
        parts.push_back("a keyboard key name (" + code("F2") + ", " + code("Tab") + ", " + code("Left Ctrl") + ")");
    } else if (r.type == "pad_combo") {
        parts.push_back(code("none") + " or pad buttons joined by " + code("+") + " (" + code("back+start") + ")");
    } else {
        parts.push_back(r.type);
    }
    std::string s;
    for (size_t i = 0; i < parts.size(); ++i) s += (i ? ", " : "") + parts[i];
    return s;
}

std::vector<std::string> sections_in_order(const std::vector<Row>& rows) {
    std::vector<std::string> names;
    for (const Row& r : rows) {
        bool seen = false;
        for (const std::string& n : names) seen = seen || n == r.section;
        if (!seen) names.push_back(r.section);
    }
    return names;
}

}  // namespace

std::string env_name(const std::string& section, const std::string& key) {
    std::string r = "NXRT_";
    auto add = [&r](const std::string& s) {
        for (unsigned char c : s) r += std::isalnum(c) ? static_cast<char>(std::toupper(c)) : '_';
    };
    add(section);
    r += '_';
    add(key);
    return r;
}

std::string markdown(const std::vector<Row>& rows, Store store) {
    const bool own_env = store == Store::GameIni;
    std::string o = std::string("| Section | Key | Default | Values | ") + (own_env ? "Environment variable" : "Overridden by") +
                    " | Meaning |\n|---|---|---|---|---|---|\n";
    for (const Row& r : rows) {
        if (r.retired) continue;
        std::string key = code(r.key);
        for (const std::string& a : r.aliases) key += " (older: " + code(a) + ")";
        std::string over;
        if (own_env) {
            over = code(env_name(r.section, r.key));
        } else if (!r.nxrt_key.empty()) {
            const size_t dot = r.nxrt_key.find('.');
            const std::string sec = r.nxrt_key.substr(0, dot), k = r.nxrt_key.substr(dot + 1);
            over = code("[" + sec + "] " + k) + ", " + code(env_name(sec, k));
        }
        std::string meaning = r.doc;
        if (r.read_only) meaning += " Read only: never written.";
        if (!r.fallback.empty()) meaning += " A file with " + code(r.fallback) + " and without this key gives it the value of " + code(r.fallback) + ".";
        const std::string def = r.optional ? "unset" : r.def.empty() ? std::string() : cell(code(r.def));
        o += "| " + code(r.section) + " | " + cell(key) + " | " + def + " | " + cell(values_text(r)) +
             " | " + cell(over) + " | " + cell(meaning) + " |\n";
    }
    bool any = false;
    for (const Row& r : rows) any = any || r.retired;
    if (any) {
        o += "\nRetired keys, read and ignored without a warning (the next save leaves them out):";
        std::string sec;
        for (const Row& r : rows) {
            if (!r.retired) continue;
            if (r.section != sec) {
                o += std::string(sec.empty() ? "" : ";") + " " + code("[" + r.section + "]");
                sec = r.section;
            } else {
                o += ",";
            }
            o += " " + code(r.key);
        }
        o += ".\n";
    }
    return o;
}

std::string schema_json(const std::string& file, const std::vector<Row>& rows, Store store) {
    std::string o = "{\n  \"file\": " + json_text(file) + ",\n  \"sections\": [";
    bool first_sec = true;
    for (const std::string& sec : sections_in_order(rows)) {
        bool first_key = true;
        std::string keys;
        for (const Row& r : rows) {
            if (r.section != sec || r.retired) continue;
            std::string k = "        {\"key\": " + json_text(r.key) + ", \"type\": " + json_text(r.type);
            if (r.optional) k += ", \"optional\": true";
            else if (!r.read_only) k += ", \"default\": " + json_text(r.def);
            if (r.has_min) k += ", \"min\": " + number(r.min);
            if (r.has_max) k += ", \"max\": " + number(r.max);
            if (r.clamped) k += ", \"clamped\": true";
            if (r.zero_ok) k += ", \"zero_ok\": true";
            if (!r.choices.empty()) k += ", \"choices\": " + json_list(r.choices);
            if (!r.choice_details.empty()) {
                k += ", \"choice_details\": {";
                for (size_t i = 0; i < r.choice_details.size(); ++i)
                    k += (i ? ", " : "") + json_text(r.choice_details[i].first) + ": " + json_list(r.choice_details[i].second);
                k += "}";
            }
            if (!r.auto_word.empty()) k += ", \"auto\": " + json_text(r.auto_word);
            if (!r.aliases.empty()) k += ", \"aliases\": " + json_list(r.aliases);
            if (!r.fallback.empty()) k += ", \"fallback\": " + json_text(r.fallback);
            if (store == Store::GameIni) {
                k += ", \"env\": " + json_text(env_name(r.section, r.key));
            } else if (!r.nxrt_key.empty()) {
                const size_t dot = r.nxrt_key.find('.');
                k += ", \"overridden_by\": {\"ini\": " + json_text(r.nxrt_key) + ", \"env\": " +
                     json_text(env_name(r.nxrt_key.substr(0, dot), r.nxrt_key.substr(dot + 1))) + "}";
            }
            if (r.read_only) k += ", \"read_only\": true";
            if (!r.group.empty()) k += ", \"group\": " + json_text(r.group);
            if (!r.parent.empty()) k += ", \"parent\": " + json_text(r.parent);
            if (r.opt_in) k += ", \"opt_in\": true";
            if (r.hidden) k += ", \"hidden\": true";
            k += ", \"doc\": " + json_text(r.doc) + "}";
            keys += (first_key ? "\n" : ",\n") + k;
            first_key = false;
        }
        if (first_key) continue;   // a section with retired keys only
        o += std::string(first_sec ? "\n" : ",\n") + "    {\"name\": " + json_text(sec) + ", \"keys\": [" + keys + "\n    ]}";
        first_sec = false;
    }
    o += "\n  ],\n  \"retired\": [";
    bool first = true;
    for (const Row& r : rows) {
        if (!r.retired) continue;
        o += std::string(first ? "\n" : ",\n") + "    {\"section\": " + json_text(r.section) + ", \"key\": " + json_text(r.key) + "}";
        first = false;
    }
    o += first ? "]\n}\n" : "\n  ]\n}\n";
    return o;
}

}  // namespace nxrt::setting_doc
