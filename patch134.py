import io

# ===========================================================================
# 1) Ein .cpp-Behaviour bekommt seine Inspector-Werte. Bisher zeigte der
#    Inspector Felder fuer .cpp-Dateien, speicherte sie am Knoten - und der
#    Code sah nie einen davon. Das ist schlimmer als kein Feld: es sieht aus,
#    als haette man etwas eingestellt.
#
# 2) Und eine Huelle, damit man nicht api->get_position(api, self) schreibt.
#    Header-only, ueber genau denselben Funktionszeigern - kein zweiter Weg
#    in die Engine, nur eine lesbare Schreibweise fuer den einen.
# ===========================================================================
p = 'include/dai_native.h'
s = io.open(p, encoding='utf-8').read()

old = """    /* Is this key held? Codes are the DAI_KEY_* values from dai_input.h. */
    int       (*key_down)(const dai_native_api *api, uint32_t key);
};"""
new = """    /* Is this key held? Codes are the DAI_KEY_* values from dai_input.h. */
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
};"""
assert s.count(old) == 1, 'native api struct end not found'
s = s.replace(old, new)

old = """/* ---- the editor's side ------------------------------------------------- */"""
new = """/* ---- the readable spelling --------------------------------------------
 *
 * C++ only, header only, and it is SUGAR: every call below ends in one of the
 * function pointers above. There is no second path into the engine, and no
 * state - a Node is two pointers wide and copying one costs nothing.
 *
 *     DAI_BEHAVIOUR_FRAME(api, self, dt) {
 *         Node me(api, self);
 *         me.position.x += 3 * dt;              // reads and writes through
 *         if (me.key('w')) me.velocity = Vec3(0, 0, -6);
 *         Node cam = me.param_node("followCam");
 *         if (cam) cam.position = me.position + Vec3(0, 4, 8);
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
    Comp x_() { return Comp{ this, 0 }; }
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

    const dai_native_api *api;
    dai_nentity ent;
};

#endif /* __cplusplus */

/* ---- the editor's side ------------------------------------------------- */"""
assert s.count(old) == 1, 'editor side anchor not found'
s = s.replace(old, new)
io.open(p, 'w', encoding='utf-8').write(s)
print('dai_native.h: params and the readable spelling')
