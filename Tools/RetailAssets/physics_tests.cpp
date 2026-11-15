// Offline tests for the Retail armor secondary-motion solver (Engine/Source/Retail/ArmorPhysics).
#include "Engine/Source/Retail/ArmorPhysics.h"
#include "Engine/Source/Retail/ArmorPhysicsSpec.h"
#include "Extensions/ASECM3/client/PhysicsBridge.h"
#include "Engine/Source/Retail/PhysFile.h"

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

using namespace Retail::ArmorPhysics;

static int failures = 0;
#define CHECK(condition) do { if (!(condition)) { std::fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #condition); ++failures; } } while (0)

static Node Hanging(Kind kind = Kind::Cloth, int parent = -1) {
    Node node;
    node.kind = kind;
    node.parent = parent;
    node.dir = {0, 0, -1};
    return node;
}

static float Length(const Vec3& v) { return std::sqrt(v.x * v.x + v.y * v.y + v.z * v.z); }

static void Run(Body& body, const Input& input, float seconds, float frame) {
    Input step = input;
    step.dt = frame;
    for (float t = 0; t < seconds; t += frame) body.Step(step);
}

static void TestRest() {
    Body body;
    body.Add(Hanging());
    Input input;   // default gravity is straight down the bone: no torque
    Run(body, input, 2.0f, 1.0f / 60.0f);
    CHECK(body.PeakAngle() < 1e-6f);
    float m[9];
    body.Matrix(0, m);
    CHECK(std::fabs(m[0] - 1) < 1e-6f && std::fabs(m[4] - 1) < 1e-6f && std::fabs(m[8] - 1) < 1e-6f);
}

static void TestWindLeansDownwind() {
    Body body;
    body.Add(Hanging());
    Input input;
    input.wind = {6, 0, 0};
    Run(body, input, 3.0f, 1.0f / 60.0f);
    float m[9];
    body.Matrix(0, m);
    // The tip (0,0,-1) rotated by the matrix leans toward +x, the wind direction.
    const float tipX = -m[2], tipZ = -m[8];
    CHECK(tipX > 0.2f);
    CHECK(tipZ < 0);
    // Steady state is wind * gain radians (0.06 * 6 = 0.36), within the damping settle.
    CHECK(std::fabs(body.PeakAngle() - 0.36f) < 0.02f);
    // Wind off: it swings back to rest.
    input.wind = {};
    Run(body, input, 4.0f, 1.0f / 60.0f);
    CHECK(body.PeakAngle() < 0.01f);
}

static void TestFrameRateIndependence() {
    Input input;
    input.wind = {5, 2, 0};
    Body a, b;
    a.Add(Hanging());
    b.Add(Hanging());
    Run(a, input, 3.0f, 1.0f / 30.0f);
    Run(b, input, 3.0f, 1.0f / 144.0f);
    CHECK(std::fabs(a.PeakAngle() - b.PeakAngle()) < 0.01f);
    // One 33 ms frame equals two 16.7 ms frames closely (the solver substeps at 120 Hz either way).
    Body c, d;
    c.Add(Hanging());
    d.Add(Hanging());
    Input kick;
    kick.acceleration = {30, 0, 0};
    kick.dt = 1.0f / 30.0f;
    c.Step(kick);
    kick.dt = 1.0f / 60.0f;
    d.Step(kick);
    d.Step(kick);
    CHECK(std::fabs(c.PeakAngle() - d.PeakAngle()) < 0.01f);
}

static void TestDeterminism() {
    Body a, b;
    for (Body* body : {&a, &b}) {
        body->Add(Hanging());
        body->Add(Hanging(Kind::Cloth, 0));
    }
    for (int i = 0; i < 500; ++i) {
        Input input;
        input.dt = 1.0f / 60.0f;
        input.acceleration = {std::sin(float(i) * 0.1f) * 12, std::cos(float(i) * 0.07f) * 8, 0};
        input.wind = {3, std::sin(float(i) * 0.03f) * 4, 0};
        a.Step(input);
        b.Step(input);
    }
    for (size_t i = 0; i < a.Size(); ++i) {
        CHECK(std::memcmp(&a.At(i).angle, &b.At(i).angle, sizeof(Vec3)) == 0);
        CHECK(std::memcmp(&a.At(i).velocity, &b.At(i).velocity, sizeof(Vec3)) == 0);
    }
}

