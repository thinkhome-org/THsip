#include "backend.h"
#include "contactstore.h"
#include "secretstore.h"

#include <QCoreApplication>
#include <QCryptographicHash>
#include <QDateTime>
#include <QDir>
#include <QDesktopServices>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonParseError>
#include <QNetworkReply>
#include <QNetworkInterface>
#include <QHostInfo>
#include <QTcpSocket>
#include <QSslSocket>
#include <QSslCipher>
#include <QStorageInfo>
#include <QSysInfo>
#include <QTimer>
#include <QDataStream>
#include <QRegularExpression>
#include <QSaveFile>
#include <QSqlError>
#include <QSqlQuery>
#include <QStandardPaths>
#include <QSettings>
#include <QUrlQuery>
#include <QUuid>
#include <QSet>
#include <algorithm>
#include <cmath>
#include <numbers>

#ifdef THSIP_HAVE_LIBPHONENUMBER
#include <phonenumbers/phonenumber.pb.h>
#include <phonenumbers/phonenumberutil.h>
#endif

namespace {
QString twoDigits(const QVariantMap &p, const char *key, int min = 1, int max = 99)
{
    bool ok = false;
    const int value = p.value(QLatin1String(key)).toInt(&ok);
    if (!ok || value < min || value > max)
        throw std::invalid_argument(key);
    return QStringLiteral("%1").arg(value, 2, 10, QLatin1Char('0'));
}

QString required(const QVariantMap &p, const char *key)
{
    const QString value = p.value(QLatin1String(key)).toString().trimmed();
    if (value.isEmpty())
        throw std::invalid_argument(key);
    return value;
}

QString pathSegment(const QString &value) { return QString::fromLatin1(QUrl::toPercentEncoding(value)); }

bool validSimMobile(const QString &value)
{
    return QRegularExpression(QStringLiteral("^\\+?[0-9]{6,16}$")).match(value.trimmed()).hasMatch();
}

QString firstJsonString(const QJsonObject &object, std::initializer_list<const char *> keys)
{
    for (const char *key : keys) {
        const QJsonValue value = object.value(QLatin1String(key));
        if (!value.isUndefined() && !value.isNull()) return value.toVariant().toString();
    }
    return {};
}

QString routingCanonical(QString value)
{
    value = value.trimmed();
    if (value.contains(QLatin1Char('@'))) return value.toLower();
    value.remove(QRegularExpression(QStringLiteral("[^0-9]")));
    if (value.startsWith(QStringLiteral("00"))) value.remove(0, 2);
    return value;
}

QString unescapeVCard(QString value)
{
    return value.replace(QStringLiteral("\\n"), QStringLiteral("\n"), Qt::CaseInsensitive)
                .replace(QStringLiteral("\\,"), QStringLiteral(","))
                .replace(QStringLiteral("\\;"), QStringLiteral(";"))
                .replace(QStringLiteral("\\\\"), QStringLiteral("\\"));
}

QList<QStringList> parseCsv(const QString &text, QChar separator)
{
    QList<QStringList> rows;
    QStringList row;
    QString field;
    bool quoted = false;
    for (qsizetype i = 0; i < text.size(); ++i) {
        const QChar c = text.at(i);
        if (c == QLatin1Char('"')) {
            if (quoted && i + 1 < text.size() && text.at(i + 1) == QLatin1Char('"')) { field += c; ++i; }
            else quoted = !quoted;
        } else if (c == separator && !quoted) { row << field; field.clear(); }
        else if ((c == QLatin1Char('\n') || c == QLatin1Char('\r')) && !quoted) {
            if (c == QLatin1Char('\r') && i + 1 < text.size() && text.at(i + 1) == QLatin1Char('\n')) ++i;
            row << field; field.clear();
            if (!row.join(QString()).trimmed().isEmpty()) rows << row;
            row.clear();
        } else field += c;
    }
    if (!field.isEmpty() || !row.isEmpty()) { row << field; rows << row; }
    return rows;
}

QVariantList parseContactFile(const QString &path, QString *error)
{
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly | QIODevice::Text)) { *error = file.errorString(); return {}; }
    QString text = QString::fromUtf8(file.readAll());
    QVariantList contacts;
    if (path.endsWith(QStringLiteral(".vcf"), Qt::CaseInsensitive) || text.contains(QStringLiteral("BEGIN:VCARD"), Qt::CaseInsensitive)) {
        text.replace(QRegularExpression(QStringLiteral("\\r?\\n[ \\t]")), QString());
        QVariantMap contact;
        QVariantList numbers;
        for (const QString &line : text.split(QRegularExpression(QStringLiteral("\\r?\\n")))) {
            const qsizetype colon = line.indexOf(QLatin1Char(':'));
            if (line.compare(QStringLiteral("BEGIN:VCARD"), Qt::CaseInsensitive) == 0) { contact.clear(); numbers.clear(); }
            else if (line.compare(QStringLiteral("END:VCARD"), Qt::CaseInsensitive) == 0) {
                if (contact.value(QStringLiteral("name")).toString().isEmpty()) contact[QStringLiteral("name")] = contact.value(QStringLiteral("organization"));
                if (!numbers.isEmpty()) { contact[QStringLiteral("numbers")] = numbers; contacts << contact; }
            } else if (colon > 0) {
                const QString key = line.left(colon);
                const QString value = unescapeVCard(line.mid(colon + 1).trimmed());
                if (key.startsWith(QStringLiteral("FN"), Qt::CaseInsensitive)) contact[QStringLiteral("name")] = value;
                else if (key.startsWith(QStringLiteral("ORG"), Qt::CaseInsensitive)) contact[QStringLiteral("organization")] = value.section(QLatin1Char(';'), 0, 0);
                else if (key.startsWith(QStringLiteral("TEL"), Qt::CaseInsensitive)) {
                    const QRegularExpressionMatch type = QRegularExpression(QStringLiteral("TYPE=([^;:]+)"), QRegularExpression::CaseInsensitiveOption).match(key);
                    numbers << QVariantMap{{QStringLiteral("number"), value}, {QStringLiteral("label"), type.captured(1)}};
                }
            }
        }
    } else {
        const QString firstLine = text.section(QRegularExpression(QStringLiteral("\\r?\\n")), 0, 0);
        const QChar separator = firstLine.count(QLatin1Char(';')) > firstLine.count(QLatin1Char(',')) ? QLatin1Char(';') : QLatin1Char(',');
        const QList<QStringList> rows = parseCsv(text, separator);
        if (rows.isEmpty()) { *error = QStringLiteral("CSV je prázdné"); return {}; }
        QStringList headers = rows.first();
        for (QString &header : headers) header = header.trimmed().toLower();
        auto column = [&headers](std::initializer_list<const char *> names) {
            for (const char *name : names) { const int i = headers.indexOf(QLatin1String(name)); if (i >= 0) return i; }
            return -1;
        };
        const int name = column({"name", "display_name", "jméno", "nazev"});
        const int number = column({"phone", "number", "telefon", "číslo", "cislo"});
        const int label = column({"label", "typ", "type"});
        const int organization = column({"organization", "company", "firma", "org"});
        if (name < 0 || number < 0) { *error = QStringLiteral("CSV musí mít sloupce name a phone/number"); return {}; }
        for (qsizetype r = 1; r < rows.size(); ++r) {
            const QStringList &values = rows.at(r);
            auto at = [&values](int i) { return i >= 0 && i < values.size() ? values.at(i).trimmed() : QString(); };
            if (!at(number).isEmpty()) contacts << QVariantMap{{QStringLiteral("name"), at(name)},
                {QStringLiteral("organization"), at(organization)},
                {QStringLiteral("numbers"), QVariantList{QVariantMap{{QStringLiteral("number"), at(number)}, {QStringLiteral("label"), at(label)}}}}};
        }
    }
    if (contacts.isEmpty() && error->isEmpty()) *error = QStringLiteral("Soubor neobsahuje kontakt s telefonem");
    return contacts;
}
}

bool DialPlanCompiler::isProtectedAddress(const QString &destination)
{
    const QString d = destination.trimmed();
    return d.startsWith(QLatin1Char('*')) || d.startsWith(QLatin1Char('#')) ||
           d.contains(QLatin1Char('@')) || QRegularExpression(QStringLiteral("^\\d{6}$")).match(d).hasMatch();
}

QString DialPlanCompiler::compile(const QString &destination, const QVariantList &modifiers)
{
    QString target = destination.trimmed();
    if (target.isEmpty())
        throw std::invalid_argument("destination");
    if (target.contains(QRegularExpression(QStringLiteral("[\\r\\n]"))))
        throw std::invalid_argument("destination");

    for (auto it = modifiers.crbegin(); it != modifiers.crend(); ++it) {
        const QVariantMap modifier = it->toMap();
        const QString id = modifier.value(QStringLiteral("id")).toString();
        const QVariantMap p = modifier.value(QStringLiteral("parameters")).toMap();
        if (id == QLatin1String("record")) target = QStringLiteral("*086") + target;
        else if (id == QLatin1String("anonymous")) target = QStringLiteral("*31*") + target;
        else if (id == QLatin1String("autoAnswer")) target = QStringLiteral("*051") + target;
        else if (id == QLatin1String("jitter")) {
            bool ok = false; const int ms = p.value(QStringLiteral("milliseconds"), 300).toInt(&ok);
            if (!ok || ms < 1 || ms > 999) throw std::invalid_argument("milliseconds");
            target = QStringLiteral("*089%1").arg(ms, 3, 10, QLatin1Char('0')) + target;
        } else if (id == QLatin1String("delayed")) target = QStringLiteral("*083") + twoDigits(p, "seconds") + target;
        else if (id == QLatin1String("volumeCaller")) target = QStringLiteral("*0818") + twoDigits(p, "level") + target;
        else if (id == QLatin1String("volumeCalled")) target = QStringLiteral("*0817") + twoDigits(p, "level") + target;
        else if (id == QLatin1String("legacyVolume")) target = QStringLiteral("*0819") + twoDigits(p, "level") + target;
        else if (id == QLatin1String("changeForwardIdentity")) target = QStringLiteral("*087") + target;
        else if (id == QLatin1String("forwardWithSms")) target = QStringLiteral("*0854") + target;
        else if (id == QLatin1String("greetingBeforeConnect"))
            target = QStringLiteral("*0853*%1*").arg(required(p, "greeting")) + target;
        else if (id == QLatin1String("viaLine"))
            target = QStringLiteral("*%1*").arg(required(p, "line")) + target;
        else if (id == QLatin1String("viaSipa"))
            target = QStringLiteral("*06%1*").arg(required(p, "speedDial")) + target;
        else if (id == QLatin1String("raw"))
            target = required(p, "prefix") + target;
        else throw std::invalid_argument("unknown modifier");
    }
    return target;
}

bool CapabilityCatalog::load(const QByteArray &json, QString *error)
{
    QJsonParseError parse;
    const QJsonDocument document = QJsonDocument::fromJson(json, &parse);
    if (parse.error != QJsonParseError::NoError || !document.isArray()) {
        if (error) *error = parse.errorString();
        return false;
    }
    items_ = document.array();
    const QStringList invalid = invalidItems();
    if (!invalid.isEmpty()) {
        if (error) *error = QStringLiteral("Neúplné capability: %1").arg(invalid.join(QStringLiteral(", ")));
        return false;
    }
    return true;
}

QVariantList CapabilityCatalog::items() const { return items_.toVariantList(); }

QStringList CapabilityCatalog::invalidItems() const
{
    QStringList result;
    for (const QJsonValue &value : items_) {
        const QJsonObject o = value.toObject();
        const QString id = o.value(QStringLiteral("id")).toString();
        for (const char *field : {"id", "category", "mechanism", "status", "source", "uiRoute", "testId"})
            if (o.value(QLatin1String(field)).toString().isEmpty()) { result << (id.isEmpty() ? QStringLiteral("<missing-id>") : id); break; }
    }
    return result;
}

