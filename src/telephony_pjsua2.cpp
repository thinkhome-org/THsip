#include "telephony_pjsua2.h"

#include <QDateTime>
#include <QDir>
#include <QMetaObject>
#include <QStandardPaths>
#include <pjsua2.hpp>
#include <pjsua-lib/pjsua.h>
#include <atomic>
#include <algorithm>
#include <cmath>
#include <mutex>
#include <thread>
#include <unordered_map>

namespace {
QString nextId(const char *prefix)
{
    static std::atomic_uint64_t value{1};
    return QStringLiteral("%1-%2").arg(QLatin1String(prefix)).arg(value++);
}

QString sipUri(QString destination)
{
    destination = destination.trimmed();
    if (!destination.startsWith(QLatin1String("sip:"), Qt::CaseInsensitive) &&
        !destination.startsWith(QLatin1String("sips:"), Qt::CaseInsensitive))
        destination = QStringLiteral("sip:%1@sip.odorik.cz").arg(destination);
    return destination;
}

QString pjString(const pj_str_t &value)
{
    return QString::fromUtf8(value.ptr, qsizetype(value.slen));
}
}

class ThsipCall final : public pj::Call {
public:
    ThsipCall(pj::Account &account, int id, QString publicId, TelephonyEngine::Private *owner)
        : pj::Call(account, id), publicId(std::move(publicId)), owner(owner) {}
    void onCallState(pj::OnCallStateParam &) override;
    void onCallMediaState(pj::OnCallMediaStateParam &) override;
    QString publicId;
    TelephonyEngine::Private *owner;
};

class ThsipAccount final : public pj::Account {
public:
    ThsipAccount(QString publicId, TelephonyEngine::Private *owner) : publicId(std::move(publicId)), owner(owner) {}
    void onRegState(pj::OnRegStateParam &) override;
    void onIncomingCall(pj::OnIncomingCallParam &param) override;
    void onInstantMessage(pj::OnInstantMessageParam &param) override;
    void onInstantMessageStatus(pj::OnInstantMessageStatusParam &param) override;
    QString publicId;
    TelephonyEngine::Private *owner;
};

class ThsipBuddy final : public pj::Buddy {
public:
    ThsipBuddy(QString publicId, QString accountId, TelephonyEngine::Private *owner)
        : publicId(std::move(publicId)), accountId(std::move(accountId)), owner(owner) {}
    void onBuddyState() override;
    void onBuddyDlgEventState() override;
    QString publicId;
    QString accountId;
    TelephonyEngine::Private *owner;
};

