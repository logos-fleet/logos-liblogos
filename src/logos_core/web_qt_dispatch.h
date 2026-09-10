#ifndef WEB_QT_DISPATCH_H
#define WEB_QT_DISPATCH_H

#include <functional>

namespace LogosCore {

// RUN `work` WHERE THE CORE IS.
//
// The Web container's two inbound edges — a page calling out
// (WebCallRouter::dispatch) and a page dying (WebContainer::announceTermination)
// — both arrive on a thread of the BACKEND's choosing: the channel's delivery
// thread, or whatever the backend noticed the death on. Everything they then
// touch is Qt-affine, so both hop here first.
//
// POSTED, never blocking. A blocking hand-off deadlocks the pair in both
// directions: the main thread's teardown closes the view and joins the
// backend's pump thread, which would be sitting here waiting for the main
// thread; and the main thread waiting on a page cannot also be the thread that
// answers it.
//
// `QCoreApplication::instance()` is the context object, so a shutdown that
// outruns the event drops it rather than running it against a half-gone
// process.
//
// RUN INLINE when there is no QCoreApplication or when this already IS its
// thread. The second case is every gtest here, which drives the container from
// the test thread and pumps no event loop.
void runOnQtMainThread(std::function<void()> work);

} // namespace LogosCore

#endif // WEB_QT_DISPATCH_H