AppController::AppController(QObject *parent) : QObject(parent)
{
    connect(&bridgeSocket_, &QWebSocket::connected, this, [this] { emit bridgeChanged(); refreshBridgeEvents(); });
    connect(&bridgeSocket_, &QWebSocket::disconnected, this, [this] {
        emit bridgeChanged();
        QTimer::singleShot(5000, this, [this] { if (bridgeSocket_.state() == QAbstractSocket::UnconnectedState) connectBridgeStream(); });
    });
    connect(&bridgeSocket_, &QWebSocket::textMessageReceived, this, [this](const QString &message) {
        const QJsonObject payload = QJsonDocument::fromJson(message.toUtf8()).object();
        if (payload.value(QStringLiteral("type")).toString() != QLatin1String("event")) return;
        bridgeEvents_.prepend(payload.value(QStringLiteral("event")).toObject().toVariantMap());
        if (bridgeEvents_.size() > 1000) bridgeEvents_.resize(1000);
        emit bridgeChanged();
    });
    QFile file(QStringLiteral(":/qt/qml/THsip/resources/capabilities.json"));
    bool opened = file.open(QIODevice::ReadOnly);
    if (!opened) {
        file.setFileName(QStringLiteral(":/resources/capabilities.json"));
        opened = file.open(QIODevice::ReadOnly);
    }
    QString error;
    if (!opened) setStatus(QStringLiteral("Capability katalog nelze otevřít"));
    else if (!catalog_.load(file.readAll(), &error)) setStatus(error);
    QFile recipes(QStringLiteral(":/qt/qml/THsip/resources/api_endpoints.json"));
    bool recipesOpened = recipes.open(QIODevice::ReadOnly);
    if (!recipesOpened) { recipes.setFileName(QStringLiteral(":/resources/api_endpoints.json")); recipesOpened = recipes.open(QIODevice::ReadOnly); }
    const QJsonDocument recipeDocument = recipesOpened ? QJsonDocument::fromJson(recipes.readAll()) : QJsonDocument();
    if (recipeDocument.isArray()) apiRecipes_ = recipeDocument.array();
    else setStatus(QStringLiteral("REST katalog nelze načíst"));
    QFile actions(QStringLiteral(":/qt/qml/THsip/resources/dial_actions.json"));
    bool actionsOpened = actions.open(QIODevice::ReadOnly);
    if (!actionsOpened) { actions.setFileName(QStringLiteral(":/resources/dial_actions.json")); actionsOpened = actions.open(QIODevice::ReadOnly); }
    const QJsonDocument actionsDocument = actionsOpened ? QJsonDocument::fromJson(actions.readAll()) : QJsonDocument();
    if (actionsDocument.isArray()) dialActions_ = actionsDocument.array();
    else setStatus(QStringLiteral("Dial-action katalog nelze načíst"));
    initializeDatabase();
    refreshContacts();
    loadSmsTemplates();
    QSettings settings;
    user_ = settings.value(QStringLiteral("account/user")).toString();
    if (!user_.isEmpty()) password_ = SecretStore::load(QStringLiteral("odorik-api/") + user_);
    bridgeBaseUrl_ = settings.value(QStringLiteral("bridge/baseUrl")).toUrl();
    if (bridgeBaseUrl_.isValid()) bridgeToken_ = SecretStore::load(QStringLiteral("bridge-client-token"));
    loadRecordings();
    if (!bridgeToken_.isEmpty()) connectBridgeStream();
    if (!password_.isEmpty()) QTimer::singleShot(0, this, &AppController::refreshDashboard);
}

AppController::~AppController()
{
    const QString connection = database_.connectionName();
    database_.close();
    database_ = {};
    QSqlDatabase::removeDatabase(connection);
}

bool AppController::webEngineAvailable() const
{
#ifdef THSIP_HAVE_WEBENGINE
    return true;
#else
    return false;
#endif
}

bool AppController::pjsipAvailable() const
{
#ifdef THSIP_HAVE_PJSIP
    return true;
#else
    return false;
#endif
}

void AppController::configureAccount(const QString &user, const QString &password)
{
    user_ = user.trimmed();
    password_ = password;
    if (user_.isEmpty() || password_.isEmpty()) { setStatus(QStringLiteral("Chybí přihlašovací údaje")); return; }
    QSettings().setValue(QStringLiteral("account/user"), user_);
    setStatus(SecretStore::save(QStringLiteral("odorik-api/") + user_, password_)
                  ? QStringLiteral("Účet bezpečně uložen v Keychainu")
                  : QStringLiteral("Účet nastaven; Keychain zápis selhal"));
    refreshDashboard();
}

bool AppController::storeSipPassword(const QString &line, const QString &password)
{
    const QString clean = line.trimmed();
    return QRegularExpression(QStringLiteral("^\\d{6}$")).match(clean).hasMatch() && !password.isEmpty() && SecretStore::save(QStringLiteral("odorik-sip/") + clean, password);
}

QUrl AppController::endpoint(const QString &path, const QVariantMap &query) const
{
    QString clean = path;
    while (clean.startsWith(QLatin1Char('/'))) clean.removeFirst();
    QUrl url(QStringLiteral("https://www.odorik.cz/api/v1/") + clean);
    QUrlQuery q;
    for (auto it = query.cbegin(); it != query.cend(); ++it)
        if (!it.value().toString().isEmpty()) q.addQueryItem(it.key(), it.value().toString());
    q.addQueryItem(QStringLiteral("user"), user_);
    q.addQueryItem(QStringLiteral("password"), password_);
    q.addQueryItem(QStringLiteral("user_agent"), QStringLiteral("THsip/%1").arg(QCoreApplication::applicationVersion()));
    url.setQuery(q);
    return url;
}

QByteArray AppController::formBody(const QVariantMap &parameters) const
{
    QUrlQuery form;
    for (auto it = parameters.cbegin(); it != parameters.cend(); ++it)
        if (!it.value().toString().isEmpty()) form.addQueryItem(it.key(), it.value().toString());
    form.addQueryItem(QStringLiteral("user"), user_);
    form.addQueryItem(QStringLiteral("password"), password_);
    form.addQueryItem(QStringLiteral("user_agent"), QStringLiteral("THsip/%1").arg(QCoreApplication::applicationVersion()));
    return form.query(QUrl::FullyEncoded).toUtf8();
}

void AppController::api(const QString &method, const QString &path, const QVariantMap &parameters)
{
    if (user_.isEmpty() || password_.isEmpty()) { setStatus(QStringLiteral("Nejdřív nastav účet")); return; }
    const QString verb = method.trimmed().toUpper();
    const QString cleanPath = path.trimmed();
    if (cleanPath.isEmpty() || cleanPath.contains(QLatin1String("://")) || cleanPath.contains(QLatin1String("..")) ||
        cleanPath.contains(QLatin1Char('?')) || cleanPath.contains(QLatin1Char('#')) || cleanPath.contains(QLatin1Char('{'))) {
        setStatus(QStringLiteral("Neplatná nebo nevyplněná API cesta"));
        return;
    }
    QVariantMap cleanParameters = parameters;
    const bool confirmed = cleanParameters.take(QStringLiteral("_confirmed")).toBool();
    if (isSensitiveMutation(verb, cleanPath) && !confirmed) { setStatus(QStringLiteral("Citlivá operace vyžaduje potvrzení")); return; }
    if (verb == QLatin1String("POST") && cleanPath == QLatin1String("sms") && cleanParameters.value(QStringLiteral("message")).toString().size() > 765) {
        setStatus(QStringLiteral("SMS překračuje limit 765 znaků"));
        return;
    }
    QNetworkRequest request(endpoint(cleanPath, verb == QLatin1String("GET") ? cleanParameters : QVariantMap{}));
    request.setHeader(QNetworkRequest::UserAgentHeader, QStringLiteral("THsip/%1").arg(QCoreApplication::applicationVersion()));
    request.setHeader(QNetworkRequest::ContentTypeHeader, QStringLiteral("application/x-www-form-urlencoded"));
    QNetworkReply *reply = nullptr;
    if (verb == QLatin1String("GET")) reply = network_.get(request);
    else if (verb == QLatin1String("POST")) reply = network_.post(request, formBody(cleanParameters));
    else if (verb == QLatin1String("PUT")) reply = network_.put(request, formBody(cleanParameters));
    else if (verb == QLatin1String("DELETE")) reply = network_.deleteResource(request);
    else { setStatus(QStringLiteral("Nepodporovaná HTTP metoda")); return; }
    setStatus(QStringLiteral("Načítám %1").arg(cleanPath));
    connect(reply, &QNetworkReply::finished, this, [this, reply, cleanPath, cleanParameters, verb] { handleReply(reply, cleanPath, cleanParameters, verb != QLatin1String("GET")); });
}

bool AppController::isSensitiveMutation(const QString &method, const QString &path) const
{
    const QString verb = method.trimmed().toUpper();
    const QString clean = path.toLower();
    if (verb == QLatin1String("DELETE")) return true;
    if (clean == QLatin1String("balance_transfer.json") || (clean == QLatin1String("lines.json") && verb == QLatin1String("POST"))) return true;
    return clean.contains(QLatin1String("/assign_number_to_sim")) || clean.contains(QLatin1String("/data_restart")) ||
           verb == QLatin1String("PUT") && clean.startsWith(QLatin1String("sim_cards/"));
}

QVariantMap AppController::smsInfo(const QString &message) const
{
    static const QString basic = QStringLiteral("@£$¥èéùìòÇ\nØø\rÅåΔ_ΦΓΛΩΠΨΣΘΞ ÆæßÉ !\"#¤%&'()*+,-./0123456789:;<=>?¡ABCDEFGHIJKLMNOPQRSTUVWXYZÄÖÑÜ§¿abcdefghijklmnopqrstuvwxyzäöñüà");
    static const QString extension = QStringLiteral("^{}\\[~]|€");
    bool gsm7 = true;
    int units = 0;
    for (const QChar character : message) {
        if (basic.contains(character)) ++units;
        else if (extension.contains(character)) units += 2;
        else { gsm7 = false; break; }
    }
    if (!gsm7) units = message.size();
    const int single = gsm7 ? 160 : 70;
    const int multipart = gsm7 ? 153 : 67;
    const int segments = units <= single ? (units == 0 ? 0 : 1) : (units + multipart - 1) / multipart;
    return {{QStringLiteral("encoding"), gsm7 ? QStringLiteral("GSM-7") : QStringLiteral("Unicode")},
            {QStringLiteral("units"), units}, {QStringLiteral("segments"), segments},
            {QStringLiteral("remaining"), segments < 2 ? single - units : segments * multipart - units},
            {QStringLiteral("valid"), message.size() <= 765}};
}

QString AppController::normalizePhone(const QString &number, const QString &region) const
{
    const QString value = number.trimmed();
    if (value.isEmpty() || DialPlanCompiler::isProtectedAddress(value)) return value;
#ifdef THSIP_HAVE_LIBPHONENUMBER
    using namespace i18n::phonenumbers;
    PhoneNumber parsed;
    PhoneNumberUtil *util = PhoneNumberUtil::GetInstance();
    if (util->Parse(value.toStdString(), region.trimmed().toUpper().toStdString(), &parsed) == PhoneNumberUtil::NO_PARSING_ERROR && util->IsValidNumber(parsed)) {
        std::string formatted;
        util->Format(parsed, PhoneNumberUtil::E164, &formatted);
        return QString::fromStdString(formatted);
    }
#endif
    return value;
}

void AppController::importSystemContacts()
{
    setStatus(QStringLiteral("Čekám na oprávnění ke kontaktům"));
    requestSystemContacts(this, [this](QVariantList contacts, QString error) {
        if (!error.isEmpty()) { setStatus(error); return; }
        const int saved = saveContacts(contacts);
        setStatus(QStringLiteral("Importováno %1 systémových kontaktů").arg(saved));
    });
}

void AppController::importContacts(const QUrl &file)
{
    if (!file.isLocalFile()) { setStatus(QStringLiteral("Import vyžaduje lokální CSV nebo vCard")); return; }
    QString error;
    const QVariantList parsed = parseContactFile(file.toLocalFile(), &error);
    if (!error.isEmpty()) { setStatus(error); return; }
    setStatus(QStringLiteral("Importováno %1 kontaktů").arg(saveContacts(parsed)));
}

int AppController::saveContacts(const QVariantList &contacts)
{
    if (!database_.transaction()) { setStatus(database_.lastError().text()); return 0; }
    int saved = 0;
    for (const QVariant &item : contacts) {
        const QVariantMap contact = item.toMap();
        const QString name = contact.value(QStringLiteral("name")).toString().trimmed();
        const QVariantList numbers = contact.value(QStringLiteral("numbers")).toList();
        if (numbers.isEmpty()) continue;
        QString id = contact.value(QStringLiteral("id")).toString();
        const QString firstNormalized = normalizePhone(numbers.first().toMap().value(QStringLiteral("number")).toString());
        QSqlQuery existing(database_);
        existing.prepare(QStringLiteral("SELECT contact_id FROM contact_numbers WHERE normalized=? LIMIT 1"));
        existing.addBindValue(firstNormalized);
        if (existing.exec() && existing.next()) id = existing.value(0).toString();
        if (id.isEmpty()) id = QStringLiteral("file:") + QString::fromLatin1(QCryptographicHash::hash((name + QLatin1Char('\0') + firstNormalized).toUtf8(), QCryptographicHash::Sha256).toHex());
        QSqlQuery upsert(database_);
        upsert.prepare(QStringLiteral("INSERT INTO contacts(id,display_name,organization) VALUES(?,?,?) ON CONFLICT(id) DO UPDATE SET display_name=excluded.display_name,organization=excluded.organization,updated_at=CURRENT_TIMESTAMP"));
        upsert.addBindValue(id); upsert.addBindValue(name.isEmpty() ? firstNormalized : name); upsert.addBindValue(contact.value(QStringLiteral("organization")));
        if (!upsert.exec()) { database_.rollback(); setStatus(upsert.lastError().text()); return 0; }
        for (const QVariant &numberItem : numbers) {
            const QVariantMap number = numberItem.toMap();
            const QString raw = number.value(QStringLiteral("number")).toString().trimmed();
            if (raw.isEmpty()) continue;
            QSqlQuery insert(database_);
            insert.prepare(QStringLiteral("INSERT INTO contact_numbers(contact_id,number,label,normalized) VALUES(?,?,?,?) ON CONFLICT(contact_id,number) DO UPDATE SET label=excluded.label,normalized=excluded.normalized"));
            insert.addBindValue(id); insert.addBindValue(raw); insert.addBindValue(number.value(QStringLiteral("label"))); insert.addBindValue(normalizePhone(raw));
            if (!insert.exec()) { database_.rollback(); setStatus(insert.lastError().text()); return 0; }
        }
        ++saved;
    }
    if (!database_.commit()) { setStatus(database_.lastError().text()); return 0; }
    refreshContacts();
    return saved;
}

