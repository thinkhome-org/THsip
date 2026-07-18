#pragma once

#include <QString>

namespace SecretStore {
bool save(const QString &account, const QString &secret);
QString load(const QString &account);
}
