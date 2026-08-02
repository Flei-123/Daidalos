// Docked panels: a tree of splits with tabbed leaves. See include/dai_dock.h
// for why it is a tree and not a set of edges.
//
// No Vulkan, no editor knowledge: this file turns a tree plus a mouse into
// rectangles and tab bars, and draws them with dai_ui primitives.

#include "dai_dock.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

namespace {

const float TAB_H    = 21.0f;   // height of a tab bar
const float SPLIT_W  = 4.0f;    // grab width of a splitter
const float MIN_SIDE = 60.0f;   // a panel narrower than this is not a panel
const float DROP_ZONE_FRAC = 0.30f;
const float DROP_ZONE_MAX  = 120.0f;

struct Rect { float x = 0, y = 0, w = 0, h = 0; };

bool hit(const Rect &r, float x, float y) {
    return x >= r.x && x < r.x + r.w && y >= r.y && y < r.y + r.h;
}

// A node is either a split (axis 1 = side by side, 2 = stacked) with exactly
// two children, or a leaf with tabs. Exactly two children is what keeps the
// tree simple: three panels in a row are a split whose child is another split,
// which is also how the ratios stay independent of each other.
struct Node {
    int   axis = 0;              // 0 = leaf
    float ratio = 0.5f;          // first child's share of the axis
    Node *a = nullptr, *b = nullptr, *parent = nullptr;
    std::vector<std::string> tabs;
    int   selected = 0;
    Rect  rect;                  // recomputed every frame
    Rect  body;                  // rect minus the tab bar

    bool leaf() const { return axis == 0; }
};

void free_tree(Node *n) {
    if (!n) return;
    free_tree(n->a);
    free_tree(n->b);
    delete n;
}

Node *find_tab(Node *n, const std::string &title, int *index) {
    if (!n) return nullptr;
    if (n->leaf()) {
        for (size_t i = 0; i < n->tabs.size(); ++i)
            if (n->tabs[i] == title) { if (index) *index = (int)i; return n; }
        return nullptr;
    }
    Node *r = find_tab(n->a, title, index);
    return r ? r : find_tab(n->b, title, index);
}

Node *first_leaf(Node *n) {
    if (!n) return nullptr;
    if (n->leaf()) return n;
    Node *r = first_leaf(n->a);
    return r ? r : first_leaf(n->b);
}

void collect_leaves(Node *n, std::vector<Node *> *out) {
    if (!n) return;
    if (n->leaf()) { out->push_back(n); return; }
    collect_leaves(n->a, out);
    collect_leaves(n->b, out);
}

} // namespace

struct dai_dock {
    Node *root = nullptr;

    // A floating root is a tree of its own with a screen rectangle. Dragging a
    // tab out of the layout makes one; dropping its last tab back into the
    // layout destroys it. Exactly Unity's ContainerWindow, minus the OS.
    struct Floating {
        Node *root = nullptr;
        Rect  rect;
        int   z = 0;
    };
    std::vector<Floating> floats;
    int next_z = 1;

    // Registration, so a panel that was never seen before lands somewhere
    // sensible and one that has been dragged keeps its place.
    struct Reg { std::string title; int edge; float frac; std::string next_to; };
    std::vector<Reg> regs;
    std::vector<std::string> closed;

    dai_ui *ui = nullptr;
    Rect area;

    // ---- interaction state -------------------------------------------------
    // Splitter being dragged, identified by the node whose ratio it changes.
    Node *split_drag = nullptr;
    // Tab being dragged. The title is the identity - node pointers do not
    // survive the tree surgery that a drop performs.
    std::string drag_title;
    bool  dragging = false;
    bool  drag_armed = false;      // pressed on a tab, not yet past the threshold
    float press_x = 0, press_y = 0;
    float grab_dx = 0, grab_dy = 0;
    Rect  drag_preview;
    int   drag_kind = -1;          // -1 none, 0 tab, 1 left, 2 right, 3 top, 4 bottom, 5 float
    Node *drop_node = nullptr;
    Floating *drag_float = nullptr;   // moving a whole floating window
    int   panel_depth = 0;
    bool  prev_down = false;

    Node *find(const std::string &t, int *idx) {
        Node *n = find_tab(root, t, idx);
        if (n) return n;
        for (auto &f : floats) { n = find_tab(f.root, t, idx); if (n) return n; }
        return nullptr;
    }
    bool is_closed(const std::string &t) const {
        return std::find(closed.begin(), closed.end(), t) != closed.end();
    }
};

