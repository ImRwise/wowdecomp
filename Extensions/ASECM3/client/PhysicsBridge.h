#pragma once
// Bridge from ASECM3's decoded M3 physics (PHRB bodies, joints) to the Retail armor physics description
// (Engine/Source/Retail/ArmorPhysicsSpec.h), so an M3 attachment's bone chains can use the same solver.
//
// Deliberately narrow, because ASECM3's own decoder says what is not established: "PHRB v2 does not specify the
// simulation type", the joint `kind`/`angle` units are undecoded, and cloth coefficients are retained but not
// interpreted. So this takes only what is firm: which bones carry a body, which of them move (mass > 0; a massless
// body is an anchor), and how they chain (nearest moving ancestor in the skeleton). Rest direction is straight down;
// joint limits are NOT applied (unit unknown), so every node gets the solver's default cone.
//
// Not yet wired to a live M3 model: the application point is SceneFrame::animationLocals ("before rigid or cloth
// feedback"), which GxScene does not drive. Header-only so it can be unit-tested without the Gx client.
#include <algorithm>
#include <vector>

#include "ASECM3/Document.h"
#include "ASECM3/Physics.h"
#include "Engine/Source/Retail/ArmorPhysicsSpec.h"

namespace ASECM3 {

inline bool BuildArmorPhysicsSpec(const PhysicsData& physics, const std::vector<Bone>& skeleton,
                                  Retail::ArmorPhysics::Spec& out) {
    out = {};
    out.source = "m3";
    if (!physics.error.empty()) return false;
    const size_t bones = skeleton.size();
    std::vector<int> bodyOfBone(bones, -1);
    for (size_t i = 0; i < physics.bodies.size(); ++i) {
        const uint32_t bone = physics.bodies[i].bone;
        if (bone < bones && bodyOfBone[bone] < 0) bodyOfBone[bone] = int(i);
    }
    auto moving = [&](uint32_t bone) { return bodyOfBone[bone] >= 0 && physics.bodies[size_t(bodyOfBone[bone])].mass > 0.0f; };
    for (const PhysicsBody& body : physics.bodies)
        out.colliders += uint32_t(body.shapes.size());

    // order: ancestors first (skeleton parents can come after children in a file, so sort by depth)
    struct Entry { uint32_t bone; int depth; };
    std::vector<Entry> order;
    for (uint32_t bone = 0; bone < bones; ++bone) {
        if (bodyOfBone[bone] < 0) continue;
        if (!moving(bone)) { ++out.anchors; continue; }
        int depth = 0;
        int32_t walk = skeleton[bone].parent;
        for (size_t guard = 0; walk >= 0 && size_t(walk) < bones && guard < bones; ++guard) {
            if (moving(uint32_t(walk))) ++depth;
            walk = skeleton[size_t(walk)].parent;
        }
        order.push_back({bone, depth});
    }
    std::stable_sort(order.begin(), order.end(), [](const Entry& a, const Entry& b) { return a.depth < b.depth; });
    std::vector<int> specOf(bones, -1);
    for (const Entry& entry : order) {
        Retail::ArmorPhysics::NodeSpec node;
        node.bone = entry.bone;
        int32_t walk = skeleton[entry.bone].parent;
        for (size_t guard = 0; walk >= 0 && size_t(walk) < bones && guard < bones; ++guard) {
            if (specOf[size_t(walk)] >= 0) { node.parent = specOf[size_t(walk)]; break; }
            walk = skeleton[size_t(walk)].parent;
        }
        node.kind = Retail::ArmorPhysics::Kind::Cloth;
        specOf[entry.bone] = int(out.nodes.size());
        out.nodes.push_back(node);
    }
    std::vector<uint8_t> hasChild(out.nodes.size(), 0);
    for (const auto& n : out.nodes) if (n.parent >= 0) hasChild[size_t(n.parent)] = 1;
    for (size_t i = 0; i < out.nodes.size(); ++i)
        if (out.nodes[i].parent < 0 && !hasChild[i]) out.nodes[i].kind = Retail::ArmorPhysics::Kind::Dangle;
    return !out.nodes.empty();
}

}  // namespace ASECM3
