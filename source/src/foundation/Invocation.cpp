// NSMethodSignature / NSInvocation (cocos2d's CCMenuItem and CCCallFunc keep their target-action calls as
// invocations). Signatures come from the method's type encoding; arguments are kept as words (armv7: each argument
// takes 1 word, 2 for doubles / long longs, structures their size rounded up to words).
#include "foundation/Foundation.h"
#include "Log.h"
#include "Memory.h"
#include <cstring>

using namespace ns;
using objc::marg;
using objc::self;
using hle::ret;

namespace {
// One type from an Objective-C type encoding (skipping qualifiers and the frame offsets after it); returns its size.
std::size_t typeSize(const char *&t);
std::size_t structSize(const char *&t) {
    while (*t && *t != '=' && *t != '}') ++t;
    if (*t == '=') ++t;
    std::size_t size = 0;
    while (*t && *t != '}') size += typeSize(t);
    if (*t == '}') ++t;
    return size;
}
std::size_t typeSize(const char *&t) {
    while (*t && std::strchr("rnNoORV", *t)) ++t;
    const char c = *t ? *t++ : 'v';
    std::size_t size;
    switch (c) {
    case 'v': size = 0; break;
    case 'c': case 'C': case 'B': size = 1; break;
    case 's': case 'S': size = 2; break;
    case 'q': case 'Q': case 'd': size = 8; break;
    case '^': { const char *inner = t; typeSize(inner); t = inner; size = 4; break; }
    case '{': size = structSize(t); break;
    case '[': { int n = 0; while (std::isdigit(static_cast<unsigned char>(*t))) n = n * 10 + (*t++ - '0'); size = std::size_t(n) * typeSize(t); if (*t == ']') ++t; break; }
    case '"': while (*t && *t != '"') ++t; if (*t) ++t; return typeSize(t);
    default: size = 4; break; // i I l L f @ # : * ?
    }
    if (c == '@' && *t == '"') { ++t; while (*t && *t != '"') ++t; if (*t) ++t; }
    while (std::isdigit(static_cast<unsigned char>(*t)) || *t == '-' || *t == '+') ++t; // frame offset
    return size;
}
struct Signature : objc::HostObject {
    std::string types;
    std::vector<std::string> argTypes; // including self and _cmd
    std::string returnType;
    std::vector<std::size_t> argSizes;
    std::size_t returnSize = 0;
};
id makeSignature(const std::string &types) {
    auto s = std::make_unique<Signature>();
    s->types = types;
    const char *t = types.c_str();
    const char *start = t;
    s->returnSize = typeSize(t);
    s->returnType = std::string(start, t);
    while (*t) {
        start = t;
        s->argSizes.push_back(typeSize(t));
        s->argTypes.push_back(std::string(start, t));
    }
    const auto o = objc::allocInstance(objc::getClass("NSMethodSignature"));
    objc::setHostObject(o, std::move(s));
    return o;
}
Signature *signature(id o) { return objc::host<Signature>(o); }
// A method's types, or a guess from the selector (all object arguments, object result) for methods without them.
std::string methodTypes(objc::Class cls, objc::SEL sel) {
    std::string name = objc::selName(sel);
    std::string guess = "@8@0:4";
    for (char c : name) if (c == ':') guess += "@";
    return guess;
}
struct InvocationData : objc::HostObject {
    id signature = 0, target = 0;
    objc::SEL selector = 0;
    std::vector<std::vector<std::uint32_t>> arguments; // index 2... (0, 1 are target and selector)
    std::uint32_t result[4] = {};
};
InvocationData *invocation(id o) { return objc::host<InvocationData>(o); }
} // namespace

std::string objcMethodTypes(objc::Class cls, objc::SEL sel); // from ObjC.cpp

