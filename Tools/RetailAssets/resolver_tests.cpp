// Offline tests for the Retail dependency resolver: no network, no Retail install.
#include "Engine/Source/Retail/CdnConfig.h"
#include "Engine/Source/Retail/Db2.h"
#include "Engine/Source/Retail/DependencyResolver.h"
#include "Engine/Source/Retail/ParallelFetch.h"

#include <algorithm>
#include <atomic>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <map>
#include <set>
#include <string>
#include <vector>

using namespace Retail::Resolve;

static int failures = 0;
#define CHECK(condition) do { if (!(condition)) { std::fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #condition); ++failures; } } while (0)

// ---- helpers to build chunked M2 files ---------------------------------------------------------
static void Chunk(std::vector<uint8_t>& out, const char* tag, const std::vector<uint32_t>& words) {
    out.insert(out.end(), tag, tag + 4);
    const uint32_t size = uint32_t(words.size() * 4);
    const auto* p = reinterpret_cast<const uint8_t*>(&size);
    out.insert(out.end(), p, p + 4);
    for (uint32_t w : words) { const auto* q = reinterpret_cast<const uint8_t*>(&w); out.insert(out.end(), q, q + 4); }
}

static std::vector<uint8_t> Model(const std::vector<uint32_t>& skins, const std::vector<uint32_t>& textures, uint32_t skeleton,
                                  uint32_t physics, const std::vector<uint32_t>& anims, const std::vector<uint32_t>& particles = {}, const std::vector<uint32_t>& recursive = {}) {
    std::vector<uint8_t> out;
    Chunk(out, "MD21", {0x3032444d, 0, 0, 0});
    Chunk(out, "SFID", skins);
    Chunk(out, "TXID", textures);
    if (skeleton) Chunk(out, "SKID", {skeleton});
    if (physics) Chunk(out, "PFID", {physics});
    std::vector<uint32_t> afid;
    for (size_t i = 0; i + 1 < anims.size(); i += 2) { afid.push_back(anims[i]); afid.push_back(anims[i + 1]); }
    if (!afid.empty()) Chunk(out, "AFID", afid);
    if (!particles.empty()) Chunk(out, "GPID", particles);
    if (!recursive.empty()) Chunk(out, "RPID", recursive);
    return out;
}

static std::vector<uint8_t> Skeleton(const std::vector<uint32_t>& anims, const std::vector<uint32_t>& bones) {
    std::vector<uint8_t> out;
    Chunk(out, "SKL1", {0, 0});
    std::vector<uint32_t> afid;
    for (size_t i = 0; i + 1 < anims.size(); i += 2) { afid.push_back(anims[i]); afid.push_back(anims[i + 1]); }
    if (!afid.empty()) Chunk(out, "AFID", afid);
    if (!bones.empty()) Chunk(out, "BFID", bones);
    return out;
}

class FakeFetcher : public Fetcher {
public:
    std::map<std::pair<uint32_t, int>, std::vector<uint8_t>> files;
    std::vector<std::pair<uint32_t, int>> requests;
    bool Get(uint32_t id, FileKind kind, std::vector<uint8_t>& bytes, std::string& error) override {
        requests.push_back({id, int(kind)});
        const auto found = files.find({id, int(kind)});
        if (found == files.end()) { error = "not on CDN"; return false; }
        bytes = found->second;
        return true;
    }
};

static void TestSha() {
    CHECK(Sha256Hex(nullptr, 0) == "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855");
    CHECK(Sha256Hex(reinterpret_cast<const uint8_t*>("abc"), 3) == "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad");
    std::vector<uint8_t> million(1000000, 'a');
    CHECK(Sha256Hex(million.data(), million.size()) == "cdc76e5c9914fb9281a1c7e284d73e67f1809a48a497200e046d39ccc7112cd0");
    std::vector<uint8_t> boundary(56, 'a');  // forces a second padding block
    CHECK(Sha256Hex(boundary.data(), boundary.size()) == "b35439a4ac6f0948b6d6f9e3c6af0f5f590ce20f1bde7090ef7970686ec6738a");
}