class TelephonyEngine::Private {
public:
    explicit Private(TelephonyEngine *q) : q(q)
    {
        endpoint.libCreate();
        pj::EpConfig config;
        config.uaConfig.threadCnt = 0;
        config.uaConfig.mainThreadOnly = false;
        config.logConfig.level = 4;
        endpoint.libInit(config);
        pj::TransportConfig udp; udp.port = 0;
        endpoint.transportCreate(PJSIP_TRANSPORT_UDP, udp);
        pj::TransportConfig tcp; tcp.port = 0;
        endpoint.transportCreate(PJSIP_TRANSPORT_TCP, tcp);
        try { pj::TransportConfig tls; tls.port = 0; endpoint.transportCreate(PJSIP_TRANSPORT_TLS, tls); }
        catch (const pj::Error &) { /* TLS availability reported by registration failure. */ }
        endpoint.libStart();
        worker = std::jthread([this](std::stop_token stop) {
            endpoint.libRegisterThread("thsip-pjsip");
            while (!stop.stop_requested()) endpoint.libHandleEvents(20);
        });
    }
    ~Private()
    {
        worker.request_stop();
        worker.join();
        std::scoped_lock lock(mutex);
        recorders.clear(); calls.clear(); buddies.clear(); accounts.clear();
        endpoint.libDestroy();
    }
    template<class Function> void protect(const QString &operation, Function function)
    {
        try { std::scoped_lock lock(mutex); endpoint.libRegisterThread("thsip-qt"); function(); }
        catch (const pj::Error &e) { emit q->error(operation, QString::fromStdString(e.info())); }
    }
    ThsipCall *call(const QString &id)
    {
        auto it = calls.find(id.toStdString());
        if (it == calls.end()) throw pj::Error(PJ_ENOTFOUND, "call", "not found", __FILE__, __LINE__);
        return it->second.get();
    }
    ThsipAccount *account(const QString &id)
    {
        auto it = accounts.find(id.toStdString());
        if (it == accounts.end()) throw pj::Error(PJ_ENOTFOUND, "account", "not found", __FILE__, __LINE__);
        return it->second.get();
    }
    void publish(ThsipCall &call)
    {
        const pj::CallInfo info = call.getInfo();
        QVariantMap state{{"remoteUri", QString::fromStdString(info.remoteUri)}, {"state", QString::fromStdString(info.stateText)}, {"lastCode", info.lastStatusCode}, {"mediaCount", int(info.media.size())}};
        QMetaObject::invokeMethod(q, [q=q, id=call.publicId, state] { emit q->callState(id, state); }, Qt::QueuedConnection);
    }
    void publish(ThsipBuddy &buddy, bool dialog)
    {
        const pj::BuddyInfo info = buddy.getInfo();
        QVariantMap state{{"uri", QString::fromStdString(info.uri)},
                          {"subscription", QString::fromStdString(info.subStateName)},
                          {"online", info.presStatus.status == PJSUA_BUDDY_STATUS_ONLINE},
                          {"status", QString::fromStdString(info.presStatus.statusText)},
                          {"dialog", dialog}};
        if (dialog) {
            pjsua_buddy_dlg_event_info event{};
            if (pjsua_buddy_get_dlg_event_info(buddy.getId(), &event) == PJ_SUCCESS) {
                state.insert(QStringLiteral("subscription"), QString::fromUtf8(event.sub_state_name ? event.sub_state_name : ""));
                state.insert(QStringLiteral("dialogState"), pjString(event.dialog_state));
                state.insert(QStringLiteral("remote"), pjString(event.remote_identity));
                state.insert(QStringLiteral("direction"), pjString(event.dialog_direction));
            }
        }
        QMetaObject::invokeMethod(q, [q=q, id=buddy.publicId, state] { emit q->buddyState(id, state); }, Qt::QueuedConnection);
    }
    TelephonyEngine *q;
    pj::Endpoint endpoint;
    std::jthread worker;
    std::recursive_mutex mutex;
    std::unordered_map<std::string, std::unique_ptr<ThsipAccount>> accounts;
    std::unordered_map<std::string, std::unique_ptr<ThsipCall>> calls;
    std::unordered_map<std::string, std::unique_ptr<ThsipBuddy>> buddies;
    std::unordered_map<std::string, std::unique_ptr<pj::AudioMediaRecorder>> recorders;
};

