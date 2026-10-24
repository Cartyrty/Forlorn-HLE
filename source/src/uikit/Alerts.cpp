// UIAlertView / UIActionSheet as the host's message boxes (platform::showAlert). Button index 0 is the cancel button (if any), then
// the other buttons in order, matching UIKit; the delegate gets alertView:clickedButtonAtIndex: and
// alertView:didDismissWithButtonIndex: from the run loop after the dialog closes.
#include "foundation/Foundation.h"
#include "foundation/RunLoop.h"
#include "Log.h"
#include "Memory.h"
#include "platform/Platform.h"
#include <algorithm>
#include <cstdlib>

using namespace ns;
using objc::marg;
using objc::self;
using hle::ret;

namespace {
struct AlertData : objc::HostObject {
    std::string title, message;
    id delegate = 0;
    std::vector<std::string> buttons; // UIKit order: cancel first when present
    bool hasCancel = false;
    bool visible = false;
};
int showDialog(const AlertData &a) {
    // HLE_AUTO_DIALOG=<index> answers dialogs automatically (scripted test runs).
    if (const char *autoAnswer = std::getenv("HLE_AUTO_DIALOG")) return std::min(std::atoi(autoAnswer), int(std::max<std::size_t>(1, a.buttons.size())) - 1);
    return platform::showAlert(a.title, a.message, a.buttons);
}
} // namespace

OBJC_CLASS(UIAlertView, UIView);
// initWithTitle:message:delegate:cancelButtonTitle:otherButtonTitles: (a nil-terminated list continuing the argument words)
OBJC_METHOD(UIAlertView, "initWithTitle:message:delegate:cancelButtonTitle:otherButtonTitles:", initWithTitle) {
    auto d = std::make_unique<AlertData>();
    d->title = marg(0) ? text(marg(0)) : "";
    d->message = marg(1) ? text(marg(1)) : "";
    d->delegate = marg(2);
    if (marg(3)) { d->buttons.push_back(text(marg(3))); d->hasCancel = true; }
    if (marg(4)) {
        for (int i = 4; marg(i); ++i) d->buttons.push_back(text(marg(i)));
    }
    objc::retain(marg(2));
    objc::setHostObject(self(), std::move(d));
    ret(self());
}
OBJC_METHOD(UIAlertView, "addButtonWithTitle:", addButtonWithTitle) {
    auto *d = objc::host<AlertData>(self());
    if (!d) return ret(0);
    d->buttons.push_back(text(marg(0)));
    ret(d->buttons.size() - 1);
}
OBJC_METHOD(UIAlertView, "setDelegate:", setDelegate) { if (auto *d = objc::host<AlertData>(self())) d->delegate = marg(0); }
OBJC_METHOD(UIAlertView, "delegate", delegate) { auto *d = objc::host<AlertData>(self()); ret(d ? d->delegate : 0); }
OBJC_METHOD(UIAlertView, "setTitle:", setTitle) { if (auto *d = objc::host<AlertData>(self())) d->title = text(marg(0)); }
OBJC_METHOD(UIAlertView, "setMessage:", setMessage) { if (auto *d = objc::host<AlertData>(self())) d->message = text(marg(0)); }
OBJC_METHOD(UIAlertView, "cancelButtonIndex", cancelButtonIndex) { auto *d = objc::host<AlertData>(self()); ret(d && d->hasCancel ? 0 : ~0ull); }
OBJC_METHOD(UIAlertView, "numberOfButtons", numberOfButtons) { auto *d = objc::host<AlertData>(self()); ret(d ? d->buttons.size() : 0); }
OBJC_METHOD(UIAlertView, "buttonTitleAtIndex:", buttonTitleAtIndex) {
    auto *d = objc::host<AlertData>(self());
    ret(d && marg(0) < d->buttons.size() ? string(d->buttons[std::size_t(marg(0))]) : 0);
}
OBJC_METHOD(UIAlertView, "isVisible", isVisible) { auto *d = objc::host<AlertData>(self()); ret(d && d->visible); }
OBJC_METHOD(UIAlertView, "setAlertViewStyle:", setAlertViewStyle) {}
OBJC_METHOD(UIAlertView, "show", show) {
    const auto alert = self();
    auto *d = objc::host<AlertData>(alert);
    if (!d) return;
    objc::retain(alert);
    d->visible = true;
    hlog("uikit", "UIAlertView \"%s\": %s", d->title.c_str(), d->message.c_str());
    // Shown from the run loop, like UIKit presenting on the next turn.
    runloop::post([alert] {
        auto *a = objc::host<AlertData>(alert);
        if (!a) return;
        const int index = showDialog(*a);
        a->visible = false;
        hlog("uikit", "UIAlertView button %d pressed", index);
        if (a->delegate) {
            if (objc::respondsTo(a->delegate, objc::sel("alertView:clickedButtonAtIndex:")))
                objc::send(a->delegate, "alertView:clickedButtonAtIndex:", {alert, std::uint64_t(index)});
            if (objc::respondsTo(a->delegate, objc::sel("alertView:didDismissWithButtonIndex:")))
                objc::send(a->delegate, "alertView:didDismissWithButtonIndex:", {alert, std::uint64_t(index)});
        }
        objc::release(alert);
    });
}
OBJC_METHOD(UIAlertView, "dismissWithClickedButtonIndex:animated:", dismissWithClickedButtonIndex) {
    if (auto *d = objc::host<AlertData>(self())) d->visible = false;
}