OBJC_CLASS(NSMethodSignature, NSObject);
OBJC_CLASS(NSInvocation, NSObject);
OBJC_CLASS_METHOD(NSMethodSignature, "signatureWithObjCTypes:", signatureWithObjCTypes) { ret(makeSignature(mem::cstring(marg(0)))); }
OBJC_METHOD(NSMethodSignature, "numberOfArguments", numberOfArguments) { auto *s = signature(self()); ret(s ? std::uint32_t(s->argSizes.size()) : 0); }
OBJC_METHOD(NSMethodSignature, "getArgumentTypeAtIndex:", getArgumentTypeAtIndex) {
    auto *s = signature(self());
    if (!s || marg(0) >= s->argTypes.size()) return ret(0);
    ret(mem::heapString(s->argTypes[std::size_t(marg(0))]));
}
OBJC_METHOD(NSMethodSignature, "methodReturnType", methodReturnType) { auto *s = signature(self()); ret(mem::heapString(s ? s->returnType : "v")); }
OBJC_METHOD(NSMethodSignature, "methodReturnLength", methodReturnLength) { auto *s = signature(self()); ret(s ? std::uint32_t(s->returnSize) : 0); }
OBJC_METHOD(NSMethodSignature, "frameLength", frameLength) {
    auto *s = signature(self());
    std::size_t n = 0;
    if (s) for (auto size : s->argSizes) n += (size + 3) & ~3u;
    ret(std::uint32_t(n));
}
OBJC_METHOD(NSMethodSignature, "isOneway", isOneway) { ret(0); }
OBJC_METHOD(NSObject, "methodSignatureForSelector:", methodSignatureForSelector) {
    const auto cls = objc::classOf(self());
    if (!objc::lookup(cls, marg(0))) return ret(0);
    auto types = objcMethodTypes(cls, marg(0));
    if (types.empty()) types = methodTypes(cls, marg(0));
    ret(makeSignature(types));
}
OBJC_CLASS_METHOD(NSObject, "instanceMethodSignatureForSelector:", instanceMethodSignatureForSelector) {
    if (!objc::lookup(self(), marg(0))) return ret(0);
    auto types = objcMethodTypes(self(), marg(0));
    if (types.empty()) types = methodTypes(self(), marg(0));
    ret(makeSignature(types));
}
OBJC_CLASS_METHOD(NSInvocation, "invocationWithMethodSignature:", invocationWithMethodSignature) {
    if (!marg(0)) return ret(0);
    const auto o = objc::allocInstance(self());
    auto d = std::make_unique<InvocationData>();
    d->signature = marg(0);
    objc::retain(marg(0));
    if (auto *s = signature(marg(0))) d->arguments.resize(s->argSizes.size() > 2 ? s->argSizes.size() - 2 : 0);
    objc::setHostObject(o, std::move(d));
    ret(o);
}
OBJC_METHOD(NSInvocation, "methodSignature", methodSignature) { auto *d = invocation(self()); ret(d ? d->signature : 0); }
OBJC_METHOD(NSInvocation, "setTarget:", setTarget) { if (auto *d = invocation(self())) d->target = marg(0); }
OBJC_METHOD(NSInvocation, "target", target) { auto *d = invocation(self()); ret(d ? d->target : 0); }
OBJC_METHOD(NSInvocation, "setSelector:", setSelector) { if (auto *d = invocation(self())) d->selector = marg(0); }
OBJC_METHOD(NSInvocation, "selector", selector) { auto *d = invocation(self()); ret(d ? d->selector : 0); }
OBJC_METHOD(NSInvocation, "retainArguments", retainArguments) {}
OBJC_METHOD(NSInvocation, "argumentsRetained", argumentsRetained) { ret(1); }
// setArgument:atIndex: copies the argument's bytes (its size from the signature); 0 and 1 are target and selector.
OBJC_METHOD(NSInvocation, "setArgument:atIndex:", setArgument) {
    auto *d = invocation(self());
    auto *s = d ? signature(d->signature) : nullptr;
    const auto index = std::size_t(marg(1));
    if (!d || !s || index >= s->argSizes.size()) return;
    if (index == 0) { d->target = mem::rp(marg(0)); return; }
    if (index == 1) { d->selector = mem::rp(marg(0)); return; }
    const std::size_t words = std::max<std::size_t>(1, (s->argSizes[index] + 3) / 4);
    std::vector<std::uint32_t> value(words, 0);
    std::memcpy(value.data(), mem::base() + marg(0), std::min(s->argSizes[index] ? s->argSizes[index] : 4, words * 4));
    d->arguments[index - 2] = value;
}
OBJC_METHOD(NSInvocation, "getArgument:atIndex:", getArgument) {
    auto *d = invocation(self());
    const auto index = std::size_t(marg(1));
    if (!d) return;
    if (index == 0) return mem::wp(marg(0), d->target);
    if (index == 1) return mem::wp(marg(0), d->selector);
    if (index - 2 < d->arguments.size())
        for (std::size_t i = 0; i < d->arguments[index - 2].size(); ++i) mem::w32(marg(0) + 4 * i, d->arguments[index - 2][i]);
}
static void invoke(id self_, id target) {
    auto *d = invocation(self_);
    auto *s = d ? signature(d->signature) : nullptr;
    if (!d || !target || !d->selector) return;
    const auto imp = objc::lookup(objc::classOf(target), d->selector);
    if (!imp) { objc::logUnrecognized(target, d->selector); return; }
    // Structure results larger than 4 bytes go through a hidden result pointer first.
    const bool stret = s && s->returnSize > 4 && !s->returnType.empty() && s->returnType[0] == '{';
    std::vector<std::uint32_t> words;
    std::uint64_t buffer = 0;
    if (stret) { buffer = mem::calloc(s->returnSize); words.push_back(std::uint32_t(buffer)); }
    words.push_back(std::uint32_t(target));
    words.push_back(std::uint32_t(d->selector));
    for (auto &a : d->arguments) {
        if (a.empty()) words.push_back(0);
        else words.insert(words.end(), a.begin(), a.end());
    }
    const auto r0 = cpu::callWords(imp, words);
    if (stret) {
        std::memcpy(d->result, mem::base() + buffer, std::min<std::size_t>(s->returnSize, sizeof d->result));
        mem::free(buffer);
    } else {
        d->result[0] = std::uint32_t(r0);
        d->result[1] = cpu::lastHigh();
    }
}
OBJC_METHOD(NSInvocation, "invoke", invoke_) { auto *d = invocation(self()); if (d) invoke(self(), d->target); }
OBJC_METHOD(NSInvocation, "invokeWithTarget:", invokeWithTarget) { invoke(self(), marg(0)); }
OBJC_METHOD(NSInvocation, "getReturnValue:", getReturnValue) {
    auto *d = invocation(self());
    auto *s = d ? signature(d->signature) : nullptr;
    if (!d || !s || !s->returnSize) return;
    std::memcpy(mem::base() + marg(0), d->result, std::min<std::size_t>(s->returnSize, sizeof d->result));
}
OBJC_METHOD(NSInvocation, "setReturnValue:", setReturnValue) {
    auto *d = invocation(self());
    if (d) std::memcpy(d->result, mem::base() + marg(0), 8);
}

