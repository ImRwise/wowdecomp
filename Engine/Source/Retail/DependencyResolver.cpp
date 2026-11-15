#include "Engine/Source/Retail/DependencyResolver.h"
#include "Engine/Source/Retail/CdnConfig.h"
#include "Engine/Source/Retail/CascSource.h"
#include "Engine/Source/Retail/Db2.h"
#include "Engine/Source/Retail/SourceContext.h"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <tuple>

namespace Retail::Resolve {

const char* KindName(FileKind kind) {
    switch (kind) {
    case FileKind::Model: return "model";
    case FileKind::Skin: return "skin";
    case FileKind::Animation: return "animation";
    case FileKind::Physics: return "physics";
    case FileKind::Skeleton: return "skeleton";
    case FileKind::Bone: return "bone";
    case FileKind::Texture: return "texture";
    case FileKind::Icon: return "icon";
    }
    return "unknown";
}

const char* KindExtension(FileKind kind) {
    switch (kind) {
    case FileKind::Model: return ".m2";
    case FileKind::Skin: return ".skin";
    case FileKind::Animation: return ".anim";
    case FileKind::Physics: return ".phys";
    case FileKind::Skeleton: return ".skel";
    case FileKind::Bone: return ".bone";
    case FileKind::Texture:
    case FileKind::Icon: return ".blp";
    }
    return ".bin";
}

// ---- SHA-256 --------------------------------------------------------------------------------
namespace {
constexpr uint32_t kK[64] = {
    0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1, 0x923f82a4, 0xab1c5ed5,
    0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3, 0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174,
    0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc, 0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da,
    0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7, 0xc6e00bf3, 0xd5a79147, 0x06ca6351, 0x14292967,
    0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13, 0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85,
    0xa2bfe8a1, 0xa81a664b, 0xc24b8b70, 0xc76c51a3, 0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070,
    0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f, 0x682e6ff3,
    0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208, 0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2};
inline uint32_t Rotr(uint32_t x, int n) { return (x >> n) | (x << (32 - n)); }
void Block(uint32_t h[8], const uint8_t* p) {
    uint32_t w[64];
    for (int i = 0; i < 16; ++i)
        w[i] = uint32_t(p[i * 4]) << 24 | uint32_t(p[i * 4 + 1]) << 16 | uint32_t(p[i * 4 + 2]) << 8 | p[i * 4 + 3];
    for (int i = 16; i < 64; ++i) {
        const uint32_t s0 = Rotr(w[i - 15], 7) ^ Rotr(w[i - 15], 18) ^ (w[i - 15] >> 3);
        const uint32_t s1 = Rotr(w[i - 2], 17) ^ Rotr(w[i - 2], 19) ^ (w[i - 2] >> 10);
        w[i] = w[i - 16] + s0 + w[i - 7] + s1;
    }
    uint32_t a = h[0], b = h[1], c = h[2], d = h[3], e = h[4], f = h[5], g = h[6], hh = h[7];
    for (int i = 0; i < 64; ++i) {
        const uint32_t t1 = hh + (Rotr(e, 6) ^ Rotr(e, 11) ^ Rotr(e, 25)) + ((e & f) ^ (~e & g)) + kK[i] + w[i];
        const uint32_t t2 = (Rotr(a, 2) ^ Rotr(a, 13) ^ Rotr(a, 22)) + ((a & b) ^ (a & c) ^ (b & c));
        hh = g; g = f; f = e; e = d + t1; d = c; c = b; b = a; a = t1 + t2;
    }
    h[0] += a; h[1] += b; h[2] += c; h[3] += d; h[4] += e; h[5] += f; h[6] += g; h[7] += hh;
}
}

std::string Sha256Hex(const uint8_t* data, size_t size) {
    uint32_t h[8] = {0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a, 0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19};
    size_t full = size / 64;
    for (size_t i = 0; i < full; ++i) Block(h, data + i * 64);
    uint8_t tail[128] = {};
    const size_t rest = size - full * 64;
    if (rest) std::memcpy(tail, data + full * 64, rest);
    tail[rest] = 0x80;
    const size_t total = rest + 1 + 8 <= 64 ? 64 : 128;
    const uint64_t bits = uint64_t(size) * 8;
    for (int i = 0; i < 8; ++i) tail[total - 1 - i] = uint8_t(bits >> (8 * i));
    for (size_t i = 0; i < total; i += 64) Block(h, tail + i);
    char out[65];
    for (int i = 0; i < 8; ++i) std::snprintf(out + i * 8, 9, "%08x", h[i]);
    return std::string(out, 64);
}

// ---- M2 ------------------------------------------------------------------------------------
bool ParseModelFiles(const std::vector<uint8_t>& bytes, ModelFiles& out, std::string& error) {
    out = ModelFiles{};
    if (bytes.size() < 8) { error = "model is too small"; return false; }
    if (!std::memcmp(bytes.data(), "MD20", 4)) { out.legacy = true; return true; }
    auto u32 = [&](size_t at) { uint32_t v; std::memcpy(&v, bytes.data() + at, 4); return v; };
    if (std::memcmp(bytes.data(), "MD21", 4) != 0) {
        // Skeleton (.skel) and similar chunk files start with another tag but share the layout.
        bool known = false;
        for (const char* tag : {"SKL1", "SKA1", "SKB1", "SKS1", "AFID", "BFID"})
            if (!std::memcmp(bytes.data(), tag, 4)) known = true;
        if (!known) { error = "unrecognised chunk container"; return false; }
    }
    for (size_t at = 0; at + 8 <= bytes.size();) {
        const uint32_t size = u32(at + 4);
        if (size > bytes.size() - at - 8) { error = "chunk overruns file"; return false; }
        const size_t data = at + 8;
        const char* tag = reinterpret_cast<const char*>(bytes.data() + at);
        auto ids = [&](std::vector<uint32_t>& to) {
            for (size_t o = 0; o + 4 <= size; o += 4)
                if (const uint32_t id = u32(data + o)) to.push_back(id);
        };
        if (!std::memcmp(tag, "SFID", 4)) ids(out.skins);
        else if (!std::memcmp(tag, "TXID", 4)) ids(out.textures);
        else if (!std::memcmp(tag, "BFID", 4)) ids(out.bones);
        else if (!std::memcmp(tag, "GPID", 4) || !std::memcmp(tag, "RPID", 4)) ids(out.particleModels);   // geometry + recursive particle models
        else if (!std::memcmp(tag, "PFID", 4) && size >= 4) out.physics = u32(data);
        else if (!std::memcmp(tag, "SKID", 4) && size >= 4) out.skeleton = u32(data);
        else if (!std::memcmp(tag, "AFID", 4)) {
            for (size_t o = 0; o + 8 <= size; o += 8) {
                uint16_t animation, variation;
                std::memcpy(&animation, bytes.data() + data + o, 2);
                std::memcpy(&variation, bytes.data() + data + o + 2, 2);
                if (const uint32_t id = u32(data + o + 4)) out.animations.emplace_back(uint32_t(animation) << 16 | variation, id);
            }
        }
        at = data + size;
    }
    return true;
}

// ---- walk ----------------------------------------------------------------------------------
ComponentFile Resolver::ModelFile(uint32_t fileId) const {
    ComponentFile file;
    file.fileId = fileId;
    const auto found = m_db.componentModels.find(fileId);
    if (found != m_db.componentModels.end()) {
        file.gender = found->second.gender; file.classId = found->second.classId;
        file.race = found->second.race; file.position = found->second.position;
    }
    return file;
}

ComponentFile Resolver::TextureFile(uint32_t fileId, uint32_t usage) const {
    ComponentFile file;
    file.fileId = fileId;
    file.usage = usage;
    const auto found = m_db.componentTextures.find(fileId);
    if (found != m_db.componentTextures.end()) {
        file.gender = found->second.gender; file.classId = found->second.classId; file.race = found->second.race;
    }
    return file;
}

// A display owns every file reachable from the files it names, including files first reached
// through another display. Link records that ownership transitively.
void Resolver::Link(Display* display, const Key& key) {
    if (!display->files.insert(key.first).second) return;
    const auto children = m_children.find(key);
    if (children == m_children.end()) return;
    for (const Key& child : children->second) Link(display, child);
}

void Resolver::AddFile(uint32_t id, FileKind kind, const std::string& parent, Display* display) {
    if (!id) return;
    const Key key{id, int(kind)};
    if (display) {
        auto& owners = m_owners[key];
        if (std::find(owners.begin(), owners.end(), display) == owners.end()) owners.push_back(display);
        Link(display, key);
    }
    if (m_seen.insert(key).second) m_pending.push_back({id, kind, parent});
}

void Resolver::Children(const Pending& entry, const ModelFiles& files, std::vector<FetchJournal::Child>& list) {
    if (entry.kind == FileKind::Model) {
        for (uint32_t id : files.skins) list.push_back({id, FileKind::Skin});
        for (uint32_t id : files.textures) list.push_back({id, FileKind::Texture});
    }
    for (const auto& animation : files.animations) list.push_back({animation.second, FileKind::Animation});
    for (uint32_t id : files.bones) list.push_back({id, FileKind::Bone});
    if (entry.kind == FileKind::Model) {
        if (files.physics) list.push_back({files.physics, FileKind::Physics});
        for (uint32_t id : files.particleModels) list.push_back({id, FileKind::Model});
        if (files.skeleton) list.push_back({files.skeleton, FileKind::Skeleton});
    }
}

void Resolver::Process(const Pending& entry, Manifest& out) {
    const Key key{entry.id, int(entry.kind)};
    const bool structural = entry.kind == FileKind::Model || entry.kind == FileKind::Skeleton;
    FileEntry file;
    file.id = entry.id; file.kind = entry.kind; file.parent = entry.parent;
    std::vector<FetchJournal::Child> children;
    const FetchJournal::Record* recorded = m_journal ? m_journal->Find(entry.id, entry.kind) : nullptr;
    if (recorded && m_fetcher.Cached(entry.id, entry.kind)) {
        // Resume: the journal says this file was fetched, hashed and parsed, and the cache still has it.
        file.bytes = recorded->bytes;
        file.sha256 = recorded->sha256;
        children = recorded->children;
    } else if (m_download || structural) {
        std::vector<uint8_t> bytes;
        std::string error;
        if (!m_fetcher.Get(entry.id, entry.kind, bytes, error)) {
            out.missing.push_back(std::string(KindName(entry.kind)) + ":" + std::to_string(entry.id) + ": " + error);
            if (m_journal) m_journal->Failed(entry.id, entry.kind, error);
            return;
        }
        file.bytes = bytes.size();
        file.sha256 = Sha256Hex(bytes.data(), bytes.size());
        if (structural) {
            ModelFiles files;
            if (!ParseModelFiles(bytes, files, error)) {
                out.warnings.push_back(std::string(KindName(entry.kind)) + ":" + std::to_string(entry.id) + ": " + error);
            } else {
                if (files.legacy && entry.kind == FileKind::Model)
                    out.warnings.push_back("model:" + std::to_string(entry.id) + ": legacy MD20 container; named textures are not tracked");
                Children(entry, files, children);
            }
        }
        if (m_journal) {
            FetchJournal::Record record;
            record.bytes = file.bytes; record.sha256 = file.sha256; record.children = children;
            m_journal->Done(entry.id, entry.kind, record);
        }
    }
    out.files.push_back(file);

    const std::string parent = std::string(KindName(entry.kind)) + ":" + std::to_string(entry.id);
    const std::vector<Display*> owners = m_owners.count(key) ? m_owners[key] : std::vector<Display*>();
    for (const FetchJournal::Child& child : children) {
        if (!child.id) continue;
        const Key childKey{child.id, int(child.kind)};
        m_children[key].push_back(childKey);
        auto& childOwners = m_owners[childKey];
        for (Display* owner : owners) {
            if (std::find(childOwners.begin(), childOwners.end(), owner) == childOwners.end()) childOwners.push_back(owner);
            Link(owner, childKey);
        }
        if (m_seen.insert(childKey).second) m_pending.push_back({child.id, child.kind, parent});
    }
}

// Breadth-first: every wave is prefetched in parallel (when the fetcher can), then processed in order,
// so the results do not depend on thread timing.
void Resolver::Drain(Manifest& out) {
    while (!m_pending.empty()) {
        std::vector<Pending> batch;
        batch.swap(m_pending);
        std::vector<PrefetchItem> need;
        for (const Pending& entry : batch) {
            const bool structural = entry.kind == FileKind::Model || entry.kind == FileKind::Skeleton;
            if (!(m_download || structural)) continue;
            if (m_journal && m_journal->Find(entry.id, entry.kind) && m_fetcher.Cached(entry.id, entry.kind)) continue;
            need.push_back({entry.id, entry.kind});
        }
        if (!need.empty()) m_fetcher.Prefetch(need);
        for (const Pending& entry : batch) Process(entry, out);
    }
}

void Resolver::AddDisplay(uint32_t displayId, Manifest& out) {
    if (!displayId || !m_displaysDone.insert(displayId).second) return;
    const auto row = m_db.displays.find(displayId);
    Display& display = out.displays[displayId];
    display.id = displayId;
    if (row == m_db.displays.end()) {
        out.warnings.push_back("display:" + std::to_string(displayId) + ": not in ItemDisplayInfo");
        return;
    }
    const auto& r = row->second;
    display.flags = r.flags; display.itemVisual = r.itemVisual; display.particleColor = r.particleColor;
    display.geosetGroupOverride = r.geosetOverride;
    std::copy(r.geoset, r.geoset + 6, display.geosetGroup);
    std::copy(r.attach, r.attach + 6, display.attachmentGeosetGroup);
    std::copy(r.helmet, r.helmet + 2, display.helmetGeosetVis);
    std::copy(r.modelType, r.modelType + 2, display.modelType);
    const std::string parent = "display:" + std::to_string(displayId);

    auto sortedTextures = [&](uint32_t material) {
        std::vector<Db::TextureRow> sorted;
        const auto found = m_db.texturesByMaterial.find(material);
        if (found != m_db.texturesByMaterial.end()) sorted = found->second;
        std::sort(sorted.begin(), sorted.end(), [](const auto& a, const auto& b) { return a.fileId < b.fileId; });
        return sorted;
    };
    for (int slot = 0; slot < 2; ++slot) {
        if (const auto models = m_db.modelFilesByResources.find(r.modelRes[slot]); r.modelRes[slot] && models != m_db.modelFilesByResources.end()) {
            std::vector<uint32_t> sorted = models->second;
            std::sort(sorted.begin(), sorted.end());
            for (uint32_t file : sorted) {
                display.models[slot].push_back(ModelFile(file));
                AddFile(file, FileKind::Model, parent, &display);
            }
        }
        if (r.matRes[slot]) {
            for (const auto& texture : sortedTextures(r.matRes[slot])) {
                display.modelTextures[slot].push_back(TextureFile(texture.fileId, texture.usage));
                display.modelTextures[slot].back().materialId = r.matRes[slot];
                AddFile(texture.fileId, FileKind::Texture, parent, &display);
            }
        }
        if (const auto helmet = m_db.helmetGeosets.find(r.helmet[slot]); r.helmet[slot] && helmet != m_db.helmetGeosets.end())
            display.helmetGeosets[slot] = helmet->second;
    }
    if (const auto materials = m_db.displayMaterials.find(displayId); materials != m_db.displayMaterials.end()) {
        for (const auto& material : materials->second) {
            for (const auto& texture : sortedTextures(material.materialResources)) {
                display.sectionTextures.emplace_back(material.section, TextureFile(texture.fileId, texture.usage));
                display.sectionTextures.back().second.materialId = material.materialResources;
                AddFile(texture.fileId, FileKind::Texture, parent, &display);
            }
        }
    }
    if (const auto materials = m_db.displayModelMaterials.find(displayId); materials != m_db.displayModelMaterials.end()) {
        for (const auto& material : materials->second) {
            if (material.modelIndex > 1) continue;
            for (const auto& texture : sortedTextures(material.materialResources)) {
                display.modelTextures[material.modelIndex].push_back(TextureFile(texture.fileId, texture.usage));
                display.modelTextures[material.modelIndex].back().textureType = int32_t(material.textureType);
                display.modelTextures[material.modelIndex].back().materialId = material.materialResources;
                AddFile(texture.fileId, FileKind::Texture, parent, &display);
            }
        }
    }
}

void Resolver::AddItem(uint32_t itemId, Manifest& out) {
    Item item;
    item.id = itemId;
    const auto base = m_db.items.find(itemId);
    const auto sparse = m_db.sparse.find(itemId);
    item.known = base != m_db.items.end() || sparse != m_db.sparse.end();
    if (!item.known) {
        // Items appear through transmog sets by ItemModifiedAppearance even when the item row is gone.
        out.warnings.push_back("item:" + std::to_string(itemId) + ": not found in Item/ItemSparse");
    }
    if (base != m_db.items.end()) {
        item.classId = base->second.classId; item.subclassId = base->second.subclassId;
        item.material = base->second.material; item.inventoryType = base->second.inventoryType;
        item.iconFileId = base->second.icon;
    }
    if (sparse != m_db.sparse.end()) {
        const auto& s = sparse->second;
        item.name = s.name; item.quality = s.quality; item.itemLevel = s.itemLevel;
        item.requiredLevel = s.requiredLevel; item.bonding = s.bonding; item.sellPrice = s.sellPrice;
        item.buyPrice = s.buyPrice;
        item.allowableRace = int32_t(s.allowableRace);
        item.allowableClass = s.allowableClass == 0xFFFF ? -1 : int32_t(s.allowableClass);
        if (!item.inventoryType) item.inventoryType = s.inventoryType;
    }
    std::vector<Db::Modified> mods;
    if (const auto ids = m_db.modifiedByItem.find(itemId); ids != m_db.modifiedByItem.end())
        for (uint32_t id : ids->second) mods.push_back(m_db.modified.at(id));
    std::sort(mods.begin(), mods.end(), [](const auto& a, const auto& b) {
        return std::tie(a.modifier, a.order, a.id) < std::tie(b.modifier, b.order, b.id); });
    for (const auto& mod : mods) {
        const auto appearance = m_db.appearances.find(mod.appearanceId);
        if (appearance == m_db.appearances.end()) continue;
        const uint32_t display = appearance->second.displayInfoId;
        if (!display) continue;
        if (!item.displayInfoId) item.displayInfoId = display;
        else if (display != item.displayInfoId &&
                 std::find(item.variantDisplayIds.begin(), item.variantDisplayIds.end(), display) == item.variantDisplayIds.end())
            item.variantDisplayIds.push_back(display);
        if (!item.iconFileId) item.iconFileId = appearance->second.defaultIcon;
        AddDisplay(display, out);
    }
    if (!item.displayInfoId) out.warnings.push_back("item:" + std::to_string(itemId) + ": no ItemModifiedAppearance display");
    if (item.iconFileId) AddFile(item.iconFileId, FileKind::Icon, "item:" + std::to_string(itemId), nullptr);
    out.items.push_back(std::move(item));
}

bool Resolver::Run(const Request& request, Manifest& out, std::string& error) {
    std::set<uint32_t> items(request.items.begin(), request.items.end());
    for (uint32_t set : request.sets) {
        const auto found = m_db.setItems.find(set);
        if (found == m_db.setItems.end()) { out.warnings.push_back("set:" + std::to_string(set) + ": not in TransmogSetItem"); continue; }
        for (uint32_t ima : found->second) {
            const auto mod = m_db.modified.find(ima);
            if (mod != m_db.modified.end()) items.insert(mod->second.itemId);
        }
    }
    for (uint32_t item : items) AddItem(item, out);
    // Bare display IDs: resolve with no owning item.
    for (uint32_t display : request.displays) AddDisplay(display, out);
    if (items.empty() && request.displays.empty()) { error = "nothing to resolve"; return false; }
    Drain(out);
    std::sort(out.files.begin(), out.files.end(), [](const FileEntry& a, const FileEntry& b) {
        return std::tie(a.kind, a.id) < std::tie(b.kind, b.id); });
    std::sort(out.items.begin(), out.items.end(), [](const Item& a, const Item& b) { return a.id < b.id; });
    std::sort(out.missing.begin(), out.missing.end());
    return true;
}

// ---- catalogue -----------------------------------------------------------------------------
bool IsArmorSlot(uint32_t classId, uint32_t subclassId, uint32_t inventoryType) {
    if (classId != 4 || subclassId == 6) return false;  // a shield is a weapon-group item
    switch (inventoryType) {
    case 1: case 3: case 4: case 5: case 6: case 7: case 8: case 9: case 10: case 16: case 19: case 20: return true;
    default: return false;
    }
}

bool IsWeaponSlot(uint32_t classId, uint32_t subclassId, uint32_t inventoryType) {
    if (classId == 2) return true;  // every weapon, incl. bows, guns, crossbows, wands, thrown
    return classId == 4 && (subclassId == 6 || inventoryType == 14 || inventoryType == 23);  // shields, held off-hands
}

void SelectCatalog(const Db& db, const CatalogOptions& options, Request& request, size_t* setCount, size_t* itemCount) {
    const bool filtered = options.minExpansion >= 0 || options.maxExpansion >= 0;
    const auto inRange = [&](int64_t expansion) {
        return (options.minExpansion < 0 || expansion >= options.minExpansion) &&
               (options.maxExpansion < 0 || expansion <= options.maxExpansion);
    };
    size_t sets = 0, items = 0;
    if (options.sets) {
        std::vector<uint32_t> ids;
        for (const auto& entry : db.setItems) {
            const auto expansion = db.setExpansion.find(entry.first);
            if (filtered && !inRange(expansion == db.setExpansion.end() ? 0 : int64_t(expansion->second))) continue;
            ids.push_back(entry.first);
        }
        std::sort(ids.begin(), ids.end());
        for (uint32_t id : ids) request.sets.push_back(id);
        sets = ids.size();
    }
    if (options.armor || options.weapons) {
        std::vector<uint32_t> ids;
        for (const auto& entry : db.modifiedByItem) ids.push_back(entry.first);
        std::sort(ids.begin(), ids.end());
        std::set<uint32_t> appearances;
        for (uint32_t itemId : ids) {
            const auto base = db.items.find(itemId);
            if (base == db.items.end()) continue;
            const auto sparse = db.sparse.find(itemId);
            const uint32_t inventory = base->second.inventoryType ? base->second.inventoryType
                                                                  : (sparse != db.sparse.end() ? sparse->second.inventoryType : 0);
            const bool armor = IsArmorSlot(base->second.classId, base->second.subclassId, inventory);
            const bool weapon = IsWeaponSlot(base->second.classId, base->second.subclassId, inventory);
            if (!((options.armor && armor) || (options.weapons && weapon))) continue;
            if (filtered && (sparse == db.sparse.end() || !inRange(sparse->second.expansion))) continue;
            if (options.skipUncollectible && (sparse == db.sparse.end() || sparse->second.quality == 0)) continue;
            // An item with no appearance row or display never produces a model.
            std::vector<uint32_t> own;
            for (uint32_t ima : db.modifiedByItem.at(itemId)) {
                const auto mod = db.modified.find(ima);
                if (mod == db.modified.end()) continue;
                const auto appearance = db.appearances.find(mod->second.appearanceId);
                if (appearance != db.appearances.end() && appearance->second.displayInfoId) own.push_back(mod->second.appearanceId);
            }
            if (own.empty()) continue;
            bool fresh = options.everyItem;
            for (uint32_t id : own) fresh |= appearances.insert(id).second;
            if (!fresh) continue;
            request.items.push_back(itemId);
            ++items;
        }
    }
    if (setCount) *setCount = sets;
    if (itemCount) *itemCount = items;
}

// ---- journal -------------------------------------------------------------------------------
namespace {
uint64_t JournalKey(uint32_t id, FileKind kind) { return (uint64_t(id) << 8) | uint64_t(kind); }
const char* const kKindNames[] = {"model", "skin", "animation", "physics", "skeleton", "bone", "texture", "icon"};
bool KindFromName(const std::string& name, FileKind& kind) {
    for (int i = 0; i < 8; ++i) {
        if (name == kKindNames[i]) { kind = FileKind(i); return true; }
    }
    return false;
}
}

// Every value is written as a string so the flat parser reads each line back unchanged.
bool FetchJournal::Open(const std::string& path, std::string& error) {
    bool torn = false;
    if (m_file) { std::fclose(static_cast<std::FILE*>(m_file)); m_file = nullptr; }  // reopening must not leak the old handle
    if (std::FILE* existing = std::fopen(path.c_str(), "rb")) {
        std::string line;
        int c;
        auto consume = [&]() {
            std::map<std::string, std::string> values;
            std::string parseError;
            FileKind kind;
            if (!Cdn::ParseFlatJson(line, values, parseError) || !KindFromName(values["k"], kind)) return;
            const uint32_t id = uint32_t(std::strtoul(values["id"].c_str(), nullptr, 10));
            if (values["s"] == "done") {
                Record record;
                record.bytes = std::strtoull(values["b"].c_str(), nullptr, 10);
                record.sha256 = values["h"];
                const std::string list = values["c"];
                for (size_t at = 0; at < list.size();) {
                    const size_t end = list.find(',', at);
                    const std::string token = list.substr(at, end == std::string::npos ? std::string::npos : end - at);
                    const size_t colon = token.find(':');
                    FileKind childKind;
                    if (colon != std::string::npos && KindFromName(token.substr(0, colon), childKind))
                        record.children.push_back({uint32_t(std::strtoul(token.c_str() + colon + 1, nullptr, 10)), childKind});
                    if (end == std::string::npos) break;
                    at = end + 1;
                }
                m_records[JournalKey(id, kind)] = std::move(record);
            } else if (values["s"] == "failed") {
                ++m_failures;
            }
        };
        // Only complete (newline-terminated) lines count: the file is append-only, so only the last can be torn.
        while ((c = std::fgetc(existing)) != EOF) {
            torn = c != '\n';
            if (torn) { line.push_back(char(c)); continue; }
            consume();
            line.clear();
        }
        std::fclose(existing);
    }
    m_file = std::fopen(path.c_str(), "ab");
    if (!m_file) { error = "cannot open fetch journal " + path; return false; }
    // Terminate a torn last line so the next record starts on a line of its own.
    if (torn) std::fputc('\n', static_cast<std::FILE*>(m_file));
    return true;
}

const FetchJournal::Record* FetchJournal::Find(uint32_t id, FileKind kind) const {
    const auto found = m_records.find(JournalKey(id, kind));
    return found == m_records.end() ? nullptr : &found->second;
}

void FetchJournal::Done(uint32_t id, FileKind kind, const Record& record) {
    m_records[JournalKey(id, kind)] = record;
    if (!m_file) return;
    std::string children;
    for (const Child& child : record.children) {
        if (!children.empty()) children += ",";
        children += std::string(kKindNames[int(child.kind)]) + ":" + std::to_string(child.id);
    }
    std::FILE* file = static_cast<std::FILE*>(m_file);
    std::fprintf(file, "{\"s\":\"done\",\"id\":\"%u\",\"k\":\"%s\",\"b\":\"%llu\",\"h\":\"%s\",\"c\":\"%s\"}\n",
                 id, kKindNames[int(kind)], (unsigned long long)record.bytes, record.sha256.c_str(), children.c_str());
    std::fflush(file);
}

void FetchJournal::Failed(uint32_t id, FileKind kind, const std::string& error) {
    ++m_failures;
    if (!m_file) return;
    std::string clean;
    for (char c : error) clean += (c == '"' || c == '\\' || c == '\n' || c == '\r') ? ' ' : c;
    std::FILE* file = static_cast<std::FILE*>(m_file);
    std::fprintf(file, "{\"s\":\"failed\",\"id\":\"%u\",\"k\":\"%s\",\"e\":\"%s\"}\n", id, kKindNames[int(kind)], clean.c_str());
    std::fflush(file);
}

FetchJournal::~FetchJournal() {
    if (m_file) std::fclose(static_cast<std::FILE*>(m_file));
}

}