void ThsipCall::onCallState(pj::OnCallStateParam &)
{
    const bool disconnected = getInfo().state == PJSIP_INV_STATE_DISCONNECTED;
    owner->publish(*this);
    if (!disconnected) return;
    QMetaObject::invokeMethod(owner->q, [owner=owner, id=publicId] {
        std::scoped_lock lock(owner->mutex);
        if (owner->recorders.erase(id.toStdString())) emit owner->q->recordingState(id, false, QString());
        owner->calls.erase(id.toStdString());
    }, Qt::QueuedConnection);
}
void ThsipCall::onCallMediaState(pj::OnCallMediaStateParam &)
{
    const pj::CallInfo info = getInfo();
    for (unsigned i = 0; i < info.media.size(); ++i) {
        if (info.media[i].type != PJMEDIA_TYPE_AUDIO || !getMedia(i)) continue;
        pj::AudioMedia audio = getAudioMedia(i);
        audio.startTransmit(owner->endpoint.audDevManager().getPlaybackDevMedia());
        owner->endpoint.audDevManager().getCaptureDevMedia().startTransmit(audio);
    }
    owner->publish(*this);
}
void ThsipAccount::onRegState(pj::OnRegStateParam &param)
{
    const pj::AccountInfo info = getInfo();
    QMetaObject::invokeMethod(owner->q, [q=owner->q,id=publicId,active=info.regIsActive,reason=QString::fromStdString(param.reason)] { emit q->accountState(id, active, reason); }, Qt::QueuedConnection);
}
void ThsipAccount::onIncomingCall(pj::OnIncomingCallParam &param)
{
    std::scoped_lock lock(owner->mutex);
    const QString id = nextId("call");
    auto call = std::make_unique<ThsipCall>(*this, param.callId, id, owner);
    QVariantMap state{{"remoteUri", QString::fromStdString(call->getInfo().remoteUri)}, {"incoming", true}};
    owner->calls.emplace(id.toStdString(), std::move(call));
    QMetaObject::invokeMethod(owner->q, [q=owner->q,id,state] { emit q->incomingCall(id, state); }, Qt::QueuedConnection);
}
void ThsipAccount::onInstantMessage(pj::OnInstantMessageParam &param)
{
    QMetaObject::invokeMethod(owner->q, [q=owner->q, id=publicId,
                              from=QString::fromStdString(param.fromUri),
                              type=QString::fromStdString(param.contentType),
                              body=QString::fromStdString(param.msgBody)] {
        emit q->messageReceived(id, from, type, body);
    }, Qt::QueuedConnection);
}
void ThsipAccount::onInstantMessageStatus(pj::OnInstantMessageStatusParam &param)
{
    QMetaObject::invokeMethod(owner->q, [q=owner->q, id=publicId,
                              to=QString::fromStdString(param.toUri), code=int(param.code),
                              reason=QString::fromStdString(param.reason)] {
        emit q->messageStatus(id, to, code, reason);
    }, Qt::QueuedConnection);
}
void ThsipBuddy::onBuddyState() { owner->publish(*this, false); }
void ThsipBuddy::onBuddyDlgEventState() { owner->publish(*this, true); }

TelephonyEngine::TelephonyEngine(QObject *parent) : QObject(parent)
{
    try { d = std::make_unique<Private>(this); }
    catch (const pj::Error &e) { emit error(QStringLiteral("initialize"), QString::fromStdString(e.info())); }
}
TelephonyEngine::~TelephonyEngine() = default;

QString TelephonyEngine::addAccount(const QVariantMap &c)
{
    const QString id = nextId("account");
    if (!d) return {};
    d->protect(QStringLiteral("addAccount"), [&] {
        pj::AccountConfig config;
        config.idUri = c.value("idUri").toString().toStdString();
        config.regConfig.registrarUri = c.value("registrar", "sip:sip.odorik.cz").toString().toStdString();
        config.sipConfig.authCreds.emplace_back("digest", "*", c.value("user").toString().toStdString(), 0, c.value("password").toString().toStdString());
        config.mediaConfig.srtpUse = c.value("srtp", true).toBool() ? PJMEDIA_SRTP_OPTIONAL : PJMEDIA_SRTP_DISABLED;
        config.mediaConfig.srtpSecureSignaling = c.value("srtp", true).toBool() ? 1 : 0;
        config.natConfig.iceEnabled = c.value("ice", true).toBool();
        const QString stun = c.value(QStringLiteral("stunServer")).toString().trimmed();
        if (!stun.isEmpty()) d->endpoint.natUpdateStunServers({stun.toStdString()}, true);
        const QString turn = c.value(QStringLiteral("turnServer")).toString().trimmed();
        if (!turn.isEmpty()) {
            config.natConfig.turnEnabled = true;
            config.natConfig.turnServer = turn.toStdString();
            config.natConfig.turnUserName = c.value(QStringLiteral("turnUser")).toString().toStdString();
            config.natConfig.turnPassword = c.value(QStringLiteral("turnPassword")).toString().toStdString();
        }
        if (c.contains(QStringLiteral("videoDevice"))) config.videoConfig.defaultCaptureDevice = c.value(QStringLiteral("videoDevice")).toInt();
        auto account = std::make_unique<ThsipAccount>(id, d.get());
        account->create(config);
        d->accounts.emplace(id.toStdString(), std::move(account));
    });
    return id;
}

