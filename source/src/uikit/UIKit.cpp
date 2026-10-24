// UIKit / QuartzCore / OpenGLES (EAGL) for the app's launch path, view hierarchy, frame loop and touch input, plus the
// CoreGraphics geometry functions. armv7: CGFloat is a float; CGPoint/CGSize/CGRect results come back through the
// stret pointer; structure arguments arrive as consecutive words.
#include "uikit/UIKit.h"
#include "foundation/Foundation.h"
#include "foundation/RunLoop.h"
#include "platform/GLES.h"
#include "platform/Window.h"
#include "Log.h"
#include "Loader.h"
#include "Memory.h"
#include "libc/System.h"
#include "platform/Platform.h"
#include <algorithm>
#include <chrono>
#include <cmath>
#include <map>
#include <mutex>

using namespace ns;
using objc::marg;
using objc::self;
using hle::arg;
using hle::ret;

namespace uikit {
Rect rectAt(int word) { return {hle::argFloat(word), hle::argFloat(word + 1), hle::argFloat(word + 2), hle::argFloat(word + 3)}; }
void writeRect(std::uint64_t address, const Rect &r) {
    mem::wf(address, r.x);
    mem::wf(address + 4, r.y);
    mem::wf(address + 8, r.w);
    mem::wf(address + 12, r.h);
}
void writePoint(std::uint64_t address, float x, float y) {
    mem::wf(address, x);
    mem::wf(address + 4, y);
}
void retRect(const Rect &r) { writeRect(objc::stretResult(), r); }
void retPoint(float x, float y) { writePoint(objc::stretResult(), x, y); }
std::vector<std::uint64_t> rectWords(const Rect &r) {
    return {cpu::floatBits(r.x), cpu::floatBits(r.y), cpu::floatBits(r.w), cpu::floatBits(r.h)};
}
} // namespace uikit
using namespace uikit;

namespace {
const Rect kScreen{0, 0, float(platform::kPointsWidth), float(platform::kPointsHeight)};

// Per-view state (views of guest subclasses get theirs lazily).
struct ViewData : objc::HostObject {
    Rect frame;
    id superview = 0, layer = 0, backgroundColor = 0;
    std::vector<id> subviews;
    bool hidden = false, opaque = false, userInteraction = true, multipleTouch = false;
    float alpha = 1, contentScale = 1;
    float transform[6] = {1, 0, 0, 1, 0, 0}; // CGAffineTransform, applied about the view's centre
    std::int32_t tag = 0;
    id viewController = 0;
};
std::mutex g_viewLock;
ViewData *view(id v) {
    if (!v) return nullptr;
    auto *d = objc::host<ViewData>(v);
    if (!d) {
        objc::setHostObject(v, std::make_unique<ViewData>());
        d = objc::host<ViewData>(v);
    }
    return d;
}
id g_application = 0, g_delegate = 0, g_screen = 0, g_device = 0, g_keyWindow = 0;
std::vector<id> g_touchViews; // views whose class handles touches
thread_local id t_currentContext = 0;
id singleton(const char *cls, id &slot) {
    if (!slot) { slot = objc::allocInstance(objc::getClass(cls)); objc::retain(slot); }
    return slot;
}
void sendLifecycle(const char *selector, const char *notification) {
    if (g_delegate && objc::respondsTo(g_delegate, objc::sel(selector))) objc::send(g_delegate, selector, {g_application});
    postNotification(string(notification), g_application);
}
// UIInterfaceOrientation -> window quarter turns: portrait 1 -> 0, upside down 2 -> 2, landscape left 4 (home button
// on the left) -> 1, landscape right 3 (home button on the right) -> 3.
int turnsFor(std::uint64_t orientation) {
    switch (orientation) {
    case 2: return 2;
    case 3: return 3;
    case 4: return 1;
    default: return 0;
    }
}
std::uint64_t g_statusBarOrientation = 1;
} // namespace

// ======================================== UIApplication ========================================
OBJC_CLASS(UIResponder, NSObject);
OBJC_CLASS(UIApplication, UIResponder);
OBJC_CLASS_METHOD(UIApplication, "sharedApplication", sharedApplication) { ret(singleton("UIApplication", g_application)); }
OBJC_METHOD(UIApplication, "delegate", delegate) { ret(g_delegate); }
OBJC_METHOD(UIApplication, "setDelegate:", setDelegate) { g_delegate = marg(0); }
OBJC_METHOD(UIApplication, "keyWindow", keyWindow) { ret(g_keyWindow); }
OBJC_METHOD(UIApplication, "windows", windows) { ret(array(g_keyWindow ? std::vector<id>{g_keyWindow} : std::vector<id>{})); }
OBJC_METHOD(UIApplication, "statusBarOrientation", statusBarOrientation) { ret(g_statusBarOrientation); }
static void setStatusBarOrientation(std::uint64_t orientation) {
    g_statusBarOrientation = orientation;
    hlog("uikit", "status bar orientation %llu", (unsigned long long)orientation);
    platform::setRotation(turnsFor(orientation));
}
OBJC_METHOD(UIApplication, "setStatusBarOrientation:animated:", setStatusBarOrientationAnimated) { setStatusBarOrientation(marg(0)); }
OBJC_METHOD(UIApplication, "setStatusBarOrientation:", setStatusBarOrientation1) { setStatusBarOrientation(marg(0)); }
OBJC_METHOD(UIApplication, "setStatusBarHidden:", setStatusBarHidden) {}
OBJC_METHOD(UIApplication, "setStatusBarHidden:withAnimation:", setStatusBarHiddenAnimation) {}
OBJC_METHOD(UIApplication, "setStatusBarHidden:animated:", setStatusBarHiddenAnimated) {}
OBJC_METHOD(UIApplication, "setStatusBarStyle:", setStatusBarStyle) {}
OBJC_METHOD(UIApplication, "setStatusBarStyle:animated:", setStatusBarStyleAnimated) {}
OBJC_METHOD(UIApplication, "isStatusBarHidden", isStatusBarHidden) { ret(1); }
OBJC_METHOD(UIApplication, "statusBarFrame", statusBarFrame) { retRect({0, 0, 0, 0}); }
OBJC_METHOD(UIApplication, "beginIgnoringInteractionEvents", beginIgnoring) {}
OBJC_METHOD(UIApplication, "endIgnoringInteractionEvents", endIgnoring) {}
OBJC_METHOD(UIApplication, "setIdleTimerDisabled:", setIdleTimerDisabled) {}
OBJC_METHOD(UIApplication, "isIdleTimerDisabled", isIdleTimerDisabled) { ret(1); }
OBJC_METHOD(UIApplication, "applicationState", applicationState) { ret(0); }
OBJC_METHOD(UIApplication, "openURL:", openURL) { hlog("uikit", "openURL %s (ignored)", describe(marg(0)).c_str()); ret(0); }
OBJC_METHOD(UIApplication, "canOpenURL:", canOpenURL) { ret(0); }
OBJC_METHOD(UIApplication, "registerForRemoteNotificationTypes:", registerForRemoteNotificationTypes) {}
OBJC_METHOD(UIApplication, "setApplicationIconBadgeNumber:", setBadge) {}
OBJC_METHOD(UIApplication, "applicationIconBadgeNumber", badge) { ret(0); }
OBJC_METHOD(UIApplication, "cancelAllLocalNotifications", cancelAllLocalNotifications) {}
OBJC_METHOD(UIApplication, "scheduleLocalNotification:", scheduleLocalNotification) {}
OBJC_METHOD(UIApplication, "beginBackgroundTaskWithExpirationHandler:", beginBackgroundTask) { ret(1); }
OBJC_METHOD(UIApplication, "endBackgroundTask:", endBackgroundTask) {}
OBJC_METHOD(UIApplication, "setNetworkActivityIndicatorVisible:", setNetworkActivityIndicatorVisible) {}
OBJC_METHOD(UIApplication, "sendAction:to:from:forEvent:", sendAction) { ret(0); }
static hle::DataRegistrar d_rectZero("_CGRectZero", [] { return mem::hleAlloc(16, 4); });
static hle::DataRegistrar d_sizeZero("_CGSizeZero", [] { return mem::hleAlloc(8, 4); });
static hle::DataRegistrar d_pointZero("_CGPointZero", [] { return mem::hleAlloc(8, 4); });
static hle::DataRegistrar d_affineIdentity("_CGAffineTransformIdentity", [] {
    const auto p = mem::hleAlloc(24, 4);
    mem::wf(p, 1.0f);
    mem::wf(p + 12, 1.0f);
    return p;
});