static void TestModelParse() {
    ModelFiles files;
    std::string error;
    // (animation id << 16 | variation) is packed by the parser from {u16 id, u16 sub}: word = id | sub << 16.
    const auto bytes = Model({10, 11}, {20, 0, 21}, 30, 40, {1 | (2u << 16), 50, 3, 51}, {60}, {61});
    CHECK(ParseModelFiles(bytes, files, error));
    CHECK((files.skins == std::vector<uint32_t>{10, 11}));
    CHECK((files.textures == std::vector<uint32_t>{20, 21}));  // zero (runtime-resolved) textures are skipped
    CHECK(files.skeleton == 30 && files.physics == 40);
    CHECK(files.animations.size() == 2 && files.animations[0].second == 50 && files.animations[0].first == (1u << 16 | 2u));
    CHECK((files.particleModels == std::vector<uint32_t>{60, 61}));  // GPID then RPID
    const char legacy[8] = {'M', 'D', '2', '0', 0, 0, 0, 0};
    CHECK(ParseModelFiles(std::vector<uint8_t>(legacy, legacy + 8), files, error) && files.legacy);
    auto truncated = bytes;
    truncated.resize(truncated.size() - 3);
    CHECK(!ParseModelFiles(truncated, files, error));
    CHECK(!ParseModelFiles(std::vector<uint8_t>{1, 2, 3}, files, error));
    const char junk[8] = {'J', 'U', 'N', 'K', 0, 0, 0, 0};
    CHECK(!ParseModelFiles(std::vector<uint8_t>(junk, junk + 8), files, error));
}

static Db SampleDb() {
    Db db;
    db.items[100] = {4, 4, 4, 1, 900};
    db.sparse[100] = {"Test Helm", 4, 80, 70, 1, 123, 456, 0xFFFFFFFFu, 0xFFFF, 1};
    db.items[101] = {4, 4, 4, 5, 0};  // chest, second item sharing the same display
    db.modified[1] = {1, 100, 0, 11, 0};
    db.modified[2] = {2, 100, 1, 12, 1};  // second colourway
    db.modified[3] = {3, 101, 0, 11, 0};
    db.modifiedByItem[100] = {2, 1};
    db.modifiedByItem[101] = {3};
    db.appearances[11] = {500, 901};
    db.appearances[12] = {501, 0};
    Db::DisplayRow a{};
    a.flags = 7; a.modelRes[0] = 7000; a.matRes[0] = 8000; a.helmet[0] = 9000;
    a.geoset[0] = 1; a.geoset[2] = 3;
    db.displays[500] = a;
    Db::DisplayRow b{};
    b.modelRes[0] = 7001;
    db.displays[501] = b;
    db.modelFilesByResources[7000] = {2002, 2001};  // female listed first; unisex/male must win
    db.modelFilesByResources[7001] = {2003};
    db.componentModels[2001] = {0, 0, 0, 1};
    db.componentModels[2002] = {1, 0, 0, 1};
    db.texturesByMaterial[8000] = {{3001, 1}};
    db.texturesByMaterial[8100] = {{3002, 2}};
    db.displayMaterials[500] = {{4, 8100}};
    db.helmetGeosets[9000] = {{1, 2, 3}};
    db.setItems[77] = {3, 1};
    return db;
}

