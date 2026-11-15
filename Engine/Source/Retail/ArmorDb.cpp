#include "Engine/Source/Retail/ArmorDb.h"
#include "Engine/Source/Retail/ArmorPath.h"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <iterator>

#if defined(WOWDECOMP_RETAIL_ASSETS) && defined(WOWDECOMP_RETAIL_ARMOR_PHYSICS)
#include "Engine/Source/Retail/Db2.h"
#include "Engine/Source/Retail/DependencyResolver.h"
#include "Engine/Source/Retail/SourceContext.h"
#endif

namespace Retail::Armor {

// ---- Compact ----------------------------------------------------------------------------------
void Compact::AddModelFile(uint32_t fileId, uint32_t resourcesId) { m_modelByFile.push_back({fileId, resourcesId}); }
void Compact::AddTextureFile(uint32_t fileId, uint32_t materialId, uint32_t usage) { m_textureByFile.push_back({fileId, materialId, usage}); }
void Compact::AddComponentModel(uint32_t f, int32_t g, int32_t c, int32_t r, int32_t p) { m_modelComponents.push_back({f, g, c, r, p}); }
void Compact::AddComponentTexture(uint32_t f, int32_t g, int32_t c, int32_t r) { m_textureComponents.push_back({f, g, c, r}); }
void Compact::AddDisplay(const DisplayRow& row) { m_displays.push_back(row); }
void Compact::AddDisplayMaterial(uint32_t d, uint32_t s, uint32_t m) { m_displayMaterials.push_back({d, s, m}); }
void Compact::AddHelmetGeoset(uint32_t v, uint32_t r, uint32_t h, uint32_t b) { m_helmets.push_back({v, r, h, b}); }

void Compact::Finalize() {
    auto byFile = [](const auto& a, const auto& b) { return a.file < b.file; };
    std::sort(m_modelByFile.begin(), m_modelByFile.end(), byFile);
    m_modelByResources = m_modelByFile;
    std::sort(m_modelByResources.begin(), m_modelByResources.end(), [](const ModelFile& a, const ModelFile& b) {
        return a.resources != b.resources ? a.resources < b.resources : a.file < b.file; });
    std::sort(m_textureByFile.begin(), m_textureByFile.end(), byFile);
    m_textureByMaterial = m_textureByFile;
    std::sort(m_textureByMaterial.begin(), m_textureByMaterial.end(), [](const TextureFile& a, const TextureFile& b) {
        return a.material != b.material ? a.material < b.material : a.file < b.file; });
    std::sort(m_modelComponents.begin(), m_modelComponents.end(), byFile);
    std::sort(m_textureComponents.begin(), m_textureComponents.end(), byFile);
    std::sort(m_displays.begin(), m_displays.end(), [](const DisplayRow& a, const DisplayRow& b) { return a.id < b.id; });
    std::sort(m_displayMaterials.begin(), m_displayMaterials.end(), [](const DisplayMaterial& a, const DisplayMaterial& b) {
        return a.display != b.display ? a.display < b.display : a.section != b.section ? a.section < b.section : a.material < b.material; });
    std::sort(m_helmets.begin(), m_helmets.end(), [](const Helmet& a, const Helmet& b) {
        return a.vis != b.vis ? a.vis < b.vis : a.race != b.race ? a.race < b.race : a.hide < b.hide; });
    for (auto* v : {&m_modelByFile, &m_modelByResources}) v->shrink_to_fit();
    m_textureByFile.shrink_to_fit(); m_textureByMaterial.shrink_to_fit();
    m_modelComponents.shrink_to_fit(); m_textureComponents.shrink_to_fit();
    m_displays.shrink_to_fit(); m_displayMaterials.shrink_to_fit(); m_helmets.shrink_to_fit();
}

size_t Compact::ApproximateBytes() const {
    return (m_modelByFile.capacity() + m_modelByResources.capacity()) * sizeof(ModelFile) +
           (m_textureByFile.capacity() + m_textureByMaterial.capacity()) * sizeof(TextureFile) +
           m_modelComponents.capacity() * sizeof(ModelComponent) + m_textureComponents.capacity() * sizeof(TextureComponent) +
           m_displays.capacity() * sizeof(DisplayRow) + m_displayMaterials.capacity() * sizeof(DisplayMaterial) +
           m_helmets.capacity() * sizeof(Helmet);
}

namespace {
constexpr int kExcluded = -1000;

// gender: 0 male, 1 female, 2 or negative = either. race: 0 = any. class: <= 0 = any.
int Fit(int32_t gender, int32_t classId, int32_t race, const Character& who) {
    int score = 0;
    if (gender < 0 || gender == 2) score += 1;
    else if (gender == who.gender) score += 2;
    else return kExcluded;
    if (race <= 0) score += 1;
    else if (uint32_t(race) == who.retailRace && who.retailRace) score += 8;
    else if (uint32_t(race) == who.donorRace && who.donorRace) score += 4;
    else return kExcluded;
    if (classId > 0) {
        if (who.classId && uint32_t(classId) == who.classId) score += 1;
        else if (who.classId) return kExcluded;
    }
    return score;
}

template <class Rows, class Key>
const typename Rows::value_type* FindFile(const Rows& rows, Key file) {
    const auto it = std::lower_bound(rows.begin(), rows.end(), file, [](const auto& row, Key key) { return row.file < key; });
    return it != rows.end() && it->file == file ? &*it : nullptr;
}
}

namespace {
struct Pick {
    uint32_t file;
    int score;
    bool requested;
};
// Higher score wins; on a tie the requested file stays, then the lowest FileDataID.
bool Better(const Pick& a, const Pick& b) {
    if (a.score != b.score) return a.score > b.score;
    if (a.requested != b.requested) return a.requested;
    return a.file < b.file;
}
}

uint32_t Compact::SelectModel(uint32_t requested, const Character& who) const {
    const ModelFile* self = FindFile(m_modelByFile, requested);
    if (!self || !self->resources) return requested;
    auto it = std::lower_bound(m_modelByResources.begin(), m_modelByResources.end(), self->resources,
        [](const ModelFile& row, uint32_t key) { return row.resources < key; });
    Pick best{requested, kExcluded, true};
    bool any = false;
    for (; it != m_modelByResources.end() && it->resources == self->resources; ++it) {
        const ModelComponent* component = FindFile(m_modelComponents, it->file);
        const Pick pick{it->file, component ? Fit(component->gender, component->classId, component->race, who) : 0, it->file == requested};
        if (pick.score <= kExcluded) continue;
        if (!any || Better(pick, best)) { best = pick; any = true; }
    }
    return any ? best.file : requested;
}

uint32_t Compact::SelectTexture(uint32_t requested, const Character& who) const {
    const TextureFile* self = FindFile(m_textureByFile, requested);
    if (!self || !self->material) return requested;
    auto it = std::lower_bound(m_textureByMaterial.begin(), m_textureByMaterial.end(), self->material,
        [](const TextureFile& row, uint32_t key) { return row.material < key; });
    Pick best{requested, kExcluded, true};
    bool any = false;
    for (; it != m_textureByMaterial.end() && it->material == self->material; ++it) {
        if (it->usage != self->usage) continue;
        const TextureComponent* component = FindFile(m_textureComponents, it->file);
        const Pick pick{it->file, component ? Fit(component->gender, component->classId, component->race, who) : 0, it->file == requested};
        if (pick.score <= kExcluded) continue;
        if (!any || Better(pick, best)) { best = pick; any = true; }
    }
    return any ? best.file : requested;
}

const DisplayRow* Compact::Display(uint32_t displayId) const {
    const auto it = std::lower_bound(m_displays.begin(), m_displays.end(), displayId,
        [](const DisplayRow& row, uint32_t key) { return row.id < key; });
    return it != m_displays.end() && it->id == displayId ? &*it : nullptr;
}

std::vector<Compact::SectionTexture> Compact::SectionTextures(uint32_t displayId, const Character& who) const {
    std::vector<SectionTexture> result;
    auto it = std::lower_bound(m_displayMaterials.begin(), m_displayMaterials.end(), displayId,
        [](const DisplayMaterial& row, uint32_t key) { return row.display < key; });
    for (; it != m_displayMaterials.end() && it->display == displayId; ++it) {
        const auto first = std::lower_bound(m_textureByMaterial.begin(), m_textureByMaterial.end(), it->material,
            [](const TextureFile& row, uint32_t key) { return row.material < key; });
        // One texture per usage type: the best fit among that usage's variants.
        for (auto t = first; t != m_textureByMaterial.end() && t->material == it->material; ++t) {
            if (t != first && std::prev(t)->usage == t->usage && std::prev(t)->material == t->material) {
                bool seen = false;
                for (const auto& done : result) seen |= done.section == it->section && done.usage == t->usage;
                if (seen) continue;
            }
            result.push_back({it->section, SelectTexture(t->file, who), t->usage});
        }
    }
    return result;
}

std::vector<uint32_t> Compact::HelmetHiddenGroups(uint32_t visId, const Character& who) const {
    std::vector<uint32_t> exact, donor, any;
    auto it = std::lower_bound(m_helmets.begin(), m_helmets.end(), visId, [](const Helmet& row, uint32_t key) { return row.vis < key; });
    for (; it != m_helmets.end() && it->vis == visId; ++it) {
        if (who.retailRace && it->race == who.retailRace) exact.push_back(it->hide);
        else if (who.donorRace && it->race == who.donorRace) donor.push_back(it->hide);
        else if (!it->race) any.push_back(it->hide);
    }
    if (!exact.empty()) return exact;
    if (!donor.empty()) return donor;
    return any;
}

// ---- Registry -------------------------------------------------------------------------------
Registry& Registry::Get() {
    static Registry registry;
    return registry;
}

void Registry::SetForTest(Compact variants, Compact displays) {
    std::lock_guard<std::mutex> lock(m_mutex);
    m_variantData = std::move(variants);
    m_displayData = std::move(displays);
    m_variants = m_displays = State::Loaded;
}

void Registry::ResetForTest() {
    std::lock_guard<std::mutex> lock(m_mutex);
    m_variantData = Compact();
    m_displayData = Compact();
    m_variants = m_displays = State::Unloaded;
    m_variantError.clear();
    m_displayError.clear();
}

#if defined(WOWDECOMP_RETAIL_ASSETS) && defined(WOWDECOMP_RETAIL_ARMOR_PHYSICS)
namespace {
// Stream a table into `fill`, then let the decoded rows go; only the compact arrays survive.
template <class Fill>
bool Stream(const char* name, std::string& error, Fill fill) {
    Db2::Table table;
    if (!Resolve::LoadNamedTable(name, table, error)) return false;
    for (const auto& row : table.Rows()) fill(table, row);
    return true;
}
}

bool Registry::EnsureVariants(std::string* error) {
    std::lock_guard<std::mutex> lock(m_mutex);
    if (m_variants == State::Failed) { if (error) *error = m_variantError; return false; }
    if (m_variants == State::Loaded) return true;
    SourceScope scope(SourceKind::Retail);
    Compact data;
    std::string failure;
    const bool ok =
        Stream("modelfiledata", failure, [&](const Db2::Table& t, const Db2::wdc5::Row& r) { data.AddModelFile(r.id, t.Value(r, "ModelResourcesID")); }) &&
        Stream("texturefiledata", failure, [&](const Db2::Table& t, const Db2::wdc5::Row& r) { data.AddTextureFile(r.id, t.Value(r, "MaterialResourcesID"), t.Value(r, "UsageType")); }) &&
        Stream("componentmodelfiledata", failure, [&](const Db2::Table& t, const Db2::wdc5::Row& r) {
            data.AddComponentModel(r.id, int32_t(t.Value(r, "GenderIndex")), int32_t(t.Value(r, "ClassID")), int32_t(t.Value(r, "RaceID")), int32_t(t.Value(r, "PositionIndex"))); }) &&
        Stream("componenttexturefiledata", failure, [&](const Db2::Table& t, const Db2::wdc5::Row& r) {
            data.AddComponentTexture(r.id, int32_t(t.Value(r, "GenderIndex")), int32_t(t.Value(r, "ClassID")), int32_t(t.Value(r, "RaceID"))); });
    if (!ok) {
        m_variants = State::Failed;
        m_variantError = failure;
        std::fprintf(stderr, "retail armor: variant tables unavailable (%s); Retail armor uses the files its DBC row names\n", failure.c_str());
        if (error) *error = failure;
        return false;
    }
    data.Finalize();
    std::fprintf(stderr, "retail armor: variant index ready (%zu models, %zu textures, ~%zu KiB)\n",
                 data.ModelFileCount(), data.TextureFileCount(), data.ApproximateBytes() / 1024);
    m_variantData = std::move(data);
    m_variants = State::Loaded;
    return true;
}

bool Registry::EnsureDisplays(std::string* error) {
    std::lock_guard<std::mutex> lock(m_mutex);
    if (m_displays == State::Failed) { if (error) *error = m_displayError; return false; }
    if (m_displays == State::Loaded) return true;
    SourceScope scope(SourceKind::Retail);
    Compact data;
    std::string failure;
    const bool ok =
        Stream("itemdisplayinfo", failure, [&](const Db2::Table& t, const Db2::wdc5::Row& r) {
            DisplayRow d;
            d.id = r.id; d.flags = t.Value(r, "Flags"); d.itemVisual = t.Value(r, "ItemVisual"); d.geosetOverride = t.Value(r, "GeosetGroupOverride");
            for (size_t i = 0; i < 2; ++i) {
                d.modelRes[i] = t.Value(r, "ModelResourcesID", i); d.matRes[i] = t.Value(r, "ModelMaterialResourcesID", i);
                d.modelType[i] = t.Value(r, "ModelType", i); d.helmet[i] = t.Value(r, "HelmetGeosetVis", i);
            }
            for (size_t i = 0; i < 6; ++i) { d.geoset[i] = t.Value(r, "GeosetGroup", i); d.attach[i] = t.Value(r, "AttachmentGeosetGroup", i); }
            data.AddDisplay(d); }) &&
        Stream("itemdisplayinfomaterialres", failure, [&](const Db2::Table& t, const Db2::wdc5::Row& r) {
            data.AddDisplayMaterial(r.parentId, t.Value(r, "ComponentSection"), t.Value(r, "MaterialResourcesID")); }) &&
        Stream("helmetgeosetdata", failure, [&](const Db2::Table& t, const Db2::wdc5::Row& r) {
            data.AddHelmetGeoset(r.parentId, t.Value(r, "RaceID"), t.Value(r, "HideGeosetGroup"), t.Value(r, "RaceBitSelection")); });
    if (!ok) {
        m_displays = State::Failed;
        m_displayError = failure;
        std::fprintf(stderr, "retail armor: display tables unavailable (%s)\n", failure.c_str());
        if (error) *error = failure;
        return false;
    }
    data.Finalize();
    std::fprintf(stderr, "retail armor: display index ready (%zu displays, ~%zu KiB)\n", data.DisplayCount(), data.ApproximateBytes() / 1024);
    m_displayData = std::move(data);
    m_displays = State::Loaded;
    return true;
}
#else
bool Registry::EnsureVariants(std::string* error) { if (error) *error = "Retail armor support is not compiled in"; return false; }
bool Registry::EnsureDisplays(std::string* error) { if (error) *error = "Retail armor support is not compiled in"; return false; }
#endif

namespace {
bool EndsWith(const std::string& text, const char* suffix) {
    const size_t n = std::strlen(suffix);
    if (text.size() < n) return false;
    for (size_t i = 0; i < n; ++i) {
        char c = text[text.size() - n + i];
        if (c >= 'A' && c <= 'Z') c = char(c - 'A' + 'a');
        if (c != suffix[i]) return false;
    }
    return true;
}
}

bool ResolveModelName(const char* name, const Character& who, std::string& out) {
    uint32_t id = 0;
    std::string extension;
    if (!ParseRetailName(name, &id, &extension) || !EndsWith(extension, ".m2")) return false;
    Registry& registry = Registry::Get();
    if (!registry.EnsureVariants()) return false;
    out = FileName(registry.Variants().SelectModel(id, who), ".m2");
    return true;
}

bool ResolveTextureName(const char* name, const Character& who, std::string& out) {
    uint32_t id = 0;
    std::string extension;
    if (!ParseRetailName(name, &id, &extension) || !EndsWith(extension, ".blp")) return false;
    Registry& registry = Registry::Get();
    if (!registry.EnsureVariants()) return false;
    out = FileName(registry.Variants().SelectTexture(id, who), ".blp");
    return true;
}

}