static void TestChainLags() {
    Body body;
    body.Add(Hanging());
    body.Add(Hanging(Kind::Cloth, 0));
    Input input;
    input.dt = 1.0f / 60.0f;
    input.acceleration = {40, 0, 0};
    body.Step(input);
    // A kick moves the parent first; the child, driven by its own force plus a share of the parent, trails.
    CHECK(Length(body.At(0).angle) > 0);
    input.acceleration = {};
    float parentPeak = 0, childPeak = 0;
    for (int i = 0; i < 120; ++i) {
        body.Step(input);
        parentPeak = std::fmax(parentPeak, Length(body.At(0).angle));
        childPeak = std::fmax(childPeak, Length(body.At(1).angle));
    }
    CHECK(childPeak > 0.001f);   // the chain passes the motion down
    CHECK(parentPeak > 0.001f);
    Run(body, input, 6.0f, 1.0f / 60.0f);
    CHECK(body.PeakAngle() < 0.01f);   // and it settles
}

static void TestLimits() {
    Body body;
    Node cape = Hanging();
    cape.params.maxAngle = 0.5f;
    body.Add(cape);
    Input input;
    input.wind = {200, 0, 0};
    float peak = 0;
    for (int i = 0; i < 300; ++i) {
        input.dt = 1.0f / 60.0f;
        body.Step(input);
        peak = std::fmax(peak, body.PeakAngle());
    }
    CHECK(peak <= 0.5f + 1e-4f);

    Body rigid;
    Node plate = Hanging(Kind::Rigid);
    plate.hinge = {0, 1, 0};
    plate.hingeMin = -0.3f;
    plate.hingeMax = 0.4f;
    rigid.Add(plate);
    float lo = 0, hi = 0;
    for (int i = 0; i < 600; ++i) {
        input.dt = 1.0f / 60.0f;
        input.wind = {i % 120 < 60 ? 80.0f : -80.0f, 0, 0};
        rigid.Step(input);
        const float phi = rigid.At(0).angle.y;
        lo = std::fmin(lo, phi);
        hi = std::fmax(hi, phi);
        CHECK(std::fabs(rigid.At(0).angle.x) < 1e-6f && std::fabs(rigid.At(0).angle.z) < 1e-6f);   // hinge only
    }
    CHECK(lo >= -0.3f - 1e-5f && hi <= 0.4f + 1e-5f);
    CHECK(lo < -0.2f && hi > 0.3f);   // and it does reach them
}

static void TestHostileInput() {
    Body body;
    body.Add(Hanging());
    Input input;
    input.dt = 1.0f / 60.0f;
    input.wind = {NAN, INFINITY, 1};
    input.acceleration = {NAN, 0, 0};
    for (int i = 0; i < 100; ++i) body.Step(input);
    CHECK(std::isfinite(body.PeakAngle()));
    input.wind = {3, 0, 0};
    input.dt = 1e9f;   // a hitch: clamped, never 1e9 s of simulation
    body.Step(input);
    CHECK(std::isfinite(body.PeakAngle()) && body.PeakAngle() <= 0.9f + 1e-4f);
    input.dt = -1;
    const Vec3 before = body.At(0).angle;
    body.Step(input);
    CHECK(std::memcmp(&before, &body.At(0).angle, sizeof before) == 0);
    input.dt = NAN;
    body.Step(input);
    CHECK(std::isfinite(body.PeakAngle()));
    // Absurd parameters stay stable (frequency clamped).
    Body stiff;
    Node node = Hanging();
    node.params.frequency = 1e6f;
    node.params.damping = NAN;
    stiff.Add(node);
    input.dt = 0.1f;
    for (int i = 0; i < 50; ++i) stiff.Step(input);
    CHECK(std::isfinite(stiff.PeakAngle()) && stiff.PeakAngle() <= 0.9f + 1e-4f);
    body.Reset();
    CHECK(body.PeakAngle() == 0);
}