static void scheduleTestTaps();
// UIApplicationMain(argc, argv, principalClassName, delegateClassName): never returns. Without a delegate class name
// the delegate comes from MainWindow.nib (the app delegate class is looked up by name instead).
HLE(UIApplicationMain) {
    const auto principal = arg(2), delegateName = arg(3);
    hlog("uikit", "UIApplicationMain(principal=%s, delegate=%s)", principal ? text(principal).c_str() : "nil",
         delegateName ? text(delegateName).c_str() : "nil");
    sys::markMainThread();
    platform::createWindow();
    gles::init();
    runloop::setEventPump(platform::pumpEvents);
    const auto app = principal ? objc::send(objc::send(objc::getClass(text(principal)), "alloc"), "init") : singleton("UIApplication", g_application);
    g_application = app;
    objc::retain(app);
    objc::Class cls = delegateName ? objc::getClass(text(delegateName)) : objc::getClass("AppDelegate");
    if (cls) {
        g_delegate = objc::send(objc::send(cls, "alloc"), "init");
        objc::retain(g_delegate);
    }
    const auto options = dictionary({});
    hlog("uikit", "-[%s application:didFinishLaunchingWithOptions:]", className(g_delegate).c_str());
    if (objc::respondsTo(g_delegate, objc::sel("application:didFinishLaunchingWithOptions:")))
        objc::send(g_delegate, "application:didFinishLaunchingWithOptions:", {app, options});
    else if (objc::respondsTo(g_delegate, objc::sel("applicationDidFinishLaunching:")))
        objc::send(g_delegate, "applicationDidFinishLaunching:", {app});
    postNotification(string("UIApplicationDidFinishLaunchingNotification"), app);
    sendLifecycle("applicationDidBecomeActive:", "UIApplicationDidBecomeActiveNotification");
    // Closing the window: the app goes inactive and into the background (where games save) before the process ends.
    platform::setCloseHandler([] {
        sendLifecycle("applicationWillResignActive:", "UIApplicationWillResignActiveNotification");
        sendLifecycle("applicationDidEnterBackground:", "UIApplicationDidEnterBackgroundNotification");
    });
    scheduleTestTaps();
    runloop::runForever();
}

OBJC_METHOD(UIResponder, "becomeFirstResponder", becomeFirstResponder) { ret(0); }
OBJC_METHOD(UIResponder, "resignFirstResponder", resignFirstResponder) { ret(1); }
OBJC_METHOD(UIResponder, "isFirstResponder", isFirstResponder) { ret(0); }
OBJC_METHOD(UIResponder, "canBecomeFirstResponder", canBecomeFirstResponder) { ret(0); }
OBJC_METHOD(UIResponder, "nextResponder", nextResponder) { ret(0); }

// ======================================== UIScreen / UIDevice ========================================
OBJC_CLASS(UIScreen, NSObject);
OBJC_CLASS_METHOD(UIScreen, "mainScreen", mainScreen) { ret(singleton("UIScreen", g_screen)); }
OBJC_CLASS_METHOD(UIScreen, "screens", screens) { ret(array({singleton("UIScreen", g_screen)})); }
OBJC_METHOD(UIScreen, "bounds", bounds) { retRect(kScreen); }
OBJC_METHOD(UIScreen, "applicationFrame", applicationFrame) { retRect(kScreen); }
OBJC_METHOD(UIScreen, "scale", scale) { hle::retFloat(float(platform::kScale)); }
OBJC_METHOD(UIScreen, "brightness", brightness) { hle::retFloat(1.0f); }
OBJC_METHOD(UIScreen, "currentMode", currentMode) { ret(objc::allocInstance(objc::getClass("UIScreenMode"))); }
OBJC_CLASS(UIScreenMode, NSObject);
OBJC_METHOD(UIScreenMode, "size", size) { retPoint(float(platform::kPixelsWidth), float(platform::kPixelsHeight)); }
OBJC_CLASS(UIDevice, NSObject);
OBJC_CLASS_METHOD(UIDevice, "currentDevice", currentDevice) { ret(singleton("UIDevice", g_device)); }
OBJC_METHOD(UIDevice, "systemVersion", systemVersion) { ret(string("4.3.5")); }
OBJC_METHOD(UIDevice, "systemName", systemName) { ret(string("iPhone OS")); }
OBJC_METHOD(UIDevice, "model", model) { ret(string("iPhone")); }
OBJC_METHOD(UIDevice, "localizedModel", localizedModel) { ret(string("iPhone")); }
OBJC_METHOD(UIDevice, "name", name) { ret(string("iPhone")); }
OBJC_METHOD(UIDevice, "uniqueIdentifier", uniqueIdentifier) { ret(string("0123456789abcdef0123456789abcdef01234567")); }
OBJC_METHOD(UIDevice, "userInterfaceIdiom", userInterfaceIdiom) { ret(0); }
OBJC_METHOD(UIDevice, "orientation", orientation) {
    // The device is held the way the status bar points (UIDeviceOrientation landscape values are swapped).
    const auto o = g_statusBarOrientation;
    ret(o == 3 ? 4 : o == 4 ? 3 : o);
}
OBJC_METHOD(UIDevice, "isMultitaskingSupported", isMultitaskingSupported) { ret(1); }
OBJC_METHOD(UIDevice, "beginGeneratingDeviceOrientationNotifications", beginOrientation) {}
OBJC_METHOD(UIDevice, "endGeneratingDeviceOrientationNotifications", endOrientation) {}
OBJC_METHOD(UIDevice, "setBatteryMonitoringEnabled:", setBatteryMonitoringEnabled) {}
OBJC_METHOD(UIDevice, "batteryLevel", batteryLevel) { hle::retFloat(1.0f); }
OBJC_METHOD(UIDevice, "batteryState", batteryState) { ret(3); }

