#include "backend.h"

#include <QFile>
#include <QDir>
#include <QSet>
#include <QSqlQuery>
#include <QTemporaryFile>
#include <QTemporaryDir>
#include <QtTest>
#include <algorithm>

class CoreTest final : public QObject {
    Q_OBJECT
private slots:
    void dialCompiler()
    {
        const QVariantList modifiers {
            QVariantMap{{"id", "jitter"}, {"parameters", QVariantMap{{"milliseconds", 300}}}},
            QVariantMap{{"id", "record"}},
            QVariantMap{{"id", "anonymous"}}
        };
        QCOMPARE(DialPlanCompiler::compile(QStringLiteral("+420123456789"), modifiers),
                 QStringLiteral("*089300*086*31*+420123456789"));
        QVERIFY(DialPlanCompiler::isProtectedAddress(QStringLiteral("123456")));
        QVERIFY(DialPlanCompiler::isProtectedAddress(QStringLiteral("alice@example.test")));
    }

    void rejectsInvalidInput()
    {
        QVERIFY_THROWS_EXCEPTION(std::invalid_argument, DialPlanCompiler::compile(QStringLiteral("12\n34"), {}));
        QVERIFY_THROWS_EXCEPTION(std::invalid_argument, DialPlanCompiler::compile(QStringLiteral("123"), {QVariantMap{{"id", "unknown"}}}));
    }

    void catalogComplete()
    {
        QFile file(QStringLiteral(THSIP_SOURCE_DIR "/resources/capabilities.json"));
        QVERIFY2(file.open(QIODevice::ReadOnly), qPrintable(file.errorString()));
        CapabilityCatalog catalog;
        QString error;
        QVERIFY2(catalog.load(file.readAll(), &error), qPrintable(error));
        QVERIFY(catalog.items().size() >= 60);
        QVERIFY(catalog.invalidItems().isEmpty());
    }

    void restCatalogComplete()
    {
        QFile file(QStringLiteral(THSIP_SOURCE_DIR "/resources/api_endpoints.json"));
        QVERIFY2(file.open(QIODevice::ReadOnly), qPrintable(file.errorString()));
        const QJsonArray recipes = QJsonDocument::fromJson(file.readAll()).array();
        QVERIFY(recipes.size() >= 40);
        QSet<QString> ids;
        for (const QJsonValue &value : recipes) {
            const QJsonObject recipe = value.toObject();
            for (const char *field : {"id", "category", "title", "method", "path"}) QVERIFY2(!recipe.value(QLatin1String(field)).toString().isEmpty(), field);
            QVERIFY2(!ids.contains(recipe.value(QStringLiteral("id")).toString()), "duplicate REST recipe id");
            ids.insert(recipe.value(QStringLiteral("id")).toString());
        }
        for (const QString &id : QStringList{"callback", "calls-json", "active-call-delete", "balance-transfer", "line-purchase", "speed-dial-delete", "sms-send", "sim-update", "sim-assign", "ringing-delete", "route-delete", "tts"}) QVERIFY2(ids.contains(id), qPrintable(id));
    }

    void sensitiveMutations()
    {
        AppController app;
        QVERIFY(app.isSensitiveMutation(QStringLiteral("DELETE"), QStringLiteral("active_calls/1.json")));
        QVERIFY(app.isSensitiveMutation(QStringLiteral("POST"), QStringLiteral("balance_transfer.json")));
        QVERIFY(app.isSensitiveMutation(QStringLiteral("PUT"), QStringLiteral("sim_cards/420.json")));
        QVERIFY(!app.isSensitiveMutation(QStringLiteral("POST"), QStringLiteral("sms")));
        QVERIFY(!app.isSensitiveMutation(QStringLiteral("GET"), QStringLiteral("calls.json")));
    }

    void odorikHttp200Errors()
    {
        QCOMPARE(AppController::odorikError(QByteArray(R"({"error":"bad_password"})")), QStringLiteral("bad_password"));
        QCOMPARE(AppController::odorikError(QByteArray(R"({"errors":{"from":"missing"}})")), QStringLiteral("{\"from\":\"missing\"}"));
        QVERIFY(AppController::odorikError(QByteArray(R"([{"id":1}])")).isEmpty());
        QVERIFY(AppController::odorikError("42.50").isEmpty());
    }

