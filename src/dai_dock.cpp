// Docked panels: a tree of splits with tabbed leaves. See include/dai_dock.h
// for why it is a tree and not a set of edges.
//
// No Vulkan, no editor knowledge: this file turns a tree plus a mouse into
// rectangles and tab bars, and draws them with dai_ui primitives.

#include "dai_dock.h"

#include "dai_icons.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

namespace {

// Tall enough that a 13 px line with descenders fits with air above and below:
// 21 clipped the tail of every 'p' and 'y' in a tab title, which is what "the
// tabs are cut off at the bottom" was.
const float TAB_H    = 26.0f;   // height of a tab bar
const float SPLIT_W  = 4.0f;    // grab width of a splitter
const float MIN_SIDE = 60.0f;   // a panel narrower than this is not a panel
// The middle square of a leaf means "as a tab"; outside it, the pointer
// belongs to whichever EDGE it is proportionally nearest. Proportionally is
// the point. The old rule was a band of min(42%, 220px) per side checked top,
// bottom, left, right in that order - and on a short wide panel (the Project
// window across the bottom of the editor, 200 px tall) top and bottom claimed
// 84% of the height between them, so "right of Project" existed in a 30 pixel
// stripe you had to find blind. Normalised distance has no such blind spot:
// every edge of every panel is reachable at every aspect ratio.
const float DROP_CORE = 0.24f;

// The dock cross. These buttons ARE the drop target, not a picture of one.
const float CROSS_B = 26.0f;   // button size
const float CROSS_G = 3.0f;    // gap between them
const float EDGE_B  = 30.0f;   // the outer buttons: dock against the LAYOUT
const float EDGE_M  = 10.0f;   // how far in from the border they sit

struct Rect { float x = 0, y = 0, w = 0, h = 0; };

bool hit(const Rect &r, float x, float y) {
    return x >= r.x && x < r.x + r.w && y >= r.y && y < r.y + r.h;
}

struct Slot { Rect r; int kind; };

// Over the leaf under the pointer: centre, left, right, top, bottom.
void cross_slots(const Rect &r, Slot out[5]) {
    float cx = r.x + r.w * 0.5f, cy = r.y + r.h * 0.5f;
    const float B = CROSS_B, G = CROSS_G;
    out[0] = Slot{ Rect{ cx - B * 0.5f,     cy - B * 0.5f,     B, B }, 0 };
    out[1] = Slot{ Rect{ cx - B * 1.5f - G, cy - B * 0.5f,     B, B }, 1 };
    out[2] = Slot{ Rect{ cx + B * 0.5f + G, cy - B * 0.5f,     B, B }, 2 };
    out[3] = Slot{ Rect{ cx - B * 0.5f,     cy - B * 1.5f - G, B, B }, 3 };
    out[4] = Slot{ Rect{ cx - B * 0.5f,     cy + B * 0.5f + G, B, B }, 4 };
}

// Against the edges of the whole dock area: a full height column down one
// side, a full width strip along the top or the bottom. Splitting a LEAF can
// never produce those - a panel dropped left of the Hierarchy is only as tall
// as the Hierarchy - and "put this down the entire left edge" is a thing
// people want often enough that hunting for the one leaf that spans the
// height is not an answer. Kinds 6..9.
void edge_slots(const Rect &a, Slot out[4]) {
    float cx = a.x + a.w * 0.5f, cy = a.y + a.h * 0.5f;
    const float B = EDGE_B, M = EDGE_M;
    out[0] = Slot{ Rect{ a.x + M,           cy - B * 0.5f,   B, B }, 6 };
    out[1] = Slot{ Rect{ a.x + a.w - M - B, cy - B * 0.5f,   B, B }, 7 };
    out[2] = Slot{ Rect{ cx - B * 0.5f,     a.y + M,         B, B }, 8 };
    out[3] = Slot{ Rect{ cx - B * 0.5f,     a.y + a.h - M - B, B, B }, 9 };
}

// What a drop of this kind would cover, so the preview and the result agree.
Rect drop_preview_rect(const Rect &r, const Rect &area, int kind) {
    switch (kind) {
    case 1: return Rect{ r.x, r.y, r.w * 0.4f, r.h };
    case 2: return Rect{ r.x + r.w * 0.6f, r.y, r.w * 0.4f, r.h };
    case 3: return Rect{ r.x, r.y, r.w, r.h * 0.4f };
    case 4: return Rect{ r.x, r.y + r.h * 0.6f, r.w, r.h * 0.4f };
    case 6: return Rect{ area.x, area.y, area.w * 0.25f, area.h };
    case 7: return Rect{ area.x + area.w * 0.75f, area.y, area.w * 0.25f, area.h };
    case 8: return Rect{ area.x, area.y, area.w, area.h * 0.25f };
    case 9: return Rect{ area.x, area.y + area.h * 0.75f, area.w, area.h * 0.25f };
    default: return r;
    }
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

// Every leaf that holds this title, with the tab's index inside it. A title is
// no longer unique - "Inspector" can be open twice - so the host asks for them
// one after another and this is what it counts through.
void collect_tab_leaves(Node *n, const std::string &title,
                        std::vector<std::pair<Node *, int>> *out) {
    if (!n) return;
    if (n->leaf()) {
        for (size_t i = 0; i < n->tabs.size(); ++i)
            if (n->tabs[i] == title) out->push_back({ n, (int)i });
        return;
    }
    collect_tab_leaves(n->a, title, out);
    collect_tab_leaves(n->b, title, out);
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
    Node *drag_home = nullptr;     // the leaf the drag started in (reorder!)
    int   reorder_at = -1;         // insertion index while hovering the home bar
    float press_x = 0, press_y = 0;
    float grab_dx = 0, grab_dy = 0;
    Rect  drag_preview;
    int   drag_kind = -1;          // -1 none, 0 tab, 1 left, 2 right, 3 top, 4 bottom, 5 float
    Node *drop_node = nullptr;
    Floating *drag_float = nullptr;   // moving a whole floating window
    int   panel_depth = 0;
    bool  prev_down = false;

    // Two tabs that must stay in one leaf (Scene and Game): the host renders
    // one world per frame, so the two views of it cannot live apart.
    std::string lock_a, lock_b;

    // The leaf menu: the kebab button at the bar's right end, or a right
    // click anywhere on the bar. Opens on RELEASE for the left button (it is
    // a button), immediately for the right one.
    dai_ui_popup leaf_menu{};
    Node *menu_leaf = nullptr;
    std::string menu_tab;
    int   menu_bar_pressed = 0;      // this frame's press was the menu's
    int   menu_pending = 0;          // left press on the kebab, awaiting release

    // How many instances of a title dai_dock_panel has already handed out THIS
    // frame. Without it the host's
    //     for (int inst = 0; dai_dock_panel(dock, "Inspector", ...); ++inst)
    // never ends: the call kept answering with the same panel forever, the
    // frame's vertex buffer grew until the process died of std::bad_alloc, and
    // the editor never drew a single frame. The counter is what makes the loop
    // terminate - it is not an optimisation.
    std::vector<std::pair<std::string, int>> panel_seq;
    int &seq_of(const std::string &t) {
        for (auto &kv : panel_seq) if (kv.first == t) return kv.second;
        panel_seq.push_back({ t, 0 });
        return panel_seq.back().second;
    }

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
void unclose(dai_dock *d, const std::string &title);

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

// Scene and Game are two views of ONE world, and the host renders that world
// once per frame into a single rectangle - so the two tabs can never live in
// different leaves. Whatever just moved (a drop, a restored layout), the
// partner follows. `keep` is the tab whose location wins: the one that was
// just dragged, or the first of the pair on a restore.
void unclose(dai_dock *d, const std::string &title) {
    for (size_t i = 0; i < d->closed.size(); ++i)
        if (d->closed[i] == title) { d->closed.erase(d->closed.begin() + (long)i); return; }
}

void enforce_pair(dai_dock *d, const std::string &keep) {
    if (!d || d->lock_a.empty() || d->lock_b.empty()) return;
    const std::string &win   = keep == d->lock_b ? d->lock_b : d->lock_a;
    const std::string &other = keep == d->lock_b ? d->lock_a : d->lock_b;
    Node *wk = d->find(win, nullptr);
    Node *ok = d->find(other, nullptr);
    if (!wk || !ok || wk == ok) return;
    remove_tab(d, other);
    wk = d->find(win, nullptr);      // the tree may have shifted under it
    if (wk) add_tab_to(wk, other, false);
}

void layout(Node *n, Rect r);   // defined just below; the menu relayouts after Add Tab

// The panel's own menu, Unity's tab context menu: close this tab, or pull
// another panel into this leaf as a tab. Runs from dai_dock_end so it draws
// above every panel.
void run_leaf_menu(dai_dock *d) {
    if (!d->leaf_menu.open) return;
    dai_ui *ui = d->ui;
    // The leaf the menu belongs to can vanish under it (its last tab closed).
    std::vector<Node *> all;
    collect_leaves(d->root, &all);
    for (auto &f : d->floats) collect_leaves(f.root, &all);
    if (std::find(all.begin(), all.end(), d->menu_leaf) == all.end()) {
        dai_ui_popup_close(&d->leaf_menu);
        d->menu_leaf = nullptr;
        return;
    }
    std::vector<dai_ui_menu_item> items;
    std::vector<std::string> storage;
    std::vector<int> addable;                    // item index - 1 -> regs index
    items.push_back({ DAI_ICON_CLOSE, "Close Tab", nullptr });
    for (size_t i = 0; i < d->regs.size(); ++i) {
        const std::string &t = d->regs[i].title;
        if (find_tab(d->menu_leaf, t.c_str(), nullptr)) continue;  // already here
        storage.push_back("Add Tab: " + t);
        items.push_back({ DAI_ICON_PLUS, storage.back().c_str(), nullptr });
        addable.push_back((int)i);
    }
    int pick = dai_ui_popup_menu(ui, &d->leaf_menu, items.data(), (uint32_t)items.size());
    if (pick == -2) return;
    if (pick == -1) {                      // dismissed: clicked outside it
        d->menu_leaf = nullptr;
        return;
    }
    if (pick == 0) {
        // "Close Tab" - the closed list remembers it, so Add Tab can undo it.
        if (!d->is_closed(d->menu_tab)) d->closed.push_back(d->menu_tab);
        remove_tab(d, d->menu_tab);
    } else if (pick > 0 && pick - 1 < (int)addable.size()) {
        const std::string &t = d->regs[(size_t)addable[(size_t)(pick - 1)]].title;
        auto it = std::find(d->closed.begin(), d->closed.end(), t);
        if (it != d->closed.end()) d->closed.erase(it);
        // Only MOVE it here when it already exists somewhere - dragging a tab
        // out of another leaf is what gives "the panel teleported" its name.
        // A title the tree does not hold is a NEW instance (Inspector #2, a
        // second Console): adding that is not a move, so nothing flickers by
        // vanishing from somewhere first.
        if (d->find(t.c_str(), nullptr)) remove_tab(d, t);
        add_tab_to(d->menu_leaf, t, true);
        layout(d->root, d->area);
        for (auto &f : d->floats) layout(f.root, f.rect);
    }
    d->menu_leaf = nullptr;
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

    // Whatever happens up here, it stays up here: the Scene/Game viewport
    // behind the bar asks "was the mouse over UI?" and a click it cannot see
    // as handled is a click that DESELECTS what you were working on.
    if (over_bar) dai_ui_claim_mouse(ui);

    float tx = r.x;
    for (size_t i = 0; i < leaf->tabs.size(); ++i) {
        const std::string &t = leaf->tabs[i];
        float tw = tab_width(ui, t);
        bool sel = (int)i == leaf->selected;
        bool over = over_bar && mx >= tx && mx < tx + tw;
        uint32_t bg = sel ? st->panel : (over ? st->titlebar_focused : st->titlebar);
        // Tabs round at the TOP only: the bottom edge meets the panel body,
        // and a rounded seam there reads as a gap, not as a tab.
        // The tab body starts 3 px down: a tab that touches the top edge of
        // its bar has no room to look rounded, which is why the corners were
        // invisible no matter what radius they were given.
        float ty = r.y + 3.0f, th = TAB_H - 3.0f;
        dai_ui_rrect_mask(ui, tx, ty, tw - 2.0f, th, 7.0f, bg, 0x3);
        // The accent stripe is INSET, or it paints square corners back over
        // the round ones it is supposed to sit on.
        if (sel && focused)
            dai_ui_rrect_mask(ui, tx + 6.0f, ty, tw - 14.0f, 2.0f, 1.0f, st->accent, 0x3);
        float lh = dai_ui_text_height(ui);
        dai_ui_text(ui, tx + 9.0f, ty + (th - lh) * 0.5f, t.c_str(),
                    sel ? st->text : st->text_dim);
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

int dai_dock_panel_rect(const dai_dock *d, const char *title, int index,
                        float *x, float *y, float *w, float *h) {
    if (!d || !title || index < 0) return 0;
    int seen = 0;
    auto walk = [&](Node *n, auto &&walk) -> int {
        if (!n) return 0;
        if (n->leaf()) {
            for (size_t i = 0; i < n->tabs.size(); ++i) {
                if (n->tabs[i] == title) {
                    if (seen == index) {
                        if (x) *x = n->rect.x;
                        if (y) *y = n->rect.y;
                        if (w) *w = n->rect.w;
                        if (h) *h = n->rect.h;
                        return 1;
                    }
                    ++seen;
                }
            }
            return 0;
        }
        return walk(n->a, walk) || walk(n->b, walk);
    };
    if (walk(d->root, walk)) return 1;
    for (const dai_dock::Floating &f : d->floats)
        if (walk(f.root, walk)) return 1;
    return 0;
}

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

void dai_dock_open(dai_dock *d, const char *title) {
    if (!d || !title || !*title) return;
    // Take it off the closed list FIRST - otherwise the panel is "known" and
    // "closed" at the same time, dai_dock_add returns early, and the window
    // can never come back. That was the Settings panel: openable exactly once
    // per session, dead after the first close.
    for (size_t i = 0; i < d->closed.size(); ++i)
        if (d->closed[i] == title) { d->closed.erase(d->closed.begin() + (long)i); break; }
    if (d->find(title, nullptr)) { dai_dock_focus(d, title); return; }
    // Known but not in the tree: put it back where it was registered.
    for (const auto &r : d->regs)
        if (r.title == title) {
            Node *centre = first_leaf(d->root);
            if (!centre) { centre = d->root = new Node(); }
            if (r.edge == DAI_DOCK_NONE || centre->tabs.empty()) add_tab_to(centre, title, true);
            else {
                int e = r.edge == DAI_DOCK_LEFT ? 1 : r.edge == DAI_DOCK_RIGHT ? 2
                      : r.edge == DAI_DOCK_TOP ? 3 : 4;
                split_into(d, d->root, title, e, r.frac);
            }
            layout(d->root, d->area);
            dai_dock_focus(d, title);
            return;
        }
    dai_dock_add(d, title, DAI_DOCK_RIGHT, 0.24f);
}

void dai_dock_add(dai_dock *d, const char *title, int edge, float fraction) {
    if (!d || !title || !*title) return;
    // Anything being ADDED is by definition not closed. Leaving it on the
    // closed list gave a tab in the tree that dai_dock_panel refuses - the
    // Settings panel that opened as a black rectangle, permanently, because
    // the closed list is saved with the layout.
    for (size_t i = 0; i < d->closed.size(); ++i)
        if (d->closed[i] == title) { d->closed.erase(d->closed.begin() + (long)i); break; }
    for (const auto &r : d->regs)
        if (r.title == title) { dai_dock_open(d, title); return; }   // known: reopen it
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

void dai_dock_lock_pair(dai_dock *d, const char *a, const char *b) {
    if (!d || !a || !b) return;
    d->lock_a = a;
    d->lock_b = b;
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
    dai_dock *m = const_cast<dai_dock *>(d);
    if (m->is_closed(title)) return 0;
    // "Open" means IN THE TREE, not merely "never closed": a panel nobody
    // ever added (Settings on first click) is not closed, and the old answer
    // of 1 here made the opener believe the window already existed - so it
    // never got added and could never appear at all.
    return m->find(title, nullptr) ? 1 : 0;
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

    // A new frame hands out the instances again from the first one.
    for (auto &kv : d->panel_seq) kv.second = 0;

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

    // A restored layout may have split a locked pair apart (a file written
    // before the lock existed): fuse it back before anyone sees it.
    enforce_pair(d, d->lock_a);

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
            if (pressed && !dai_ui_popup_active(ui)) d->split_drag = n;
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

    // ---- the leaf menu: the kebab button, or a right click on the bar ----
    // Unity puts the panel's own menu there (Close Tab, Add Tab). A button
    // that only ever drew itself is why "the three dots do nothing".
    d->menu_bar_pressed = 0;
    {
        int right_pressed = dai_ui_right_pressed(ui);
        if ((pressed || right_pressed) && !d->dragging && !d->leaf_menu.open) {
            Node *bar_leaf = nullptr;
            int   bar_tab = -1;
            bool  consumed = false;      // a floating window eats what is under it
            auto probe = [&](Node *leaf) -> bool {
                Rect lr = leaf->rect;
                if (my < lr.y || my >= lr.y + TAB_H || mx < lr.x || mx >= lr.x + lr.w)
                    return false;
                bool on_dots = mx >= lr.x + lr.w - 18.0f;
                float tx0 = 0, tw0 = 0;
                int t = tab_at(ui, leaf, mx, &tx0, &tw0);
                if (right_pressed || on_dots) { bar_leaf = leaf; bar_tab = t; }
                return true;
            };
            for (auto it = order.rbegin(); it != order.rend(); ++it) {
                if (!hit(it->second->rect, mx, my)) continue;
                consumed = true;
                std::vector<Node *> fl;
                collect_leaves(it->second->root, &fl);
                for (Node *leaf : fl) if (probe(leaf)) break;
                break;
            }
            if (!bar_leaf && !consumed)
                for (Node *leaf : leaves) { if (probe(leaf)) break; }
            if (bar_leaf) {
                int sel = bar_tab >= 0 ? bar_tab : bar_leaf->selected;
                if (sel >= 0 && sel < (int)bar_leaf->tabs.size()) {
                    if (right_pressed) {
                        // A right click selects the tab it landed on, like Unity.
                        if (bar_tab >= 0) bar_leaf->selected = bar_tab;
                        d->menu_tab = bar_leaf->tabs[(size_t)sel];
                        d->menu_leaf = bar_leaf;
                        dai_ui_popup_open(&d->leaf_menu, mx, my);
                    } else {
                        // The kebab is a button: it opens on release, or the
                        // press that opened it would count as a click on the
                        // first item.
                        d->menu_pending = 1;
                        d->menu_leaf = bar_leaf;
                        d->menu_tab = bar_leaf->tabs[(size_t)sel];
                    }
                    d->menu_bar_pressed = 1;
                    d->split_drag = nullptr;   // the kebab corner can sit on a splitter
                    dai_ui_claim_mouse(ui);
                }
            }
        }
        // The kebab click completes on release, still over the button.
        if (d->menu_pending && !down) {
            Node *leaf = d->menu_leaf;
            std::vector<Node *> all;
            collect_leaves(d->root, &all);
            for (auto &f : d->floats) collect_leaves(f.root, &all);
            bool alive = std::find(all.begin(), all.end(), leaf) != all.end();
            Rect lr = alive ? leaf->rect : Rect{ 0, 0, 0, 0 };
            if (alive && my >= lr.y && my < lr.y + TAB_H &&
                mx >= lr.x + lr.w - 18.0f && mx < lr.x + lr.w)
                dai_ui_popup_open(&d->leaf_menu, mx, my);
            else
                d->menu_leaf = nullptr;
            d->menu_pending = 0;
        } else if (d->menu_pending) {
            dai_ui_claim_mouse(ui);
        }
    }

    if (pressed && hit_float && !d->menu_bar_pressed) hit_float->z = ++d->next_z;
    if (pressed && hit_leaf && hit_tab >= 0 && !d->menu_bar_pressed &&
        !dai_ui_popup_active(ui)) {
        hit_leaf->selected = hit_tab;
        d->drag_armed = true;
        d->drag_home = hit_leaf;
        d->drag_title = hit_leaf->tabs[(size_t)hit_tab];
        d->press_x = mx; d->press_y = my;
        d->grab_dx = mx - hit_x;
        d->grab_dy = my - hit_leaf->rect.y;
        dai_ui_claim_mouse(ui);
    } else if (pressed && hit_float && !d->menu_bar_pressed &&
               !dai_ui_popup_active(ui)) {
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
        d->reorder_at = -1;
        // Reorder FIRST: hovering the tab bar the drag started in, on the
        // bar's strip, is the one gesture that means "put it between these
        // two". Everything else still means dock/split/float.
        if (d->drag_home) {
            Rect hr = d->drag_home->rect;
            bool on_home_bar = my >= hr.y && my < hr.y + TAB_H &&
                               mx >= hr.x && mx < hr.x + hr.w;
            if (on_home_bar && (int)d->drag_home->tabs.size() > 1) {
                float cx = hr.x;
                int at = (int)d->drag_home->tabs.size();
                for (size_t i = 0; i < d->drag_home->tabs.size(); ++i) {
                    float tw = tab_width(ui, d->drag_home->tabs[i]);
                    if (mx < cx + tw * 0.5f) { at = (int)i; break; }
                    cx += tw;
                }
                d->reorder_at = at;
                d->drop_node = d->drag_home;
                // the insertion line, like every file manager's
                float ix = hr.x;
                for (int i = 0; i < at && i < (int)d->drag_home->tabs.size(); ++i)
                    ix += tab_width(ui, d->drag_home->tabs[(size_t)i]);
                dai_ui_layer_push(ui, DAI_LAYER_DOCK_PREVIEW);
                dai_ui_rect(ui, ix - 1.0f, hr.y + 3.0f, 2.0f, TAB_H - 4.0f,
                            dai_ui_style_of(ui)->accent);
                dai_ui_layer_pop(ui);
            }
        }
        // Claimed either way: a reorder that does not claim the mouse lets the
        // same press fall through to the 3D view and clear the selection.
        dai_ui_claim_mouse(ui);
        if (d->reorder_at >= 0) goto drag_target_done;
        // Find the leaf under the pointer. A floating window is checked
        // first and, once the pointer is inside one, the layout underneath is
        // not a candidate at all - dropping "into" a window you cannot see is
        // never what the gesture meant.
        Node *target = nullptr;
        bool over_float = false;
        for (auto it = order.rbegin(); it != order.rend() && !target; ++it) {
            if (!hit(it->second->rect, mx, my)) continue;
            over_float = true;
            std::vector<Node *> fl;
            collect_leaves(it->second->root, &fl);
            for (Node *leaf : fl) if (hit(leaf->rect, mx, my)) { target = leaf; break; }
        }
        if (!target && !over_float)
            for (Node *leaf : leaves) if (hit(leaf->rect, mx, my)) { target = leaf; break; }

        d->drop_node = target;
        int kind = -1;
        // 1. The leaf's own cross, first: it is drawn on top, it is the most
        //    specific thing under the pointer, and it is what you were aiming
        //    at if you were aiming at anything.
        if (target) {
            Slot cs[5];
            cross_slots(target->rect, cs);
            for (const Slot &sl : cs) if (hit(sl.r, mx, my)) { kind = sl.kind; break; }
        }
        // 2. The outer buttons - the whole layout's edges.
        if (kind < 0 && !over_float && hit(d->area, mx, my)) {
            Slot es[4];
            edge_slots(d->area, es);
            for (const Slot &sl : es) if (hit(sl.r, mx, my)) { kind = sl.kind; break; }
        }
        if (target) {
            Rect r = target->rect;
            // 3. The tab bar strip still means "another tab of this one",
            //    which is the gesture everybody tries before they find a
            //    button, and it must keep working while the cross is up.
            if (kind < 0 && my < r.y + TAB_H + 4.0f) {
                kind = 0;
                d->drag_kind = 0;
                d->drag_preview = Rect{ r.x, r.y, r.w, TAB_H };
            } else {
                // 4. Otherwise: the middle square is a tab, and everything
                //    around it belongs to the nearest edge measured as a
                //    FRACTION of the panel, so a wide short panel has a
                //    usable right edge and a tall thin one a usable top.
                if (kind < 0) {
                    float u = r.w > 1.0f ? (mx - r.x) / r.w : 0.5f;
                    float v = r.h > 1.0f ? (my - r.y) / r.h : 0.5f;
                    if (std::fabs(u - 0.5f) < DROP_CORE && std::fabs(v - 0.5f) < DROP_CORE) {
                        kind = 0;
                    } else {
                        float dl = u, dr = 1.0f - u, dt = v, db = 1.0f - v;
                        float best = dl; kind = 1;
                        if (dr < best) { best = dr; kind = 2; }
                        if (dt < best) { best = dt; kind = 3; }
                        if (db < best) { best = db; kind = 4; }
                    }
                }
                d->drag_kind = kind;
                d->drag_preview = drop_preview_rect(r, d->area, kind);
            }
        } else if (kind >= 0) {
            d->drag_kind = kind;
            d->drag_preview = drop_preview_rect(d->area, d->area, kind);
        } else {
            d->drag_kind = 5;                            // nowhere: a new window
            d->drag_preview = Rect{ mx - 90.0f, my - 10.0f, 180.0f, 120.0f };
        }
    }
drag_target_done: (void)0;

    if (d->dragging && released) {
        std::string title = d->drag_title;
        int kind = d->drag_kind;
        Node *target = d->drop_node;
        // A drop that stayed on the home bar is a REORDER, not a dock move:
        // the tab comes out and goes back in at the insertion index. This
        // must run before the degenerate check, or a single-tab leaf would
        // swallow it.
        if (d->reorder_at >= 0 && d->drag_home) {
            Node *home = d->drag_home;
            int from = -1;
            for (size_t i = 0; i < home->tabs.size(); ++i)
                if (home->tabs[i] == title) { from = (int)i; break; }
            if (from >= 0) {
                int at = d->reorder_at;
                if (at > from) at--;              // removing shifts the index
                if (at < 0) at = 0;
                if (at > (int)home->tabs.size() - 1) at = (int)home->tabs.size() - 1;
                if (at != from) {
                    home->tabs.erase(home->tabs.begin() + from);
                    home->tabs.insert(home->tabs.begin() + at, title);
                }
                home->selected = at;
            }
            d->dragging = false;
            d->drag_armed = false;
            d->reorder_at = -1;
            d->drag_home = nullptr;
            return;
        }
        // Dropping a tab onto its own leaf, when that leaf has only this one
        // tab, must do nothing: it would remove the tab, delete the leaf and
        // then look for the leaf it was going to drop into.
        // Dropping a tab into the leaf it already owns ALONE is a no-op in
        // every direction: splitting a leaf against itself would remove the
        // tab, delete the now empty leaf, and then look for it.
        // ...but only for the four LEAF splits and the tab drop. Dropping
        // that same lone tab onto an outer button is not a no-op: it asks for
        // a full height column, which is a different tree.
        bool degenerate = kind >= 0 && kind <= 4 && target && target->leaf() &&
                          target->tabs.size() == 1 && target->tabs[0] == title;
        if (!degenerate) {
            if (kind >= 6) {
                // Against the whole layout. The tab comes out first, which
                // can promote a sibling and REPLACE d->root - so the root is
                // read again afterwards, never cached across remove_tab.
                bool only = d->root && d->root->leaf() && d->root->tabs.size() == 1 &&
                            d->root->tabs[0] == title;
                if (!only) {
                    remove_tab(d, title);
                    if (!d->root) d->root = new Node();
                    if (d->root->leaf() && d->root->tabs.empty())
                        add_tab_to(d->root, title, true);
                    else
                        split_into(d, d->root, title, kind - 5, 0.25f);
                }
            } else if (kind == 5) {
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
                // The anchor must be a tab that SURVIVES the removal below.
                // It used to be the target's SELECTED tab - but pressing a tab
                // selects it, so dragging Game out of a Scene+Game leaf made
                // the anchor "Game" itself. After remove_tab it was gone, the
                // lookup failed, and the fallback first_leaf() docked it onto
                // whatever leaf happens to come first in the tree - the panel
                // appeared on the opposite side of the editor.
                std::string anchor;
                for (const std::string &t : target->tabs)
                    if (t != title) { anchor = t; break; }
                remove_tab(d, title);
                Node *t2 = anchor.empty() ? first_leaf(d->root) : d->find(anchor, nullptr);
                if (!t2) t2 = first_leaf(d->root);
                if (t2) {
                    if (kind == 0) add_tab_to(t2, title, true);
                    else           split_into(d, t2, title, kind, 0.4f);
                }
            }
        }
        // The dragged tab landed wherever it landed; its locked partner
        // follows it there - the two can never end up in different leaves.
        enforce_pair(d, title);
        d->dragging = false;
        d->drag_armed = false;
        d->drag_title.clear();
        d->drag_kind = -1;
        d->drop_node = nullptr;
        layout(d->root, d->area);
        for (auto &f : d->floats) layout(f.root, f.rect);
    }
    if (!down) { d->dragging = false; d->drag_armed = false; d->drag_kind = -1;
                 d->drop_node = nullptr; d->drag_home = nullptr; d->reorder_at = -1; }
}

int dai_dock_panel(dai_dock *d, const char *title, float *x, float *y, float *w, float *h) {
    if (!d || !title) return 0;
    if (d->is_closed(title)) return 0;

    // Which instance of this title is being asked for. The host loops until
    // this answers 0, so "no more instances" MUST be reachable - see seq_of.
    int &seq = d->seq_of(title);
    std::vector<std::pair<Node *, int>> hits;
    collect_tab_leaves(d->root, title, &hits);
    for (auto &f : d->floats) collect_tab_leaves(f.root, title, &hits);

    Node *leaf = nullptr;
    int seen = 0;
    for (auto &hp : hits) {
        // A tab that is not the selected one of its leaf is behind another
        // panel: it has no body to draw into.
        if (hp.second != hp.first->selected) continue;
        if (seen++ == seq) { leaf = hp.first; break; }
    }
    if (!leaf) return 0;
    ++seq;

    if (x) *x = leaf->body.x;
    if (y) *y = leaf->body.y;
    if (w) *w = leaf->body.w;
    if (h) *h = leaf->body.h;
    // A panel is a root: only the frontmost one under the pointer reacts, and
    // since docked panels tile, "frontmost" is simply "the one you are over".
    // Which floating window this instance belongs to is decided by walking UP
    // from the leaf - asking "does any float contain this title" would give
    // every instance the topmost float's layer.
    int layer = DAI_LAYER_WINDOW;
    Node *top = leaf;
    while (top->parent) top = top->parent;
    for (size_t i = 0; i < d->floats.size(); ++i)
        if (d->floats[i].root == top) layer = DAI_LAYER_WINDOW + 200 + d->floats[i].z;
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
    // The frame's panel pass is over: the instance counters start again. A
    // one-off geometry query between frames ("where is the Hierarchy?") then
    // answers with the FIRST instance instead of "no more of those", which is
    // what a caller outside the loop means every time.
    for (auto &kv : d->panel_seq) kv.second = 0;
    if (!d->dragging) { run_leaf_menu(d); return; }
    dai_ui *ui = d->ui;
    const dai_ui_style *st = dai_ui_style_of(ui);
    dai_ui_layer_push(ui, DAI_LAYER_DOCK_PREVIEW);
    uint32_t tint = (st->accent & 0x00FFFFFFu) | 0x60000000u;
    dai_ui_rect(ui, d->drag_preview.x, d->drag_preview.y, d->drag_preview.w, d->drag_preview.h, tint);
    dai_ui_rect_outline(ui, d->drag_preview.x, d->drag_preview.y, d->drag_preview.w,
                        d->drag_preview.h, 2.0f, st->accent);
    // The tab being carried, and what will happen when it is let go. Four of
    // the five previews are rectangles of the same colour; only a word tells
    // "as a tab" from "underneath".
    {
        const char *what = d->drag_kind == 0 ? "as a tab"
                         : d->drag_kind == 1 ? "left of"
                         : d->drag_kind == 2 ? "right of"
                         : d->drag_kind == 3 ? "above"
                         : d->drag_kind == 4 ? "below"
                         : d->drag_kind == 6 ? "down the left edge"
                         : d->drag_kind == 7 ? "down the right edge"
                         : d->drag_kind == 8 ? "along the top"
                         : d->drag_kind == 9 ? "along the bottom"
                         : "as a window";
        char note[128];
        std::snprintf(note, sizeof(note), "%s  -  %s", d->drag_title.c_str(), what);
        float mx2 = 0, my2 = 0;
        dai_ui_mouse(ui, &mx2, &my2, nullptr, nullptr);
        float tw = dai_ui_text_width(ui, note) + 14.0f;
        float th = dai_ui_text_height(ui) + 8.0f;
        dai_ui_rrect(ui, mx2 + 14.0f, my2 + 10.0f, tw, th, 4.0f, 0xF01E1E1Eu);
        dai_ui_rect_outline(ui, mx2 + 14.0f, my2 + 10.0f, tw, th, 1.0f, st->accent);
        dai_ui_text(ui, mx2 + 21.0f, my2 + 14.0f, note, st->text);
    }

    // The dock cross, over the leaf the pointer is on. Without it the four
    // split zones are invisible geography you have to find by waving the
    // mouse - which is why "I still cannot put the console under the
    // inspector" is a UI bug and not a missing feature: the feature was
    // there, the target was not.
    {
        // One button drawer for both crosses: the four outer ones against the
        // area's edges, then the five over the leaf on top of them. Same
        // geometry the drop reads, so what lights up is what happens.
        auto draw_slot = [&](const Slot &sl, bool outer) {
            bool on = d->drag_kind == sl.kind;
            const Rect &b = sl.r;
            uint32_t bg = on ? st->accent : ((st->chrome & 0x00FFFFFFu) | 0xD8000000u);
            dai_ui_rrect(ui, b.x, b.y, b.w, b.h, 4.0f, bg);
            dai_ui_rect_outline(ui, b.x, b.y, b.w, b.h, 1.0f,
                                on ? 0xFFFFFFFFu : st->panel_border);
            uint32_t fg = on ? 0xFF101010u : st->text_dim;
            float ix = b.x + 5.0f, iy = b.y + 5.0f, iw = b.w - 10.0f, ih = b.h - 10.0f;
            // The outer buttons draw the strip INSIDE a frame of the whole
            // window, so "a column down the side of everything" is visibly a
            // different promise from "the left half of this panel".
            if (outer) dai_ui_rect_outline(ui, ix, iy, iw, ih, 1.0f, fg);
            switch (sl.kind) {
            case 1: dai_ui_rect(ui, ix, iy, iw * 0.5f, ih, fg); break;
            case 2: dai_ui_rect(ui, ix + iw * 0.5f, iy, iw * 0.5f, ih, fg); break;
            case 3: dai_ui_rect(ui, ix, iy, iw, ih * 0.5f, fg); break;
            case 4: dai_ui_rect(ui, ix, iy + ih * 0.5f, iw, ih * 0.5f, fg); break;
            case 6: dai_ui_rect(ui, ix, iy, iw * 0.34f, ih, fg); break;
            case 7: dai_ui_rect(ui, ix + iw * 0.66f, iy, iw * 0.34f, ih, fg); break;
            case 8: dai_ui_rect(ui, ix, iy, iw, ih * 0.34f, fg); break;
            case 9: dai_ui_rect(ui, ix, iy + ih * 0.66f, iw, ih * 0.34f, fg); break;
            default: dai_ui_rect(ui, ix, iy, iw, 3.0f, fg);
                     dai_ui_rect_outline(ui, ix, iy, iw, ih, 1.0f, fg); break;
            }
        };
        Slot es[4];
        edge_slots(d->area, es);
        for (const Slot &sl : es) draw_slot(sl, true);
        if (d->drop_node) {
            Slot cs[5];
            cross_slots(d->drop_node->rect, cs);
            for (const Slot &sl : cs) draw_slot(sl, false);
        }
    }
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

    // Every title the file mentions is now a panel this dock KNOWS - the
    // register is what the Window menu is built from, and loading a layout
    // used to leave it empty.
    {
        std::vector<Node *> all;
        collect_leaves(d->root, &all);
        for (auto &f : d->floats) collect_leaves(f.root, &all);
        for (Node *leaf : all)
            for (const std::string &t : leaf->tabs) {
                bool known = false;
                for (const auto &r : d->regs) if (r.title == t) { known = true; break; }
                if (!known) d->regs.push_back(dai_dock::Reg{ t, DAI_DOCK_NONE, 0.22f, "" });
            }
    }

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