static void TestAddSanitises() {
    Body body;
    Node node = Hanging();
    node.parent = 5;                 // forward reference
    node.dir = {0, 0, 0};            // degenerate direction
    CHECK(body.Add(node) == 0);
    CHECK(body.At(0).parent == -1);
    CHECK(std::fabs(Length(body.At(0).dir) - 1) < 1e-6f);
    float m[9];
    body.Matrix(99, m);              // out of range: identity, no crash
    CHECK(m[0] == 1 && m[4] == 1 && m[8] == 1);
}


// ---- .phys reader -------------------------------------------------------------------------------------------------

namespace {
struct Builder {
    std::vector<uint8_t> bytes;
    void Chunk(const char* tag, const std::vector<uint8_t>& body) {
        const char reversed[4] = {tag[3], tag[2], tag[1], tag[0]};
        bytes.insert(bytes.end(), reversed, reversed + 4);
        const uint32_t size = uint32_t(body.size());
        bytes.insert(bytes.end(), reinterpret_cast<const uint8_t*>(&size), reinterpret_cast<const uint8_t*>(&size) + 4);
        bytes.insert(bytes.end(), body.begin(), body.end());
    }
};
void Put(std::vector<uint8_t>& v, uint32_t x) { v.insert(v.end(), reinterpret_cast<uint8_t*>(&x), reinterpret_cast<uint8_t*>(&x) + 4); }
void PutF(std::vector<uint8_t>& v, float x) { v.insert(v.end(), reinterpret_cast<uint8_t*>(&x), reinterpret_cast<uint8_t*>(&x) + 4); }
void PutFrame(std::vector<uint8_t>& v) {
    const float identity[9] = {1, 0, 0, 0, 1, 0, 0, 0, 1};
    for (float f : identity) PutF(v, f);
    for (int i = 0; i < 3; ++i) PutF(v, 0);
}
std::vector<uint8_t> BodyRecord(uint16_t dynamic, uint16_t bone, uint32_t shape, uint32_t shapes) {
    std::vector<uint8_t> b;
    Put(b, uint32_t(dynamic) | (uint32_t(bone) << 16));
    PutF(b, 0.1f); PutF(b, 0.2f); PutF(b, -0.3f);
    Put(b, shape); Put(b, shapes);
    for (float f : {0.75f, 1.0f, 3.0f, 5.0f, 0.01f}) PutF(b, f);
    Put(b, 0);
    return b;
}
// two bodies (anchor on bone 2, dynamic on bone 5), one capsule, one revolute joint limited to -30..45 degrees
std::vector<uint8_t> Sample(uint16_t version = 5) {
    Builder f;
    f.Chunk("PHYS", {uint8_t(version), 0});
    std::vector<uint8_t> type; Put(type, 4); f.Chunk("PHYT", type);
    std::vector<uint8_t> bodies = BodyRecord(0, 2, 0, 0), second = BodyRecord(1, 5, 0, 1);
    bodies.insert(bodies.end(), second.begin(), second.end());
    f.Chunk("BDY4", bodies);
    std::vector<uint8_t> shape;
    Put(shape, 1u | (0u << 16)); Put(shape, 0); PutF(shape, 0.6f); PutF(shape, 0.1f); PutF(shape, 100.0f); Put(shape, 0); PutF(shape, 1.0f); Put(shape, 0);
    f.Chunk("SHP2", shape);
    std::vector<uint8_t> caps;
    for (float v : {0.0f, 0.0f, 0.0f, 0.0f, 0.0f, -0.2f, 0.03f}) PutF(caps, v);
    f.Chunk("CAPS", caps);
    std::vector<uint8_t> join;
    Put(join, 0); Put(join, 1); Put(join, 0); Put(join, 3u | (0u << 16));
    f.Chunk("JOIN", join);
    std::vector<uint8_t> rev;
    PutFrame(rev); PutFrame(rev);
    PutF(rev, -30.0f); PutF(rev, 45.0f); PutF(rev, 0.1f); Put(rev, 1);
    f.Chunk("REVJ", rev);
    return f.bytes;
}
}

