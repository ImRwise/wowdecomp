#ifndef ENGINE_SOURCE_RETAIL_ARMORPHYSICSSPEC_H
#define ENGINE_SOURCE_RETAIL_ARMORPHYSICSSPEC_H

// A neutral description of "which bones move and how", filled either from a Retail .phys file (BuildFromPhys) or by
// the heuristic bone classifier / the ASECM3 bridge, and turned into solver nodes by Instantiate. Keeping this layer
// separate means the solver never sees file formats and every source takes the same, tested path into it.

#include <cstdint>
#include <string>
#include <vector>

#include "Engine/Source/Retail/ArmorPhysics.h"
#include "Engine/Source/Retail/PhysFile.h"

namespace Retail::ArmorPhysics {

struct NodeSpec {
    uint32_t bone = 0;             // bone index in the model whose bones are driven
    int parent = -1;               // index of an earlier NodeSpec, or -1
    Kind kind = Kind::Cloth;
    Vec3 dir{0, 0, -1};            // rest direction, pivot to tip, model space (need not be unit)
    Vec3 hinge{0, 1, 0};           // Rigid only
    float hingeMin = -0.6f, hingeMax = 0.6f;   // radians
    float maxAngle = -1.0f;        // radians; < 0 keeps the solver default
    float frequency = 0.0f;        // rad/s; 0 keeps the solver default
    float follow = -1.0f;          // share of the parent angle a chain bone inherits; < 0 keeps the default
};

struct Spec {
    std::vector<NodeSpec> nodes;
    uint32_t colliders = 0;        // capsules/spheres the authored data carries (reported, not collided against)
    uint32_t anchors = 0;          // fixed bodies that only parent the simulated ones
    const char* source = "";       // "phys", "heuristic", "m3"
};

// Builds a spec from authored data. Bodies are dynamic or anchors; each dynamic body's parent is found through the
// joint table (skipping anchors); its direction is the vector between the authored rest positions of parent and body;
// revolute joints become hinges with their authored degree limits, spherical joints get a cone of their swing/twist
// limit when `limited`. Bodies whose bone is >= `boneCount` are dropped (the file does not belong to this model).
// Returns false when nothing is left to simulate.
bool BuildFromPhys(const Retail::Phys::File& file, uint32_t boneCount, Spec& out);

// Adds the spec's nodes to `body` (parents first), returning how many were added. `nodeBone[i]` receives the bone of
// the i-th node added to `body` (appended).
size_t Instantiate(const Spec& spec, Body& body, std::vector<uint32_t>& nodeBone);

}  // namespace Retail::ArmorPhysics

#endif
