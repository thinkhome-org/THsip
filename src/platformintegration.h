#pragma once

#include <QObject>

class IPlatformIntegration {
public:
    virtual ~IPlatformIntegration() = default;
    virtual void requestPermissions() = 0;
    virtual void showIncomingCall(const QString &caller) = 0;
    virtual void clearIncomingCall() = 0;
    virtual void preventSleepDuringCall(bool enabled) = 0;
};

class PlatformIntegration final : public QObject, public IPlatformIntegration {
    Q_OBJECT
public:
    explicit PlatformIntegration(QObject *parent = nullptr);
    ~PlatformIntegration() override;
    Q_INVOKABLE void requestPermissions() override;
    Q_INVOKABLE void showIncomingCall(const QString &caller) override;
    Q_INVOKABLE void clearIncomingCall() override;
    Q_INVOKABLE void preventSleepDuringCall(bool enabled) override;
private:
    quint32 sleepAssertion_ = 0;
};
