// Input actions. See include/dai_input.h.

#include "dai_input.h"

#include <cstdio>
#include <cstring>
#include <string>
#include <unordered_map>
#include <vector>

namespace {

struct Action {
    std::string name;
    std::vector<uint32_t> sources;
    bool held = false, prev = false;
};

struct Axis {
    std::string name;
    std::vector<std::pair<uint32_t, uint32_t>> digital;   // negative, positive
    std::vector<std::pair<uint32_t, float>> analog;       // source, scale
    float value = 0.0f;
};

} // namespace

struct dai_actions {
    std::vector<Action> actions;
    std::vector<Axis> axes;
    std::unordered_map<uint32_t, char> down;      // source -> pressed
    std::unordered_map<uint32_t, float> analog;
};

extern "C" {

dai_actions *dai_actions_create(void) { return new dai_actions(); }
void dai_actions_destroy(dai_actions *a) { delete a; }

dai_action dai_action_define(dai_actions *a, const char *name) {
    if (!a || !name) return DAI_INVALID_ACTION;
    for (uint32_t i = 0; i < a->actions.size(); ++i)
        if (a->actions[i].name == name) return i;
    Action act; act.name = name;
    a->actions.push_back(act);
    return (dai_action)(a->actions.size() - 1);
}

dai_axis dai_axis_define(dai_actions *a, const char *name) {
    if (!a || !name) return DAI_INVALID_ACTION;
    for (uint32_t i = 0; i < a->axes.size(); ++i)
        if (a->axes[i].name == name) return i;
    Axis ax; ax.name = name;
    a->axes.push_back(ax);
    return (dai_axis)(a->axes.size() - 1);
}

dai_action dai_action_find(const dai_actions *a, const char *name) {
    if (!a || !name) return DAI_INVALID_ACTION;
    for (uint32_t i = 0; i < a->actions.size(); ++i)
        if (a->actions[i].name == name) return i;
    return DAI_INVALID_ACTION;
}

dai_axis dai_axis_find(const dai_actions *a, const char *name) {
    if (!a || !name) return DAI_INVALID_ACTION;
    for (uint32_t i = 0; i < a->axes.size(); ++i)
        if (a->axes[i].name == name) return i;
    return DAI_INVALID_ACTION;
}

uint32_t dai_action_count(const dai_actions *a) { return a ? (uint32_t)a->actions.size() : 0; }
uint32_t dai_axis_count(const dai_actions *a) { return a ? (uint32_t)a->axes.size() : 0; }

const char *dai_action_name(const dai_actions *a, dai_action act) {
    return (a && act < a->actions.size()) ? a->actions[act].name.c_str() : "";
}

dai_result dai_action_bind(dai_actions *a, dai_action act, uint32_t source) {
    if (!a || act >= a->actions.size()) return DAI_ERR_NOT_FOUND;
    for (uint32_t s : a->actions[act].sources) if (s == source) return DAI_OK;
    a->actions[act].sources.push_back(source);
    return DAI_OK;
}

dai_result dai_action_clear(dai_actions *a, dai_action act) {
    if (!a || act >= a->actions.size()) return DAI_ERR_NOT_FOUND;
    a->actions[act].sources.clear();
    return DAI_OK;
}

dai_result dai_axis_bind(dai_actions *a, dai_axis ax, uint32_t neg, uint32_t pos) {
    if (!a || ax >= a->axes.size()) return DAI_ERR_NOT_FOUND;
    a->axes[ax].digital.push_back({ neg, pos });
    return DAI_OK;
}

dai_result dai_axis_bind_analog(dai_actions *a, dai_axis ax, uint32_t source, float scale) {
    if (!a || ax >= a->axes.size()) return DAI_ERR_NOT_FOUND;
    a->axes[ax].analog.push_back({ source, scale == 0.0f ? 1.0f : scale });
    return DAI_OK;
}

void dai_actions_source(dai_actions *a, uint32_t source, int down) {
    if (!a) return;
    a->down[source] = down ? 1 : 0;
}

void dai_actions_analog(dai_actions *a, uint32_t source, float value) {
    if (!a) return;
    a->analog[source] = value;
}

void dai_actions_update(dai_actions *a) {
    if (!a) return;
    for (Action &act : a->actions) {
        act.prev = act.held;
        bool held = false;
        for (uint32_t s : act.sources) {
            auto it = a->down.find(s);
            if (it != a->down.end() && it->second) { held = true; break; }
        }
        act.held = held;
    }
    for (Axis &ax : a->axes) {
        float v = 0.0f;
        for (auto &d : ax.digital) {
            auto n = a->down.find(d.first), p = a->down.find(d.second);
            if (n != a->down.end() && n->second) v -= 1.0f;
            if (p != a->down.end() && p->second) v += 1.0f;
        }
        for (auto &an : ax.analog) {
            auto it = a->analog.find(an.first);
            if (it != a->analog.end()) v += it->second * an.second;
        }
        // digital sources cancel out; analogue ones can push past 1 if the host
        // feeds a raw value, so clamp once at the end rather than per source
        ax.value = v < -1.0f ? -1.0f : (v > 1.0f ? 1.0f : v);
    }
}

int dai_action_held(const dai_actions *a, dai_action act) {
    return (a && act < a->actions.size() && a->actions[act].held) ? 1 : 0;
}
int dai_action_pressed(const dai_actions *a, dai_action act) {
    if (!a || act >= a->actions.size()) return 0;
    return (a->actions[act].held && !a->actions[act].prev) ? 1 : 0;
}
int dai_action_released(const dai_actions *a, dai_action act) {
    if (!a || act >= a->actions.size()) return 0;
    return (!a->actions[act].held && a->actions[act].prev) ? 1 : 0;
}
float dai_axis_value(const dai_actions *a, dai_axis ax) {
    return (a && ax < a->axes.size()) ? a->axes[ax].value : 0.0f;
}

dai_result dai_actions_save(const dai_actions *a, const char *path) {
    if (!a || !path) return DAI_ERR_INVALID_ARG;
    FILE *f = std::fopen(path, "wb");
    if (!f) return DAI_ERR_FILE;
    std::fprintf(f, "# daidalos input bindings\n");
    for (const Action &act : a->actions) {
        std::fprintf(f, "action %s =", act.name.c_str());
        for (size_t i = 0; i < act.sources.size(); ++i)
            std::fprintf(f, "%s %u", i ? "," : "", act.sources[i]);
        std::fprintf(f, "\n");
    }
    for (const Axis &ax : a->axes) {
        std::fprintf(f, "axis %s =", ax.name.c_str());
        for (size_t i = 0; i < ax.digital.size(); ++i)
            std::fprintf(f, "%s %u/%u", i ? "," : "", ax.digital[i].first, ax.digital[i].second);
        for (size_t i = 0; i < ax.analog.size(); ++i)
            std::fprintf(f, "%s %u*%.3f", (i || !ax.digital.empty()) ? "," : "",
                         ax.analog[i].first, ax.analog[i].second);
        std::fprintf(f, "\n");
    }
    std::fclose(f);
    return DAI_OK;
}

dai_result dai_actions_load(dai_actions *a, const char *path) {
    if (!a || !path) return DAI_ERR_INVALID_ARG;
    FILE *f = std::fopen(path, "rb");
    if (!f) return DAI_ERR_FILE;
    char line[1024];
    while (std::fgets(line, sizeof(line), f)) {
        if (line[0] == '#' || line[0] == '\n') continue;
        char kind[16] = {0}, name[128] = {0};
        const char *eq = std::strchr(line, '=');
        if (!eq) continue;
        if (std::sscanf(line, "%15s %127[^=]", kind, name) < 2) continue;
        // trim the trailing spaces sscanf leaves before the '='
        for (int i = (int)std::strlen(name) - 1; i >= 0 && (name[i] == ' ' || name[i] == '\t'); --i) name[i] = 0;

        if (!std::strcmp(kind, "action")) {
            dai_action act = dai_action_define(a, name);
            dai_action_clear(a, act);              // the file replaces defaults
            const char *p = eq + 1;
            while (*p) {
                uint32_t src = 0;
                int consumed = 0;
                if (std::sscanf(p, " %u%n", &src, &consumed) == 1) { dai_action_bind(a, act, src); p += consumed; }
                else break;
                while (*p == ',' || *p == ' ') ++p;
            }
        } else if (!std::strcmp(kind, "axis")) {
            dai_axis ax = dai_axis_define(a, name);
            a->axes[ax].digital.clear();
            a->axes[ax].analog.clear();
            const char *p = eq + 1;
            while (*p) {
                uint32_t s1 = 0, s2 = 0; float scale = 0;
                int consumed = 0;
                if (std::sscanf(p, " %u/%u%n", &s1, &s2, &consumed) == 2) { dai_axis_bind(a, ax, s1, s2); p += consumed; }
                else if (std::sscanf(p, " %u*%f%n", &s1, &scale, &consumed) == 2) { dai_axis_bind_analog(a, ax, s1, scale); p += consumed; }
                else break;
                while (*p == ',' || *p == ' ') ++p;
            }
        }
    }
    std::fclose(f);
    return DAI_OK;
}

} // extern "C"
