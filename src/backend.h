#pragma once

#include <QAbstractListModel>
#include <QJsonArray>
#include <QJsonObject>
#include <QDateTime>
#include <QNetworkAccessManager>
#include <QObject>
#include <QSqlDatabase>
#include <QUrl>
#include <QWebSocket>
#include <functional>

struct DialModifier {
    QString id;
    QVariantMap parameters;
};

class DialPlanCompiler final {
public:
    static QString compile(const QString &destination, const QVariantList &modifiers);
    static bool isProtectedAddress(const QString &destination);
};

class CapabilityCatalog final {
public:
    bool load(const QByteArray &json, QString *error = nullptr);
    QVariantList items() const;
    QStringList invalidItems() const;
private:
    QJsonArray items_;
};

class AppController final : public QObject {
    Q_OBJECT
    Q_PROPERTY(QString status READ status NOTIFY statusChanged)
    Q_PROPERTY(QString output READ output NOTIFY outputChanged)
    Q_PROPERTY(QString balance READ balance NOTIFY balanceChanged)
    Q_PROPERTY(QVariantList capabilities READ capabilities CONSTANT)
    Q_PROPERTY(QVariantList apiRecipes READ apiRecipes CONSTANT)
    Q_PROPERTY(QVariantList dialActions READ dialActions CONSTANT)
    Q_PROPERTY(QVariantList contacts READ contacts NOTIFY contactsChanged)
    Q_PROPERTY(QVariantList routingNumbers READ routingNumbers NOTIFY routingChanged)
    Q_PROPERTY(QVariantList routingRingings READ routingRingings NOTIFY routingChanged)
    Q_PROPERTY(QVariantList routingRoutes READ routingRoutes NOTIFY routingChanged)
    Q_PROPERTY(QVariantList recordings READ recordings NOTIFY recordingsChanged)
    Q_PROPERTY(QUrl recordingPlaybackUrl READ recordingPlaybackUrl NOTIFY recordingPlaybackUrlChanged)
    Q_PROPERTY(QVariantList networkDiagnostics READ networkDiagnostics NOTIFY networkDiagnosticsChanged)
    Q_PROPERTY(QVariantMap dashboard READ dashboard NOTIFY dashboardChanged)
    Q_PROPERTY(QVariantList calls READ calls NOTIFY callsChanged)
    Q_PROPERTY(QVariantList activeCalls READ activeCalls NOTIFY callsChanged)
    Q_PROPERTY(int callPage READ callPage NOTIFY callsChanged)
    Q_PROPERTY(int callPages READ callPages NOTIFY callsChanged)
    Q_PROPERTY(QVariantList smsRecords READ smsRecords NOTIFY smsChanged)
    Q_PROPERTY(QStringList smsSenders READ smsSenders NOTIFY smsChanged)
    Q_PROPERTY(QVariantList smsTemplates READ smsTemplates NOTIFY smsChanged)
    Q_PROPERTY(QVariantList simCards READ simCards NOTIFY simChanged)
    Q_PROPERTY(QVariantMap simDetail READ simDetail NOTIFY simChanged)
    Q_PROPERTY(QVariantList simData READ simData NOTIFY simChanged)
    Q_PROPERTY(QVariantList bridgeEvents READ bridgeEvents NOTIFY bridgeChanged)
    Q_PROPERTY(QString ivrScript READ ivrScript NOTIFY bridgeChanged)
    Q_PROPERTY(bool bridgeLive READ bridgeLive NOTIFY bridgeChanged)
    Q_PROPERTY(bool webEngineAvailable READ webEngineAvailable CONSTANT)
    Q_PROPERTY(bool pjsipAvailable READ pjsipAvailable CONSTANT)

public:
    explicit AppController(QObject *parent = nullptr);
    ~AppController() override;

