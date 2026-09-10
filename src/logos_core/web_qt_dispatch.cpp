#include "web_qt_dispatch.h"

#include <QCoreApplication>
#include <QMetaObject>
#include <QThread>

#include <utility>

namespace LogosCore {

void runOnQtMainThread(std::function<void()> work)
{
    QCoreApplication* app = QCoreApplication::instance();
    if (app && QThread::currentThread() != app->thread()) {
        QMetaObject::invokeMethod(app, std::move(work), Qt::QueuedConnection);
        return;
    }
    work();
}

} // namespace LogosCore
