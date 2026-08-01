// Daidalos - Jolt Physics backend. THE ONLY FILE IN THE ENGINE THAT INCLUDES
// A JOLT HEADER. If that ever stops being true, the abstraction has leaked.

#include "dai_physics.hpp"

#include <Jolt/Jolt.h>
#include <Jolt/RegisterTypes.h>
#include <Jolt/Core/Factory.h>
#include <Jolt/Core/TempAllocator.h>
#include <Jolt/Core/JobSystemThreadPool.h>
#include <Jolt/Physics/PhysicsSettings.h>
#include <Jolt/Physics/PhysicsSystem.h>
#include <Jolt/Physics/StateRecorderImpl.h>
#include <Jolt/Physics/Collision/Shape/BoxShape.h>
#include <Jolt/Physics/Collision/Shape/SphereShape.h>
#include <Jolt/Physics/Collision/Shape/CapsuleShape.h>
#include <Jolt/Physics/Collision/Shape/StaticCompoundShape.h>
#include <Jolt/Physics/Collision/RayCast.h>
#include <Jolt/Physics/Collision/CastResult.h>
#include <Jolt/Physics/Body/BodyCreationSettings.h>
#include <Jolt/Physics/Collision/ContactListener.h>
#include <Jolt/Physics/Constraints/FixedConstraint.h>
#include <Jolt/Physics/Constraints/HingeConstraint.h>
#include <Jolt/Physics/Constraints/SliderConstraint.h>
#include <Jolt/Physics/Constraints/PointConstraint.h>
#include <Jolt/Physics/Constraints/DistanceConstraint.h>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <mutex>

using namespace JPH;

namespace dai {
namespace {

namespace Layers {
    constexpr ObjectLayer NON_MOVING = 0;
    constexpr ObjectLayer MOVING     = 1;
    constexpr ObjectLayer NUM        = 2;
}
namespace BPL {
    constexpr BroadPhaseLayer NON_MOVING(0);
    constexpr BroadPhaseLayer MOVING(1);
    constexpr JPH::uint NUM = 2;
}

class BPLayerImpl final : public BroadPhaseLayerInterface {
public:
    BPLayerImpl() { mMap[Layers::NON_MOVING] = BPL::NON_MOVING; mMap[Layers::MOVING] = BPL::MOVING; }
    JPH::uint GetNumBroadPhaseLayers() const override { return BPL::NUM; }
    BroadPhaseLayer GetBroadPhaseLayer(ObjectLayer l) const override { return mMap[l]; }
#if defined(JPH_EXTERNAL_PROFILE) || defined(JPH_PROFILE_ENABLED)
    const char *GetBroadPhaseLayerName(BroadPhaseLayer) const override { return "layer"; }
#endif
private:
    BroadPhaseLayer mMap[Layers::NUM];
};
class OvBFilter final : public ObjectVsBroadPhaseLayerFilter {
public:
    bool ShouldCollide(ObjectLayer a, BroadPhaseLayer b) const override {
        return a == Layers::NON_MOVING ? b == BPL::MOVING : true; }
};
class OvOFilter final : public ObjectLayerPairFilter {
public:
    bool ShouldCollide(ObjectLayer a, ObjectLayer b) const override {
        return a == Layers::NON_MOVING ? b == Layers::MOVING : true; }
};

inline Vec3  V(const dai_vec3 &v) { return Vec3(v.x, v.y, v.z); }
inline RVec3 RV(const dai_vec3 &v) { return RVec3(v.x, v.y, v.z); }
inline dai_vec3 DV(Vec3Arg v)  { return dai_vec3{ v.GetX(), v.GetY(), v.GetZ() }; }
inline dai_vec3 DVR(RVec3Arg v) { return dai_vec3{ (float)v.GetX(), (float)v.GetY(), (float)v.GetZ() }; }
inline Quat  Q(const dai_quat &q) { Quat r(q.x, q.y, q.z, q.w); return r.IsNormalized() ? r : Quat::sIdentity(); }
inline dai_quat DQ(QuatArg q) { return dai_quat{ q.GetX(), q.GetY(), q.GetZ(), q.GetW() }; }

struct JointSlot {
    bool            alive = false;
    int             type  = 0;
    Ref<Constraint> c;
    // Hinge/slider axis expressed in body 1's local frame. Needed to report a
    // joint speed: the world space axis rotates with body 1, and assuming a
    // fixed axis silently reports zero the moment the body turns.
    Vec3            axis_local1 = Vec3::sAxisY();
};

struct Slot {
    bool     alive = false;
    BodyID   id;
    Body    *body = nullptr;
    float    mu_static = 0.6f, mu_kinetic = 0.6f;
    bool     sliding = false;
    bool     dynamic = false;
};

class JoltBackend;

// Contact listener. Reads shared state only - Jolt calls this from several
// worker threads at once and the call order is not defined. Collected events
// go into a per thread bucket and are merged and sorted after the step.
class Listener final : public ContactListener {
public:
    JoltBackend *be = nullptr;
    void OnContactAdded(const Body &b1, const Body &b2, const ContactManifold &m, ContactSettings &s) override;
    void OnContactPersisted(const Body &b1, const Body &b2, const ContactManifold &m, ContactSettings &s) override;
};

class JoltBackend final : public IPhysicsBackend {
public:
    ~JoltBackend() override {
        for (uint32_t i = 0; i < joints.size(); ++i) if (joints[i].alive) destroy_joint(i);
        for (uint32_t i = 0; i < slots.size(); ++i) if (slots[i].alive) destroy_body(i);
        delete jobs; delete temp;
    }