    QString status() const { return status_; }
    QString output() const { return output_; }
    QString balance() const { return balance_; }
    QVariantList capabilities() const { return catalog_.items(); }
    QVariantList apiRecipes() const { return apiRecipes_.toVariantList(); }
    QVariantList dialActions() const { return dialActions_.toVariantList(); }
    QVariantList contacts() const { return contacts_; }
    QVariantList routingNumbers() const { return routingNumbers_; }
    QVariantList routingRingings() const { return routingRingings_; }
    QVariantList routingRoutes() const { return routingRoutes_; }
    QVariantList recordings() const { return recordings_; }
    QUrl recordingPlaybackUrl() const { return recordingPlaybackUrl_; }
    QVariantList networkDiagnostics() const { return networkDiagnostics_; }
    QVariantMap dashboard() const { return dashboard_; }
    QVariantList calls() const { return calls_; }
    QVariantList activeCalls() const { return activeCalls_; }
    int callPage() const { return callPage_; }
    int callPages() const { return callPages_; }
    QVariantList smsRecords() const { return smsRecords_; }
    QStringList smsSenders() const { return smsSenders_; }
    QVariantList smsTemplates() const { return smsTemplates_; }
    QVariantList simCards() const { return simCards_; }
    QVariantMap simDetail() const { return simDetail_; }
    QVariantList simData() const { return simData_; }
    QVariantList bridgeEvents() const { return bridgeEvents_; }
    QString ivrScript() const { return ivrScript_; }
    bool bridgeLive() const { return bridgeSocket_.state() == QAbstractSocket::ConnectedState; }
    bool webEngineAvailable() const;
    bool pjsipAvailable() const;