QString TelephonyEngine::makeCall(const QString &accountId, const QString &destination, bool video)
{
    const QString id = nextId("call");
    if (!d) return {};
    d->protect(QStringLiteral("makeCall"), [&] {
        auto *account = d->account(accountId);
        auto call = std::make_unique<ThsipCall>(*account, PJSUA_INVALID_ID, id, d.get());
        pj::CallOpParam operation(true); operation.opt.videoCount = video ? 1 : 0;
        const QString uri = sipUri(destination);
        call->makeCall(uri.toStdString(), operation);
        d->calls.emplace(id.toStdString(), std::move(call));
    });
    return id;
}

void TelephonyEngine::answer(const QString &id, int code) { if (d) d->protect("answer", [&]{ pj::CallOpParam p; p.statusCode = pjsip_status_code(code); d->call(id)->answer(p); }); }
void TelephonyEngine::hangup(const QString &id) { if (d) d->protect("hangup", [&]{ pj::CallOpParam p; p.statusCode = PJSIP_SC_DECLINE; d->call(id)->hangup(p); }); }
void TelephonyEngine::hold(const QString &id, bool enabled) { if (d) d->protect("hold", [&]{ pj::CallOpParam p; enabled ? d->call(id)->setHold(p) : d->call(id)->reinvite(p); }); }
void TelephonyEngine::mute(const QString &id, bool enabled)
{
    if (!d) return;
    d->protect(QStringLiteral("mute"), [&] {
        auto *call = d->call(id);
        const pj::CallInfo info = call->getInfo();
        for (unsigned i = 0; i < info.media.size(); ++i) {
            if (info.media[i].type != PJMEDIA_TYPE_AUDIO || !call->getMedia(i)) continue;
            pj::AudioMedia audio = call->getAudioMedia(i);
            enabled ? d->endpoint.audDevManager().getCaptureDevMedia().stopTransmit(audio)
                    : d->endpoint.audDevManager().getCaptureDevMedia().startTransmit(audio);
        }
    });
}
void TelephonyEngine::transfer(const QString &id, const QString &destination) { if (d) d->protect("transfer", [&]{ pj::CallOpParam p; d->call(id)->xfer(destination.toStdString(), p); }); }
void TelephonyEngine::attendedTransfer(const QString &id, const QString &consultationId) { if (d) d->protect("attendedTransfer", [&]{ pj::CallOpParam p; d->call(id)->xferReplaces(*d->call(consultationId), p); }); }
void TelephonyEngine::sendDtmf(const QString &id, const QString &digits) { if (d) d->protect("dtmf", [&]{ pj::CallSendDtmfParam p; p.digits = digits.toStdString(); d->call(id)->sendDtmf(p); }); }

QString TelephonyEngine::subscribeBuddy(const QString &accountId, const QString &destination, bool dialogEvent)
{
    const QString id = nextId("buddy");
    if (!d || destination.trimmed().isEmpty()) return {};
    d->protect(QStringLiteral("subscribeBuddy"), [&] {
        pj::BuddyConfig config;
        config.uri = sipUri(destination).toStdString();
        config.subscribe = !dialogEvent;
        config.subscribe_dlg_event = dialogEvent;
        auto buddy = std::make_unique<ThsipBuddy>(id, accountId, d.get());
        buddy->create(*d->account(accountId), config);
        d->buddies.emplace(id.toStdString(), std::move(buddy));
    });
    return id;
}

void TelephonyEngine::unsubscribeBuddy(const QString &id)
{
    if (d) d->protect(QStringLiteral("unsubscribeBuddy"), [&] {
        if (!d->buddies.erase(id.toStdString())) throw pj::Error(PJ_ENOTFOUND, "buddy", "not found", __FILE__, __LINE__);
    });
}

void TelephonyEngine::sendMessage(const QString &accountId, const QString &destination, const QString &message)
{
    if (!d || message.isEmpty()) { emit error(QStringLiteral("sendMessage"), QStringLiteral("message is empty")); return; }
    d->protect(QStringLiteral("sendMessage"), [&] {
        ThsipBuddy *buddy = nullptr;
        const QString uri = sipUri(destination);
        for (auto &[_, candidate] : d->buddies)
            if (candidate->accountId == accountId && QString::fromStdString(candidate->getInfo().uri) == uri) { buddy = candidate.get(); break; }
        if (!buddy) {
            const QString id = nextId("buddy");
            pj::BuddyConfig config; config.uri = uri.toStdString();
            auto created = std::make_unique<ThsipBuddy>(id, accountId, d.get());
            created->create(*d->account(accountId), config);
            buddy = created.get();
            d->buddies.emplace(id.toStdString(), std::move(created));
        }
        pj::SendInstantMessageParam messageParam;
        messageParam.contentType = "text/plain; charset=utf-8";
        messageParam.content = message.toStdString();
        buddy->sendInstantMessage(messageParam);
    });
}