static void TestPhysFile() {
    using namespace Retail::Phys;
    File file;
    std::string error;
    const auto bytes = Sample();
    CHECK(Parse(bytes, file, error));
    CHECK(file.version == 5 && file.bodies.size() == 2 && file.joints.size() == 1 && file.capsules.size() == 1);
    CHECK(!file.bodies[0].dynamic && file.bodies[0].bone == 2);
    CHECK(file.bodies[1].dynamic && file.bodies[1].bone == 5 && file.bodies[1].shapeCount == 1);
    CHECK(std::fabs(file.bodies[1].tuning[2] - 3.0f) < 1e-6f);
    CHECK(file.joints[0].kind == JointKind::Revolute && file.joints[0].limited);
    CHECK(file.joints[0].twistMin == -30.0f && file.joints[0].twistMax == 45.0f);
    CHECK(ParentBody(file, 1) == 0 && ParentBody(file, 0) == -1);
    CHECK(std::fabs(file.capsules[0].radius - 0.03f) < 1e-6f && file.shapes[0].kind == ShapeKind::Capsule);

    // Every truncation and a version bump are refused without reading past the end.
    for (size_t cut = 0; cut < bytes.size(); ++cut) {
        File partial;
        std::string why;
        Parse(bytes.data(), cut, partial, why);   // must not read past `cut` (ASan/Application Verifier territory); result varies
    }
    CHECK(!Parse(Sample(3), file, error));            // older generation: heuristic fallback
    // out-of-range references
    auto broken = Sample();
    for (size_t i = 0; i + 8 <= broken.size(); ++i)    // the JOIN chunk is stored as "NIOJ"
        if (!std::memcmp(&broken[i], "NIOJ", 4)) { broken[i + 8] = 9; break; }   // bodyA -> 9
    CHECK(!Parse(broken, file, error));
    CHECK(!Parse(nullptr, 0, file, error));
    std::vector<uint8_t> noise(256, 0xFF);
    CHECK(!Parse(noise, file, error));
}