// ======================================== UIView / UIWindow ========================================
OBJC_CLASS(UIView, UIResponder);
OBJC_CLASS(UIWindow, UIView);
OBJC_CLASS(UIImageView, UIView);
OBJC_CLASS(UILabel, UIView);
OBJC_CLASS(UIControl, UIView);
OBJC_CLASS(UIButton, UIControl);
OBJC_CLASS(UITextField, UIControl);
OBJC_CLASS(UITextView, UIView);
OBJC_CLASS(UIActivityIndicatorView, UIView);
OBJC_CLASS(UIScrollView, UIView);
OBJC_CLASS(UITableView, UIScrollView);
OBJC_CLASS(UITableViewCell, UIView);
OBJC_CLASS(UIToolbar, UIView);
OBJC_CLASS(UIProgressView, UIView);
OBJC_CLASS(UIBarButtonItem, NSObject);
OBJC_CLASS_METHOD(UIView, "layerClass", layerClass) { ret(objc::getClass("CALayer")); }
static id initView(id v, const Rect &frame) {
    auto *d = view(v);
    d->frame = frame;
    if (!d->layer) {
        const auto layerClass = objc::send(objc::classOf(v), "layerClass");
        d->layer = objc::send(objc::send(layerClass ? layerClass : objc::getClass("CALayer"), "alloc"), "init");
        objc::send(d->layer, "setDelegate:", {v});
    }
    if (objc::respondsTo(v, objc::sel("touchesBegan:withEvent:")) && objc::lookup(objc::classOf(v), objc::sel("touchesBegan:withEvent:")) !=
                                                                          objc::lookup(objc::getClass("UIView"), objc::sel("touchesBegan:withEvent:"))) {
        std::lock_guard lock(g_viewLock);
        g_touchViews.push_back(v);
    }
    if (objc::isKindOf(v, "UIImageView") || objc::isKindOf(v, "UILabel")) d->userInteraction = false;
    return v;
}
OBJC_METHOD(UIView, "init", init) { ret(initView(self(), {0, 0, 0, 0})); }
OBJC_METHOD(UIView, "initWithFrame:", initWithFrame) { ret(initView(self(), rectAt(2))); }
OBJC_METHOD(UIView, "initWithCoder:", initWithCoder) { ret(initView(self(), kScreen)); }
OBJC_METHOD(UIView, "frame", frame) { retRect(view(objc::stretSelf())->frame); }
OBJC_METHOD(UIView, "setFrame:", setFrame) {
    auto *d = view(self());
    const auto r = rectAt(2);
    const bool resized = r.w != d->frame.w || r.h != d->frame.h;
    d->frame = r;
    if (resized) objc::send(self(), "layoutSubviews");
}
OBJC_METHOD(UIView, "bounds", bounds) { const auto &f = view(objc::stretSelf())->frame; retRect({0, 0, f.w, f.h}); }
OBJC_METHOD(UIView, "setBounds:", setBounds) { auto *d = view(self()); const auto r = rectAt(2); d->frame.w = r.w; d->frame.h = r.h; }
OBJC_METHOD(UIView, "center", center) { const auto &f = view(objc::stretSelf())->frame; retPoint(f.x + f.w / 2, f.y + f.h / 2); }
OBJC_METHOD(UIView, "setCenter:", setCenter) {
    auto *d = view(self());
    d->frame.x = objc::margFloat(0) - d->frame.w / 2;
    d->frame.y = objc::margFloat(1) - d->frame.h / 2;
}
OBJC_METHOD(UIView, "layer", layer) {
    auto *d = view(self());
    if (!d->layer) initView(self(), d->frame);
    ret(d->layer);
}
OBJC_METHOD(UIView, "addSubview:", addSubview) {
    const auto child = marg(0);
    if (!child || child == self()) return;
    auto *c = view(child);
    if (c->superview) objc::send(child, "removeFromSuperview");
    objc::retain(child);
    c->superview = self();
    view(self())->subviews.push_back(child);
    if (objc::respondsTo(child, objc::sel("didMoveToSuperview"))) objc::send(child, "didMoveToSuperview");
    if (objc::respondsTo(child, objc::sel("didMoveToWindow"))) objc::send(child, "didMoveToWindow");
}
OBJC_METHOD(UIView, "insertSubview:atIndex:", insertSubviewAtIndex) {
    objc::send(self(), "addSubview:", {marg(0)});
    auto &subs = view(self())->subviews;
    if (subs.size() > 1 && marg(1) < subs.size() - 1) {
        subs.pop_back();
        subs.insert(subs.begin() + std::ptrdiff_t(marg(1)), marg(0));
    }
}
OBJC_METHOD(UIView, "insertSubview:belowSubview:", insertBelow) { objc::send(self(), "insertSubview:atIndex:", {marg(0), 0}); }
OBJC_METHOD(UIView, "insertSubview:aboveSubview:", insertAbove) { objc::send(self(), "addSubview:", {marg(0)}); }
OBJC_METHOD(UIView, "removeFromSuperview", removeFromSuperview) {
    auto *d = view(self());
    if (!d->superview) return;
    auto &subs = view(d->superview)->subviews;
    subs.erase(std::remove(subs.begin(), subs.end(), self()), subs.end());
    d->superview = 0;
    objc::release(self());
}
OBJC_METHOD(UIView, "bringSubviewToFront:", bringSubviewToFront) {
    auto &subs = view(self())->subviews;
    auto it = std::find(subs.begin(), subs.end(), marg(0));
    if (it != subs.end()) { subs.erase(it); subs.push_back(marg(0)); }
}
OBJC_METHOD(UIView, "sendSubviewToBack:", sendSubviewToBack) {
    auto &subs = view(self())->subviews;
    auto it = std::find(subs.begin(), subs.end(), marg(0));
    if (it != subs.end()) { subs.erase(it); subs.insert(subs.begin(), marg(0)); }
}
OBJC_METHOD(UIView, "subviews", subviews) { ret(array(view(self())->subviews)); }
OBJC_METHOD(UIView, "superview", superview) { ret(view(self())->superview); }
OBJC_METHOD(UIView, "window", window) {
    for (id v = self(); v; v = view(v)->superview) if (objc::isKindOf(v, "UIWindow")) return ret(v);
    ret(g_keyWindow);
}
OBJC_METHOD(UIView, "setHidden:", setHidden) { view(self())->hidden = (marg(0) & 0xFF) != 0; }
OBJC_METHOD(UIView, "isHidden", isHidden) { ret(view(self())->hidden); }
OBJC_METHOD(UIView, "setAlpha:", setAlpha) { view(self())->alpha = objc::margFloat(0); }
OBJC_METHOD(UIView, "alpha", alpha) { hle::retFloat(view(self())->alpha); }
OBJC_METHOD(UIView, "setOpaque:", setOpaque) { view(self())->opaque = (marg(0) & 0xFF) != 0; }
OBJC_METHOD(UIView, "isOpaque", isOpaque) { ret(view(self())->opaque); }
OBJC_METHOD(UIView, "setBackgroundColor:", setBackgroundColor) { view(self())->backgroundColor = marg(0); }
OBJC_METHOD(UIView, "backgroundColor", backgroundColor) { ret(view(self())->backgroundColor); }
OBJC_METHOD(UIView, "setUserInteractionEnabled:", setUserInteractionEnabled) { view(self())->userInteraction = (marg(0) & 0xFF) != 0; }
OBJC_METHOD(UIView, "isUserInteractionEnabled", isUserInteractionEnabled) { ret(view(self())->userInteraction); }
OBJC_METHOD(UIView, "setMultipleTouchEnabled:", setMultipleTouchEnabled) { view(self())->multipleTouch = (marg(0) & 0xFF) != 0; }
OBJC_METHOD(UIView, "isMultipleTouchEnabled", isMultipleTouchEnabled) { ret(view(self())->multipleTouch); }
OBJC_METHOD(UIView, "setExclusiveTouch:", setExclusiveTouch) {}
// Changing the scale relayouts the view (EAGLView recreates its framebuffer at the new pixel size).
OBJC_METHOD(UIView, "setContentScaleFactor:", setContentScaleFactor) {
    auto *d = view(self());
    const float scale = objc::margFloat(0);
    if (scale == d->contentScale) return;
    d->contentScale = scale;
    objc::send(self(), "layoutSubviews");
}
OBJC_METHOD(UIView, "contentScaleFactor", contentScaleFactor) { hle::retFloat(view(self())->contentScale); }
OBJC_METHOD(UIView, "setTag:", setTag) { view(self())->tag = std::int32_t(marg(0)); }
OBJC_METHOD(UIView, "tag", tag) { ret(std::uint32_t(view(self())->tag)); }
OBJC_METHOD(UIView, "viewWithTag:", viewWithTag) {
    for (auto s : view(self())->subviews) if (view(s)->tag == std::int32_t(marg(0))) return ret(s);
    ret(0);
}
OBJC_METHOD(UIView, "setAutoresizingMask:", setAutoresizingMask) {}
OBJC_METHOD(UIView, "setAutoresizesSubviews:", setAutoresizesSubviews) {}
OBJC_METHOD(UIView, "setContentMode:", setContentMode) {}
OBJC_METHOD(UIView, "setClipsToBounds:", setClipsToBounds) {}
OBJC_METHOD(UIView, "setTransform:", setTransform) {
    auto *d = view(self());
    for (int i = 0; i < 6; ++i) d->transform[i] = objc::margFloat(i);
}
OBJC_METHOD(UIView, "transform", transform) {
    const auto *d = view(objc::stretSelf());
    for (int i = 0; i < 6; ++i) mem::wf(objc::stretResult() + 4 * std::uint64_t(i), d->transform[i]);
}
OBJC_METHOD(UIView, "setNeedsLayout", setNeedsLayout) {}
OBJC_METHOD(UIView, "layoutIfNeeded", layoutIfNeeded) {}
OBJC_METHOD(UIView, "layoutSubviews", layoutSubviews) {}
OBJC_METHOD(UIView, "setNeedsDisplay", setNeedsDisplay) {}
OBJC_METHOD(UIView, "sizeToFit", sizeToFit) {}
OBJC_METHOD(UIView, "endEditing:", endEditing) { ret(1); }
// Point conversion: the views share the screen's coordinate space here (full-screen views).
OBJC_METHOD(UIView, "convertPoint:fromView:", convertPointFromView) { retPoint(objc::stretArgFloat(0), objc::stretArgFloat(1)); }
OBJC_METHOD(UIView, "convertPoint:toView:", convertPointToView) { retPoint(objc::stretArgFloat(0), objc::stretArgFloat(1)); }
OBJC_METHOD(UIView, "drawRect:", drawRect) {}
OBJC_METHOD(UIView, "touchesBegan:withEvent:", touchesBegan) {}
OBJC_METHOD(UIView, "touchesMoved:withEvent:", touchesMoved) {}
OBJC_METHOD(UIView, "touchesEnded:withEvent:", touchesEnded) {}
OBJC_METHOD(UIView, "touchesCancelled:withEvent:", touchesCancelled) {}
OBJC_CLASS_METHOD(UIView, "beginAnimations:context:", beginAnimations) {}
OBJC_CLASS_METHOD(UIView, "commitAnimations", commitAnimations) {}
OBJC_CLASS_METHOD(UIView, "setAnimationDuration:", setAnimationDuration) {}
OBJC_CLASS_METHOD(UIView, "setAnimationDelegate:", setAnimationDelegate) {}
OBJC_CLASS_METHOD(UIView, "setAnimationDidStopSelector:", setAnimationDidStopSelector) {}
OBJC_CLASS_METHOD(UIView, "setAnimationCurve:", setAnimationCurve) {}
OBJC_CLASS_METHOD(UIView, "setAnimationsEnabled:", setAnimationsEnabled) {}
OBJC_METHOD(UIWindow, "makeKeyAndVisible", makeKeyAndVisible) {
    g_keyWindow = self();
    objc::retain(self());
    hlog("uikit", "window %llx is key and visible", (unsigned long long)self());
}
OBJC_METHOD(UIWindow, "makeKeyWindow", makeKeyWindow) { g_keyWindow = self(); }
OBJC_METHOD(UIWindow, "setRootViewController:", setRootViewController) {
    view(self())->viewController = marg(0);
    objc::retain(marg(0));
    if (marg(0)) if (const auto v = objc::send(marg(0), "view")) objc::send(self(), "addSubview:", {v});
}
OBJC_METHOD(UIWindow, "rootViewController", rootViewController) { ret(view(self())->viewController); }
OBJC_METHOD(UIWindow, "setWindowLevel:", setWindowLevel) {}
OBJC_METHOD(UIWindow, "screen", screen) { ret(singleton("UIScreen", g_screen)); }
OBJC_METHOD(UIImageView, "initWithImage:", initWithImage) { ret(initView(self(), kScreen)); }
OBJC_METHOD(UIImageView, "setImage:", setImage) {}