    const char *name() const override { return "jolt"; }

    bool init(const dai_config &cfg, char *err, size_t err_len) override {
        static bool registered = false;
        if (!registered) {
            RegisterDefaultAllocator();
            if (Factory::sInstance == nullptr) Factory::sInstance = new Factory();
            RegisterTypes();
            registered = true;
        }
        int workers = (int)cfg.physics_threads;
        workers = (workers == 0) ? -1 : workers - 1;
        temp = new TempAllocatorImpl(32 * 1024 * 1024);
        jobs = new JobSystemThreadPool(cMaxPhysicsJobs, cMaxPhysicsBarriers, workers);

        uint32_t max_pairs = std::max(1024u, cfg.max_bodies * 4);
        ps.Init(cfg.max_bodies, 0, max_pairs, max_pairs, bpl, ovb, ovo);
        PhysicsSettings st = ps.GetPhysicsSettings();
        st.mNumVelocitySteps = cfg.velocity_steps;
        st.mNumPositionSteps = cfg.position_steps;
        ps.SetPhysicsSettings(st);
        listener.be = this;
        ps.SetContactListener(&listener);
        ps.SetGravity(Vec3(0, -9.81f, 0));
        slots.resize(cfg.max_bodies);
        joints.resize(512);
        (void)err; (void)err_len;
        return true;
    }

    bool create_body(uint32_t slot, const dai_body_desc &d,
                     const std::vector<dai_compound_part> &parts) override {
        if (slot >= slots.size()) return false;
        RefConst<Shape> shape = make_shape(d, parts);
        if (shape == nullptr) return false;

        EMotionType mt = d.motion == DAI_DYNAMIC ? EMotionType::Dynamic
                       : d.motion == DAI_KINEMATIC ? EMotionType::Kinematic : EMotionType::Static;
        ObjectLayer layer = (mt == EMotionType::Static) ? Layers::NON_MOVING : Layers::MOVING;

        BodyCreationSettings bc(shape, RV(d.position), Q(d.rotation), mt, layer);
        bc.mFriction        = d.friction_static;
        bc.mRestitution     = d.restitution;
        bc.mLinearDamping   = d.linear_damping;
        bc.mAngularDamping  = d.angular_damping;
        bc.mAllowSleeping   = d.no_sleeping == 0;
        bc.mLinearVelocity  = V(d.linear_velocity);
        bc.mAngularVelocity = V(d.angular_velocity);
        bc.mUserData        = slot;
        if (d.density > 0.0f && mt != EMotionType::Static) {
            MassProperties mp = shape->GetMassProperties();
            float k = d.density / 1000.0f;
            mp.mMass *= k; mp.mInertia *= k; mp.mInertia(3, 3) = 1.0f;
            bc.mOverrideMassProperties = EOverrideMassProperties::MassAndInertiaProvided;
            bc.mMassPropertiesOverride = mp;
        }

        BodyInterface &bi = ps.GetBodyInterface();
        Body *b = bi.CreateBodyWithID(BodyID(slot), bc);
        if (b == nullptr) return false;
        bi.AddBody(b->GetID(), mt == EMotionType::Static ? EActivation::DontActivate : EActivation::Activate);

        Slot &s = slots[slot];
        s.alive = true; s.id = b->GetID(); s.body = b;
        s.mu_static = d.friction_static; s.mu_kinetic = d.friction_kinetic;
        s.sliding = false; s.dynamic = (mt == EMotionType::Dynamic);
        return true;
    }

