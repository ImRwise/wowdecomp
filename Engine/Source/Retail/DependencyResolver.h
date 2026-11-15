#ifndef ENGINE_SOURCE_RETAIL_DEPENDENCYRESOLVER_H
#define ENGINE_SOURCE_RETAIL_DEPENDENCYRESOLVER_H

// Walks Retail item data down to the files a client needs:
//   Item / TransmogSet -> ItemModifiedAppearance -> ItemAppearance -> ItemDisplayInfo
//     -> ModelFileData (+ ComponentModelFileData) -> M2 -> .skin .anim .phys .skel .bone, textures, particle models
//     -> TextureFileData (+ ComponentTextureFileData), ItemDisplayInfoMaterialRes / ModelMatRes
//     -> HelmetGeosetData
//     -> icon BLP
// The walk is separated from storage: `Db` holds plain rows (loaded from CDN DB2 tables by
// LoadDb, or filled directly by tests) and `Fetcher` supplies original file bytes.

#include <cstdint>
#include <map>
#include <set>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

namespace Retail::Db2 { class Table; }

namespace Retail::Resolve {

enum class FileKind { Model, Skin, Animation, Physics, Skeleton, Bone, Texture, Icon };
const char* KindName(FileKind kind);
const char* KindExtension(FileKind kind);

struct Request {
    std::vector<uint32_t> items, displays, sets;
};

struct ComponentFile {
    uint32_t fileId = 0;
    int32_t gender = -1;   // -1: not a component row (or any)
    int32_t classId = -1;
    int32_t race = -1;
    int32_t position = -1; // models only
    uint32_t usage = 0;    // textures only (TextureFileData.UsageType)
    // Textures only. The M2 texture type this file feeds (ItemDisplayInfoModelMatRes.TextureType: 2 object skin, 3 second
    // layer...); -1 for ModelMaterialResourcesID rows and section materials, which name no type. materialId groups the files
    // of one MaterialResources row, so two files of one layer are not mistaken for two layers.
    int32_t textureType = -1;
    uint32_t materialId = 0;
};

struct HelmetGeoset {
    uint32_t raceId = 0, hideGroup = 0, raceBit = 0;
};

struct Display {
    uint32_t id = 0, flags = 0, itemVisual = 0, particleColor = 0;
    uint32_t geosetGroupOverride = 0;
    uint32_t geosetGroup[6] = {}, attachmentGeosetGroup[6] = {}, helmetGeosetVis[2] = {}, modelType[2] = {};
    std::vector<ComponentFile> models[2];
    std::vector<ComponentFile> modelTextures[2]; // ModelMaterialResourcesID[slot] / ModelMatRes
    std::vector<std::pair<uint32_t, ComponentFile>> sectionTextures; // ComponentSection, file
    std::vector<HelmetGeoset> helmetGeosets[2];
    std::set<uint32_t> files; // every FileDataID reached from this display
};

struct Item {
    uint32_t id = 0;
    std::string name;
    uint32_t classId = 0, subclassId = 0, material = 0, inventoryType = 0, quality = 0, itemLevel = 0;
    uint32_t requiredLevel = 0, bonding = 0, sellPrice = 0, buyPrice = 0, iconFileId = 0;
    int32_t allowableClass = -1, allowableRace = -1;
    bool known = false;
    uint32_t displayInfoId = 0;
    std::vector<uint32_t> variantDisplayIds;
};

struct FileEntry {
    uint32_t id = 0;
    FileKind kind = FileKind::Model;
    uint64_t bytes = 0;
    std::string sha256;
    std::string parent; // "m2:123" etc. for diagnostics
};

struct Manifest {
    std::string product, region, build, buildNumber;
    uint32_t locale = 0;
    std::vector<Item> items;
    std::map<uint32_t, Display> displays;
    std::vector<FileEntry> files;
    std::vector<std::string> warnings;
    std::vector<std::string> missing; // "kind:id: reason"
};

// ---- plain rows ----------------------------------------------------------------------------
struct Db {
    struct ItemRow { uint32_t classId, subclassId, material, inventoryType, icon; };
    struct SparseRow { std::string name; uint32_t quality, itemLevel, requiredLevel, bonding, sellPrice, buyPrice;
                       uint32_t allowableRace, allowableClass, inventoryType; uint32_t expansion = 0; };
    struct Modified { uint32_t id, itemId, modifier, appearanceId, order; };
    struct Appearance { uint32_t displayInfoId, defaultIcon; };
    struct DisplayRow { uint32_t flags, itemVisual, particleColor, geosetOverride; uint32_t modelRes[2], matRes[2], modelType[2];
                        uint32_t geoset[6], attach[6], helmet[2]; };
    struct TextureRow { uint32_t fileId, usage; };
    struct ComponentModel { int32_t gender, classId, race, position; };
    struct ComponentTexture { int32_t gender, classId, race; };
    struct DisplayMaterial { uint32_t section, materialResources; };
    struct DisplayModelMaterial { uint32_t materialResources, textureType, modelIndex; };