// Message forwarding (objc_msgSend with no method but an overridden -forwardInvocation:): an invocation holding the
// message's arguments, read from argument word `firstWord` on (r2 for objc_msgSend), and its result afterwards.
objc::id forwardingInvocation(objc::id signatureObject, objc::id target, objc::SEL selector, int firstWord) {
    auto *s = signature(signatureObject);
    if (!s) return 0;
    const auto o = objc::allocInstance(objc::getClass("NSInvocation"));
    auto d = std::make_unique<InvocationData>();
    d->signature = signatureObject;
    objc::retain(signatureObject);
    d->target = target;
    d->selector = selector;
    int word = firstWord;
    for (std::size_t i = 2; i < s->argSizes.size(); ++i) {
        const std::size_t words = std::max<std::size_t>(1, (s->argSizes[i] + 3) / 4);
        std::vector<std::uint32_t> value;
        for (std::size_t k = 0; k < words; ++k) value.push_back(std::uint32_t(hle::arg(word++)));
        d->arguments.push_back(std::move(value));
    }
    objc::setHostObject(o, std::move(d));
    return o;
}
void forwardedResult(objc::id invocationObject, std::uint32_t &r0, std::uint32_t &r1) {
    auto *d = invocation(invocationObject);
    r0 = d ? d->result[0] : 0;
    r1 = d ? d->result[1] : 0;
}

