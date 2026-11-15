#ifndef ENGINE_SOURCE_RETAIL_CDNCONFIG_H
#define ENGINE_SOURCE_RETAIL_CDNCONFIG_H

// cdn_config.json -> RetailCasc.ini. The JSON is one flat object of strings/numbers:
//   {"product":"wow|wow_beta|wow_ptr","region":"us|eu","build":"<32 hex>","cdn":"<32 hex>",
//    "version":"12.1.7.70892","locale":"2","cache":"Cache/Retail"}
// Command-line flags override file values. wow_ptr is accepted as an alias of CASC product wowt.

#include <cctype>
#include <filesystem>
#include <fstream>
#include <map>
#include <sstream>
#include <string>

namespace Retail::Cdn {

struct Config {
    std::string product = "wow", region = "us", build, cdn, version, locale = "2", cache = "Cache/Retail";
};

inline std::string NormalizeProduct(std::string product) {
    for (char& c : product) c = char(std::tolower(static_cast<unsigned char>(c)));
    return product == "wow_ptr" || product == "ptr" ? "wowt" : product;
}

// Flat-object parser: strings (with \" and \ escapes), numbers, true/false/null.
inline bool ParseFlatJson(const std::string& text, std::map<std::string, std::string>& out, std::string& error) {
    size_t i = 0;
    auto ws = [&] { while (i < text.size() && std::isspace(static_cast<unsigned char>(text[i]))) ++i; };
    auto str = [&](std::string& value) {
        if (i >= text.size() || text[i] != '"') return false;
        ++i;
        for (; i < text.size() && text[i] != '"'; ++i) {
            if (text[i] == '\\' &&i + 1 < text.size()) ++i;
            value += text[i];
        }
        if (i >= text.size()) return false;
        ++i;
        return true;
    };
    ws();
    if (i >= text.size() || text[i++] != '{') { error = "expected a JSON object"; return false; }
    for (;;) {
        ws();
        if (i < text.size() && text[i] == '}') return true;
        std::string key, value;
        if (!str(key)) { error = "expected a string key"; return false; }
        ws();
        if (i >= text.size() || text[i++] != ':') { error = "expected ':'"; return false; }
        ws();
        if (i < text.size() && text[i] == '"') {
            if (!str(value)) { error = "unterminated string"; return false; }
        } else {
            while (i < text.size() && text[i] != ',' && text[i] != '}' && !std::isspace(static_cast<unsigned char>(text[i]))) value += text[i++];
            if (value.empty() || value == "null") value.clear();
        }
        out[key] = value;
        ws();
        if (i < text.size() && text[i] == ',') { ++i; continue; }
        ws();
        if (i < text.size() && text[i] == '}') return true;
        error = "expected ',' or '}'";
        return false;
    }
}

inline bool Apply(const std::map<std::string, std::string>& values, Config& config, std::string& error) {
    for (const auto& [key, value] : values) {
        if (key == "product") config.product = NormalizeProduct(value);
        else if (key == "region") config.region = value;
        else if (key == "build") config.build = value;
        else if (key == "cdn") config.cdn = value;
        else if (key == "version") config.version = value;
        else if (key == "locale") config.locale = value;
        else if (key == "cache") config.cache = value;
        else { error = "unknown cdn_config key: " + key; return false; }
    }
    return true;
}

inline std::string ToIni(const Config& config, const std::filesystem::path& base) {
    std::ostringstream ini;
    ini << "mode=online\n"
        << "path=" << std::filesystem::absolute(base / config.cache).lexically_normal().string() << "\n"
        << "product=" << config.product << "\n"
        << "region=" << config.region << "\n"
        << "build=" << config.build << "\n"
        << "locale=" << config.locale << "\n";
    if (!config.cdn.empty()) ini << "cdn=" << config.cdn << "\nversion=" << config.version << "\n";
    return ini.str();
}

}
#endif
