// CDN-backed storage (DB2 tables, original files) and JSON output for the dependency resolver.
#include "Engine/Source/Retail/CascSource.h"
#include "Engine/Source/Retail/Db2.h"
#include "Engine/Source/Retail/DependencyResolver.h"
#include "Engine/Source/Retail/SourceContext.h"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <set>
#include <tuple>

namespace Retail::Resolve {
namespace {
using Db2::Definition;
using Db2::Field;

// Physical (file-order) fields, from WoWDBDefs for the 12.x layouts. The layout hash is left at 0
// (not enforced): the physical field count and each pallet cardinality are still validated on load.
constexpr Field kItem[] = {{"ClassID"}, {"SubclassID"}, {"Material"}, {"InventoryType"}, {"SheatheType"},
    {"SoundOverrideSubclassID"}, {"IconFileDataID"}, {"ItemGroupSoundsID"}, {"ContentTuningID"},
    {"ModifiedCraftingReagentItemID"}, {"Field_12_0_0_63534_010"}, {"CraftingQualityID"}, {"ItemSquishEraID"},
    {"RecraftReagentCountPercentage"}, {"OrderSource"}};
constexpr Field kItemSparse[] = {{"Description_lang", 1, true}, {"Display3_lang", 1, true}, {"Display2_lang", 1, true},
    {"Display1_lang", 1, true}, {"Display_lang", 1, true}, {"ExpansionID"}, {"DmgVariance"}, {"LimitCategory"},
    {"DurationInInventory"}, {"QualityModifier"}, {"BagFamily"}, {"StartQuestID"}, {"LanguageID"}, {"ItemRange"},
    {"StatPercentageOfSocket", 10}, {"StatPercentEditor", 10}, {"StatModifier_bonusStat", 10}, {"Stackable"},
    {"MaxCount"}, {"MinReputation"}, {"RequiredAbility"}, {"AllowableRaces", 2}, {"SellPrice"}, {"BuyPrice"},
    {"VendorStackCount"}, {"PriceVariance"}, {"PriceRandomValue"}, {"Flags", 5}, {"OppositeFactionItemID"},
    {"ModifiedCraftingReagentItemID"}, {"ContentTuningID"}, {"PlayerLevelToItemLevelCurveID"},
    {"ItemLevelOffsetCurveID"}, {"ItemLevelOffsetItemLevel"}, {"ItemSquishEraID"}, {"ItemNameDescriptionID"},
    {"RequiredTransmogHoliday"}, {"RequiredHoliday"}, {"GemProperties"}, {"SocketMatchEnchantmentID"},
    {"TotemCategoryID"}, {"InstanceBound"}, {"ZoneBound", 2}, {"ItemSet"}, {"LockID"}, {"PageID"}, {"ItemDelay"},
    {"MinFactionID"}, {"RequiredSkillRank"}, {"RequiredSkill"}, {"ItemLevel"}, {"AllowableClass"}, {"ArtifactID"},
    {"SpellWeight"}, {"SpellWeightCategory"}, {"SocketType", 3}, {"SheatheType"}, {"Material"}, {"PageMaterialID"},
    {"Bonding"}, {"DamageType"}, {"ContainerSlots"}, {"RequiredPVPMedal"}, {"RequiredPVPRank"}, {"RequiredLevel"},
    {"InventoryType"}, {"OverallQualityID"}};
constexpr Field kItemAppearance[] = {{"DisplayType"}, {"ItemDisplayInfoID"}, {"DefaultIconFileDataID"}, {"UiOrder"},
    {"TransmogPlayerConditionID"}};
constexpr Field kItemModifiedAppearance[] = {{"ID"}, {"ItemID"}, {"ItemAppearanceModifierID"}, {"ItemAppearanceID"},
    {"OrderIndex"}, {"TransmogSourceTypeEnum"}, {"Flags"}};
constexpr Field kItemDisplayInfo[] = {{"GeosetGroupOverride"}, {"ItemVisual"}, {"ParticleColorID"},
    {"ItemRangedDisplayInfoID"}, {"OverrideSwooshSoundKitID"}, {"SheatheTransformMatrixID"}, {"StateSpellVisualKitID"},
    {"SheathedSpellVisualKitID"}, {"UnsheathedSpellVisualKitID"}, {"Flags"}, {"ModelResourcesID", 2},
    {"ModelMaterialResourcesID", 2}, {"ModelType", 2}, {"GeosetGroup", 6}, {"AttachmentGeosetGroup", 6},
    {"HelmetGeosetVis", 2}};
constexpr Field kMaterialRes[] = {{"ComponentSection"}, {"MaterialResourcesID"}};
constexpr Field kModelMatRes[] = {{"MaterialResourcesID"}, {"TextureType"}, {"ModelIndex"}};
constexpr Field kComponentModel[] = {{"GenderIndex"}, {"ClassID"}, {"RaceID"}, {"PositionIndex"}};
constexpr Field kComponentTexture[] = {{"GenderIndex"}, {"ClassID"}, {"RaceID"}};
constexpr Field kHelmetGeoset[] = {{"RaceID"}, {"HideGeosetGroup"}, {"RaceBitSelection"}, {"Field_10_0_0_46047_003"}};
constexpr Field kModelFileData[] = {{"GeoBox", 6}, {"FileDataID"}, {"Flags"}, {"LodCount"}, {"ModelResourcesID"}};
constexpr Field kTextureFileData[] = {{"FileDataID"}, {"UsageType"}, {"MaterialResourcesID"}};
constexpr Field kTransmogSet[] = {{"Name_lang", 1, true}, {"ID"}, {"ClassMask"}, {"TrackingQuestID"}, {"Flags"},
    {"TransmogSetGroupID"}, {"ItemNameDescriptionID"}, {"ParentTransmogSetID"}, {"CompleteWorldStateID"},
    {"ExpansionID"}, {"PatchIntroduced"}, {"UiOrder"}, {"ConditionID"}};
constexpr Field kTransmogSetItem[] = {{"TransmogSetID"}, {"ItemModifiedAppearanceID"}, {"Flags"}};

struct TableSpec { const char* name; uint32_t fileDataId; Db2::Span<const Field> fields; };
const TableSpec kSpecs[] = {
    {"item", 841626, kItem}, {"itemsparse", 1572924, kItemSparse}, {"itemappearance", 982462, kItemAppearance},
    {"itemmodifiedappearance", 982457, kItemModifiedAppearance}, {"itemdisplayinfo", 1266429, kItemDisplayInfo},
    {"itemdisplayinfomaterialres", 1280614, kMaterialRes}, {"itemdisplayinfomodelmatres", 4050937, kModelMatRes},
    {"componentmodelfiledata", 1349053, kComponentModel}, {"componenttexturefiledata", 1278239, kComponentTexture},
    {"helmetgeosetdata", 2821752, kHelmetGeoset}, {"modelfiledata", 1337833, kModelFileData},
    {"texturefiledata", 982459, kTextureFileData}, {"transmogset", 1376213, kTransmogSet},
    {"transmogsetitem", 1376212, kTransmogSetItem},
};

bool Load(const char* name, Db2::Table& table, std::string& error) {
    for (const TableSpec& spec : kSpecs) {
        if (std::string(spec.name) != name) continue;
        std::vector<uint8_t> bytes;
        if (!Source().ReadTable((std::string(SourcePrefix()) + std::to_string(spec.fileDataId) + ".db2").c_str(), bytes, error)) {
            error = std::string(name) + ": " + error;
            return false;
        }
        const Definition definition{spec.name, spec.name, 0, spec.fields};
        std::string detail;
        if (!table.LoadBytes(definition, bytes, &detail)) { error = std::string(name) + ": " + detail; return false; }
        return true;
    }
    error = std::string("unknown table ") + name;
    return false;
}
}

bool LoadNamedTable(const char* name, Db2::Table& table, std::string& error) { return Load(name, table, error); }

bool LoadItemTables(Db& db, std::string& error) {
    Db2::Table t;
    auto v = [&](const Db2::wdc5::Row& row, std::string_view field, size_t element = 0) { return t.Value(row, field, element); };

    if (!Load("item", t, error)) return false;
    for (const auto& row : t.Rows())
        db.items[row.id] = {v(row, "ClassID"), v(row, "SubclassID"), v(row, "Material"), v(row, "InventoryType"), v(row, "IconFileDataID")};
    if (!Load("itemsparse", t, error)) return false;
    for (const auto& row : t.Rows())
        db.sparse[row.id] = {t.Text(row, "Display_lang"), v(row, "OverallQualityID"), v(row, "ItemLevel"),
            v(row, "RequiredLevel"), v(row, "Bonding"), v(row, "SellPrice"), v(row, "BuyPrice"), v(row, "AllowableRaces"),
            v(row, "AllowableClass"), v(row, "InventoryType"), v(row, "ExpansionID")};
    if (!Load("itemmodifiedappearance", t, error)) return false;
    for (const auto& row : t.Rows()) {
        db.modified[row.id] = {row.id, v(row, "ItemID"), v(row, "ItemAppearanceModifierID"), v(row, "ItemAppearanceID"), v(row, "OrderIndex")};
        db.modifiedByItem[v(row, "ItemID")].push_back(row.id);
    }
    if (!Load("itemappearance", t, error)) return false;
    for (const auto& row : t.Rows()) db.appearances[row.id] = {v(row, "ItemDisplayInfoID"), v(row, "DefaultIconFileDataID")};
    if (!Load("transmogsetitem", t, error)) return false;
    for (const auto& row : t.Rows()) db.setItems[v(row, "TransmogSetID")].push_back(v(row, "ItemModifiedAppearanceID"));
    if (!Load("transmogset", t, error)) return false;
    for (const auto& row : t.Rows()) {
        db.setNames[row.id] = t.Text(row, "Name_lang");
        db.setExpansion[row.id] = v(row, "ExpansionID");
    }
    return true;
}

bool LoadDisplayTables(Db& db, std::string& error) {
    Db2::Table t;
    auto v = [&](const Db2::wdc5::Row& row, std::string_view field, size_t element = 0) { return t.Value(row, field, element); };

    if (!Load("itemdisplayinfo", t, error)) return false;
    for (const auto& row : t.Rows()) {
        Db::DisplayRow d{};
        d.flags = v(row, "Flags"); d.itemVisual = v(row, "ItemVisual"); d.particleColor = v(row, "ParticleColorID");
        d.geosetOverride = v(row, "GeosetGroupOverride");
        for (size_t i = 0; i < 2; ++i) {
            d.modelRes[i] = v(row, "ModelResourcesID", i); d.matRes[i] = v(row, "ModelMaterialResourcesID", i);
            d.modelType[i] = v(row, "ModelType", i); d.helmet[i] = v(row, "HelmetGeosetVis", i);
        }
        for (size_t i = 0; i < 6; ++i) { d.geoset[i] = v(row, "GeosetGroup", i); d.attach[i] = v(row, "AttachmentGeosetGroup", i); }
        db.displays[row.id] = d;
    }
    if (!Load("itemdisplayinfomaterialres", t, error)) return false;
    for (const auto& row : t.Rows())
        db.displayMaterials[row.parentId].push_back({v(row, "ComponentSection"), v(row, "MaterialResourcesID")});
    if (!Load("itemdisplayinfomodelmatres", t, error)) return false;
    for (const auto& row : t.Rows())
        db.displayModelMaterials[row.parentId].push_back({v(row, "MaterialResourcesID"), v(row, "TextureType"), v(row, "ModelIndex")});
    if (!Load("modelfiledata", t, error)) return false;
    for (const auto& row : t.Rows()) db.modelFilesByResources[v(row, "ModelResourcesID")].push_back(row.id);
    if (!Load("texturefiledata", t, error)) return false;
    for (const auto& row : t.Rows()) db.texturesByMaterial[v(row, "MaterialResourcesID")].push_back({row.id, v(row, "UsageType")});
    if (!Load("componentmodelfiledata", t, error)) return false;
    for (const auto& row : t.Rows())
        db.componentModels[row.id] = {int32_t(v(row, "GenderIndex")), int32_t(v(row, "ClassID")), int32_t(v(row, "RaceID")), int32_t(v(row, "PositionIndex"))};
    if (!Load("componenttexturefiledata", t, error)) return false;
    for (const auto& row : t.Rows())
        db.componentTextures[row.id] = {int32_t(v(row, "GenderIndex")), int32_t(v(row, "ClassID")), int32_t(v(row, "RaceID"))};
    if (!Load("helmetgeosetdata", t, error)) return false;
    for (const auto& row : t.Rows())
        db.helmetGeosets[row.parentId].push_back({v(row, "RaceID"), v(row, "HideGeosetGroup"), v(row, "RaceBitSelection")});
    return true;
}

bool LoadDb(Db& db, std::string& error) { return LoadItemTables(db, error) && LoadDisplayTables(db, error); }

const std::vector<TableRef>& DisplayTables() {
    static const std::vector<TableRef> tables = [] {
        std::vector<TableRef> list;
        for (const TableSpec& spec : kSpecs)
            for (const char* name : {"itemdisplayinfo", "itemdisplayinfomaterialres", "itemdisplayinfomodelmatres", "modelfiledata", "texturefiledata", "componentmodelfiledata", "componenttexturefiledata", "helmetgeosetdata"})
                if (std::string(spec.name) == name) list.push_back({spec.name, spec.fileDataId});
        return list;
    }();
    return tables;
}

bool CascFetcher::Get(uint32_t fileId, FileKind kind, std::vector<uint8_t>& bytes, std::string& error) {
    return Source().Inspect((std::string(SourcePrefix()) + std::to_string(fileId) + KindExtension(kind)).c_str(), bytes, error);
}

// ---- JSON ----------------------------------------------------------------------------------
namespace {
std::string Esc(const std::string& text) {
    std::string out = "\"";
    for (unsigned char c : text) {
        if (c == '"' || c == '\\') { out += '\\'; out += char(c); }
        else if (c == '\n') out += "\\n";
        else if (c == '\r') out += "\\r";
        else if (c == '\t') out += "\\t";
        else if (c < 0x20) { char b[8]; std::snprintf(b, sizeof b, "\\u%04x", c); out += b; }
        else out += char(c);
    }
    return out + '"';
}

template <class Range> std::string Ints(const Range& range) {
    std::string out = "[";
    bool first = true;
    for (auto value : range) { if (!first) out += ","; first = false; out += std::to_string(value); }
    return out + "]";
}

// Prefer an unisex/any-race variant, then male, then the lowest FileDataID.
uint32_t PickDefault(const std::vector<ComponentFile>& files) {
    if (files.empty()) return 0;
    auto rank = [](const ComponentFile& f) {
        const int gender = f.gender == 2 || f.gender < 0 ? 0 : f.gender == 0 ? 1 : 2;
        return std::make_tuple(f.race > 0 ? 1 : 0, gender, f.fileId);
    };
    return std::min_element(files.begin(), files.end(), [&](const auto& a, const auto& b) { return rank(a) < rank(b); })->fileId;
}

std::string Component(const ComponentFile& f) {
    return "{\"file_data_id\":" + std::to_string(f.fileId) + ",\"gender\":" + std::to_string(f.gender) +
           ",\"class\":" + std::to_string(f.classId) + ",\"race\":" + std::to_string(f.race) +
           ",\"position\":" + std::to_string(f.position) + ",\"usage\":" + std::to_string(f.usage) +
           ",\"texture_type\":" + std::to_string(f.textureType) + ",\"material_id\":" + std::to_string(f.materialId) + "}";
}
std::string Components(const std::vector<ComponentFile>& files) {
    std::string out = "[";
    for (size_t i = 0; i < files.size(); ++i) out += (i ? "," : "") + Component(files[i]);
    return out + "]";
}
}

std::string ToJson(const Manifest& m) {
    std::string j = "{\n\"schema\":1,\n\"product\":" + Esc(m.product) + ",\"region\":" + Esc(m.region) +
                    ",\"build\":" + Esc(m.build) + ",\"build_number\":" + std::to_string(std::strtoul(m.buildNumber.c_str(), nullptr, 10)) +
                    ",\"locale\":" + std::to_string(m.locale) + ",\n\"items\":[";
    for (size_t i = 0; i < m.items.size(); ++i) {
        const Item& it = m.items[i];
        std::set<uint32_t> files;
        for (uint32_t d : [&] { std::vector<uint32_t> all{it.displayInfoId}; all.insert(all.end(), it.variantDisplayIds.begin(), it.variantDisplayIds.end()); return all; }())
            if (const auto found = m.displays.find(d); found != m.displays.end()) files.insert(found->second.files.begin(), found->second.files.end());
        if (it.iconFileId) files.insert(it.iconFileId);
        j += std::string(i ? ",\n" : "\n") + "{\"item_id\":" + std::to_string(it.id) + ",\"known\":" + (it.known ? "true" : "false") +
             ",\"name\":" + Esc(it.name) + ",\"class_id\":" + std::to_string(it.classId) +
             ",\"subclass_id\":" + std::to_string(it.subclassId) + ",\"inventory_type\":" + std::to_string(it.inventoryType) +
             ",\"quality\":" + std::to_string(it.quality) + ",\"item_level\":" + std::to_string(it.itemLevel) +
             ",\"required_level\":" + std::to_string(it.requiredLevel) + ",\"sell_price\":" + std::to_string(it.sellPrice) +
             ",\"buy_price\":" + std::to_string(it.buyPrice) + ",\"allowable_class\":" + std::to_string(it.allowableClass) +
             ",\"allowable_race\":" + std::to_string(it.allowableRace) + ",\"material\":" + std::to_string(it.material) +
             ",\"bonding\":" + std::to_string(it.bonding) + ",\"icon_file_id\":" + std::to_string(it.iconFileId) +
             ",\"icon_name\":" + Esc(it.iconFileId ? "RetailIcon_" + std::to_string(it.iconFileId) : "") +
             ",\"display_info_id\":" + std::to_string(it.displayInfoId) +
             ",\"variant_display_info_ids\":" + Ints(it.variantDisplayIds) + ",\"file_data_ids\":" + Ints(files) + "}";
    }
    j += "\n],\n\"displays\":[";
    bool first = true;
    for (const auto& [id, d] : m.displays) {
        std::vector<uint32_t> sections;
        std::string sectionTextures = "[";
        for (const auto& s : d.sectionTextures) {
            sections.push_back(s.second.fileId);
            sectionTextures += std::string(sectionTextures.size() > 1 ? "," : "") + "{\"section\":" + std::to_string(s.first) +
                               ",\"file_data_id\":" + std::to_string(s.second.fileId) +
                               ",\"material_id\":" + std::to_string(s.second.materialId) + "}";
        }
        sectionTextures += "]";
        std::vector<uint32_t> modelIds{PickDefault(d.models[0]), PickDefault(d.models[1])};
        std::vector<uint32_t> textureIds{PickDefault(d.modelTextures[0]), PickDefault(d.modelTextures[1])};
        j += std::string(first ? "\n" : ",\n") + "{\"id\":" + std::to_string(id) + ",\"flags\":" + std::to_string(d.flags) +
             ",\"item_visual\":" + std::to_string(d.itemVisual) + ",\"geoset_group_override\":" + std::to_string(d.geosetGroupOverride) +
             ",\"geoset_groups\":" + Ints(std::vector<uint32_t>(d.geosetGroup, d.geosetGroup + 6)) +
             ",\"attachment_geoset_groups\":" + Ints(std::vector<uint32_t>(d.attachmentGeosetGroup, d.attachmentGeosetGroup + 6)) +
             ",\"helmet_geoset_vis\":" + Ints(std::vector<uint32_t>(d.helmetGeosetVis, d.helmetGeosetVis + 2)) +
             ",\"model_file_ids\":" + Ints(modelIds) + ",\"texture_file_ids\":" + Ints(textureIds) +
             ",\"material_file_ids\":" + Ints(sections) + ",\"section_textures\":" + sectionTextures + ",\"models\":[" + Components(d.models[0]) + "," + Components(d.models[1]) +
             "],\"model_textures\":[" + Components(d.modelTextures[0]) + "," + Components(d.modelTextures[1]) +
             "],\"icon_name\":\"\",\"files\":" + Ints(d.files) + "}";
        first = false;
    }
    j += "\n],\n\"files\":[";
    for (size_t i = 0; i < m.files.size(); ++i) {
        const FileEntry& f = m.files[i];
        j += std::string(i ? ",\n" : "\n") + "{\"file_data_id\":" + std::to_string(f.id) + ",\"kind\":" + Esc(KindName(f.kind)) +
             ",\"path\":" + Esc(std::string("retail/") + std::to_string(f.id) + KindExtension(f.kind)) +
             ",\"bytes\":" + std::to_string(f.bytes) + ",\"sha256\":" + Esc(f.sha256) + ",\"parent\":" + Esc(f.parent) + "}";
    }
    j += "\n],\n\"tables\":[";
    for (size_t i = 0; i < DisplayTables().size(); ++i) {
        const TableRef& table = DisplayTables()[i];
        j += std::string(i ? "," : "") + "{\"file_data_id\":" + std::to_string(table.fileDataId) + ",\"name\":" + Esc(table.name) + "}";
    }
    j += "],\n\"missing\":[";
    for (size_t i = 0; i < m.missing.size(); ++i) j += (i ? "," : "") + Esc(m.missing[i]);
    j += "],\n\"warnings\":[";
    for (size_t i = 0; i < m.warnings.size(); ++i) j += (i ? "," : "") + Esc(m.warnings[i]);
    return j + "]\n}\n";
}

}