void AppController::refreshContacts()
{
    QVariantList result;
    QSqlQuery query(database_);
    if (!query.exec(QStringLiteral("SELECT c.id,c.display_name,c.organization,n.number,n.label,n.normalized FROM contacts c JOIN contact_numbers n ON n.contact_id=c.id ORDER BY c.display_name COLLATE NOCASE,n.label"))) return;
    QHash<QString, qsizetype> indexes;
    while (query.next()) {
        const QString id = query.value(0).toString();
        qsizetype index = indexes.value(id, -1);
        if (index < 0) {
            index = result.size(); indexes.insert(id, index);
            result << QVariantMap{{QStringLiteral("id"), id}, {QStringLiteral("name"), query.value(1)},
                                  {QStringLiteral("organization"), query.value(2)}, {QStringLiteral("numbers"), QVariantList{}}};
        }
        QVariantMap contact = result.at(index).toMap();
        QVariantList numbers = contact.value(QStringLiteral("numbers")).toList();
        numbers << QVariantMap{{QStringLiteral("number"), query.value(3)}, {QStringLiteral("label"), query.value(4)}, {QStringLiteral("normalized"), query.value(5)}};
        contact[QStringLiteral("numbers")] = numbers;
        result[index] = contact;
    }
    contacts_ = std::move(result);
    emit contactsChanged();
}

void AppController::refreshBalance() { api(QStringLiteral("GET"), QStringLiteral("balance")); }

QVariantMap AppController::summarizeStatistics(const QJsonObject &statistics)
{
    QVariantMap result;
    qlonglong count = 0;
    qlonglong length = 0;
    double price = 0;
    for (const QString &direction : {QStringLiteral("incoming"), QStringLiteral("outgoing"), QStringLiteral("redirected")}) {
        const QJsonObject value = statistics.value(direction).toObject();
        const QVariantMap summary{{QStringLiteral("count"), value.value(QStringLiteral("count")).toInteger()},
                                  {QStringLiteral("length"), value.value(QStringLiteral("length")).toInteger()},
                                  {QStringLiteral("price"), value.value(QStringLiteral("price")).toDouble()}};
        result[direction] = summary;
        count += summary.value(QStringLiteral("count")).toLongLong();
        length += summary.value(QStringLiteral("length")).toLongLong();
        price += summary.value(QStringLiteral("price")).toDouble();
    }
    result[QStringLiteral("count")] = count;
    result[QStringLiteral("length")] = length;
    result[QStringLiteral("price")] = price;
    return result;
}

void AppController::finishDashboardRequest(quint64 generation)
{
    if (generation != dashboardGeneration_ || --dashboardPending_ > 0) return;
    dashboard_[QStringLiteral("loading")] = false;
    dashboard_[QStringLiteral("refreshedAt")] = QDateTime::currentDateTime().toString(Qt::ISODate);
    emit dashboardChanged();
    setStatus(QStringLiteral("Přehled aktualizován"));
}

void AppController::refreshDashboard()
{
    if (user_.isEmpty() || password_.isEmpty()) { setStatus(QStringLiteral("Nejdřív nastav účet")); return; }
    const quint64 generation = ++dashboardGeneration_;
    dashboard_ = {{QStringLiteral("loading"), true}};
    dashboardPending_ = 8;
    emit dashboardChanged();
    setStatus(QStringLiteral("Načítám přehled"));
    refreshBalance();

    const QDate today = QDate::currentDate();
    const int daysFromMonday = today.dayOfWeek() - 1;
    const auto start = [](const QDate &date) { return QDateTime(date, QTime(0, 0)).toString(Qt::ISODate); };
    const QString now = QDateTime::currentDateTime().toString(Qt::ISODate);
    const QVariantMap periods{
        {QStringLiteral("today"), start(today)},
        {QStringLiteral("week"), start(today.addDays(-daysFromMonday))},
        {QStringLiteral("month"), start(QDate(today.year(), today.month(), 1))}
    };
    for (auto it = periods.cbegin(); it != periods.cend(); ++it) {
        getJson(QStringLiteral("call_statistics.json"), {{QStringLiteral("from"), it.value()}, {QStringLiteral("to"), now}},
                [this, generation, key = it.key()](const QJsonDocument &json) {
                    if (generation == dashboardGeneration_) { dashboard_[key] = summarizeStatistics(json.object()); emit dashboardChanged(); }
                    finishDashboardRequest(generation);
                }, [this, generation] { finishDashboardRequest(generation); });
    }

    const QVariantMap month{{QStringLiteral("from"), periods.value(QStringLiteral("month"))}, {QStringLiteral("to"), now}};
    getJson(QStringLiteral("call_statistics/by_destination.json"), month, [this, generation](const QJsonDocument &json) {
        if (generation == dashboardGeneration_) { dashboard_[QStringLiteral("destinations")] = json.array().toVariantList(); emit dashboardChanged(); }
        finishDashboardRequest(generation);
    }, [this, generation] { finishDashboardRequest(generation); });
    getJson(QStringLiteral("call_statistics/missed_calls.json"), month, [this, generation](const QJsonDocument &json) {
        if (generation == dashboardGeneration_) { dashboard_[QStringLiteral("missed")] = json.array().toVariantList(); emit dashboardChanged(); }
        finishDashboardRequest(generation);
    }, [this, generation] { finishDashboardRequest(generation); });
    getJson(QStringLiteral("active_calls.json"), {}, [this, generation](const QJsonDocument &json) {
        if (generation == dashboardGeneration_) { dashboard_[QStringLiteral("activeCalls")] = json.array().toVariantList(); emit dashboardChanged(); }
        finishDashboardRequest(generation);
    }, [this, generation] { finishDashboardRequest(generation); });
    getJson(QStringLiteral("sim_cards.json"), {}, [this, generation](const QJsonDocument &json) {
        if (generation == dashboardGeneration_) {
            const QJsonArray cards = json.isArray() ? json.array() : json.object().value(QStringLiteral("sim_cards")).toArray();
            dashboard_[QStringLiteral("simCards")] = cards.toVariantList();
            emit dashboardChanged();
        }
        finishDashboardRequest(generation);
    }, [this, generation] { finishDashboardRequest(generation); });
    getJson(QStringLiteral("lines.json"), {}, [this, generation, month](const QJsonDocument &json) {
        if (generation != dashboardGeneration_) { finishDashboardRequest(generation); return; }
        QVariantList safeLines;
        for (const QJsonValue &value : json.array()) {
            QJsonObject line = value.toObject();
            line.remove(QStringLiteral("sip_password"));
            safeLines << line.toVariantMap();
            const QString id = line.value(QStringLiteral("id")).toVariant().toString();
            if (id.isEmpty()) continue;
            ++dashboardPending_;
            QVariantMap query = month;
            query[QStringLiteral("line")] = id;
            getJson(QStringLiteral("call_statistics.json"), query, [this, generation, id](const QJsonDocument &stats) {
                if (generation == dashboardGeneration_) {
                    QVariantList byLine = dashboard_.value(QStringLiteral("byLine")).toList();
                    QVariantMap summary = summarizeStatistics(stats.object());
                    summary[QStringLiteral("line")] = id;
                    byLine << summary;
                    dashboard_[QStringLiteral("byLine")] = byLine;
                    emit dashboardChanged();
                }
                finishDashboardRequest(generation);
            }, [this, generation] { finishDashboardRequest(generation); });
        }
        dashboard_[QStringLiteral("lines")] = safeLines;
        emit dashboardChanged();
        finishDashboardRequest(generation);
    }, [this, generation] { finishDashboardRequest(generation); });
}

QVariantMap AppController::normalizeCallFilters(const QVariantMap &filters, QDateTime now)
{
    static const QSet<QString> allowed{QStringLiteral("from"), QStringLiteral("to"), QStringLiteral("since_id"),
        QStringLiteral("direction"), QStringLiteral("line"), QStringLiteral("min_length"), QStringLiteral("max_length"),
        QStringLiteral("min_price"), QStringLiteral("max_price"), QStringLiteral("status"), QStringLiteral("phone_number_filter"),
        QStringLiteral("page_size"), QStringLiteral("page"), QStringLiteral("sip_ids"), QStringLiteral("include_sms")};
    QVariantMap query;
    for (auto it = filters.cbegin(); it != filters.cend(); ++it)
        if (allowed.contains(it.key()) && !it.value().toString().trimmed().isEmpty()) query.insert(it.key(), it.value());
    if (!query.contains(QStringLiteral("since_id"))) {
        if (!query.contains(QStringLiteral("from"))) query[QStringLiteral("from")] = now.addDays(-30).toString(Qt::ISODate);
        if (!query.contains(QStringLiteral("to"))) query[QStringLiteral("to")] = now.toString(Qt::ISODate);
    }
    query[QStringLiteral("page_size")] = std::clamp(query.value(QStringLiteral("page_size"), 200).toInt(), 1, 2000);
    query[QStringLiteral("page")] = std::max(1, query.value(QStringLiteral("page"), 1).toInt());
    query[QStringLiteral("sip_ids")] = true;
    query[QStringLiteral("include_sms")] = true;
    return query;
}

void AppController::refreshCalls(const QVariantMap &filters)
{
    if (user_.isEmpty() || password_.isEmpty()) { setStatus(QStringLiteral("Nejdřív nastav účet")); return; }
    const QVariantMap query = normalizeCallFilters(filters);
    callFilters_ = query;
    setStatus(QStringLiteral("Načítám historii hovorů"));

    QNetworkReply *reply = network_.get(QNetworkRequest(endpoint(QStringLiteral("calls.json"), query)));
    connect(reply, &QNetworkReply::finished, this, [this, reply, query] {
        const QByteArray body = reply->readAll();
        const int status = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
        const QString error = odorikError(body);
        const QJsonDocument json = QJsonDocument::fromJson(body);
        if (reply->error() != QNetworkReply::NoError || status >= 400 || !error.isEmpty() || !json.isArray()) {
            setStatus(QStringLiteral("Historii nelze načíst: %1").arg(error.isEmpty() ? QString::number(status) : error));
        } else {
            calls_ = json.array().toVariantList();
            callPage_ = query.value(QStringLiteral("page")).toInt();
            callPages_ = std::max(1, reply->rawHeader("Odorik-Pages").toInt());
            emit callsChanged();
            if (database_.transaction()) {
                QSqlQuery save(database_);
                save.prepare(QStringLiteral("INSERT INTO calls(id,occurred_at,direction,status,from_number,to_number,line,price,payload) VALUES(?,?,?,?,?,?,?,?,?) ON CONFLICT(id) DO UPDATE SET occurred_at=excluded.occurred_at,direction=excluded.direction,status=excluded.status,from_number=excluded.from_number,to_number=excluded.to_number,line=excluded.line,price=excluded.price,payload=excluded.payload"));
                bool ok = true;
                for (const QJsonValue &value : json.array()) {
                    const QJsonObject call = value.toObject();
                    const QString id = firstJsonString(call, {"id"});
                    const QString occurred = firstJsonString(call, {"date", "time", "start", "occurred_at"});
                    if (id.isEmpty() || occurred.isEmpty()) continue;
                    save.bindValue(0, id); save.bindValue(1, occurred);
                    save.bindValue(2, firstJsonString(call, {"direction"})); save.bindValue(3, firstJsonString(call, {"status"}));
                    save.bindValue(4, firstJsonString(call, {"source_number", "from"})); save.bindValue(5, firstJsonString(call, {"destination_number", "to"}));
                    save.bindValue(6, firstJsonString(call, {"line"})); save.bindValue(7, firstJsonString(call, {"price"}));
                    save.bindValue(8, QString::fromUtf8(QJsonDocument(call).toJson(QJsonDocument::Compact)));
                    if (!save.exec()) { ok = false; break; }
                    save.finish();
                }
                ok ? database_.commit() : database_.rollback();
            }
            setStatus(QStringLiteral("Historie načtena · strana %1/%2").arg(callPage_).arg(callPages_));
        }
        reply->deleteLater();
    });
    getJson(QStringLiteral("active_calls.json"), {}, [this](const QJsonDocument &json) {
        activeCalls_ = json.array().toVariantList();
        emit callsChanged();
    });
}

