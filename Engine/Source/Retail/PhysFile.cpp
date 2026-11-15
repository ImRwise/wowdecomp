#include "Engine/Source/Retail/PhysFile.h"

#include <cmath>
#include <cstring>

namespace Retail::Phys {

namespace {

struct Reader {
    const uint8_t* p;
    size_t n;
    uint32_t U32(size_t at) const { uint32_t v; std::memcpy(&v, p + at, 4); return v; }
    uint16_t U16(size_t at) const { uint16_t v; std::memcpy(&v, p + at, 2); return v; }
    float F32(size_t at) const { float v; std::memcpy(&v, p + at, 4); return v; }
};

bool Fail(std::string& error, const char* message) { error = message; return false; }

bool FiniteRange(float v, float limit) { return std::isfinite(v) && std::fabs(v) <= limit; }

void ReadFrame(const Reader& r, size_t at, Frame& frame) {
    for (int i = 0; i < 9; ++i) frame.rotation[i] = r.F32(at + size_t(i) * 4);
    for (int i = 0; i < 3; ++i) frame.position[i] = r.F32(at + 36 + size_t(i) * 4);
}
bool FrameSane(const Frame& frame) {
    for (float v : frame.rotation) if (!FiniteRange(v, 100.0f)) return false;
    for (float v : frame.position) if (!FiniteRange(v, 10000.0f)) return false;
    return true;
}

}  // namespace

bool Parse(const uint8_t* data, size_t size, File& out, std::string& error) {
    out = {};
    error.clear();
    if (!data || size < 10) return Fail(error, "phys file too small");

    struct Chunk { size_t at, size; };
    Chunk bdy4{}, shp2{}, caps{}, sphs{}, join{}, shoj{}, revj{}, wlj2{};
    bool havePhys = false;
    for (size_t at = 0; at < size;) {
        if (size - at < 8) return Fail(error, "truncated chunk header");
        char tag[5] = {char(data[at + 3]), char(data[at + 2]), char(data[at + 1]), char(data[at]), 0};   // stored reversed
        uint32_t chunkSize;
        std::memcpy(&chunkSize, data + at + 4, 4);
        if (chunkSize > size - at - 8) return Fail(error, "chunk overruns file");
        const Chunk chunk{at + 8, chunkSize};
        if (!std::strcmp(tag, "PHYS")) {
            if (chunkSize < 2 || havePhys) return Fail(error, "bad PHYS chunk");
            std::memcpy(&out.version, data + chunk.at, 2);
            havePhys = true;
        } else if (!std::strcmp(tag, "PHYT")) {
            if (chunkSize >= 4) std::memcpy(&out.type, data + chunk.at, 4);
        } else if (!std::strcmp(tag, "BDY4")) bdy4 = chunk;
        else if (!std::strcmp(tag, "SHP2")) shp2 = chunk;
        else if (!std::strcmp(tag, "CAPS")) caps = chunk;
        else if (!std::strcmp(tag, "SPHS")) sphs = chunk;
        else if (!std::strcmp(tag, "JOIN")) join = chunk;
        else if (!std::strcmp(tag, "SHOJ")) shoj = chunk;
        else if (!std::strcmp(tag, "REVJ")) revj = chunk;
        else if (!std::strcmp(tag, "WLJ2")) wlj2 = chunk;
        else if (!std::strcmp(tag, "PLYT")) out.polytopeBytes += chunkSize;
        else ++out.unsupportedChunks;
        at = chunk.at + chunkSize;
    }
    if (!havePhys) return Fail(error, "no PHYS chunk");
    if (out.version != 4 && out.version != 5) return Fail(error, "unsupported phys version");
    if (!bdy4.size) return Fail(error, "no bodies");
    if (bdy4.size % 48 || shp2.size % 32 || caps.size % 28 || sphs.size % 16 || join.size % 16 || shoj.size % 116 ||
        revj.size % 112 || wlj2.size % 112) return Fail(error, "chunk size is not a whole number of records");

    const Reader r{data, size};
    const size_t shapeCount = shp2.size / 32, capsuleCount = caps.size / 28, sphereCount = sphs.size / 16;
    out.shapes.resize(shapeCount);
    for (size_t i = 0; i < shapeCount; ++i) {
        const size_t at = shp2.at + i * 32;
        const uint16_t kind = r.U16(at);
        if (kind != 1 && kind != 2 && kind != 3) return Fail(error, "unknown shape kind");
        Shape& s = out.shapes[i];
        s.kind = ShapeKind(kind);
        s.dataIndex = r.U16(at + 2);
        s.friction = r.F32(at + 8);
        s.restitution = r.F32(at + 12);
        s.density = r.F32(at + 16);
        if (!FiniteRange(s.friction, 100.0f) || !FiniteRange(s.restitution, 100.0f) || !FiniteRange(s.density, 1e7f))
            return Fail(error, "shape values out of range");
        if (s.kind == ShapeKind::Capsule && s.dataIndex >= capsuleCount) return Fail(error, "capsule index out of range");
        if (s.kind == ShapeKind::Sphere && s.dataIndex >= sphereCount) return Fail(error, "sphere index out of range");
    }
    out.capsules.resize(capsuleCount);
    for (size_t i = 0; i < capsuleCount; ++i) {
        const size_t at = caps.at + i * 28;
        Capsule& c = out.capsules[i];
        for (int k = 0; k < 3; ++k) { c.a[k] = r.F32(at + size_t(k) * 4); c.b[k] = r.F32(at + 12 + size_t(k) * 4); }
        c.radius = r.F32(at + 24);
        for (float v : {c.a[0], c.a[1], c.a[2], c.b[0], c.b[1], c.b[2], c.radius})
            if (!FiniteRange(v, 1000.0f)) return Fail(error, "capsule values out of range");
    }
    out.spheres.resize(sphereCount);
    for (size_t i = 0; i < sphereCount; ++i) {
        const size_t at = sphs.at + i * 16;
        Sphere& s = out.spheres[i];
        for (int k = 0; k < 3; ++k) s.centre[k] = r.F32(at + size_t(k) * 4);
        s.radius = r.F32(at + 12);
        for (float v : {s.centre[0], s.centre[1], s.centre[2], s.radius})
            if (!FiniteRange(v, 1000.0f)) return Fail(error, "sphere values out of range");
    }

    const size_t bodyCount = bdy4.size / 48;
    out.bodies.resize(bodyCount);
    for (size_t i = 0; i < bodyCount; ++i) {
        const size_t at = bdy4.at + i * 48;
        Body& b = out.bodies[i];
        b.dynamic = r.U16(at) != 0;
        b.bone = r.U16(at + 2);
        for (int k = 0; k < 3; ++k) b.position[k] = r.F32(at + 4 + size_t(k) * 4);
        b.firstShape = r.U32(at + 16);
        b.shapeCount = r.U32(at + 20);
        for (int k = 0; k < 5; ++k) b.tuning[k] = r.F32(at + 24 + size_t(k) * 4);
        for (float v : b.position) if (!FiniteRange(v, 1000.0f)) return Fail(error, "body position out of range");
        for (float v : b.tuning) if (!FiniteRange(v, 1e6f)) return Fail(error, "body tuning out of range");
        if (b.shapeCount && (uint64_t(b.firstShape) + b.shapeCount > shapeCount)) return Fail(error, "body shape range out of range");
    }

    const size_t jointCount = join.size / 16;
    const size_t sizes[4] = {0, shoj.size / 116, wlj2.size / 112, revj.size / 112};
    const size_t offsets[4] = {0, shoj.at, wlj2.at, revj.at};
    out.joints.resize(jointCount);
    for (size_t i = 0; i < jointCount; ++i) {
        const size_t at = join.at + i * 16;
        Joint& j = out.joints[i];
        j.bodyA = r.U32(at);
        j.bodyB = r.U32(at + 4);
        const uint16_t type = r.U16(at + 12), index = r.U16(at + 14);
        if (j.bodyA >= bodyCount || j.bodyB >= bodyCount || j.bodyA == j.bodyB) return Fail(error, "joint endpoint out of range");
        if (type < 1 || type > 3 || index >= sizes[type]) return Fail(error, "joint record out of range");
        j.kind = JointKind(type);
        const size_t rec = offsets[type] + size_t(index) * (type == 1 ? 116 : 112);
        ReadFrame(r, rec, j.frameA);
        ReadFrame(r, rec + 48, j.frameB);
        if (!FrameSane(j.frameA) || !FrameSane(j.frameB)) return Fail(error, "joint frame out of range");
        if (type == 1) {
            j.twistMin = r.F32(rec + 96);
            j.twistMax = r.F32(rec + 100);
            j.swing = r.F32(rec + 104);
            j.friction = r.F32(rec + 108);
            j.limited = r.U32(rec + 112) != 0;
        } else if (type == 3) {
            j.twistMin = r.F32(rec + 96);
            j.twistMax = r.F32(rec + 100);
            j.friction = r.F32(rec + 104);
            j.limited = r.U32(rec + 108) != 0;
        }
        for (float v : {j.twistMin, j.twistMax, j.swing, j.friction})
            if (!FiniteRange(v, 360.0f * 4)) return Fail(error, "joint limits out of range");
    }
    return true;
}

int ParentBody(const File& file, uint32_t body) {
    for (const Joint& joint : file.joints)
        if (joint.bodyB == body) return int(joint.bodyA);
    return -1;
}

}  // namespace Retail::Phys