// ======================================== Key-value coding ========================================
// -setValue:forKey: calls -set<Key>: with the value unboxed for the setter's argument type (cocos2d's CCActionTween
// tweens opacity, scale, position... this way); -valueForKey: calls the getter and boxes its result. Without an
// accessor the call is logged and ignored (or answers nil).
namespace {
std::string typeAfterSelfAndCmd(const std::string &types, int index) {
    const char *t = types.c_str();
    typeSize(t); // return type
    for (int i = 0; *t; ++i) {
        const char *start = t;
        typeSize(t);
        if (i == index + 2) return std::string(start, t);
    }
    return "@";
}
char baseType(const std::string &type) {
    for (char c : type) if (!std::strchr("rnNoORV", c)) return c;
    return '@';
}
// Without accessors KVC uses the instance variable _key, _isKey, key or isKey (accessInstanceVariablesDirectly).
bool ivarFor(id object, const std::string &key, std::uint32_t &offset, std::string &type) {
    std::string capital = key;
    capital[0] = char(std::toupper(static_cast<unsigned char>(capital[0])));
    for (const auto &name : {"_" + key, "_is" + capital, key, "is" + capital})
        if (objc::findIvar(objc::classOf(object), name, offset, type)) return true;
    return false;
}
bool setIvar(id object, const std::string &key, id value) {
    std::uint32_t offset;
    std::string type;
    if (!ivarFor(object, key, offset, type)) return false;
    const auto at = object + offset;
    switch (baseType(type)) {
    case 'f': mem::w32(at, cpu::floatBits(float(doubleValue(value)))); break;
    case 'd': { const double d = doubleValue(value); std::uint64_t bits; std::memcpy(&bits, &d, 8); mem::w64(at, bits); break; }
    case 'c': case 'C': case 'B': mem::write<std::uint8_t>(at, std::uint8_t(intValue(value))); break;
    case 's': case 'S': mem::write<std::uint16_t>(at, std::uint16_t(intValue(value))); break;
    case 'i': case 'I': case 'l': case 'L': mem::w32(at, std::uint32_t(intValue(value))); break;
    case 'q': case 'Q': mem::w64(at, std::uint64_t(intValue(value))); break;
    case '@': {
        const auto old = mem::rp(at);
        if (value) objc::retain(value);
        mem::wp(at, value);
        if (old) objc::release(old);
        break;
    }
    default: return false;
    }
    return true;
}
bool getIvar(id object, const std::string &key, std::uint64_t &result) {
    std::uint32_t offset;
    std::string type;
    if (!ivarFor(object, key, offset, type)) return false;
    const auto at = object + offset;
    switch (baseType(type)) {
    case 'f': result = numberDouble(cpu::bitsFloat(mem::r32(at)), 'f'); break;
    case 'd': { const auto bits = mem::r64(at); double d; std::memcpy(&d, &bits, 8); result = numberDouble(d); break; }
    case 'c': case 'B': result = number(std::int8_t(mem::read<std::uint8_t>(at)), 'c'); break;
    case 'C': result = number(mem::read<std::uint8_t>(at), 'C'); break;
    case 's': result = number(std::int16_t(mem::read<std::uint16_t>(at)), 's'); break;
    case 'S': result = number(mem::read<std::uint16_t>(at), 'S'); break;
    case 'i': case 'l': result = number(std::int32_t(mem::r32(at)), 'i'); break;
    case 'I': case 'L': result = number(mem::r32(at), 'I'); break;
    case '@': result = mem::rp(at); break;
    default: return false;
    }
    return true;
}
} // namespace
OBJC_METHOD(NSObject, "setValue:forKey:", setValueForKey) {
    const auto value = marg(0);
    auto key = text(marg(1));
    if (key.empty()) return;
    const std::string setter = "set" + std::string(1, char(std::toupper(static_cast<unsigned char>(key[0])))) + key.substr(1) + ":";
    const auto cls = objc::classOf(self());
    const auto sel = objc::sel(setter);
    if (!objc::lookup(cls, sel)) {
        if (setIvar(self(), key, value)) return;
        hlog("kvc", "%s has no %s", className(self()).c_str(), setter.c_str());
        return;
    }
    const char type = baseType(typeAfterSelfAndCmd(objcMethodTypes(cls, sel), 0));
    switch (type) {
    case 'f': objc::send(self(), setter, {cpu::floatBits(float(doubleValue(value)))}); break;
    case 'd': {
        const double d = doubleValue(value);
        std::uint64_t bits;
        std::memcpy(&bits, &d, 8);
        objc::send(self(), setter, {bits & 0xFFFFFFFF, bits >> 32});
        break;
    }
    case 'c': case 'C': case 's': case 'S': case 'i': case 'I': case 'l': case 'L': case 'B':
        objc::send(self(), setter, {std::uint32_t(intValue(value))});
        break;
    case 'q': case 'Q': {
        const auto v = std::uint64_t(intValue(value));
        objc::send(self(), setter, {v & 0xFFFFFFFF, v >> 32});
        break;
    }
    default: objc::send(self(), setter, {value}); break;
    }
}
OBJC_METHOD(NSObject, "valueForKey:", valueForKey) {
    const auto key = text(marg(0));
    const auto cls = objc::classOf(self());
    const auto sel = objc::sel(key);
    if (key.empty()) return ret(0);
    if (!objc::lookup(cls, sel)) {
        std::uint64_t value;
        if (getIvar(self(), key, value)) return ret(value);
        hlog("kvc", "%s has no %s", className(self()).c_str(), key.c_str());
        return ret(0);
    }
    const auto types = objcMethodTypes(cls, sel);
    const char *t = types.c_str();
    const char *start = t;
    typeSize(t);
    const char type = baseType(std::string(start, t));
    switch (type) {
    case 'f': return ret(numberDouble(objc::sendFloat(self(), key), 'f'));
    case 'd': return ret(numberDouble(objc::sendDouble(self(), key)));
    case 'c': case 'B': return ret(number(std::int8_t(objc::send(self(), key)), 'c'));
    case 'C': return ret(number(std::uint8_t(objc::send(self(), key)), 'C'));
    case 's': return ret(number(std::int16_t(objc::send(self(), key)), 's'));
    case 'S': return ret(number(std::uint16_t(objc::send(self(), key)), 'S'));
    case 'i': case 'l': return ret(number(std::int32_t(objc::send(self(), key)), 'i'));
    case 'I': case 'L': return ret(number(std::uint32_t(objc::send(self(), key)), 'I'));
    default: return ret(objc::send(self(), key));
    }
}
