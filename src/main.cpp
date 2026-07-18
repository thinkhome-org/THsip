#include "backend.h"
#include "platformintegration.h"

#include <QGuiApplication>
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
    PlatformIntegration platform;
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
