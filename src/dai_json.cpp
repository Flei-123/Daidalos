#include "dai_json.hpp"

#include <cstdio>

namespace daijson {

bool Document::fail(const char *msg) {
    if (err_) {
        char buf[160];
        std::snprintf(buf, sizeof(buf), "JSON: %s at byte %zu", msg, (size_t)(p_ - begin_));
        *err_ = buf;
    }
    return false;
}

bool Document::skip_ws() {
    while (p_ < end_ && (*p_ == ' ' || *p_ == '\t' || *p_ == '\n' || *p_ == '\r')) ++p_;
    return p_ < end_;
}

Value *Document::parse_value() {
    if (!skip_ws()) { fail("unexpected end"); return nullptr; }
    char c = *p_;

    if (c == '{') {
        ++p_;
        Value *v = alloc(); v->type = Value::OBJECT;
        if (!skip_ws()) { fail("unterminated object"); return nullptr; }
        if (*p_ == '}') { ++p_; return v; }
        for (;;) {
            if (!skip_ws() || *p_ != '"') { fail("expected key"); return nullptr; }
            Value *key = parse_value();
            if (!key) return nullptr;
            if (!skip_ws() || *p_ != ':') { fail("expected ':'"); return nullptr; }
            ++p_;
            Value *val = parse_value();
            if (!val) return nullptr;
            v->members.push_back({ key->str, val });
            if (!skip_ws()) { fail("unterminated object"); return nullptr; }
            if (*p_ == ',') { ++p_; continue; }
            if (*p_ == '}') { ++p_; return v; }
            fail("expected ',' or '}'"); return nullptr;
        }
    }
    if (c == '[') {
        ++p_;
        Value *v = alloc(); v->type = Value::ARRAY;
        if (!skip_ws()) { fail("unterminated array"); return nullptr; }
        if (*p_ == ']') { ++p_; return v; }
        for (;;) {
            Value *item = parse_value();
            if (!item) return nullptr;
            v->items.push_back(item);
            if (!skip_ws()) { fail("unterminated array"); return nullptr; }
            if (*p_ == ',') { ++p_; continue; }
            if (*p_ == ']') { ++p_; return v; }
            fail("expected ',' or ']'"); return nullptr;
        }
    }
    if (c == '"') {
        ++p_;
        Value *v = alloc(); v->type = Value::STRING;
        std::string &s = v->str;
        while (p_ < end_ && *p_ != '"') {
            if (*p_ == '\\') {
                ++p_;
                if (p_ >= end_) { fail("bad escape"); return nullptr; }
                switch (*p_) {
                case 'n': s += '\n'; break;
                case 't': s += '\t'; break;
                case 'r': s += '\r'; break;
                case 'b': s += '\b'; break;
                case 'f': s += '\f'; break;
                case '/': s += '/';  break;
                case '"': s += '"';  break;
                case '\\': s += '\\'; break;
                case 'u': {
                    if (p_ + 4 >= end_) { fail("bad \\u escape"); return nullptr; }
                    unsigned cp = 0;
                    for (int i = 1; i <= 4; ++i) {
                        char h = p_[i];
                        cp <<= 4;
                        if (h >= '0' && h <= '9') cp |= (unsigned)(h - '0');
                        else if (h >= 'a' && h <= 'f') cp |= (unsigned)(h - 'a' + 10);
                        else if (h >= 'A' && h <= 'F') cp |= (unsigned)(h - 'A' + 10);
                        else { fail("bad \\u escape"); return nullptr; }
                    }
                    p_ += 4;
                    // UTF-8 encode (surrogate pairs are left as-is; asset names
                    // in the BMP are what actually shows up in glTF)
                    if (cp < 0x80) s += (char)cp;
                    else if (cp < 0x800) { s += (char)(0xC0 | (cp >> 6)); s += (char)(0x80 | (cp & 0x3F)); }
                    else { s += (char)(0xE0 | (cp >> 12)); s += (char)(0x80 | ((cp >> 6) & 0x3F)); s += (char)(0x80 | (cp & 0x3F)); }
                    break;
                }
                default: fail("unknown escape"); return nullptr;
                }
                ++p_;
            } else {
                s += *p_++;
            }
        }
        if (p_ >= end_) { fail("unterminated string"); return nullptr; }
        ++p_;
        return v;
    }
    if (c == 't' && end_ - p_ >= 4 && !std::memcmp(p_, "true", 4)) {
        p_ += 4; Value *v = alloc(); v->type = Value::BOOL; v->boolean = true; return v;
    }
    if (c == 'f' && end_ - p_ >= 5 && !std::memcmp(p_, "false", 5)) {
        p_ += 5; Value *v = alloc(); v->type = Value::BOOL; v->boolean = false; return v;
    }
    if (c == 'n' && end_ - p_ >= 4 && !std::memcmp(p_, "null", 4)) {
        p_ += 4; return alloc();
    }
    if (c == '-' || (c >= '0' && c <= '9')) {
        char *endp = nullptr;
        double d = std::strtod(p_, &endp);
        if (endp == p_) { fail("bad number"); return nullptr; }
        p_ = endp;
        Value *v = alloc(); v->type = Value::NUMBER; v->number = d;
        return v;
    }
    fail("unexpected character");
    return nullptr;
}

bool Document::parse(const char *text, size_t len, std::string *error) {
    begin_ = p_ = text; end_ = text + len; err_ = error;
    root_ = parse_value();
    return root_ != nullptr;
}

} // namespace daijson