    void destroy_body(uint32_t slot) override {
        if (slot >= slots.size() || !slots[slot].alive) return;
        BodyInterface &bi = ps.GetBodyInterface();
        bi.RemoveBody(slots[slot].id);
        bi.DestroyBody(slots[slot].id);
        slots[slot].alive = false;
    }

    void add_impulse(uint32_t slot, dai_vec3 imp) override {
        if (slot < slots.size() && slots[slot].alive) ps.GetBodyInterface().AddImpulse(slots[slot].id, V(imp));
    }
    void set_velocity(uint32_t slot, dai_vec3 lin, dai_vec3 ang) override {
        if (slot >= slots.size() || !slots[slot].alive) return;
        BodyInterface &bi = ps.GetBodyInterface();
        bi.SetLinearVelocity(slots[slot].id, V(lin));
        bi.SetAngularVelocity(slots[slot].id, V(ang));
    }
    void set_gravity(dai_vec3 g) override { ps.SetGravity(V(g)); }

    void step(float dt) override {
        contacts.clear();
        ps.Update(dt, 1, temp, jobs);

        // Deterministic ordering: the callbacks fired from several threads,
        // so sort before anyone downstream can observe the order.
        std::sort(contacts.begin(), contacts.end(), [](const ContactEvent &a, const ContactEvent &b) {
            if (a.slot_a != b.slot_a) return a.slot_a < b.slot_a;
            return a.slot_b < b.slot_b;
        });

        // Stiction bookkeeping, single threaded and after the solver, so the
        // velocity does not still contain this tick's gravity.
        BodyInterface &bi = ps.GetBodyInterface();
        for (Slot &s : slots) {
            if (!s.alive || !s.dynamic || s.mu_static == s.mu_kinetic) continue;
            float v = bi.GetLinearVelocity(s.id).Length();
            s.sliding = s.sliding ? (v > 0.05f) : (v > 0.20f);
        }
    }

    void get_transform(uint32_t slot, dai_vec3 &p, dai_quat &r) const override {
        if (slot >= slots.size() || !slots[slot].alive) { p = {}; r = { 0,0,0,1 }; return; }
        const BodyInterface &bi = ps.GetBodyInterface();
        // GetPosition, not GetCenterOfMassPosition: create_body takes the body
        // ORIGIN, so reporting the centre of mass makes the two disagree for
        // compound shapes - the round trip through a save file then walks the
        // body a little further every time, and compound parts render offset.
        p = DVR(bi.GetPosition(slots[slot].id));
        r = DQ(bi.GetRotation(slots[slot].id));
    }
    void set_transform(uint32_t slot, dai_vec3 p, dai_quat r) override {
        if (slot >= slots.size() || !slots[slot].alive) return;
        BodyInterface &bi = ps.GetBodyInterface();
        // A static body cannot be "activated", and asking Jolt to do so makes
        // the move a no-op - which is exactly what an editor does all day.
        EMotionType mt = bi.GetMotionType(slots[slot].id);
        bi.SetPositionAndRotation(slots[slot].id, RV(p), Q(r),
                                  mt == EMotionType::Static ? EActivation::DontActivate
                                                            : EActivation::Activate);
    }
    void get_velocity(uint32_t slot, dai_vec3 &l, dai_vec3 &a) const override {
        if (slot >= slots.size() || !slots[slot].alive) { l = {}; a = {}; return; }
        const BodyInterface &bi = ps.GetBodyInterface();
        l = DV(bi.GetLinearVelocity(slots[slot].id));
        a = DV(bi.GetAngularVelocity(slots[slot].id));
    }
    bool is_sliding(uint32_t slot) const override {
        return slot < slots.size() && slots[slot].alive && slots[slot].sliding;
    }

    bool raycast(dai_vec3 from, dai_vec3 dir, float max_distance, RayHit &out) const override {
        Vec3 d = V(dir);
        if (d.LengthSq() < 1e-12f) return false;
        d = d.Normalized() * max_distance;
        RRayCast ray{ RV(from), d };
        RayCastResult hit;
        if (!ps.GetNarrowPhaseQuery().CastRay(ray, hit)) return false;
        uint32_t idx = hit.mBodyID.GetIndex();
        out.slot     = idx;
        out.distance = hit.mFraction * max_distance;
        RVec3 p = ray.GetPointOnRay(hit.mFraction);
        out.point = DVR(p);
        BodyLockRead lock(ps.GetBodyLockInterface(), hit.mBodyID);
        if (lock.Succeeded())
            out.normal = DV(lock.GetBody().GetWorldSpaceSurfaceNormal(hit.mSubShapeID2, p));
        return true;
    }

