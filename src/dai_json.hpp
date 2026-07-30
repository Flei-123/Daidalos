// A small, allocation-honest JSON DOM. Exists because glTF is JSON and the
// engine takes no third party dependencies; it is deliberately not a general
// purpose library - it parses what glTF files contain, strictly, and reports
// the offset when it does not.
#ifndef DAI_JSON_HPP
#define DAI_JSON_HPP

#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

namespace daijson {

struct Value;

struct Member { std::string key; Value *value; };

struct Value {
    enum Type { NUL, BOOL, NUMBER, STRING, ARRAY, OBJECT } type = NUL;
    bool boolean = false;
    double number = 0.0;
    std::string str;
    std::vector<Value *> items;
    std::vector<Member> members;

    // convenience accessors: missing keys are not errors, they return defaults
    const Value *get(const char *key) const {
        if (type != OBJECT) return nullptr;
        for (const Member &m : members) if (m.key == key) return m.value;
        return nullptr;
    }
    const Value *at(size_t i) const { return (type == ARRAY && i < items.size()) ? items[i] : nullptr; }
    size_t size() const { return type == ARRAY ? items.size() : 0; }

    double num(double def = 0.0) const { return type == NUMBER ? number : def; }
    int    integer(int def = -1) const { return type == NUMBER ? (int)number : def; }
    const char *string(const char *def = "") const { return type == STRING ? str.c_str() : def; }

    double num_at(const char *key, double def) const { const Value *v = get(key); return v ? v->num(def) : def; }
    int    int_at(const char *key, int def) const { const Value *v = get(key); return v ? v->integer(def) : def; }
    const char *str_at(const char *key, const char *def = "") const {
        const Value *v = get(key); return v ? v->string(def) : def;
    }
};

// Owns every node; freeing the document frees the tree.
class Document {
public:
    ~Document() { for (Value *v : pool_) delete v; }
    bool parse(const char *text, size_t len, std::string *error);
    const Value *root() const { return root_; }

private:
    Value *alloc() { Value *v = new Value(); pool_.push_back(v); return v; }
    Value *parse_value();
    bool skip_ws();
    bool fail(const char *msg);

    const char *p_ = nullptr, *end_ = nullptr, *begin_ = nullptr;
    Value *root_ = nullptr;
    std::string *err_ = nullptr;
    std::vector<Value *> pool_;
};

} // namespace daijson

#endif