// ======================================== UIViewController ========================================
struct ControllerData : objc::HostObject { id view = 0; bool loaded = false; };
static ControllerData *controller(id c) {
    auto *d = objc::host<ControllerData>(c);
    if (!d) { objc::setHostObject(c, std::make_unique<ControllerData>()); d = objc::host<ControllerData>(c); }
    return d;
}
// UIActivityIndicatorView (MBProgressHUD's spinner): a plain view, nothing is drawn for it.
OBJC_METHOD(UIActivityIndicatorView, "initWithActivityIndicatorStyle:", initWithActivityIndicatorStyle) {
    const float side = marg(0) == 0 ? 37.f : 20.f; // UIActivityIndicatorViewStyleWhiteLarge : the small styles
    initView(self(), Rect{0, 0, side, side});
    ret(self());
}
OBJC_METHOD(UIActivityIndicatorView, "startAnimating", startAnimating) {}
OBJC_METHOD(UIActivityIndicatorView, "stopAnimating", stopAnimating) {}
OBJC_METHOD(UIActivityIndicatorView, "isAnimating", isAnimating) { ret(0); }
OBJC_METHOD(UIActivityIndicatorView, "setHidesWhenStopped:", setHidesWhenStopped) {}
OBJC_METHOD(UIActivityIndicatorView, "setActivityIndicatorViewStyle:", setActivityIndicatorViewStyle) {}
OBJC_METHOD(UIActivityIndicatorView, "setColor:", setColor) {}

OBJC_CLASS(UIViewController, UIResponder);
OBJC_METHOD(UIViewController, "init", init) { controller(self()); ret(self()); }
OBJC_METHOD(UIViewController, "initWithNibName:bundle:", initWithNibName) { controller(self()); ret(self()); }
OBJC_METHOD(UIViewController, "view", view_) {
    auto *d = controller(self());
    if (!d->view && !d->loaded) {
        d->loaded = true;
        objc::send(self(), "loadView");
        if (!d->view) {
            const auto w = rectWords(kScreen);
            d->view = objc::send(objc::send(objc::getClass("UIView"), "alloc"), "initWithFrame:", {w[0], w[1], w[2], w[3]});
            objc::retain(d->view);
        }
        objc::send(self(), "viewDidLoad");
    }
    ret(d->view);
}
OBJC_METHOD(UIViewController, "isViewLoaded", isViewLoaded) { ret(controller(self())->view != 0); }
OBJC_METHOD(UIViewController, "setView:", setView) {
    auto *d = controller(self());
    objc::retain(marg(0));
    d->view = marg(0);
    d->loaded = true;
    if (marg(0)) view(marg(0))->viewController = self();
}
OBJC_METHOD(UIViewController, "loadView", loadView) {}
OBJC_METHOD(UIViewController, "viewDidLoad", viewDidLoad) {}
OBJC_METHOD(UIViewController, "viewWillAppear:", viewWillAppear) {}
OBJC_METHOD(UIViewController, "viewDidAppear:", viewDidAppear) {}
OBJC_METHOD(UIViewController, "viewWillDisappear:", viewWillDisappear) {}
OBJC_METHOD(UIViewController, "viewDidDisappear:", viewDidDisappear) {}
OBJC_METHOD(UIViewController, "didReceiveMemoryWarning", didReceiveMemoryWarning) {}
OBJC_METHOD(UIViewController, "interfaceOrientation", interfaceOrientation) { ret(g_statusBarOrientation); }
OBJC_METHOD(UIViewController, "setWantsFullScreenLayout:", setWantsFullScreenLayout) {}
OBJC_METHOD(UIViewController, "presentModalViewController:animated:", presentModalViewController) {
    hlog("uikit", "presentModalViewController %s (not shown)", className(marg(0)).c_str());
}
OBJC_METHOD(UIViewController, "dismissModalViewControllerAnimated:", dismissModalViewController) {}
OBJC_METHOD(UIViewController, "navigationController", navigationController) { ret(0); }
OBJC_METHOD(UIViewController, "parentViewController", parentViewController) { ret(0); }

// ======================================== CALayer / CAEAGLLayer ========================================
struct LayerData : objc::HostObject { id delegate = 0, drawableProperties = 0; float contentsScale = 1; };
static LayerData *layer(id l) {
    auto *d = objc::host<LayerData>(l);
    if (!d) { objc::setHostObject(l, std::make_unique<LayerData>()); d = objc::host<LayerData>(l); }
    return d;
}
OBJC_CLASS(CALayer, NSObject);
OBJC_CLASS(CAEAGLLayer, CALayer);
OBJC_METHOD(CALayer, "init", init) { layer(self()); ret(self()); }
OBJC_METHOD(CALayer, "setDelegate:", setDelegate) { layer(self())->delegate = marg(0); }
OBJC_METHOD(CALayer, "delegate", delegate) { ret(layer(self())->delegate); }
OBJC_METHOD(CALayer, "bounds", bounds) {
    const auto v = layer(objc::stretSelf())->delegate;
    if (v) { const auto &f = view(v)->frame; return retRect({0, 0, f.w, f.h}); }
    retRect(kScreen);
}
OBJC_METHOD(CALayer, "frame", frame) {
    const auto v = layer(objc::stretSelf())->delegate;
    retRect(v ? view(v)->frame : kScreen);
}
OBJC_METHOD(CALayer, "setContentsScale:", setContentsScale) { layer(self())->contentsScale = objc::margFloat(0); }
OBJC_METHOD(CALayer, "contentsScale", contentsScale) { hle::retFloat(layer(self())->contentsScale); }
OBJC_METHOD(CALayer, "setOpaque:", setOpaque) {}
OBJC_METHOD(CALayer, "setFrame:", setFrame) {}
OBJC_METHOD(CALayer, "setBounds:", setBounds) {}
OBJC_METHOD(CALayer, "addSublayer:", addSublayer) {}
OBJC_METHOD(CALayer, "removeFromSuperlayer", removeFromSuperlayer) {}
OBJC_METHOD(CALayer, "setTransform:", setTransform) {}
OBJC_METHOD(CALayer, "setNeedsDisplay", setNeedsDisplay) {}
OBJC_METHOD(CAEAGLLayer, "setDrawableProperties:", setDrawableProperties) { objc::retain(marg(0)); layer(self())->drawableProperties = marg(0); }
OBJC_METHOD(CAEAGLLayer, "drawableProperties", drawableProperties) { ret(layer(self())->drawableProperties); }

// ======================================== EAGLContext ========================================
// kEAGLRenderingAPIOpenGLES1 = 1, ES2 = 2. Both run on the desktop compatibility context.
struct ContextData : objc::HostObject { std::uint64_t api = 1; id sharegroup = 0; };
OBJC_CLASS(EAGLContext, NSObject);
OBJC_CLASS(EAGLSharegroup, NSObject);
OBJC_METHOD(EAGLContext, "initWithAPI:", initWithAPI) {
    auto d = std::make_unique<ContextData>();
    d->api = marg(0);
    d->sharegroup = objc::allocInstance(objc::getClass("EAGLSharegroup"));
    objc::setHostObject(self(), std::move(d));
    hlog("eagl", "EAGLContext %llx (API %llu)", (unsigned long long)self(), (unsigned long long)marg(0));
    ret(self());
}
OBJC_METHOD(EAGLContext, "initWithAPI:sharegroup:", initWithAPISharegroup) {
    auto d = std::make_unique<ContextData>();
    d->api = marg(0);
    d->sharegroup = marg(1);
    objc::setHostObject(self(), std::move(d));
    hlog("eagl", "EAGLContext %llx shared", (unsigned long long)self());
    ret(self());
}
OBJC_METHOD(EAGLContext, "API", API) { auto *d = objc::host<ContextData>(self()); ret(d ? d->api : 1); }
OBJC_METHOD(EAGLContext, "sharegroup", sharegroup) { auto *d = objc::host<ContextData>(self()); ret(d ? d->sharegroup : 0); }
OBJC_CLASS_METHOD(EAGLContext, "setCurrentContext:", setCurrentContext) {
    t_currentContext = marg(0);
    if (marg(0)) gles::ensureContext();
    ret(1);
}
OBJC_CLASS_METHOD(EAGLContext, "currentContext", currentContext) { ret(t_currentContext); }
// -renderbufferStorage:fromDrawable: allocates the bound renderbuffer at the layer's pixel size.
OBJC_METHOD(EAGLContext, "renderbufferStorage:fromDrawable:", renderbufferStorage) {
    const auto l = marg(1);
    float scale = 1;
    Rect bounds = kScreen;
    if (l) {
        if (const auto v = layer(l)->delegate) {
            bounds = {0, 0, view(v)->frame.w, view(v)->frame.h};
            scale = view(v)->contentScale;
        }
        if (layer(l)->contentsScale > scale) scale = layer(l)->contentsScale;
    }
    if (bounds.w <= 0 || bounds.h <= 0) bounds = kScreen;
    ret(gles::storageFromDrawable(int(bounds.w * scale + 0.5f), int(bounds.h * scale + 0.5f)));
}
OBJC_METHOD(EAGLContext, "presentRenderbuffer:", presentRenderbuffer) { ret(gles::present()); }
static hle::DataRegistrar d_eaglColorFormatRGBA8("_kEAGLColorFormatRGBA8", [] { const auto p = mem::hleAlloc(4); mem::wp(p, string("EAGLColorFormatRGBA8")); objc::retain(mem::rp(p)); return p; });
static hle::DataRegistrar d_eaglColorFormatRGB565("_kEAGLColorFormatRGB565", [] { const auto p = mem::hleAlloc(4); mem::wp(p, string("EAGLColorFormat565")); objc::retain(mem::rp(p)); return p; });

