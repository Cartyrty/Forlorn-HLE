// The app's online services, behaving as on a device without a network connection: NSURLConnection requests fail
// (the delegate gets connection:didFailWithError: from the run loop), StoreKit has no products and refuses payments.
#include "foundation/Foundation.h"
#include "foundation/RunLoop.h"
#include "Log.h"
#include "Memory.h"

using namespace ns;
using objc::marg;
using objc::self;
using hle::ret;

namespace {
id offlineError() {
    // NSURLErrorDomain / NSURLErrorNotConnectedToInternet (-1009).
    return objc::send(objc::getClass("NSError"), "errorWithDomain:code:userInfo:", {string("NSURLErrorDomain"), std::uint32_t(-1009), 0});
}
void failLater(id connection, id delegate) {
    objc::retain(connection);
    objc::retain(delegate);
    runloop::post([connection, delegate] {
        if (delegate && objc::respondsTo(delegate, objc::sel("connection:didFailWithError:")))
            objc::send(delegate, "connection:didFailWithError:", {connection, offlineError()});
    });
}
std::string requestUrl(id request) { return request ? describe(objc::send(request, "URL")) : std::string("(nil)"); }
} // namespace

OBJC_CLASS(NSURLConnection, NSObject);
OBJC_CLASS(NSHTTPURLResponse, NSObject);
OBJC_CLASS_METHOD(NSURLConnection, "connectionWithRequest:delegate:", connectionWithRequest) {
    hlog("network", "offline: %s", requestUrl(marg(0)).c_str());
    const auto c = objc::allocInstance(self());
    failLater(c, marg(1));
    ret(c);
}
OBJC_METHOD(NSURLConnection, "initWithRequest:delegate:", initWithRequest) {
    hlog("network", "offline: %s", requestUrl(marg(0)).c_str());
    failLater(self(), marg(1));
    ret(self());
}
OBJC_METHOD(NSURLConnection, "initWithRequest:delegate:startImmediately:", initWithRequestStart) {
    hlog("network", "offline: %s", requestUrl(marg(0)).c_str());
    failLater(self(), marg(1));
    ret(self());
}
OBJC_METHOD(NSURLConnection, "start", start) {}
OBJC_METHOD(NSURLConnection, "cancel", cancel) {}
OBJC_METHOD(NSURLConnection, "scheduleInRunLoop:forMode:", scheduleInRunLoop) {}
OBJC_METHOD(NSURLConnection, "unscheduleFromRunLoop:forMode:", unscheduleFromRunLoop) {}
// +sendSynchronousRequest:returningResponse:error: -> nil with the error.
OBJC_CLASS_METHOD(NSURLConnection, "sendSynchronousRequest:returningResponse:error:", sendSynchronousRequest) {
    hlog("network", "offline (sync): %s", requestUrl(marg(0)).c_str());
    if (marg(1)) mem::wp(marg(1), 0);
    if (marg(2)) mem::wp(marg(2), offlineError());
    ret(0);
}
OBJC_CLASS_METHOD(NSURLConnection, "canHandleRequest:", canHandleRequest) { ret(1); }
OBJC_METHOD(NSHTTPURLResponse, "statusCode", statusCode) { ret(0); }

// StoreKit: payments are not allowed; product requests come back empty.
OBJC_CLASS(SKPaymentQueue, NSObject);
OBJC_CLASS(SKPayment, NSObject);
OBJC_CLASS(SKProductsRequest, NSObject);
OBJC_CLASS(SKProductsResponse, NSObject);
static id g_paymentQueue = 0;
OBJC_CLASS_METHOD(SKPaymentQueue, "defaultQueue", defaultQueue) {
    if (!g_paymentQueue) { g_paymentQueue = objc::allocInstance(self()); objc::retain(g_paymentQueue); }
    ret(g_paymentQueue);
}
OBJC_CLASS_METHOD(SKPaymentQueue, "canMakePayments", canMakePayments) { ret(0); }
OBJC_METHOD(SKPaymentQueue, "addTransactionObserver:", addTransactionObserver) {}
OBJC_METHOD(SKPaymentQueue, "removeTransactionObserver:", removeTransactionObserver) {}
OBJC_METHOD(SKPaymentQueue, "addPayment:", addPayment) { hlog("storekit", "payment refused (offline)"); }
OBJC_METHOD(SKPaymentQueue, "restoreCompletedTransactions", restoreCompletedTransactions) {}
OBJC_METHOD(SKPaymentQueue, "finishTransaction:", finishTransaction) {}
OBJC_METHOD(SKPaymentQueue, "transactions", transactions) { ret(array({})); }
OBJC_CLASS_METHOD(SKPayment, "paymentWithProductIdentifier:", paymentWithProductIdentifier) { ret(objc::allocInstance(self())); }
OBJC_CLASS_METHOD(SKPayment, "paymentWithProduct:", paymentWithProduct) { ret(objc::allocInstance(self())); }
struct ProductsRequest : objc::HostObject { id delegate = 0; };
OBJC_METHOD(SKProductsRequest, "initWithProductIdentifiers:", initWithProductIdentifiers) {
    objc::setHostObject(self(), std::make_unique<ProductsRequest>());
    ret(self());
}
OBJC_METHOD(SKProductsRequest, "setDelegate:", setDelegate) { if (auto *d = objc::host<ProductsRequest>(self())) d->delegate = marg(0); }
OBJC_METHOD(SKProductsRequest, "delegate", delegate) { auto *d = objc::host<ProductsRequest>(self()); ret(d ? d->delegate : 0); }
OBJC_METHOD(SKProductsRequest, "start", start) {
    auto *d = objc::host<ProductsRequest>(self());
    const auto request = self(), delegate = d ? d->delegate : 0;
    objc::retain(request);
    runloop::post([request, delegate] {
        if (!delegate) return;
        if (objc::respondsTo(delegate, objc::sel("request:didFailWithError:")))
            objc::send(delegate, "request:didFailWithError:", {request, offlineError()});
        else if (objc::respondsTo(delegate, objc::sel("productsRequest:didReceiveResponse:")))
            objc::send(delegate, "productsRequest:didReceiveResponse:", {request, objc::allocInstance(objc::getClass("SKProductsResponse"))});
    });
}
OBJC_METHOD(SKProductsRequest, "cancel", cancel) {}
OBJC_METHOD(SKProductsResponse, "products", products) { ret(array({})); }
OBJC_METHOD(SKProductsResponse, "invalidProductIdentifiers", invalidProductIdentifiers) { ret(array({})); }

// The ivar offset of NSObject's isa (always 0).
static hle::DataRegistrar d_isaIvar("_OBJC_IVAR_$_NSObject.isa", [] { return mem::hleAlloc(4); });
static hle::DataRegistrar d_emptyVtable("__objc_empty_vtable", [] { return mem::hleAlloc(16); });