    std::unordered_map<uint32_t, ItemRow> items;
    std::unordered_map<uint32_t, SparseRow> sparse;
    std::unordered_map<uint32_t, Modified> modified;                    // by IMA id
    std::unordered_map<uint32_t, std::vector<uint32_t>> modifiedByItem; // item -> IMA ids
    std::unordered_map<uint32_t, Appearance> appearances;
    std::unordered_map<uint32_t, DisplayRow> displays;
    std::unordered_map<uint32_t, std::vector<DisplayMaterial>> displayMaterials;           // by display
    std::unordered_map<uint32_t, std::vector<DisplayModelMaterial>> displayModelMaterials; // by display
    std::unordered_map<uint32_t, std::vector<uint32_t>> modelFilesByResources;
    std::unordered_map<uint32_t, std::vector<TextureRow>> texturesByMaterial;
    std::unordered_map<uint32_t, ComponentModel> componentModels;     // by model FileDataID
    std::unordered_map<uint32_t, ComponentTexture> componentTextures; // by texture FileDataID
    std::unordered_map<uint32_t, std::vector<HelmetGeoset>> helmetGeosets; // by HelmetGeosetVisDataID
    std::unordered_map<uint32_t, std::vector<uint32_t>> setItems;          // set -> IMA ids
    std::unordered_map<uint32_t, std::string> setNames;
    std::unordered_map<uint32_t, uint32_t> setExpansion;
};

struct PrefetchItem { uint32_t fileId; FileKind kind; };

class Fetcher {
public:
    virtual ~Fetcher() = default;
    // Original CDN bytes for a FileDataID. Implementations should leave them in the durable cache.
    virtual bool Get(uint32_t fileId, FileKind kind, std::vector<uint8_t>& bytes, std::string& error) = 0;
    // Bring many files into the durable cache at once (parallel, retried). Failures are not reported
    // here: a later Get of the same file reports them. Default: nothing to do.
    virtual void Prefetch(const std::vector<PrefetchItem>&) {}
    // True when Get would be served locally (packed archive or durable cache) without the network.
    virtual bool Cached(uint32_t, FileKind) { return false; }
};

// Append-only checkpoint of resolved files (JSON lines). A rerun skips every file that is recorded
// and still cached, without hashing or parsing it again, so an interrupted pull resumes at once.
class FetchJournal {
public:
    struct Child { uint32_t id; FileKind kind; };
    struct Record { uint64_t bytes = 0; std::string sha256; std::vector<Child> children; };
    // Loads an existing journal (tolerating a torn last line) and opens it for appending.
    bool Open(const std::string& path, std::string& error);
    const Record* Find(uint32_t id, FileKind kind) const;
    void Done(uint32_t id, FileKind kind, const Record& record);
    void Failed(uint32_t id, FileKind kind, const std::string& error);
    size_t Loaded() const { return m_records.size(); }
    size_t Failures() const { return m_failures; }
    ~FetchJournal();
private:
    std::unordered_map<uint64_t, Record> m_records;
    size_t m_failures = 0;
    void* m_file = nullptr;
};

// ---- bulk catalogue selection ----------------------------------------------------------------
struct CatalogOptions {
    bool sets = false, armor = false, weapons = false;
    bool everyItem = false;        // default: one representative item per ItemAppearance
    bool skipUncollectible = false;
    int64_t minExpansion = -1, maxExpansion = -1;
};
// Adds every matching set / item to the request. Returns counts through the out parameters.
void SelectCatalog(const Db& db, const CatalogOptions& options, Request& request, size_t* sets = nullptr, size_t* items = nullptr);
bool IsArmorSlot(uint32_t classId, uint32_t subclassId, uint32_t inventoryType);
bool IsWeaponSlot(uint32_t classId, uint32_t subclassId, uint32_t inventoryType);

// ---- M2 chunk dependencies (exposed for tests) ------------------------------------------
struct ModelFiles {
    std::vector<uint32_t> skins, textures, bones, particleModels;
    std::vector<std::pair<uint32_t, uint32_t>> animations; // (animation id << 16 | variation, FileDataID)
    uint32_t physics = 0, skeleton = 0;
    bool legacy = false; // MD20 container: no FileDataID chunks
};
bool ParseModelFiles(const std::vector<uint8_t>& bytes, ModelFiles& out, std::string& error);

std::string Sha256Hex(const uint8_t* data, size_t size);

class Resolver {
public:
    Resolver(const Db& db, Fetcher& fetcher) : m_db(db), m_fetcher(fetcher) {}
    // `download=false` records the graph without touching file bytes (used for --graph-only).
    void SetDownload(bool value) { m_download = value; }
    void SetJournal(FetchJournal* journal) { m_journal = journal; }
    bool Run(const Request& request, Manifest& out, std::string& error);

private:
    struct Pending { uint32_t id; FileKind kind; std::string parent; };
    using Key = std::pair<uint32_t, int>;
    const Db& m_db;
    Fetcher& m_fetcher;
    FetchJournal* m_journal = nullptr;
    bool m_download = true;
    std::set<Key> m_seen;
    std::vector<Pending> m_pending;
    std::map<Key, std::vector<Key>> m_children;
    std::map<Key, std::vector<Display*>> m_owners;
    std::set<uint32_t> m_displaysDone;
    void AddItem(uint32_t itemId, Manifest& out);
    void AddDisplay(uint32_t displayId, Manifest& out);
    void AddFile(uint32_t id, FileKind kind, const std::string& parent, Display* display);
    void Link(Display* display, const Key& key);
    void Drain(Manifest& out);
    void Process(const Pending& entry, Manifest& out);
    void Children(const Pending& entry, const ModelFiles& files, std::vector<FetchJournal::Child>& list);
    ComponentFile ModelFile(uint32_t fileId) const;
    ComponentFile TextureFile(uint32_t fileId, uint32_t usage) const;
};

// ---- storage -----------------------------------------------------------------------------
// Loads every table from the configured CDN build. Slow on a cold cache, then instant.
bool LoadDb(Db& db, std::string& error);
// The two halves of LoadDb. The display tables are all the client needs at runtime to pick armor
// variants; the item tables (ItemSparse is 60 MB) are for tools only.
bool LoadItemTables(Db& db, std::string& error);
bool LoadDisplayTables(Db& db, std::string& error);
// Fetch (packed -> cache -> CDN) and decode one of the tables above by lowercase name.
bool LoadNamedTable(const char* name, Db2::Table& table, std::string& error);
struct TableRef { const char* name; uint32_t fileDataId; };
const std::vector<TableRef>& DisplayTables(); // FileDataIDs of the display tables, for packing

// Fetcher backed by the process CascSource: original CDN bytes, cached durably as a side effect.
class CascFetcher : public Fetcher {
public:
    bool Get(uint32_t fileId, FileKind kind, std::vector<uint8_t>& bytes, std::string& error) override;
};

std::string ToJson(const Manifest& manifest);

}
#endif
