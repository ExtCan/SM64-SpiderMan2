// Minimal INI reader used for the user config and for bindings.ini.
#pragma once

#include <cstdint>
#include <map>
#include <string>
#include <vector>

namespace sm2m {

class Ini {
public:
    bool LoadFile(const std::string& path);
    void LoadString(const std::string& text);

    // Values from `other` override values in this file.
    void Merge(const Ini& other);

    bool Has(const std::string& section, const std::string& key) const;
    std::string GetString(const std::string& section, const std::string& key,
                          const std::string& def = "") const;
    int64_t GetInt(const std::string& section, const std::string& key, int64_t def = 0) const;
    double GetFloat(const std::string& section, const std::string& key, double def = 0.0) const;
    bool GetBool(const std::string& section, const std::string& key, bool def = false) const;

    std::vector<std::string> Sections() const;
    std::vector<std::string> Keys(const std::string& section) const;

    void Set(const std::string& section, const std::string& key, const std::string& value);

    // Edits ini text in place: sets section/key to value, keeping comments,
    // spacing and line endings; adds the key (or the section) if missing.
    static std::string SetValueInText(const std::string& text, const std::string& section, const std::string& key,
                                      const std::string& value);
    // The raw value of section/key in ini text (comment stripped), if present.
    static bool GetValueInText(const std::string& text, const std::string& section, const std::string& key,
                               std::string& value);
    // SetValueInText on a file (written only if it changed). False on I/O errors.
    static bool SetValueInFile(const std::string& path, const std::string& section, const std::string& key,
                               const std::string& value);
    // Several edits, one write (the file is replaced as a whole, so it is
    // never left half-written). False on I/O errors.
    struct Edit {
        std::string section, key, value;
    };
    struct Key {
        std::string section, key;
    };
    // `remove`: keys taken out of the file (their lines go).
    static bool SetValuesInFile(const std::string& path, const std::vector<Edit>& edits,
                                const std::vector<Key>& remove = {});
    // Ini text without section/key's line(s).
    static std::string RemoveKeyInText(const std::string& text, const std::string& section, const std::string& key);

    // Parse helpers (exposed for tests and for other modules).
    static bool ParseInt(const std::string& s, int64_t& out);
    static bool ParseFloat(const std::string& s, double& out);
    static bool ParseBool(const std::string& s, bool& out);
    static std::string Trim(const std::string& s);
    static std::string Lower(const std::string& s);
    // Splits "a, b ,c" into {"a","b","c"} (empty items dropped).
    static std::vector<std::string> SplitList(const std::string& s, char sep = ',');

private:
    // lowercase(section) -> lowercase(key) -> value
    std::map<std::string, std::map<std::string, std::string>> data_;
    std::vector<std::string> sectionOrder_;
};

} // namespace sm2m
