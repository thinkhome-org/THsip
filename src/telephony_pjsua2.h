#pragma once

#include <QObject>
#include <QVariantList>
#include <QVariantMap>
#include <memory>

class TelephonyEngine final : public QObject {
    Q_OBJECT
public:
    class Private;
    explicit TelephonyEngine(QObject *parent = nullptr);
    ~TelephonyEngine() override;
    Q_INVOKABLE QString addAccount(const QVariantMap &configuration);
    Q_INVOKABLE QString makeCall(const QString &accountId, const QString &destination, bool video = false);
    Q_INVOKABLE void answer(const QString &callId, int code = 200);
    Q_INVOKABLE void hangup(const QString &callId);
    Q_INVOKABLE void hold(const QString &callId, bool enabled);
    Q_INVOKABLE void mute(const QString &callId, bool enabled);
    Q_INVOKABLE void transfer(const QString &callId, const QString &destination);
    Q_INVOKABLE void attendedTransfer(const QString &callId, const QString &consultationCallId);
    Q_INVOKABLE void sendDtmf(const QString &callId, const QString &digits);
    Q_INVOKABLE void sendMessage(const QString &accountId, const QString &destination, const QString &message);
    Q_INVOKABLE void sendTyping(const QString &accountId, const QString &destination, bool typing);
    Q_INVOKABLE QString subscribeBuddy(const QString &accountId, const QString &destination, bool dialogEvent = true);
    Q_INVOKABLE void unsubscribeBuddy(const QString &buddyId);
    Q_INVOKABLE QString startRecording(const QString &callId);
    Q_INVOKABLE void stopRecording(const QString &callId);
    Q_INVOKABLE void conference(const QString &firstCallId, const QString &secondCallId, bool enabled = true);
    Q_INVOKABLE QVariantList audioDevices();
    Q_INVOKABLE QVariantList videoDevices();
    Q_INVOKABLE void setAudioDevices(int captureId, int playbackId);
    Q_INVOKABLE QVariantList codecs();
    Q_INVOKABLE void setCodecPriority(const QString &codecId, int priority);
    Q_INVOKABLE QVariantList mediaStatistics(const QString &callId);
signals:
    void accountState(QString accountId, bool registered, QString reason);
    void callState(QString callId, QVariantMap state);
    void incomingCall(QString callId, QVariantMap state);
    void messageReceived(QString accountId, QString from, QString contentType, QString message);
    void messageStatus(QString accountId, QString destination, int code, QString reason);
    void buddyState(QString buddyId, QVariantMap state);
    void recordingState(QString callId, bool recording, QString path);
    void error(QString operation, QString message);
private:
    std::unique_ptr<Private> d;
};