// ======================================== CADisplayLink ========================================
struct LinkData : objc::HostObject { id target = 0; objc::SEL selector = 0; int interval = 1; };
OBJC_CLASS(CADisplayLink, NSObject);
OBJC_CLASS_METHOD(CADisplayLink, "displayLinkWithTarget:selector:", displayLinkWithTarget) {
    const auto link = objc::allocInstance(self());
    objc::retain(link);
    auto d = std::make_unique<LinkData>();
    d->target = marg(0);
    d->selector = marg(1);
    objc::retain(marg(0));
    objc::setHostObject(link, std::move(d));
    hlog("uikit", "CADisplayLink -> -[%s %s]", className(marg(0)).c_str(), objc::selName(marg(1)).c_str());
    ret(link);
}
OBJC_METHOD(CADisplayLink, "setFrameInterval:", setFrameInterval) {
    auto *d = objc::host<LinkData>(self());
    d->interval = std::max(1, int(marg(0)));
    runloop::setDisplayLinkInterval(self(), d->interval);
}
OBJC_METHOD(CADisplayLink, "frameInterval", frameInterval) { ret(std::uint32_t(objc::host<LinkData>(self())->interval)); }
OBJC_METHOD(CADisplayLink, "addToRunLoop:forMode:", addToRunLoop) {
    auto *d = objc::host<LinkData>(self());
    runloop::addDisplayLink(self(), d->target, d->selector, d->interval);
}
OBJC_METHOD(CADisplayLink, "removeFromRunLoop:forMode:", removeFromRunLoop) { runloop::removeDisplayLink(self()); }
OBJC_METHOD(CADisplayLink, "invalidate", invalidate) { runloop::removeDisplayLink(self()); }
OBJC_METHOD(CADisplayLink, "setPaused:", setPaused) {
    auto *d = objc::host<LinkData>(self());
    if (marg(0) & 0xFF) runloop::removeDisplayLink(self());
    else runloop::addDisplayLink(self(), d->target, d->selector, d->interval);
}
OBJC_METHOD(CADisplayLink, "timestamp", timestamp) { hle::retDouble(runloop::displayLinkTimestamp()); }
OBJC_METHOD(CADisplayLink, "duration", duration) { hle::retDouble(1.0 / 60.0); }
HLE(CACurrentMediaTime) {
    hle::retDouble(std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count());
}

// ======================================== Touches ========================================
// UITouch objects: one per finger, kept for the touch's lifetime (UIKit reuses the same object across phases).
struct TouchData : objc::HostObject { float x = 0, y = 0, px = 0, py = 0; double timestamp = 0; int phase = 0; id view = 0; };
OBJC_CLASS(UITouch, NSObject);
OBJC_METHOD(UITouch, "locationInView:", locationInView) {
    auto *t = objc::host<TouchData>(objc::stretSelf());
    retPoint(t ? t->x : 0, t ? t->y : 0);
}
OBJC_METHOD(UITouch, "previousLocationInView:", previousLocationInView) {
    auto *t = objc::host<TouchData>(objc::stretSelf());
    retPoint(t ? t->px : 0, t ? t->py : 0);
}
OBJC_METHOD(UITouch, "phase", phase) { auto *t = objc::host<TouchData>(self()); ret(t ? std::uint64_t(t->phase) : 0); }
OBJC_METHOD(UITouch, "tapCount", tapCount) { ret(1); }
OBJC_METHOD(UITouch, "timestamp", timestamp) { auto *t = objc::host<TouchData>(self()); hle::retDouble(t ? t->timestamp : 0); }
OBJC_METHOD(UITouch, "view", view_) { auto *t = objc::host<TouchData>(self()); ret(t ? t->view : 0); }
OBJC_METHOD(UITouch, "window", window) { ret(g_keyWindow); }
// UIEvent: its touches set in the second word.
OBJC_CLASS_SIZED(UIEvent, NSObject, 8);
OBJC_METHOD(UIEvent, "allTouches", allTouches) { ret(mem::rp(self() + 4)); }
OBJC_METHOD(UIEvent, "touchesForView:", touchesForView) { ret(mem::rp(self() + 4)); }
OBJC_METHOD(UIEvent, "timestamp", timestamp) { hle::retDouble(runloop::displayLinkTimestamp()); }
OBJC_METHOD(UIEvent, "type", type) { ret(0); }

static id touchTarget() {
    // The touch view in the key window's hierarchy (newest first), else the newest touch view.
    std::lock_guard lock(g_viewLock);
    for (auto it = g_touchViews.rbegin(); it != g_touchViews.rend(); ++it) {
        if (view(*it)->hidden) continue;
        for (id v = *it; v; v = view(v)->superview)
            if (v == g_keyWindow) return *it;
    }
    return g_touchViews.empty() ? 0 : g_touchViews.back();
}
// Fingers: 0 is the mouse, others come from keyboard bindings. Each has its own UITouch while down; the event's
// allTouches holds every finger that is down. Without multipleTouchEnabled, a second finger is ignored, as on iOS.
static std::map<int, id> g_fingers;
static void deliverTouch(int finger, int phase, double x, double y) {
    const auto target = touchTarget();
    if (!target) return;
    auto found = g_fingers.find(finger);
    if (phase == 0 && found != g_fingers.end()) return;
    if (phase != 0 && found == g_fingers.end()) return;
    if (phase == 0 && !g_fingers.empty() && !view(target)->multipleTouch) return;
    id touch;
    if (phase == 0) {
        touch = objc::allocInstance(objc::getClass("UITouch"));
        objc::retain(touch);
        auto d = std::make_unique<TouchData>();
        d->px = float(x);
        d->py = float(y);
        objc::setHostObject(touch, std::move(d));
        g_fingers[finger] = touch;
    } else {
        touch = found->second;
    }
    auto *t = objc::host<TouchData>(touch);
    if (phase != 0) { t->px = t->x; t->py = t->y; }
    t->x = float(x);
    t->y = float(y);
    t->phase = phase;
    t->view = target;
    t->timestamp = runloop::displayLinkTimestamp();
    std::vector<id> all;
    for (const auto &[f, other] : g_fingers) all.push_back(other);
    const auto touches = set({touch});
    const auto event = objc::allocInstance(objc::getClass("UIEvent"));
    mem::wp(event + 4, set(all));
    const char *selector = phase == 0 ? "touchesBegan:withEvent:" : phase == 1 ? "touchesMoved:withEvent:" :
                           phase == 3 ? "touchesEnded:withEvent:" : "touchesCancelled:withEvent:";
    objc::send(target, selector, {touches, event});
    if (phase >= 3) {
        g_fingers.erase(finger);
        objc::release(touch);
    }
}
static void deliverTouch(int phase, double x, double y) { deliverTouch(0, phase, x, y); }
// Mouse buttons in a level: a tap on the open playfield switches the stance on the phone, which a missed left click on
// the attack button would do too. So while a level is playing (its PlayLayer running: the pause menu takes it out), a
// left click in the middle band of the screen (between the HUD strips at the top and bottom) presses the attack
// button instead, and a right click is the plain tap (stance switch). Elsewhere both buttons touch where they click.
constexpr float kAttackX = 45, kAttackY = 342;                 // the attack button (portrait points)
constexpr float kPlayfieldMinX = 80, kPlayfieldMaxX = 256;      // portrait x = landscape height: the middle band
static bool levelPlaying() {
    const auto manager = objc::getClass("GameManager");
    const auto state = manager ? objc::send(manager, "sharedState") : 0;
    if (!state) return false;
    // The title screen's animated background is a level too, without the HUD (controlLayer).
    const auto layer = objc::send(state, "playLayer"), control = objc::send(state, "controlLayer");
    return layer && control && (objc::send(layer, "isRunning") & 0xFF);
}
static void mouseTouch(int phase, double x, double y, int button) {
    static bool attacking = false;
    if (phase == 0) attacking = button == 0 && x >= kPlayfieldMinX && x <= kPlayfieldMaxX && levelPlaying();
    hlog("touch", "%s at %.1f,%.1f%s", phase == 0 ? "began" : phase == 1 ? "moved" : phase == 3 ? "ended" : "cancelled", x, y,
         attacking ? " (attack)" : "");
    if (attacking) {
        if (phase != 1) deliverTouch(6, phase, kAttackX, kAttackY);
        if (phase >= 3) attacking = false;
        return;
    }
    deliverTouch(phase, x, y);
}
static const bool g_touchHook = [] {
    platform::setTouchHandler(mouseTouch);
    return true;
}();
// HLE_TAPS="seconds:x,y;seconds:drag=x,y>x,y;seconds:shot=path.bmp;seconds:keydown=0x27;seconds:keyup=0x27" scripts
// taps (portrait points), swipes, screenshots and key presses (virtual-key codes), seconds after launch, for testing;
// "seconds:lclick=x,y" / "rclick=x,y" click with a mouse button.
namespace { void keyState(int key, bool down); }
static void scheduleTestTaps() {
    const char *spec = std::getenv("HLE_TAPS");
    if (!spec) return;
    std::string list = spec;
    std::size_t start = 0;
    while (start < list.size()) {
        auto end = list.find(';', start);
        if (end == std::string::npos) end = list.size();
        const auto item = list.substr(start, end - start);
        double t = 0, x = 0, y = 0, x2 = 0, y2 = 0;
        char shotPath[512], typed[256];
        int key = 0;
        if (std::sscanf(item.c_str(), "%lf:keydown=%i", &t, &key) == 2) {
            runloop::postAfter(t, [key] { keyState(key, true); });
        } else if (std::sscanf(item.c_str(), "%lf:keyup=%i", &t, &key) == 2) {
            runloop::postAfter(t, [key] { keyState(key, false); });
        } else if (std::sscanf(item.c_str(), "%lf:lclick=%lf,%lf", &t, &x, &y) == 3 || std::sscanf(item.c_str(), "%lf:rclick=%lf,%lf", &t, &x, &y) == 3) {
            const int button = item.find("rclick") != std::string::npos;
            runloop::postAfter(t, [x, y, button] { mouseTouch(0, x, y, button); });
            runloop::postAfter(t + 0.08, [x, y, button] { mouseTouch(3, x, y, button); });
        } else if (std::sscanf(item.c_str(), "%lf:type=%255[^;]", &t, typed) == 2) {
            // "|" stands for Return.
            const std::string text = typed;
            runloop::postAfter(t, [text] { for (char c : text) platform::injectKey(c == '|' ? 13u : std::uint32_t(static_cast<unsigned char>(c))); });
        } else if (std::sscanf(item.c_str(), "%lf:shot=%511[^;]", &t, shotPath) == 2) {
            const std::string path = shotPath;
            runloop::postAfter(t, [path] { gles::requestScreenshot(path); });
        } else if (std::sscanf(item.c_str(), "%lf:drag=%lf,%lf>%lf,%lf", &t, &x, &y, &x2, &y2) == 5) {
            runloop::postAfter(t, [x, y] { deliverTouch(0, x, y); });
            for (int k = 1; k <= 8; ++k) {
                const double px = x + (x2 - x) * k / 8, py = y + (y2 - y) * k / 8;
                runloop::postAfter(t + 0.03 * k, [px, py] { deliverTouch(1, px, py); });
            }
            runloop::postAfter(t + 0.27, [x2, y2] { deliverTouch(3, x2, y2); });
        } else if (std::sscanf(item.c_str(), "%lf:%lf,%lf", &t, &x, &y) == 3) {
            runloop::postAfter(t, [x, y] { hlog("touch", "test tap %.1f,%.1f", x, y); deliverTouch(0, x, y); });
            runloop::postAfter(t + 0.08, [x, y] { deliverTouch(3, x, y); });
        }
        start = end + 1;
    }
}

