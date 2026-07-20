#include "backend.h"
#include "platformintegration.h"

#include <QGuiApplication>
#include <QFile>
#include <QSqlQuery>
#include <QQmlApplicationEngine>
#include <QQmlContext>
#include <QTimer>

#ifdef THSIP_HAVE_PJSIP
#include "telephony_pjsua2.h"
#endif

#ifdef THSIP_HAVE_WEBENGINE
#include <QtWebEngineQuick/qtwebenginequickglobal.h>
#endif

int main(int argc, char *argv[])
{
#ifdef THSIP_HAVE_WEBENGINE
    QtWebEngineQuick::initialize();
#endif
    QGuiApplication app(argc, argv);
    QGuiApplication::setOrganizationName(QStringLiteral("ThinkHome"));
    QGuiApplication::setApplicationName(QStringLiteral("THsip"));

    AppController controller;
    if (app.arguments().contains(QStringLiteral("--database-self-test")) || app.arguments().contains(QStringLiteral("--database-migration-self-test"))) {
        QFile file(controller.databasePath());
        const bool encrypted = file.open(QIODevice::ReadOnly) && file.read(16) != QByteArrayLiteral("SQLite format 3\0");
        bool migration = true;
        if (app.arguments().contains(QStringLiteral("--database-migration-self-test"))) {
            QSqlQuery query(QSqlDatabase::database(QStringLiteral("thsip-main")));
            migration = query.exec(QStringLiteral("SELECT value FROM migration_probe")) && query.next() && query.value(0).toString() == QLatin1String("ok");
        }
        return encrypted && migration && controller.systemDiagnostics().value(QStringLiteral("sqlCipher")).toString() != QLatin1String("not-active") ? 0 : 2;
    }
    PlatformIntegration platform;
#ifdef THSIP_HAVE_PJSIP
    if (app.arguments().contains(QStringLiteral("--sip-registration-self-test"))) {
        const QString user = qEnvironmentVariable("THSIP_SIP_USER");
        const QString password = qEnvironmentVariable("THSIP_SIP_PASSWORD");
        if (user.isEmpty() || password.isEmpty()) return 4;
        TelephonyEngine telephony;
        if (!telephony.diagnostics().value(QStringLiteral("available")).toBool()) return 3;
        QObject::connect(&telephony, &TelephonyEngine::accountState, &app,
                         [&app](const QString &, bool registered, const QString &) { if (registered) app.exit(0); });
        QObject::connect(&telephony, &TelephonyEngine::error, &app,
                         [&app](const QString &, const QString &) { app.exit(5); });
        QTimer::singleShot(20000, &app, [&app] { app.exit(6); });
        telephony.addAccount({
            {QStringLiteral("idUri"), QStringLiteral("<sip:%1@sip.odorik.cz>").arg(user)},
            {QStringLiteral("registrar"), QStringLiteral("sips:sip.odorik.cz:5061;transport=tls")},
            {QStringLiteral("user"), user}, {QStringLiteral("password"), password},
            {QStringLiteral("srtp"), true}, {QStringLiteral("ice"), true}
        });
        return app.exec();
    }
    if (app.arguments().contains(QStringLiteral("--telephony-self-test"))) {
        TelephonyEngine telephony;
        return telephony.diagnostics().value(QStringLiteral("available")).toBool() ? 0 : 3;
    }
#endif
    QQmlApplicationEngine engine;
    engine.rootContext()->setContextProperty(QStringLiteral("backend"), &controller);
    engine.rootContext()->setContextProperty(QStringLiteral("platform"), &platform);
#ifdef THSIP_HAVE_PJSIP
    TelephonyEngine telephony;
    engine.rootContext()->setContextProperty(QStringLiteral("telephony"), &telephony);
    QObject::connect(&telephony, &TelephonyEngine::recordingState, &controller,
                     [&controller](const QString &callId, bool recording, const QString &path) {
                         if (recording) controller.registerLocalRecording(callId, path);
                     });
#endif
    QObject::connect(&engine, &QQmlApplicationEngine::objectCreationFailed, &app,
                     [] { QCoreApplication::exit(1); }, Qt::QueuedConnection);
    engine.loadFromModule(QStringLiteral("THsip"), QStringLiteral("Main"));
    if (app.arguments().contains(QStringLiteral("--qml-self-test")))
        return engine.rootObjects().isEmpty() ? 7 : 0;
    return app.exec();
}
