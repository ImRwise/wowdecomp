#include "WoW/Common/ComponentCore/RetailArmorPhysics.h"

#if defined(WOWDECOMP_RETAIL_ASSETS) && defined(WOWDECOMP_RETAIL_ARMOR_PHYSICS)

#include <algorithm>
#include <atomic>
#include <cmath>
#include <condition_variable>
#include <cstdarg>
#include <cstdio>
#include <deque>
#include <memory>
#include <mutex>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "Engine/Source/Model2/M2Data.h"
#include "Engine/Source/Model2/M2Model.h"
#include "Engine/Source/Retail/ArmorPath.h"
#include "Engine/Source/Retail/ArmorPhysics.h"
#include "Engine/Source/Retail/ArmorPhysicsSpec.h"
#include "Engine/Source/Retail/CascSource.h"
#include "Engine/Source/Retail/DependencyResolver.h"
#include "Engine/Source/Retail/PhysFile.h"
#include "Engine/Source/Tempest/C44Matrix.h"
#include "WoW/Source/Ui/OptionCVars.h"

namespace RetailArmorPhysics {

namespace {

namespace Solver = Retail::ArmorPhysics;

constexpr float kYardsPerSecondSq = 10.7f;     // gravity, yd/s^2
constexpr float kMaxAcceleration = 80.0f;      // yd/s^2, rejects spikes from placement snaps
constexpr float kMaxAngularRate = 20.0f;       // rad/s, same for turning
constexpr float kTeleportDistance = 25.0f;     // yards in one frame: a teleport, not a run
constexpr float kLongFrame = 0.25f;            // seconds: a stall, simulation restarts from rest
constexpr size_t kMaxBones = 64;               // per group
constexpr size_t kUnrecognisedRig = 96;        // more free bones than this and the heuristic is not describing cloth
constexpr size_t kMaxChildren = 16;            // attached item models simulated per wearer
constexpr uint32_t kProceduralFlag = 0x80;
constexpr uint32_t kBusyFlags = 0x180;         // already driven by something else (twist, wheels, body shape)
constexpr uint32_t kCollectionSocket = UINT32_MAX - 1;   // armor collections skin to the body: they have no rig of their own

CVar* s_enabled = nullptr;
CVar* s_strength = nullptr;
CVar* s_windScale = nullptr;
CVar* s_max = nullptr;
CVar* s_debug = nullptr;
CVar* s_authored = nullptr;
WindProvider s_wind = nullptr;

void Log(const char* format, ...) {
    FILE* file = std::fopen("Logs/RetailArmorPhysics.log", "a");
    if (!file) return;
    va_list args;
    va_start(args, format);
    std::vfprintf(file, format, args);
    va_end(args);
    std::fputc('\n', file);
    std::fclose(file);
}

// ---- .phys loading ------------------------------------------------------------------------------------------------
// A model's PFID names its .phys file. Both files come from the Retail source (packed patch, decoded cache or CDN),
// which can mean network IO: a worker thread does it, the main thread only polls.

enum class PhysState { Pending, Ready, None };
struct PhysEntry {
    PhysState state = PhysState::Pending;
    std::shared_ptr<const Retail::Phys::File> file;
    std::string note;
};

class PhysLoader {
public:
    ~PhysLoader() {
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            m_stop = true;
        }
        m_wake.notify_all();
        if (m_thread.joinable()) m_thread.join();
    }
    // Returns the current state of `modelId`'s physics (starting the lookup on first use).
    PhysEntry Get(uint32_t modelId) {
        std::lock_guard<std::mutex> lock(m_mutex);
        auto found = m_entries.find(modelId);
        if (found != m_entries.end()) return found->second;
        m_entries[modelId] = PhysEntry{};
        m_queue.push_back(modelId);
        if (!m_thread.joinable()) m_thread = std::thread([this] { Run(); });
        m_wake.notify_one();
        return PhysEntry{};
    }
    void Clear() {
        std::lock_guard<std::mutex> lock(m_mutex);
        for (auto it = m_entries.begin(); it != m_entries.end();)
            it = it->second.state == PhysState::Pending ? std::next(it) : m_entries.erase(it);
    }

private:
    void Run() {
        for (;;) {
            uint32_t id;
            {
                std::unique_lock<std::mutex> lock(m_mutex);
                m_wake.wait(lock, [this] { return m_stop || !m_queue.empty(); });
                if (m_stop) return;
                id = m_queue.front();
                m_queue.pop_front();
            }
            PhysEntry result = Load(id);
            std::lock_guard<std::mutex> lock(m_mutex);
            m_entries[id] = std::move(result);
        }
    }
    static PhysEntry Load(uint32_t modelId) {
        PhysEntry result;
        result.state = PhysState::None;
        Retail::SourceScope scope(Retail::SourceKind::Retail);
        const std::string modelName = Retail::Armor::FileName(modelId, ".m2");
        std::vector<uint8_t> bytes;
        std::string error;
        Retail::CascSource& source = Retail::SourceForPath(modelName.c_str());
        if (!source.Inspect(modelName.c_str(), bytes, error)) { result.note = "model: " + error; return result; }
        Retail::Resolve::ModelFiles files;
        if (!Retail::Resolve::ParseModelFiles(bytes, files, error)) { result.note = "chunks: " + error; return result; }
        if (!files.physics) { result.note = "no PFID"; return result; }
        const std::string physName = Retail::Armor::FileName(files.physics, ".phys");
        if (!source.Inspect(physName.c_str(), bytes, error)) { result.note = "phys " + std::to_string(files.physics) + ": " + error; return result; }
        auto file = std::make_shared<Retail::Phys::File>();
        if (!Retail::Phys::Parse(bytes, *file, error)) { result.note = "phys " + std::to_string(files.physics) + ": " + error; return result; }
        result.state = PhysState::Ready;
        result.file = std::move(file);
        result.note = "phys " + std::to_string(files.physics);
        return result;
    }