static FakeFetcher SampleFiles() {
    FakeFetcher fetch;
    fetch.files[{2001, int(FileKind::Model)}] = Model({4001}, {3003, 3001}, 5001, 6001, {0 | (0u << 16), 7001});
    fetch.files[{2002, int(FileKind::Model)}] = Model({4002}, {3003}, 5001, 0, {});
    fetch.files[{2003, int(FileKind::Model)}] = Model({4003}, {}, 0, 0, {});
    fetch.files[{5001, int(FileKind::Skeleton)}] = Skeleton({0, 7002}, {7003});
    for (uint32_t id : {4001u, 4002u, 4003u}) fetch.files[{id, int(FileKind::Skin)}] = {1};
    for (uint32_t id : {3001u, 3002u, 3003u}) fetch.files[{id, int(FileKind::Texture)}] = {2};
    for (uint32_t id : {7001u, 7002u}) fetch.files[{id, int(FileKind::Animation)}] = {3};
    fetch.files[{7003, int(FileKind::Bone)}] = {4};
    fetch.files[{6001, int(FileKind::Physics)}] = {5};
    fetch.files[{900, int(FileKind::Icon)}] = {6};
    fetch.files[{901, int(FileKind::Icon)}] = {7};
    return fetch;
}

static bool Has(const Manifest& m, uint32_t id, FileKind kind) {
    for (const auto& f : m.files) if (f.id == id && f.kind == kind) return true;
    return false;
}

static void TestWalk() {
    const Db db = SampleDb();
    FakeFetcher fetch = SampleFiles();
    Resolver resolver(db, fetch);
    Manifest manifest;
    std::string error;
    Request request;
    request.items = {100};
    CHECK(resolver.Run(request, manifest, error));
    CHECK(manifest.items.size() == 1);
    const Item& item = manifest.items[0];
    CHECK(item.name == "Test Helm" && item.quality == 4 && item.allowableClass == -1 && item.allowableRace == -1);
    CHECK(item.displayInfoId == 500);                          // modifier 0 is primary
    CHECK((item.variantDisplayIds == std::vector<uint32_t>{501}));
    CHECK(item.iconFileId == 900);                             // Item.db2 icon beats the appearance default
    CHECK(manifest.displays.size() == 2);
    // Everything reachable is present exactly once.
    for (uint32_t id : {2001u, 2002u, 2003u}) CHECK(Has(manifest, id, FileKind::Model));
    for (uint32_t id : {4001u, 4002u, 4003u}) CHECK(Has(manifest, id, FileKind::Skin));
    for (uint32_t id : {3001u, 3002u, 3003u}) CHECK(Has(manifest, id, FileKind::Texture));
    CHECK(Has(manifest, 5001, FileKind::Skeleton) && Has(manifest, 6001, FileKind::Physics));
    CHECK(Has(manifest, 7001, FileKind::Animation) && Has(manifest, 7002, FileKind::Animation) && Has(manifest, 7003, FileKind::Bone));
    CHECK(Has(manifest, 900, FileKind::Icon) && !Has(manifest, 901, FileKind::Icon));
    CHECK(manifest.missing.empty());
    size_t duplicates = 0;
    for (size_t i = 0; i < manifest.files.size(); ++i)
        for (size_t j = i + 1; j < manifest.files.size(); ++j)
            duplicates += manifest.files[i].id == manifest.files[j].id && manifest.files[i].kind == manifest.files[j].kind;
    CHECK(duplicates == 0);
    int skeletonFetches = 0;
    for (const auto& r : fetch.requests) skeletonFetches += r.first == 5001;
    CHECK(skeletonFetches == 1);  // shared by two models, fetched once
    const Display& display = manifest.displays.at(500);
    CHECK(display.models[0].size() == 2 && display.geosetGroup[2] == 3 && display.flags == 7);
    CHECK(display.files.count(7003) == 1 && display.files.count(3002) == 1);  // skeleton bones + material-res texture
    CHECK(display.sectionTextures.size() == 1 && display.sectionTextures[0].first == 4);
    CHECK(display.helmetGeosets[0].size() == 1);

    const std::string json = ToJson(manifest);
    CHECK(json.find("\"path\":\"retail/2001.m2\"") != std::string::npos);
    CHECK(json.find("\"model_file_ids\":[2001,0]") != std::string::npos);     // male variant beats the listed-first female one
    CHECK(json.find("\"sha256\":\"") != std::string::npos);
    int depth = 0;
    for (char c : json) depth += c == '{' || c == '[' ? 1 : c == '}' || c == ']' ? -1 : 0;
    CHECK(depth == 0);
    CHECK(ToJson(manifest) == json);  // deterministic
}