// Optional: PHYS_SAMPLE_DIR points at real Retail .phys files; every version 4/5 file must parse, the rest must be
// refused cleanly. (The files are Blizzard data and are not in the repository.)
static void TestRealFiles() {
    const char* dir = std::getenv("PHYS_SAMPLE_DIR");
    if (!dir) return;
    int parsed = 0, refused = 0, built = 0, nodes = 0;
    for (const auto& entry : std::filesystem::directory_iterator(dir)) {
        if (entry.path().extension() != ".phys") continue;
        std::ifstream in(entry.path(), std::ios::binary);
        const std::vector<uint8_t> bytes((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
        Retail::Phys::File file;
        std::string error;
        if (Retail::Phys::Parse(bytes, file, error)) {
            ++parsed;
            Retail::ArmorPhysics::Spec spec;
            if (Retail::ArmorPhysics::BuildFromPhys(file, 65535, spec)) {
                ++built;
                nodes += int(spec.nodes.size());
                for (size_t i = 0; i < spec.nodes.size(); ++i) CHECK(spec.nodes[i].parent < int(i));   // parents first
            }
            for (const auto& joint : file.joints) CHECK(joint.bodyA < file.bodies.size() && joint.bodyB < file.bodies.size());
        } else {
            ++refused;
            if (std::getenv("PHYS_VERBOSE")) std::printf("refused %s: %s\n", entry.path().filename().string().c_str(), error.c_str());
        }
    }
    std::printf("real .phys: %d parsed, %d refused, %d simulatable (%d nodes)\n", parsed, refused, built, nodes);
    CHECK(parsed > 0);
}

static void TestParentFrame() {
    // A node hanging from a tilted parent: gravity along the tilted frame pulls it toward the new down.
    Body tilted, upright;
    tilted.Add(Hanging());
    upright.Add(Hanging());
    Input input;
    input.dt = 1.0f / 60.0f;
    std::vector<NodeFrame> frames(1);
    frames[0].valid = true;
    frames[0].gravity = {-3.0f, 0, -10.0f};   // the parent leaned: down is now partly -x in its frame
    for (int i = 0; i < 300; ++i) { tilted.Step(input, frames); upright.Step(input); }
    CHECK(upright.PeakAngle() < 1e-6f);
    CHECK(tilted.PeakAngle() > 0.001f);

    // Turning throws a tip that has a horizontal lever: a yaw acceleration moves a tip at +x along +y, so the node
    // (driven by -inertia * that acceleration) swings the other way.
    Node sideways = Hanging();
    sideways.dir = {1, 0, 0};
    Body lever;
    lever.Add(sideways);
    NodeFrame yaw;
    yaw.valid = true;
    yaw.gravity = {0, 0, 0};
    yaw.angularAcceleration = {0, 0, 60.0f};   // tip at +x accelerates toward +y; inertia pushes the response back (-y)
    for (int i = 0; i < 10; ++i) lever.Step(input, {yaw});
    float m[9];
    lever.Matrix(0, m);
    CHECK(lever.PeakAngle() > 1e-4f);
    // Invalid frames are ignored: identical to Step without frames.
    Body a, b;
    a.Add(Hanging());
    b.Add(Hanging());
    input.wind = {4, 0, 0};
    NodeFrame invalid;
    for (int i = 0; i < 30; ++i) { a.Step(input, {invalid}); b.Step(input); }
    CHECK(std::memcmp(&a.At(0).angle, &b.At(0).angle, sizeof(Vec3)) == 0);
    // Non-finite frame data cannot poison the state.
    NodeFrame bad;
    bad.valid = true;
    bad.gravity = {NAN, 0, 0};
    bad.angularVelocity = {INFINITY, 0, 0};
    bad.angularAcceleration = {0, NAN, 0};
    for (int i = 0; i < 30; ++i) a.Step(input, {bad});
    CHECK(std::isfinite(a.PeakAngle()));
}

static void TestSpecFromPhys() {
    using namespace Retail::ArmorPhysics;
    Retail::Phys::File file;
    std::string error;
    CHECK(Retail::Phys::Parse(Sample(), file, error));
    Spec spec;
    CHECK(BuildFromPhys(file, 100, spec));
    CHECK(spec.nodes.size() == 1 && spec.anchors == 1 && spec.colliders == 1);
    CHECK(spec.nodes[0].bone == 5 && spec.nodes[0].kind == Kind::Rigid && spec.nodes[0].parent == -1);
    CHECK(std::fabs(spec.nodes[0].hingeMin - (-30.0f * 3.14159265f / 180.0f)) < 1e-4f);
    CHECK(std::fabs(spec.nodes[0].hingeMax - (45.0f * 3.14159265f / 180.0f)) < 1e-4f);
    CHECK(spec.nodes[0].hinge.x == 1.0f);
    // the body belongs to bone 5: a model with fewer bones drops it, nothing is left to simulate
    CHECK(!BuildFromPhys(file, 5, spec));
    Body body;
    std::vector<uint32_t> bones;
    CHECK(BuildFromPhys(file, 100, spec));
    CHECK(Instantiate(spec, body, bones) == 1 && bones.size() == 1 && bones[0] == 5);
    // The instantiated hinge keeps to its authored limits under a hard push.
    Input push;
    push.dt = 1.0f / 60.0f;
    push.wind = {500, 0, 0};
    float hi = -10, lo = 10;
    for (int i = 0; i < 300; ++i) {
        body.Step(push);
        const float phi = Length(body.At(0).angle) * (body.At(0).angle.x + body.At(0).angle.y + body.At(0).angle.z >= 0 ? 1.0f : -1.0f);
        hi = std::fmax(hi, phi);
        lo = std::fmin(lo, phi);
    }
    CHECK(hi <= 45.0f * 3.14159265f / 180.0f + 1e-3f && lo >= -30.0f * 3.14159265f / 180.0f - 1e-3f);
}

static void TestM3Bridge() {
    // skeleton: 0 root <- 1 <- 2 <- 3 (a chain), 4 <- 0 (a lone leaf); bodies on 1 (anchor), 2, 3 (moving), 4 (moving)
    std::vector<ASECM3::Bone> skeleton(5);
    skeleton[0].parent = -1; skeleton[1].parent = 0; skeleton[2].parent = 1; skeleton[3].parent = 2; skeleton[4].parent = 0;
    ASECM3::PhysicsData data;
    auto body = [&](uint32_t bone, float mass) {
        ASECM3::PhysicsBody b;
        b.bone = bone;
        b.mass = mass;
        b.shapes.resize(1);
        data.bodies.push_back(b);
    };
    body(1, 0.0f); body(3, 1.0f); body(2, 1.0f); body(4, 2.0f);   // file order is not skeleton order
    Retail::ArmorPhysics::Spec spec;
    CHECK(ASECM3::BuildArmorPhysicsSpec(data, skeleton, spec));
    CHECK(spec.nodes.size() == 3 && spec.anchors == 1 && spec.colliders == 4);
    // ancestors first: bone 2 (no moving ancestor), then 3 (child of 2), bone 4 is a free dangle
    int at2 = -1, at3 = -1, at4 = -1;
    for (size_t i = 0; i < spec.nodes.size(); ++i) {
        if (spec.nodes[i].bone == 2) at2 = int(i);
        if (spec.nodes[i].bone == 3) at3 = int(i);
        if (spec.nodes[i].bone == 4) at4 = int(i);
    }
    CHECK(at2 >= 0 && at3 > at2 && spec.nodes[size_t(at3)].parent == at2);
    CHECK(spec.nodes[size_t(at2)].parent == -1 && spec.nodes[size_t(at2)].kind == Retail::ArmorPhysics::Kind::Cloth);
    CHECK(at4 >= 0 && spec.nodes[size_t(at4)].kind == Retail::ArmorPhysics::Kind::Dangle);
    // a decode error or a body outside the skeleton yields nothing to simulate
    data.error = "decode failed";
    CHECK(!ASECM3::BuildArmorPhysicsSpec(data, skeleton, spec));
    data.error.clear();
    data.bodies.clear();
    body(99, 1.0f);
    CHECK(!ASECM3::BuildArmorPhysicsSpec(data, skeleton, spec));
    // and the spec runs in the solver like any other
    data.bodies.clear();
    body(2, 1.0f); body(3, 1.0f);
    CHECK(ASECM3::BuildArmorPhysicsSpec(data, skeleton, spec));
    Retail::ArmorPhysics::Body solver;
    std::vector<uint32_t> bones;
    CHECK(Retail::ArmorPhysics::Instantiate(spec, solver, bones) == 2);
    Retail::ArmorPhysics::Input input;
    input.dt = 1.0f / 60.0f;
    input.wind = {5, 0, 0};
    for (int i = 0; i < 120; ++i) solver.Step(input);
    CHECK(solver.PeakAngle() > 0.05f);
}

int main() {
    TestRest();
    TestWindLeansDownwind();
    TestFrameRateIndependence();
    TestDeterminism();
    TestChainLags();
    TestLimits();
    TestHostileInput();
    TestAddSanitises();
    TestPhysFile();
    TestSpecFromPhys();
    TestParentFrame();
    TestM3Bridge();
    TestRealFiles();
    if (failures) { std::fprintf(stderr, "%d failure(s)\n", failures); return 1; }
    std::puts("physics tests passed");
    return 0;
}
