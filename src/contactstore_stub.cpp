#include "contactstore.h"

#include <QMetaObject>

void requestSystemContacts(QObject *context, ContactStoreCallback callback)
{
    QMetaObject::invokeMethod(context, [callback = std::move(callback)] {
        callback({}, QStringLiteral("Systémové kontakty zatím podporuje macOS"));
    }, Qt::QueuedConnection);
}