static void TestSetsAndFailures() {
    const Db db = SampleDb();
    FakeFetcher fetch = SampleFiles();
    fetch.files.erase({3001, int(FileKind::Texture)});
    Resolver resolver(db, fetch);
    Manifest manifest;
    std::string error;
    Request request;
    request.sets = {77};
    request.displays = {999};
    CHECK(resolver.Run(request, manifest, error));
    CHECK(manifest.items.size() == 2);                          // items 100 and 101
    CHECK(manifest.missing.size() == 1 && manifest.missing[0].find("texture:3001") == 0);
    bool warned = false;
    for (const auto& w : manifest.warnings) warned |= w.find("display:999") == 0;
    CHECK(warned);
    Manifest empty;
    Request none;
    CHECK(!Resolver(db, fetch).Run(none, empty, error));
    Manifest graph;
    FakeFetcher graphFetch = SampleFiles();
    Resolver graphOnly(db, graphFetch);
    graphOnly.SetDownload(false);
    Request one;
    one.items = {100};
    CHECK(graphOnly.Run(one, graph, error));
    bool textureFetched = false;
    for (const auto& r : graphFetch.requests) textureFetched |= r.second == int(FileKind::Texture);
    CHECK(!textureFetched && Has(graph, 3001, FileKind::Texture));
}

// ---- WDC5 sparse tables --------------------------------------------------------------------
static void Put32(std::vector<uint8_t>& v, uint32_t x) { for (int i = 0; i < 4; ++i) v.push_back(uint8_t(x >> (8 * i))); }
static void Put16(std::vector<uint8_t>& v, uint16_t x) { v.push_back(uint8_t(x)); v.push_back(uint8_t(x >> 8)); }

static void TestSparseTable() {
    using namespace Retail::Db2;
    // Two fields: int32 Value and an inline string Name. Records 3 ("alpha", 30) and 9 ("be", 90).
    const std::vector<std::pair<uint32_t, std::pair<uint32_t, std::string>>> records = {{3, {30, "alpha"}}, {9, {90, "be"}}};
    std::vector<uint8_t> blob;
    const uint32_t headerSize = 204, sectionSize = 40, metaSize = 2 * 4 + 2 * 24;
    const uint32_t fileOffset = headerSize + sectionSize + metaSize;
    std::vector<std::pair<uint32_t, uint16_t>> map;
    for (const auto& r : records) {
        const uint32_t start = fileOffset + uint32_t(blob.size());
        Put32(blob, r.second.first);
        blob.insert(blob.end(), r.second.second.begin(), r.second.second.end());
        blob.push_back(0);
        map.push_back({start, uint16_t(fileOffset + blob.size() - start)});
    }
    std::vector<uint8_t> file;
    file.insert(file.end(), {'W', 'D', 'C', '5'});
    Put32(file, 5);
    file.resize(8 + 128, 0);
    for (uint32_t v : {uint32_t(records.size()), 2u, 9u, 0u, 1u, 2u, 3u, 9u, 0u}) Put32(file, v);  // counts .. locale
    Put16(file, 0x5); Put16(file, 0);                                                              // flags sparse|non-inline id, id index
    for (uint32_t v : {2u, 0u, 0u, 2u * 24, 0u, 0u, 1u}) Put32(file, v);
    CHECK(file.size() == headerSize);
    for (uint32_t v : {0u, 0u}) Put32(file, v);                                                   // tact key
    Put32(file, fileOffset); Put32(file, uint32_t(records.size())); Put32(file, 0);
    const uint32_t recordsEnd = fileOffset + uint32_t(blob.size());
    Put32(file, recordsEnd); Put32(file, uint32_t(records.size() * 4)); Put32(file, 0);
    Put32(file, uint32_t(records.size())); Put32(file, 0);
    for (int i = 0; i < 2; ++i) { Put16(file, 0); Put16(file, 0); }                               // field meta
    for (int i = 0; i < 2; ++i) { Put16(file, 0); Put16(file, 32); Put32(file, 0); Put32(file, 0); Put32(file, 0); Put32(file, 32); Put32(file, 0); }
    CHECK(file.size() == fileOffset);
    file.insert(file.end(), blob.begin(), blob.end());
    for (const auto& r : records) Put32(file, r.first);                                          // id list
    for (const auto& m : map) { Put32(file, m.first); Put16(file, m.second); }                    // offset map

    static constexpr Field fields[] = {{"Value"}, {"Name", 1, true}};
    const Definition definition{"sparse", "sparse", 0, fields};
    Table table;
    std::string error;
    CHECK(table.LoadBytes(definition, file, &error));
    CHECK(table.Rows().size() == 2);
    if (table.Rows().size() == 2) {
        const auto* three = table.Find(3);
        const auto* nine = table.Find(9);
        CHECK(three && nine);
        if (three && nine) {
            CHECK(table.Value(*three, "Value") == 30 && table.Text(*three, "Name") == "alpha");
            CHECK(table.Value(*nine, "Value") == 90 && table.Text(*nine, "Name") == "be");
        }
    }
    static constexpr Field wrong[] = {{"Only"}};
    CHECK(!table.LoadBytes(Definition{"w", "w", 0, wrong}, file, &error));
}

