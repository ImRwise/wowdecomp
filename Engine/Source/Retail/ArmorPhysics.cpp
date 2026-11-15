#include "Engine/Source/Retail/ArmorPhysics.h"

#include <algorithm>
#include <cmath>

namespace Retail::ArmorPhysics {

namespace {

bool Finite(float v) { return std::isfinite(v); }
bool Finite(const Vec3& v) { return Finite(v.x) && Finite(v.y) && Finite(v.z); }
Vec3 Clean(const Vec3& v) { return Finite(v) ? v : Vec3{}; }
Vec3 Plus(const Vec3& a, const Vec3& b) { return {a.x + b.x, a.y + b.y, a.z + b.z}; }
Vec3 Scale(const Vec3& a, float s) { return {a.x * s, a.y * s, a.z * s}; }
float Dot(const Vec3& a, const Vec3& b) { return a.x * b.x + a.y * b.y + a.z * b.z; }
Vec3 Cross(const Vec3& a, const Vec3& b) {
    return {a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x};
}
float Length(const Vec3& a) { return std::sqrt(Dot(a, a)); }

// Pulls |theta| back to `limit` and drops the outward part of the velocity (a soft stop for free bones).
void LimitCone(Vec3& theta, Vec3& velocity, float limit) {
    const float length = Length(theta);
    if (!(length > limit) || length <= 1e-9f) return;
    const Vec3 unit = Scale(theta, 1.0f / length);
    theta = Scale(unit, limit);
    const float outward = Dot(velocity, unit);
    if (outward > 0) velocity = Plus(velocity, Scale(unit, -outward));
}

}  // namespace

void RotationMatrix(const Vec3& theta, float out[9]) {
    const float angle = Length(theta);
    if (!Finite(angle) || angle < 1e-7f) {
        const float identity[9] = {1, 0, 0, 0, 1, 0, 0, 0, 1};
        std::copy(identity, identity + 9, out);
        return;
    }
    const Vec3 u = Scale(theta, 1.0f / angle);
    const float c = std::cos(angle), s = std::sin(angle), t = 1.0f - c;
    out[0] = c + u.x * u.x * t;       out[1] = u.x * u.y * t - u.z * s; out[2] = u.x * u.z * t + u.y * s;
    out[3] = u.y * u.x * t + u.z * s; out[4] = c + u.y * u.y * t;       out[5] = u.y * u.z * t - u.x * s;
    out[6] = u.z * u.x * t - u.y * s; out[7] = u.z * u.y * t + u.x * s; out[8] = c + u.z * u.z * t;
}

int Body::Add(const Node& node) {
    Node copy = node;
    if (copy.parent >= int(m_nodes.size())) copy.parent = -1;   // parents first: a forward reference would never settle
    const float length = Length(copy.dir);
    copy.dir = length > 1e-6f && Finite(length) ? Scale(copy.dir, 1.0f / length) : Vec3{0, 0, -1};
    const float hinge = Length(copy.hinge);
    copy.hinge = hinge > 1e-6f && Finite(hinge) ? Scale(copy.hinge, 1.0f / hinge) : Vec3{0, 1, 0};
    if (!(copy.hingeMin <= copy.hingeMax)) std::swap(copy.hingeMin, copy.hingeMax);
    copy.angle = {};
    copy.velocity = {};
    m_nodes.push_back(copy);
    return int(m_nodes.size()) - 1;
}

void Body::Reset() {
    for (Node& node : m_nodes) {
        node.angle = {};
        node.velocity = {};
    }
}

void Body::Step(const Input& input) { Step(input, {}); }

void Body::Step(const Input& input, const std::vector<NodeFrame>& frames) {
    float remaining = Finite(input.dt) ? std::min(std::max(input.dt, 0.0f), kMaxFrame) : 0.0f;
    if (remaining <= 0 || m_nodes.empty()) return;
    const Vec3 acceleration = Clean(input.acceleration);
    const Vec3 gravity = Clean(input.gravity);
    const Vec3 wind = Clean(input.wind);

    const int steps = std::max(1, std::min(kMaxSubsteps, int(std::ceil(remaining / kSubstep - 1e-4f))));
    const float h = remaining / float(steps);
    for (int step = 0; step < steps; ++step) {
        for (size_t i = 0; i < m_nodes.size(); ++i) {
            Node& node = m_nodes[i];
            const Params& p = node.params;
            if (!Finite(node.angle) || !Finite(node.velocity)) {   // a poisoned state must not stay in the simulation
                node.angle = {};
                node.velocity = {};
            }
            const float omega = std::min(std::max(Finite(p.frequency) ? p.frequency : 0.0f, 0.0f), kMaxFrequency);
            const float zeta = std::min(std::max(Finite(p.damping) ? p.damping : 0.0f, 0.0f), 2.0f);

            const NodeFrame* frame = i < frames.size() && frames[i].valid ? &frames[i] : nullptr;
            Vec3 pushed = acceleration;
            Vec3 down = gravity;
            if (frame) {
                // The node hangs from a moving frame: its tip is thrown by the frame's turning, and gravity follows the tilt.
                const Vec3 lever = Scale(node.dir, Finite(p.arm) ? p.arm : 0.0f);
                const Vec3 w = Clean(frame->angularVelocity), alpha = Clean(frame->angularAcceleration);
                pushed = Plus(pushed, Plus(Cross(alpha, lever), Cross(w, Cross(w, lever))));
                down = Clean(frame->gravity);
            }
            Vec3 force = Scale(pushed, -p.inertia);
            force = Plus(force, Scale(wind, p.wind * p.windScale));
            force = Plus(force, Scale(down, p.gravity * p.gravityScale));
            Vec3 target = Cross(node.dir, force);
            if (node.kind == Kind::Cloth && node.parent >= 0)
                target = Plus(target, Scale(m_nodes[size_t(node.parent)].angle, p.follow));
            if (node.kind == Kind::Rigid) target = Scale(node.hinge, Dot(target, node.hinge));

            // Semi-implicit Euler: velocity first, then position with the new velocity.
            const Vec3 error = {node.angle.x - target.x, node.angle.y - target.y, node.angle.z - target.z};
            const Vec3 accel = Plus(Scale(error, -omega * omega), Scale(node.velocity, -2.0f * zeta * omega));
            node.velocity = Plus(node.velocity, Scale(accel, h));
            node.angle = Plus(node.angle, Scale(node.velocity, h));

            if (node.kind == Kind::Rigid) {
                float phi = Dot(node.angle, node.hinge), rate = Dot(node.velocity, node.hinge);
                if (phi > node.hingeMax) { phi = node.hingeMax; if (rate > 0) rate = -rate * std::max(0.0f, p.restitution); }
                if (phi < node.hingeMin) { phi = node.hingeMin; if (rate < 0) rate = -rate * std::max(0.0f, p.restitution); }
                node.angle = Scale(node.hinge, phi);
                node.velocity = Scale(node.hinge, rate);
            } else {
                LimitCone(node.angle, node.velocity, std::max(0.0f, Finite(p.maxAngle) ? p.maxAngle : 0.0f));
            }
        }
    }
}

void Body::Matrix(size_t i, float out[9]) const {
    if (i >= m_nodes.size()) {
        RotationMatrix({}, out);
        return;
    }
    RotationMatrix(m_nodes[i].angle, out);
}

float Body::PeakAngle() const {
    float peak = 0;
    for (const Node& node : m_nodes) peak = std::max(peak, Length(node.angle));
    return peak;
}

}  // namespace Retail::ArmorPhysics