void TelephonyEngine::sendTyping(const QString &accountId, const QString &destination, bool typing)
{
    if (!d) return;
    d->protect(QStringLiteral("sendTyping"), [&] {
        const QString uri = sipUri(destination);
        for (auto &[_, buddy] : d->buddies) {
            if (buddy->accountId != accountId || QString::fromStdString(buddy->getInfo().uri) != uri) continue;
            pj::SendTypingIndicationParam param; param.isTyping = typing;
            buddy->sendTypingIndication(param);
            return;
        }
        throw pj::Error(PJ_ENOTFOUND, "buddy", "send a message or subscribe first", __FILE__, __LINE__);
    });
}

QString TelephonyEngine::startRecording(const QString &id)
{
    if (!d) return {};
    QString path;
    d->protect(QStringLiteral("startRecording"), [&] {
        if (d->recorders.contains(id.toStdString())) throw pj::Error(PJ_EEXISTS, "recording", "already recording", __FILE__, __LINE__);
        const QString directory = QStandardPaths::writableLocation(QStandardPaths::AppDataLocation) + QStringLiteral("/recordings");
        if (!QDir().mkpath(directory)) throw pj::Error(PJ_EUNKNOWN, "recording", "cannot create directory", __FILE__, __LINE__);
        path = directory + QStringLiteral("/%1-%2.wav").arg(QDateTime::currentDateTimeUtc().toString(QStringLiteral("yyyyMMdd-HHmmsszzz")), id);
        auto recorder = std::make_unique<pj::AudioMediaRecorder>();
        recorder->createRecorder(path.toStdString());
        auto *call = d->call(id);
        const pj::CallInfo info = call->getInfo();
        for (unsigned i = 0; i < info.media.size(); ++i) {
            if (info.media[i].type != PJMEDIA_TYPE_AUDIO || !call->getMedia(i)) continue;
            call->getAudioMedia(i).startTransmit(*recorder);
            d->endpoint.audDevManager().getCaptureDevMedia().startTransmit(*recorder);
        }
        d->recorders.emplace(id.toStdString(), std::move(recorder));
        emit recordingState(id, true, path);
    });
    return path;
}

void TelephonyEngine::stopRecording(const QString &id)
{
    if (!d) return;
    d->protect(QStringLiteral("stopRecording"), [&] {
        if (!d->recorders.erase(id.toStdString())) throw pj::Error(PJ_ENOTFOUND, "recording", "not recording", __FILE__, __LINE__);
        emit recordingState(id, false, QString());
    });
}

void TelephonyEngine::conference(const QString &firstId, const QString &secondId, bool enabled)
{
    if (!d) return;
    d->protect(QStringLiteral("conference"), [&] {
        auto *first = d->call(firstId);
        auto *second = d->call(secondId);
        pj::AudioMedia firstAudio = first->getAudioMedia(-1);
        pj::AudioMedia secondAudio = second->getAudioMedia(-1);
        if (enabled) { firstAudio.startTransmit(secondAudio); secondAudio.startTransmit(firstAudio); }
        else { firstAudio.stopTransmit(secondAudio); secondAudio.stopTransmit(firstAudio); }
    });
}

QVariantList TelephonyEngine::audioDevices()
{
    QVariantList result;
    if (!d) return result;
    d->protect(QStringLiteral("audioDevices"), [&] {
        const int capture = d->endpoint.audDevManager().getCaptureDev();
        const int playback = d->endpoint.audDevManager().getPlaybackDev();
        for (const pj::AudioDevInfo &device : d->endpoint.audDevManager().enumDev2())
            result << QVariantMap{{"id", device.id}, {"name", QString::fromStdString(device.name)},
                                  {"driver", QString::fromStdString(device.driver)}, {"inputs", int(device.inputCount)},
                                  {"outputs", int(device.outputCount)}, {"sampleRate", int(device.defaultSamplesPerSec)},
                                  {"capture", device.id == capture}, {"playback", device.id == playback}};
    });
    return result;
}

