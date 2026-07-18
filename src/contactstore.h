#pragma once

#include <functional>
#include <QString>
#include <QVariantList>

class QObject;

using ContactStoreCallback = std::function<void(QVariantList, QString)>;
void requestSystemContacts(QObject *context, ContactStoreCallback callback);