// ---- cdn_config.json ------------------------------------------------------------------------
static void TestConfig() {
    using namespace Retail::Cdn;
    std::map<std::string, std::string> values;
    std::string error;
    CHECK(ParseFlatJson("{ \"product\": \"wow_ptr\", \"region\":\"eu\", \"locale\": 2, \"build\":\"" + std::string(32, 'a') + "\", \"cdn\": null }", values, error));
    Config config;
    CHECK(Apply(values, config, error));
    CHECK(config.product == "wowt" && config.region == "eu" && config.locale == "2" && config.cdn.empty());
    CHECK(NormalizeProduct("WOW_BETA") == "wow_beta" && NormalizeProduct("wow") == "wow");
    const std::string ini = ToIni(config, "/tmp/cfg");
    CHECK(ini.find("mode=online\n") == 0 && ini.find("product=wowt\n") != std::string::npos && ini.find("region=eu\n") != std::string::npos);
    CHECK(ini.find("cdn=") == std::string::npos);
    config.cdn = std::string(32, 'b'); config.version = "12.1.7.70892";
    CHECK(ToIni(config, "/tmp/cfg").find("version=12.1.7.70892") != std::string::npos);
    CHECK(!ParseFlatJson("[1]", values, error));
    CHECK(!ParseFlatJson("{\"a\" 1}", values, error));
    std::map<std::string, std::string> bad{{"proudct", "x"}};
    CHECK(!Apply(bad, config, error));
}

// ---- bulk catalogue, checkpoint journal, prefetch waves ------------------------------------------
class WaveFetcher : public FakeFetcher {
public:
    std::vector<std::vector<PrefetchItem>> waves;
    bool allCached = false;
    void Prefetch(const std::vector<PrefetchItem>& items) override { waves.push_back(items); }
    bool Cached(uint32_t id, FileKind kind) override { return allCached && files.count({id, int(kind)}); }
};

