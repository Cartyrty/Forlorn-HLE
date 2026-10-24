#include "foundation/PList.h"
#include "Memory.h"
#include <filesystem>
#include <fstream>
#include <sstream>

namespace plist {
namespace {
std::string escape(const std::string &s) {
    std::string out;
    for (char c : s) {
        if (c == '<') out += "&lt;";
        else if (c == '>') out += "&gt;";
        else if (c == '&') out += "&amp;";
        else out.push_back(c);
    }
    return out;
}
std::string unescape(const std::string &s) {
    std::string out;
    for (std::size_t i = 0; i < s.size(); ++i) {
        if (s[i] == '&') {
            if (s.compare(i, 4, "&lt;") == 0) { out.push_back('<'); i += 3; continue; }
            if (s.compare(i, 4, "&gt;") == 0) { out.push_back('>'); i += 3; continue; }
            if (s.compare(i, 5, "&amp;") == 0) { out.push_back('&'); i += 4; continue; }
        }
        out.push_back(s[i]);
    }
    return out;
}
const char *kB64 = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
std::string base64(const std::uint8_t *p, std::size_t n) {
    std::string out;
    for (std::size_t i = 0; i < n; i += 3) {
        const std::uint32_t v = (std::uint32_t(p[i]) << 16) | (i + 1 < n ? std::uint32_t(p[i + 1]) << 8 : 0) | (i + 2 < n ? p[i + 2] : 0);
        out += kB64[(v >> 18) & 63];
        out += kB64[(v >> 12) & 63];
        out += i + 1 < n ? kB64[(v >> 6) & 63] : '=';
        out += i + 2 < n ? kB64[v & 63] : '=';
    }
    return out;
}
std::string unbase64(const std::string &s) {
    std::string out;
    std::uint32_t buffer = 0;
    int bits = 0;
    for (char c : s) {
        const char *p = c ? std::strchr(kB64, c) : nullptr;
        if (!p) continue;
        buffer = (buffer << 6) | std::uint32_t(p - kB64);
        bits += 6;
        if (bits >= 8) { bits -= 8; out.push_back(char((buffer >> bits) & 0xFF)); }
    }
    return out;
}
void write(ns::id o, std::string &out, int depth) {
    const std::string pad(std::size_t(depth), '\t');
    if (!o) { out += pad + "<string></string>\n"; return; }
    if (ns::isString(o)) { out += pad + "<string>" + escape(ns::text(o)) + "</string>\n"; return; }
    if (auto *n = objc::host<ns::NumberData>(o)) {
        if (n->kind == ns::NumberData::Bool) out += pad + (n->i ? "<true/>\n" : "<false/>\n");
        else if (n->kind == ns::NumberData::Double) { char b[64]; std::snprintf(b, sizeof b, "%.17g", n->d); out += pad + "<real>" + b + "</real>\n"; }
        else out += pad + "<integer>" + std::to_string(n->i) + "</integer>\n";
        return;
    }
    if (objc::host<ns::DataBytes>(o)) {
        out += pad + "<data>" + base64(mem::base() + ns::dataBytes(o), std::size_t(ns::dataLength(o))) + "</data>\n";
        return;
    }
    if (auto *d = objc::host<ns::DictData>(o)) {
        out += pad + "<dict>\n";
        for (auto &[k, v] : d->items) {
            out += pad + "\t<key>" + escape(ns::text(k)) + "</key>\n";
            write(v, out, depth + 1);
        }
        out += pad + "</dict>\n";
        return;
    }
    if (objc::host<ns::ArrayData>(o)) {
        out += pad + "<array>\n";
        for (auto i : ns::arrayItems(o)) write(i, out, depth + 1);
        out += pad + "</array>\n";
        return;
    }
    if (objc::isKindOf(o, "NSDate")) {
        char b[64];
        std::snprintf(b, sizeof b, "%.6f", objc::sendDouble(o, "timeIntervalSinceReferenceDate"));
        out += pad + "<real>" + b + "</real>\n";
        return;
    }
    out += pad + "<string>" + escape(ns::describe(o)) + "</string>\n";
}
struct Parser {
    const std::string &s;
    std::size_t i = 0;
    bool mutableContainers;
    void skip() { while (i < s.size() && std::isspace(static_cast<unsigned char>(s[i]))) ++i; }
    std::string tag() {
        skip();
        if (i >= s.size() || s[i] != '<') return {};
        const auto end = s.find('>', i);
        const auto t = s.substr(i + 1, end - i - 1);
        i = end + 1;
        return t;
    }
    std::string textUntil(const std::string &close) {
        const auto end = s.find(close, i);
        const auto t = s.substr(i, end - i);
        i = end + close.size();
        return t;
    }
    ns::id value() {
        for (;;) {
            const auto t = tag();
            if (t.empty()) return 0;
            if (t[0] == '?' || t[0] == '!' || t.rfind("plist", 0) == 0) continue;
            if (t == "string") return ns::string(unescape(textUntil("</string>")));
            if (t == "string/") return ns::string("");
            if (t == "integer") return ns::number(std::atoll(textUntil("</integer>").c_str()));
            if (t == "real") return ns::numberDouble(std::atof(textUntil("</real>").c_str()));
            if (t == "true/") return ns::numberBool(true);
            if (t == "false/") return ns::numberBool(false);
            if (t == "data") { const auto bytes = unbase64(textUntil("</data>")); return ns::data(bytes.data(), bytes.size(), mutableContainers); }
            if (t == "date") return ns::string(textUntil("</date>"));
            if (t == "dict/") return ns::dictionary({}, mutableContainers);
            if (t == "array/") return ns::array({}, mutableContainers);
            if (t == "dict") {
                std::vector<std::pair<ns::id, ns::id>> items;
                for (;;) {
                    skip();
                    if (s.compare(i, 7, "</dict>") == 0) { i += 7; break; }
                    if (tag() != "key") return 0;
                    const auto key = ns::string(unescape(textUntil("</key>")));
                    items.push_back({key, value()});
                }
                return ns::dictionary(items, mutableContainers);
            }
            if (t == "array") {
                std::vector<ns::id> items;
                for (;;) {
                    skip();
                    if (s.compare(i, 8, "</array>") == 0) { i += 8; break; }
                    items.push_back(value());
                }
                return ns::array(items, mutableContainers);
            }
            return 0;
        }
    }
};
// Binary property lists (bplist00): an offset table indexes the objects; containers refer to objects by index.
// Trailer (last 32 bytes): 6 unused, offset int size, object ref size, object count, top object, offset table offset.
struct BinaryParser {
    const std::string &s;
    bool mutableContainers;
    std::size_t offsetSize = 0, refSize = 0;
    std::uint64_t count = 0, top = 0, tableOffset = 0;
    std::uint64_t be(std::size_t at, std::size_t n) const {
        std::uint64_t v = 0;
        for (std::size_t k = 0; k < n && at + k < s.size(); ++k) v = (v << 8) | static_cast<unsigned char>(s[at + k]);
        return v;
    }
    bool init() {
        if (s.size() < 40) return false;
        const auto t = s.size() - 32;
        offsetSize = static_cast<unsigned char>(s[t + 6]);
        refSize = static_cast<unsigned char>(s[t + 7]);
        count = be(t + 8, 8);
        top = be(t + 16, 8);
        tableOffset = be(t + 24, 8);
        return offsetSize && refSize && tableOffset < s.size();
    }
    std::size_t offsetOf(std::uint64_t index) const { return std::size_t(be(std::size_t(tableOffset + index * offsetSize), offsetSize)); }
    // Length from the marker's low nibble, or a following int object when it is 0xF.
    std::uint64_t length(std::size_t &at, unsigned marker) const {
        std::uint64_t n = marker & 0x0F;
        if (n == 0x0F) {
            const unsigned intMarker = static_cast<unsigned char>(s[at]);
            const std::size_t bytes = std::size_t(1) << (intMarker & 0x0F);
            n = be(at + 1, bytes);
            at += 1 + bytes;
        }
        return n;
    }
    ns::id object(std::uint64_t index, int depth = 0) {
        if (index >= count || depth > 64) return 0;
        std::size_t at = offsetOf(index);
        const unsigned marker = static_cast<unsigned char>(s[at++]);
        switch (marker >> 4) {
        case 0x0:
            if (marker == 0x08) return ns::numberBool(false);
            if (marker == 0x09) return ns::numberBool(true);
            return ns::null();
        case 0x1: {
            const std::size_t bytes = std::size_t(1) << (marker & 0x0F);
            const auto raw = be(at, bytes);
            const std::int64_t v = bytes == 8 ? std::int64_t(raw) : bytes == 4 ? std::int64_t(raw) : std::int64_t(raw);
            return ns::number(v);
        }
        case 0x2: {
            const std::size_t bytes = std::size_t(1) << (marker & 0x0F);
            const auto raw = be(at, bytes);
            if (bytes == 4) { std::uint32_t r = std::uint32_t(raw); float f; std::memcpy(&f, &r, 4); return ns::numberDouble(f, 'f'); }
            double d; std::memcpy(&d, &raw, 8); return ns::numberDouble(d);
        }
        case 0x3: {
            const auto raw = be(at, 8);
            double d; std::memcpy(&d, &raw, 8);
            return ns::dateWithTimeIntervalSinceReferenceDate(d);
        }
        case 0x4: {
            const auto n = length(at, marker);
            return ns::data(s.data() + at, std::size_t(n), mutableContainers);
        }
        case 0x5: {
            const auto n = length(at, marker);
            return ns::string(std::string(s.data() + at, std::size_t(n)));
        }
        case 0x6: {
            const auto n = length(at, marker);
            std::u16string text;
            for (std::uint64_t k = 0; k < n; ++k) text.push_back(char16_t(be(at + 2 * k, 2)));
            return ns::stringU16(text);
        }
        case 0x8: return ns::number(std::int64_t(be(at, (marker & 0x0F) + 1)));
        case 0xA: case 0xC: {
            const auto n = length(at, marker);
            std::vector<ns::id> items;
            for (std::uint64_t k = 0; k < n; ++k) items.push_back(object(be(at + k * refSize, refSize), depth + 1));
            return (marker >> 4) == 0xA ? ns::array(items, mutableContainers) : ns::set(items, mutableContainers);
        }
        case 0xD: {
            const auto n = length(at, marker);
            std::vector<std::pair<ns::id, ns::id>> items;
            for (std::uint64_t k = 0; k < n; ++k)
                items.push_back({object(be(at + k * refSize, refSize), depth + 1), object(be(at + (n + k) * refSize, refSize), depth + 1)});
            return ns::dictionary(items, mutableContainers);
        }
        default: return 0;
        }
    }
};
} // namespace
std::string serialize(ns::id root) {
    std::string out = "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n<!DOCTYPE plist PUBLIC \"-//Apple//DTD PLIST 1.0//EN\" "
                      "\"http://www.apple.com/DTDs/PropertyList-1.0.dtd\">\n<plist version=\"1.0\">\n";
    write(root, out, 0);
    return out + "</plist>\n";
}
ns::id parse(const std::string &xml, bool mutableContainers) {
    if (xml.rfind("bplist00", 0) == 0) {
        BinaryParser b{xml, mutableContainers};
        return b.init() ? b.object(b.top) : 0;
    }
    Parser p{xml, 0, mutableContainers};
    return p.value();
}
bool writeFile(ns::id root, const std::string &hostPath) {
    std::filesystem::create_directories(std::filesystem::u8path(hostPath).parent_path());
    std::ofstream out(std::filesystem::u8path(hostPath), std::ios::binary);
    const auto text = serialize(root);
    out.write(text.data(), std::streamsize(text.size()));
    return bool(out);
}
ns::id readFile(const std::string &hostPath, bool mutableContainers) {
    std::ifstream in(std::filesystem::u8path(hostPath), std::ios::binary);
    if (!in) return 0;
    const std::string text((std::istreambuf_iterator<char>(in)), {});
    return parse(text, mutableContainers);
}
} // namespace plist
