#pragma once

#include <QAbstractListModel>
#include <QJsonArray>
#include <QJsonObject>
#include <QNetworkAccessManager>
#include <QObject>
#include <QSqlDatabase>
#include <QUrl>
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

private:
    void setStatus(QString value);
    void setOutput(QString value);
    void initializeDatabase();
    int saveContacts(const QVariantList &contacts);
    QUrl endpoint(const QString &path, const QVariantMap &query) const;
    QByteArray formBody(const QVariantMap &parameters) const;
    void handleReply(class QNetworkReply *reply, const QString &path, const QVariantMap &parameters, bool mutation);
    void getJson(const QString &path, const QVariantMap &parameters, std::function<void(QJsonDocument)> callback);
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
    QSqlDatabase database_;
    QString user_;
    QString password_;
    QString status_ = QStringLiteral("Připraveno");
    QString output_;
    QString balance_ = QStringLiteral("—");
};
