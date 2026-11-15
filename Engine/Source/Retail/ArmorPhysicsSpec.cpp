#include "Engine/Source/Retail/ArmorPhysicsSpec.h"

#include <algorithm>
#include <cmath>

namespace Retail::ArmorPhysics {

namespace {
constexpr float kDegrees = 3.14159265358979f / 180.0f;
Vec3 Minus(const float a[3], const float b[3]) { return {a[0] - b[0], a[1] - b[1], a[2] - b[2]}; }
float Length(const Vec3& v) { return std::sqrt(v.x * v.x + v.y * v.y + v.z * v.z); }
}  // namespace

bool BuildFromPhys(const Retail::Phys::File& file, uint32_t boneCount, Spec& out) {
    out = {};
    out.source = "phys";
    out.colliders = uint32_t(file.capsules.size() + file.spheres.size());
    const size_t count = file.bodies.size();
    std::vector<int> specOf(count, -1);
    std::vector<int> jointOf(count, -1);   // the joint that makes body i a child
    for (size_t j = 0; j < file.joints.size(); ++j)
        if (jointOf[file.joints[j].bodyB] < 0) jointOf[file.joints[j].bodyB] = int(j);

    // parents first: a body is emitted after its parent chain, found by walking the joint table (cycle-safe)
    std::vector<uint32_t> order;
    std::vector<uint8_t> visiting(count, 0), done(count, 0);
    auto visit = [&](auto&& self, uint32_t body) -> void {
        if (done[body] || visiting[body]) return;
        visiting[body] = 1;
        if (jointOf[body] >= 0) self(self, file.joints[size_t(jointOf[body])].bodyA);
        visiting[body] = 0;
        done[body] = 1;
        order.push_back(body);
    };
    for (uint32_t i = 0; i < count; ++i) visit(visit, i);

    for (uint32_t body : order) {
        const Retail::Phys::Body& b = file.bodies[body];
        if (b.bone >= boneCount) continue;
        if (!b.dynamic) { ++out.anchors; continue; }
        NodeSpec node;
        node.bone = b.bone;
        // nearest dynamic ancestor that survived (anchors and dropped bodies are skipped)
        int ancestor = -1;
        for (int walk = jointOf[body] >= 0 ? int(file.joints[size_t(jointOf[body])].bodyA) : -1, guard = 0;
             walk >= 0 && guard < int(count); ++guard) {
            if (specOf[size_t(walk)] >= 0) { ancestor = specOf[size_t(walk)]; break; }
            walk = jointOf[size_t(walk)] >= 0 ? int(file.joints[size_t(jointOf[size_t(walk)])].bodyA) : -1;
        }
        node.parent = ancestor;

        // direction: parent body -> this body at rest; a body with no parent hangs from its own bone's origin
        const int parentBody = jointOf[body] >= 0 ? int(file.joints[size_t(jointOf[body])].bodyA) : -1;
        if (parentBody >= 0) node.dir = Minus(b.position, file.bodies[size_t(parentBody)].position);
        if (Length(node.dir) < 1e-4f) node.dir = {0, 0, -1};

        node.kind = Kind::Cloth;
        if (jointOf[body] >= 0) {
            const Retail::Phys::Joint& joint = file.joints[size_t(jointOf[body])];
            if (joint.kind == Retail::Phys::JointKind::Revolute) {
                node.kind = Kind::Rigid;
                // hinge axis: the first row of the joint's frame on the parent side (x axis of the authored frame)
                node.hinge = {joint.frameA.rotation[0], joint.frameA.rotation[1], joint.frameA.rotation[2]};
                if (Length(node.hinge) < 1e-4f) node.hinge = {0, 1, 0};
                if (joint.limited) {
                    node.hingeMin = joint.twistMin * kDegrees;
                    node.hingeMax = joint.twistMax * kDegrees;
                }
            } else if (joint.kind == Retail::Phys::JointKind::Spherical && joint.limited) {
                const float widest = std::max({joint.swing, std::fabs(joint.twistMin), std::fabs(joint.twistMax)});
                if (widest > 0) node.maxAngle = std::min(widest, 120.0f) * kDegrees;
            }
        }
        specOf[body] = int(out.nodes.size());
        out.nodes.push_back(node);
    }
    // a simulated body with no simulated child and no joint is a free dangle
    std::vector<uint8_t> hasChild(out.nodes.size(), 0);
    for (const NodeSpec& n : out.nodes) if (n.parent >= 0) hasChild[size_t(n.parent)] = 1;
    for (size_t i = 0; i < out.nodes.size(); ++i)
        if (out.nodes[i].kind == Kind::Cloth && out.nodes[i].parent < 0 && !hasChild[i]) out.nodes[i].kind = Kind::Dangle;
    return !out.nodes.empty();
}

size_t Instantiate(const Spec& spec, Body& body, std::vector<uint32_t>& nodeBone) {
    std::vector<int> index(spec.nodes.size(), -1);
    size_t added = 0;
    for (size_t i = 0; i < spec.nodes.size(); ++i) {
        const NodeSpec& s = spec.nodes[i];
        Node node;
        node.kind = s.kind;
        node.dir = s.dir;
        node.hinge = s.hinge;
        node.hingeMin = s.hingeMin;
        node.hingeMax = s.hingeMax;
        node.parent = s.parent >= 0 && size_t(s.parent) < i ? index[size_t(s.parent)] : -1;
        if (s.maxAngle >= 0) node.params.maxAngle = s.maxAngle;
        if (s.frequency > 0) node.params.frequency = s.frequency;
        if (s.follow >= 0) node.params.follow = s.follow;
        if (node.kind != Kind::Cloth) node.params.follow = 0.0f;
        index[i] = body.Add(node);
        nodeBone.push_back(s.bone);
        ++added;
    }
    return added;
}

}  // namespace Retail::ArmorPhysics
