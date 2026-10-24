// NSKeyedArchiver / NSKeyedUnarchiver (the app's save data) and NSOperation / NSInvocationOperation /
// NSOperationQueue.
//
// Archives are XML property lists of the encoded values: property-list objects as themselves, arrays as
// {"$array": [...], "$m": mutable}, dictionaries as {"$dict": {...}, "$m": mutable}, and NSCoding objects as their
// -encodeWithCoder: keys plus "$class". Decoding rebuilds the same objects (-initWithCoder: for NSCoding classes).
#include "foundation/Foundation.h"
#include "foundation/PList.h"
#include "libc/System.h"
#include "Log.h"
#include "Memory.h"
#include <mutex>

using namespace ns;
using objc::marg;
using objc::self;
using hle::ret;

namespace {
struct CoderData : objc::HostObject {
    id values = 0;      // NSMutableDictionary of encoded values (archiver) / the archive's dictionary (unarchiver)
    id data = 0;        // the NSMutableData to write at -finishEncoding
    bool finished = false;
};
CoderData *coder(id c) { return objc::host<CoderData>(c); }
id newCoder(const char *cls, id values, id data = 0) {
    const auto c = objc::allocInstance(objc::getClass(cls));
    auto d = std::make_unique<CoderData>();
    d->values = values;
    objc::retain(values);
    d->data = data;
    objc::setHostObject(c, std::move(d));
    return c;
}
bool isMutable(id o, const char *mutableClass) { return objc::isKindOf(o, mutableClass); }
id encodeValue(id o, int depth = 0);
id encodeValue(id o, int depth) {
    if (!o || depth > 64) return 0;
    if (isString(o) || isNumber(o) || objc::host<DataBytes>(o) || objc::isKindOf(o, "NSDate")) return o;
    if (objc::host<ArrayData>(o) || objc::host<SetData>(o)) {
        std::vector<id> items;
        for (auto i : arrayItems(o)) if (const auto e = encodeValue(i, depth + 1)) items.push_back(e);
        return dictionary({{string("$array"), array(items)}, {string("$m"), numberBool(isMutable(o, "NSMutableArray"))}});
    }
    if (auto *d = objc::host<DictData>(o)) {
        std::vector<std::pair<id, id>> items;
        for (auto &[k, v] : d->items) if (const auto e = encodeValue(v, depth + 1)) items.push_back({string(describe(k)), e});
        return dictionary({{string("$dict"), dictionary(items)}, {string("$m"), numberBool(isMutable(o, "NSMutableDictionary"))}});
    }
    if (objc::respondsTo(o, objc::sel("encodeWithCoder:"))) {
        const auto values = dictionary({}, true);
        const auto nested = newCoder("NSKeyedArchiver", values);
        objc::send(o, "encodeWithCoder:", {nested});
        dictSet(values, string("$class"), string(className(o)));
        return values;
    }
    hlog("coder", "cannot encode a %s", className(o).c_str());
    return 0;
}
id decodeValue(id v, int depth = 0);
id decodeValue(id v, int depth) {
    if (!v || depth > 64) return 0;
    if (auto *d = objc::host<DictData>(v)) {
        if (const auto items = dictGet(v, string("$array"))) {
            std::vector<id> out;
            for (auto i : arrayItems(items)) if (const auto e = decodeValue(i, depth + 1)) out.push_back(e);
            return array(out, intValue(dictGet(v, string("$m"))) != 0);
        }
        if (const auto items = dictGet(v, string("$dict"))) {
            std::vector<std::pair<id, id>> out;
            if (auto *dd = objc::host<DictData>(items))
                for (auto &[k, value] : dd->items) if (const auto e = decodeValue(value, depth + 1)) out.push_back({k, e});
            return dictionary(out, intValue(dictGet(v, string("$m"))) != 0);
        }
        if (const auto cls = dictGet(v, string("$class"))) {
            const auto c = objc::getClass(text(cls));
            if (!c) { hlog("coder", "unknown archived class %s", text(cls).c_str()); return 0; }
            const auto nested = newCoder("NSKeyedUnarchiver", v);
            return objc::send(objc::send(c, "alloc"), "initWithCoder:", {nested});
        }
        (void)d;
    }
    return v;
}
id archivedData(id root) {
    const auto values = dictionary({{string("root"), encodeValue(root)}});
    const auto xml = plist::serialize(values);
    return data(xml.data(), xml.size());
}
id unarchive(id archive) {
    if (!archive) return 0;
    const auto values = plist::parse(std::string(reinterpret_cast<char *>(mem::base() + dataBytes(archive)), std::size_t(dataLength(archive))));
    return objc::host<DictData>(values) ? decodeValue(dictGet(values, string("root"))) : 0;
}
id valueFor(id key) {
    auto *c = coder(self());
    return c ? dictGet(c->values, key) : 0;
}
void store(id key, id value) {
    auto *c = coder(self());
    if (c && key && value) dictSet(c->values, key, value);
}
} // namespace

