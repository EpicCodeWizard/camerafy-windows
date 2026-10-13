#pragma once
// Minimal XML property lists: an emitter for the few request shapes usbmux/lockdownd take
// and a tolerant reader. Control-path only; never on the frame path.
#include <stdint.h>
#include <string>
#include <vector>
#include <memory>

namespace cf::plist {

struct Node {
    enum class Type { None, Dict, Array, String, Integer, Data, Bool, Real } type = Type::None;
    std::string str;                  // String, and the raw text of Real
    int64_t integer = 0;              // Integer, Bool
    std::vector<uint8_t> data;        // Data
    std::vector<std::string> keys;    // Dict
    std::vector<Node> items;          // Dict values / Array items

    const Node* get(const char* key) const;
    const std::string* getString(const char* key) const;
    bool getInt(const char* key, int64_t& out) const;
};

// Emitter: builds <plist><dict>...</dict></plist>.
class Writer {
public:
    Writer& key(const char* k);
    Writer& string(const std::string& v);
    Writer& integer(int64_t v);
    Writer& data(const uint8_t* p, size_t n);
    Writer& boolean(bool v);
    Writer& beginDict();
    Writer& endDict();
    std::string finish();  // wraps the accumulated top-level dict body

    // Convenience: key + value
    Writer& kv(const char* k, const std::string& v) { return key(k).string(v); }
    Writer& kv(const char* k, int64_t v) { return key(k).integer(v); }

private:
    std::string body_;
};

// Returns false on malformed input. The root is the plist's top-level element.
bool parse(const char* xml, size_t n, Node& root);

std::string escape(const std::string& s);

}  // namespace cf::plist
