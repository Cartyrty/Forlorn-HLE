#include "Runtime.h"
#include "Cpu.h"
#include "Loader.h"
#include "Log.h"
#include "objc/ObjC.h"
#include "Hle.h"

namespace runtime {
namespace {
std::string g_bundle;
}
void setBundlePath(const std::string &path) { g_bundle = path; }
const std::string &bundlePath() { return g_bundle; }
void initialize() {
    objc::loadImage();     // classes, categories, selectors (and method names for patches by "-[Class selector]")
    hle::applyPatches();
    // The game builds levels on a background thread (-[PlayLayer initialLoading]): scheduling timers and running actions
    // there while the main thread walks the same cocos2d hash tables in tick:/update: corrupts them now and then (rare on
    // a phone, more frequent under emulation). Both classes run their methods one thread at a time.
    for (const char *name : {"CCScheduler", "CCActionManager"})
        if (objc::getClass(name)) objc::serializeClass(name);
    const auto inits = loader::initializers();
    hlog("init", "%zu static initializers", inits.size());
    for (std::size_t i = 0; i < inits.size(); ++i) {
        hlog("init", "[%zu] %s", i, loader::describe(inits[i]).c_str());
        cpu::call(inits[i], {0, 0, 0, 0, 0});
    }
}
} // namespace runtime
