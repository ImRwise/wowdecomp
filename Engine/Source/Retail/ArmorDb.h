#ifndef ENGINE_SOURCE_RETAIL_ARMORDB_H
#define ENGINE_SOURCE_RETAIL_ARMORDB_H

// Runtime Retail armor metadata, built for a client that may know tens of thousands of items:
//
//  * Nothing loads at startup. Variant tables (ModelFileData, TextureFileData, ComponentModelFileData,
//    ComponentTextureFileData) load the first time a Retail armor path is resolved; display tables
//    (ItemDisplayInfo, ItemDisplayInfoMaterialRes / ModelMatRes, HelmetGeosetData) load the first time
//    a display is queried. Each table is decoded, folded into sorted flat arrays (8-16 bytes a row,
//    binary search by FileDataID / DisplayID, no per-row allocation) and the decoded table is freed.
//  * No M2/BLP is read here: files stay on disk (packed EsteriaRetail\ -> cache -> CDN) until the
//    model loader asks for the one it needs.
//
// A DBC row only names one file ("retail/<id>.m2"). The tables tell us its siblings (the other
// race/gender variants of the same part), and SelectModel / SelectTexture pick the right one for
// the character being built.

#include <cstdint>
#include <mutex>
#include <string>
#include <vector>

namespace Retail::Armor {

struct Character {
    int32_t gender = 0;       // 0 male, 1 female (the client's sex ID)
    uint32_t retailRace = 0;  // Retail ChrRaces ID (RetailRaces.csv); 0 = unknown
    uint32_t donorRace = 0;   // anatomy family used when a part has no exact row (Retail::HelmetMaskRace)
    uint32_t classId = 0;     // 0 = ignore class
};

struct DisplayRow {
    uint32_t id = 0, flags = 0, itemVisual = 0, geosetOverride = 0;
    uint32_t modelRes[2] = {}, matRes[2] = {}, modelType[2] = {};
    uint32_t geoset[6] = {}, attach[6] = {}, helmet[2] = {};
};

// Sorted flat indexes. Build with the Add* calls, then Finalize(); all queries are const.
class Compact {
public:
    void AddModelFile(uint32_t fileId, uint32_t resourcesId);
    void AddTextureFile(uint32_t fileId, uint32_t materialId, uint32_t usage);
    void AddComponentModel(uint32_t fileId, int32_t gender, int32_t classId, int32_t race, int32_t position);
    void AddComponentTexture(uint32_t fileId, int32_t gender, int32_t classId, int32_t race);
    void AddDisplay(const DisplayRow& row);
    void AddDisplayMaterial(uint32_t displayId, uint32_t section, uint32_t materialId);
    void AddHelmetGeoset(uint32_t visId, uint32_t raceId, uint32_t hideGroup, uint32_t raceBit);
    void Finalize();

    // Pick the best-fitting file among `requested` and its siblings. Returns `requested` when the
    // tables know nothing about it, so unknown or custom files keep working.
    uint32_t SelectModel(uint32_t requested, const Character& who) const;
    uint32_t SelectTexture(uint32_t requested, const Character& who) const;

    const DisplayRow* Display(uint32_t displayId) const;
    // Texture files a display layers onto the character (ItemDisplayInfoMaterialRes), by section.
    struct SectionTexture { uint32_t section; uint32_t fileId; uint32_t usage; };
    std::vector<SectionTexture> SectionTextures(uint32_t displayId, const Character& who) const;
    // Geoset groups a helmet vis row hides for this race (empty when none apply).
    std::vector<uint32_t> HelmetHiddenGroups(uint32_t visId, const Character& who) const;

    size_t ModelFileCount() const { return m_modelByFile.size(); }
    size_t TextureFileCount() const { return m_textureByFile.size(); }
    size_t DisplayCount() const { return m_displays.size(); }
    size_t ApproximateBytes() const;

private:
    struct ModelFile { uint32_t file, resources; };
    struct TextureFile { uint32_t file, material, usage; };
    struct ModelComponent { uint32_t file; int32_t gender, classId, race, position; };
    struct TextureComponent { uint32_t file; int32_t gender, classId, race; };
    struct DisplayMaterial { uint32_t display, section, material; };
    struct Helmet { uint32_t vis, race, hide, raceBit; };
    std::vector<ModelFile> m_modelByFile, m_modelByResources;
    std::vector<TextureFile> m_textureByFile, m_textureByMaterial;
    std::vector<ModelComponent> m_modelComponents;
    std::vector<TextureComponent> m_textureComponents;
    std::vector<DisplayRow> m_displays;
    std::vector<DisplayMaterial> m_displayMaterials;
    std::vector<Helmet> m_helmets;
};

// ---- process-wide lazy registry -----------------------------------------------------------
class Registry {
public:
    static Registry& Get();
    // First call loads from the Retail source (packed tables -> durable cache -> CDN). A failure is
    // remembered (and logged once) so a client with no CDN access does not retry on every equip.
    bool EnsureVariants(std::string* error = nullptr);
    bool EnsureDisplays(std::string* error = nullptr);
    const Compact& Variants() const { return m_variantData; }
    const Compact& Displays() const { return m_displayData; }
    // Test hooks.
    void SetForTest(Compact variants, Compact displays);
    void ResetForTest();

private:
    enum class State { Unloaded, Loaded, Failed };
    std::mutex m_mutex;
    State m_variants = State::Unloaded, m_displays = State::Unloaded;
    std::string m_variantError, m_displayError;
    Compact m_variantData, m_displayData;
};

// "retail/<id>.m2|.blp" in, the variant for `who` out. Returns false (out unchanged) when `name`
// is not a Retail name or the tables are unavailable; callers then use `name` as written.
bool ResolveModelName(const char* name, const Character& who, std::string& out);
bool ResolveTextureName(const char* name, const Character& who, std::string& out);

}
#endif