// ======================================== UITextField ========================================
// Drawn over the game in the window (a white box with the text, upright for the player) while it is in a visible view
// hierarchy; typing on the Windows keyboard edits the first responder's text through its delegate, as the iOS
// keyboard does (Return -> textFieldShouldReturn:, Esc -> resign).
namespace {
struct TextFieldData : objc::HostObject {
    std::u16string text, placeholder;
    id delegate = 0, font = 0;
    float color[4] = {0, 0, 0, 1};
};
TextFieldData *field(id f) {
    auto *d = objc::host<TextFieldData>(f);
    if (!d) {
        // The view state lives in ViewData; text fields keep theirs alongside in a separate map.
        static std::map<id, std::unique_ptr<TextFieldData>> fields;
        auto &slot = fields[f];
        if (!slot) slot = std::make_unique<TextFieldData>();
        return slot.get();
    }
    return d;
}
id g_responder = 0;
std::vector<id> g_textFields;
bool inVisibleHierarchy(id v) {
    for (id p = v; p; p = view(p)->superview) {
        if (view(p)->hidden) return false;
        if (p == g_keyWindow) return true;
    }
    return false;
}
bool delegateSays(id f, const char *selector, std::initializer_list<std::uint64_t> args, bool fallback) {
    const auto delegate = field(f)->delegate;
    if (!delegate || !objc::respondsTo(delegate, objc::sel(selector))) return fallback;
    return (objc::send(delegate, selector, args) & 0xFF) != 0;
}
void becomeResponder(id f) {
    if (g_responder == f) return;
    if (!delegateSays(f, "textFieldShouldBeginEditing:", {f}, true)) return;
    g_responder = f;
    hlog("uikit", "text field editing (type on the Windows keyboard, Enter to finish)");
    if (field(f)->delegate && objc::respondsTo(field(f)->delegate, objc::sel("textFieldDidBeginEditing:")))
        objc::send(field(f)->delegate, "textFieldDidBeginEditing:", {f});
}
void resignResponder(id f) {
    if (g_responder != f) return;
    g_responder = 0;
    if (field(f)->delegate && objc::respondsTo(field(f)->delegate, objc::sel("textFieldDidEndEditing:")))
        objc::send(field(f)->delegate, "textFieldDidEndEditing:", {f});
}
// Replaces the range with the string if the delegate agrees (textField:shouldChangeCharactersInRange:replacementString:).
void edit(id f, std::size_t location, std::size_t length, const std::u16string &replacement) {
    auto *d = field(f);
    if (location > d->text.size()) return;
    if (!delegateSays(f, "textField:shouldChangeCharactersInRange:replacementString:", {f, location, length, stringU16(replacement)}, true)) return;
    d->text.replace(location, length, replacement);
}
void keyTyped(std::uint32_t character) {
    const auto f = g_responder;
    if (!f) return;
    auto *d = field(f);
    if (character == 8) { if (!d->text.empty()) edit(f, d->text.size() - 1, 1, u""); }
    else if (character == 13) delegateSays(f, "textFieldShouldReturn:", {f}, true);
    else if (character == 27) objc::send(f, "resignFirstResponder");
    else if (character >= 32 && character != 127) edit(f, d->text.size(), 0, std::u16string(1, char16_t(character)));
}
// Portrait screen point -> window client pixel.
void toWindow(float x, float y, int w, int h, float &wx, float &wy) {
    const float px = x / float(platform::kPointsWidth), py = y / float(platform::kPointsHeight);
    float u, v;
    switch (platform::rotation() & 3) {
    case 0: u = px; v = py; break;
    case 1: v = px; u = 1 - py; break;
    case 2: u = 1 - px; v = 1 - py; break;
    default: u = py; v = 1 - px; break;
    }
    wx = u * float(w);
    wy = v * float(h);
}
// Draws the visible text fields: the transformed frame's bounding box in window pixels, filled white, with the text.
void drawTextFields(int windowWidth, int windowHeight) {
    for (auto f : g_textFields) {
        if (!inVisibleHierarchy(f)) continue;
        const auto *v = view(f);
        const float cx = v->frame.x + v->frame.w / 2, cy = v->frame.y + v->frame.h / 2;
        float minX = 1e30f, minY = 1e30f, maxX = -1e30f, maxY = -1e30f;
        for (int i = 0; i < 4; ++i) {
            const float dx = ((i & 1) ? v->frame.w : 0) - v->frame.w / 2, dy = ((i & 2) ? v->frame.h : 0) - v->frame.h / 2;
            const float tx = v->transform[0] * dx + v->transform[2] * dy + v->transform[4] + cx;
            const float ty = v->transform[1] * dx + v->transform[3] * dy + v->transform[5] + cy;
            float wx, wy;
            toWindow(tx, ty, windowWidth, windowHeight, wx, wy);
            minX = std::min(minX, wx); minY = std::min(minY, wy); maxX = std::max(maxX, wx); maxY = std::max(maxY, wy);
        }
        auto *d = field(f);
        const bool editing = g_responder == f;
        std::u16string shown = d->text;
        if (editing && (platform::milliseconds() / 500) % 2) shown += u"|";
        const bool placeholder = shown.empty() && !d->placeholder.empty();
        uikit::drawOverlayBox(int(minX), int(minY), int(maxX), int(maxY), placeholder ? d->placeholder : shown, placeholder);
    }
}
// Keyboard controls: each binding holds a finger down on one of the game's on-screen buttons (portrait points), so
// moving, jumping and attacking can be combined as with several fingers on the phone. Off while a text field edits.
struct KeyBinding { int finger; float x, y; std::initializer_list<int> keys; };
const KeyBinding kBindings[] = {
    {1, 45, 57, {platform::kKeyLeft, 'A'}},                      // move left (left half of the arrow pad)
    {2, 45, 135, {platform::kKeyRight, 'D'}},                    // move right
    {3, kAttackX, kAttackY, {'X', 'J', platform::kKeyControl}},  // attack
    {4, 45, 425, {platform::kKeyUp, 'W', platform::kKeySpace, 'K'}}, // jump
    {5, 275, 45, {'P', platform::kKeyTab, platform::kKeyEscape}},  // menu (the portrait, top left)
};
void keyState(int key, bool down) {
    for (const auto &b : kBindings) {
        const bool match = key == -1 || std::find(b.keys.begin(), b.keys.end(), key) != b.keys.end();
        if (!match) continue;
        if (down && g_responder) return;
        const bool held = g_fingers.count(b.finger) != 0;
        if (down && !held) deliverTouch(b.finger, 0, b.x, b.y);
        else if (!down && held) {
            // Another key of the same binding may still be held.
            bool other = false;
            if (key != -1)
                for (int k : b.keys)
                    if (k != key && platform::keyDown(k)) other = true;
            if (!other) deliverTouch(b.finger, 3, b.x, b.y);
        }
    }
}
const bool g_textFieldHooks = [] {
    platform::setKeyStateHandler([](int key, bool down) { keyState(key, down); });
    platform::setKeyHandler([](std::uint32_t character, int) { keyTyped(character); });
    gles::setOverlay(drawTextFields);
    return true;
}();
} // namespace
OBJC_METHOD(UITextField, "initWithFrame:", initWithFrame) {
    initView(self(), rectAt(2));
    field(self());
    g_textFields.push_back(self());
    objc::retain(self());
    ret(self());
}
OBJC_METHOD(UITextField, "setText:", setText) { field(self())->text = text16(marg(0)); }
OBJC_METHOD(UITextField, "text", text_) { ret(stringU16(field(self())->text)); }
OBJC_METHOD(UITextField, "setPlaceholder:", setPlaceholder) { field(self())->placeholder = text16(marg(0)); }
OBJC_METHOD(UITextField, "placeholder", placeholder) { ret(stringU16(field(self())->placeholder)); }
OBJC_METHOD(UITextField, "setDelegate:", setDelegate) { field(self())->delegate = marg(0); }
OBJC_METHOD(UITextField, "delegate", delegate) { ret(field(self())->delegate); }
OBJC_METHOD(UITextField, "setFont:", setFont) { field(self())->font = marg(0); }
OBJC_METHOD(UITextField, "font", font) { ret(field(self())->font); }
OBJC_METHOD(UITextField, "setTextColor:", setTextColor) { colorComponents(marg(0), field(self())->color); }
OBJC_METHOD(UITextField, "setTextAlignment:", setTextAlignment) {}
OBJC_METHOD(UITextField, "setKeyboardType:", setKeyboardType) {}
OBJC_METHOD(UITextField, "setReturnKeyType:", setReturnKeyType) {}
OBJC_METHOD(UITextField, "setKeyboardAppearance:", setKeyboardAppearance) {}
OBJC_METHOD(UITextField, "setAutocorrectionType:", setAutocorrectionType) {}
OBJC_METHOD(UITextField, "setAutocapitalizationType:", setAutocapitalizationType) {}
OBJC_METHOD(UITextField, "setAdjustsFontSizeToFitWidth:", setAdjustsFontSizeToFitWidth) {}
OBJC_METHOD(UITextField, "setClearsOnBeginEditing:", setClearsOnBeginEditing) {}
OBJC_METHOD(UITextField, "setBorderStyle:", setBorderStyle) {}
OBJC_METHOD(UITextField, "setClearButtonMode:", setClearButtonMode) {}
OBJC_METHOD(UITextField, "setSecureTextEntry:", setSecureTextEntry) {}
OBJC_METHOD(UITextField, "setEnabled:", setEnabled) {}
OBJC_METHOD(UITextField, "canBecomeFirstResponder", canBecomeFirstResponder) { ret(1); }
OBJC_METHOD(UITextField, "becomeFirstResponder", becomeFirstResponder) { becomeResponder(self()); ret(1); }
OBJC_METHOD(UITextField, "resignFirstResponder", resignFirstResponder) { resignResponder(self()); ret(1); }
OBJC_METHOD(UITextField, "isFirstResponder", isFirstResponder) { ret(g_responder == self()); }
OBJC_METHOD(UITextField, "isEditing", isEditing) { ret(g_responder == self()); }
OBJC_METHOD(UITextField, "removeFromSuperview", removeFromSuperview) {
    hlog("uikit", "text field removed");
    resignResponder(self());
    objc::sendSuper(self(), objc::getClass("UIView"), "removeFromSuperview");
}