namespace {

// ---- tree surgery ---------------------------------------------------------

// Removes a tab; if that empties its leaf, the leaf goes away and its sibling
// takes the parent's place. This is what keeps a tree from filling up with
// empty rectangles - Unity calls it KillIfEmpty, and without it dragging the
// last tab out of a panel leaves a hole nothing can fill.
void remove_tab(dai_dock *d, const std::string &title) {
    int idx = -1;
    Node *leaf = d->find(title, &idx);
    if (!leaf || idx < 0) return;
    leaf->tabs.erase(leaf->tabs.begin() + idx);
    if (leaf->selected >= (int)leaf->tabs.size())
        leaf->selected = (int)leaf->tabs.size() - 1;
    if (leaf->selected < 0) leaf->selected = 0;
    if (!leaf->tabs.empty()) return;

    Node *parent = leaf->parent;
    if (!parent) {
        // The root of a tree. If it is a floating one, the window closes.
        for (size_t i = 0; i < d->floats.size(); ++i) {
            if (d->floats[i].root != leaf) continue;
            free_tree(d->floats[i].root);
            d->floats.erase(d->floats.begin() + (long)i);
            return;
        }
        return;   // the main root always exists, even empty
    }
    Node *sib = (parent->a == leaf) ? parent->b : parent->a;
    // The sibling is promoted into the parent's slot, which makes its
    // rectangle grow to the parent's. If it is a split along the SAME axis,
    // its ratio was relative to its old, smaller rectangle - rescale it, or
    // promoting it visibly moves every panel inside it.
    if (!sib->leaf() && sib->axis == parent->axis) {
        float pe = parent->axis == 1 ? parent->rect.w : parent->rect.h;
        float se = parent->axis == 1 ? sib->rect.w : sib->rect.h;
        if (pe > 1.0f && se > 1.0f) {
            sib->ratio = sib->ratio * se / pe;
            if (sib->ratio < 0.05f) sib->ratio = 0.05f;
            if (sib->ratio > 0.95f) sib->ratio = 0.95f;
        }
    }
    // Copying the sibling's contents into the parent (rather than re-pointing
    // the grandparent) keeps every other pointer in the tree valid.
    Node *gp = parent->parent;
    sib->parent = gp;
    if (gp) {
        if (gp->a == parent) gp->a = sib; else gp->b = sib;
    } else {
        if (d->root == parent) d->root = sib;
        for (auto &f : d->floats) if (f.root == parent) f.root = sib;
    }
    parent->a = parent->b = nullptr;
    delete leaf;
    delete parent;
}

// Splits `target` and puts `title` in the new half. `edge` is which half the
// new panel takes: 1 left, 2 right, 3 top, 4 bottom.
void split_into(dai_dock *d, Node *target, const std::string &title, int edge, float frac) {
    Node *moved = new Node();          // takes over what target held
    moved->axis = target->axis;
    moved->ratio = target->ratio;
    moved->a = target->a; moved->b = target->b;
    moved->tabs = target->tabs;
    moved->selected = target->selected;
    if (moved->a) moved->a->parent = moved;
    if (moved->b) moved->b->parent = moved;

    Node *fresh = new Node();
    fresh->tabs.push_back(title);

    target->axis = (edge == 1 || edge == 2) ? 1 : 2;
    target->tabs.clear();
    bool first = (edge == 1 || edge == 3);
    target->a = first ? fresh : moved;
    target->b = first ? moved : fresh;
    target->a->parent = target;
    target->b->parent = target;
    target->ratio = first ? frac : 1.0f - frac;
    (void)d;
}

// `select` is what tells a DROP apart from a REGISTRATION: dropping a tab
// into a bar selects it (you just put it there), registering a second view
// must not - otherwise opening the editor shows the Game tab because it was
// registered after the Scene tab.
void add_tab_to(Node *leaf, const std::string &title, bool select, int at = -1) {
    if (!leaf->leaf()) leaf = first_leaf(leaf);
    if (!leaf) return;
    if (at < 0 || at > (int)leaf->tabs.size()) at = (int)leaf->tabs.size();
    leaf->tabs.insert(leaf->tabs.begin() + at, title);
    if (select) leaf->selected = at;
    else if (leaf->selected >= at) leaf->selected++;   // keep pointing at the same tab
    if (leaf->selected >= (int)leaf->tabs.size()) leaf->selected = (int)leaf->tabs.size() - 1;
    if (leaf->selected < 0) leaf->selected = 0;
}

// ---- layout ---------------------------------------------------------------

void layout(Node *n, Rect r) {
    if (!n) return;
    n->rect = r;
    if (n->leaf()) {
        n->body = Rect{ r.x, r.y + TAB_H, r.w, r.h - TAB_H };
        if (n->body.h < 0) n->body.h = 0;
        return;
    }
    if (n->ratio < 0.05f) n->ratio = 0.05f;
    if (n->ratio > 0.95f) n->ratio = 0.95f;
    if (n->axis == 1) {
        float aw = std::floor(r.w * n->ratio);
        if (aw < MIN_SIDE) aw = std::min(MIN_SIDE, r.w * 0.5f);
        if (r.w - aw < MIN_SIDE) aw = std::max(r.w - MIN_SIDE, 0.0f);
        layout(n->a, Rect{ r.x, r.y, aw, r.h });
        layout(n->b, Rect{ r.x + aw, r.y, r.w - aw, r.h });
    } else {
        float ah = std::floor(r.h * n->ratio);
        if (ah < MIN_SIDE) ah = std::min(MIN_SIDE, r.h * 0.5f);
        if (r.h - ah < MIN_SIDE) ah = std::max(r.h - MIN_SIDE, 0.0f);
        layout(n->a, Rect{ r.x, r.y, r.w, ah });
        layout(n->b, Rect{ r.x, r.y + ah, r.w, r.h - ah });
    }
}

// ---- drawing --------------------------------------------------------------

float tab_width(dai_ui *ui, const std::string &t) {
    return dai_ui_text_width(ui, t.c_str()) + 26.0f;
}

// Which tab of this leaf is under x, and where that tab starts.
int tab_at(dai_ui *ui, const Node *leaf, float mx, float *out_x, float *out_w) {
    float tx = leaf->rect.x;
    for (size_t i = 0; i < leaf->tabs.size(); ++i) {
        float tw = tab_width(ui, leaf->tabs[i]);
        if (mx >= tx && mx < tx + tw) {
            if (out_x) *out_x = tx;
            if (out_w) *out_w = tw;
            return (int)i;
        }
        tx += tw;
    }
    return -1;
}

void draw_tab_bar(dai_dock *d, Node *leaf, bool focused) {
    dai_ui *ui = d->ui;
    const dai_ui_style *st = dai_ui_style_of(ui);
    Rect r = leaf->rect;
    dai_ui_rect(ui, r.x, r.y, r.w, TAB_H, st->chrome);

    float mx = 0, my = 0;
    int down = 0, pressed = 0;
    dai_ui_mouse(ui, &mx, &my, &down, &pressed);
    bool over_bar = my >= r.y && my < r.y + TAB_H && mx >= r.x && mx < r.x + r.w;

    float tx = r.x;
    for (size_t i = 0; i < leaf->tabs.size(); ++i) {
        const std::string &t = leaf->tabs[i];
        float tw = tab_width(ui, t);
        bool sel = (int)i == leaf->selected;
        bool over = over_bar && mx >= tx && mx < tx + tw;
        uint32_t bg = sel ? st->panel : (over ? st->titlebar_focused : st->titlebar);
        dai_ui_rect(ui, tx, r.y, tw - 1.0f, TAB_H, bg);
        if (sel && focused) dai_ui_rect(ui, tx, r.y, tw - 1.0f, 2.0f, st->accent);
        dai_ui_text(ui, tx + 8.0f, r.y + 3.0f, t.c_str(), sel ? st->text : st->text_dim);
        tx += tw;
    }
    // The ⋮ button of the leaf, at the right end of its bar.
    float bx = r.x + r.w - 16.0f, by = r.y + 4.0f;
    bool over_menu = over_bar && mx >= bx - 2.0f && mx < bx + 12.0f;
    uint32_t col = over_menu ? st->text : st->text_dim;
    for (int k = -1; k <= 1; ++k)
        dai_ui_rect(ui, bx + 4.0f, by + 5.0f + (float)k * 4.0f, 2.0f, 2.0f, col);
}

} // namespace