static Db CatalogDb() {
    Db db;
    // id, class, subclass, inventory type, expansion, quality, appearance, display
    struct Row { uint32_t id, cls, sub, inv, expansion, quality, appearance, display; };
    const Row rows[] = {
        {1, 4, 4, 1, 0, 4, 11, 500},   // plate helm
        {2, 4, 4, 1, 0, 4, 11, 500},   // same appearance as item 1: a duplicate by default
        {3, 4, 4, 5, 9, 3, 12, 501},   // chest, newer expansion
        {4, 4, 6, 14, 0, 3, 13, 502},  // shield: a weapon-group item
        {5, 2, 7, 13, 10, 5, 14, 503}, // sword
        {6, 2, 2, 15, 10, 1, 15, 504}, // bow
        {7, 4, 0, 2, 0, 2, 16, 505},   // neck: not an armor appearance
        {8, 4, 1, 3, 0, 0, 17, 506},   // poor-quality cloth shoulder
        {9, 4, 4, 1, 0, 4, 0, 0},      // no appearance at all
    };
    for (const Row& r : rows) {
        db.items[r.id] = {r.cls, r.sub, 0, r.inv, 0};
        db.sparse[r.id] = {"x", r.quality, 1, 1, 0, 0, 0, 0xFFFFFFFFu, 0xFFFF, r.inv, r.expansion};
        db.modifiedByItem[r.id] = {r.id};
        db.modified[r.id] = {r.id, r.id, 0, r.appearance, 0};
        if (r.appearance) db.appearances[r.appearance] = {r.display, 0};
    }
    db.setItems[1] = {1, 3};
    db.setItems[2] = {5};
    db.setExpansion[1] = 0;
    db.setExpansion[2] = 10;
    return db;
}

static std::vector<uint32_t> Sorted(std::vector<uint32_t> v) { std::sort(v.begin(), v.end()); return v; }

static void TestCatalog() {
    CHECK(IsArmorSlot(4, 4, 1) && IsArmorSlot(4, 0, 16) && !IsArmorSlot(4, 6, 14) && !IsArmorSlot(4, 0, 2) && !IsArmorSlot(2, 7, 13));
    CHECK(IsWeaponSlot(2, 2, 15) && IsWeaponSlot(4, 6, 14) && !IsWeaponSlot(4, 4, 1));

    const Db db = CatalogDb();
    CatalogOptions armor; armor.armor = true;
    Request request; size_t sets = 99, items = 99;
    SelectCatalog(db, armor, request, &sets, &items);
    CHECK((Sorted(request.items) == std::vector<uint32_t>{1, 3, 8}) && sets == 0 && items == 3);  // 2 shares 1's appearance; 7 is a neck; 9 has no display

    CatalogOptions every = armor; every.everyItem = true;
    Request all; SelectCatalog(db, every, all);
    CHECK((Sorted(all.items) == std::vector<uint32_t>{1, 2, 3, 8}));

    CatalogOptions weapons; weapons.weapons = true;
    Request w; SelectCatalog(db, weapons, w);
    CHECK((Sorted(w.items) == std::vector<uint32_t>{4, 5, 6}));  // shield, sword, bow

    CatalogOptions modern = armor; modern.weapons = true; modern.minExpansion = 9;
    Request m; SelectCatalog(db, modern, m);
    CHECK((Sorted(m.items) == std::vector<uint32_t>{3, 5, 6}));
    modern.maxExpansion = 9;
    Request narrow; SelectCatalog(db, modern, narrow);
    CHECK((Sorted(narrow.items) == std::vector<uint32_t>{3}));

    CatalogOptions collectible = armor; collectible.weapons = true; collectible.skipUncollectible = true;
    Request c; SelectCatalog(db, collectible, c);
    CHECK((Sorted(c.items) == std::vector<uint32_t>{1, 3, 4, 5, 6}));  // item 8 is Poor quality

    CatalogOptions onlySets; onlySets.sets = true; onlySets.minExpansion = 10;
    Request s; SelectCatalog(db, onlySets, s, &sets, &items);
    CHECK((s.sets == std::vector<uint32_t>{2}) && sets == 1 && s.items.empty());
}