    uint32_t poll_contacts(ContactEvent *out, uint32_t max) override {
        uint32_t n = (uint32_t)contacts.size();
        if (out) for (uint32_t i = 0; i < n && i < max; ++i) out[i] = contacts[i];
        return n;
    }

    bool create_joint(uint32_t slot, const dai_joint_desc &d, uint32_t sa, uint32_t sb) override {
        if (slot >= joints.size()) joints.resize(slot + 64);
        if (joints[slot].alive) return false;
        Body *ba = (sa < slots.size() && slots[sa].alive) ? slots[sa].body : nullptr;
        Body *bb = (sb < slots.size() && slots[sb].alive) ? slots[sb].body : &Body::sFixedToWorld;
        if (!ba || !bb) return false;

        RVec3 anchor = RV(d.anchor);
        Vec3  axis   = V(d.axis);
        if (axis.LengthSq() < 1e-12f) axis = Vec3::sAxisY(); else axis = axis.Normalized();
        Vec3  normal = V(d.normal_axis);
        if (normal.LengthSq() < 1e-12f || fabsf(normal.Dot(axis)) > 0.99f)
            normal = axis.GetNormalizedPerpendicular();
        else
            normal = (normal - axis * normal.Dot(axis)).Normalized();

        Ref<Constraint> c;
        switch (d.type) {
        case DAI_JOINT_HINGE: {
            HingeConstraintSettings st;
            st.mSpace = EConstraintSpace::WorldSpace;
            st.mPoint1 = st.mPoint2 = anchor;
            st.mHingeAxis1 = st.mHingeAxis2 = axis;
            st.mNormalAxis1 = st.mNormalAxis2 = normal;
            if (d.enable_limits) { st.mLimitsMin = d.limit_min; st.mLimitsMax = d.limit_max; }
            st.mMaxFrictionTorque = d.max_friction;
            if (d.max_motor_force > 0.0f) st.mMotorSettings.SetTorqueLimit(d.max_motor_force);
            c = st.Create(*ba, *bb);
            break;
        }
        case DAI_JOINT_SLIDER: {
            SliderConstraintSettings st;
            st.mSpace = EConstraintSpace::WorldSpace;
            st.mPoint1 = st.mPoint2 = anchor;
            st.mSliderAxis1 = st.mSliderAxis2 = axis;
            st.mNormalAxis1 = st.mNormalAxis2 = normal;
            if (d.enable_limits) { st.mLimitsMin = d.limit_min; st.mLimitsMax = d.limit_max; }
            st.mMaxFrictionForce = d.max_friction;
            if (d.max_motor_force > 0.0f) st.mMotorSettings.SetForceLimit(d.max_motor_force);
            c = st.Create(*ba, *bb);
            break;
        }
        case DAI_JOINT_POINT: {
            PointConstraintSettings st;
            st.mSpace = EConstraintSpace::WorldSpace;
            st.mPoint1 = st.mPoint2 = anchor;
            c = st.Create(*ba, *bb);
            break;
        }
        case DAI_JOINT_DISTANCE: {
            DistanceConstraintSettings st;
            st.mSpace = EConstraintSpace::WorldSpace;
            st.mPoint1 = anchor;
            st.mPoint2 = RV(d.normal_axis);   // second anchor rides in normal_axis
            st.mMinDistance = d.min_distance;
            st.mMaxDistance = d.max_distance;
            c = st.Create(*ba, *bb);
            break;
        }
        default: {
            FixedConstraintSettings st;
            st.mSpace = EConstraintSpace::WorldSpace;
            st.mAutoDetectPoint = true;
            c = st.Create(*ba, *bb);
            break;
        }
        }
        if (c == nullptr) return false;
        ps.AddConstraint(c);
        joints[slot].alive = true;
        joints[slot].type  = d.type;
        joints[slot].c     = c;
        joints[slot].axis_local1 = ba->GetRotation().Conjugated() * axis;
        return true;
    }

    void destroy_joint(uint32_t slot) override {
        if (slot >= joints.size() || !joints[slot].alive) return;
        ps.RemoveConstraint(joints[slot].c);
        joints[slot].c = nullptr;
        joints[slot].alive = false;
    }