extern "C" {

dai_dock *dai_dock_create(void) {
    dai_dock *d = new dai_dock();
    d->root = new Node();      // an empty leaf; the first panel lands in it
    return d;
}

void dai_dock_destroy(dai_dock *d) {
    if (!d) return;
    free_tree(d->root);
    for (auto &f : d->floats) free_tree(f.root);
    delete d;
}

void dai_dock_add(dai_dock *d, const char *title, int edge, float fraction) {
    if (!d || !title || !*title) return;
    for (const auto &r : d->regs) if (r.title == title) return;    // already known
    d->regs.push_back(dai_dock::Reg{ title, edge, fraction > 0.02f ? fraction : 0.22f, "" });

    if (d->find(title, nullptr)) return;
    Node *centre = first_leaf(d->root);
    if (!centre) { centre = d->root = new Node(); }
    if (centre->tabs.empty() && edge == DAI_DOCK_NONE) {
        centre->tabs.push_back(title);
        centre->selected = 0;
        return;
    }
    if (edge == DAI_DOCK_NONE) { add_tab_to(centre, title, false); return; }
    // Split the WHOLE layout, not the centre leaf: docking left means the left
    // of everything, which is what a fresh editor layout means by it.
    int e = edge == DAI_DOCK_LEFT ? 1 : edge == DAI_DOCK_RIGHT ? 2
          : edge == DAI_DOCK_TOP ? 3 : 4;
    split_into(d, d->root, title, e, fraction > 0.02f ? fraction : 0.22f);
}

void dai_dock_add_tab(dai_dock *d, const char *title, const char *next_to) {
    if (!d || !title || !*title) return;
    for (const auto &r : d->regs) if (r.title == title) return;
    d->regs.push_back(dai_dock::Reg{ title, DAI_DOCK_NONE, 0.22f, next_to ? next_to : "" });
    if (d->find(title, nullptr)) return;
    Node *host = next_to ? d->find(next_to, nullptr) : nullptr;
    if (!host) host = first_leaf(d->root);
    if (host) add_tab_to(host, title, false);
}

void dai_dock_reset(dai_dock *d) {
    if (!d) return;
    free_tree(d->root);
    for (auto &f : d->floats) free_tree(f.root);
    d->floats.clear();
    d->root = new Node();
    std::vector<dai_dock::Reg> regs = d->regs;
    d->regs.clear();
    d->closed.clear();
    for (const auto &r : regs) {
        if (r.next_to.empty()) dai_dock_add(d, r.title.c_str(), r.edge, r.frac);
        else                   dai_dock_add_tab(d, r.title.c_str(), r.next_to.c_str());
    }
}

int dai_dock_visible(const dai_dock *d, const char *title) {
    if (!d || !title) return 0;
    dai_dock *m = const_cast<dai_dock *>(d);
    if (m->is_closed(title)) return 0;
    int idx = -1;
    Node *leaf = m->find(title, &idx);
    return (leaf && idx == leaf->selected) ? 1 : 0;
}

void dai_dock_focus(dai_dock *d, const char *title) {
    if (!d || !title) return;
    int idx = -1;
    Node *leaf = d->find(title, &idx);
    if (leaf && idx >= 0) leaf->selected = idx;
    for (auto &f : d->floats)
        if (find_tab(f.root, title, nullptr)) f.z = ++d->next_z;
}

void dai_dock_close(dai_dock *d, const char *title) {
    if (!d || !title) return;
    if (!d->is_closed(title)) d->closed.push_back(title);
    remove_tab(d, title);
}

int dai_dock_is_open(const dai_dock *d, const char *title) {
    if (!d || !title) return 0;
    return const_cast<dai_dock *>(d)->is_closed(title) ? 0 : 1;
}

uint32_t dai_dock_panels(const dai_dock *d, const char **out, uint32_t max) {
    if (!d) return 0;
    uint32_t n = 0;
    for (const auto &r : d->regs) {
        if (out && n < max) out[n] = r.title.c_str();
        ++n;
    }
    return n;
}

// ---------------------------------------------------------------- the frame

void dai_dock_begin(dai_dock *d, dai_ui *ui, float x, float y, float w, float h) {
    if (!d || !ui) return;
    d->ui = ui;
    d->area = Rect{ x, y, w, h };

    // Panels that were registered but are not in the tree (a fresh session, a
    // reopened panel) get put back where they were registered.
    for (const auto &r : d->regs) {
        if (d->is_closed(r.title)) continue;
        if (d->find(r.title, nullptr)) continue;
        if (r.next_to.empty()) {
            Node *centre = first_leaf(d->root);
            if (r.edge == DAI_DOCK_NONE && centre) add_tab_to(centre, r.title, false);
            else {
                int e = r.edge == DAI_DOCK_LEFT ? 1 : r.edge == DAI_DOCK_RIGHT ? 2
                      : r.edge == DAI_DOCK_TOP ? 3 : 4;
                split_into(d, d->root, r.title, e, r.frac);
            }
        } else {
            Node *host = d->find(r.next_to, nullptr);
            add_tab_to(host ? host : first_leaf(d->root), r.title, false);
        }
    }

    layout(d->root, d->area);
    for (auto &f : d->floats) {
        // Keep floating windows on the surface: one dragged off the bottom can
        // never be grabbed again.
        if (f.rect.x > x + w - 60.0f) f.rect.x = x + w - 60.0f;
        if (f.rect.y > y + h - TAB_H) f.rect.y = y + h - TAB_H;
        if (f.rect.x + f.rect.w < x + 60.0f) f.rect.x = x + 60.0f - f.rect.w;
        if (f.rect.y < y) f.rect.y = y;
        layout(f.root, f.rect);
    }

    float mx = 0, my = 0;
    int down = 0, pressed = 0;
    dai_ui_mouse(ui, &mx, &my, &down, &pressed);
    bool released = !down && d->prev_down;
    d->prev_down = down != 0;

    // ---- splitters -------------------------------------------------------
    // Dragging one changes ONE node's ratio, which moves the two panels either
    // side of it and nothing else. That is what "the split between them" means
    // and why neighbours resize together.
    std::vector<Node *> stack{ d->root };
    for (auto &f : d->floats) stack.push_back(f.root);
    std::vector<Node *> splits;
    while (!stack.empty()) {
        Node *n = stack.back(); stack.pop_back();
        if (!n || n->leaf()) continue;
        splits.push_back(n);
        stack.push_back(n->a);
        stack.push_back(n->b);
    }
    if (d->split_drag && down) {
        Node *n = d->split_drag;
        if (n->axis == 1 && n->rect.w > 1.0f)
            n->ratio = (mx - n->rect.x) / n->rect.w;
        else if (n->axis == 2 && n->rect.h > 1.0f)
            n->ratio = (my - n->rect.y) / n->rect.h;
        dai_ui_cursor_set(ui, n->axis == 1 ? DAI_CURSOR_SIZE_WE : DAI_CURSOR_SIZE_NS);
        dai_ui_claim_mouse(ui);
        layout(d->root, d->area);
        for (auto &f : d->floats) layout(f.root, f.rect);
    } else if (!down) {
        d->split_drag = nullptr;
    }
    if (!d->split_drag && !d->dragging) {
        for (Node *n : splits) {
            Rect sr;
            if (n->axis == 1) sr = Rect{ n->a->rect.x + n->a->rect.w - SPLIT_W * 0.5f,
                                         n->rect.y, SPLIT_W, n->rect.h };
            else              sr = Rect{ n->rect.x, n->a->rect.y + n->a->rect.h - SPLIT_W * 0.5f,
                                         n->rect.w, SPLIT_W };
            if (!hit(sr, mx, my)) continue;
            dai_ui_cursor_set(ui, n->axis == 1 ? DAI_CURSOR_SIZE_WE : DAI_CURSOR_SIZE_NS);
            dai_ui_claim_mouse(ui);
            if (pressed) d->split_drag = n;
            break;
        }
    }

    // ---- tab bars, and picking a tab up ----------------------------------
    std::vector<Node *> leaves;
    collect_leaves(d->root, &leaves);
    std::vector<std::pair<int, dai_dock::Floating *>> order;
    for (auto &f : d->floats) order.push_back({ f.z, &f });
    std::sort(order.begin(), order.end(),
              [](const std::pair<int, dai_dock::Floating *> &a,
                 const std::pair<int, dai_dock::Floating *> &b) { return a.first < b.first; });

    const dai_ui_style *st = dai_ui_style_of(ui);
    for (Node *leaf : leaves) draw_tab_bar(d, leaf, true);
    for (auto &kv : order) {
        dai_dock::Floating *f = kv.second;
        dai_ui_layer_push(ui, DAI_LAYER_WINDOW + 200 + f->z);
        dai_ui_rect(ui, f->rect.x + 3.0f, f->rect.y + 3.0f, f->rect.w, f->rect.h, st->shadow);
        dai_ui_rect(ui, f->rect.x, f->rect.y, f->rect.w, f->rect.h, st->panel);
        dai_ui_rect_outline(ui, f->rect.x, f->rect.y, f->rect.w, f->rect.h, 1.0f, st->panel_border);
        std::vector<Node *> fl;
        collect_leaves(f->root, &fl);
        for (Node *leaf : fl) draw_tab_bar(d, leaf, true);
        dai_ui_layer_pop(ui);
    }

    // Which tab is under the pointer, in the frontmost thing that has one?
    Node *hit_leaf = nullptr;
    int   hit_tab = -1;
    float hit_x = 0, hit_w = 0;
    dai_dock::Floating *hit_float = nullptr;
    for (auto it = order.rbegin(); it != order.rend() && !hit_leaf; ++it) {
        dai_dock::Floating *f = it->second;
        if (!hit(f->rect, mx, my)) continue;
        std::vector<Node *> fl;
        collect_leaves(f->root, &fl);
        for (Node *leaf : fl) {
            if (my < leaf->rect.y || my >= leaf->rect.y + TAB_H) continue;
            int t = tab_at(ui, leaf, mx, &hit_x, &hit_w);
            if (t >= 0) { hit_leaf = leaf; hit_tab = t; hit_float = f; break; }
        }
        if (!hit_leaf) hit_float = f;     // the window itself, for raising/moving
    }
    if (!hit_leaf && !hit_float) {
        for (Node *leaf : leaves) {
            if (my < leaf->rect.y || my >= leaf->rect.y + TAB_H) continue;
            int t = tab_at(ui, leaf, mx, &hit_x, &hit_w);
            if (t >= 0) { hit_leaf = leaf; hit_tab = t; break; }
        }
    }

    if (pressed && hit_float) hit_float->z = ++d->next_z;
    if (pressed && hit_leaf && hit_tab >= 0) {
        hit_leaf->selected = hit_tab;
        d->drag_armed = true;
        d->drag_title = hit_leaf->tabs[(size_t)hit_tab];
        d->press_x = mx; d->press_y = my;
        d->grab_dx = mx - hit_x;
        d->grab_dy = my - hit_leaf->rect.y;
        dai_ui_claim_mouse(ui);
    } else if (pressed && hit_float) {
        // Pressed the title area of a floating window: move the whole thing.
        d->drag_float = hit_float;
        d->grab_dx = mx - hit_float->rect.x;
        d->grab_dy = my - hit_float->rect.y;
        dai_ui_claim_mouse(ui);
    }

    if (d->drag_float && down) {
        d->drag_float->rect.x = mx - d->grab_dx;
        d->drag_float->rect.y = my - d->grab_dy;
        layout(d->drag_float->root, d->drag_float->rect);
        dai_ui_claim_mouse(ui);
    } else if (!down) {
        d->drag_float = nullptr;
    }

    // ---- dragging a tab --------------------------------------------------
    // Ten pixels, like every other editor: a click that moves one pixel is a
    // click, not a drag.
    if (d->drag_armed && down && !d->dragging) {
        float dx = mx - d->press_x, dy = my - d->press_y;
        if (dx * dx + dy * dy > 100.0f) d->dragging = true;
    }
    if (d->dragging && down) {
        // The target is recomputed while the button is held and must SURVIVE
        // into the release frame - clearing it here every frame meant the
        // drop handler always saw "nowhere" and the tab never docked.
        d->drag_kind = -1;
        d->drop_node = nullptr;
        dai_ui_claim_mouse(ui);
        // Find the drop target: tab bars first, then the edges of a body.
        Node *target = nullptr;
        for (auto it = order.rbegin(); it != order.rend() && !target; ++it) {
            if (!hit(it->second->rect, mx, my)) continue;
            std::vector<Node *> fl;
            collect_leaves(it->second->root, &fl);
            for (Node *leaf : fl) if (hit(leaf->rect, mx, my)) { target = leaf; break; }
        }
        if (!target)
            for (Node *leaf : leaves) if (hit(leaf->rect, mx, my)) { target = leaf; break; }

        if (target) {
            d->drop_node = target;
            Rect r = target->rect;
            if (my < r.y + TAB_H + 4.0f) {
                d->drag_kind = 0;                        // into this tab bar
                d->drag_preview = Rect{ r.x, r.y, r.w, TAB_H };
            } else {
                float zw = std::min(r.w * DROP_ZONE_FRAC, DROP_ZONE_MAX);
                float zh = std::min(r.h * DROP_ZONE_FRAC, DROP_ZONE_MAX);
                if (mx < r.x + zw)              { d->drag_kind = 1; d->drag_preview = Rect{ r.x, r.y, r.w * 0.4f, r.h }; }
                else if (mx > r.x + r.w - zw)   { d->drag_kind = 2; d->drag_preview = Rect{ r.x + r.w * 0.6f, r.y, r.w * 0.4f, r.h }; }
                else if (my < r.y + zh)         { d->drag_kind = 3; d->drag_preview = Rect{ r.x, r.y, r.w, r.h * 0.4f }; }
                else if (my > r.y + r.h - zh)   { d->drag_kind = 4; d->drag_preview = Rect{ r.x, r.y + r.h * 0.6f, r.w, r.h * 0.4f }; }
                else                            { d->drag_kind = 0; d->drag_preview = Rect{ r.x, r.y, r.w, r.h }; }
            }
        } else {
            d->drag_kind = 5;                            // nowhere: a new window
            d->drag_preview = Rect{ mx - 90.0f, my - 10.0f, 180.0f, 120.0f };
        }
    }

    if (d->dragging && released) {
        std::string title = d->drag_title;
        int kind = d->drag_kind;
        Node *target = d->drop_node;
        // Dropping a tab onto its own leaf, when that leaf has only this one
        // tab, must do nothing: it would remove the tab, delete the leaf and
        // then look for the leaf it was going to drop into.
        bool degenerate = target && target->leaf() && target->tabs.size() == 1 &&
                          target->tabs[0] == title;
        if (!degenerate) {
            if (kind == 5) {
                remove_tab(d, title);
                dai_dock::Floating f;
                f.root = new Node();
                f.root->tabs.push_back(title);
                f.rect = Rect{ mx - 80.0f, my - TAB_H * 0.5f, 320.0f, 240.0f };
                f.z = ++d->next_z;
                d->floats.push_back(f);
            } else if (target) {
                // Remember where the target is by identity, since removing the
                // dragged tab can delete nodes - including the target itself.
                std::string anchor = target->tabs.empty() ? std::string()
                                   : target->tabs[(size_t)std::max(0, target->selected)];
                remove_tab(d, title);
                Node *t2 = anchor.empty() ? first_leaf(d->root) : d->find(anchor, nullptr);
                if (!t2) t2 = first_leaf(d->root);
                if (t2) {
                    if (kind == 0) add_tab_to(t2, title, true);
                    else           split_into(d, t2, title, kind, 0.4f);
                }
            }
        }
        d->dragging = false;
        d->drag_armed = false;
        d->drag_title.clear();
        d->drag_kind = -1;
        d->drop_node = nullptr;
        layout(d->root, d->area);
        for (auto &f : d->floats) layout(f.root, f.rect);
    }
    if (!down) { d->dragging = false; d->drag_armed = false; d->drag_kind = -1; d->drop_node = nullptr; }
}

int dai_dock_panel(dai_dock *d, const char *title, float *x, float *y, float *w, float *h) {
    if (!d || !title) return 0;
    if (!dai_dock_visible(d, title)) return 0;
    int idx = -1;
    Node *leaf = d->find(title, &idx);
    if (!leaf) return 0;
    if (x) *x = leaf->body.x;
    if (y) *y = leaf->body.y;
    if (w) *w = leaf->body.w;
    if (h) *h = leaf->body.h;
    // A panel is a root: only the frontmost one under the pointer reacts, and
    // since docked panels tile, "frontmost" is simply "the one you are over".
    int layer = DAI_LAYER_WINDOW;
    for (size_t i = 0; i < d->floats.size(); ++i)
        if (find_tab(d->floats[i].root, title, nullptr))
            layer = DAI_LAYER_WINDOW + 200 + d->floats[i].z;
    dai_ui_layer_push(d->ui, layer);
    dai_ui_root_begin(d->ui, title, leaf->body.x, leaf->body.y, leaf->body.w, leaf->body.h);
    d->panel_depth++;
    return 1;
}

void dai_dock_panel_end(dai_dock *d) {
    if (!d || d->panel_depth <= 0) return;
    d->panel_depth--;
    dai_ui_root_end(d->ui);
    dai_ui_layer_pop(d->ui);
}

void dai_dock_end(dai_dock *d) {
    if (!d || !d->ui) return;
    if (!d->dragging) return;
    dai_ui *ui = d->ui;
    const dai_ui_style *st = dai_ui_style_of(ui);
    dai_ui_layer_push(ui, DAI_LAYER_DOCK_PREVIEW);
    uint32_t tint = (st->accent & 0x00FFFFFFu) | 0x60000000u;
    dai_ui_rect(ui, d->drag_preview.x, d->drag_preview.y, d->drag_preview.w, d->drag_preview.h, tint);
    dai_ui_rect_outline(ui, d->drag_preview.x, d->drag_preview.y, d->drag_preview.w,
                        d->drag_preview.h, 2.0f, st->accent);
    // The tab itself, under the cursor, so the drag has something to follow.
    float mx = 0, my = 0;
    dai_ui_mouse(ui, &mx, &my, nullptr, nullptr);
    float tw = tab_width(ui, d->drag_title);
    dai_ui_rect(ui, mx - d->grab_dx, my - d->grab_dy, tw, TAB_H, st->titlebar_focused);
    dai_ui_rect_outline(ui, mx - d->grab_dx, my - d->grab_dy, tw, TAB_H, 1.0f, st->accent);
    dai_ui_text(ui, mx - d->grab_dx + 8.0f, my - d->grab_dy + 3.0f, d->drag_title.c_str(), st->text);
    dai_ui_layer_pop(ui);
}

// ---------------------------------------------------------------- text form

namespace {

void write_node(const Node *n, std::string &s) {
    if (!n) { s += "{ leaf }"; return; }
    if (n->leaf()) {
        s += "{ leaf ";
        char buf[16];
        std::snprintf(buf, sizeof(buf), "%d", n->selected);
        s += buf;
        for (const auto &t : n->tabs) { s += " \""; s += t; s += "\""; }
        s += " }";
        return;
    }
    char buf[32];
    std::snprintf(buf, sizeof(buf), "{ %s %.4f ", n->axis == 1 ? "h" : "v", (double)n->ratio);
    s += buf;
    write_node(n->a, s);
    s += " ";
    write_node(n->b, s);
    s += " }";
}

const char *skip_ws(const char *p) { while (*p == ' ' || *p == '\n' || *p == '\t' || *p == '\r') ++p; return p; }

Node *read_node(const char **pp) {
    const char *p = skip_ws(*pp);
    if (*p != '{') return nullptr;
    ++p;
    p = skip_ws(p);
    Node *n = new Node();
    if (std::strncmp(p, "leaf", 4) == 0) {
        p += 4;
        n->selected = (int)std::strtol(p, (char **)&p, 10);
        for (;;) {
            p = skip_ws(p);
            if (*p != '"') break;
            ++p;
            const char *start = p;
            while (*p && *p != '"') ++p;
            n->tabs.push_back(std::string(start, (size_t)(p - start)));
            if (*p == '"') ++p;
        }
    } else {
        n->axis = (*p == 'h') ? 1 : 2;
        ++p;
        n->ratio = (float)std::strtod(p, (char **)&p);
        n->a = read_node(&p);
        n->b = read_node(&p);
        if (n->a) n->a->parent = n;
        if (n->b) n->b->parent = n;
    }
    p = skip_ws(p);
    if (*p == '}') ++p;
    *pp = p;
    return n;
}

} // namespace

size_t dai_dock_to_text(const dai_dock *d, char *buf, size_t buf_size) {
    if (!d) return 0;
    std::string s = "dock 1\n";
    write_node(d->root, s);
    s += "\n";
    for (const auto &f : d->floats) {
        char head[96];
        std::snprintf(head, sizeof(head), "float %.1f %.1f %.1f %.1f ",
                      (double)f.rect.x, (double)f.rect.y, (double)f.rect.w, (double)f.rect.h);
        s += head;
        write_node(f.root, s);
        s += "\n";
    }
    for (const auto &c : d->closed) { s += "closed \""; s += c; s += "\"\n"; }
    if (buf && buf_size) {
        size_t n = s.size() < buf_size - 1 ? s.size() : buf_size - 1;
        std::memcpy(buf, s.c_str(), n);
        buf[n] = 0;
    }
    return s.size();
}

dai_result dai_dock_from_text(dai_dock *d, const char *text) {
    if (!d || !text) return DAI_ERR_INVALID_ARG;
    const char *p = std::strstr(text, "dock ");
    if (!p) return DAI_ERR_INVALID_ARG;
    p = std::strchr(p, '\n');
    if (!p) return DAI_ERR_INVALID_ARG;
    Node *root = read_node(&p);
    if (!root) return DAI_ERR_INVALID_ARG;
    free_tree(d->root);
    for (auto &f : d->floats) free_tree(f.root);
    d->floats.clear();
    d->closed.clear();
    d->root = root;

    for (;;) {
        p = skip_ws(p);
        if (std::strncmp(p, "float", 5) == 0) {
            p += 5;
            dai_dock::Floating f;
            f.rect.x = (float)std::strtod(p, (char **)&p);
            f.rect.y = (float)std::strtod(p, (char **)&p);
            f.rect.w = (float)std::strtod(p, (char **)&p);
            f.rect.h = (float)std::strtod(p, (char **)&p);
            f.root = read_node(&p);
            f.z = ++d->next_z;
            if (f.root) d->floats.push_back(f);
        } else if (std::strncmp(p, "closed", 6) == 0) {
            p += 6;
            p = skip_ws(p);
            if (*p == '"') {
                ++p;
                const char *start = p;
                while (*p && *p != '"') ++p;
                d->closed.push_back(std::string(start, (size_t)(p - start)));
                if (*p == '"') ++p;
            }
        } else break;
    }
    return DAI_OK;
}

void dai_dock_dump(const dai_dock *d, char *out, size_t n) {
    if (!d || !out || !n) return;
    std::string s;
    write_node(d->root, s);
    char buf[64];
    std::snprintf(buf, sizeof(buf), "  floats=%u", (unsigned)d->floats.size());
    s += buf;
    std::snprintf(out, n, "%s", s.c_str());
}

} // extern "C"