static void TestSharedDisplayOwnership() {
    Db db = SampleDb();
    db.displays[502] = db.displays[501];
    db.displays[502].modelRes[0] = 7000;  // shares model 2001/2002 with display 500
    Request request; request.displays = {500, 502};
    FakeFetcher fetch = SampleFiles();
    Manifest manifest; std::string error;
    CHECK(Resolver(db, fetch).Run(request, manifest, error));
    CHECK(manifest.displays.at(502).files.count(4001) == 1 && manifest.displays.at(502).files.count(7003) == 1);
    CHECK(manifest.displays.at(500).files.count(4001) == 1);
    int fetches = 0;
    for (const auto& r : fetch.requests) fetches += r.first == 2001 && r.second == int(FileKind::Model);
    CHECK(fetches == 1);
}

static void TestJournalResume() {
    namespace fs = std::filesystem;
    const fs::path path = fs::temp_directory_path() / "resolver-journal-test.json";
    fs::remove(path);
    const Db db = SampleDb();
    Request request; request.items = {100};
    std::string first;
    size_t cold = 0;
    {
        FetchJournal journal; std::string error;
        CHECK(journal.Open(path.string(), error));
        WaveFetcher fetch; fetch.files = SampleFiles().files;
        Resolver resolver(db, fetch);
        resolver.SetJournal(&journal);
        Manifest manifest;
        CHECK(resolver.Run(request, manifest, error));
        first = ToJson(manifest);
        cold = fetch.requests.size();
        CHECK(cold > 10 && !fetch.waves.empty() && journal.Loaded() == manifest.files.size());
        // Every wave lists each file at most once, and no file appears in two waves.
        std::set<std::pair<uint32_t, int>> seen;
        bool unique = true;
        for (const auto& wave : fetch.waves) for (const auto& item : wave) unique &= seen.insert({item.fileId, int(item.kind)}).second;
        CHECK(unique);
    }
    {   // A crash can leave a torn final line: it must not poison the reload.
        std::ofstream(path, std::ios::app | std::ios::binary) << "{\"s\":\"done\",\"id\":\"99";
    }
    {
        FetchJournal journal; std::string error;
        CHECK(journal.Open(path.string(), error) && journal.Loaded() > 10 && journal.Failures() == 0);
        WaveFetcher fetch; fetch.files = SampleFiles().files; fetch.allCached = true;
        Resolver resolver(db, fetch);
        resolver.SetJournal(&journal);
        Manifest manifest;
        CHECK(resolver.Run(request, manifest, error));
        CHECK(fetch.requests.empty());                    // nothing was fetched, hashed or parsed again
        CHECK(fetch.waves.empty());                       // and nothing was prefetched
        CHECK(ToJson(manifest) == first);                 // identical result, rebuilt from the checkpoint
    }
    {   // Recorded but evicted from the cache: it is fetched again.
        FetchJournal journal; std::string error;
        CHECK(journal.Open(path.string(), error));
        WaveFetcher fetch; fetch.files = SampleFiles().files; fetch.allCached = false;
        Manifest manifest;
        Resolver resolver(db, fetch);
        resolver.SetJournal(&journal);
        CHECK(resolver.Run(request, manifest, error) && fetch.requests.size() == cold);
        CHECK(ToJson(manifest) == first);
    }
    {   // Failures are logged and retried on the next run, never recorded as done.
        FetchJournal journal; std::string error;
        CHECK(journal.Open(path.string(), error));
        journal.Failed(5, FileKind::Texture, "boom \"quoted\"");
        CHECK(journal.Find(5, FileKind::Texture) == nullptr && journal.Failures() == 1);
    }
    {
        FetchJournal journal; std::string error;
        CHECK(journal.Open(path.string(), error) && journal.Failures() == 1 && journal.Find(5, FileKind::Texture) == nullptr);
        CHECK(!journal.Open("C:\\nonexistent-dir-xyz\\j.json", error));
    }
    fs::remove(path);
}

