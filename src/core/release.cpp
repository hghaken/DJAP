#include "release.h"

#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <utility>
#include <vector>

namespace ap {

namespace {

// ---- a small JSON reader: enough for GitHub's release answer (objects, arrays, strings, numbers, true / false /
// null); strings are UTF-8, \u escapes included ----

struct Json {
    enum Type { Null, Bool, Number, String, Array, Object } type = Null;
    bool b = false;
    double num = 0.0;
    std::string str;
    std::vector<Json> arr;
    std::vector<std::pair<std::string, Json>> obj;

    const Json* get(const char* key) const {
        if (type != Object) return nullptr;
        for (const auto& kv : obj)
            if (kv.first == key) return &kv.second;
        return nullptr;
    }
    std::string text(const char* key) const {
        const Json* v = get(key);
        return v && v->type == String ? v->str : std::string();
    }
};

class Reader {
public:
    explicit Reader(const std::string& s) : s_(s) {}
    bool parse(Json& out, std::string& error) {
        if (!value(out, 0)) {
            error = "not valid JSON near character " + std::to_string(pos_);
            return false;
        }
        space();
        if (pos_ != s_.size()) {
            error = "text after the JSON value";
            return false;
        }
        return true;
    }

private:
    void space() {
        while (pos_ < s_.size() && (s_[pos_] == ' ' || s_[pos_] == '\t' || s_[pos_] == '\n' || s_[pos_] == '\r')) ++pos_;
    }
    bool literal(const char* word) {
        const size_t n = std::strlen(word);
        if (s_.compare(pos_, n, word) != 0) return false;
        pos_ += n;
        return true;
    }
    static void utf8(std::string& out, uint32_t c) {
        if (c < 0x80) {
            out += static_cast<char>(c);
        } else if (c < 0x800) {
            out += static_cast<char>(0xC0 | (c >> 6));
            out += static_cast<char>(0x80 | (c & 0x3F));
        } else if (c < 0x10000) {
            out += static_cast<char>(0xE0 | (c >> 12));
            out += static_cast<char>(0x80 | ((c >> 6) & 0x3F));
            out += static_cast<char>(0x80 | (c & 0x3F));
        } else {
            out += static_cast<char>(0xF0 | (c >> 18));
            out += static_cast<char>(0x80 | ((c >> 12) & 0x3F));
            out += static_cast<char>(0x80 | ((c >> 6) & 0x3F));
            out += static_cast<char>(0x80 | (c & 0x3F));
        }
    }
    bool hex4(uint32_t& v) {
        if (pos_ + 4 > s_.size()) return false;
        v = 0;
        for (int i = 0; i < 4; ++i) {
            const char c = s_[pos_++];
            v <<= 4;
            if (c >= '0' && c <= '9') v |= static_cast<uint32_t>(c - '0');
            else if (c >= 'a' && c <= 'f') v |= static_cast<uint32_t>(c - 'a' + 10);
            else if (c >= 'A' && c <= 'F') v |= static_cast<uint32_t>(c - 'A' + 10);
            else return false;
        }
        return true;
    }
    bool string(std::string& out) {
        if (pos_ >= s_.size() || s_[pos_] != '"') return false;
        ++pos_;
        while (pos_ < s_.size()) {
            const char c = s_[pos_++];
            if (c == '"') return true;
            if (static_cast<unsigned char>(c) < 0x20) return false;
            if (c != '\\') {
                out += c;
                continue;
            }
            if (pos_ >= s_.size()) return false;
            const char e = s_[pos_++];
            switch (e) {
            case '"': out += '"'; break;
            case '\\': out += '\\'; break;
            case '/': out += '/'; break;
            case 'b': out += '\b'; break;
            case 'f': out += '\f'; break;
            case 'n': out += '\n'; break;
            case 'r': out += '\r'; break;
            case 't': out += '\t'; break;
            case 'u': {
                uint32_t c1;
                if (!hex4(c1)) return false;
                if (c1 >= 0xD800 && c1 < 0xDC00) { // a surrogate pair
                    uint32_t c2;
                    if (pos_ + 2 > s_.size() || s_[pos_] != '\\' || s_[pos_ + 1] != 'u') return false;
                    pos_ += 2;
                    if (!hex4(c2) || c2 < 0xDC00 || c2 >= 0xE000) return false;
                    c1 = 0x10000 + ((c1 - 0xD800) << 10) + (c2 - 0xDC00);
                }
                utf8(out, c1);
                break;
            }
            default:
                return false;
            }
        }
        return false;
    }
    bool value(Json& v, int depth) {
        if (depth > 64) return false;
        space();
        if (pos_ >= s_.size()) return false;
        const char c = s_[pos_];
        if (c == '{') {
            ++pos_;
            v.type = Json::Object;
            space();
            if (pos_ < s_.size() && s_[pos_] == '}') {
                ++pos_;
                return true;
            }
            for (;;) {
                space();
                std::string key;
                if (!string(key)) return false;
                space();
                if (pos_ >= s_.size() || s_[pos_++] != ':') return false;
                Json item;
                if (!value(item, depth + 1)) return false;
                v.obj.emplace_back(std::move(key), std::move(item));
                space();
                if (pos_ >= s_.size()) return false;
                if (s_[pos_] == ',') {
                    ++pos_;
                    continue;
                }
                if (s_[pos_] == '}') {
                    ++pos_;
                    return true;
                }
                return false;
            }
        }
        if (c == '[') {
            ++pos_;
            v.type = Json::Array;
            space();
            if (pos_ < s_.size() && s_[pos_] == ']') {
                ++pos_;
                return true;
            }
            for (;;) {
                Json item;
                if (!value(item, depth + 1)) return false;
                v.arr.push_back(std::move(item));
                space();
                if (pos_ >= s_.size()) return false;
                if (s_[pos_] == ',') {
                    ++pos_;
                    continue;
                }
                if (s_[pos_] == ']') {
                    ++pos_;
                    return true;
                }
                return false;
            }
        }
        if (c == '"') {
            v.type = Json::String;
            return string(v.str);
        }
        if (literal("true")) {
            v.type = Json::Bool;
            v.b = true;
            return true;
        }
        if (literal("false")) {
            v.type = Json::Bool;
            return true;
        }
        if (literal("null")) return true;
        // a number
        const size_t start = pos_;
        while (pos_ < s_.size() && std::strchr("+-0123456789.eE", s_[pos_]) && s_[pos_] != '\0') ++pos_;
        if (pos_ == start) return false;
        const std::string n = s_.substr(start, pos_ - start);
        char* end = nullptr;
        v.num = std::strtod(n.c_str(), &end);
        v.type = Json::Number;
        return end && *end == '\0';
    }

