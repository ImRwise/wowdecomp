#include "Engine/Source/Retail/ResolveCli.h"
#include "Engine/Source/Retail/CascSource.h"
#include "Engine/Source/Retail/CdnConfig.h"
#include "Engine/Source/Retail/DependencyResolver.h"
#include "Engine/Source/Retail/ParallelFetch.h"
#include "Engine/Source/Retail/SourceContext.h"

#include <cstdio>
#include <memory>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <map>
#include <sstream>

namespace fs = std::filesystem;

namespace Retail::Resolve {
namespace {

std::string Trim(std::string text) {
    const auto begin = text.find_first_not_of(" \t\r\n");
    if (begin == std::string::npos) return "";
    return text.substr(begin, text.find_last_not_of(" \t\r\n") - begin + 1);
}

bool ParseList(const std::string& text, Request& request, std::string& error) {
    std::string token;
    std::vector<std::string> tokens;
    for (char c : text + ",") {
        if (c == ',' || c == ';' || c == '\n' || c == '\r' || c == ' ' || c == '\t') {
            if (!token.empty()) tokens.push_back(token);
            token.clear();
        } else token += c;
    }
    for (const std::string& entry : tokens) {
        std::string kind = "item", number = entry;
        if (const auto colon = entry.find(':'); colon != std::string::npos) { kind = entry.substr(0, colon); number = entry.substr(colon + 1); }
        char* end = nullptr;
        const unsigned long value = std::strtoul(number.c_str(), &end, 10);
        if (number.empty() || *end || !value || value > 0xFFFFFFFFul) { error = "invalid ID in list: " + entry; return false; }
        if (kind == "item") request.items.push_back(uint32_t(value));
        else if (kind == "display") request.displays.push_back(uint32_t(value));
        else if (kind == "set") request.sets.push_back(uint32_t(value));
        else { error = "unknown list prefix: " + kind; return false; }
    }
    return true;
}

std::map<std::string, std::string> ReadIni(const fs::path& path) {
    std::map<std::string, std::string> values;
    std::ifstream input(path);
    std::string line;
    while (std::getline(input, line)) {
        line = Trim(line);
        const auto equal = line.find('=');
        if (line.empty() || line[0] == '#' || line[0] == ';' || equal == std::string::npos) continue;
        values[Trim(line.substr(0, equal))] = Trim(line.substr(equal + 1));
    }
    return values;
}

}

int ResolveMain(int argc, char** argv) {
    // argv[1] config, argv[2] == --resolve-items, argv[3] optional list, then flags.
    std::map<std::string, std::string> option;
    bool graphOnly = false;
    CatalogOptions catalog;
    int first = 3;
    std::string listText;
    if (argc > 3 && std::string(argv[3]).rfind("--", 0) != 0) { listText = argv[3]; first = 4; }
    for (int i = first; i < argc; ++i) {
        const std::string flag = argv[i];
        if (flag == "--graph-only") { graphOnly = true; continue; }
        if (flag == "--all-sets") { catalog.sets = true; continue; }
        if (flag == "--all-armor") { catalog.armor = true; continue; }
        if (flag == "--all-weapons") { catalog.weapons = true; continue; }
        if (flag == "--all-catalog") { catalog.sets = catalog.armor = catalog.weapons = true; continue; }
        if (flag == "--skip-uncollectible") { catalog.skipUncollectible = true; continue; }
        if (flag == "--every-item") { catalog.everyItem = true; continue; }
        if (flag.rfind("--", 0) == 0 && i + 1 < argc) { option[flag.substr(2)] = argv[++i]; continue; }
        std::fprintf(stderr, "unexpected argument: %s\n", argv[i]);
        return 2;
    }
    const bool bulk = catalog.sets || catalog.armor || catalog.weapons;
    for (const char* key : {"min-expansion", "max-expansion"}) {
        if (!option.count(key)) continue;
        char* end = nullptr;
        const long value = std::strtol(option[key].c_str(), &end, 10);
        if (option[key].empty() || *end || value < 0) { std::fprintf(stderr, "--%s needs a non-negative expansion number\n", key); return 2; }
        (std::string(key) == "min-expansion" ? catalog.minExpansion : catalog.maxExpansion) = value;
    }
    if (catalog.minExpansion >= 0 && catalog.maxExpansion >= 0 && catalog.minExpansion > catalog.maxExpansion) {
        std::fprintf(stderr, "--min-expansion is greater than --max-expansion\n");
        return 2;
    }
    if (!bulk && (catalog.minExpansion >= 0 || catalog.maxExpansion >= 0 || catalog.skipUncollectible || catalog.everyItem)) {
        std::fprintf(stderr, "--min-expansion/--max-expansion/--skip-uncollectible/--every-item need --all-sets, --all-armor, --all-weapons or --all-catalog\n");
        return 2;
    }
    if (!listText.empty() && listText[0] == '@') {
        std::ifstream file(listText.substr(1));
        if (!file) { std::fprintf(stderr, "cannot read list file %s\n", listText.c_str() + 1); return 2; }
        std::stringstream buffer; buffer << file.rdbuf(); listText = buffer.str();
    }
    Request request;
    std::string error;
    if (!ParseList(listText, request, error)) { std::fprintf(stderr, "%s\n", error.c_str()); return 2; }
    if (request.items.empty() && request.displays.empty() && request.sets.empty() && !bulk) {
        std::fprintf(stderr, "give an item/display/set list or one of --all-sets/--all-armor/--all-weapons/--all-catalog\n");
        return 2;
    }
    unsigned workers = 8;  // graph-only still downloads every M2/skeleton to read its dependency chunks
    if (option.count("workers")) {
        char* end = nullptr;
        const unsigned long value = std::strtoul(option["workers"].c_str(), &end, 10);
        if (option["workers"].empty() || *end || value < 1 || value > 64) { std::fprintf(stderr, "--workers must be 1..64\n"); return 2; }
        workers = unsigned(value);
    }

    // Build the effective ini.
    const fs::path configPath = fs::absolute(argv[1]);
    std::map<std::string, std::string> values;
    fs::path iniPath = configPath, iniDirectory = configPath.parent_path();
    bool temporaryIni = false;
    const bool json = configPath.extension() == ".json";
    if (json) {
        std::ifstream file(configPath);
        if (!file) { std::fprintf(stderr, "cannot read %s\n", configPath.string().c_str()); return 2; }
        std::stringstream buffer; buffer << file.rdbuf();
        std::map<std::string, std::string> flat;
        Cdn::Config config;
        if (!Cdn::ParseFlatJson(buffer.str(), flat, error) || !Cdn::Apply(flat, config, error)) {
            std::fprintf(stderr, "%s: %s\n", configPath.string().c_str(), error.c_str());
            return 2;
        }
        std::map<std::string, std::string> overrides;
        for (const char* key : {"product", "region", "build", "cdn", "version", "locale", "cache"})
            if (option.count(key)) overrides[key] = option[key];
        if (!Cdn::Apply(overrides, config, error)) { std::fprintf(stderr, "%s\n", error.c_str()); return 2; }
        iniPath = iniDirectory / ".resolve-items.ini";
        std::ofstream(iniPath) << Cdn::ToIni(config, iniDirectory);
        temporaryIni = true;
    } else {
        values = ReadIni(configPath);
        bool changed = false;
        for (const char* key : {"product", "region", "build", "cdn", "version", "locale"})
            if (option.count(key)) { values[key] = key == std::string("product") ? Cdn::NormalizeProduct(option[key]) : option[key]; changed = true; }
        if (option.count("cache")) {
            values["path"] = fs::absolute(option["cache"]).lexically_normal().string();
            values.erase("cache");
            changed = true;
        }
        if (changed) {
            iniPath = iniDirectory / ".resolve-items.ini";
            std::ofstream out(iniPath);
            for (const auto& [key, value] : values) out << key << "=" << value << "\n";
            temporaryIni = true;
        }
    }
    values = ReadIni(iniPath);
    struct Cleanup { fs::path path; bool active; ~Cleanup() { std::error_code ec; if (active) fs::remove(path, ec); } } cleanup{iniPath, temporaryIni};

    if (values["build"].empty()) {
        std::fprintf(stderr, "no CDN build configured: set \"build\" (and \"cdn\"+\"version\" for archived builds) in the config or pass --build\n");
        return 2;
    }
    if (!Source().Configure(iniPath.string().c_str(), error)) { std::fprintf(stderr, "configure: %s\n", error.c_str()); return 1; }

    std::fprintf(stderr, "loading item DB2 tables (first run downloads them)...\n");
    Db db;
    if (!LoadDb(db, error)) { std::fprintf(stderr, "tables: %s\n", error.c_str()); return 1; }
    if (bulk) {
        size_t sets = 0, items = 0;
        SelectCatalog(db, catalog, request, &sets, &items);
        std::fprintf(stderr, "catalogue selection: %zu set(s), %zu item(s) (%s)\n", sets, items,
                     catalog.everyItem ? "every item" : "one item per appearance");
    }
    std::fprintf(stderr, "resolving %zu item(s), %zu display(s), %zu set(s)...\n", request.items.size(), request.displays.size(), request.sets.size());

    // Checkpoint: scoped to the build, since a FileDataID's bytes change between builds.
    const fs::path output = option.count("out") ? fs::path(option["out"]) : fs::path("resolved_manifest.json");
    fs::path journalPath = output.has_parent_path() ? output.parent_path() : fs::path(".");
    journalPath /= "cdn_fetch_state-" + values["build"].substr(0, 8) + ".json";
    if (option.count("journal")) journalPath = option["journal"];
    if (journalPath.has_parent_path()) fs::create_directories(journalPath.parent_path());
    FetchJournal journal;
    if (!journal.Open(journalPath.string(), error)) { std::fprintf(stderr, "%s\n", error.c_str()); return 1; }
    std::fprintf(stderr, "checkpoint %s: %zu file(s) already recorded\n", journalPath.string().c_str(), journal.Loaded());

    ParallelOptions parallel;
    parallel.workers = workers;
    std::unique_ptr<CascFetcher> fetcher;
    if (workers > 1) fetcher = std::make_unique<ParallelCascFetcher>(iniPath, parallel);
    else fetcher = std::make_unique<CascFetcher>();
    Resolver resolver(db, *fetcher);
    resolver.SetDownload(!graphOnly);
    resolver.SetJournal(&journal);
    Manifest manifest;
    manifest.product = values["product"].empty() ? "wow" : values["product"];
    manifest.region = values["region"].empty() ? "us" : values["region"];
    manifest.build = values["build"];
    manifest.locale = uint32_t(std::strtoul(values["locale"].empty() ? "2" : values["locale"].c_str(), nullptr, 16));
    manifest.buildNumber = values["version"].empty() ? "" : values["version"].substr(values["version"].find_last_of('.') + 1);
    if (!resolver.Run(request, manifest, error)) { std::fprintf(stderr, "resolve: %s\n", error.c_str()); return 1; }

    if (option.count("icons-dir")) {
        const fs::path directory = option["icons-dir"];
        fs::create_directories(directory);
        for (const FileEntry& file : manifest.files) {
            if (file.kind != FileKind::Icon) continue;
            std::vector<uint8_t> bytes;
            if (!fetcher->Get(file.id, FileKind::Icon, bytes, error)) { manifest.missing.push_back("icon:" + std::to_string(file.id) + ": " + error); continue; }
            std::ofstream out(directory / ("RetailIcon_" + std::to_string(file.id) + ".blp"), std::ios::binary);
            out.write(reinterpret_cast<const char*>(bytes.data()), std::streamsize(bytes.size()));
        }
    }

    const std::string json_text = ToJson(manifest);
    if (output.has_parent_path()) fs::create_directories(output.parent_path());
    std::ofstream(output, std::ios::binary) << json_text;
    const auto stats = Source().Stats();
    std::fprintf(stderr, "wrote %s: %zu item(s), %zu display(s), %zu file(s), %zu missing, %zu warning(s); cache reused %llu, downloaded %llu\n",
                 output.string().c_str(), manifest.items.size(), manifest.displays.size(), manifest.files.size(),
                 manifest.missing.size(), manifest.warnings.size(), (unsigned long long)stats.reused, (unsigned long long)stats.acquired);
    return manifest.missing.empty() ? 0 : 3;
}

}