    void smsSegmentation()
    {
        AppController app;
        QCOMPARE(app.smsInfo(QString(160, QLatin1Char('a'))).value(QStringLiteral("segments")).toInt(), 1);
        QCOMPARE(app.smsInfo(QString(161, QLatin1Char('a'))).value(QStringLiteral("segments")).toInt(), 2);
        QCOMPARE(app.smsInfo(QStringLiteral("€")).value(QStringLiteral("units")).toInt(), 2);
        QCOMPARE(app.smsInfo(QStringLiteral("Příliš žluťoučký")).value(QStringLiteral("encoding")).toString(), QStringLiteral("Unicode"));
        QVERIFY(!app.smsInfo(QString(766, QLatin1Char('a'))).value(QStringLiteral("valid")).toBool());
    }

    void contactNormalizationAndImport()
    {
        AppController app;
        QCOMPARE(app.normalizePhone(QStringLiteral("123456")), QStringLiteral("123456"));
        QCOMPARE(app.normalizePhone(QStringLiteral("*086+420123456789")), QStringLiteral("*086+420123456789"));
        QCOMPARE(app.normalizePhone(QStringLiteral("alice@sip.example")), QStringLiteral("alice@sip.example"));
        QTemporaryFile csv(QDir::tempPath() + QStringLiteral("/thsip-XXXXXX.csv"));
        QVERIFY(csv.open());
        QVERIFY(csv.write("name,phone,label,organization\nTest Contact,+420 777 123 456,mobil,TH\n") > 0);
        csv.flush();
        app.importContacts(QUrl::fromLocalFile(csv.fileName()));
        QVERIFY(std::ranges::any_of(app.contacts(), [](const QVariant &value) { return value.toMap().value(QStringLiteral("name")) == QLatin1String("Test Contact"); }));
    }

    void dialActions()
    {
        AppController app;
        QVERIFY(app.dialActions().size() >= 35);
        QCOMPARE(app.compileDialAction(QStringLiteral("pickup-line"), {{QStringLiteral("line"), QStringLiteral("123456")}}, false), QStringLiteral("*0822*123456"));
        QCOMPARE(app.compileDialAction(QStringLiteral("voicemail"), {{QStringLiteral("delay"), QStringLiteral("20")}, {QStringLiteral("greeting"), QStringLiteral("7")}}, true), QStringLiteral("*085120*7"));
        QVERIFY(app.compileDialAction(QStringLiteral("queue"), {{QStringLiteral("queue"), QStringLiteral("sales")}}, false).startsWith(QStringLiteral("Chyba:")));
        QVERIFY(app.compileDialAction(QStringLiteral("pickup-line"), {{QStringLiteral("line"), QStringLiteral("12\n3456")}}, false).startsWith(QStringLiteral("Chyba:")));
    }

    void dialActionCatalogComplete()
    {
        QFile file(QStringLiteral(THSIP_SOURCE_DIR "/resources/dial_actions.json"));
        QVERIFY(file.open(QIODevice::ReadOnly));
        const QJsonArray actions = QJsonDocument::fromJson(file.readAll()).array();
        QSet<QString> ids;
        for (const QJsonValue &value : actions) {
            const QJsonObject action = value.toObject();
            QVERIFY(!action.value(QStringLiteral("id")).toString().isEmpty());
            QVERIFY(!action.value(QStringLiteral("template")).toString().isEmpty());
            QVERIFY(action.value(QStringLiteral("parameters")).isArray());
            QVERIFY(!ids.contains(action.value(QStringLiteral("id")).toString()));
            ids.insert(action.value(QStringLiteral("id")).toString());
        }
        for (const QString &id : QStringList{"support-voip", "dtmf-test", "conference-room", "web-ivr", "direct-video", "trunk", "time-rule", "pickup-line", "voicemail", "queue", "missed-sms", "raw"}) QVERIFY2(ids.contains(id), qPrintable(id));
    }