    void set_motor(uint32_t slot, int state, float target) override {
        if (slot >= joints.size() || !joints[slot].alive) return;
        EMotorState ms = state == DAI_MOTOR_VELOCITY ? EMotorState::Velocity
                       : state == DAI_MOTOR_POSITION ? EMotorState::Position : EMotorState::Off;
        if (joints[slot].type == DAI_JOINT_HINGE) {
            HingeConstraint *h = static_cast<HingeConstraint *>(joints[slot].c.GetPtr());
            if (ms != EMotorState::Off && !h->GetMotorSettings().IsValid()) return;
            h->SetMotorState(ms);
            if (ms == EMotorState::Velocity) h->SetTargetAngularVelocity(target);
            else if (ms == EMotorState::Position) h->SetTargetAngle(target);
        } else if (joints[slot].type == DAI_JOINT_SLIDER) {
            SliderConstraint *sl = static_cast<SliderConstraint *>(joints[slot].c.GetPtr());
            if (ms != EMotorState::Off && !sl->GetMotorSettings().IsValid()) return;
            sl->SetMotorState(ms);
            if (ms == EMotorState::Velocity) sl->SetTargetVelocity(target);
            else if (ms == EMotorState::Position) sl->SetTargetPosition(target);
        }
    }

    bool get_joint_state(uint32_t slot, float &position, float &speed) const override {
        position = 0.0f; speed = 0.0f;
        if (slot >= joints.size() || !joints[slot].alive) return false;
        const JointSlot &j = joints[slot];
        if (j.type == DAI_JOINT_HINGE) {
            const HingeConstraint *h = static_cast<const HingeConstraint *>(j.c.GetPtr());
            position = h->GetCurrentAngle();
            const Body &b1 = *h->GetBody1(); const Body &b2 = *h->GetBody2();
            Vec3 axis = b1.GetRotation() * j.axis_local1;
            speed = (b1.GetAngularVelocity() - b2.GetAngularVelocity()).Dot(axis);
            return true;
        }
        if (j.type == DAI_JOINT_SLIDER) {
            const SliderConstraint *s = static_cast<const SliderConstraint *>(j.c.GetPtr());
            position = s->GetCurrentPosition();
            const Body &b1 = *s->GetBody1(); const Body &b2 = *s->GetBody2();
            Vec3 axis = b1.GetRotation() * j.axis_local1;
            speed = (b1.GetLinearVelocity() - b2.GetLinearVelocity()).Dot(axis);
            return true;
        }
        return true;
    }

    bool save_state(std::string &out) const override {
        StateRecorderImpl rec;
        ps.SaveState(rec);
        out = rec.GetData();
        return true;
    }
    bool restore_state(const std::string &in) override {
        StateRecorderImpl rec;
        rec.WriteBytes(in.data(), in.size());
        rec.Rewind();
        return ps.RestoreState(rec);
    }
    uint32_t active_bodies() const override { return ps.GetNumActiveBodies(EBodyType::RigidBody); }

    // used by the listener
    void record_contact(const ContactEvent &e) {
        std::lock_guard<std::mutex> g(mtx);
        if (contacts.size() < 8192) contacts.push_back(e);
    }
    float friction_for(uint64 user) const {
        if (user >= slots.size()) return 0.6f;
        const Slot &s = slots[(size_t)user];
        return s.sliding ? s.mu_kinetic : s.mu_static;
    }

private:
    static RefConst<Shape> one_shape(int shape, const dai_vec3 &he) {
        switch (shape) {
        case DAI_SHAPE_SPHERE: {
            SphereShapeSettings s(he.x > 0 ? he.x : 0.5f); s.SetEmbedded(); return s.Create().Get();
        }
        case DAI_SHAPE_CAPSULE: {
            CapsuleShapeSettings s(he.y > 0 ? he.y : 0.5f, he.x > 0 ? he.x : 0.5f); s.SetEmbedded();
            return s.Create().Get();
        }
        default: {
            Vec3 e(he.x > 0 ? he.x : 0.5f, he.y > 0 ? he.y : 0.5f, he.z > 0 ? he.z : 0.5f);
            float cr = std::min({ e.GetX(), e.GetY(), e.GetZ(), 0.05f });
            BoxShapeSettings s(e, cr); s.SetEmbedded(); return s.Create().Get();
        }
        }
    }
    static RefConst<Shape> make_shape(const dai_body_desc &d, const std::vector<dai_compound_part> &parts) {
        if (d.shape != DAI_SHAPE_COMPOUND || parts.empty()) return one_shape(d.shape, d.half_extent);
        StaticCompoundShapeSettings cs; cs.SetEmbedded();
        for (const dai_compound_part &p : parts)
            cs.AddShape(V(p.offset), Q(p.rotation), one_shape(p.shape, p.half_extent));
        ShapeSettings::ShapeResult r = cs.Create();
        return r.IsValid() ? r.Get() : one_shape(DAI_SHAPE_BOX, d.half_extent);
    }