// ======================================== UIAccelerometer ========================================
// No sensor: the delegate is kept but never called (the device lies flat).
static id g_accelerometer = 0;
OBJC_CLASS(UIAccelerometer, NSObject);
OBJC_CLASS_METHOD(UIAccelerometer, "sharedAccelerometer", sharedAccelerometer) { ret(singleton("UIAccelerometer", g_accelerometer)); }
OBJC_METHOD(UIAccelerometer, "setDelegate:", setDelegate) {}
OBJC_METHOD(UIAccelerometer, "delegate", delegate) { ret(0); }
OBJC_METHOD(UIAccelerometer, "setUpdateInterval:", setUpdateInterval) {}

// ======================================== UIColor ========================================
struct ColorData : objc::HostObject { float r = 0, g = 0, b = 0, a = 1; };
static id makeColor(float r, float g, float b, float a) {
    const auto c = objc::allocInstance(objc::getClass("UIColor"));
    auto d = std::make_unique<ColorData>();
    d->r = r; d->g = g; d->b = b; d->a = a;
    objc::setHostObject(c, std::move(d));
    return c;
}
bool uikit::colorComponents(id color, float rgba[4]) {
    auto *d = objc::host<ColorData>(color);
    if (!d) return false;
    rgba[0] = d->r; rgba[1] = d->g; rgba[2] = d->b; rgba[3] = d->a;
    return true;
}
OBJC_CLASS(UIColor, NSObject);
OBJC_CLASS_METHOD(UIColor, "colorWithRed:green:blue:alpha:", colorWithRGBA) {
    ret(makeColor(objc::margFloat(0), objc::margFloat(1), objc::margFloat(2), objc::margFloat(3)));
}
OBJC_CLASS_METHOD(UIColor, "colorWithWhite:alpha:", colorWithWhite) {
    ret(makeColor(objc::margFloat(0), objc::margFloat(0), objc::margFloat(0), objc::margFloat(1)));
}
OBJC_METHOD(UIColor, "initWithRed:green:blue:alpha:", initWithRGBA) {
    auto d = std::make_unique<ColorData>();
    d->r = objc::margFloat(0); d->g = objc::margFloat(1); d->b = objc::margFloat(2); d->a = objc::margFloat(3);
    objc::setHostObject(self(), std::move(d));
    ret(self());
}
OBJC_CLASS_METHOD(UIColor, "blackColor", blackColor) { ret(makeColor(0, 0, 0, 1)); }
OBJC_CLASS_METHOD(UIColor, "whiteColor", whiteColor) { ret(makeColor(1, 1, 1, 1)); }
OBJC_CLASS_METHOD(UIColor, "clearColor", clearColor) { ret(makeColor(0, 0, 0, 0)); }
OBJC_CLASS_METHOD(UIColor, "grayColor", grayColor) { ret(makeColor(0.5f, 0.5f, 0.5f, 1)); }
OBJC_CLASS_METHOD(UIColor, "darkGrayColor", darkGrayColor) { ret(makeColor(1.f / 3, 1.f / 3, 1.f / 3, 1)); }
OBJC_CLASS_METHOD(UIColor, "lightGrayColor", lightGrayColor) { ret(makeColor(2.f / 3, 2.f / 3, 2.f / 3, 1)); }
OBJC_CLASS_METHOD(UIColor, "redColor", redColor) { ret(makeColor(1, 0, 0, 1)); }
OBJC_CLASS_METHOD(UIColor, "greenColor", greenColor) { ret(makeColor(0, 1, 0, 1)); }
OBJC_CLASS_METHOD(UIColor, "blueColor", blueColor) { ret(makeColor(0, 0, 1, 1)); }
OBJC_CLASS_METHOD(UIColor, "yellowColor", yellowColor) { ret(makeColor(1, 1, 0, 1)); }
OBJC_CLASS_METHOD(UIColor, "orangeColor", orangeColor) { ret(makeColor(1, 0.5f, 0, 1)); }
OBJC_CLASS_METHOD(UIColor, "colorWithPatternImage:", colorWithPatternImage) { ret(makeColor(0, 0, 0, 1)); }
OBJC_METHOD(UIColor, "CGColor", CGColor) { ret(self()); }
OBJC_METHOD(UIColor, "set", set_) {
    float c[4];
    if (colorComponents(self(), c)) uikit::setFillColor(c);
}
OBJC_METHOD(UIColor, "setFill", setFill) {
    float c[4];
    if (colorComponents(self(), c)) uikit::setFillColor(c);
}
OBJC_METHOD(UIColor, "colorWithAlphaComponent:", colorWithAlphaComponent) {
    auto *d = objc::host<ColorData>(self());
    ret(makeColor(d ? d->r : 0, d ? d->g : 0, d ? d->b : 0, objc::margFloat(0)));
}

