#ifndef ENGINE_SOURCE_RETAIL_PHYSFILE_H
#define ENGINE_SOURCE_RETAIL_PHYSFILE_H

// Reader for Retail ".phys" files (model physics, referenced by an M2's PFID chunk; the resolver already downloads
// them as FileKind::Physics). Not in the original binary, and Blizzard publishes no layout: what is decoded here was
// read out of the 94 files present in the pinned CDN build (88d5e8ef...), and every claim below was checked against
// all of them (versions 4 and 5: 89 files).
//
// A file is a sequence of chunks, `char tag[4]` stored reversed (the bytes "SYHP" are chunk PHYS) then `uint32 size`.
//   PHYS  u16 version                      PHYT  u32 type
//   BDY4  48-byte bodies:  u16 dynamic, u16 bone, vec3 rest position, u32 firstShape, u32 shapeCount,
//                          5 floats kept raw (see Body::tuning), u32 shapeBase
//   SHP2  32-byte shapes:  u16 kind (1 capsule, 2 sphere, 3 polytope), u16 index into that kind's table (CAPS, SPHS,
//                          the PLYT blobs), u32 0, float friction, float restitution, float density (kg/m3: 8769 for a
//                          metal buckle), u32 0, float 1.0, u32 unlabelled (kept out of the model)
//   CAPS  28-byte capsules: vec3 a, vec3 b, float radius          SPHS  16-byte spheres: vec3 centre, float radius
//   JOIN  16-byte joint table: u32 bodyA, u32 bodyB, u32 0, u16 type, u16 index into that type's chunk
//         type 1 = SHOJ (spherical), 2 = WLJ2 (weld), 3 = REVJ (revolute); counts per type match the chunks exactly
//   SHOJ  116 bytes: two frames (3x3 + vec3 each), then float twistMin, twistMax, swing (degrees), float friction, u32 flag
//   REVJ  112 bytes: two frames, then float min, max (degrees), float friction, u32 limited
//   WLJ2  112 bytes: two frames, 16 further bytes (weld, not simulated here)
// Older files (versions 0..3: BODY/SHAP/WELJ) and anything that fails a size or range check are reported as
// unsupported and the caller keeps its heuristic. Fields whose meaning was not established (Body::tuning) are carried
// raw and never turned into simulation parameters.

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace Retail::Phys {

enum class ShapeKind : uint16_t { Capsule = 1, Sphere = 2, Polytope = 3 };
enum class JointKind : uint16_t { Spherical = 1, Weld = 2, Revolute = 3 };

struct Shape {
    ShapeKind kind = ShapeKind::Capsule;
    float friction = 0, restitution = 0, density = 0;
    uint32_t dataIndex = 0;
};
struct Capsule { float a[3]{}, b[3]{}, radius = 0; };
struct Sphere { float centre[3]{}, radius = 0; };

struct Body {
    bool dynamic = false;          // false: fixed to its bone (an anchor)
    uint16_t bone = 0;             // M2 bone index in the model the file belongs to
    float position[3]{};           // rest position, model space
    uint32_t firstShape = 0, shapeCount = 0;
    float tuning[5]{};             // unlabelled authored floats (0.5-1 / 1 or 10 / 0-5 / 0-15 / 0.01-0.9), raw
};

struct Frame { float rotation[9]{}; float position[3]{}; };   // row-major 3x3 then translation

struct Joint {
    JointKind kind = JointKind::Spherical;
    uint32_t bodyA = 0, bodyB = 0; // A is the parent body, B the child
    Frame frameA, frameB;
    // degrees. Spherical: twistMin/twistMax and swing. Revolute: min/max only (limited says whether they apply).
    float twistMin = 0, twistMax = 0, swing = 0, friction = 0;
    bool limited = false;
};

struct File {
    uint16_t version = 0;
    uint32_t type = 0;
    std::vector<Body> bodies;
    std::vector<Shape> shapes;
    std::vector<Capsule> capsules;
    std::vector<Sphere> spheres;
    std::vector<Joint> joints;
    size_t polytopeBytes = 0;      // PLYT is counted, not decoded (no use for it without mesh collision)
    uint32_t unsupportedChunks = 0;
};

// False with `error` set for anything but a well-formed version 4/5 file. Never reads outside [data, data + size).
bool Parse(const uint8_t* data, size_t size, File& out, std::string& error);
inline bool Parse(const std::vector<uint8_t>& bytes, File& out, std::string& error) {
    return Parse(bytes.data(), bytes.size(), out, error);
}

// Index of the body that is the parent of `body` through the joint table (the joint whose bodyB it is), or -1.
int ParentBody(const File& file, uint32_t body);

}  // namespace Retail::Phys

#endif