void AppController::hangupActiveCall(const QString &id, bool confirmed)
{
    const QString clean = id.trimmed();
    if (!confirmed) { setStatus(QStringLiteral("Vzdálené zavěšení vyžaduje potvrzení")); return; }
    if (user_.isEmpty() || password_.isEmpty() || !QRegularExpression(QStringLiteral("^[A-Za-z0-9._:-]{1,128}$")).match(clean).hasMatch()) {
        setStatus(QStringLiteral("Neplatné ID aktivního hovoru")); return;
    }
    QNetworkReply *reply = network_.deleteResource(QNetworkRequest(endpoint(QStringLiteral("active_calls/%1.json").arg(pathSegment(clean)), {})));
    connect(reply, &QNetworkReply::finished, this, [this, reply] {
        const QByteArray body = reply->readAll();
        const int status = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
        const QString error = odorikError(body);
        if (reply->error() != QNetworkReply::NoError || status >= 400 || !error.isEmpty())
            setStatus(QStringLiteral("Hovor nelze vzdáleně ukončit: %1").arg(error.isEmpty() ? QString::number(status) : error));
        else { setStatus(QStringLiteral("Hovor vzdáleně ukončen")); refreshCalls(callFilters_); }
        reply->deleteLater();
    });
}

void AppController::refreshSms(const QVariantMap &filters)
{
    if (user_.isEmpty() || password_.isEmpty()) { setStatus(QStringLiteral("Nejdřív nastav účet")); return; }
    QVariantMap query;
    for (const QString &key : {QStringLiteral("from"), QStringLiteral("to"), QStringLiteral("direction"), QStringLiteral("line")}) {
        const QString value = filters.value(key).toString().trimmed();
        if (!value.isEmpty()) query[key] = value;
    }
    if (!query.contains(QStringLiteral("from"))) query[QStringLiteral("from")] = QDateTime::currentDateTime().addDays(-30).toString(Qt::ISODate);
    if (!query.contains(QStringLiteral("to"))) query[QStringLiteral("to")] = QDateTime::currentDateTime().toString(Qt::ISODate);
    getJson(QStringLiteral("sms.json"), query, [this](const QJsonDocument &json) {
        QVariantList records;
        for (const QJsonValue &value : json.array()) {
            QVariantMap item = value.toObject().toVariantMap();
            item[QStringLiteral("local")] = false;
            records << item;
        }
        QSqlQuery local(database_);
        if (local.exec(QStringLiteral("SELECT id,occurred_at,direction,sender,recipient,local_body FROM sms_records WHERE local_body IS NOT NULL ORDER BY occurred_at DESC LIMIT 500"))) {
            while (local.next()) records.prepend(QVariantMap{{QStringLiteral("id"), local.value(0)}, {QStringLiteral("date"), local.value(1)},
                {QStringLiteral("direction"), local.value(2)}, {QStringLiteral("source_number"), local.value(3)},
                {QStringLiteral("destination_number"), local.value(4)}, {QStringLiteral("message"), local.value(5)}, {QStringLiteral("local"), true}});
        }
        smsRecords_ = std::move(records);
        emit smsChanged();
    });

    QNetworkReply *reply = network_.get(QNetworkRequest(endpoint(QStringLiteral("sms/allowed_sender"), {})));
    connect(reply, &QNetworkReply::finished, this, [this, reply] {
        const QByteArray body = reply->readAll();
        const QJsonDocument json = QJsonDocument::fromJson(body);
        QStringList senders;
        if (json.isArray()) for (const QJsonValue &value : json.array()) senders << value.toString();
        else senders = QString::fromUtf8(body).split(QRegularExpression(QStringLiteral("[,;\\r\\n]+")), Qt::SkipEmptyParts);
        for (QString &sender : senders) sender = sender.trimmed();
        senders.removeAll(QString());
        senders.removeDuplicates();
        if (reply->error() == QNetworkReply::NoError && odorikError(body).isEmpty()) { smsSenders_ = senders; emit smsChanged(); }
        reply->deleteLater();
    });
}

void AppController::loadSmsTemplates()
{
    QSqlQuery query(database_);
    query.prepare(QStringLiteral("SELECT value FROM settings WHERE key='sms/templates'"));
    if (query.exec() && query.next()) smsTemplates_ = QJsonDocument::fromJson(query.value(0).toByteArray()).array().toVariantList();
}

void AppController::persistSmsTemplates()
{
    QSqlQuery query(database_);
    query.prepare(QStringLiteral("INSERT INTO settings(key,value) VALUES('sms/templates',?) ON CONFLICT(key) DO UPDATE SET value=excluded.value,updated_at=CURRENT_TIMESTAMP"));
    query.addBindValue(QString::fromUtf8(QJsonDocument::fromVariant(smsTemplates_).toJson(QJsonDocument::Compact)));
    if (!query.exec()) setStatus(QStringLiteral("SMS templates nelze uložit"));
}

void AppController::saveSmsTemplate(const QString &name, const QString &body)
{
    const QString cleanName = name.trimmed();
    if (cleanName.isEmpty() || cleanName.size() > 80 || body.isEmpty() || body.size() > 765) { setStatus(QStringLiteral("Template vyžaduje název a SMS do 765 znaků")); return; }
    const QString id = QString::fromLatin1(QCryptographicHash::hash(cleanName.toUtf8(), QCryptographicHash::Sha256).toHex());
    const QVariantMap item{{QStringLiteral("id"), id}, {QStringLiteral("name"), cleanName}, {QStringLiteral("body"), body}};
    bool replaced = false;
    for (qsizetype i = 0; i < smsTemplates_.size(); ++i)
        if (smsTemplates_.at(i).toMap().value(QStringLiteral("id")).toString() == id) { smsTemplates_[i] = item; replaced = true; break; }
    if (!replaced) smsTemplates_ << item;
    persistSmsTemplates(); emit smsChanged(); setStatus(QStringLiteral("SMS template uložen"));
}

void AppController::deleteSmsTemplate(const QString &id)
{
    const qsizetype before = smsTemplates_.size();
    smsTemplates_.removeIf([&id](const QVariant &value) { return value.toMap().value(QStringLiteral("id")).toString() == id; });
    if (smsTemplates_.size() == before) { setStatus(QStringLiteral("SMS template nenalezen")); return; }
    persistSmsTemplates(); emit smsChanged(); setStatus(QStringLiteral("SMS template smazán"));
}

void AppController::refreshSimCards()
{
    const QVariantMap query{{QStringLiteral("include_available_data_packages"), true}, {QStringLiteral("include_available_credit_addons"), true},
        {QStringLiteral("include_available_packages_delayed_billing"), true}, {QStringLiteral("include_available_roaming_settings"), true}};
    getJson(QStringLiteral("sim_cards.json"), query, [this](const QJsonDocument &json) {
        simCards_ = json.array().toVariantList();
        emit simChanged();
        setStatus(QStringLiteral("SIM načteny"));
    });
}

void AppController::loadSim(const QString &mobile)
{
    const QString clean = mobile.trimmed();
    if (!validSimMobile(clean)) { setStatus(QStringLiteral("Neplatné mobilní číslo SIM")); return; }
    const QVariantMap query{{QStringLiteral("include_available_data_packages"), true}, {QStringLiteral("include_available_credit_addons"), true},
        {QStringLiteral("include_available_packages_delayed_billing"), true}, {QStringLiteral("include_available_roaming_settings"), true}};
    getJson(QStringLiteral("sim_cards/%1.json").arg(pathSegment(clean)), query, [this](const QJsonDocument &json) {
        simDetail_ = json.object().toVariantMap(); emit simChanged(); setStatus(QStringLiteral("SIM detail načten"));
    });
}

void AppController::loadSimData(const QString &mobile, const QString &from, const QString &to)
{
    const QString clean = mobile.trimmed();
    if (!validSimMobile(clean)) { setStatus(QStringLiteral("Neplatné mobilní číslo SIM")); return; }
    const QVariantMap query{{QStringLiteral("from"), from.trimmed().isEmpty() ? QDateTime::currentDateTime().addDays(-30).toString(Qt::ISODate) : from.trimmed()},
        {QStringLiteral("to"), to.trimmed().isEmpty() ? QDateTime::currentDateTime().toString(Qt::ISODate) : to.trimmed()}};
    getJson(QStringLiteral("sim_cards/%1/mobile_data.json").arg(pathSegment(clean)), query, [this](const QJsonDocument &json) {
        simData_ = json.array().toVariantList(); emit simChanged(); setStatus(QStringLiteral("Historie SIM dat načtena"));
    });
}

void AppController::updateSim(const QString &mobile, const QVariantMap &changes, bool confirmed)
{
    const QString clean = mobile.trimmed();
    if (!confirmed) { setStatus(QStringLiteral("Změna SIM vyžaduje potvrzení")); return; }
    if (!validSimMobile(clean)) { setStatus(QStringLiteral("Neplatné mobilní číslo SIM")); return; }
    static const QSet<QString> allowed{QStringLiteral("state"), QStringLiteral("data_package"), QStringLiteral("data_package_for_next_month"),
        QStringLiteral("voice_package"), QStringLiteral("voice_package_for_next_month"), QStringLiteral("package_delayed_billing"),
        QStringLiteral("package_delayed_billing_for_next_month"), QStringLiteral("missed_calls_register"), QStringLiteral("mobile_data"),
        QStringLiteral("lte"), QStringLiteral("lte_for_next_month"), QStringLiteral("roaming"), QStringLiteral("premium_services"), QStringLiteral("add_credit")};
    QVariantMap parameters;
    for (auto it = changes.cbegin(); it != changes.cend(); ++it)
        if (allowed.contains(it.key()) && !it.value().toString().trimmed().isEmpty()) parameters[it.key()] = it.value();
    const QString state = parameters.value(QStringLiteral("state")).toString();
    if (!state.isEmpty() && state != QLatin1String("active") && state != QLatin1String("suspended")) { setStatus(QStringLiteral("Neplatný stav SIM")); return; }
    if (!state.isEmpty()) parameters = {{QStringLiteral("state"), state}};
    if (parameters.isEmpty()) { setStatus(QStringLiteral("Není vybraná žádná změna SIM")); return; }
    parameters[QStringLiteral("_confirmed")] = true;
    api(QStringLiteral("PUT"), QStringLiteral("sim_cards/%1.json").arg(pathSegment(clean)), parameters);
}

void AppController::restartSimData(const QString &mobile, bool confirmed)
{
    const QString clean = mobile.trimmed();
    if (!confirmed || !validSimMobile(clean)) { setStatus(confirmed ? QStringLiteral("Neplatné mobilní číslo SIM") : QStringLiteral("Restart dat vyžaduje potvrzení")); return; }
    api(QStringLiteral("POST"), QStringLiteral("sim_cards/%1/data_restart").arg(pathSegment(clean)), {{QStringLiteral("_confirmed"), true}});
}

void AppController::assignSim(const QString &mobile, const QString &iccid, const QString &pin, bool delayed, bool confirmed)
{
    const QString clean = mobile.trimmed();
    if (!confirmed) { setStatus(QStringLiteral("Přiřazení SIM vyžaduje potvrzení")); return; }
    if (!validSimMobile(clean) || !QRegularExpression(QStringLiteral("^[0-9]{18,22}$")).match(iccid.trimmed()).hasMatch() ||
        !QRegularExpression(QStringLiteral("^[0-9]{4,8}$")).match(pin.trimmed()).hasMatch()) { setStatus(QStringLiteral("Neplatné číslo, ICCID nebo PIN")); return; }
    api(QStringLiteral("POST"), QStringLiteral("sim_cards/%1/assign_number_to_sim.json").arg(pathSegment(clean)),
        {{QStringLiteral("iccid"), iccid.trimmed()}, {QStringLiteral("pin"), pin.trimmed()}, {QStringLiteral("delayed_activation"), delayed}, {QStringLiteral("_confirmed"), true}});
}

void AppController::handleReply(QNetworkReply *reply, const QString &path, const QVariantMap &parameters, bool mutation)
{
    const QByteArray data = reply->readAll();
    const int status = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
    const QJsonDocument json = QJsonDocument::fromJson(data);
    const QString apiError = odorikError(data);
    if (reply->error() != QNetworkReply::NoError || status >= 400 || !apiError.isEmpty() || data.startsWith("error")) {
        setStatus(QStringLiteral("Odorik API chyba (%1)").arg(status));
        setOutput(!apiError.isEmpty() ? apiError : QString::fromUtf8(data.left(1000)));
    } else {
        const QByteArray pages = reply->rawHeader("Odorik-Pages");
        setStatus(pages.isEmpty() ? QStringLiteral("Hotovo") : QStringLiteral("Hotovo · %1 stran").arg(QString::fromUtf8(pages)));
        const QString contentType = reply->header(QNetworkRequest::ContentTypeHeader).toString().toLower();
        if (contentType.startsWith(QLatin1String("audio/")) || path == QLatin1String("tts/synth")) {
            const QString directory = QStandardPaths::writableLocation(QStandardPaths::AppDataLocation) + QStringLiteral("/audio");
            QDir().mkpath(directory);
            const QString outputPath = directory + QStringLiteral("/tts-%1.wav").arg(QDateTime::currentDateTimeUtc().toString(QStringLiteral("yyyyMMdd-HHmmsszzz")));
            QSaveFile output(outputPath);
            if (!output.open(QIODevice::WriteOnly) || output.write(data) != data.size() || !output.commit()) {
                setStatus(QStringLiteral("TTS WAV nelze uložit"));
                setOutput(output.errorString());
            } else setOutput(outputPath);
        } else setOutput(json.isNull() ? QString::fromUtf8(data) : QString::fromUtf8(json.toJson(QJsonDocument::Indented)));
        if (path == QLatin1String("balance")) { balance_ = QString::fromUtf8(data).trimmed(); emit balanceChanged(); }
        if (path == QLatin1String("sms") && parameters.contains(QStringLiteral("message"))) {
            QSqlQuery insert(database_);
            insert.prepare(QStringLiteral("INSERT INTO sms_records(id,occurred_at,direction,sender,recipient,local_body,payload) VALUES(?,CURRENT_TIMESTAMP,'out',?,?,?,'{}')"));
            insert.addBindValue(QUuid::createUuid().toString(QUuid::WithoutBraces));
            insert.addBindValue(parameters.value(QStringLiteral("sender")));
            insert.addBindValue(parameters.value(QStringLiteral("recipient")));
            insert.addBindValue(parameters.value(QStringLiteral("message")));
            if (!insert.exec()) setStatus(QStringLiteral("SMS odeslána, lokální historii nelze uložit"));
            else refreshSms({});
        }
        if (mutation && path.startsWith(QLatin1String("public_numbers/"))) {
            const QString encoded = path.section(QLatin1Char('/'), 1, 1);
            loadRouting(QUrl::fromPercentEncoding(encoded.toUtf8()));
        }
        if (mutation && path.startsWith(QLatin1String("sim_cards/"))) {
            const QString mobile = QUrl::fromPercentEncoding(path.section(QLatin1Char('/'), 1, 1).toUtf8()).remove(QStringLiteral(".json"));
            refreshSimCards();
            if (validSimMobile(mobile)) loadSim(mobile);
        }
    }
    reply->deleteLater();
}

