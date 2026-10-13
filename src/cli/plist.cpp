#include "plist.h"
#include <string.h>
#include <stdlib.h>

namespace cf::plist {

const Node* Node::get(const char* key) const {
    if (type != Type::Dict) return nullptr;
    for (size_t i = 0; i < keys.size(); ++i)
        if (keys[i] == key) return &items[i];
    return nullptr;
}

const std::string* Node::getString(const char* key) const {
    const Node* n = get(key);
    return (n && n->type == Type::String) ? &n->str : nullptr;
}

bool Node::getInt(const char* key, int64_t& out) const {
    const Node* n = get(key);
    if (!n || (n->type != Type::Integer && n->type != Type::Bool)) return false;
    out = n->integer;
    return true;
}

std::string escape(const std::string& s) {
    std::string o;
    o.reserve(s.size());
    for (char c : s) {
        switch (c) {
        case '&': o += "&amp;"; break;
        case '<': o += "&lt;"; break;
        case '>': o += "&gt;"; break;
        case '"': o += "&quot;"; break;
        case '\'': o += "&apos;"; break;
        default: o += c;
        }
    }
    return o;
}

// ---------------------------------------------------------------- writer

Writer& Writer::key(const char* k) { body_ += "<key>" + escape(k) + "</key>"; return *this; }
Writer& Writer::string(const std::string& v) { body_ += "<string>" + escape(v) + "</string>"; return *this; }
Writer& Writer::integer(int64_t v) { body_ += "<integer>" + std::to_string(v) + "</integer>"; return *this; }
Writer& Writer::boolean(bool v) { body_ += v ? "<true/>" : "<false/>"; return *this; }
Writer& Writer::beginDict() { body_ += "<dict>"; return *this; }
Writer& Writer::endDict() { body_ += "</dict>"; return *this; }

Writer& Writer::data(const uint8_t* p, size_t n) {
    static const char* tbl = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    body_ += "<data>";
    size_t i = 0;
    for (; i + 2 < n; i += 3) {
        const uint32_t v = (p[i] << 16) | (p[i + 1] << 8) | p[i + 2];
        body_ += tbl[(v >> 18) & 63]; body_ += tbl[(v >> 12) & 63]; body_ += tbl[(v >> 6) & 63]; body_ += tbl[v & 63];
    }
    if (n - i == 1) {
        const uint32_t v = p[i] << 16;
        body_ += tbl[(v >> 18) & 63]; body_ += tbl[(v >> 12) & 63]; body_ += "==";
    } else if (n - i == 2) {
        const uint32_t v = (p[i] << 16) | (p[i + 1] << 8);
        body_ += tbl[(v >> 18) & 63]; body_ += tbl[(v >> 12) & 63]; body_ += tbl[(v >> 6) & 63]; body_ += '=';
    }
    body_ += "</data>";
    return *this;
}

std::string Writer::finish() {
    return "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n"
           "<!DOCTYPE plist PUBLIC \"-//Apple//DTD PLIST 1.0//EN\" \"http://www.apple.com/DTDs/PropertyList-1.0.dtd\">\n"
           "<plist version=\"1.0\"><dict>" + body_ + "</dict></plist>\n";
}

// ---------------------------------------------------------------- reader

namespace {

struct Parser {
    const char* p;
    const char* end;

