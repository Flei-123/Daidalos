// Daidalos - the physics boundary.
//
// This header is the contract between the engine and whatever actually
// simulates rigid bodies. It contains NO third party type. No JPH::Vec3, no
// JPH::BodyID, no Jolt header. Gameplay and engine code only ever see
// dai_vec3 / dai_quat / uint32_t slot indices.
//
// Two implementations exist:
//   physics_jolt.cpp  - the real one
//   physics_null.cpp  - gravity only, no collisions
//
// The null backend is not a toy: it is the leak test. If the engine still
// compiles and runs against it, no physics detail has escaped this header.
// The day this stops being true is the day the "we could swap the backend"
// claim becomes a lie, and it will be years before anyone notices otherwise.

#pragma once

#include "daidalos.h"
#include <string>
#include <vector>

namespace dai {

struct RayHit {
    uint32_t slot     = UINT32_MAX;   // UINT32_MAX = nothing hit
    float    distance = 0.0f;
    dai_vec3 point{};
    dai_vec3 normal{};
};

struct ContactEvent {
    uint32_t slot_a = 0, slot_b = 0;
    dai_vec3 point{};
    dai_vec3 normal{};
    float    impulse = 0.0f;
};

class IPhysicsBackend {
public:
    virtual ~IPhysicsBackend() = default;

    virtual const char *name() const = 0;
    virtual bool init(const dai_config &cfg, char *err, size_t err_len) = 0;

    // Bodies are addressed by the engine's slot index. The backend is
    // responsible for mapping that onto whatever it uses internally, and it
    // must do so reproducibly: two peers that issued the same create calls in
    // the same order must end up with the same internal ids, or no save state
    // will ever match.
    virtual bool create_body(uint32_t slot, const dai_body_desc &desc,
                             const std::vector<dai_compound_part> &parts) = 0;
    virtual void destroy_body(uint32_t slot) = 0;

    virtual void add_impulse(uint32_t slot, dai_vec3 impulse) = 0;
    virtual void set_velocity(uint32_t slot, dai_vec3 linear, dai_vec3 angular) = 0;
    virtual void set_gravity(dai_vec3 g) = 0;

    virtual void step(float dt) = 0;

    virtual void get_transform(uint32_t slot, dai_vec3 &pos, dai_quat &rot) const = 0;
    virtual void get_velocity(uint32_t slot, dai_vec3 &lin, dai_vec3 &ang) const = 0;
    virtual bool is_sliding(uint32_t slot) const = 0;

    // Queries. Trivially portable, which is exactly why they belong here.
    virtual bool raycast(dai_vec3 from, dai_vec3 dir, float max_distance, RayHit &out) const = 0;

    // Contacts of the last step, already sorted deterministically by the
    // backend. Jolt reports them from several threads in an arbitrary order;
    // handing that straight to gameplay would be a desync waiting to happen.
    virtual uint32_t poll_contacts(ContactEvent *out, uint32_t max) = 0;

    // Snapshot of everything the backend owns. Opaque to the engine on
    // purpose - what is inside is backend specific and does not survive a
    // backend change. That is a documented limitation, not a bug.
    // Joints. Addressed by their own slot index, same reproducibility rule
    // as bodies. slot_b == UINT32_MAX means "anchored to the world".
    virtual bool create_joint(uint32_t slot, const dai_joint_desc &desc,
                              uint32_t slot_a, uint32_t slot_b) = 0;
    virtual void destroy_joint(uint32_t slot) = 0;
    virtual void set_motor(uint32_t slot, int motor_state, float target) = 0;
    virtual bool get_joint_state(uint32_t slot, float &position, float &speed) const = 0;

    virtual bool save_state(std::string &out) const = 0;
    virtual bool restore_state(const std::string &in) = 0;

    virtual uint32_t active_bodies() const = 0;
};

IPhysicsBackend *create_jolt_backend();
IPhysicsBackend *create_null_backend();

} // namespace dai