QString AppController::odorikError(const QByteArray &body)
{
    const QJsonDocument json = QJsonDocument::fromJson(body);
    if (json.isObject()) {
        const QJsonObject object = json.object();
        for (const QString &key : {QStringLiteral("error"), QStringLiteral("errors")}) {
            const QJsonValue value = object.value(key);
            if (value.isUndefined() || value.isNull()) continue;
            if (value.isString()) { if (!value.toString().isEmpty()) return value.toString(); else continue; }
            if (value.isArray() && value.toArray().isEmpty()) continue;
            if (value.isObject() && value.toObject().isEmpty()) continue;
            const QJsonDocument detail = value.isArray() ? QJsonDocument(value.toArray()) : QJsonDocument(value.toObject());
            return QString::fromUtf8(detail.toJson(QJsonDocument::Compact));
        }
    }
    const QString text = QString::fromUtf8(body).trimmed();
    return text.startsWith(QStringLiteral("error"), Qt::CaseInsensitive) ? text : QString();
}

QString AppController::compileDial(const QString &destination, const QVariantList &modifiers) const
{
    try { return DialPlanCompiler::compile(destination, modifiers); }
    catch (const std::exception &e) { return QStringLiteral("Chyba: %1").arg(QString::fromUtf8(e.what())); }
}

QString AppController::compileDialAction(const QString &id, const QVariantMap &parameters, bool advancedEnabled) const
{
    for (const QJsonValue &value : dialActions_) {
        const QJsonObject action = value.toObject();
        if (action.value(QStringLiteral("id")).toString() != id) continue;
        if (action.value(QStringLiteral("advanced")).toBool() && !advancedEnabled) return QStringLiteral("Chyba: vyžaduje Advanced režim");
        QString result = action.value(QStringLiteral("template")).toString();
        for (const QJsonValue &parameterValue : action.value(QStringLiteral("parameters")).toArray()) {
            const QJsonObject parameter = parameterValue.toObject();
            const QString name = parameter.value(QStringLiteral("name")).toString();
            const QString input = parameters.value(name, parameter.value(QStringLiteral("default")).toVariant()).toString().trimmed();
            const QRegularExpression pattern(parameter.value(QStringLiteral("pattern")).toString());
            if (!pattern.isValid() || !pattern.match(input).hasMatch()) return QStringLiteral("Chyba: neplatný parametr %1").arg(name);
            result.replace(QStringLiteral("{%1}").arg(name), input);
        }
        if (result.contains(QRegularExpression(QStringLiteral("\\{[^}]+\\}"))) || result.contains(QRegularExpression(QStringLiteral("[\\r\\n]"))))
            return QStringLiteral("Chyba: neúplný dial string");
        return result;
    }
    return QStringLiteral("Chyba: neznámá akce");
}

QVariantMap AppController::routingPreview(const QString &publicNumber, const QString &mode, const QVariantList &targets, const QVariantMap &options) const
{
    QStringList errors;
    QStringList compiled;
    QStringList terminals;
    if (publicNumber.trimmed().isEmpty()) errors << QStringLiteral("Vyberte veřejné číslo");
    if (targets.isEmpty()) errors << QStringLiteral("Přidejte alespoň jeden cíl");
    if (targets.size() > 32) errors << QStringLiteral("Maximum náhledu je 32 cílů");
    for (const QVariant &value : targets) {
        const QVariantMap target = value.toMap();
        const QString kind = target.value(QStringLiteral("kind"), QStringLiteral("normal")).toString();
        const QString terminal = target.value(QStringLiteral("target")).toString().trimmed();
        QString result;
        if (kind == QLatin1String("normal")) {
            if (!QRegularExpression(QStringLiteral("^[0-9+*#@._:-]{1,128}$")).match(terminal).hasMatch()) errors << QStringLiteral("Neplatný cíl");
            else result = terminal;
        } else if (kind == QLatin1String("voicemail")) result = compileDialAction(QStringLiteral("voicemail"), {{QStringLiteral("delay"), target.value(QStringLiteral("mailboxDelay"))}, {QStringLiteral("greeting"), target.value(QStringLiteral("greeting"))}}, true);
        else if (kind == QLatin1String("greeting")) result = compileDialAction(QStringLiteral("greeting-only"), {{QStringLiteral("delay"), target.value(QStringLiteral("mailboxDelay"))}, {QStringLiteral("greeting"), target.value(QStringLiteral("greeting"))}}, true);
        else if (kind == QLatin1String("mobileVoicemail")) result = compileDialAction(QStringLiteral("mobile-voicemail"), {{QStringLiteral("delay"), target.value(QStringLiteral("mailboxDelay"))}, {QStringLiteral("greeting"), target.value(QStringLiteral("greeting"))}}, true);
        else if (kind == QLatin1String("greetingThenDial")) result = compileDialAction(QStringLiteral("greeting-then-dial"), {{QStringLiteral("delay"), target.value(QStringLiteral("mailboxDelay"))}, {QStringLiteral("greeting"), target.value(QStringLiteral("greeting"))}, {QStringLiteral("target"), terminal}}, true);
        else if (kind == QLatin1String("queue")) result = compileDialAction(QStringLiteral("queue"), {{QStringLiteral("queue"), target.value(QStringLiteral("queue"))}}, true);
        else if (kind == QLatin1String("webIvr")) result = compileDialAction(QStringLiteral("web-ivr"), {{QStringLiteral("slot"), target.value(QStringLiteral("slot"))}}, true);
        else if (kind == QLatin1String("falseRinging")) result = QStringLiteral("*082");
        else if (kind == QLatin1String("busy")) result = QStringLiteral("*0821");
        else errors << QStringLiteral("Neznámý typ cíle");
        if (result.startsWith(QStringLiteral("Chyba:"))) { errors << result; continue; }
        if (result.isEmpty()) continue;
        QVariantList modifiers;
        const QString forward = target.value(QStringLiteral("forwardMode")).toString();
        if (forward == QLatin1String("replace")) modifiers << QVariantMap{{QStringLiteral("id"), QStringLiteral("changeForwardIdentity")}};
        else if (forward == QLatin1String("sms")) modifiers << QVariantMap{{QStringLiteral("id"), QStringLiteral("forwardWithSms")}};
        const QString preconnect = target.value(QStringLiteral("preconnectGreeting")).toString().trimmed();
        if (!preconnect.isEmpty()) modifiers << QVariantMap{{QStringLiteral("id"), QStringLiteral("greetingBeforeConnect")}, {QStringLiteral("parameters"), QVariantMap{{QStringLiteral("greeting"), preconnect}}}};
        if (target.value(QStringLiteral("main")).toBool()) {
            if (kind != QLatin1String("normal") || !QRegularExpression(QStringLiteral("^[0-9]{6}$")).match(terminal).hasMatch()) errors << QStringLiteral("Hlavní větev vyžaduje šestimístnou Odorik linku");
            else result = QStringLiteral("*088") + result;
        }
        const int delay = target.value(QStringLiteral("delay")).toInt();
        if (delay) modifiers << QVariantMap{{QStringLiteral("id"), QStringLiteral("delayed")}, {QStringLiteral("parameters"), QVariantMap{{QStringLiteral("seconds"), delay}}}};
        try { result = DialPlanCompiler::compile(result, modifiers); }
        catch (...) { errors << QStringLiteral("Neplatné routing modifikátory"); continue; }
        compiled << result;
        if (!terminal.isEmpty()) terminals << terminal;
    }
    const QString canonicalPublic = routingCanonical(publicNumber);
    for (const QString &terminal : terminals)
        if (!canonicalPublic.isEmpty() && routingCanonical(terminal) == canonicalPublic) errors << QStringLiteral("Detekována přímá směrovací smyčka");
    QSet<QString> unique;
    for (const QString &target : compiled) {
        if (unique.contains(target)) errors << QStringLiteral("Duplicitní cíl: %1").arg(target);
        unique.insert(target);
    }
    QStringList outputTargets = compiled;
    if (mode == QLatin1String("random")) {
        if (compiled.size() < 2) errors << QStringLiteral("Náhodné rozdělení vyžaduje nejméně dva cíle");
        else outputTargets = {QStringLiteral("*0888*") + compiled.join(QLatin1Char('*'))};
    } else if (mode != QLatin1String("parallel")) errors << QStringLiteral("Neznámý režim směrování");
    const QString period = options.value(QStringLiteral("timePeriod")).toString().trimmed();
    if (!period.isEmpty()) {
        if (outputTargets.size() != 1) errors << QStringLiteral("Časové pravidlo vyžaduje jeden nebo náhodně sloučený cíl");
        else {
            const QString timed = compileDialAction(QStringLiteral("time-rule"), {{QStringLiteral("period"), period}, {QStringLiteral("yes"), outputTargets.first()}, {QStringLiteral("no"), options.value(QStringLiteral("timeFallback"))}}, true);
            if (timed.startsWith(QStringLiteral("Chyba:"))) errors << timed; else outputTargets = {timed};
        }
    }
    if (options.value(QStringLiteral("concurrencyEnabled")).toBool()) {
        const QString limit = compileDialAction(QStringLiteral("concurrency-limit"), {{QStringLiteral("line"), options.value(QStringLiteral("concurrencyLine"))}, {QStringLiteral("max"), options.value(QStringLiteral("concurrencyMax"))}, {QStringLiteral("fallback"), options.value(QStringLiteral("concurrencyFallback"))}}, true);
        if (limit.startsWith(QStringLiteral("Chyba:"))) errors << limit; else outputTargets << limit;
    }
    errors.removeDuplicates();
    return {{QStringLiteral("valid"), errors.isEmpty()}, {QStringLiteral("errors"), errors},
            {QStringLiteral("targets"), outputTargets}, {QStringLiteral("preview"), outputTargets.join(QStringLiteral("\n"))}};
}

bool AppController::knownRoutingNumber(const QString &number) const
{
    return std::ranges::any_of(routingNumbers_, [&number](const QVariant &value) {
        return value.toMap().value(QStringLiteral("public_number")).toString() == number;
    });
}

void AppController::getJson(const QString &path, const QVariantMap &parameters, std::function<void(QJsonDocument)> callback,
                            std::function<void()> failure)
{
    if (user_.isEmpty() || password_.isEmpty()) { setStatus(QStringLiteral("Nejdřív nastav účet")); return; }
    QNetworkReply *reply = network_.get(QNetworkRequest(endpoint(path, parameters)));
    connect(reply, &QNetworkReply::finished, this, [this, reply, callback = std::move(callback), failure = std::move(failure)] {
        const QByteArray body = reply->readAll();
        const QString error = odorikError(body);
        const int status = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
        if (reply->error() != QNetworkReply::NoError || status >= 400 || !error.isEmpty()) {
            setStatus(QStringLiteral("Odorik API chyba: %1").arg(error.isEmpty() ? QString::number(status) : error));
            if (failure) failure();
        }
        else callback(QJsonDocument::fromJson(body));
        reply->deleteLater();
    });
}

void AppController::refreshRouting()
{
    getJson(QStringLiteral("public_numbers.json"), {{QStringLiteral("include_sip_names"), true}, {QStringLiteral("include_shared_numbers"), true}}, [this](const QJsonDocument &json) {
        routingNumbers_ = json.array().toVariantList();
        emit routingChanged();
        setStatus(QStringLiteral("Načteno %1 veřejných čísel").arg(routingNumbers_.size()));
    });
}

void AppController::loadRouting(const QString &publicNumber)
{
    if (!knownRoutingNumber(publicNumber)) { setStatus(QStringLiteral("Neznámé veřejné číslo")); return; }
    const QString base = QStringLiteral("public_numbers/%1/").arg(pathSegment(publicNumber));
    getJson(base + QStringLiteral("ringings.json"), {}, [this](const QJsonDocument &json) { routingRingings_ = json.array().toVariantList(); emit routingChanged(); });
    getJson(base + QStringLiteral("routes.json"), {}, [this](const QJsonDocument &json) { routingRoutes_ = json.array().toVariantList(); emit routingChanged(); });
}