    void routingPreview()
    {
        AppController app;
        const QVariantList targets {
            QVariantMap{{"kind", "normal"}, {"target", "123456"}, {"delay", 5}, {"main", true}},
            QVariantMap{{"kind", "voicemail"}, {"mailboxDelay", "20"}, {"greeting", "7"}}
        };
        const QVariantMap parallel = app.routingPreview(QStringLiteral("00420910123456"), QStringLiteral("parallel"), targets, {});
        QVERIFY2(parallel.value(QStringLiteral("valid")).toBool(), qPrintable(parallel.value(QStringLiteral("errors")).toStringList().join(QLatin1Char(','))));
        QCOMPARE(parallel.value(QStringLiteral("targets")).toStringList(), QStringList({QStringLiteral("*08305*088123456"), QStringLiteral("*085120*7")}));
        const QVariantMap random = app.routingPreview(QStringLiteral("00420910123456"), QStringLiteral("random"), targets, {});
        QVERIFY(random.value(QStringLiteral("valid")).toBool());
        QCOMPARE(random.value(QStringLiteral("targets")).toStringList().size(), 1);
        QVERIFY(random.value(QStringLiteral("preview")).toString().startsWith(QStringLiteral("*0888*")));
        const QVariantMap loop = app.routingPreview(QStringLiteral("00420910123456"), QStringLiteral("parallel"), {QVariantMap{{"kind", "normal"}, {"target", "+420910123456"}}}, {});
        QVERIFY(!loop.value(QStringLiteral("valid")).toBool());
        QVERIFY(loop.value(QStringLiteral("errors")).toStringList().contains(QStringLiteral("Detekována přímá směrovací smyčka")));
    }

    void localRecordingLifecycle()
    {
        AppController app;
        QTemporaryFile source(QDir::tempPath() + QStringLiteral("/thsip-recording-XXXXXX.wav"));
        QVERIFY(source.open());
        QCOMPARE(source.write("RIFFtest"), 8);
        source.flush();
        app.registerLocalRecording(QStringLiteral("call-test"), source.fileName());
        const QVariantList recordings = app.recordings();
        const auto it = std::ranges::find_if(recordings, [](const QVariant &value) {
            const QVariantMap item = value.toMap();
            return item.value(QStringLiteral("callId")) == QLatin1String("call-test") && item.value(QStringLiteral("local")).toBool();
        });
        QVERIFY(it != recordings.end());
        const QString id = it->toMap().value(QStringLiteral("id")).toString();
        app.updateRecording(id, QStringLiteral("poznámka"), QStringLiteral("2099-01-01T00:00:00Z"));
        QTemporaryDir output;
        QVERIFY(output.isValid());
        const QString exportPath = output.filePath(QStringLiteral("export.wav"));
        app.exportRecording(id, QUrl::fromLocalFile(exportPath));
        QFile exported(exportPath);
        QVERIFY(exported.open(QIODevice::ReadOnly));
        QCOMPARE(exported.readAll(), QByteArrayLiteral("RIFFtest"));
    }

    void portalWhitelist()
    {
        AppController app;
        QVERIFY(app.isOfficialPortalUrl(QUrl(QStringLiteral("https://www.odorik.cz/ucet/"))));
        QVERIFY(app.isOfficialPortalUrl(QUrl(QStringLiteral("https://forum.odorik.cz/viewtopic.php?t=1"))));
        QVERIFY(!app.isOfficialPortalUrl(QUrl(QStringLiteral("http://www.odorik.cz/"))));
        QVERIFY(!app.isOfficialPortalUrl(QUrl(QStringLiteral("https://odorik.cz.evil.invalid/"))));
    }

    void databaseSchema()
    {
        AppController app;
        const QStringList required {"accounts", "sip_accounts", "lines", "public_numbers", "routes", "ringings", "sim_cards", "mobile_data", "contacts", "contact_numbers", "speed_dials", "calls", "call_legs", "active_calls", "sms_records", "recordings", "voicemails", "dial_presets", "capability_catalog", "webhook_events", "sync_state", "settings"};
        QSqlQuery query(QSqlDatabase::database(QStringLiteral("thsip-main")));
        QVERIFY(query.exec(QStringLiteral("SELECT name FROM sqlite_master WHERE type='table'")));
        QStringList actual;
        while (query.next()) actual << query.value(0).toString();
        for (const QString &table : required) QVERIFY2(actual.contains(table), qPrintable(table));
    }
};

QTEST_MAIN(CoreTest)
#include "test_core.moc"