    std::mutex m_mutex;
    std::condition_variable m_wake;
    std::deque<uint32_t> m_queue;
    std::unordered_map<uint32_t, PhysEntry> m_entries;
    std::thread m_thread;
    bool m_stop = false;
};

PhysLoader& Loader() {
    static PhysLoader loader;
    return loader;
}

// ---- groups: one body rig or one attached item model --------------------------------------------------------------

struct Motion {                 // a node's parent frame between frames
    bool valid = false;
    float rows[9]{};            // parent bone orientation rows (model space), scale removed
    float omega[3]{};           // local angular velocity
    float alpha[3]{};           // local angular acceleration (low-passed)
};

struct Group {
    CM2Model* model = nullptr;
    const void* data = nullptr;               // the M2Data the nodes were built from
    uint32_t fileId = 0;                      // retail/<id>.m2 for children, 0 for the body
    std::vector<uint32_t> bones;              // model bone index per solver node
    Solver::Body body;
    std::vector<Motion> motion;
    std::vector<Solver::NodeFrame> frames;
    std::string source = "none";              // "phys", "heuristic", "none"
    bool wantsPhys = false;
    bool rigRejected = false;
    bool built = false;
};

struct Wearer {
    CM2Model* model = nullptr;
    Group body;
    std::unordered_map<CM2Model*, Group> children;
    bool haveHistory = false;
    float position[3]{};
    float velocity[3]{};
    float acceleration[3]{};
    uint32_t lastMs = 0;
    uint32_t lastLogMs = 0;
};

std::unordered_map<CCharacterComponent*, Wearer> s_wearers;
uint32_t s_frameMs = 0;
uint32_t s_simulatedThisFrame = 0;

float Length3(float x, float y, float z) { return std::sqrt(x * x + y * y + z * z); }

// A free bone: part of the rig but animated by nothing (no key bone id, no tracks, no billboard).
bool FreeBone(const M2Data* data, CM2Model* model, uint32_t index) {
    const auto& bone = data->m_bones[index];
    if (bone.m_parentIndex == 0xFFFF || bone.m_parentIndex >= data->m_bones.m_count) return false;
    if (bone.m_boneId != 0xFFFFFFFFu) return false;
    if (bone.m_flags & 0x78) return false;
    if (bone.m_translationTrack.m_sequenceTimes.m_count || bone.m_rotationTrack.m_sequenceTimes.m_count ||
        bone.m_scaleTrack.m_sequenceTimes.m_count) return false;
    return !(model->m_bones[index].m_flags & kBusyFlags);
}

// The heuristic: free bones in the rig, chained by their nearest free ancestor.
bool HeuristicSpec(Group& group, CM2Model* model, Solver::Spec& spec) {
    spec = {};
    spec.source = "heuristic";
    const M2Data* data = model->m_shared->m_data;
    const uint32_t count = data->m_bones.m_count;
    if (!count || count > 65535 || !model->m_bones) return false;
    // Whatever hangs off an attachment point (helm, shoulders, weapons, quiver) rides that point's bone. Retail rigs park the
    // attachment points on small unanimated leaf bones, which look exactly like loose cloth: simulating one throws the item
    // around and shifts it off the head. Every bone an attachment uses, and every ancestor of one, stays rigid.
    std::vector<uint8_t> pinned(count, 0);
    for (uint32_t a = 0; a < data->m_attachments.m_count; ++a) {
        uint32_t walk = data->m_attachments[a].m_bone;
        for (uint32_t guard = 0; walk < count && guard < count && !pinned[walk]; ++guard) {
            pinned[walk] = 1;
            walk = data->m_bones[walk].m_parentIndex;
        }
    }
    std::vector<uint8_t> free(count, 0);
    size_t freeCount = 0;
    for (uint32_t i = 0; i < count; ++i) {
        free[i] = (!pinned[i] && FreeBone(data, model, i)) ? 1 : 0;
        freeCount += free[i];
    }
    if (!freeCount) return false;
    if (freeCount > kUnrecognisedRig) {
        if (!group.rigRejected) Log("rig has %zu free bones (limit %zu): heuristic physics skipped", freeCount, kUnrecognisedRig);
        group.rigRejected = true;
        return false;
    }
    struct Candidate { uint16_t index; int ancestor; int depth; };
    std::vector<Candidate> candidates;
    for (uint32_t i = 0; i < count; ++i) {
        if (!free[i]) continue;
        int ancestor = -1, depth = 0;
        uint16_t walk = data->m_bones[i].m_parentIndex;
        for (uint32_t guard = 0; walk < count && guard < count; ++guard) {
            if (free[walk]) { if (ancestor < 0) ancestor = walk; ++depth; }
            walk = data->m_bones[walk].m_parentIndex;
        }
        candidates.push_back({uint16_t(i), ancestor, depth});
    }
    std::stable_sort(candidates.begin(), candidates.end(), [](const Candidate& a, const Candidate& b) { return a.depth < b.depth; });
    if (candidates.size() > kMaxBones) candidates.resize(kMaxBones);
    std::vector<int> specOf(count, -1);
    for (const Candidate& c : candidates) {
        const auto& bone = data->m_bones[c.index];
        float dx = 0, dy = 0, dz = -1;
        bool hasChild = false;
        for (uint32_t k = 0; k < count && !hasChild; ++k) {
            if (!free[k] || data->m_bones[k].m_parentIndex != c.index) continue;
            dx = data->m_bones[k].m_pivot.x - bone.m_pivot.x;
            dy = data->m_bones[k].m_pivot.y - bone.m_pivot.y;
            dz = data->m_bones[k].m_pivot.z - bone.m_pivot.z;
            hasChild = true;
        }
        if (!hasChild) {
            const auto& parent = data->m_bones[bone.m_parentIndex];
            dx = bone.m_pivot.x - parent.m_pivot.x;
            dy = bone.m_pivot.y - parent.m_pivot.y;
            dz = bone.m_pivot.z - parent.m_pivot.z;
        }
        if (Length3(dx, dy, dz) < 1e-4f) { dx = 0; dy = 0; dz = -1; }
        Solver::NodeSpec node;
        node.bone = c.index;
        node.dir = {dx, dy, dz};
        const bool chained = c.ancestor >= 0 && specOf[size_t(c.ancestor)] >= 0;
        node.kind = (chained || hasChild) ? Solver::Kind::Cloth : Solver::Kind::Dangle;
        node.parent = chained ? specOf[size_t(c.ancestor)] : -1;
        node.frequency = std::max(5.0f, 10.0f - 0.9f * float(c.depth));   // longer chains swing slower at the tip
        specOf[c.index] = int(spec.nodes.size());
        spec.nodes.push_back(node);
    }
    return !spec.nodes.empty();
}

void Build(Group& group, const Solver::Spec& spec) {
    group.body = Solver::Body();
    group.bones.clear();
    Solver::Instantiate(spec, group.body, group.bones);
    group.motion.assign(group.bones.size(), Motion{});
    group.frames.assign(group.bones.size(), Solver::NodeFrame{});
    group.source = spec.source;
    group.built = !group.bones.empty();
}

void Release(Group& group, bool touchModel) {
    if (touchModel && group.model)
        for (uint32_t index : group.bones) group.model->SetBoneFlagsByIndex(index, 0, kProceduralFlag);
    group.body.Reset();
    group.bones.clear();
    group.motion.clear();
    group.frames.clear();
    group.built = false;
    group.model = nullptr;
    group.data = nullptr;
}

void Write(CM2Model* model, uint32_t index, const float r[9]) {
    // Row-vector convention: the matrix is the transpose of the column-vector rotation.
    C44Matrix matrix;
    matrix.Identity();
    matrix.a0 = r[0]; matrix.a1 = r[3]; matrix.a2 = r[6];
    matrix.b0 = r[1]; matrix.b1 = r[4]; matrix.b2 = r[7];
    matrix.c0 = r[2]; matrix.c1 = r[5]; matrix.c2 = r[8];
    model->SetBoneFlagsByIndex(index, kProceduralFlag, kProceduralFlag);
    model->SetBoneProceduralTransformByIndex(index, matrix);
}

void WriteRest(Group& group) {
    for (size_t i = 0; i < group.bones.size(); ++i) {
        float identity[9];
        group.body.Matrix(i, identity);
        Write(group.model, group.bones[i], identity);
    }
}

// Orientation rows of a bone's final matrix (scale removed). False for a missing or degenerate matrix.
bool BoneRows(CM2Model* model, uint32_t bone, float rows[9]) {
    if (!model->m_boneMatrices || bone >= model->GetBoneCount()) return false;
    const C44Matrix& m = model->m_boneMatrices[bone];
    const float raw[3][3] = {{m.a0, m.a1, m.a2}, {m.b0, m.b1, m.b2}, {m.c0, m.c1, m.c2}};
    for (int r = 0; r < 3; ++r) {
        const float length = Length3(raw[r][0], raw[r][1], raw[r][2]);
        if (!(length > 1e-6f) || !std::isfinite(length)) return false;
        for (int c = 0; c < 3; ++c) rows[r * 3 + c] = raw[r][c] / length;
    }
    return true;
}

// Rotation vector of R_now * R_before^T for two row sets (column-vector rotations are the transposes of the rows).
void DeltaRotation(const float now[9], const float before[9], float out[3]) {
    // D = Rnow * Rbefore^T with R = rows^T: D[i][j] = sum_k now[k][i] * before[k][j]  (rows indexed [k][col])
    float d[3][3];
    for (int i = 0; i < 3; ++i)
        for (int j = 0; j < 3; ++j) {
            float sum = 0;
            for (int k = 0; k < 3; ++k) sum += now[k * 3 + i] * before[k * 3 + j];
            d[i][j] = sum;
        }
    const float vx = d[2][1] - d[1][2], vy = d[0][2] - d[2][0], vz = d[1][0] - d[0][1];
    const float s = 0.5f * Length3(vx, vy, vz);
    const float c = std::min(1.0f, std::max(-1.0f, 0.5f * (d[0][0] + d[1][1] + d[2][2] - 1.0f)));
    const float angle = std::atan2(s, c);
    const float scale = s > 1e-6f ? angle / (2.0f * s) : 0.5f;   // small-angle limit: vee(D - D^T) / 2
    out[0] = vx * scale; out[1] = vy * scale; out[2] = vz * scale;
}

struct Frame {                  // the wearer's motion this frame, in the model frame
    Solver::Input input;
    float dt = 0;
    float strength = 1;
};

// Fills each node's parent-frame sample (gravity tilt, angular velocity and acceleration) from the model's bone matrices.
void SampleFrames(Group& group, const Frame& frame) {
    const M2Data* data = group.model->m_shared->m_data;
    for (size_t i = 0; i < group.bones.size(); ++i) {
        Solver::NodeFrame& out = group.frames[i];
        Motion& motion = group.motion[i];
        out.valid = false;
        const uint32_t bone = group.bones[i];
        if (bone >= data->m_bones.m_count) continue;
        float rows[9];
        if (!BoneRows(group.model, data->m_bones[bone].m_parentIndex, rows)) { motion.valid = false; continue; }
        const Solver::Vec3 g = frame.input.gravity;
        // model -> parent frame: component k = row k . vector
        auto local = [&](const float v[3], float to[3]) {
            for (int k = 0; k < 3; ++k) to[k] = rows[k * 3] * v[0] + rows[k * 3 + 1] * v[1] + rows[k * 3 + 2] * v[2];
        };
        const float gm[3] = {g.x, g.y, g.z};
        float gl[3];
        local(gm, gl);
        out.gravity = {gl[0], gl[1], gl[2]};
        if (motion.valid && frame.dt > 0) {
            float omegaModel[3], omegaLocal[3];
            DeltaRotation(rows, motion.rows, omegaModel);
            for (float& v : omegaModel) v /= frame.dt;
            const float rate = Length3(omegaModel[0], omegaModel[1], omegaModel[2]);
            if (rate > kMaxAngularRate)
                for (float& v : omegaModel) v *= kMaxAngularRate / rate;
            local(omegaModel, omegaLocal);
            const float blend = std::min(1.0f, frame.dt * 20.0f);
            for (int k = 0; k < 3; ++k) {
                const float alpha = (omegaLocal[k] - motion.omega[k]) / frame.dt;
                motion.alpha[k] += (alpha - motion.alpha[k]) * blend;
                motion.omega[k] = omegaLocal[k];
            }
            out.angularVelocity = {motion.omega[0] * frame.strength, motion.omega[1] * frame.strength, motion.omega[2] * frame.strength};
            out.angularAcceleration = {motion.alpha[0] * frame.strength, motion.alpha[1] * frame.strength, motion.alpha[2] * frame.strength};
            out.valid = true;
        }
        std::copy(rows, rows + 9, motion.rows);
        motion.valid = true;
    }
}

// A helmet or a shoulder is a rigid fitting. Only authored data may move parts of it (a plume, a tassel, a hanging chain),
// and never the piece's own root bone: dropping every node on a bone with no parent keeps the piece locked to its
// attachment while a node that hangs off it keeps its authored joint. The guesswork heuristic never runs on these.
void DropRootNodes(Solver::Spec& spec, const M2Data* data) {
    const uint32_t count = data->m_bones.m_count;
    std::vector<int> remap(spec.nodes.size(), -1);
    std::vector<Solver::NodeSpec> kept;
    for (size_t i = 0; i < spec.nodes.size(); ++i) {
        const Solver::NodeSpec& node = spec.nodes[i];
        if (node.bone < count && data->m_bones[node.bone].m_parentIndex >= count) continue;   // the piece's primary bone
        Solver::NodeSpec copy = node;
        copy.parent = node.parent >= 0 && size_t(node.parent) < remap.size() ? remap[size_t(node.parent)] : -1;
        remap[i] = int(kept.size());
        kept.push_back(copy);
    }
    spec.nodes = std::move(kept);
}

// M2 attachment ids: 11 helm (never simulated at all), 5 right shoulder and 6 left shoulder (rigid unless authored).
constexpr uint32_t kHelmAttachment = 11;
bool RigidAttachment(uint32_t id) { return id == kHelmAttachment || id == 5 || id == 6; }

// Picks the nodes for a group the first time it is usable: authored .phys data when present, else the heuristic.
void Prepare(Group& group, CM2Model* model, bool isChild, bool rigidPart = false) {
    group.model = model;
    group.data = model->m_shared->m_data;
    const uint32_t boneCount = model->m_shared->m_data->m_bones.m_count;
    Solver::Spec spec;
    if (isChild && OptionCVars::Bool(s_authored, true) && Retail::Armor::ParseRetailName(model->m_shared->m_fileName, &group.fileId)) {
        group.wantsPhys = true;
        const PhysEntry entry = Loader().Get(group.fileId);
        if (entry.state == PhysState::Pending) return;   // decide when the lookup finishes; nothing moves meanwhile
        group.wantsPhys = false;
        if (entry.state == PhysState::Ready && Solver::BuildFromPhys(*entry.file, boneCount, spec)) {
            if (rigidPart) {
                DropRootNodes(spec, model->m_shared->m_data);
                if (spec.nodes.empty()) return;   // nothing authored hangs off the rigid piece
            }
            Build(group, spec);
            if (OptionCVars::Bool(s_debug, false))
                Log("%s: %zu authored nodes from %s (%u anchors, %u colliders)", model->m_shared->m_fileName, group.bones.size(),
                    entry.note.c_str(), spec.anchors, spec.colliders);
            return;
        }
        if (OptionCVars::Bool(s_debug, false)) Log("%s: no authored physics (%s); heuristic", model->m_shared->m_fileName, entry.note.c_str());
    }
    if (rigidPart) return;   // never guess on a helmet or shoulder
    if (HeuristicSpec(group, model, spec)) {
        Build(group, spec);
        if (OptionCVars::Bool(s_debug, false))
            Log("%s: %zu heuristic nodes", model->m_shared->m_fileName, group.bones.size());
    }
}

bool Usable(CM2Model* model) {
    return model && (model->m_fFlags & 1) && model->m_shared && model->m_shared->m_data && model->m_bones &&
           model->m_boneMatrices;
}

void StepGroup(Group& group, const Frame& frame) {
    SampleFrames(group, frame);
    group.body.Step(frame.input, group.frames);
    for (size_t i = 0; i < group.bones.size(); ++i) {
        float matrix[9];
        group.body.Matrix(i, matrix);
        Write(group.model, group.bones[i], matrix);
    }
}

void ResetGroup(Group& group) {
    group.body.Reset();
    for (Motion& m : group.motion) m = Motion{};
    if (group.model) WriteRest(group);
}

}  // namespace