    Q_INVOKABLE void configureAccount(const QString &user, const QString &password);
    Q_INVOKABLE bool storeSipPassword(const QString &line, const QString &password);
    Q_INVOKABLE void api(const QString &method, const QString &path, const QVariantMap &parameters = {});
    Q_INVOKABLE bool isSensitiveMutation(const QString &method, const QString &path) const;
    static QString odorikError(const QByteArray &body);
    Q_INVOKABLE QVariantMap smsInfo(const QString &message) const;
    Q_INVOKABLE QString normalizePhone(const QString &number, const QString &region = QStringLiteral("CZ")) const;
    Q_INVOKABLE void importSystemContacts();
    Q_INVOKABLE void importContacts(const QUrl &file);
    Q_INVOKABLE void refreshContacts();
    Q_INVOKABLE void refreshBalance();
    Q_INVOKABLE void refreshDashboard();
    Q_INVOKABLE void refreshCalls(const QVariantMap &filters = {});
    Q_INVOKABLE void hangupActiveCall(const QString &id, bool confirmed);
    Q_INVOKABLE void refreshSms(const QVariantMap &filters = {});
    Q_INVOKABLE void saveSmsTemplate(const QString &name, const QString &body);
    Q_INVOKABLE void deleteSmsTemplate(const QString &id);
    Q_INVOKABLE void refreshSimCards();
    Q_INVOKABLE void loadSim(const QString &mobile);
    Q_INVOKABLE void loadSimData(const QString &mobile, const QString &from = {}, const QString &to = {});
    Q_INVOKABLE void updateSim(const QString &mobile, const QVariantMap &changes, bool confirmed);
    Q_INVOKABLE void restartSimData(const QString &mobile, bool confirmed);
    Q_INVOKABLE void assignSim(const QString &mobile, const QString &iccid, const QString &pin, bool delayed, bool confirmed);
    Q_INVOKABLE void refreshBridgeEvents();
    Q_INVOKABLE void loadBridgeIvr(int slot);
    Q_INVOKABLE void saveBridgeIvr(int slot, const QString &script);
    static QVariantMap normalizeCallFilters(const QVariantMap &filters, QDateTime now = QDateTime::currentDateTime());
    static QVariantMap summarizeStatistics(const QJsonObject &statistics);
    Q_INVOKABLE QString compileDial(const QString &destination, const QVariantList &modifiers) const;
    Q_INVOKABLE QString compileDialAction(const QString &id, const QVariantMap &parameters, bool advancedEnabled) const;
    Q_INVOKABLE QVariantMap routingPreview(const QString &publicNumber, const QString &mode, const QVariantList &targets, const QVariantMap &options) const;
    Q_INVOKABLE void refreshRouting();
    Q_INVOKABLE void loadRouting(const QString &publicNumber);
    Q_INVOKABLE void addRinging(const QString &publicNumber, const QString &target);
    Q_INVOKABLE void addRoute(const QString &publicNumber, const QString &source, const QString &target, bool replaceBySource);
    Q_INVOKABLE void deleteRinging(const QString &publicNumber, const QString &target, bool confirmed);
    Q_INVOKABLE void deleteRoute(const QString &publicNumber, const QString &id, bool confirmed);
    Q_INVOKABLE void configureBridge(const QUrl &baseUrl, const QString &token);
    Q_INVOKABLE void refreshRecordings();
    Q_INVOKABLE void playRecording(const QString &id);
    Q_INVOKABLE void exportRecording(const QString &id, const QUrl &destination);
    Q_INVOKABLE void updateRecording(const QString &id, const QString &notes, const QString &retainUntil);
    Q_INVOKABLE void deleteRecording(const QString &id, bool confirmed);
    Q_INVOKABLE QVariantMap systemDiagnostics() const;
    Q_INVOKABLE void runNetworkDiagnostics();
    Q_INVOKABLE QUrl testToneUrl();
    Q_INVOKABLE QString exportDiagnostics(const QVariantMap &telephony);
    static QVariant redactDiagnostics(const QVariant &value, const QString &key = {});
    void registerLocalRecording(const QString &callId, const QString &path);
    Q_INVOKABLE QUrl portalUrl(const QString &page) const;
    Q_INVOKABLE bool isOfficialPortalUrl(const QUrl &url) const;
    Q_INVOKABLE void openExternal(const QUrl &url) const;
    Q_INVOKABLE QString databasePath() const;

signals:
    void statusChanged();
    void outputChanged();
    void balanceChanged();
    void contactsChanged();
    void routingChanged();
    void recordingsChanged();
    void recordingPlaybackUrlChanged();
    void networkDiagnosticsChanged();
    void dashboardChanged();
    void callsChanged();
    void smsChanged();
    void simChanged();
    void bridgeChanged();

private:
    void setStatus(QString value);
    void setOutput(QString value);
    void initializeDatabase();
    int saveContacts(const QVariantList &contacts);
    QUrl endpoint(const QString &path, const QVariantMap &query) const;
    QByteArray formBody(const QVariantMap &parameters) const;
    void handleReply(class QNetworkReply *reply, const QString &path, const QVariantMap &parameters, bool mutation);
    void getJson(const QString &path, const QVariantMap &parameters, std::function<void(QJsonDocument)> callback,
                 std::function<void()> failure = {});
    void finishDashboardRequest(quint64 generation);
    void loadSmsTemplates();
    void persistSmsTemplates();
    void connectBridgeStream();
    bool knownRoutingNumber(const QString &number) const;
    void loadRecordings();
    void fetchRecording(const QString &id, std::function<void(QByteArray, QString)> callback);
    QNetworkRequest bridgeRequest(const QString &path) const;

    QNetworkAccessManager network_;
    CapabilityCatalog catalog_;
    QJsonArray apiRecipes_;
    QJsonArray dialActions_;
    QVariantList contacts_;
    QVariantList routingNumbers_;
    QVariantList routingRingings_;
    QVariantList routingRoutes_;
    QVariantList recordings_;
    QUrl recordingPlaybackUrl_;
    QUrl bridgeBaseUrl_;
    QString bridgeToken_;
    QVariantList networkDiagnostics_;
    QVariantMap dashboard_;
    quint64 dashboardGeneration_ = 0;
    int dashboardPending_ = 0;
    QVariantList calls_;
    QVariantList activeCalls_;
    QVariantMap callFilters_;
    int callPage_ = 1;
    int callPages_ = 1;
    QVariantList smsRecords_;
    QStringList smsSenders_;
    QVariantList smsTemplates_;
    QVariantList simCards_;
    QVariantMap simDetail_;
    QVariantList simData_;
    QVariantList bridgeEvents_;
    QString ivrScript_;
    QWebSocket bridgeSocket_;
    QSqlDatabase database_;
    QString user_;
    QString password_;
    QString status_ = QStringLiteral("Připraveno");
    QString output_;
    QString balance_ = QStringLiteral("—");
};
