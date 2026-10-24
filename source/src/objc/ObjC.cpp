#include "objc/ObjC.h"
#include "Loader.h"
#include "Log.h"
#include "Memory.h"
#include <algorithm>
#include <cstring>
#include <cctype>
#include <map>
#include <mutex>
#include <set>
#include <unordered_map>
#include <unordered_set>

namespace objc {
namespace {
// 32-bit objc2 (armv7) layouts:
// class_t: isa +0, superclass +4, cache +8, vtable +12, data +16 (-> class_ro_t, low 2 bits flags).
// class_ro_t: flags +0, instanceStart +4, instanceSize +8, ivarLayout +12, name +16, baseMethods +20,
//             baseProtocols +24, ivars +28, weakIvarLayout +32, baseProperties +36.
// method_t: name +0, types +4, imp +8. category_t: name, cls, instanceMethods, classMethods, protocols, properties.
// protocol_t: isa +0, name +4, protocols +8. protocol_list_t: count (word), then pointers.
constexpr std::uint64_t kDataMask = 0xFFFFFFFCull;
constexpr std::uint32_t RO_META = 1, RO_ROOT = 2;

struct ClassInfo {
    Class cls = 0;
    std::string name;
    bool meta = false, host = false, initialized = false, initializing = false;
    Class instanceClass = 0; // for a metaclass: its class
    std::uint32_t instanceSize = 8;
    std::unordered_map<SEL, std::uint64_t> methods; // selector -> method_t (name, types, imp) in guest memory
    std::vector<std::string> protocols;
};

std::recursive_mutex g_lock;
std::unordered_map<std::string, SEL> g_selectors;
std::unordered_map<SEL, std::string> g_selectorNames;
std::unordered_map<Class, ClassInfo> g_classes;
std::unordered_map<std::string, Class> g_classByName;
std::map<std::pair<Class, SEL>, std::uint64_t> g_cache;
std::unordered_map<std::string, ClassDef> &defs() { static std::unordered_map<std::string, ClassDef> m; return m; }
std::uint64_t g_emptyCache = 0;
Class g_rootMeta = 0, g_root = 0;

std::mutex g_hostLock;
std::unordered_map<id, std::unique_ptr<HostObject>> g_hostObjects;
std::mutex g_refLock;
std::unordered_map<id, std::int64_t> g_refCounts; // extra references beyond the initial one
std::unordered_set<id> g_deallocated;

std::uint64_t emptyCache() {
    if (!g_emptyCache) g_emptyCache = mem::hleAlloc(16, 16);
    return g_emptyCache;
}
std::uint64_t methodStruct(SEL sel, std::uint64_t imp, const char *types) {
    const auto m = mem::hleAlloc(12, 4);
    mem::wp(m, sel);
    mem::wp(m + 4, types ? mem::hleString(types) : 0);
    mem::wp(m + 8, imp);
    return m;
}
ClassInfo *info(Class cls) {
    auto found = g_classes.find(cls);
    return found == g_classes.end() ? nullptr : &found->second;
}
// Reads a method_list_t (entsize/flags, count, then entries) into the class's method table.
void addMethodList(ClassInfo &ci, std::uint64_t list) {
    if (!list) return;
    const auto entsize = mem::r32(list) & ~3u, count = mem::r32(list + 4);
    for (std::uint32_t i = 0; i < count; ++i) {
        const auto m = list + 8 + std::uint64_t(i) * entsize;
        const SEL canonical = sel(mem::cstring(mem::rp(m)));
        mem::wp(m, canonical);
        ci.methods[canonical] = m;
    }
}
void addProtocols(ClassInfo &ci, std::uint64_t list) {
    if (!list) return;
    const auto count = mem::rp(list);
    for (std::uint64_t i = 0; i < count; ++i) {
        const auto proto = mem::rp(list + 4 + 4 * i);
        if (!proto) continue;
        ci.protocols.push_back(mem::cstring(mem::rp(proto + 4)));
        addProtocols(ci, mem::rp(proto + 8)); // inherited protocols
    }
}
// Registers a class struct already present in guest memory (from the app image).
void registerGuestClass(Class cls, bool meta) {
    if (info(cls)) return;
    ClassInfo ci;
    ci.cls = cls;
    ci.meta = meta;
    const auto ro = mem::rp(cls + 16) & kDataMask;
    ci.name = mem::cstring(mem::rp(ro + 16));
    ci.instanceSize = mem::r32(ro + 8);
    addMethodList(ci, mem::rp(ro + 20));
    addProtocols(ci, mem::rp(ro + 24));
    // Empty method caches: nothing in the app reads them, but keep them valid.
    mem::wp(cls + 8, emptyCache());
    // Method names for crash descriptions (the app is stripped).
    for (const auto &[s, m] : ci.methods)
        loader::addSymbol(std::string(meta ? "+[" : "-[") + ci.name + " " + mem::cstring(s) + "]", mem::rp(m + 8));
    g_classes.emplace(cls, std::move(ci));
}

// Non-fragile ivars: a subclass compiled against a smaller superclass (one whose ivars were synthesized later) has its
// ivars moved past the superclass's, keeping their largest alignment, as the system runtime does at realization.
void slideIvars(Class cls, std::set<Class> &done) {
    if (!cls || !done.insert(cls).second) return;
    auto *ci = info(cls);
    const Class super = mem::rp(cls + 4);
    if (!ci || ci->host || !super) return;
    slideIvars(super, done);
    const auto *si = info(super);
    if (!si) return;
    const auto ro = mem::rp(cls + 16) & kDataMask;
    const std::uint32_t start = mem::r32(ro + 4);
    if (si->instanceSize <= start) return;
    std::uint32_t diff = si->instanceSize - start;
    const auto ivars = mem::rp(ro + 28);
    if (ivars) {
        const std::uint32_t size = mem::r32(ivars) & ~3u, count = mem::r32(ivars + 4);
        std::uint32_t maxAlignment = 1;
        for (std::uint32_t i = 0; i < count; ++i) {
            const std::uint32_t raw = mem::r32(ivars + 8 + i * size + 12);
            maxAlignment = std::max(maxAlignment, raw == 0xFFFFFFFF ? 4u : 1u << raw);
        }
        diff = (diff + maxAlignment - 1) & ~(maxAlignment - 1);
        for (std::uint32_t i = 0; i < count; ++i)
            if (const auto offset = mem::rp(ivars + 8 + i * size)) mem::w32(offset, mem::r32(offset) + diff);
    }
    mem::w32(ro + 4, start + diff);
    mem::w32(ro + 8, mem::r32(ro + 8) + diff);
    ci->instanceSize += diff;
    hlog("objc", "%s ivars slid by %u", ci->name.c_str(), diff);
}

Class materialize(const std::string &name);
// Creates the guest structs for a host class (and its superclasses first).
Class createHostClass(const ClassDef &def) {
    Class super = 0, superMeta = 0;
    if (!def.superName.empty()) {
        super = materialize(def.superName);
        superMeta = mem::rp(super) & kIsaMask;
    }
    const auto cls = mem::hleAlloc(20, 16), meta = mem::hleAlloc(20, 16);
    const auto ro = mem::hleAlloc(40, 4), metaRo = mem::hleAlloc(40, 4);
    const auto nameStr = mem::hleString(def.name);
    mem::w32(ro, def.superName.empty() ? RO_ROOT : 0);
    mem::w32(ro + 4, def.instanceSize);
    mem::w32(ro + 8, def.instanceSize);
    mem::wp(ro + 16, nameStr);
    mem::w32(metaRo, RO_META | (def.superName.empty() ? RO_ROOT : 0));
    mem::w32(metaRo + 4, 20);
    mem::w32(metaRo + 8, 20);
    mem::wp(metaRo + 16, nameStr);
    const bool root = def.superName.empty();
    if (root) { g_root = cls; g_rootMeta = meta; }
    // isa: class -> meta -> root meta; root meta's superclass is the root class.
    mem::wp(cls, meta);
    mem::wp(cls + 4, super);
    mem::wp(cls + 8, emptyCache());
    mem::wp(cls + 16, ro);
    mem::wp(meta, root ? meta : g_rootMeta);
    mem::wp(meta + 4, root ? cls : superMeta);
    mem::wp(meta + 8, emptyCache());
    mem::wp(meta + 16, metaRo);
    ClassInfo ci, mi;
    ci.cls = cls; ci.name = def.name; ci.host = true; ci.instanceSize = def.instanceSize;
    mi.cls = meta; mi.name = def.name; mi.host = true; mi.meta = true; mi.instanceClass = cls; mi.instanceSize = 20;
    for (const auto &[selector, handler] : def.instanceMethods)
        ci.methods[sel(selector)] = methodStruct(sel(selector), hle::makeStub("-[" + def.name + " " + selector + "]", handler), nullptr);
    for (const auto &[selector, handler] : def.classMethods)
        mi.methods[sel(selector)] = methodStruct(sel(selector), hle::makeStub("+[" + def.name + " " + selector + "]", handler), nullptr);
    g_classes.emplace(cls, std::move(ci));
    g_classes.emplace(meta, std::move(mi));
    g_classByName[def.name] = cls;
    return cls;
}
Class materialize(const std::string &name) {
    if (auto found = g_classByName.find(name); found != g_classByName.end()) return found->second;
    auto def = defs().find(name);
    if (def == defs().end()) {
        // A framework class we have no implementation for: an empty NSObject subclass, so messages to it are logged
        // as unrecognized and return nil instead of crashing.
        hlog("objc", "no implementation for class %s; using an empty NSObject subclass", name.c_str());
        auto &d = defineClass(name, "NSObject");
        return createHostClass(d);
    }
    if (def->second.superName == "\x01") def->second.superName = name == "NSObject" ? "" : "NSObject";
    return createHostClass(def->second);
}

// The receiver's class for a lookup; class objects use their metaclass.
SEL g_selInitialize = 0, g_selForwardingTarget = 0, g_selResolveInstance = 0, g_selResolveClass = 0;

void runInitialize(Class cls) {
    // cls is a non-meta class.
    ClassInfo *ci;
    {
        std::lock_guard lock(g_lock);
        ci = info(cls);
        if (!ci || ci->initialized || ci->initializing) return;
        ci->initializing = true;
    }
    if (const auto super = superclassOf(cls)) runInitialize(super);
    const auto meta = classOf(cls);
    std::uint64_t imp = 0;
    for (Class c = meta; c && !imp; c = superclassOf(c)) {
        std::lock_guard lock(g_lock);
        if (auto *i = info(c)) if (auto m = i->methods.find(g_selInitialize); m != i->methods.end()) imp = mem::rp(m->second + 8);
    }
    if (imp) cpu::call(imp, {cls, g_selInitialize});
    std::lock_guard lock(g_lock);
    ci->initializing = false;
    ci->initialized = true;
}
} // namespace

SEL sel(std::string_view name) {
    std::lock_guard lock(g_lock);
    const std::string key(name);
    if (auto found = g_selectors.find(key); found != g_selectors.end()) return found->second;
    const SEL s = mem::hleString(key);
    g_selectors.emplace(key, s);
    g_selectorNames.emplace(s, key);
    return s;
}
std::string selName(SEL s) {
    std::lock_guard lock(g_lock);
    if (auto found = g_selectorNames.find(s); found != g_selectorNames.end()) return found->second;
    return s ? mem::cstring(s) : "(null)";
}
ClassDef &defineClass(const std::string &name, const std::string &superName, std::uint32_t size) {
    auto &d = defs()[name];
    d.name = name;
    if (superName != "\x01") { d.superName = superName; d.instanceSize = size; }
    else if (d.superName.empty() && name != "NSObject") d.superName = "\x01";
    return d;
}
Class getClass(std::string_view name) {
    std::lock_guard lock(g_lock);
    const std::string key(name);
    if (auto found = g_classByName.find(key); found != g_classByName.end()) return found->second;
    if (defs().count(key)) return materialize(key);
    return 0;
}
Class classOf(id object) {
    if (!object) return 0;
    return mem::rp(object) & kIsaMask;
}
Class superclassOf(Class cls) { return cls ? mem::rp(cls + 4) : 0; }
std::string className(Class cls) {
    std::lock_guard lock(g_lock);
    if (auto *ci = info(cls)) return ci->name;
    return "?";
}
bool isMetaClass(Class cls) {
    std::lock_guard lock(g_lock);
    auto *ci = info(cls);
    return ci && ci->meta;
}
bool isSubclass(Class cls, Class ancestor) {
    for (Class c = cls; c; c = superclassOf(c)) if (c == ancestor) return true;
    return false;
}
bool isKindOf(id object, std::string_view name) {
    const auto target = getClass(name);
    return target && isSubclass(classOf(object), target);
}
std::uint32_t instanceSize(Class cls) {
    std::lock_guard lock(g_lock);
    auto *ci = info(cls);
    return ci ? ci->instanceSize : 4;
}

std::uint64_t lookup(Class cls, SEL s) {
    if (!cls) return 0;
    {
        std::lock_guard lock(g_lock);
        if (auto found = g_cache.find({cls, s}); found != g_cache.end()) return found->second;
    }
    // +initialize on first use of a class (instance or class side).
    {
        ClassInfo *ci;
        Class target;
        {
            std::lock_guard lock(g_lock);
            ci = info(cls);
            target = ci && ci->meta ? ci->instanceClass : cls;
            if (ci && ci->meta && !target) {
                // Metaclass of an app class: its instance class is the class whose isa is this metaclass.
                for (auto &[c, i] : g_classes) if (!i.meta && (mem::rp(c) & kIsaMask) == cls) { ci->instanceClass = c; break; }
                target = ci->instanceClass;
            }
        }
        if (target) {
            std::lock_guard lock(g_lock);
            auto *ti = info(target);
            if (ti && !ti->initialized && !ti->initializing) {
                // Leave the lock before running guest code.
                g_lock.unlock();
                runInitialize(target);
                g_lock.lock();
            }
        }
    }
    std::lock_guard lock(g_lock);
    for (Class c = cls; c; c = superclassOf(c)) {
        auto *ci = info(c);
        if (!ci) {
            warn("lookup through unregistered class %llx", (unsigned long long)c);
            break;
        }
        if (auto m = ci->methods.find(s); m != ci->methods.end()) {
            const auto imp = mem::rp(m->second + 8);
            g_cache[{cls, s}] = imp;
            return imp;
        }
    }
    return 0;
}
bool respondsTo(id object, SEL s) { return lookup(classOf(object), s) != 0; }
void addMethod(Class cls, SEL s, std::uint64_t imp, const char *types) {
    std::lock_guard lock(g_lock);
    if (auto *ci = info(cls)) {
        ci->methods[s] = methodStruct(s, imp, types);
        g_cache.clear();
    }
}
std::uint64_t methodImplementation(Class cls, SEL s) { return lookup(cls, s); }
bool conformsTo(Class cls, std::string_view protocol) {
    std::lock_guard lock(g_lock);
    for (Class c = cls; c; c = superclassOf(c))
        if (auto *ci = info(c))
            for (const auto &p : ci->protocols) if (p == protocol) return true;
    return false;
}

void logUnrecognized(id receiver, SEL s) {
    static std::mutex lock;
    static std::set<std::pair<std::string, std::string>> seen;
    const auto cls = classOf(receiver);
    const bool meta = isMetaClass(cls);
    const auto name = className(cls), selector = selName(s);
    std::lock_guard guard(lock);
    if (seen.insert({name, selector}).second)
        hlog("objc", "unrecognized selector %c[%s %s] (returning nil) from %s", meta ? '+' : '-', name.c_str(), selector.c_str(),
             loader::describe(cpu::lr()).c_str());
}

id send(id receiver, std::string_view selector, std::initializer_list<std::uint64_t> args) {
    if (!receiver) return 0;
    const SEL s = sel(selector);
    const auto imp = lookup(classOf(receiver), s);
    if (!imp) {
        logUnrecognized(receiver, s);
        return 0;
    }
    std::vector<std::uint32_t> all{std::uint32_t(receiver), std::uint32_t(s)};
    for (auto a : args) all.push_back(std::uint32_t(a));
    return cpu::callWords(imp, all);
}
double sendDouble(id receiver, std::string_view selector, std::initializer_list<std::uint64_t> args) {
    if (!receiver) return 0;
    const std::uint64_t low = std::uint32_t(send(receiver, selector, args));
    const std::uint64_t bits = low | (std::uint64_t(cpu::lastHigh()) << 32);
    double value;
    std::memcpy(&value, &bits, 8);
    return value;
}
float sendFloat(id receiver, std::string_view selector, std::initializer_list<std::uint64_t> args) {
    return receiver ? cpu::bitsFloat(send(receiver, selector, args)) : 0.0f;
}
void sendStret(std::uint64_t result, id receiver, std::string_view selector, std::initializer_list<std::uint64_t> args) {
    if (!receiver) return;
    const SEL s = sel(selector);
    const auto imp = lookup(classOf(receiver), s);
    if (!imp) { logUnrecognized(receiver, s); return; }
    std::vector<std::uint32_t> all{std::uint32_t(result), std::uint32_t(receiver), std::uint32_t(s)};
    for (auto a : args) all.push_back(std::uint32_t(a));
    cpu::callWords(imp, all);
}
id sendSuper(id receiver, Class startClass, std::string_view selector, std::initializer_list<std::uint64_t> args) {
    if (!receiver) return 0;
    const SEL s = sel(selector);
    const auto imp = lookup(startClass, s);
    if (!imp) { logUnrecognized(receiver, s); return 0; }
    std::vector<std::uint32_t> all{std::uint32_t(receiver), std::uint32_t(s)};
    for (auto a : args) all.push_back(std::uint32_t(a));
    return cpu::callWords(imp, all);
}

// --- Objects -----------------------------------------------------------------------------------------------------------
id allocInstance(Class cls, std::size_t extra) {
    const auto size = std::max<std::size_t>(instanceSize(cls), 4) + extra;
    const auto object = mem::calloc(size);
    mem::wp(object, cls);
    static const char *watch = std::getenv("HLE_WATCH"); // Class:offset
    if (watch) {
        const std::string spec = watch;
        const auto colon = spec.find(':');
        if (className(cls) == spec.substr(0, colon)) hle::watch(std::uint32_t(object + std::stoul(spec.substr(colon + 1))));
    }
    return object;
}
void setHostObject(id object, std::unique_ptr<HostObject> data) {
    std::lock_guard lock(g_hostLock);
    if (data) g_hostObjects[object] = std::move(data);
    else g_hostObjects.erase(object);
}
HostObject *hostObject(id object) {
    std::lock_guard lock(g_hostLock);
    auto found = g_hostObjects.find(object);
    return found == g_hostObjects.end() ? nullptr : found->second.get();
}
static bool isHleConstant(id object) { return object >= mem::kHleBase && object < mem::kHleBase + mem::kHleSize; }
static bool isImageConstant(id object) { return object >= loader::imageBase() && object < loader::imageEnd(); }
void retain(id object) {
    if (!object || isHleConstant(object) || isImageConstant(object)) return;
    std::lock_guard lock(g_refLock);
    ++g_refCounts[object];
}
std::uint64_t retainCount(id object) {
    if (!object) return 0;
    if (isHleConstant(object) || isImageConstant(object)) return ~0ull >> 1;
    std::lock_guard lock(g_refLock);
    auto found = g_refCounts.find(object);
    return 1 + (found == g_refCounts.end() ? 0 : std::uint64_t(found->second));
}
void release(id object) {
    if (!object || isHleConstant(object) || isImageConstant(object)) return;
    bool dealloc = false;
    {
        std::lock_guard lock(g_refLock);
        auto &count = g_refCounts[object];
        if (count > 0) --count;
        else {
            g_refCounts.erase(object);
            dealloc = g_deallocated.insert(object).second;
        }
    }
    if (dealloc) send(object, "dealloc");
}

// --- Autorelease pools ------------------------------------------------------------------------------------------------
namespace {
thread_local std::vector<id> t_pool;
}
id autorelease(id object) {
    if (object) t_pool.push_back(object);
    return object;
}
std::uint64_t poolPush() { return t_pool.size() + 1; }
void poolPop(std::uint64_t token) {
    const std::size_t mark = token ? std::size_t(token - 1) : 0;
    while (t_pool.size() > mark) {
        const auto object = t_pool.back();
        t_pool.pop_back();
        release(object);
    }
}

// --- Image loading ----------------------------------------------------------------------------------------------------
void loadImage() {
    g_selInitialize = sel("initialize");
    g_selForwardingTarget = sel("forwardingTargetForSelector:");
    g_selResolveInstance = sel("resolveInstanceMethod:");
    g_selResolveClass = sel("resolveClassMethod:");
    std::lock_guard lock(g_lock);
    materialize("NSObject");
    // Selector references: point each at the unique selector.
    if (auto *s = loader::section("__DATA", "__objc_selrefs"))
        for (std::uint64_t a = s->address; a < s->address + s->size; a += 4) mem::wp(a, sel(mem::cstring(mem::rp(a))));
    std::size_t classes = 0;
    if (auto *s = loader::section("__DATA", "__objc_classlist"))
        for (std::uint64_t a = s->address; a < s->address + s->size; a += 4) {
            const Class cls = mem::rp(a), meta = mem::rp(cls) & kIsaMask;
            registerGuestClass(cls, false);
            registerGuestClass(meta, true);
            info(meta)->instanceClass = cls;
            g_classByName[info(cls)->name] = cls;
            ++classes;
        }
    {
        std::set<Class> done;
        if (auto *s = loader::section("__DATA", "__objc_classlist"))
            for (std::uint64_t a = s->address; a < s->address + s->size; a += 4) slideIvars(mem::rp(a), done);
    }
    std::size_t categories = 0;
    if (auto *s = loader::section("__DATA", "__objc_catlist"))
        for (std::uint64_t a = s->address; a < s->address + s->size; a += 4) {
            const auto cat = mem::rp(a);
            const Class cls = mem::rp(cat + 4);
            auto *ci = info(cls);
            if (!ci) { warn("category %s on unknown class", mem::cstring(mem::rp(cat)).c_str()); continue; }
            addMethodList(*ci, mem::rp(cat + 8));
            if (auto *mi = info(mem::rp(cls) & kIsaMask)) addMethodList(*mi, mem::rp(cat + 12));
            addProtocols(*ci, mem::rp(cat + 16));
            hlog("objc", "category %s on %s", mem::cstring(mem::rp(cat)).c_str(), ci->name.c_str());
            ++categories;
        }
    g_cache.clear();
    hlog("objc", "%zu app classes, %zu categories, %zu selectors", classes, categories, g_selectors.size());
    // +load: classes in __objc_nlclslist, then categories in __objc_nlcatlist.
    std::vector<Class> loads;
    if (auto *s = loader::section("__DATA", "__objc_nlclslist"))
        for (std::uint64_t a = s->address; a < s->address + s->size; a += 4) loads.push_back(mem::rp(a));
    for (const Class cls : loads) {
        auto *mi = info(mem::rp(cls) & kIsaMask);
        if (auto m = mi->methods.find(sel("load")); m != mi->methods.end()) {
            hlog("objc", "+[%s load]", info(cls)->name.c_str());
            g_lock.unlock();
            cpu::call(mem::rp(m->second + 8), {cls, sel("load")});
            g_lock.lock();
        }
    }
}

bool findIvar(Class cls, std::string_view name, std::uint32_t &offset, std::string &type) {
    std::lock_guard lock(g_lock);
    for (Class c = cls; c; c = mem::rp(c + 4)) {
        const auto *ci = info(c);
        if (!ci || ci->host) continue;
        const auto ivars = mem::rp((mem::rp(c + 16) & kDataMask) + 28);
        if (!ivars) continue;
        const std::uint32_t size = mem::r32(ivars) & ~3u, count = mem::r32(ivars + 4);
        for (std::uint32_t i = 0; i < count; ++i) {
            const auto ivar = ivars + 8 + i * size;
            if (mem::cstring(mem::rp(ivar + 4)) != name || !mem::rp(ivar)) continue;
            offset = mem::r32(mem::rp(ivar));
            type = mem::cstring(mem::rp(ivar + 8));
            return true;
        }
    }
    return false;
}

// --- Serialized classes -----------------------------------------------------------------------------------------------
// Every instance method of the class runs under one recursive lock shared by all serialized classes. The entry stub
// takes the lock and jumps to the original IMP with LR pointing at an exit stub, which releases it and returns to the
// real caller, so arguments (registers and stack) and results pass through untouched.
namespace {
std::recursive_mutex g_serialLock;
std::unordered_map<std::string, std::uint64_t> g_serialImps; // entry stub name -> original IMP
std::uint64_t g_serialExit = 0;
thread_local std::vector<std::uint64_t> t_serialReturns;
void serialEnter() {
    const auto &name = hle::currentFunction();
    std::uint64_t imp;
    {
        std::lock_guard lock(g_lock);
        imp = g_serialImps.at(name);
    }
    g_serialLock.lock();
    t_serialReturns.push_back(cpu::lr());
    cpu::setX(14, g_serialExit);
    cpu::setPc(imp);
}
void serialExit() {
    const auto caller = t_serialReturns.back();
    t_serialReturns.pop_back();
    g_serialLock.unlock();
    cpu::setPc(caller);
}
} // namespace
void serializeClass(std::string_view name) {
    std::lock_guard lock(g_lock);
    auto found = g_classByName.find(std::string(name));
    if (found == g_classByName.end()) { warn("serializeClass: no class %s", std::string(name).c_str()); return; }
    if (!g_serialExit) g_serialExit = hle::makeStub("serialized method exit", serialExit);
    auto *ci = info(found->second);
    for (const auto &[s, m] : ci->methods) {
        const auto stubName = "-[" + ci->name + " " + mem::cstring(s) + "] (serialized)";
        g_serialImps[stubName] = mem::rp(m + 8);
        mem::wp(m + 8, hle::makeStub(stubName, serialEnter));
    }
    g_cache.clear();
    hlog("objc", "%s methods serialized", ci->name.c_str());
}

// --- libobjc exports --------------------------------------------------------------------------------------------------
namespace {
void returnNil() {
    cpu::setX(0, 0);
    cpu::setX(1, 0);
}
// Unimplemented framework classes behave like simple models: -setFoo: stores x2, -foo returns it.
std::mutex g_bagLock;
std::map<std::pair<id, std::string>, std::uint64_t> g_bag;
bool propertyFallback(id receiver, SEL s) {
    const auto name = selName(s);
    const auto colons = std::count(name.begin(), name.end(), ':');
    if (colons == 1 && name.size() > 4 && name.rfind("set", 0) == 0 && std::isupper(static_cast<unsigned char>(name[3])) && name.back() == ':') {
        std::string key = name.substr(3, name.size() - 4);
        key[0] = char(std::tolower(static_cast<unsigned char>(key[0])));
        std::lock_guard lock(g_bagLock);
        g_bag[{receiver, key}] = cpu::x(2);
        return true;
    }
    if (colons == 0) {
        std::lock_guard lock(g_bagLock);
        auto found = g_bag.find({receiver, name});
        if (found != g_bag.end()) { cpu::setX(0, found->second); return true; }
    }
    return false;
}
} // namespace
} // namespace objc
objc::id forwardingInvocation(objc::id signature, objc::id target, objc::SEL selector, int firstWord); // Invocation.cpp
void forwardedResult(objc::id invocation, std::uint32_t &r0, std::uint32_t &r1);
namespace objc {
namespace {
// -forwardInvocation: for a message the class has no method for (when it overrides it): the arguments go into an
// NSInvocation built from -methodSignatureForSelector:, and its return value becomes the message's result.
bool forwardMessage(id receiver, SEL s, int firstWord) {
    const auto forward = lookup(classOf(receiver), sel("forwardInvocation:"));
    if (!forward) return false;
    const auto signature = send(receiver, "methodSignatureForSelector:", {s});
    if (!signature) return false;
    const auto invocation = forwardingInvocation(signature, receiver, s, firstWord);
    if (!invocation) return false;
    cpu::call(forward, {receiver, sel("forwardInvocation:"), invocation});
    std::uint32_t r0, r1;
    forwardedResult(invocation, r0, r1);
    release(invocation);
    cpu::setX(0, r0);
    cpu::setX(1, r1);
    return true;
}
// objc_msgSend: looks up the method and tail-calls it with the registers untouched.
void msgSend() {
    const id receiver = cpu::x(0);
    const SEL s = cpu::x(1);
    if (!receiver) return returnNil();
    const Class cls = classOf(receiver);
    auto imp = lookup(cls, s);
    if (!imp) {
        // forwardingTargetForSelector:
        if (const auto fwd = lookup(cls, g_selForwardingTarget)) {
            const auto target = cpu::call(fwd, {receiver, g_selForwardingTarget, s});
            if (target && target != receiver) {
                cpu::setX(0, target);
                if ((imp = lookup(classOf(target), s))) return cpu::setPc(imp);
            }
        }
        if (forwardMessage(receiver, s, 2)) return;
        logUnrecognized(receiver, s);
        returnNil();
        if (propertyFallback(receiver, s)) return;
        return;
    }
    if (logEnabled("msg")) {
        static const bool on = std::getenv("HLE_TRACE_MSG") != nullptr;
        // HLE_TRACE_SEL=a,b: only selectors containing one of the parts, with their argument words (and as floats).
        static const std::vector<std::string> filter = [] {
            std::vector<std::string> parts;
            if (const char *f = std::getenv("HLE_TRACE_SEL")) {
                std::string list = f;
                for (std::size_t start = 0; start <= list.size();) {
                    auto end = list.find(',', start);
                    if (end == std::string::npos) end = list.size();
                    if (end > start) parts.push_back(list.substr(start, end - start));
                    start = end + 1;
                }
            }
            return parts;
        }();
        if (!filter.empty()) {
            const auto name = selName(s);
            for (const auto &part : filter)
                if (name.find(part) != std::string::npos) {
                    // Run it nested (register arguments only) to log the result too, then return to the caller.
                    const auto r2 = cpu::x(2), r3 = cpu::x(3), caller = cpu::lr();
                    const auto result = cpu::call(imp, {receiver, s, r2, r3});
                    const auto high = cpu::lastHigh();
                    hlog("msg", "%c[%s %s] self=%08x r2=%08x (%g) r3=%08x (%g) -> %08x (%g) from %s", isMetaClass(cls) ? '+' : '-', className(cls).c_str(),
                         name.c_str(), unsigned(receiver), unsigned(r2), cpu::bitsFloat(r2), unsigned(r3), cpu::bitsFloat(r3), unsigned(result),
                         cpu::bitsFloat(result), loader::describe(caller).c_str());
                    cpu::setX(0, result);
                    cpu::setX(1, high);
                    cpu::setPc(caller);
                    return;
                }
        } else if (on) hlog("msg", "%c[%s %s]", isMetaClass(cls) ? '+' : '-', className(cls).c_str(), selName(s).c_str());
    }
    cpu::setPc(imp);
}
// objc_msgSendSuper2(struct objc_super *{receiver, current class}, SEL, ...): starts at the current class's superclass.
void msgSendSuper2() {
    const auto super = cpu::x(0);
    const id receiver = mem::rp(super);
    const Class current = mem::rp(super + 4);
    const SEL s = cpu::x(1);
    cpu::setX(0, receiver);
    if (!receiver) return returnNil();
    const auto imp = lookup(superclassOf(current), s);
    if (!imp) { logUnrecognized(receiver, s); return returnNil(); }
    cpu::setPc(imp);
}
// objc_msgSend_stret(result, self, SEL, ...): structures larger than 4 bytes come back through r0.
void msgSendStret() {
    const id receiver = cpu::x(1);
    const SEL s = cpu::x(2);
    if (!receiver) return;
    auto imp = lookup(classOf(receiver), s);
    if (logEnabled("msg")) {
        static const char *filter = std::getenv("HLE_TRACE_SEL");
        if (filter && imp && std::strstr(filter, selName(s).c_str())) {
            // Run it nested to log the result (methods without stack arguments only), then return to the caller.
            const auto out = cpu::x(0), caller = cpu::lr();
            cpu::call(imp, {out, receiver, s, cpu::x(3)});
            hlog("msg", "stret -[%s %s] self=%08x -> {%g, %g} from %s", className(classOf(receiver)).c_str(), selName(s).c_str(), unsigned(receiver),
                 mem::rf(out), mem::rf(out + 4), loader::describe(caller).c_str());
            cpu::setPc(caller);
            return;
        }
    }
    if (!imp) { logUnrecognized(receiver, s); return; }
    cpu::setPc(imp);
}
// objc_msgSendSuper2_stret(result, struct objc_super *, SEL, ...)
void msgSendSuper2Stret() {
    const auto super = cpu::x(1);
    const id receiver = mem::rp(super);
    const Class current = mem::rp(super + 4);
    cpu::setX(1, receiver);
    if (!receiver) return;
    const auto imp = lookup(superclassOf(current), cpu::x(2));
    if (!imp) { logUnrecognized(receiver, cpu::x(2)); return; }
    cpu::setPc(imp);
}
hle::Registrar r_msgSendStret("_objc_msgSend_stret", msgSendStret);
hle::Registrar r_msgSendSuper2Stret("_objc_msgSendSuper2_stret", msgSendSuper2Stret);
hle::Registrar r_msgSend("_objc_msgSend", msgSend);
hle::Registrar r_msgSendSuper2("_objc_msgSendSuper2", msgSendSuper2);
hle::Registrar r_msgSendSuper("_objc_msgSendSuper", [] {
    const auto super = cpu::x(0);
    const id receiver = mem::rp(super);
    const Class start = mem::rp(super + 4);
    cpu::setX(0, receiver);
    if (!receiver) return returnNil();
    const auto imp = lookup(start, cpu::x(1));
    if (!imp) { logUnrecognized(receiver, cpu::x(1)); return returnNil(); }
    cpu::setPc(imp);
});
hle::DataRegistrar d_emptyCache("__objc_empty_cache", [] { return emptyCache(); });
const bool g_classImports = [] {
    hle::registerDataPrefix("_OBJC_CLASS_$_", [](const std::string &name) {
        std::lock_guard lock(g_lock);
        return materialize(name.substr(14));
    });
    hle::registerDataPrefix("_OBJC_METACLASS_$_", [](const std::string &name) {
        std::lock_guard lock(g_lock);
        return mem::rp(materialize(name.substr(18))) & kIsaMask;
    });
    hle::registerDataPrefix("_OBJC_EHTYPE_$_", [](const std::string &name) {
        // objc_ehtype_t {vtable, name, cls}
        const auto e = mem::hleAlloc(12, 4);
        mem::wp(e + 4, mem::hleString(name.substr(15)));
        std::lock_guard lock(g_lock);
        mem::wp(e + 8, materialize(name.substr(15)));
        return e;
    });
    return true;
}();
} // namespace

HLE(objc_retain) { retain(hle::arg(0)); }
HLE(objc_release) { release(hle::arg(0)); }
HLE(objc_autorelease) { autorelease(hle::arg(0)); }
HLE(objc_autoreleaseReturnValue) { autorelease(hle::arg(0)); }
HLE(objc_retainAutorelease) { retain(hle::arg(0)); autorelease(hle::arg(0)); }
HLE(objc_retainAutoreleaseReturnValue) { retain(hle::arg(0)); autorelease(hle::arg(0)); }
HLE(objc_retainAutoreleasedReturnValue) { retain(hle::arg(0)); }
HLE(objc_autoreleasePoolPush) { hle::ret(poolPush()); }
HLE(objc_autoreleasePoolPop) { poolPop(hle::arg(0)); }
HLE(objc_storeStrong) {
    const auto location = hle::arg(0), object = hle::arg(1);
    const auto old = mem::rp(location);
    if (old == object) return;
    retain(object);
    mem::wp(location, object);
    release(old);
}
// Weak references (objects are never freed, so they never need zeroing).
HLE(objc_storeWeak) { mem::wp(hle::arg(0), hle::arg(1)); }
HLE(objc_initWeak) { mem::wp(hle::arg(0), hle::arg(1)); }
HLE(objc_destroyWeak) { mem::wp(hle::arg(0), 0); }
HLE(objc_loadWeakRetained) { const auto o = mem::rp(hle::arg(0)); retain(o); hle::ret(o); }
HLE(objc_loadWeak) { hle::ret(mem::rp(hle::arg(0))); }
HLE(objc_copyWeak) { mem::wp(hle::arg(0), mem::rp(hle::arg(1))); }
HLE(objc_moveWeak) { mem::wp(hle::arg(0), mem::rp(hle::arg(1))); mem::wp(hle::arg(1), 0); }
// Properties: objc_getProperty(self, _cmd, offset, atomic), objc_setProperty(self, _cmd, offset, value, atomic, copy).
HLE(objc_getProperty) { hle::ret(mem::rp(hle::arg(0) + hle::arg(2))); }
static void setProperty(id self, std::uint64_t offset, id value, bool copy, bool mutableCopy) {
    if (copy) value = send(value, "copyWithZone:", {0});
    else if (mutableCopy) value = send(value, "mutableCopyWithZone:", {0});
    else retain(value);
    const auto old = mem::rp(self + offset);
    mem::wp(self + offset, value);
    release(old);
}
HLE(objc_setProperty) { setProperty(hle::arg(0), hle::arg(2), hle::arg(3), hle::arg(5) != 0 && (hle::arg(5) & 0xFF) != 2, (hle::arg(5) & 0xFF) == 2); }
HLE(objc_setProperty_atomic) { setProperty(hle::arg(0), hle::arg(3), hle::arg(2), false, false); }
HLE(objc_setProperty_nonatomic) { setProperty(hle::arg(0), hle::arg(3), hle::arg(2), false, false); }
HLE(objc_setProperty_atomic_copy) { setProperty(hle::arg(0), hle::arg(3), hle::arg(2), true, false); }
HLE(objc_setProperty_nonatomic_copy) { setProperty(hle::arg(0), hle::arg(3), hle::arg(2), true, false); }
HLE(objc_copyStruct) { std::memmove(mem::base() + hle::arg(0), mem::base() + hle::arg(1), std::size_t(hle::arg(2))); }
// @synchronized
namespace {
std::mutex g_syncLock;
std::unordered_map<id, std::unique_ptr<std::recursive_mutex>> g_syncs;
std::recursive_mutex &syncFor(id object) {
    std::lock_guard lock(g_syncLock);
    auto &m = g_syncs[object];
    if (!m) m = std::make_unique<std::recursive_mutex>();
    return *m;
}
} // namespace
HLE(objc_sync_enter) { if (hle::arg(0)) syncFor(hle::arg(0)).lock(); hle::ret(0); }
HLE(objc_sync_exit) { if (hle::arg(0)) syncFor(hle::arg(0)).unlock(); hle::ret(0); }
// Associated objects.
namespace {
std::mutex g_assocLock;
std::map<std::pair<id, std::uint64_t>, id> g_assoc;
} // namespace
HLE(objc_setAssociatedObject) {
    std::lock_guard lock(g_assocLock);
    retain(hle::arg(2));
    g_assoc[{hle::arg(0), hle::arg(1)}] = hle::arg(2);
}
HLE(objc_getAssociatedObject) {
    std::lock_guard lock(g_assocLock);
    auto found = g_assoc.find({hle::arg(0), hle::arg(1)});
    hle::ret(found == g_assoc.end() ? 0 : found->second);
}
HLE(objc_enumerationMutation) { warn("collection mutated while being enumerated (%llx)", (unsigned long long)hle::arg(0)); }
HLE(objc_terminate) { cpu::backtrace("objc_terminate"); fatal("objc_terminate"); }
HLE(objc_exception_throw) {
    const id e = hle::arg(0);
    const auto name = send(e, "name"), reason = send(e, "reason");
    const auto text = [](id s) { return s ? mem::cstring(send(s, "UTF8String")) : std::string("(nil)"); };
    cpu::backtrace("Objective-C exception");
    fatal("uncaught Objective-C exception %s: %s", text(name).c_str(), text(reason).c_str());
}
HLE(objc_exception_rethrow) { cpu::backtrace("objc_exception_rethrow"); fatal("objc_exception_rethrow"); }
HLE(objc_begin_catch) { hle::ret(hle::arg(0)); }
HLE(objc_end_catch) {}
HLE(objc_getClass) { hle::ret(getClass(mem::cstring(hle::arg(0)))); }
HLE(objc_lookUpClass) { hle::ret(getClass(mem::cstring(hle::arg(0)))); }
HLE(object_getClass) { hle::ret(classOf(hle::arg(0))); }
HLE(class_getName) { hle::ret(hle::arg(0) ? mem::rp((mem::rp(hle::arg(0) + 16) & kDataMask) + 16) : 0); }
HLE(class_getInstanceSize) { hle::ret(instanceSize(hle::arg(0))); }
HLE(class_getSuperclass) { hle::ret(superclassOf(hle::arg(0))); }
HLE(sel_registerName) { hle::ret(sel(mem::cstring(hle::arg(0)))); }
HLE(sel_getName) { hle::ret(hle::arg(0)); }
HLE(class_addMethod) {
    const auto cls = hle::arg(0), s = hle::arg(1);
    {
        std::lock_guard lock(g_lock);
        auto *ci = info(cls);
        if (!ci || ci->methods.count(s)) return hle::ret(0);
    }
    addMethod(cls, s, hle::arg(2), mem::cstring(hle::arg(3)).c_str());
    hle::ret(1);
}
// Method handles are the method_t structs.
HLE(class_getInstanceMethod) {
    std::lock_guard lock(g_lock);
    for (Class c = hle::arg(0); c; c = superclassOf(c))
        if (auto *ci = info(c))
            if (auto m = ci->methods.find(hle::arg(1)); m != ci->methods.end()) return hle::ret(m->second);
    hle::ret(0);
}
HLE(class_getClassMethod) {
    std::lock_guard lock(g_lock);
    for (Class c = classOf(hle::arg(0)); c; c = superclassOf(c))
        if (auto *ci = info(c))
            if (auto m = ci->methods.find(hle::arg(1)); m != ci->methods.end()) return hle::ret(m->second);
    hle::ret(0);
}
HLE(class_getMethodImplementation) {
    auto imp = lookup(hle::arg(0), hle::arg(1));
    if (!imp) imp = hle::resolveImport("__objc_msgForward");
    hle::ret(imp);
}
HLE(class_respondsToSelector) { hle::ret(lookup(hle::arg(0), hle::arg(1)) != 0); }
HLE(method_getImplementation) { hle::ret(hle::arg(0) ? mem::rp(hle::arg(0) + 8) : 0); }
HLE(method_setImplementation) {
    const auto old = mem::rp(hle::arg(0) + 8);
    mem::wp(hle::arg(0) + 8, hle::arg(1));
    std::lock_guard lock(g_lock);
    g_cache.clear();
    hle::ret(old);
}
HLE(method_exchangeImplementations) {
    const auto a = hle::arg(0), b = hle::arg(1);
    const auto ia = mem::rp(a + 8), ib = mem::rp(b + 8);
    mem::wp(a + 8, ib);
    mem::wp(b + 8, ia);
    std::lock_guard lock(g_lock);
    g_cache.clear();
}
HLE(method_getName) { hle::ret(hle::arg(0) ? mem::rp(hle::arg(0)) : 0); }
HLE(method_getTypeEncoding) { hle::ret(hle::arg(0) ? mem::rp(hle::arg(0) + 4) : 0); }
HLE(protocol_conformsToProtocol) {
    const auto a = hle::arg(0), b = hle::arg(1);
    if (!a || !b) return hle::ret(0);
    hle::ret(mem::cstring(mem::rp(a + 4)) == mem::cstring(mem::rp(b + 4)));
}
HLE(protocol_copyMethodDescriptionList) { if (hle::arg(3)) mem::w32(hle::arg(3), 0); hle::ret(0); }
HLE(protocol_copyProtocolList) { if (hle::arg(1)) mem::w32(hle::arg(1), 0); hle::ret(0); }
HLE(_objc_msgForward) {
    logUnrecognized(cpu::x(0), cpu::x(1));
    returnNil();
}

// --- NSObject ---------------------------------------------------------------------------------------------------------
OBJC_ROOT_CLASS(NSObject);
OBJC_CLASS_METHOD(NSObject, "alloc", alloc) { hle::ret(allocInstance(self())); }
OBJC_CLASS_METHOD(NSObject, "allocWithZone:", allocWithZone) { hle::ret(allocInstance(self())); }
OBJC_CLASS_METHOD(NSObject, "new", new_) { hle::ret(send(allocInstance(self()), "init")); }
OBJC_CLASS_METHOD(NSObject, "class", cls) { hle::ret(self()); }
OBJC_CLASS_METHOD(NSObject, "superclass", superclass) { hle::ret(superclassOf(self())); }
OBJC_CLASS_METHOD(NSObject, "initialize", initialize) {}
OBJC_CLASS_METHOD(NSObject, "load", load) {}
OBJC_CLASS_METHOD(NSObject, "isSubclassOfClass:", isSubclassOfClass) { hle::ret(isSubclass(self(), marg(0))); }
OBJC_CLASS_METHOD(NSObject, "instancesRespondToSelector:", instancesRespond) { hle::ret(lookup(self(), marg(0)) != 0); }
OBJC_CLASS_METHOD(NSObject, "respondsToSelector:", classResponds) { hle::ret(lookup(classOf(self()), marg(0)) != 0); }
OBJC_CLASS_METHOD(NSObject, "conformsToProtocol:", classConforms) {
    hle::ret(marg(0) && conformsTo(self(), mem::cstring(mem::rp(marg(0) + 4))));
}
OBJC_CLASS_METHOD(NSObject, "instanceMethodForSelector:", instanceMethodFor) { hle::ret(lookup(self(), marg(0))); }
OBJC_CLASS_METHOD(NSObject, "resolveInstanceMethod:", resolveInstance) { hle::ret(0); }
OBJC_CLASS_METHOD(NSObject, "resolveClassMethod:", resolveClass) { hle::ret(0); }
OBJC_CLASS_METHOD(NSObject, "retain", classRetain) { hle::ret(self()); }
OBJC_CLASS_METHOD(NSObject, "release", classRelease) {}
OBJC_CLASS_METHOD(NSObject, "autorelease", classAutorelease) { hle::ret(self()); }
OBJC_CLASS_METHOD(NSObject, "self", classSelf) { hle::ret(self()); }
OBJC_CLASS_METHOD(NSObject, "isKindOfClass:", classIsKind) { hle::ret(isSubclass(classOf(self()), marg(0))); }
OBJC_CLASS_METHOD(NSObject, "description", classDescription) { hle::ret(send(getClass("NSString"), "stringWithUTF8String:", {mem::heapString(className(self()))})); }
OBJC_CLASS_METHOD(NSObject, "hash", classHash) { hle::ret(self()); }
OBJC_CLASS_METHOD(NSObject, "isEqual:", classIsEqual) { hle::ret(self() == marg(0)); }
OBJC_METHOD(NSObject, "init", init) { hle::ret(self()); }
OBJC_METHOD(NSObject, "dealloc", dealloc) {
    // Memory is not reused (stale references in emulated code then stay harmless); host state is dropped.
    setHostObject(self(), nullptr);
}
OBJC_METHOD(NSObject, "finalize", finalize) {}
OBJC_METHOD(NSObject, "retain", retain_) { retain(self()); hle::ret(self()); }
OBJC_METHOD(NSObject, "release", release_) { release(self()); }
OBJC_METHOD(NSObject, "autorelease", autorelease_) { autorelease(self()); hle::ret(self()); }
OBJC_METHOD(NSObject, "retainCount", retainCount_) { hle::ret(retainCount(self())); }
OBJC_METHOD(NSObject, "class", cls) { hle::ret(classOf(self())); }
OBJC_METHOD(NSObject, "superclass", superclass) { hle::ret(superclassOf(classOf(self()))); }
OBJC_METHOD(NSObject, "self", self_) { hle::ret(self()); }
OBJC_METHOD(NSObject, "zone", zone) { hle::ret(0); }
OBJC_METHOD(NSObject, "hash", hash) { hle::ret(self()); }
OBJC_METHOD(NSObject, "isEqual:", isEqual) { hle::ret(self() == marg(0)); }
OBJC_METHOD(NSObject, "isKindOfClass:", isKindOfClass) { hle::ret(isSubclass(classOf(self()), marg(0))); }
OBJC_METHOD(NSObject, "isMemberOfClass:", isMemberOfClass) { hle::ret(classOf(self()) == marg(0)); }
OBJC_METHOD(NSObject, "respondsToSelector:", respondsToSelector) { hle::ret(lookup(classOf(self()), marg(0)) != 0); }
OBJC_METHOD(NSObject, "conformsToProtocol:", conformsToProtocol) {
    hle::ret(marg(0) && conformsTo(classOf(self()), mem::cstring(mem::rp(marg(0) + 4))));
}
OBJC_METHOD(NSObject, "methodForSelector:", methodForSelector) { hle::ret(lookup(classOf(self()), marg(0))); }
OBJC_METHOD(NSObject, "isProxy", isProxy) { hle::ret(0); }
OBJC_METHOD(NSObject, "copy", copy) { hle::ret(send(self(), "copyWithZone:", {0})); }
OBJC_METHOD(NSObject, "mutableCopy", mutableCopy) { hle::ret(send(self(), "mutableCopyWithZone:", {0})); }
OBJC_METHOD(NSObject, "performSelector:", performSelector) { hle::ret(send(self(), selName(marg(0)))); }
OBJC_METHOD(NSObject, "performSelector:withObject:", performSelector1) { hle::ret(send(self(), selName(marg(0)), {marg(1)})); }
OBJC_METHOD(NSObject, "performSelector:withObject:withObject:", performSelector2) {
    hle::ret(send(self(), selName(marg(0)), {marg(1), marg(2)}));
}
OBJC_METHOD(NSObject, "description", description) {
    char text[128];
    std::snprintf(text, sizeof text, "<%s: 0x%llx>", className(classOf(self())).c_str(), (unsigned long long)self());
    hle::ret(send(getClass("NSString"), "stringWithUTF8String:", {mem::heapString(text)}));
}
OBJC_METHOD(NSObject, "debugDescription", debugDescription) { hle::ret(send(self(), "description")); }
OBJC_METHOD(NSObject, "forwardingTargetForSelector:", forwardingTarget) { hle::ret(0); }
OBJC_METHOD(NSObject, "awakeFromNib", awakeFromNib) {}
OBJC_METHOD(NSObject, "willChangeValueForKey:", willChange) {}
OBJC_METHOD(NSObject, "didChangeValueForKey:", didChange) {}
OBJC_METHOD(NSObject, "addObserver:forKeyPath:options:context:", addObserver) {}
OBJC_METHOD(NSObject, "removeObserver:forKeyPath:", removeObserver) {}

// NSAutoreleasePool
OBJC_METHOD(NSAutoreleasePool, "init", init) {
    // The token is kept in the object's second word.
    mem::wp(self() + 4, poolPush());
    hle::ret(self());
}
OBJC_METHOD(NSAutoreleasePool, "drain", drain) { poolPop(mem::rp(self() + 4)); }
OBJC_METHOD(NSAutoreleasePool, "release", release_) { poolPop(mem::rp(self() + 4)); }
OBJC_CLASS_SIZED(NSAutoreleasePool, NSObject, 8);
} // namespace objc

// The type encoding of a method (searching superclasses), "" when it has none (host methods).
std::string objcMethodTypes(objc::Class cls, objc::SEL s) {
    std::lock_guard lock(objc::g_lock);
    for (objc::Class c = cls; c; c = objc::superclassOf(c))
        if (auto *ci = objc::info(c))
            if (auto m = ci->methods.find(s); m != ci->methods.end()) {
                const auto types = mem::rp(m->second + 4);
                return types ? mem::cstring(types) : std::string();
            }
    return {};
}