void AppController::addRinging(const QString &publicNumber, const QString &target)
{
    if (!knownRoutingNumber(publicNumber) || target.isEmpty()) { setStatus(QStringLiteral("Neplatné zvonění")); return; }
    api(QStringLiteral("POST"), QStringLiteral("public_numbers/%1/ringings.json").arg(pathSegment(publicNumber)), {{QStringLiteral("ringing_number"), target}});
}

void AppController::addRoute(const QString &publicNumber, const QString &source, const QString &target, bool replaceBySource)
{
    if (!knownRoutingNumber(publicNumber) || source.trimmed().isEmpty() || target.isEmpty()) { setStatus(QStringLiteral("Neplatná route")); return; }
    api(QStringLiteral("POST"), QStringLiteral("public_numbers/%1/routes.json").arg(pathSegment(publicNumber)), {{QStringLiteral("source_number"), source.trimmed()}, {QStringLiteral("ringing_number"), target}, {QStringLiteral("replace_by_source_number"), replaceBySource}});
}

void AppController::deleteRinging(const QString &publicNumber, const QString &target, bool confirmed)
{
    if (!knownRoutingNumber(publicNumber) || !confirmed) { setStatus(QStringLiteral("Smazání routingu vyžaduje potvrzení")); return; }
    api(QStringLiteral("DELETE"), QStringLiteral("public_numbers/%1/ringings/%2.json").arg(pathSegment(publicNumber), pathSegment(target)), {{QStringLiteral("_confirmed"), true}});
}

void AppController::deleteRoute(const QString &publicNumber, const QString &id, bool confirmed)
{
    if (!knownRoutingNumber(publicNumber) || !QRegularExpression(QStringLiteral("^[0-9]+$")).match(id).hasMatch() || !confirmed) { setStatus(QStringLiteral("Smazání routingu vyžaduje potvrzení")); return; }
    api(QStringLiteral("DELETE"), QStringLiteral("public_numbers/%1/routes/%2.json").arg(pathSegment(publicNumber), id), {{QStringLiteral("_confirmed"), true}});
}

void AppController::configureBridge(const QUrl &baseUrl, const QString &token)
{
    QUrl clean = baseUrl.adjusted(QUrl::StripTrailingSlash);
    const bool loopback = clean.host() == QLatin1String("localhost") || clean.host() == QLatin1String("127.0.0.1") || clean.host() == QLatin1String("::1");
    if (!clean.isValid() || clean.host().isEmpty() || (!loopback && clean.scheme() != QLatin1String("https")) ||
        (loopback && clean.scheme() != QLatin1String("https") && clean.scheme() != QLatin1String("http")) ||
        !clean.userInfo().isEmpty() || clean.hasQuery() || clean.hasFragment() || (clean.path() != QString() && clean.path() != QLatin1String("/")) || token.isEmpty()) {
        setStatus(QStringLiteral("Bridge vyžaduje HTTPS URL (HTTP jen localhost) a token"));
        return;
    }
    clean.setPath(QStringLiteral("/"));
    bridgeBaseUrl_ = clean;
    bridgeToken_ = token;
    QSettings().setValue(QStringLiteral("bridge/baseUrl"), bridgeBaseUrl_);
    setStatus(SecretStore::save(QStringLiteral("bridge-client-token"), token) ? QStringLiteral("Bridge uložen v Keychainu") : QStringLiteral("Bridge token nelze uložit"));
    refreshRecordings();
    connectBridgeStream();
}

void AppController::connectBridgeStream()
{
    if (!bridgeBaseUrl_.isValid() || bridgeToken_.isEmpty()) return;
    bridgeSocket_.abort();
    QNetworkRequest request = bridgeRequest(QStringLiteral("v1/stream"));
    QUrl url = request.url();
    url.setScheme(url.scheme() == QLatin1String("https") ? QStringLiteral("wss") : QStringLiteral("ws"));
    request.setUrl(url);
    bridgeSocket_.open(request);
}

void AppController::refreshBridgeEvents()
{
    if (!bridgeBaseUrl_.isValid() || bridgeToken_.isEmpty()) { setStatus(QStringLiteral("Bridge není nastaven")); return; }
    QNetworkReply *reply = network_.get(bridgeRequest(QStringLiteral("v1/events?limit=1000")));
    connect(reply, &QNetworkReply::finished, this, [this, reply] {
        const QJsonDocument json = QJsonDocument::fromJson(reply->readAll());
        if (reply->error() != QNetworkReply::NoError || !json.isArray()) setStatus(QStringLiteral("Bridge události nelze načíst"));
        else { bridgeEvents_ = json.array().toVariantList(); emit bridgeChanged(); setStatus(QStringLiteral("Bridge události načteny")); }
        reply->deleteLater();
    });
}

void AppController::loadBridgeIvr(int slot)
{
    if (slot < 1 || slot > 99 || !bridgeBaseUrl_.isValid() || bridgeToken_.isEmpty()) { setStatus(QStringLiteral("IVR slot musí být 1–99 a Bridge nastaven")); return; }
    QNetworkReply *reply = network_.get(bridgeRequest(QStringLiteral("v1/ivr/%1").arg(slot)));
    connect(reply, &QNetworkReply::finished, this, [this, reply] {
        const QJsonDocument json = QJsonDocument::fromJson(reply->readAll());
        if (reply->error() != QNetworkReply::NoError || !json.isObject()) setStatus(QStringLiteral("IVR script nelze načíst"));
        else { ivrScript_ = json.object().value(QStringLiteral("script")).toString(); emit bridgeChanged(); setStatus(QStringLiteral("IVR script načten")); }
        reply->deleteLater();
    });
}

void AppController::saveBridgeIvr(int slot, const QString &script)
{
    static const QRegularExpression command(QStringLiteral("^(answer|hangup|play:(?:https?://\\S+|\\d{1,3})|play2:(?:https?://\\S+|\\d{1,3})|playnumber:\\d{1,6}|dial:\\S+|setclip:\\+?\\d+|uri:https?://\\S+)$"));
    QStringList lines;
    for (QString line : script.split(QRegularExpression(QStringLiteral("\\r?\\n")))) if (!(line = line.trimmed()).isEmpty()) lines << line;
    if (slot < 1 || slot > 99 || lines.isEmpty() || lines.size() > 100 || std::ranges::any_of(lines, [](const QString &line) { return !command.match(line).hasMatch(); })) {
        setStatus(QStringLiteral("Neplatný IVR script nebo slot")); return;
    }
    if (!bridgeBaseUrl_.isValid() || bridgeToken_.isEmpty()) { setStatus(QStringLiteral("Bridge není nastaven")); return; }
    QNetworkRequest request = bridgeRequest(QStringLiteral("v1/ivr/%1").arg(slot));
    QNetworkReply *reply = network_.put(request, QJsonDocument(QJsonObject{{QStringLiteral("script"), lines.join(QLatin1Char('\n'))}}).toJson(QJsonDocument::Compact));
    connect(reply, &QNetworkReply::finished, this, [this, reply, normalized = lines.join(QLatin1Char('\n'))] {
        const QJsonDocument json = QJsonDocument::fromJson(reply->readAll());
        if (reply->error() != QNetworkReply::NoError || json.object().value(QStringLiteral("ok")).toBool() != true) setStatus(QStringLiteral("IVR script nelze uložit"));
        else { ivrScript_ = normalized; emit bridgeChanged(); setStatus(QStringLiteral("IVR script uložen")); }
        reply->deleteLater();
    });
}

QNetworkRequest AppController::bridgeRequest(const QString &path) const
{
    QNetworkRequest request(bridgeBaseUrl_.resolved(QUrl(path.startsWith(QLatin1Char('/')) ? path.mid(1) : path)));
    request.setRawHeader("Authorization", QByteArrayLiteral("Bearer ") + bridgeToken_.toUtf8());
    request.setHeader(QNetworkRequest::UserAgentHeader, QStringLiteral("THsip/%1").arg(QCoreApplication::applicationVersion()));
    request.setHeader(QNetworkRequest::ContentTypeHeader, QStringLiteral("application/json"));
    return request;
}

void AppController::loadRecordings()
{
    QVariantList result;
    QSqlQuery query(database_);
    if (query.exec(QStringLiteral("SELECT id,call_id,path,bridge_id,mime,size,notes,retain_until,metadata FROM recordings ORDER BY rowid DESC"))) {
        while (query.next()) result << QVariantMap{{QStringLiteral("id"), query.value(0)}, {QStringLiteral("callId"), query.value(1)},
            {QStringLiteral("path"), query.value(2)}, {QStringLiteral("bridgeId"), query.value(3)}, {QStringLiteral("mime"), query.value(4)},
            {QStringLiteral("size"), query.value(5)}, {QStringLiteral("notes"), query.value(6)}, {QStringLiteral("retainUntil"), query.value(7)},
            {QStringLiteral("metadata"), QJsonDocument::fromJson(query.value(8).toByteArray()).object().toVariantMap()},
            {QStringLiteral("local"), !query.value(2).toString().isEmpty()}};
    }
    recordings_ = std::move(result);
    emit recordingsChanged();
}

void AppController::registerLocalRecording(const QString &callId, const QString &path)
{
    if (path.isEmpty()) return;
    QSqlQuery existing(database_);
    existing.prepare(QStringLiteral("SELECT 1 FROM recordings WHERE path=?"));
    existing.addBindValue(path);
    if (existing.exec() && existing.next()) return;
    QSqlQuery insert(database_);
    insert.prepare(QStringLiteral("INSERT INTO recordings(id,call_id,path,mime,size,notes,metadata) VALUES(?,?,?,?,?,'','{}')"));
    insert.addBindValue(QUuid::createUuid().toString(QUuid::WithoutBraces)); insert.addBindValue(callId); insert.addBindValue(path);
    insert.addBindValue(QStringLiteral("audio/wav")); insert.addBindValue(QFileInfo(path).size());
    if (!insert.exec()) setStatus(QStringLiteral("Lokální recording nelze zařadit: %1").arg(insert.lastError().text()));
    loadRecordings();
}

void AppController::refreshRecordings()
{
    loadRecordings();
    if (!bridgeBaseUrl_.isValid() || bridgeToken_.isEmpty()) { setStatus(QStringLiteral("Lokální nahrávky načteny; Bridge není nastaven")); return; }
    QNetworkReply *reply = network_.get(bridgeRequest(QStringLiteral("v1/recordings")));
    connect(reply, &QNetworkReply::finished, this, [this, reply] {
        const QByteArray body = reply->readAll();
        const QJsonDocument json = QJsonDocument::fromJson(body);
        if (reply->error() != QNetworkReply::NoError || !json.isArray()) { setStatus(QStringLiteral("Bridge recordings nelze načíst")); reply->deleteLater(); return; }
        if (!database_.transaction()) { reply->deleteLater(); return; }
        for (const QJsonValue &value : json.array()) {
            const QJsonObject item = value.toObject();
            const QString bridgeId = item.value(QStringLiteral("id")).toString();
            if (!QRegularExpression(QStringLiteral("^[a-f0-9]{64}$")).match(bridgeId).hasMatch()) continue;
            QSqlQuery upsert(database_);
            upsert.prepare(QStringLiteral("INSERT INTO recordings(id,call_id,bridge_id,mime,size,notes,retain_until,metadata) VALUES(?,?,?,?,?,?,?,?) ON CONFLICT(id) DO UPDATE SET call_id=excluded.call_id,mime=excluded.mime,size=excluded.size,notes=excluded.notes,retain_until=excluded.retain_until,metadata=excluded.metadata"));
            upsert.addBindValue(QStringLiteral("bridge:") + bridgeId); upsert.addBindValue(item.value(QStringLiteral("callId")).toString()); upsert.addBindValue(bridgeId);
            upsert.addBindValue(item.value(QStringLiteral("mime")).toString()); upsert.addBindValue(item.value(QStringLiteral("size")).toInteger());
            upsert.addBindValue(item.value(QStringLiteral("notes")).toString()); upsert.addBindValue(item.value(QStringLiteral("retainUntil")).toVariant());
            upsert.addBindValue(QString::fromUtf8(QJsonDocument(item.value(QStringLiteral("metadata")).toObject()).toJson(QJsonDocument::Compact)));
            if (!upsert.exec()) { database_.rollback(); setStatus(upsert.lastError().text()); reply->deleteLater(); return; }
        }
        if (!database_.commit()) setStatus(database_.lastError().text()); else { loadRecordings(); setStatus(QStringLiteral("Nahrávky synchronizovány")); }
        reply->deleteLater();
    });
}

