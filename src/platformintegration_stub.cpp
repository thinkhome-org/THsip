#include "platformintegration.h"

PlatformIntegration::PlatformIntegration(QObject *parent) : QObject(parent) {}
PlatformIntegration::~PlatformIntegration() = default;
void PlatformIntegration::requestPermissions() {}
void PlatformIntegration::showIncomingCall(const QString &) {}
void PlatformIntegration::clearIncomingCall() {}
void PlatformIntegration::preventSleepDuringCall(bool) {}
