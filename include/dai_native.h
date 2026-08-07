/*
 * Native (C++) behaviours.
 *
 * A .cpp file in the project is compiled to a shared library and loaded while
 * the editor runs, exactly the way a .js is handed to QuickJS. The point is
 * not speed - it is that the engine is written in C++ and a gameplay
 * programmer should not have to leave the language to touch it.
 *
 * WHY A SHARED LIBRARY AND NOT A LINK STEP
 * A behaviour that has to be linked into the editor cannot be edited while the
 * editor is open, and an engine you have to restart to see a one line change
 * in is an engine nobody iterates in. The library is rebuilt when the source
 * is newer than the binary, unloaded, and loaded again - Ctrl+S to running
 * code, no restart.
 *
 * WHY A C STRUCT OF FUNCTION POINTERS AND NOT A C++ BASE CLASS
 * The two sides are separate compilations. A virtual base class would make the
 * behaviour depend on the editor's exact compiler, standard library and build
 * flags: one MSVC/MinGW mismatch and the vtable layout is a crash rather than
 * an error message. A C struct of function pointers has one layout on every
 * compiler on earth.
 *
 * WHAT A BEHAVIOUR LOOKS LIKE
 *
 *     #include <dai_native.h>
 *     DAI_BEHAVIOUR_INIT(api, self)  { api->log(api, "hello"); }
 *     DAI_BEHAVIOUR_FRAME(api, self, dt) {
 *         dai_vec3 p = api->get_position(api, self);
 *         p.y += dt;
 *         api->set_position(api, self, p);
 *     }
 *
 * Neither entry point is required; a file with only a frame function is fine.
 */
#ifndef DAI_NATIVE_H
#define DAI_NATIVE_H

#include <stddef.h>
#include <stdint.h>

#ifndef DAI_API
#  define DAI_API extern
#endif