void AppController::fetchRecording(const QString &id, std::function<void(QByteArray, QString)> callback)
{
    QSqlQuery query(database_);
    query.prepare(QStringLiteral("SELECT path,bridge_id,mime FROM recordings WHERE id=?")); query.addBindValue(id);
    if (!query.exec() || !query.next()) { setStatus(QStringLiteral("Nahrávka nenalezena")); return; }
    const QString path = query.value(0).toString();
    const QString bridgeId = query.value(1).toString();
    const QString mime = query.value(2).toString();
    if (!path.isEmpty()) {
        QFile file(path);
        if (!file.open(QIODevice::ReadOnly)) { setStatus(file.errorString()); return; }
        callback(file.readAll(), mime);
        return;
    }
    if (!bridgeBaseUrl_.isValid() || bridgeToken_.isEmpty() || !QRegularExpression(QStringLiteral("^[a-f0-9]{64}$")).match(bridgeId).hasMatch()) { setStatus(QStringLiteral("Bridge není nastaven")); return; }
    QNetworkReply *reply = network_.get(bridgeRequest(QStringLiteral("v1/recordings/") + bridgeId));
    connect(reply, &QNetworkReply::finished, this, [this, reply, callback = std::move(callback), mime] {
        const qint64 length = reply->header(QNetworkRequest::ContentLengthHeader).toLongLong();
        if (reply->error() != QNetworkReply::NoError || length > 100 * 1024 * 1024) setStatus(QStringLiteral("Nahrávku nelze stáhnout"));
        else callback(reply->readAll(), reply->header(QNetworkRequest::ContentTypeHeader).toString().isEmpty() ? mime : reply->header(QNetworkRequest::ContentTypeHeader).toString());
        reply->deleteLater();
    });
}

void AppController::playRecording(const QString &id)
{
    fetchRecording(id, [this, id](QByteArray data, const QString &mime) {
        const QString extension = mime.contains(QLatin1String("mpeg")) ? QStringLiteral("mp3") : mime.contains(QLatin1String("opus")) ? QStringLiteral("opus") : mime.contains(QLatin1String("ogg")) ? QStringLiteral("ogg") : QStringLiteral("wav");
        const QString directory = QStandardPaths::writableLocation(QStandardPaths::CacheLocation) + QStringLiteral("/recordings");
        QDir().mkpath(directory);
        const QString path = directory + QLatin1Char('/') + QString::fromLatin1(QCryptographicHash::hash(id.toUtf8(), QCryptographicHash::Sha256).toHex()) + QLatin1Char('.') + extension;
        QSaveFile file(path);
        if (!file.open(QIODevice::WriteOnly) || file.write(data) != data.size() || !file.commit()) { setStatus(QStringLiteral("Playback cache nelze uložit")); return; }
        recordingPlaybackUrl_ = QUrl::fromLocalFile(path); emit recordingPlaybackUrlChanged();
    });
}

void AppController::exportRecording(const QString &id, const QUrl &destination)
{
    if (!destination.isLocalFile()) { setStatus(QStringLiteral("Export vyžaduje lokální cestu")); return; }
    fetchRecording(id, [this, destination](QByteArray data, const QString &) {
        QSaveFile file(destination.toLocalFile());
        if (!file.open(QIODevice::WriteOnly) || file.write(data) != data.size() || !file.commit()) setStatus(QStringLiteral("Export selhal: %1").arg(file.errorString()));
        else setStatus(QStringLiteral("Nahrávka exportována"));
    });
}

void AppController::updateRecording(const QString &id, const QString &notes, const QString &retainUntil)
{
    if (notes.size() > 10000) { setStatus(QStringLiteral("Poznámka je příliš dlouhá")); return; }
    const QDateTime retention = QDateTime::fromString(retainUntil, Qt::ISODate);
    if (!retainUntil.isEmpty() && (!retention.isValid() || retention <= QDateTime::currentDateTime())) { setStatus(QStringLiteral("Retence musí být budoucí ISO datum")); return; }
    QSqlQuery query(database_); query.prepare(QStringLiteral("SELECT bridge_id FROM recordings WHERE id=?")); query.addBindValue(id);
    if (!query.exec() || !query.next()) { setStatus(QStringLiteral("Nahrávka nenalezena")); return; }
    const QString bridgeId = query.value(0).toString();
    auto saveLocal = [this, id, notes, retainUntil] {
        QSqlQuery update(database_); update.prepare(QStringLiteral("UPDATE recordings SET notes=?,retain_until=? WHERE id=?"));
        update.addBindValue(notes); update.addBindValue(retainUntil.isEmpty() ? QVariant() : retainUntil); update.addBindValue(id);
        if (!update.exec()) setStatus(update.lastError().text()); else { loadRecordings(); setStatus(QStringLiteral("Metadata nahrávky uložena")); }
    };
    if (bridgeId.isEmpty()) { saveLocal(); return; }
    QNetworkRequest request = bridgeRequest(QStringLiteral("v1/recordings/") + bridgeId);
    const QByteArray body = QJsonDocument(QJsonObject{{QStringLiteral("notes"), notes}, {QStringLiteral("retainUntil"), retainUntil.isEmpty() ? QJsonValue(QJsonValue::Null) : QJsonValue(retainUntil)}}).toJson(QJsonDocument::Compact);
    QNetworkReply *reply = network_.sendCustomRequest(request, QByteArrayLiteral("PATCH"), body);
    connect(reply, &QNetworkReply::finished, this, [this, reply, saveLocal] { if (reply->error() == QNetworkReply::NoError) saveLocal(); else setStatus(QStringLiteral("Bridge metadata nelze uložit")); reply->deleteLater(); });
}

void AppController::deleteRecording(const QString &id, bool confirmed)
{
    if (!confirmed) { setStatus(QStringLiteral("Smazání nahrávky vyžaduje potvrzení")); return; }
    QSqlQuery query(database_); query.prepare(QStringLiteral("SELECT path,bridge_id FROM recordings WHERE id=?")); query.addBindValue(id);
    if (!query.exec() || !query.next()) { setStatus(QStringLiteral("Nahrávka nenalezena")); return; }
    const QString path = query.value(0).toString(); const QString bridgeId = query.value(1).toString();
    auto removeRow = [this, id] { QSqlQuery remove(database_); remove.prepare(QStringLiteral("DELETE FROM recordings WHERE id=?")); remove.addBindValue(id); if (remove.exec()) { loadRecordings(); setStatus(QStringLiteral("Nahrávka smazána")); } else setStatus(remove.lastError().text()); };
    if (!path.isEmpty()) {
        if (!QFile::moveToTrash(path)) { setStatus(QStringLiteral("Lokální nahrávku nelze přesunout do Koše")); return; }
        removeRow(); return;
    }
    if (!bridgeBaseUrl_.isValid() || bridgeToken_.isEmpty() || !QRegularExpression(QStringLiteral("^[a-f0-9]{64}$")).match(bridgeId).hasMatch()) { setStatus(QStringLiteral("Bridge není nastaven")); return; }
    QNetworkRequest request = bridgeRequest(QStringLiteral("v1/recordings/") + bridgeId); request.setRawHeader("X-Confirm-Delete", "recording");
    QNetworkReply *reply = network_.deleteResource(request);
    connect(reply, &QNetworkReply::finished, this, [this, reply, removeRow] { if (reply->error() == QNetworkReply::NoError) removeRow(); else setStatus(QStringLiteral("Bridge nahrávku nelze smazat")); reply->deleteLater(); });
}

QVariantMap AppController::systemDiagnostics() const
{
    QStringList addresses;
    for (const QHostAddress &address : QNetworkInterface::allAddresses())
        if (!address.isLoopback() && !address.isLinkLocal()) addresses << address.toString();
    addresses.removeDuplicates();
    QString cipher;
    QSqlQuery query(database_);
    if (query.exec(QStringLiteral("PRAGMA cipher_version")) && query.next()) cipher = query.value(0).toString();
    const QStorageInfo storage(database_.databaseName());
    return {{QStringLiteral("app"), QCoreApplication::applicationName()}, {QStringLiteral("version"), QCoreApplication::applicationVersion()},
            {QStringLiteral("qt"), QString::fromLatin1(qVersion())}, {QStringLiteral("os"), QSysInfo::prettyProductName()},
            {QStringLiteral("kernel"), QSysInfo::kernelType() + QLatin1Char(' ') + QSysInfo::kernelVersion()},
            {QStringLiteral("architecture"), QSysInfo::currentCpuArchitecture()}, {QStringLiteral("localAddresses"), addresses},
            {QStringLiteral("sslSupported"), QSslSocket::supportsSsl()}, {QStringLiteral("sslBuild"), QSslSocket::sslLibraryBuildVersionString()},
            {QStringLiteral("sslRuntime"), QSslSocket::sslLibraryVersionString()}, {QStringLiteral("databaseDriver"), database_.driverName()},
            {QStringLiteral("sqlCipher"), cipher.isEmpty() ? QStringLiteral("not-active") : cipher},
            {QStringLiteral("databaseFreeBytes"), storage.isValid() ? storage.bytesAvailable() : -1},
            {QStringLiteral("apiConfigured"), !user_.isEmpty() && !password_.isEmpty()},
            {QStringLiteral("bridgeConfigured"), bridgeBaseUrl_.isValid() && !bridgeToken_.isEmpty()},
            {QStringLiteral("pjsipAvailable"), pjsipAvailable()}, {QStringLiteral("webEngineAvailable"), webEngineAvailable()}};
}

void AppController::runNetworkDiagnostics()
{
    networkDiagnostics_.clear();
    networkDiagnostics_ << QVariantMap{{QStringLiteral("kind"), QStringLiteral("dns")}, {QStringLiteral("target"), QStringLiteral("sip.odorik.cz")}, {QStringLiteral("status"), QStringLiteral("running")}};
    const QList<QPair<int, bool>> ports {{443,false},{5060,false},{6688,false},{6699,false},{5061,true},{6689,true}};
    for (const auto &[port, tls] : ports)
        networkDiagnostics_ << QVariantMap{{QStringLiteral("kind"), tls ? QStringLiteral("tls") : QStringLiteral("tcp")}, {QStringLiteral("target"), QStringLiteral("sip.odorik.cz:%1").arg(port)}, {QStringLiteral("port"), port}, {QStringLiteral("status"), QStringLiteral("running")}};
    emit networkDiagnosticsChanged();
    QHostInfo::lookupHost(QStringLiteral("sip.odorik.cz"), this, [this](const QHostInfo &host) {
        QStringList addresses; for (const QHostAddress &address : host.addresses()) addresses << address.toString();
        if (!networkDiagnostics_.isEmpty()) networkDiagnostics_[0] = QVariantMap{{QStringLiteral("kind"), QStringLiteral("dns")}, {QStringLiteral("target"), QStringLiteral("sip.odorik.cz")},
            {QStringLiteral("status"), host.error() == QHostInfo::NoError ? QStringLiteral("ok") : QStringLiteral("failed")}, {QStringLiteral("detail"), host.error() == QHostInfo::NoError ? addresses.join(QStringLiteral(", ")) : host.errorString()}};
        emit networkDiagnosticsChanged();
    });
    for (qsizetype index = 0; index < ports.size(); ++index) {
        const int port = ports.at(index).first; const bool tls = ports.at(index).second; const qsizetype resultIndex = index + 1;
        QAbstractSocket *socket = tls ? static_cast<QAbstractSocket *>(new QSslSocket(this)) : static_cast<QAbstractSocket *>(new QTcpSocket(this));
        auto finish = [this, socket, resultIndex, port, tls](const QString &status, const QString &detail) {
            if (socket->property("finished").toBool()) return;
            socket->setProperty("finished", true);
            networkDiagnostics_[resultIndex] = QVariantMap{{QStringLiteral("kind"), tls ? QStringLiteral("tls") : QStringLiteral("tcp")},
                {QStringLiteral("target"), QStringLiteral("sip.odorik.cz:%1").arg(port)}, {QStringLiteral("port"), port},
                {QStringLiteral("status"), status}, {QStringLiteral("detail"), detail}};
            emit networkDiagnosticsChanged(); socket->deleteLater();
        };
        connect(socket, &QAbstractSocket::errorOccurred, this, [finish, socket](QAbstractSocket::SocketError) { finish(QStringLiteral("failed"), socket->errorString()); });
        if (tls) {
            auto *ssl = static_cast<QSslSocket *>(socket);
            connect(ssl, &QSslSocket::encrypted, this, [finish, ssl] { finish(QStringLiteral("ok"), ssl->sessionCipher().name() + QStringLiteral(" · protocol %1").arg(int(ssl->sessionProtocol()))); });
            ssl->connectToHostEncrypted(QStringLiteral("sip.odorik.cz"), quint16(port));
        } else {
            connect(socket, &QAbstractSocket::connected, this, [finish] { finish(QStringLiteral("ok"), QStringLiteral("TCP connected")); });
            socket->connectToHost(QStringLiteral("sip.odorik.cz"), quint16(port));
        }
        QTimer::singleShot(4000, socket, [finish] { finish(QStringLiteral("timeout"), QString()); });
    }
}

QUrl AppController::testToneUrl()
{
    const QString directory = QStandardPaths::writableLocation(QStandardPaths::CacheLocation);
    QDir().mkpath(directory);
    const QString path = directory + QStringLiteral("/test-tone.wav");
    constexpr quint32 sampleRate = 44100, samples = sampleRate, dataSize = samples * 2;
    QSaveFile file(path);
    if (!file.open(QIODevice::WriteOnly)) return {};
    QDataStream out(&file); out.setByteOrder(QDataStream::LittleEndian);
    out.writeRawData("RIFF",4); out << quint32(36 + dataSize); out.writeRawData("WAVEfmt ",8); out << quint32(16) << quint16(1) << quint16(1) << sampleRate << quint32(sampleRate * 2) << quint16(2) << quint16(16); out.writeRawData("data",4); out << dataSize;
    for (quint32 i = 0; i < samples; ++i) out << qint16(std::sin(2.0 * std::numbers::pi * 440.0 * i / sampleRate) * 6000.0);
    if (!file.commit()) return {};
    return QUrl::fromLocalFile(path);
}