QVariantList TelephonyEngine::videoDevices()
{
    QVariantList result;
    if (!d) return result;
    d->protect(QStringLiteral("videoDevices"), [&] {
        for (const pj::VideoDevInfo &device : d->endpoint.vidDevManager().enumDev2())
            result << QVariantMap{{"id", device.id}, {"name", QString::fromStdString(device.name)},
                                  {"driver", QString::fromStdString(device.driver)}, {"direction", int(device.dir)}};
    });
    return result;
}

void TelephonyEngine::setAudioDevices(int captureId, int playbackId)
{
    if (d) d->protect(QStringLiteral("setAudioDevices"), [&] {
        d->endpoint.audDevManager().setCaptureDev(captureId);
        d->endpoint.audDevManager().setPlaybackDev(playbackId);
    });
}

QVariantList TelephonyEngine::codecs()
{
    QVariantList result;
    if (!d) return result;
    d->protect(QStringLiteral("codecs"), [&] {
        for (const pj::CodecInfo &codec : d->endpoint.codecEnum2())
            result << QVariantMap{{"id", QString::fromStdString(codec.codecId)}, {"priority", int(codec.priority)}, {"description", QString::fromStdString(codec.desc)}};
    });
    return result;
}

void TelephonyEngine::setCodecPriority(const QString &codecId, int priority)
{
    if (!d || priority < 0 || priority > 255) { emit error(QStringLiteral("setCodecPriority"), QStringLiteral("priority must be 0-255")); return; }
    d->protect(QStringLiteral("setCodecPriority"), [&] { d->endpoint.codecSetPriority(codecId.toStdString(), pj_uint8_t(priority)); });
}

QVariantList TelephonyEngine::mediaStatistics(const QString &id)
{
    QVariantList result;
    if (!d) return result;
    d->protect(QStringLiteral("mediaStatistics"), [&] {
        auto *call = d->call(id);
        const pj::CallInfo callInfo = call->getInfo();
        for (unsigned i = 0; i < callInfo.media.size(); ++i) {
            if (callInfo.media[i].status == PJSUA_CALL_MEDIA_NONE) continue;
            const pj::StreamInfo info = call->getStreamInfo(i);
            const pj::StreamStat stat = call->getStreamStat(i);
            const double received = stat.rtcp.rxStat.pkt + stat.rtcp.rxStat.loss;
            const double lossPercent = received > 0 ? stat.rtcp.rxStat.loss * 100.0 / received : 0.0;
            const double jitterMs = stat.rtcp.rxStat.jitterUsec.mean / 1000.0;
            const double rttMs = stat.rtcp.rttUsec.mean / 1000.0;
            const double seconds = std::max(1L, callInfo.connectDuration.sec);
            const double bitrate = (stat.rtcp.rxStat.bytes + stat.rtcp.txStat.bytes) * 8.0 / seconds;
            // ponytail: codec-neutral E-model estimate; use codec-specific impairment factors if diagnostics need lab accuracy.
            const double latency = rttMs / 2.0 + jitterMs * 2.0 + 10.0;
            const double rating = std::clamp(93.2 - (latency < 160.0 ? latency / 40.0 : (latency - 120.0) / 10.0) - lossPercent * 2.5, 0.0, 100.0);
            const double mos = std::clamp(1.0 + 0.035 * rating + 0.000007 * rating * (rating - 60.0) * (100.0 - rating), 1.0, 4.5);
            result << QVariantMap{{"index", int(i)}, {"type", int(info.type)}, {"codec", QString::fromStdString(info.codecName)},
                                  {"clockRate", int(info.codecClockRate)}, {"jitterMs", jitterMs}, {"lossPercent", lossPercent},
                                  {"rttMs", rttMs}, {"bitrate", bitrate}, {"mos", mos},
                                  {"rxPackets", int(stat.rtcp.rxStat.pkt)}, {"txPackets", int(stat.rtcp.txStat.pkt)}};
        }
    });
    return result;
}