    const std::string& s_;
    size_t pos_ = 0;
};

bool endsWith(const std::string& s, const char* tail) {
    const size_t n = std::strlen(tail);
    return s.size() >= n && s.compare(s.size() - n, n, tail) == 0;
}

std::string lower(std::string s) {
    for (char& c : s)
        if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
    return s;
}

bool isHex64(const std::string& s) {
    if (s.size() != 64) return false;
    for (char c : s)
        if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'))) return false;
    return true;
}

} // namespace

bool parseLatestRelease(const std::string& json, ReleaseInfo& out, std::string& error) {
    out = ReleaseInfo();
    Json root;
    if (!Reader(json).parse(root, error)) return false;
    if (root.type != Json::Object) {
        error = "the answer is not a release";
        return false;
    }
    std::string tag = root.text("tag_name");
    if (!tag.empty() && (tag[0] == 'v' || tag[0] == 'V')) tag.erase(0, 1);
    if (tag.empty()) {
        error = "the release has no version tag";
        return false;
    }
    out.version = tag;
    out.page = root.text("html_url");
    const Json* assets = root.get("assets");
    if (assets && assets->type == Json::Array) {
        for (const Json& a : assets->arr) { // the setup first
            const std::string name = a.text("name"), l = lower(name);
            if (l.compare(0, 11, "djap-setup-") == 0 && endsWith(l, ".exe")) {
                out.setupName = name;
                out.setupUrl = a.text("browser_download_url");
                const Json* size = a.get("size");
                if (size && size->type == Json::Number && size->num > 0) out.setupSize = static_cast<uint64_t>(size->num);
                const std::string digest = lower(a.text("digest"));
                if (digest.compare(0, 7, "sha256:") == 0 && isHex64(digest.substr(7))) out.sha256 = digest.substr(7);
                break;
            }
        }
        for (const Json& a : assets->arr) // its checksum file, when there is one
            if (!out.setupName.empty() && lower(a.text("name")) == lower(out.setupName) + ".sha256")
                out.sha256Url = a.text("browser_download_url");
    }
    if (out.setupUrl.empty()) {
        error = "the release " + out.version + " has no setup program";
        return false;
    }
    return true;
}

int compareVersions(const std::string& a, const std::string& b) {
    auto parts = [](std::string s) {
        if (!s.empty() && (s[0] == 'v' || s[0] == 'V')) s.erase(0, 1);
        const size_t cut = s.find_first_of("+-");
        if (cut != std::string::npos) s.erase(cut);
        std::vector<unsigned long> p;
        size_t i = 0;
        while (i <= s.size()) {
            const size_t dot = s.find('.', i);
            const std::string part = s.substr(i, dot == std::string::npos ? std::string::npos : dot - i);
            p.push_back(std::strtoul(part.c_str(), nullptr, 10));
            if (dot == std::string::npos) break;
            i = dot + 1;
        }
        return p;
    };
    const std::vector<unsigned long> x = parts(a), y = parts(b);
    for (size_t i = 0; i < x.size() || i < y.size(); ++i) {
        const unsigned long u = i < x.size() ? x[i] : 0, v = i < y.size() ? y[i] : 0;
        if (u != v) return u < v ? -1 : 1;
    }
    return 0;
}

// ---- SHA-256 ----

namespace {
constexpr uint32_t kK[64] = {
    0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1, 0x923f82a4, 0xab1c5ed5,
    0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3, 0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174,
    0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc, 0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da,
    0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7, 0xc6e00bf3, 0xd5a79147, 0x06ca6351, 0x14292967,
    0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13, 0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85,
    0xa2bfe8a1, 0xa81a664b, 0xc24b8b70, 0xc76c51a3, 0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070,
    0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f, 0x682e6ff3,
    0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208, 0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2};
inline uint32_t rotr(uint32_t x, int n) { return (x >> n) | (x << (32 - n)); }
} // namespace

Sha256::Sha256() {
    const uint32_t init[8] = {0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a,
                              0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19};
    std::memcpy(h_, init, sizeof h_);
}

void Sha256::block(const uint8_t* p) {
    uint32_t w[64];
    for (int i = 0; i < 16; ++i)
        w[i] = static_cast<uint32_t>(p[4 * i]) << 24 | static_cast<uint32_t>(p[4 * i + 1]) << 16 |
               static_cast<uint32_t>(p[4 * i + 2]) << 8 | static_cast<uint32_t>(p[4 * i + 3]);
    for (int i = 16; i < 64; ++i) {
        const uint32_t s0 = rotr(w[i - 15], 7) ^ rotr(w[i - 15], 18) ^ (w[i - 15] >> 3);
        const uint32_t s1 = rotr(w[i - 2], 17) ^ rotr(w[i - 2], 19) ^ (w[i - 2] >> 10);
        w[i] = w[i - 16] + s0 + w[i - 7] + s1;
    }
    uint32_t a = h_[0], b = h_[1], c = h_[2], d = h_[3], e = h_[4], f = h_[5], g = h_[6], h = h_[7];
    for (int i = 0; i < 64; ++i) {
        const uint32_t t1 = h + (rotr(e, 6) ^ rotr(e, 11) ^ rotr(e, 25)) + ((e & f) ^ (~e & g)) + kK[i] + w[i];
        const uint32_t t2 = (rotr(a, 2) ^ rotr(a, 13) ^ rotr(a, 22)) + ((a & b) ^ (a & c) ^ (b & c));
        h = g;
        g = f;
        f = e;
        e = d + t1;
        d = c;
        c = b;
        b = a;
        a = t1 + t2;
    }
    h_[0] += a;
    h_[1] += b;
    h_[2] += c;
    h_[3] += d;
    h_[4] += e;
    h_[5] += f;
    h_[6] += g;
    h_[7] += h;
}

void Sha256::update(const void* data, size_t n) {
    const uint8_t* p = static_cast<const uint8_t*>(data);
    bytes_ += n;
    while (n > 0) {
        const size_t take = std::min(n, sizeof buf_ - used_);
        std::memcpy(buf_ + used_, p, take);
        used_ += take;
        p += take;
        n -= take;
        if (used_ == sizeof buf_) {
            block(buf_);
            used_ = 0;
        }
    }
}

std::string Sha256::hex() {
    const uint64_t bits = bytes_ * 8;
    const uint8_t one = 0x80, zero = 0;
    update(&one, 1);
    while (used_ != 56) update(&zero, 1);
    uint8_t len[8];
    for (int i = 0; i < 8; ++i) len[i] = static_cast<uint8_t>(bits >> (56 - 8 * i));
    update(len, 8);
    static const char* digits = "0123456789abcdef";
    std::string out;
    for (uint32_t v : h_)
        for (int s = 28; s >= 0; s -= 4) out += digits[(v >> s) & 0xF];
    return out;
}

} // namespace ap
