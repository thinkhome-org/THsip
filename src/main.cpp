#include "backend.h"
#include "platformintegration.h"

#include <QGuiApplication>
#include <QFile>
#include <QSqlQuery>
#include <QQmlApplicationEngine>
#include <QQmlContext>

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
    return app.exec();
}