#ifdef __cplusplus
extern "C" {
#endif

/* Kept structurally identical to dai_vec3 in daidalos.h, and deliberately
 * redeclared: a behaviour compiles against THIS header alone. */
typedef struct dai_nvec3 { float x, y, z; } dai_nvec3;

/* The object the behaviour is attached to. Opaque on purpose - it is the
 * editor's node id, and a behaviour that does arithmetic on it is a behaviour
 * that breaks when node ids change.
 *
 * Named dai_nentity and not dai_entity because the engine already has a
 * dai_entity meaning something else (a scene entity, not a document node), and
 * the host includes both headers. */
typedef uint32_t dai_nentity;

typedef struct dai_native_api dai_native_api;

/* Everything a behaviour can do to the world. Grows by APPENDING - a field
 * inserted in the middle silently rebinds every already compiled behaviour to
 * the wrong function, which is the one bug this design exists to prevent.
 * `abi` is checked on load; a mismatch is refused with a message instead of a
 * crash. */
#define DAI_NATIVE_ABI 2

struct dai_native_api {
    uint32_t  abi;              /* DAI_NATIVE_ABI                              */
    void     *impl;             /* the editor's side, do not touch             */

    void      (*log)(const dai_native_api *api, const char *text);
    dai_nvec3 (*get_position)(const dai_native_api *api, dai_nentity e);
    void      (*set_position)(const dai_native_api *api, dai_nentity e, dai_nvec3 p);
    dai_nvec3 (*get_velocity)(const dai_native_api *api, dai_nentity e);
    void      (*set_velocity)(const dai_native_api *api, dai_nentity e, dai_nvec3 v);
    void      (*add_impulse) (const dai_native_api *api, dai_nentity e, dai_nvec3 i);
    dai_nvec3 (*get_scale)   (const dai_native_api *api, dai_nentity e);
    void      (*set_scale)   (const dai_native_api *api, dai_nentity e, dai_nvec3 s);
    /* Rotation as a quaternion, x y z w. */
    void      (*get_rotation)(const dai_native_api *api, dai_nentity e, float *xyzw);
    void      (*set_rotation)(const dai_native_api *api, dai_nentity e, const float *xyzw);
    /* Find another object by name. 0 = not found. */
    dai_nentity (*find)(const dai_native_api *api, const char *name);
    const char *(*name_of)(const dai_native_api *api, dai_nentity e);
    /* Seconds since play started. */
    float     (*time)(const dai_native_api *api);
    /* Is this key held? Codes are the DAI_KEY_* values from dai_input.h. */
    int       (*key_down)(const dai_native_api *api, uint32_t key);

    /* ---- appended, and appending is the only safe direction --------------
     * The inspector's values for this object's "@param" lines. They existed
     * on the node all along - the editor drew the fields, stored what you
     * typed and then handed a C++ behaviour none of it, which looks exactly
     * like a setting that does nothing.
     *
     * `name` is the @param's name. The fallback is what you get when the
     * field was never touched, so the default written in the file is the
     * default the code uses - one source of truth, not two. */
    double      (*param_num)(const dai_native_api *api, dai_nentity e,
                             const char *name, double fallback);
    const char *(*param_str)(const dai_native_api *api, dai_nentity e,
                             const char *name, const char *fallback);
    /* A "@param node/camera/..." reference: the object it points at, or 0. */
    dai_nentity (*param_node)(const dai_native_api *api, dai_nentity e, const char *name);
    /* How far the pointer moved this frame, and which buttons are down
     * (1 left, 2 right, 4 middle) - the same numbers the JS side gets. */
    void        (*mouse)(const dai_native_api *api, float *dx, float *dy, int *buttons);

    /* ---- components, by name (ABI 2) ------------------------------------
     * A camera's field of view, a light's colour, the words in a Text: each
     * is a field of the node in the document, and this reaches all of them
     * through one pair of calls instead of one pair per field. It is the
     * SAME bridge the JavaScript side uses, with the same names, so
     * self.light.intensity means one thing in the engine and not two.
     *
     * Names are "component.property": "light.intensity", "camera.fov",
     * "text.value", "transform.scale", "image.asset". An unknown name is
     * not a crash - it answers `fallback` - so a behaviour built against a
     * newer editor degrades instead of exploding. */
    double      (*get_num)(const dai_native_api *api, dai_nentity e,
                           const char *prop, double fallback);
    void        (*set_num)(const dai_native_api *api, dai_nentity e,
                           const char *prop, double v);
    dai_nvec3   (*get_vec)(const dai_native_api *api, dai_nentity e, const char *prop);
    void        (*set_vec)(const dai_native_api *api, dai_nentity e,
                           const char *prop, dai_nvec3 v);
    const char *(*get_str)(const dai_native_api *api, dai_nentity e, const char *prop);
    void        (*set_str)(const dai_native_api *api, dai_nentity e,
                           const char *prop, const char *v);
};

/* The two entry points, spelled so a behaviour never has to remember the
 * exact signature or the extern "C". */
#ifdef _WIN32
#  define DAI_BEHAVIOUR_EXPORT __declspec(dllexport)
#else
#  define DAI_BEHAVIOUR_EXPORT __attribute__((visibility("default")))
#endif

#define DAI_BEHAVIOUR_INIT(api, self)                                          \
    extern "C" DAI_BEHAVIOUR_EXPORT void dai_behaviour_init(                   \
        const dai_native_api *api, dai_nentity self)

#define DAI_BEHAVIOUR_FRAME(api, self, dt)                                     \
    extern "C" DAI_BEHAVIOUR_EXPORT void dai_behaviour_frame(                  \
        const dai_native_api *api, dai_nentity self, float dt)

/* The key codes a behaviour needs, spelled here rather than pulled in from
 * dai_render.h. This header is the WHOLE contract - the editor embeds it into
 * the project so a .cpp behaviour compiles with nothing else on the include
 * path - and "include the renderer to ask about the space bar" would break
 * that the moment someone exports a game.
 *
 * The values are the engine's own (X11 keysyms for the named keys, the ASCII
 * code for letters and digits), so 'w' works directly. */
#define DAI_KEY_SPACE     0x0020
#define DAI_KEY_ESCAPE    0xFF1B
#define DAI_KEY_TAB       0xFF09
#define DAI_KEY_RETURN    0xFF0D
#define DAI_KEY_LEFT      0xFF51
#define DAI_KEY_UP        0xFF52
#define DAI_KEY_RIGHT     0xFF53
#define DAI_KEY_DOWN      0xFF54
#define DAI_KEY_SHIFT_L   0xFFE1
#define DAI_KEY_SHIFT_R   0xFFE2
#define DAI_KEY_CTRL_L    0xFFE3
#define DAI_KEY_CTRL_R    0xFFE4
#define DAI_KEY_ALT_L     0xFFE9
#define DAI_KEY_ALT_R     0xFFEA

/* ---- the readable spelling --------------------------------------------
 *
 * C++ only, header only, and it is SUGAR: every call below ends in one of the
 * function pointers above. There is no second path into the engine, and no
 * state - a Node is two pointers wide and copying one costs nothing.
 *
 *     DAI_BEHAVIOUR_FRAME(api, self, dt) {
 *         Node me(api, self);
 *         me.transform().position.x() += 3 * dt;   // reads and writes through
 *         if (me.key('w')) me.rigidbody().velocity = Vec3(0, 0, -6);
 *         me.light().intensity *= 1.01f;
 *         Node cam = me.param_node("followCam");
 *         if (cam) cam.position(me.position() + Vec3(0, 4, 8));
 *     }
 *
 * Why not a base class: see the note at the top of this file. The contract
 * has to stay a C struct, so the convenience lives on TOP of it, in the
 * behaviour's own translation unit, compiled by the behaviour's own compiler.
 */
#ifdef __cplusplus

struct Vec3 {
    float x = 0, y = 0, z = 0;
    Vec3() {}
    Vec3(float ax, float ay, float az) : x(ax), y(ay), z(az) {}
    Vec3(dai_nvec3 v) : x(v.x), y(v.y), z(v.z) {}
    operator dai_nvec3() const { dai_nvec3 v; v.x = x; v.y = y; v.z = z; return v; }
    Vec3 operator+(const Vec3 &o) const { return Vec3(x + o.x, y + o.y, z + o.z); }
    Vec3 operator-(const Vec3 &o) const { return Vec3(x - o.x, y - o.y, z - o.z); }
    Vec3 operator*(float k) const { return Vec3(x * k, y * k, z * k); }
    Vec3 &operator+=(const Vec3 &o) { x += o.x; y += o.y; z += o.z; return *this; }
    float length() const { float d = x * x + y * y + z * z; return d > 0 ? __builtin_sqrtf(d) : 0; }
    Vec3 normalised() const { float l = length(); return l > 1e-6f ? Vec3(x / l, y / l, z / l) : Vec3(); }
};

class Node;

/* A property that reads and writes through to the engine, so `me.position.x
 * += 1` does what it says. It is the one clever thing in this header, and it
 * is clever exactly once: the alternative is get/modify/set at every call
 * site, which is where the "I set X and lost Y" bugs come from. */
class Vec3Prop {
public:
    Vec3Prop(const dai_native_api *a, dai_nentity e, int which) : api(a), ent(e), w(which) {}
    operator Vec3() const { return get(); }
    Vec3Prop &operator=(const Vec3 &v) { set(v); return *this; }
    Vec3Prop &operator+=(const Vec3 &v) { set(get() + v); return *this; }
    Vec3 operator+(const Vec3 &v) const { return get() + v; }
    Vec3 operator-(const Vec3 &v) const { return get() - v; }
    struct Comp {
        Vec3Prop *p; int i;
        operator float() const { Vec3 v = p->get(); return i == 0 ? v.x : (i == 1 ? v.y : v.z); }
        Comp &operator=(float f) { Vec3 v = p->get(); (i == 0 ? v.x : (i == 1 ? v.y : v.z)) = f; p->set(v); return *this; }
        Comp &operator+=(float f) { return *this = (float)*this + f; }
        Comp &operator-=(float f) { return *this = (float)*this - f; }
        Comp &operator*=(float f) { return *this = (float)*this * f; }
    };
    /* Assignable components: me.transform.position.y() += 3 * dt. */
    Comp x() { return Comp{ this, 0 }; }
    Comp y() { return Comp{ this, 1 }; }
    Comp z() { return Comp{ this, 2 }; }
    Comp x_() { return Comp{ this, 0 }; }   /* the old spelling, still valid */
    Vec3 get() const {
        if (w == 0) return api->get_position(api, ent);
        if (w == 1) return api->get_velocity(api, ent);
        return api->get_scale(api, ent);
    }
    void set(const Vec3 &v) const {
        if (w == 0) api->set_position(api, ent, v);
        else if (w == 1) api->set_velocity(api, ent, v);
        else api->set_scale(api, ent, v);
    }
private:
    const dai_native_api *api; dai_nentity ent; int w;
};

/* A property that is one number in the document: light.intensity,
 * camera.fov, text.size. Assignable and readable in the same place the
 * engine keeps it, so there is no local copy to forget to write back. */
class NumProp {
public:
    NumProp(const dai_native_api *a, dai_nentity e, const char *p) : api(a), ent(e), prop(p) {}
    operator float() const { return get(); }
    NumProp &operator=(float v) { set(v); return *this; }
    NumProp &operator+=(float v) { set(get() + v); return *this; }
    NumProp &operator-=(float v) { set(get() - v); return *this; }
    NumProp &operator*=(float v) { set(get() * v); return *this; }
    float get() const {
        return (api && api->get_num) ? (float)api->get_num(api, ent, prop, 0.0) : 0.0f;
    }
    void set(float v) const { if (api && api->set_num) api->set_num(api, ent, prop, v); }
private:
    const dai_native_api *api; dai_nentity ent; const char *prop;
};

/* The same for a flag, so `me.light.enabled = false` reads like a sentence
 * rather than like an integer. */
class BoolProp {
public:
    BoolProp(const dai_native_api *a, dai_nentity e, const char *p) : api(a), ent(e), prop(p) {}
    operator bool() const { return (api && api->get_num) ? api->get_num(api, ent, prop, 0.0) != 0.0 : false; }
    BoolProp &operator=(bool v) { if (api && api->set_num) api->set_num(api, ent, prop, v ? 1.0 : 0.0); return *this; }
private:
    const dai_native_api *api; dai_nentity ent; const char *prop;
};

/* A three component property that is NOT the transform - a light colour, a
 * sprite size. Same read-through/write-through contract as Vec3Prop. */
class NamedVec3Prop {
public:
    NamedVec3Prop(const dai_native_api *a, dai_nentity e, const char *p) : api(a), ent(e), prop(p) {}
    operator Vec3() const { return get(); }
    NamedVec3Prop &operator=(const Vec3 &v) { set(v); return *this; }
    Vec3 get() const {
        if (!api || !api->get_vec) return Vec3();
        return Vec3(api->get_vec(api, ent, prop));
    }
    void set(const Vec3 &v) const { if (api && api->set_vec) api->set_vec(api, ent, prop, v); }
private:
    const dai_native_api *api; dai_nentity ent; const char *prop;
};

/* Words in the document. Reading gives a pointer the engine owns, valid for
 * this frame - copy it if you want to keep it. */
class StrProp {
public:
    StrProp(const dai_native_api *a, dai_nentity e, const char *p) : api(a), ent(e), prop(p) {}
    operator const char *() const { return get(); }
    StrProp &operator=(const char *v) { set(v); return *this; }
    const char *get() const { return (api && api->get_str) ? api->get_str(api, ent, prop) : ""; }
    void set(const char *v) const { if (api && api->set_str) api->set_str(api, ent, prop, v); }
private:
    const dai_native_api *api; dai_nentity ent; const char *prop;
};

/* ---- the components ----------------------------------------------------
 *
 * One class per component, exactly the way the inspector shows them and the
 * way the JavaScript side spells them:
 *
 *     me.transform.position.y() += 3 * dt;
 *     me.rigidbody.velocity = Vec3(0, 6, 0);
 *     me.light.intensity   *= 2;
 *     me.text.value         = "score: 3";
 *
 * They hold an api pointer and a node id and nothing else, so one is made and
 * thrown away per statement without allocating anything.
 */
class Transform {
public:
    Transform(const dai_native_api *a, dai_nentity e)
        : position(a, e, 0), scale(a, e, 2), api(a), ent(e) {}
    Vec3Prop position;
    Vec3Prop scale;
    /* Yaw in DEGREES around Y - the number anyone actually has in mind. */
    void yaw(float degrees) {
        float hh = degrees * 3.14159265f / 360.0f;
        float q[4] = { 0, __builtin_sinf(hh), 0, __builtin_cosf(hh) };
        api->set_rotation(api, ent, q);
    }
    void rotation(const float *xyzw) { api->set_rotation(api, ent, xyzw); }
    void rotation(float *out) const { api->get_rotation(api, ent, out); }
    void translate(const Vec3 &d) { position = position.get() + d; }
private:
    const dai_native_api *api; dai_nentity ent;
};

class Rigidbody {
public:
    Rigidbody(const dai_native_api *a, dai_nentity e)
        : velocity(a, e, 1),
          density(a, e, "rigidbody.density"),
          friction(a, e, "rigidbody.friction"),
          restitution(a, e, "rigidbody.restitution"),
          trigger(a, e, "rigidbody.trigger"),
          enabled(a, e, "rigidbody.enabled"),
          api(a), ent(e) {}
    Vec3Prop velocity;
    NumProp  density, friction, restitution;
    BoolProp trigger, enabled;
    void impulse(const Vec3 &v) { api->add_impulse(api, ent, v); }
    /* Standing on something. Not a raycast: a body that is neither rising nor
     * sinking measurably is resting on whatever is under it. */
    bool grounded() const {
        Vec3 v = api->get_velocity(api, ent);
        return v.y > -0.35f && v.y < 0.35f;
    }
private:
    const dai_native_api *api; dai_nentity ent;
};

class Camera {
public:
    Camera(const dai_native_api *a, dai_nentity e)
        : mode(a, e, "camera.mode"), fov(a, e, "camera.fov"), size(a, e, "camera.size"),
          enabled(a, e, "camera.enabled") {}
    NumProp  mode;          /* 0 none, 1 perspective, 2 orthographic */
    NumProp  fov;           /* perspective, degrees */
    NumProp  size;          /* orthographic, half the visible height */
    BoolProp enabled;
    bool orthographic() const { return (float)mode == 2.0f; }
};

class Light {
public:
    Light(const dai_native_api *a, dai_nentity e)
        : mode(a, e, "light.mode"), range(a, e, "light.range"),
          intensity(a, e, "light.intensity"), cone(a, e, "light.cone"),
          color(a, e, "light.color"), enabled(a, e, "light.enabled") {}
    NumProp        mode;    /* 0 none, 1 point, 2 spot, 3 sun */
    NumProp        range, intensity, cone;
    NamedVec3Prop  color;
    BoolProp       enabled;
};

class Text {
public:
    Text(const dai_native_api *a, dai_nentity e)
        : value(a, e, "text.value"), size(a, e, "text.size"), anchor(a, e, "text.anchor"),
          color(a, e, "text.color"), enabled(a, e, "text.enabled") {}
    StrProp       value;
    NumProp       size, anchor;
    NamedVec3Prop color;
    BoolProp      enabled;
};

class Image {
public:
    Image(const dai_native_api *a, dai_nentity e)
        : asset(a, e, "image.asset"), size(a, e, "image.size"), enabled(a, e, "image.enabled") {}
    StrProp       asset;
    NamedVec3Prop size;
    BoolProp      enabled;
};

class Node {
public:
    Node() : api(nullptr), ent(0) {}
    Node(const dai_native_api *a, dai_nentity e) : api(a), ent(e) {}
    explicit operator bool() const { return api != nullptr && ent != 0; }
    dai_nentity id() const { return ent; }

    Vec3 position() const { return api->get_position(api, ent); }
    void position(const Vec3 &v) { api->set_position(api, ent, v); }
    Vec3 velocity() const { return api->get_velocity(api, ent); }
    void velocity(const Vec3 &v) { api->set_velocity(api, ent, v); }
    Vec3 scale() const { return api->get_scale(api, ent); }
    void scale(const Vec3 &v) { api->set_scale(api, ent, v); }
    void impulse(const Vec3 &v) { api->add_impulse(api, ent, v); }
    const char *name() const { return api->name_of(api, ent); }

    /* Yaw in DEGREES around Y - the number anyone actually has in mind. */
    void yaw(float degrees) {
        float h = degrees * 3.14159265f / 360.0f;
        float q[4] = { 0, __builtin_sinf(h), 0, __builtin_cosf(h) };
        api->set_rotation(api, ent, q);
    }

    bool key(unsigned code) const { return api->key_down(api, code) != 0; }
    float time() const { return api->time(api); }
    void log(const char *text) const { api->log(api, text); }
    Node find(const char *n) const { return Node(api, api->find(api, n)); }

    /* Standing on something. Not a raycast: a body that is neither rising nor
     * sinking measurably is resting on whatever is under it. */
    bool grounded() const {
        Vec3 v = velocity();
        return v.y > -0.35f && v.y < 0.35f;
    }

    /* The inspector's values. */
    float param(const char *n, float fallback = 0.0f) const {
        return api->param_num ? (float)api->param_num(api, ent, n, fallback) : fallback;
    }
    const char *param_text(const char *n, const char *fallback = "") const {
        return api->param_str ? api->param_str(api, ent, n, fallback) : fallback;
    }
    Node param_node(const char *n) const {
        return api->param_node ? Node(api, api->param_node(api, ent, n)) : Node();
    }

    /* ---- the components -------------------------------------------------
     * Methods rather than members on purpose: a Node stays two words wide and
     * costs nothing to copy, which is what the header promises at the top.
     * The component is built where it is used and dies at the semicolon.
     *
     * The old spellings above - position(), velocity(), scale(), impulse() -
     * are not going anywhere. Everything a behaviour was written against a
     * year ago still compiles. */
    ::Transform transform() const { return ::Transform(api, ent); }
    ::Rigidbody rigidbody() const { return ::Rigidbody(api, ent); }
    ::Camera    camera()    const { return ::Camera(api, ent); }
    ::Light     light()     const { return ::Light(api, ent); }
    ::Text      text()      const { return ::Text(api, ent); }
    ::Image     image()     const { return ::Image(api, ent); }

    const dai_native_api *api;
    dai_nentity ent;
};

#endif /* __cplusplus */

/* ---- the editor's side ------------------------------------------------- */

typedef struct dai_native dai_native;

/* `cache_dir` is where the built libraries go - one per source file, next to
 * a stamp of the source's mtime so an unchanged file is not rebuilt. */
DAI_API dai_native *dai_native_create(const char *cache_dir);
DAI_API void        dai_native_destroy(dai_native *n);

/* The C++ compiler this machine has, or NULL. Checked once. The editor shows
 * this: "no C++ compiler found" is a sentence a user can act on, an empty
 * inspector is not. */
DAI_API const char *dai_native_compiler(dai_native *n);

/* Compile if needed, then load. Returns a behaviour id, or -1 with the reason
 * in `err` - which is the compiler's own output, because a build error the
 * user cannot read is a build error they cannot fix. */
DAI_API int  dai_native_load(dai_native *n, const char *source_path,
                             const char *include_dir, char *err, size_t err_len);
DAI_API void dai_native_unload(dai_native *n, int id);
/* 1 when the source is newer than what is loaded - the editor polls this and
 * reloads, which is the hot reload. */
DAI_API int  dai_native_stale(dai_native *n, int id);

DAI_API void dai_native_init(dai_native *n, int id, const dai_native_api *api, dai_nentity self);
DAI_API void dai_native_frame(dai_native *n, int id, const dai_native_api *api,
                              dai_nentity self, float dt);

#ifdef __cplusplus
}
#endif

#endif /* DAI_NATIVE_H */