    PhysicsSystem        ps;
    BPLayerImpl          bpl;
    OvBFilter            ovb;
    OvOFilter            ovo;
    Listener             listener;
    TempAllocatorImpl   *temp = nullptr;
    JobSystemThreadPool *jobs = nullptr;
    std::vector<Slot>       slots;
    std::vector<JointSlot>  joints;
    std::vector<ContactEvent> contacts;
    std::mutex           mtx;
};


// How hard was that hit.
//
// Jolt does not hand the solver's applied impulse to a contact callback - it is
// not known yet when the callback runs. What IS known is the state going in,
// and the collision impulse for a pair of rigid bodies is textbook:
//
//     j = (1 + e) * v_rel_n / (1/m1 + 1/m2)
//
// with v_rel_n the closing speed along the contact normal, measured AT the
// contact point so a spinning body's rim counts, and e the restitution Jolt
// combined for this pair. Rotational inertia terms are left out: they lower
// the true impulse for an off centre hit, so this is an upper bound rather
// than a number that quietly grows.
//
// Two things this is deliberately NOT: it is the impulse of the IMPACT, so a
// crate resting on the floor reports ~0 rather than its weight - "how hard did
// that hit" and "how much load is on this" are different questions. And two
// static bodies report 0, because nothing about that collision has a mass.
static float impact_impulse(const Body &b1, const Body &b2, const ContactManifold &m, float restitution) {
    const MotionProperties *mp1 = b1.GetMotionPropertiesUnchecked();
    const MotionProperties *mp2 = b2.GetMotionPropertiesUnchecked();
    float inv_mass = 0.0f;
    if (mp1 && !b1.IsStatic()) inv_mass += mp1->GetInverseMass();
    if (mp2 && !b2.IsStatic()) inv_mass += mp2->GetInverseMass();
    if (inv_mass <= 0.0f) return 0.0f;

    RVec3 p = m.GetWorldSpaceContactPointOn1(0);
    Vec3 v_rel = b1.GetPointVelocity(p) - b2.GetPointVelocity(p);
    // The manifold normal points from shape 1 towards shape 2, so a closing
    // pair has a positive component along it.
    float closing = v_rel.Dot(m.mWorldSpaceNormal);
    if (closing <= 0.0f) return 0.0f;          // separating, or already resting
    return (1.0f + restitution) * closing / inv_mass;
}

void Listener::OnContactAdded(const Body &b1, const Body &b2, const ContactManifold &m, ContactSettings &s) {
    s.mCombinedFriction = sqrtf(be->friction_for(b1.GetUserData()) * be->friction_for(b2.GetUserData()));
    ContactEvent e;
    e.slot_a = (uint32_t)b1.GetUserData();
    e.slot_b = (uint32_t)b2.GetUserData();
    e.normal = DV(m.mWorldSpaceNormal);
    e.point  = DVR(m.GetWorldSpaceContactPointOn1(0));
    e.impulse = impact_impulse(b1, b2, m, s.mCombinedRestitution);
    be->record_contact(e);
}
void Listener::OnContactPersisted(const Body &b1, const Body &b2, const ContactManifold &m, ContactSettings &s) {
    s.mCombinedFriction = sqrtf(be->friction_for(b1.GetUserData()) * be->friction_for(b2.GetUserData()));
    ContactEvent e;
    e.slot_a = (uint32_t)b1.GetUserData();
    e.slot_b = (uint32_t)b2.GetUserData();
    e.normal = DV(m.mWorldSpaceNormal);
    e.point  = DVR(m.GetWorldSpaceContactPointOn1(0));
    e.impulse = impact_impulse(b1, b2, m, s.mCombinedRestitution);
    be->record_contact(e);
}

} // namespace

IPhysicsBackend *create_jolt_backend() { return new JoltBackend(); }

} // namespace dai