QVariant AppController::redactDiagnostics(const QVariant &value, const QString &key)
{
    const QString lower = key.toLower();
    if (lower.contains(QLatin1String("password")) || lower.contains(QLatin1String("token")) || lower.contains(QLatin1String("secret")) || lower == QLatin1String("pin")) return QStringLiteral("[redacted]");
    if (value.metaType().id() == QMetaType::QVariantMap) {
        QVariantMap result; const QVariantMap source = value.toMap();
        for (auto it = source.cbegin(); it != source.cend(); ++it) result.insert(it.key(), redactDiagnostics(it.value(), it.key()));
        return result;
    }
    if (value.metaType().id() == QMetaType::QVariantList || value.metaType().id() == QMetaType::QStringList) {
        QVariantList result; for (const QVariant &item : value.toList()) result << redactDiagnostics(item); return result;
    }
    if (value.metaType().id() != QMetaType::QString) return value;
    QString text = value.toString();
    text.replace(QRegularExpression(QStringLiteral("(?i)(sips?:)[^@\\s>]+@")), QStringLiteral("\\1[redacted]@"));
    text.replace(QRegularExpression(QStringLiteral("\\b(\\d{1,3}\\.\\d{1,3}\\.\\d{1,3})\\.\\d{1,3}\\b")), QStringLiteral("\\1.x"));
    if (QRegularExpression(QStringLiteral("^[0-9A-Fa-f:]+$")).match(text).hasMatch() && text.count(QLatin1Char(':')) >= 2) text = QStringLiteral("[ipv6-redacted]");
    text.replace(QRegularExpression(QStringLiteral("(?<![A-Za-z])([0-9]{2})[0-9]{4,}(?![A-Za-z])")), QStringLiteral("\\1***"));
    return text;
}

QString AppController::exportDiagnostics(const QVariantMap &telephony)
{
    const QVariantMap report{{QStringLiteral("createdAt"), QDateTime::currentDateTimeUtc().toString(Qt::ISODateWithMs)},
                             {QStringLiteral("system"), systemDiagnostics()}, {QStringLiteral("network"), networkDiagnostics_},
                             {QStringLiteral("telephony"), telephony}};
    const QString directory = QStandardPaths::writableLocation(QStandardPaths::AppDataLocation) + QStringLiteral("/diagnostics");
    QDir().mkpath(directory);
    const QString path = directory + QStringLiteral("/thsip-%1.json").arg(QDateTime::currentDateTimeUtc().toString(QStringLiteral("yyyyMMdd-HHmmss")));
    QSaveFile file(path); const QByteArray data = QJsonDocument::fromVariant(redactDiagnostics(report)).toJson(QJsonDocument::Indented);
    if (!file.open(QIODevice::WriteOnly) || file.write(data) != data.size() || !file.commit()) { setStatus(QStringLiteral("Diagnostický bundle nelze uložit")); return {}; }
    setStatus(QStringLiteral("Redacted diagnostika exportována")); setOutput(path); return path;
}

QUrl AppController::portalUrl(const QString &page) const
{
    static const QHash<QString, QString> pages {
        {QStringLiteral("home"), QStringLiteral("https://www.odorik.cz/ucet/")},
        {QStringLiteral("lines"), QStringLiteral("https://www.odorik.cz/ucet/prehled_linek.html")},
        {QStringLiteral("audio"), QStringLiteral("https://www.odorik.cz/ucet/hlasove_zpravy.html")},
        {QStringLiteral("fax"), QStringLiteral("https://www.odorik.cz/w/fax")},
        {QStringLiteral("limits"), QStringLiteral("https://www.odorik.cz/ucet/omezeni_volani.html")},
        {QStringLiteral("help"), QStringLiteral("https://www.odorik.cz/w/api")}
    };
    return QUrl(pages.value(page, pages.value(QStringLiteral("home"))));
}

bool AppController::isOfficialPortalUrl(const QUrl &url) const
{
    const QString host = url.host().toLower();
    return url.scheme() == QLatin1String("https") && (host == QLatin1String("odorik.cz") || host.endsWith(QLatin1String(".odorik.cz")));
}

void AppController::openExternal(const QUrl &url) const
{
    if (url.scheme() == QLatin1String("https")) QDesktopServices::openUrl(url);
}

QString AppController::databasePath() const { return database_.databaseName(); }

void AppController::initializeDatabase()
{
    const QString dir = QStandardPaths::writableLocation(QStandardPaths::AppDataLocation);
    QDir().mkpath(dir);
    database_ = QSqlDatabase::addDatabase(QStringLiteral("QSQLITE"), QStringLiteral("thsip-main"));
    database_.setDatabaseName(dir + QStringLiteral("/thsip.sqlite"));
    if (!database_.open()) { setStatus(QStringLiteral("DB chyba: %1").arg(database_.lastError().text())); return; }
    QSqlQuery q(database_);
    const QStringList schema {
        QStringLiteral("PRAGMA foreign_keys=ON"),
        QStringLiteral("CREATE TABLE IF NOT EXISTS accounts (id TEXT PRIMARY KEY, label TEXT, user TEXT NOT NULL UNIQUE, updated_at TEXT NOT NULL DEFAULT CURRENT_TIMESTAMP)"),
        QStringLiteral("CREATE TABLE IF NOT EXISTS sip_accounts (id TEXT PRIMARY KEY, account_id TEXT NOT NULL REFERENCES accounts(id) ON DELETE CASCADE, line TEXT NOT NULL, registrar TEXT NOT NULL, transport TEXT, enabled INTEGER NOT NULL DEFAULT 1, UNIQUE(account_id,line))"),
        QStringLiteral("CREATE TABLE IF NOT EXISTS lines (id TEXT PRIMARY KEY, account_id TEXT NOT NULL REFERENCES accounts(id) ON DELETE CASCADE, payload TEXT NOT NULL, updated_at TEXT NOT NULL DEFAULT CURRENT_TIMESTAMP)"),
        QStringLiteral("CREATE TABLE IF NOT EXISTS public_numbers (id TEXT PRIMARY KEY, account_id TEXT NOT NULL REFERENCES accounts(id) ON DELETE CASCADE, type TEXT, payload TEXT NOT NULL, updated_at TEXT NOT NULL DEFAULT CURRENT_TIMESTAMP)"),
        QStringLiteral("CREATE TABLE IF NOT EXISTS routes (id TEXT PRIMARY KEY, public_number_id TEXT NOT NULL REFERENCES public_numbers(id) ON DELETE CASCADE, source_number TEXT NOT NULL, ringing_number TEXT NOT NULL, payload TEXT NOT NULL)"),
        QStringLiteral("CREATE TABLE IF NOT EXISTS ringings (public_number_id TEXT NOT NULL REFERENCES public_numbers(id) ON DELETE CASCADE, target TEXT NOT NULL, payload TEXT NOT NULL, PRIMARY KEY(public_number_id,target))"),
        QStringLiteral("CREATE TABLE IF NOT EXISTS sim_cards (mobile TEXT PRIMARY KEY, account_id TEXT NOT NULL REFERENCES accounts(id) ON DELETE CASCADE, payload TEXT NOT NULL, updated_at TEXT NOT NULL DEFAULT CURRENT_TIMESTAMP)"),
        QStringLiteral("CREATE TABLE IF NOT EXISTS mobile_data (id TEXT PRIMARY KEY, mobile TEXT NOT NULL REFERENCES sim_cards(mobile) ON DELETE CASCADE, occurred_at TEXT NOT NULL, bytes_up INTEGER, bytes_down INTEGER, price TEXT, payload TEXT NOT NULL)"),
        QStringLiteral("CREATE TABLE IF NOT EXISTS contacts (id TEXT PRIMARY KEY, display_name TEXT NOT NULL, organization TEXT, notes TEXT, updated_at TEXT NOT NULL DEFAULT CURRENT_TIMESTAMP)"),
        QStringLiteral("CREATE TABLE IF NOT EXISTS contact_numbers (contact_id TEXT NOT NULL REFERENCES contacts(id) ON DELETE CASCADE, number TEXT NOT NULL, label TEXT, normalized TEXT, PRIMARY KEY(contact_id,number))"),
        QStringLiteral("CREATE TABLE IF NOT EXISTS speed_dials (account_id TEXT NOT NULL REFERENCES accounts(id) ON DELETE CASCADE, line TEXT NOT NULL DEFAULT '', shortcut TEXT NOT NULL, number TEXT NOT NULL, name TEXT, owner TEXT, PRIMARY KEY(account_id,line,shortcut))"),
        QStringLiteral("CREATE TABLE IF NOT EXISTS calls (id TEXT PRIMARY KEY, account_id TEXT REFERENCES accounts(id) ON DELETE SET NULL, occurred_at TEXT NOT NULL, direction TEXT, status TEXT, from_number TEXT, to_number TEXT, line TEXT, price TEXT, payload TEXT NOT NULL)"),
        QStringLiteral("CREATE TABLE IF NOT EXISTS call_legs (id TEXT PRIMARY KEY, call_id TEXT NOT NULL REFERENCES calls(id) ON DELETE CASCADE, parent_id TEXT, sip_call_id TEXT, payload TEXT NOT NULL)"),
        QStringLiteral("CREATE TABLE IF NOT EXISTS active_calls (id TEXT PRIMARY KEY, account_id TEXT REFERENCES accounts(id) ON DELETE CASCADE, payload TEXT NOT NULL, updated_at TEXT NOT NULL DEFAULT CURRENT_TIMESTAMP)"),
        QStringLiteral("CREATE TABLE IF NOT EXISTS sms_records (id TEXT PRIMARY KEY, account_id TEXT REFERENCES accounts(id) ON DELETE SET NULL, occurred_at TEXT NOT NULL, direction TEXT, sender TEXT, recipient TEXT, local_body TEXT, payload TEXT NOT NULL)"),
        QStringLiteral("CREATE TABLE IF NOT EXISTS recordings (id TEXT PRIMARY KEY, call_id TEXT REFERENCES calls(id) ON DELETE SET NULL, path TEXT, bridge_id TEXT, mime TEXT, size INTEGER, notes TEXT, retain_until TEXT, metadata TEXT NOT NULL)"),
        QStringLiteral("CREATE TABLE IF NOT EXISTS voicemails (id TEXT PRIMARY KEY, call_id TEXT REFERENCES calls(id) ON DELETE SET NULL, recording_id TEXT REFERENCES recordings(id) ON DELETE SET NULL, payload TEXT NOT NULL)"),
        QStringLiteral("CREATE TABLE IF NOT EXISTS dial_presets (id TEXT PRIMARY KEY, name TEXT NOT NULL, destination TEXT, modifiers TEXT NOT NULL, advanced INTEGER NOT NULL DEFAULT 0)"),
        QStringLiteral("CREATE TABLE IF NOT EXISTS capability_catalog (id TEXT PRIMARY KEY, payload TEXT NOT NULL, updated_at TEXT NOT NULL DEFAULT CURRENT_TIMESTAMP)"),
        QStringLiteral("CREATE TABLE IF NOT EXISTS webhook_events (id TEXT PRIMARY KEY, call_id TEXT NOT NULL, kind TEXT NOT NULL, occurred_at TEXT NOT NULL, payload TEXT NOT NULL)"),
        QStringLiteral("CREATE TABLE IF NOT EXISTS sync_state (resource TEXT PRIMARY KEY, cursor TEXT, updated_at TEXT NOT NULL DEFAULT CURRENT_TIMESTAMP)"),
        QStringLiteral("CREATE TABLE IF NOT EXISTS settings (key TEXT PRIMARY KEY, value TEXT NOT NULL, updated_at TEXT NOT NULL DEFAULT CURRENT_TIMESTAMP)"),
        QStringLiteral("CREATE INDEX IF NOT EXISTS calls_occurred_at ON calls(occurred_at DESC)"),
        QStringLiteral("CREATE INDEX IF NOT EXISTS sms_occurred_at ON sms_records(occurred_at DESC)"),
        QStringLiteral("PRAGMA user_version=1")
    };
    if (!database_.transaction()) { setStatus(QStringLiteral("DB transakci nelze zahájit")); return; }
    for (const QString &statement : schema) {
        if (q.exec(statement)) continue;
        database_.rollback();
        setStatus(QStringLiteral("DB migrace selhala: %1").arg(q.lastError().text()));
        return;
    }
    if (!database_.commit()) setStatus(QStringLiteral("DB migraci nelze uložit: %1").arg(database_.lastError().text()));
}

void AppController::setStatus(QString value) { if (status_ == value) return; status_ = std::move(value); emit statusChanged(); }
void AppController::setOutput(QString value) { if (output_ == value) return; output_ = std::move(value); emit outputChanged(); }