OBJC_CLASS(NSCoder, NSObject);
OBJC_CLASS(NSKeyedArchiver, NSCoder);
OBJC_CLASS(NSKeyedUnarchiver, NSCoder);
OBJC_METHOD(NSCoder, "allowsKeyedCoding", allowsKeyedCoding) { ret(1); }
OBJC_METHOD(NSCoder, "containsValueForKey:", containsValueForKey) { ret(valueFor(marg(0)) != 0); }
// Archiving.
OBJC_CLASS_METHOD(NSKeyedArchiver, "archivedDataWithRootObject:", archivedDataWithRootObject) { ret(archivedData(marg(0))); }
OBJC_CLASS_METHOD(NSKeyedArchiver, "archiveRootObject:toFile:", archiveRootObjectToFile) {
    ret(objc::send(archivedData(marg(0)), "writeToFile:atomically:", {marg(1), 1}));
}
OBJC_METHOD(NSKeyedArchiver, "initForWritingWithMutableData:", initForWriting) {
    auto d = std::make_unique<CoderData>();
    d->values = dictionary({}, true);
    objc::retain(d->values);
    d->data = marg(0);
    objc::retain(marg(0));
    objc::setHostObject(self(), std::move(d));
    ret(self());
}
OBJC_METHOD(NSKeyedArchiver, "setOutputFormat:", setOutputFormat) {}
OBJC_METHOD(NSKeyedArchiver, "finishEncoding", finishEncoding) {
    auto *c = coder(self());
    if (!c || c->finished || !c->data) return;
    c->finished = true;
    const auto xml = plist::serialize(dictionary({{string("root"), c->values}}));
    objc::send(c->data, "appendBytes:length:", {mem::heapString(xml), xml.size()});
}
OBJC_METHOD(NSKeyedArchiver, "encodeObject:forKey:", encodeObject) { store(marg(1), encodeValue(marg(0))); }
OBJC_METHOD(NSKeyedArchiver, "encodeConditionalObject:forKey:", encodeConditionalObject) { store(marg(1), encodeValue(marg(0))); }
OBJC_METHOD(NSKeyedArchiver, "encodeInt:forKey:", encodeInt) { store(marg(1), number(std::int32_t(marg(0)), 'i')); }
OBJC_METHOD(NSKeyedArchiver, "encodeInt32:forKey:", encodeInt32) { store(marg(1), number(std::int32_t(marg(0)), 'i')); }
OBJC_METHOD(NSKeyedArchiver, "encodeInteger:forKey:", encodeInteger) { store(marg(1), number(std::int32_t(marg(0)), 'i')); }
OBJC_METHOD(NSKeyedArchiver, "encodeInt64:forKey:", encodeInt64) { store(marg(2), number(hle::arg64(2), 'q')); }
OBJC_METHOD(NSKeyedArchiver, "encodeBool:forKey:", encodeBool) { store(marg(1), numberBool((marg(0) & 0xFF) != 0)); }
OBJC_METHOD(NSKeyedArchiver, "encodeFloat:forKey:", encodeFloat) { store(marg(1), numberDouble(objc::margFloat(0), 'f')); }
OBJC_METHOD(NSKeyedArchiver, "encodeDouble:forKey:", encodeDouble) { store(marg(2), numberDouble(objc::margDouble(0))); }
OBJC_METHOD(NSKeyedArchiver, "encodeBytes:length:forKey:", encodeBytes) { store(marg(2), dataFromGuest(marg(0), std::size_t(marg(1)))); }
OBJC_METHOD(NSKeyedArchiver, "encodeCGPoint:forKey:", encodeCGPoint) {
    store(marg(2), array({numberDouble(objc::margFloat(0), 'f'), numberDouble(objc::margFloat(1), 'f')}));
}
// Unarchiving.
OBJC_CLASS_METHOD(NSKeyedUnarchiver, "unarchiveObjectWithData:", unarchiveObjectWithData) { ret(unarchive(marg(0))); }
OBJC_CLASS_METHOD(NSKeyedUnarchiver, "unarchiveObjectWithFile:", unarchiveObjectWithFile) {
    ret(unarchive(objc::send(objc::getClass("NSData"), "dataWithContentsOfFile:", {marg(0)})));
}
OBJC_METHOD(NSKeyedUnarchiver, "initForReadingWithData:", initForReading) {
    if (!marg(0)) return ret(0);
    const auto values = plist::parse(std::string(reinterpret_cast<char *>(mem::base() + dataBytes(marg(0))), std::size_t(dataLength(marg(0)))));
    const auto root = objc::host<DictData>(values) ? dictGet(values, string("root")) : 0;
    if (!objc::host<DictData>(root)) { hlog("coder", "initForReadingWithData: not an archive written here"); return ret(0); }
    auto d = std::make_unique<CoderData>();
    d->values = root;
    objc::retain(root);
    objc::setHostObject(self(), std::move(d));
    ret(self());
}
OBJC_METHOD(NSKeyedUnarchiver, "finishDecoding", finishDecoding) {}
OBJC_METHOD(NSKeyedUnarchiver, "decodeObjectForKey:", decodeObject) { ret(decodeValue(valueFor(marg(0)))); }
OBJC_METHOD(NSKeyedUnarchiver, "decodeIntForKey:", decodeInt) { ret(std::uint32_t(intValue(valueFor(marg(0))))); }
OBJC_METHOD(NSKeyedUnarchiver, "decodeInt32ForKey:", decodeInt32) { ret(std::uint32_t(intValue(valueFor(marg(0))))); }
OBJC_METHOD(NSKeyedUnarchiver, "decodeIntegerForKey:", decodeInteger) { ret(std::uint32_t(intValue(valueFor(marg(0))))); }
OBJC_METHOD(NSKeyedUnarchiver, "decodeInt64ForKey:", decodeInt64) { hle::ret64(std::uint64_t(intValue(valueFor(marg(0))))); }
OBJC_METHOD(NSKeyedUnarchiver, "decodeBoolForKey:", decodeBool) { ret(doubleValue(valueFor(marg(0))) != 0); }
OBJC_METHOD(NSKeyedUnarchiver, "decodeFloatForKey:", decodeFloat) { hle::retFloat(float(doubleValue(valueFor(marg(0))))); }
OBJC_METHOD(NSKeyedUnarchiver, "decodeDoubleForKey:", decodeDouble) { hle::retDouble(doubleValue(valueFor(marg(0)))); }
// decodeBytesForKey:returnedLength: -> a pointer to the bytes (kept alive by the archive's data object).
OBJC_METHOD(NSKeyedUnarchiver, "decodeBytesForKey:returnedLength:", decodeBytes) {
    const auto v = valueFor(marg(0));
    if (marg(1)) mem::w32(marg(1), std::uint32_t(dataLength(v)));
    ret(dataBytes(v));
}
// CGPoint result: objc_msgSend_stret.
OBJC_METHOD(NSKeyedUnarchiver, "decodeCGPointForKey:", decodeCGPoint) {
    auto *c = coder(objc::stretSelf());
    const auto v = c ? dictGet(c->values, objc::stretArg(0)) : 0;
    const auto items = arrayItems(v);
    mem::wf(objc::stretResult(), items.size() > 0 ? float(doubleValue(items[0])) : 0);
    mem::wf(objc::stretResult() + 4, items.size() > 1 ? float(doubleValue(items[1])) : 0);
}