static void TestParallelHelpers() {
    namespace fs = std::filesystem;
    std::vector<std::atomic<int>> hits(1000);
    RunParallel(hits.size(), 8, [&](size_t index, unsigned) { ++hits[index]; });
    bool once = true;
    for (auto& hit : hits) once &= hit == 1;
    CHECK(once);
    int inline_runs = 0;
    RunParallel(5, 1, [&](size_t, unsigned worker) { inline_runs += worker == 0; });
    CHECK(inline_runs == 5);
    RunParallel(0, 8, [&](size_t, unsigned) { CHECK(false); });
    RunParallel(3, 8, [&](size_t, unsigned) { throw 1; });  // a throwing task must not terminate the process

    int attempts = 0;
    CHECK(WithRetry(4, 1, [&](unsigned n) { ++attempts; return n == 2; }) && attempts == 3);
    attempts = 0;
    CHECK(!WithRetry(3, 1, [&](unsigned) { ++attempts; return false; }) && attempts == 3);

    const fs::path temp = fs::temp_directory_path() / "resolver-parallel-test";
    std::error_code ec;
    fs::remove_all(temp, ec);
    fs::create_directories(temp / "main" / "data");
    fs::create_directories(temp / "main" / "decoded" / "2");
    std::ofstream(temp / "main" / "versions") << "v";
    { std::ofstream big(temp / "main" / "data" / "index.idx", std::ios::binary); std::string block(2 << 20, 'x'); big << block; }
    std::ofstream(temp / "main" / "decoded" / "2" / "id-1.bin") << "do-not-seed";
    std::ofstream(temp / "main" / "data" / "small.index") << "s";
    CHECK(SeedDirectory(temp / "main", temp / "w0", {"decoded"}) == 3);
    CHECK(fs::hard_link_count(temp / "w0" / "data" / "small.index") == 2);  // linked, not copied
    CHECK(fs::hard_link_count(temp / "w0" / "versions") == 1);              // top-level manifest is a private copy
    CHECK(fs::exists(temp / "w0" / "versions") && fs::exists(temp / "w0" / "data" / "index.idx") && !fs::exists(temp / "w0" / "decoded"));
    CHECK(fs::file_size(temp / "w0" / "data" / "index.idx") == (2u << 20));
    CHECK(SeedDirectory(temp / "main", temp / "w0", {"decoded"}) == 0);  // second seed leaves existing files alone

    fs::create_directories(temp / "w0" / "decoded" / "2" / "strict");
    std::ofstream(temp / "w0" / "decoded" / "2" / "strict" / "id-7.bin") << "new";
    std::ofstream(temp / "w0" / "decoded" / "2" / "strict" / "id-8.bin") << "dup";
    std::ofstream(temp / "w0" / "decoded" / "2" / "strict" / "id-9.tmp") << "partial";
    fs::create_directories(temp / "main" / "decoded" / "2" / "strict");
    std::ofstream(temp / "main" / "decoded" / "2" / "strict" / "id-8.bin") << "original";
    CHECK(MergeDirectory(temp / "w0" / "decoded", temp / "main" / "decoded") == 1);
    CHECK(fs::exists(temp / "main" / "decoded" / "2" / "strict" / "id-7.bin"));
    CHECK(!fs::exists(temp / "w0" / "decoded" / "2" / "strict" / "id-7.bin") && !fs::exists(temp / "main" / "decoded" / "2" / "strict" / "id-9.tmp"));
    { std::ifstream in(temp / "main" / "decoded" / "2" / "strict" / "id-8.bin"); std::string text; in >> text; CHECK(text == "original"); }
    CHECK(MergeDirectory(temp / "missing", temp / "main") == 0);
    fs::remove_all(temp, ec);
}

int main() {
    TestCatalog();
    TestSharedDisplayOwnership();
    TestJournalResume();
    TestParallelHelpers();
    TestSha();
    TestModelParse();
    TestWalk();
    TestSetsAndFailures();
    TestSparseTable();
    TestConfig();
    if (failures) { std::fprintf(stderr, "%d failure(s)\n", failures); return 1; }
    std::puts("resolver tests passed");
    return 0;
}