// ======================================== CoreGraphics geometry ========================================
// Functions returning structures write through the hidden pointer in r0; the arguments follow from r1.
HLE(CGRectGetMinX) { const auto r = rectAt(0); hle::retFloat(std::min(r.x, r.x + r.w)); }
HLE(CGRectGetMinY) { const auto r = rectAt(0); hle::retFloat(std::min(r.y, r.y + r.h)); }
HLE(CGRectGetMaxX) { const auto r = rectAt(0); hle::retFloat(std::max(r.x, r.x + r.w)); }
HLE(CGRectGetMaxY) { const auto r = rectAt(0); hle::retFloat(std::max(r.y, r.y + r.h)); }
HLE(CGRectGetMidX) { const auto r = rectAt(0); hle::retFloat(r.x + r.w / 2); }
HLE(CGRectGetMidY) { const auto r = rectAt(0); hle::retFloat(r.y + r.h / 2); }
HLE(CGRectGetWidth) { hle::retFloat(std::fabs(hle::argFloat(2))); }
HLE(CGRectGetHeight) { hle::retFloat(std::fabs(hle::argFloat(3))); }
static Rect standardized(Rect r) {
    if (r.w < 0) { r.x += r.w; r.w = -r.w; }
    if (r.h < 0) { r.y += r.h; r.h = -r.h; }
    return r;
}
// CGRectContainsPoint(rect, point)
HLE(CGRectContainsPoint) {
    const auto r = standardized(rectAt(0));
    const float x = hle::argFloat(4), y = hle::argFloat(5);
    ret(x >= r.x && x < r.x + r.w && y >= r.y && y < r.y + r.h);
}
HLE(CGRectIntersectsRect) {
    const auto a = standardized(rectAt(0)), b = standardized(rectAt(4));
    ret(a.w > 0 && a.h > 0 && b.w > 0 && b.h > 0 && a.x < b.x + b.w && b.x < a.x + a.w && a.y < b.y + b.h && b.y < a.y + a.h);
}
HLE(CGRectEqualToRect) {
    const auto a = rectAt(0), b = rectAt(4);
    ret(a.x == b.x && a.y == b.y && a.w == b.w && a.h == b.h);
}
HLE(CGRectIsEmpty) { const auto r = rectAt(0); ret(r.w == 0 || r.h == 0); }
// CGAffineTransform {a, b, c, d, tx, ty} (6 floats).
namespace {
struct Affine { float a = 1, b = 0, c = 0, d = 1, tx = 0, ty = 0; };
Affine affineAt(int word) {
    return {hle::argFloat(word), hle::argFloat(word + 1), hle::argFloat(word + 2), hle::argFloat(word + 3), hle::argFloat(word + 4),
            hle::argFloat(word + 5)};
}
void retAffine(const Affine &t) {
    const auto out = arg(0);
    const float v[6] = {t.a, t.b, t.c, t.d, t.tx, t.ty};
    for (int i = 0; i < 6; ++i) mem::wf(out + 4 * i, v[i]);
}
Affine concat(const Affine &t1, const Affine &t2) {
    return {t1.a * t2.a + t1.b * t2.c, t1.a * t2.b + t1.b * t2.d, t1.c * t2.a + t1.d * t2.c, t1.c * t2.b + t1.d * t2.d,
            t1.tx * t2.a + t1.ty * t2.c + t2.tx, t1.tx * t2.b + t1.ty * t2.d + t2.ty};
}
} // namespace
HLE(CGAffineTransformMakeScale) { retAffine({hle::argFloat(1), 0, 0, hle::argFloat(2), 0, 0}); }
HLE(CGAffineTransformMakeTranslation) { retAffine({1, 0, 0, 1, hle::argFloat(1), hle::argFloat(2)}); }
HLE(CGAffineTransformMakeRotation) {
    const float a = hle::argFloat(1), c = std::cos(a), s = std::sin(a);
    retAffine({c, s, -s, c, 0, 0});
}
HLE(CGAffineTransformConcat) { retAffine(concat(affineAt(1), affineAt(7))); }
HLE(CGAffineTransformInvert) {
    const auto t = affineAt(1);
    const float det = t.a * t.d - t.b * t.c;
    if (det == 0) return retAffine(t);
    retAffine({t.d / det, -t.b / det, -t.c / det, t.a / det, (t.c * t.ty - t.d * t.tx) / det, (t.b * t.tx - t.a * t.ty) / det});
}
HLE(CGAffineTransformRotate) {
    const float a = hle::argFloat(7), c = std::cos(a), s = std::sin(a);
    retAffine(concat({c, s, -s, c, 0, 0}, affineAt(1)));
}
HLE(CGAffineTransformScale) { retAffine(concat({hle::argFloat(7), 0, 0, hle::argFloat(8), 0, 0}, affineAt(1))); }
HLE(CGAffineTransformTranslate) { retAffine(concat({1, 0, 0, 1, hle::argFloat(7), hle::argFloat(8)}, affineAt(1))); }
// CGRectApplyAffineTransform(result *, rect, transform): the bounding box of the transformed corners.
HLE(CGRectApplyAffineTransform) {
    const auto r = rectAt(1);
    const auto t = affineAt(5);
    float minX = 1e30f, minY = 1e30f, maxX = -1e30f, maxY = -1e30f;
    for (int i = 0; i < 4; ++i) {
        const float x = r.x + ((i & 1) ? r.w : 0), y = r.y + ((i & 2) ? r.h : 0);
        const float tx = t.a * x + t.c * y + t.tx, ty = t.b * x + t.d * y + t.ty;
        minX = std::min(minX, tx); minY = std::min(minY, ty); maxX = std::max(maxX, tx); maxY = std::max(maxY, ty);
    }
    writeRect(arg(0), {minX, minY, maxX - minX, maxY - minY});
}
// CGPointFromString("{x, y}"), CGSizeFromString("{w, h}"), CGRectFromString("{{x, y}, {w, h}}") (result pointer first).
static std::vector<float> numbersIn(const std::string &s) {
    std::vector<float> out;
    const char *p = s.c_str();
    while (*p) {
        if (std::isdigit(static_cast<unsigned char>(*p)) || *p == '-' || *p == '.' || *p == '+') {
            char *end = nullptr;
            out.push_back(std::strtof(p, &end));
            p = end > p ? end : p + 1;
        } else ++p;
    }
    return out;
}
HLE(CGPointFromString) {
    auto n = numbersIn(text(arg(1)));
    n.resize(2, 0);
    writePoint(arg(0), n[0], n[1]);
}
HLE(CGSizeFromString) {
    auto n = numbersIn(text(arg(1)));
    n.resize(2, 0);
    writePoint(arg(0), n[0], n[1]);
}
HLE(CGRectFromString) {
    auto n = numbersIn(text(arg(1)));
    n.resize(4, 0);
    writeRect(arg(0), {n[0], n[1], n[2], n[3]});
}
HLE(NSStringFromCGRect) {
    const auto r = rectAt(0);
    char b[128];
    std::snprintf(b, sizeof b, "{{%g, %g}, {%g, %g}}", r.x, r.y, r.w, r.h);
    ret(string(b));
}
HLE(NSStringFromCGPoint) {
    char b[96];
    std::snprintf(b, sizeof b, "{%g, %g}", hle::argFloat(0), hle::argFloat(1));
    ret(string(b));
}
HLE(NSStringFromCGSize) {
    char b[96];
    std::snprintf(b, sizeof b, "{%g, %g}", hle::argFloat(0), hle::argFloat(1));
    ret(string(b));
}
// NSRange helpers (two words each): NSUnionRange / NSIntersectionRange return the NSRange through r0.
HLE(NSUnionRange) {
    const auto a0 = arg(1), a1 = a0 + arg(2), b0 = arg(3), b1 = b0 + arg(4);
    const auto lo = std::min(a0, b0), hi = std::max(a1, b1);
    mem::w32(arg(0), std::uint32_t(lo));
    mem::w32(arg(0) + 4, std::uint32_t(hi - lo));
}
HLE(NSIntersectionRange) {
    const auto a0 = arg(1), a1 = a0 + arg(2), b0 = arg(3), b1 = b0 + arg(4);
    const auto lo = std::max(a0, b0), hi = std::min(a1, b1);
    mem::w32(arg(0), hi > lo ? std::uint32_t(lo) : 0);
    mem::w32(arg(0) + 4, hi > lo ? std::uint32_t(hi - lo) : 0);
}
