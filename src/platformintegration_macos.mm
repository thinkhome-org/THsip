#include "platformintegration.h"

#include <QDateTime>

#import <AppKit/AppKit.h>
#import <AVFoundation/AVFoundation.h>
#import <IOKit/pwr_mgt/IOPMLib.h>
#import <UserNotifications/UserNotifications.h>

@interface THsipNotificationDelegate : NSObject <UNUserNotificationCenterDelegate>
@end
@implementation THsipNotificationDelegate
- (void)userNotificationCenter:(UNUserNotificationCenter *)center willPresentNotification:(UNNotification *)notification
         withCompletionHandler:(void (^)(UNNotificationPresentationOptions options))completionHandler
{
    Q_UNUSED(center); Q_UNUSED(notification);
    completionHandler(UNNotificationPresentationOptionBanner | UNNotificationPresentationOptionSound);
}
@end

static THsipNotificationDelegate *notificationDelegate()
{
    static THsipNotificationDelegate *delegate = [THsipNotificationDelegate new];
    return delegate;
}

PlatformIntegration::PlatformIntegration(QObject *parent) : QObject(parent)
{
    [UNUserNotificationCenter currentNotificationCenter].delegate = notificationDelegate();
}

PlatformIntegration::~PlatformIntegration()
{
    clearIncomingCall();
    preventSleepDuringCall(false);
}

void PlatformIntegration::requestPermissions()
{
    [[UNUserNotificationCenter currentNotificationCenter]
        requestAuthorizationWithOptions:(UNAuthorizationOptionAlert | UNAuthorizationOptionSound | UNAuthorizationOptionBadge)
        completionHandler:^(BOOL, NSError *) {}];
    [AVCaptureDevice requestAccessForMediaType:AVMediaTypeAudio completionHandler:^(BOOL) {}];
    [AVCaptureDevice requestAccessForMediaType:AVMediaTypeVideo completionHandler:^(BOOL) {}];
}

void PlatformIntegration::showIncomingCall(const QString &caller)
{
    auto *content = [UNMutableNotificationContent new];
    content.title = @"Příchozí Odorik hovor";
    content.body = caller.toNSString();
    content.sound = [UNNotificationSound defaultSound];
    const QString identifier = QStringLiteral("incoming-%1").arg(QDateTime::currentMSecsSinceEpoch());
    auto *request = [UNNotificationRequest requestWithIdentifier:identifier.toNSString() content:content trigger:nil];
    [[UNUserNotificationCenter currentNotificationCenter] addNotificationRequest:request withCompletionHandler:nil];
    NSApp.dockTile.badgeLabel = @"1";
    [NSApp requestUserAttention:NSCriticalRequest];
    NSSound *sound = [NSSound soundNamed:@"Submarine"];
    sound.loops = YES;
    [sound play];
}

void PlatformIntegration::clearIncomingCall()
{
    NSApp.dockTile.badgeLabel = nil;
    [[NSSound soundNamed:@"Submarine"] stop];
    [[UNUserNotificationCenter currentNotificationCenter] removeAllDeliveredNotifications];
}

void PlatformIntegration::preventSleepDuringCall(bool enabled)
{
    if (enabled && !sleepAssertion_)
        IOPMAssertionCreateWithName(kIOPMAssertionTypeNoIdleSleep, kIOPMAssertionLevelOn, CFSTR("THsip active call"), &sleepAssertion_);
    else if (!enabled && sleepAssertion_) { IOPMAssertionRelease(sleepAssertion_); sleepAssertion_ = 0; }
}