void RegisterCVars() {
    s_enabled = OptionCVars::Register("armorPhysics", "Retail armor: capes, tabards, belts and chains move with the wearer and the wind", "1");
    s_strength = OptionCVars::Register("armorPhysicsStrength", "Retail armor physics: scales every response (0..2)", "1");
    s_windScale = OptionCVars::Register("armorPhysicsWind", "Retail armor physics: scales the wind's share (0..3)", "1");
    s_max = OptionCVars::Register("armorPhysicsMax", "Retail armor physics: most wearers simulated per frame (1..256)", "32");
    s_debug = OptionCVars::Register("armorPhysicsDebug", "Retail armor physics: 1 logs bone counts and angles to Logs/RetailArmorPhysics.log", "0");
    s_authored = OptionCVars::Register("armorPhysicsAuthored", "Retail armor physics: 1 uses Blizzard's .phys joints and limits for attached items, 0 forces the heuristic", "1");
}

void SetWindProvider(WindProvider provider) { s_wind = provider; }

void Tick(CCharacterComponent* character, CM2Model* wearerModel, bool hasArmor, uint32_t nowMs) {
    if (nowMs != s_frameMs) { s_frameMs = nowMs; s_simulatedThisFrame = 0; }
    const auto found = s_wearers.find(character);
    const bool usable = Usable(wearerModel) && hasArmor && OptionCVars::Bool(s_enabled, true);
    if (!usable) {
        if (found != s_wearers.end()) {
            // The models that held the bones may already be gone (swap): only touch the ones still attached.
            Wearer& w = found->second;
            const bool same = w.model == wearerModel && wearerModel != nullptr;
            Release(w.body, same);
            if (same)
                for (CM2Model* child = wearerModel->m_attachList; child; child = child->m_attachNext) {
                    const auto g = w.children.find(child);
                    if (g != w.children.end()) Release(g->second, true);
                }
            s_wearers.erase(found);
        }
        return;
    }
    Wearer& wearer = s_wearers[character];
    if (wearer.model != wearerModel || wearer.body.data != wearerModel->m_shared->m_data) {
        // A new model or a different rig: the old ones are gone or re-created, never dereference them.
        wearer.children.clear();
        Release(wearer.body, false);
        wearer.body.rigRejected = false;
        wearer.model = wearerModel;
        wearer.haveHistory = false;
        Prepare(wearer.body, wearerModel, false);
    }

    // Attached item models with a rig of their own (shoulders, belts, helms): one group each, found per frame so
    // equipment changes are picked up and gone models are dropped.
    std::unordered_set<CM2Model*> present;
    size_t seen = 0;
    for (CM2Model* child = wearerModel->m_attachList; child && seen < kMaxChildren; child = child->m_attachNext, ++seen) {
        if (child->m_attachmentId == kCollectionSocket || !Usable(child)) continue;
        // A helm is never registered: it follows the head bone and its own M2 animations, authored .phys or not (Retail
        // does the same). Not putting it in `wearer.children` also drops any group left from before it was equipped.
        if (child->m_attachmentId == kHelmAttachment) continue;
        if (!Retail::Armor::IsRetailName(child->m_shared->m_fileName)) continue;
        present.insert(child);
        Group& group = wearer.children[child];
        if (group.model != child || group.data != child->m_shared->m_data) {
            Release(group, false);
            Prepare(group, child, true, RigidAttachment(child->m_attachmentId));
        } else if (group.wantsPhys) {
            Prepare(group, child, true, RigidAttachment(child->m_attachmentId));   // the lookup may have finished
        }
    }
    for (auto it = wearer.children.begin(); it != wearer.children.end();)
        it = present.count(it->first) ? std::next(it) : wearer.children.erase(it);   // model gone: never touch it

    if (wearer.body.bones.empty() && std::none_of(wearer.children.begin(), wearer.children.end(),
                                                  [](const auto& g) { return !g.second.bones.empty(); })) return;
    if (s_simulatedThisFrame >= uint32_t(OptionCVars::Int(s_max, 1, 256, 32))) return;
    ++s_simulatedThisFrame;

    const C44Matrix& world = wearerModel->m_worldTransform;
    const float position[3] = {world.d0, world.d1, world.d2};
    const float dt = wearer.haveHistory ? float(nowMs - wearer.lastMs) * 0.001f : 0.0f;
    float jump = 0;
    if (wearer.haveHistory)
        jump = Length3(position[0] - wearer.position[0], position[1] - wearer.position[1], position[2] - wearer.position[2]);
    auto groups = [&](auto&& fn) {
        if (wearer.body.built) fn(wearer.body);
        for (auto& entry : wearer.children) if (entry.second.built) fn(entry.second);
    };
    if (!wearer.haveHistory || !(dt > 0.0f) || dt > kLongFrame || jump > kTeleportDistance || !std::isfinite(jump)) {
        // first frame, a stall or a teleport: no usable velocity, restart from rest
        std::fill(wearer.velocity, wearer.velocity + 3, 0.0f);
        std::fill(wearer.acceleration, wearer.acceleration + 3, 0.0f);
        std::copy(position, position + 3, wearer.position);
        wearer.lastMs = nowMs;
        wearer.haveHistory = true;
        groups([](Group& g) { ResetGroup(g); });
        return;
    }

    // wearer motion in the world frame, then acceleration with a short low-pass (placement updates are steppy)
    float velocity[3], rawAcceleration[3];
    for (int k = 0; k < 3; ++k) {
        velocity[k] = (position[k] - wearer.position[k]) / dt;
        rawAcceleration[k] = (velocity[k] - wearer.velocity[k]) / dt;
    }
    const float magnitude = Length3(rawAcceleration[0], rawAcceleration[1], rawAcceleration[2]);
    if (magnitude > kMaxAcceleration)
        for (float& v : rawAcceleration) v *= kMaxAcceleration / magnitude;
    const float blend = std::min(1.0f, dt * 20.0f);
    for (int k = 0; k < 3; ++k) {
        wearer.acceleration[k] += (rawAcceleration[k] - wearer.acceleration[k]) * blend;
        wearer.velocity[k] = velocity[k];
        wearer.position[k] = position[k];
    }
    wearer.lastMs = nowMs;

    float wind[3] = {0, 0, 0};
    if (s_wind && !s_wind(position, wind)) std::fill(wind, wind + 3, 0.0f);
    const float windScale = OptionCVars::Float(s_windScale, 0.0f, 3.0f, 1.0f);
    const float strength = OptionCVars::Float(s_strength, 0.0f, 2.0f, 1.0f);

    // world -> model frame: the rows of the placement matrix are the model's axes in the world (scale removed)
    const float rows[3][3] = {{world.a0, world.a1, world.a2}, {world.b0, world.b1, world.b2}, {world.c0, world.c1, world.c2}};
    auto toModel = [&](const float v[3]) {
        Solver::Vec3 out;
        float* components[3] = {&out.x, &out.y, &out.z};
        for (int axis = 0; axis < 3; ++axis) {
            const float length = Length3(rows[axis][0], rows[axis][1], rows[axis][2]);
            *components[axis] = length > 1e-6f
                ? (v[0] * rows[axis][0] + v[1] * rows[axis][1] + v[2] * rows[axis][2]) / length : 0.0f;
        }
        return out;
    };
    const float down[3] = {0, 0, -kYardsPerSecondSq * strength};
    const float relativeWind[3] = {(wind[0] * windScale - velocity[0]) * strength, (wind[1] * windScale - velocity[1]) * strength,
                                   (wind[2] * windScale - velocity[2]) * strength};
    const float scaledAcceleration[3] = {wearer.acceleration[0] * strength, wearer.acceleration[1] * strength,
                                         wearer.acceleration[2] * strength};
    Frame frame;
    frame.dt = dt;
    frame.strength = strength;
    frame.input.dt = dt;
    frame.input.acceleration = toModel(scaledAcceleration);
    frame.input.gravity = toModel(down);
    frame.input.wind = toModel(relativeWind);

    float peak = 0;
    size_t nodes = 0;
    groups([&](Group& g) {
        StepGroup(g, frame);
        peak = std::max(peak, g.body.PeakAngle());
        nodes += g.bones.size();
    });

    if (OptionCVars::Bool(s_debug, false) && nowMs - wearer.lastLogMs >= 1000) {
        wearer.lastLogMs = nowMs;
        Log("character %p: %zu nodes (body %zu %s, %zu item rigs), peak %.3f rad, wind (%.1f %.1f %.1f), accel %.1f, dt %.4f",
            static_cast<void*>(character), nodes, wearer.body.bones.size(), wearer.body.source.c_str(), wearer.children.size(), peak,
            wind[0], wind[1], wind[2], Length3(wearer.acceleration[0], wearer.acceleration[1], wearer.acceleration[2]), dt);
    }
}

void Forget(CCharacterComponent* character) {
    // The component is being freed together with its model: never touch the model here.
    s_wearers.erase(character);
}

void ForgetAll() {
    s_wearers.clear();
    Loader().Clear();
}

uint32_t ActiveCount() {
    uint32_t count = 0;
    for (const auto& entry : s_wearers) {
        bool any = !entry.second.body.bones.empty();
        for (const auto& child : entry.second.children) any = any || !child.second.bones.empty();
        count += any ? 1 : 0;
    }
    return count;
}

}  // namespace RetailArmorPhysics

#else

namespace RetailArmorPhysics {
void RegisterCVars() {}
void SetWindProvider(WindProvider) {}
void Tick(CCharacterComponent*, CM2Model*, bool, uint32_t) {}
void Forget(CCharacterComponent*) {}
void ForgetAll() {}
uint32_t ActiveCount() { return 0; }
}  // namespace RetailArmorPhysics

#endif
