#ifndef ENGINE_SOURCE_RETAIL_ARMORPHYSICS_H
#define ENGINE_SOURCE_RETAIL_ARMORPHYSICS_H

// Bone-driven secondary motion for Retail armor: capes, tabards, belts, dangling shoulder plates, chains.
// Not in the original binary. This header is the engine-independent solver (no client types, testable in isolation);
// the adapter that finds the bones, samples the wind and writes the procedural bone transforms lives in
// WoW/Common/ComponentCore/RetailArmorPhysics.cpp. Everything here is deterministic: the same inputs and the same
// sequence of Step() calls give bit-identical results, and a hitch longer than kMaxFrame is dropped, not simulated.
//
// Model: every simulated bone is a damped angular spring about its pivot, an angle vector theta (axis * angle, radians)
// in the bone's rest frame. Forces on the bone turn into a torque through the bone's rest direction `dir` (pivot to
// its tip): torque = dir x force, so a hanging cape is pushed sideways by wind, swings back by gravity and spring, and
// lags behind the wearer's movement. Chains (cloth) pass their parent's motion down with `follow`; a rigid plate is a
// hinge (one axis, hard limits, restitution).
//
//   theta'' = -k theta - c theta' + gain * (dir x F)
//   F       = -inertia * a_root + drag * (wind - v_root) + gravityGain * g
//
// Vectors are in the model (wearer) frame, yards and seconds.

#include <cstddef>
#include <cstdint>
#include <vector>

namespace Retail::ArmorPhysics {

constexpr float kMaxFrame = 0.1f;     // longest simulated frame; a longer one is clamped (the rest is dropped)
constexpr float kSubstep = 1.0f / 120.0f;
constexpr int kMaxSubsteps = 12;      // kMaxFrame / kSubstep
constexpr float kMaxFrequency = 60.0f; // rad/s; above this the explicit step could lose stability, so it is clamped

enum class Kind : uint8_t { Cloth, Dangle, Rigid };

struct Vec3 { float x = 0, y = 0, z = 0; };

struct Params {
    float frequency = 9.0f;       // rad/s, sqrt(k)
    float damping = 0.35f;        // fraction of critical damping
    float inertia = 0.045f;       // radians of swing per (yd/s^2) of root acceleration
    float wind = 0.060f;          // radians of lean per (yd/s) of relative wind
    float gravity = 0.020f;       // radians of sag per (yd/s^2) of gravity not along the bone
    float follow = 0.45f;         // share of the parent's angle a child chain bone inherits (Cloth only)
    float maxAngle = 0.9f;        // hard limit on |theta| (Cloth, Dangle) or on the hinge angle (Rigid), radians
    float restitution = 0.25f;    // Rigid: share of the velocity kept when the hinge hits a limit
    float arm = 0.45f;            // yards from the parent bone's pivot to the tip: lever for turning and leaning
    float gravityScale = 1.0f;    // scales this node's gravity (authored data, 1 = as heuristic)
    float windScale = 1.0f;       // scales this node's share of the wind
};

struct Node {
    Kind kind = Kind::Cloth;
    int parent = -1;              // index of an earlier node in the same body, or -1
    Vec3 dir{0, 0, -1};           // unit rest direction, pivot to tip, model frame
    Vec3 hinge{0, 0, 0};          // Rigid: unit hinge axis
    float hingeMin = -0.6f, hingeMax = 0.6f;
    Params params;
    Vec3 angle, velocity;         // state: rotation vector and its rate
};

struct Input {
    float dt = 0;                 // frame time, seconds
    Vec3 acceleration;            // root acceleration, model frame, yd/s^2
    Vec3 gravity{0, 0, -9.8f};    // model frame, yd/s^2 (down)
    Vec3 wind;                    // relative wind velocity at the root, model frame, yd/s
};

// Motion of one node's parent frame (the torso, pelvis, spine or arm bone it hangs from), in that frame's own axes.
// Without it a node sees the body's frame: gravity straight down, no turning. With it, a wearer who leans, turns or
// swings an arm gives the node gravity along the tilted frame plus the tangential and centripetal acceleration of
// its tip (angular acceleration x arm, omega x (omega x arm)).
struct NodeFrame {
    bool valid = false;
    Vec3 gravity;                 // gravity in the parent frame, yd/s^2
    Vec3 angularVelocity;         // rad/s
    Vec3 angularAcceleration;     // rad/s^2
};

// A rotation matrix (column-vector convention, row-major 3x3) for a rotation vector.
void RotationMatrix(const Vec3& theta, float out[9]);

class Body {
public:
    // Nodes must be added parents first. Returns the node index.
    int Add(const Node& node);
    size_t Size() const { return m_nodes.size(); }
    const Node& At(size_t i) const { return m_nodes[i]; }
    // Advances every node. Non-finite inputs are treated as zero; non-finite state is reset.
    void Step(const Input& input);
    // As Step, with a parent-frame sample per node (frames.size() <= Size(); missing entries are not valid).
    void Step(const Input& input, const std::vector<NodeFrame>& frames);
    // Returns every node to rest.
    void Reset();
    // The node's current rotation matrix.
    void Matrix(size_t i, float out[9]) const;
    // The largest |theta| over the body, for diagnostics.
    float PeakAngle() const;
private:
    std::vector<Node> m_nodes;
};

}  // namespace Retail::ArmorPhysics

#endif