    void skipWs() {
        while (p < end) {
            if (*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n') { ++p; continue; }
            if (end - p >= 4 && !memcmp(p, "<!--", 4)) {
                const char* e = strstr_n(p + 4, "-->");
                p = e ? e + 3 : end;
                continue;
            }
            if (end - p >= 2 && (!memcmp(p, "<?", 2) || !memcmp(p, "<!", 2))) {
                while (p < end && *p != '>') ++p;
                if (p < end) ++p;
                continue;
            }
            break;
        }
    }

    const char* strstr_n(const char* from, const char* needle) {
        const size_t n = strlen(needle);
        for (const char* q = from; q + n <= end; ++q)
            if (!memcmp(q, needle, n)) return q;
        return nullptr;
    }

    // Reads "<name ...>" or "<name/>". Returns false if not at a tag.
    bool openTag(std::string& name, bool& selfClosing) {
        skipWs();
        if (p >= end || *p != '<' || (p + 1 < end && p[1] == '/')) return false;
        ++p;
        const char* s = p;
        while (p < end && *p != '>' && *p != ' ' && *p != '/') ++p;
        name.assign(s, p);
        while (p < end && *p != '>') ++p;
        if (p >= end) return false;
        selfClosing = p[-1] == '/';
        ++p;
        return true;
    }

    bool closeTag(const char* name) {
        skipWs();
        const size_t n = strlen(name);
        if (end - p < (ptrdiff_t)(n + 3) || p[0] != '<' || p[1] != '/' || memcmp(p + 2, name, n) || p[2 + n] != '>')
            return false;
        p += n + 3;
        return true;
    }

    bool text(const char* tag, std::string& out) {
        std::string close = std::string("</") + tag + ">";
        const char* e = strstr_n(p, close.c_str());
        if (!e) return false;
        out.clear();
        for (const char* q = p; q < e; ++q) {
            if (*q == '&') {
                if (!strncmp(q, "&amp;", 5)) { out += '&'; q += 4; }
                else if (!strncmp(q, "&lt;", 4)) { out += '<'; q += 3; }
                else if (!strncmp(q, "&gt;", 4)) { out += '>'; q += 3; }
                else if (!strncmp(q, "&quot;", 6)) { out += '"'; q += 5; }
                else if (!strncmp(q, "&apos;", 6)) { out += '\''; q += 5; }
                else out += *q;
            } else {
                out += *q;
            }
        }
        p = e + close.size();
        return true;
    }

    static int b64(char c) {
        if (c >= 'A' && c <= 'Z') return c - 'A';
        if (c >= 'a' && c <= 'z') return c - 'a' + 26;
        if (c >= '0' && c <= '9') return c - '0' + 52;
        if (c == '+') return 62;
        if (c == '/') return 63;
        return -1;
    }

    bool value(Node& n, int depth) {
        if (depth > 32) return false;
        std::string name;
        bool selfClosing = false;
        if (!openTag(name, selfClosing)) return false;
        if (name == "dict") {
            n.type = Node::Type::Dict;
            if (selfClosing) return true;
            while (true) {
                skipWs();
                if (closeTag("dict")) return true;
                std::string kname;
                bool sc = false;
                if (!openTag(kname, sc) || kname != "key") return false;
                std::string k;
                if (!sc && !text("key", k)) return false;
                Node v;
                if (!value(v, depth + 1)) return false;
                n.keys.push_back(std::move(k));
                n.items.push_back(std::move(v));
            }
        }
        if (name == "array") {
            n.type = Node::Type::Array;
            if (selfClosing) return true;
            while (true) {
                skipWs();
                if (closeTag("array")) return true;
                Node v;
                if (!value(v, depth + 1)) return false;
                n.items.push_back(std::move(v));
            }
        }
        if (name == "true" || name == "false") {
            n.type = Node::Type::Bool;
            n.integer = name == "true";
            return selfClosing || closeTag(name.c_str());
        }
        if (name == "string") {
            n.type = Node::Type::String;
            return selfClosing || text("string", n.str);
        }
        if (name == "integer") {
            n.type = Node::Type::Integer;
            std::string t;
            if (!text("integer", t)) return false;
            n.integer = _strtoi64(t.c_str(), nullptr, 10);
            return true;
        }
        if (name == "real" || name == "date") {
            n.type = Node::Type::Real;
            return selfClosing || text(name.c_str(), n.str);
        }
        if (name == "data") {
            n.type = Node::Type::Data;
            if (selfClosing) return true;
            std::string t;
            if (!text("data", t)) return false;
            uint32_t acc = 0;
            int bits = 0;
            for (char c : t) {
                const int v = b64(c);
                if (v < 0) continue;
                acc = (acc << 6) | (uint32_t)v;
                bits += 6;
                if (bits >= 8) {
                    bits -= 8;
                    n.data.push_back((uint8_t)(acc >> bits));
                }
            }
            return true;
        }
        return false;
    }
};

}  // namespace

bool parse(const char* xml, size_t n, Node& root) {
    Parser ps{xml, xml + n};
    std::string name;
    bool sc = false;
    if (!ps.openTag(name, sc)) return false;
    if (name != "plist") return false;
    if (!ps.value(root, 0)) return false;
    return ps.closeTag("plist");
}

}  // namespace cf::plist
