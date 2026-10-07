#include "ini.h"

#include <algorithm>
#include <cctype>
#include <cerrno>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <sstream>

#ifdef _WIN32
// (Not <windows.h>: its LoadString macro would rename Ini::LoadString.)
extern "C" __declspec(dllimport) int __stdcall MoveFileExW(const wchar_t* from, const wchar_t* to, unsigned long flags);
#endif

namespace sm2m {

std::string Ini::Trim(const std::string& s) {
    size_t b = 0, e = s.size();
    while (b < e && std::isspace(static_cast<unsigned char>(s[b]))) ++b;
    while (e > b && std::isspace(static_cast<unsigned char>(s[e - 1]))) --e;
    return s.substr(b, e - b);
}

std::string Ini::Lower(const std::string& s) {
    std::string r = s;
    std::transform(r.begin(), r.end(), r.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return r;
}

std::vector<std::string> Ini::SplitList(const std::string& s, char sep) {
    std::vector<std::string> out;
    std::string cur;
    for (char c : s) {
        if (c == sep) {
            std::string t = Trim(cur);
            if (!t.empty()) out.push_back(t);
            cur.clear();
        } else {
            cur.push_back(c);
        }
    }
    std::string t = Trim(cur);
    if (!t.empty()) out.push_back(t);
    return out;
}

// Paths are UTF-8 (a narrow path would be read in the ANSI code page on Windows).
bool Ini::LoadFile(const std::string& path) {
    std::ifstream f(std::filesystem::u8path(path), std::ios::binary);
    if (!f) return false;
    std::stringstream ss;
    ss << f.rdbuf();
    LoadString(ss.str());
    return true;
}

void Ini::LoadString(const std::string& text) {
    std::string section;
    std::istringstream in(text);
    std::string raw;
    bool first = true;
    while (std::getline(in, raw)) {
        if (first) {
            first = false;
            // Strip UTF-8 BOM.
            if (raw.size() >= 3 && static_cast<unsigned char>(raw[0]) == 0xEF &&
                static_cast<unsigned char>(raw[1]) == 0xBB && static_cast<unsigned char>(raw[2]) == 0xBF)
                raw = raw.substr(3);
        }
        std::string line = Trim(raw);
        if (line.empty() || line[0] == ';' || line[0] == '#') continue;
        if (line.front() == '[') {
            size_t close = line.find(']');
            if (close == std::string::npos) continue;
            section = Lower(Trim(line.substr(1, close - 1)));
            if (!data_.count(section)) sectionOrder_.push_back(section);
            data_[section];
            continue;
        }
        size_t eq = line.find('=');
        if (eq == std::string::npos) continue;
        std::string key = Lower(Trim(line.substr(0, eq)));
        std::string value = line.substr(eq + 1);
        // Inline comment: " ;" or " #" preceded by whitespace.
        for (size_t i = 1; i < value.size(); ++i) {
            if ((value[i] == ';' || value[i] == '#') &&
                std::isspace(static_cast<unsigned char>(value[i - 1]))) {
                value = value.substr(0, i);
                break;
            }
        }
        value = Trim(value);
        if (value.size() >= 2 && value.front() == '"' && value.back() == '"')
            value = value.substr(1, value.size() - 2);
        if (!data_.count(section)) sectionOrder_.push_back(section);
        data_[section][key] = value;
    }
}

void Ini::Merge(const Ini& other) {
    for (const auto& sec : other.sectionOrder_) {
        auto it = other.data_.find(sec);
        if (it == other.data_.end()) continue;
        if (!data_.count(sec)) sectionOrder_.push_back(sec);
        for (const auto& kv : it->second) data_[sec][kv.first] = kv.second;
    }
}

bool Ini::Has(const std::string& section, const std::string& key) const {
    auto s = data_.find(Lower(section));
    if (s == data_.end()) return false;
    return s->second.count(Lower(key)) != 0;
}

std::string Ini::GetString(const std::string& section, const std::string& key,
                           const std::string& def) const {
    auto s = data_.find(Lower(section));
    if (s == data_.end()) return def;
    auto k = s->second.find(Lower(key));
    if (k == s->second.end()) return def;
    return k->second;
}

bool Ini::ParseInt(const std::string& str, int64_t& out) {
    std::string s = Trim(str);
    if (s.empty()) return false;
    bool neg = false;
    size_t i = 0;
    if (s[0] == '-' || s[0] == '+') {
        neg = s[0] == '-';
        i = 1;
    }
    int base = 10;
    if (s.size() > i + 1 && s[i] == '0' && (s[i + 1] == 'x' || s[i + 1] == 'X')) {
        base = 16;
        i += 2;
    }
    if (i >= s.size()) return false;
    errno = 0;
    char* end = nullptr;
    unsigned long long v = std::strtoull(s.c_str() + i, &end, base);
    if (errno != 0 || end == s.c_str() + i || *end != '\0') return false;
    out = neg ? -static_cast<int64_t>(v) : static_cast<int64_t>(v);
    return true;
}

bool Ini::ParseFloat(const std::string& str, double& out) {
    std::string s = Trim(str);
    if (s.empty()) return false;
    errno = 0;
    char* end = nullptr;
    double v = std::strtod(s.c_str(), &end);
    if (errno != 0 || end == s.c_str() || *end != '\0') return false;
    out = v;
    return true;
}

bool Ini::ParseBool(const std::string& str, bool& out) {
    std::string s = Lower(Trim(str));
    if (s == "1" || s == "true" || s == "yes" || s == "on") {
        out = true;
        return true;
    }
    if (s == "0" || s == "false" || s == "no" || s == "off") {
        out = false;
        return true;
    }
    return false;
}

int64_t Ini::GetInt(const std::string& section, const std::string& key, int64_t def) const {
    int64_t v;
    return ParseInt(GetString(section, key), v) ? v : def;
}

double Ini::GetFloat(const std::string& section, const std::string& key, double def) const {
    double v;
    return ParseFloat(GetString(section, key), v) ? v : def;
}

bool Ini::GetBool(const std::string& section, const std::string& key, bool def) const {
    bool v;
    return ParseBool(GetString(section, key), v) ? v : def;
}

std::vector<std::string> Ini::Sections() const { return sectionOrder_; }

std::vector<std::string> Ini::Keys(const std::string& section) const {
    std::vector<std::string> keys;
    auto s = data_.find(Lower(section));
    if (s == data_.end()) return keys;
    for (const auto& kv : s->second) keys.push_back(kv.first);
    return keys;
}

void Ini::Set(const std::string& section, const std::string& key, const std::string& value) {
    std::string sec = Lower(section);
    if (!data_.count(sec)) sectionOrder_.push_back(sec);
    data_[sec][Lower(key)] = value;
}

namespace {
struct TextLine {
    std::string body; // without the line ending
    std::string eol;  // "\r\n", "\n" or "" (last line)
};

std::vector<TextLine> SplitLines(const std::string& text) {
    std::vector<TextLine> lines;
    size_t pos = 0;
    while (pos < text.size()) {
        size_t nl = text.find('\n', pos);
        TextLine l;
        if (nl == std::string::npos) {
            l.body = text.substr(pos);
            pos = text.size();
        } else {
            size_t end = nl;
            l.eol = "\n";
            if (end > pos && text[end - 1] == '\r') {
                --end;
                l.eol = "\r\n";
            }
            l.body = text.substr(pos, end - pos);
            pos = nl + 1;
        }
        lines.push_back(l);
    }
    return lines;
}

// Section name of a "[name]" line, or "" if it isn't one.
bool SectionOf(const std::string& body, std::string& name) {
    const std::string t = Ini::Trim(body);
    if (t.empty() || t[0] != '[') return false;
    const size_t close = t.find(']');
    if (close == std::string::npos) return false;
    name = Ini::Lower(Ini::Trim(t.substr(1, close - 1)));
    return true;
}

// For a "key = value ; comment" line: the key, and where the value starts/ends.
bool KeyOf(const std::string& body, std::string& key, size_t& valueBegin, size_t& valueEnd) {
    const std::string t = Ini::Trim(body);
    if (t.empty() || t[0] == ';' || t[0] == '#' || t[0] == '[') return false;
    const size_t eq = body.find('=');
    if (eq == std::string::npos) return false;
    key = Ini::Lower(Ini::Trim(body.substr(0, eq)));
    valueBegin = eq + 1;
    valueEnd = body.size();
    for (size_t i = valueBegin + 1; i < body.size(); ++i)
        if ((body[i] == ';' || body[i] == '#') && std::isspace(static_cast<unsigned char>(body[i - 1]))) {
            valueEnd = i;
            break;
        }
    return true;
}
} // namespace

bool Ini::GetValueInText(const std::string& text, const std::string& section, const std::string& key,
                         std::string& value) {
    const std::string sec = Lower(section), k = Lower(key);
    std::string cur;
    for (const TextLine& l : SplitLines(text)) {
        std::string name;
        if (SectionOf(l.body, name)) {
            cur = name;
            continue;
        }
        std::string lk;
        size_t vb, ve;
        if (cur == sec && KeyOf(l.body, lk, vb, ve) && lk == k) {
            value = Trim(l.body.substr(vb, ve - vb));
            return true;
        }
    }
    return false;
}

std::string Ini::SetValueInText(const std::string& text, const std::string& section, const std::string& key,
                                const std::string& value) {
    std::vector<TextLine> lines = SplitLines(text);
    std::string eol = "\n";
    for (const TextLine& l : lines)
        if (!l.eol.empty()) {
            eol = l.eol;
            break;
        }
    const std::string sec = Lower(section), k = Lower(key);
    std::string cur;
    int sectionLine = -1, lastInSection = -1;
    for (size_t i = 0; i < lines.size(); ++i) {
        std::string name;
        if (SectionOf(lines[i].body, name)) {
            cur = name;
            if (cur == sec && sectionLine < 0) {
                sectionLine = int(i);
                lastInSection = int(i);
            }
            continue;
        }
        if (cur != sec) continue;
        std::string lk;
        size_t vb, ve;
        if (KeyOf(lines[i].body, lk, vb, ve)) {
            lastInSection = int(i);
            if (lk != k) continue;
            // Replace the value, keeping the comment where it was if it fits.
            std::string& b = lines[i].body;
            const std::string comment = b.substr(ve);
            std::string head = b.substr(0, vb) + " " + value;
            if (!comment.empty()) {
                if (head.size() + 1 < ve) head.append(ve - head.size(), ' ');
                else head += ' ';
                head += comment;
            }
            b = head;
            std::string out;
            for (const TextLine& l : lines) out += l.body + l.eol;
            return out;
        }
    }
    const std::string newLine = key + " = " + value;
    if (sectionLine < 0) {
        std::string out;
        for (const TextLine& l : lines) out += l.body + l.eol;
        if (!out.empty() && out.back() != '\n') out += eol;
        out += "[" + section + "]" + eol + newLine + eol;
        return out;
    }
    TextLine nl;
    nl.body = newLine;
    nl.eol = eol;
    if (lines[size_t(lastInSection)].eol.empty()) lines[size_t(lastInSection)].eol = eol;
    lines.insert(lines.begin() + lastInSection + 1, nl);
    std::string out;
    for (const TextLine& l : lines) out += l.body + l.eol;
    return out;
}

bool Ini::SetValueInFile(const std::string& path, const std::string& section, const std::string& key,
                         const std::string& value) {
    return SetValuesInFile(path, {{section, key, value}});
}

std::string Ini::RemoveKeyInText(const std::string& text, const std::string& section, const std::string& key) {
    std::vector<TextLine> lines = SplitLines(text);
    const std::string sec = Lower(section), k = Lower(key);
    std::string cur, out;
    for (const TextLine& l : lines) {
        std::string name;
        if (SectionOf(l.body, name)) {
            cur = name;
        } else if (cur == sec) {
            std::string lk;
            size_t vb, ve;
            if (KeyOf(l.body, lk, vb, ve) && lk == k) continue;
        }
        out += l.body + l.eol;
    }
    return out;
}

bool Ini::SetValuesInFile(const std::string& path, const std::vector<Edit>& edits, const std::vector<Key>& remove) {
    const std::filesystem::path p = std::filesystem::u8path(path);
    std::string text;
    {
        std::ifstream f(p, std::ios::binary);
        if (f) {
            std::stringstream ss;
            ss << f.rdbuf();
            text = ss.str();
        }
    }
    std::string out = text;
    for (const Key& r : remove) out = RemoveKeyInText(out, r.section, r.key);
    for (const Edit& e : edits) out = SetValueInText(out, e.section, e.key, e.value);
    if (out == text) return true;
    // Written next to it, then swapped in: a crash or a full disk mid-write
    // never leaves a cut-off file behind.
    std::filesystem::path tmp = p;
    tmp += ".tmp";
    {
        std::ofstream f(tmp, std::ios::binary | std::ios::trunc);
        if (!f) return false;
        f << out;
        f.flush();
        if (!f) {
            f.close();
            std::error_code ec;
            std::filesystem::remove(tmp, ec);
            return false;
        }
    }
#ifdef _WIN32
    const unsigned long kReplaceExisting = 0x1, kWriteThrough = 0x8;
    if (!MoveFileExW(tmp.wstring().c_str(), p.wstring().c_str(), kReplaceExisting | kWriteThrough)) {
        std::error_code ec;
        std::filesystem::remove(tmp, ec);
        return false;
    }
#else
    std::error_code ec;
    std::filesystem::rename(tmp, p, ec);
    if (ec) {
        std::filesystem::remove(tmp, ec);
        return false;
    }
#endif
    return true;
}

} // namespace sm2m