// ======================================== Operations ========================================
namespace {
struct OperationData : objc::HostObject {
    id target = 0, argument = 0;
    objc::SEL selector = 0;
    bool executing = false, finished = false, cancelled = false;
};
OperationData *operation(id o) {
    auto *d = objc::host<OperationData>(o);
    if (!d) { objc::setHostObject(o, std::make_unique<OperationData>()); d = objc::host<OperationData>(o); }
    return d;
}
} // namespace
OBJC_CLASS(NSOperation, NSObject);
OBJC_CLASS(NSInvocationOperation, NSOperation);
OBJC_CLASS(NSOperationQueue, NSObject);
OBJC_METHOD(NSOperation, "init", init) { operation(self()); ret(self()); }
OBJC_METHOD(NSOperation, "start", start) {
    auto *d = operation(self());
    if (d->cancelled) { d->finished = true; return; }
    d->executing = true;
    objc::send(self(), "main");
    d->executing = false;
    d->finished = true;
}
OBJC_METHOD(NSOperation, "main", main_) {}
OBJC_METHOD(NSOperation, "cancel", cancel) { operation(self())->cancelled = true; }
OBJC_METHOD(NSOperation, "isCancelled", isCancelled) { ret(operation(self())->cancelled); }
OBJC_METHOD(NSOperation, "isExecuting", isExecuting) { ret(operation(self())->executing); }
OBJC_METHOD(NSOperation, "isFinished", isFinished) { ret(operation(self())->finished); }
OBJC_METHOD(NSOperation, "isConcurrent", isConcurrent) { ret(0); }
OBJC_METHOD(NSOperation, "isReady", isReady) { ret(1); }
OBJC_METHOD(NSOperation, "setQueuePriority:", setQueuePriority) {}
OBJC_METHOD(NSOperation, "setThreadPriority:", setThreadPriority) {}
OBJC_METHOD(NSOperation, "waitUntilFinished", waitUntilFinished) {}
OBJC_METHOD(NSInvocationOperation, "initWithTarget:selector:object:", initWithTarget) {
    auto *d = operation(self());
    d->target = marg(0);
    d->selector = marg(1);
    d->argument = marg(2);
    objc::retain(marg(0));
    objc::retain(marg(2));
    ret(self());
}
OBJC_METHOD(NSInvocationOperation, "main", main_) {
    auto *d = operation(self());
    if (d->target && d->selector) objc::send(d->target, objc::selName(d->selector), {d->argument});
}
// Each operation runs on its own guest thread.
OBJC_METHOD(NSOperationQueue, "addOperation:", addOperation) {
    const auto op = marg(0);
    if (!op) return;
    objc::retain(op);
    sys::spawnGuestThread("NSOperation", [op] { objc::send(op, "start"); });
}
OBJC_METHOD(NSOperationQueue, "setMaxConcurrentOperationCount:", setMaxConcurrentOperationCount) {}
OBJC_METHOD(NSOperationQueue, "setSuspended:", setSuspended) {}
OBJC_METHOD(NSOperationQueue, "isSuspended", isSuspended) { ret(0); }
OBJC_METHOD(NSOperationQueue, "cancelAllOperations", cancelAllOperations) {}
OBJC_METHOD(NSOperationQueue, "operationCount", operationCount) { ret(0); }
OBJC_METHOD(NSOperationQueue, "operations", operations) { ret(array({})); }
OBJC_METHOD(NSOperationQueue, "waitUntilAllOperationsAreFinished", waitUntilAllOperationsAreFinished) {}
OBJC_METHOD(NSOperationQueue, "setName:", setName) {}
