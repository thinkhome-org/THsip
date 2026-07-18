#include "secretstore.h"

#import <Foundation/Foundation.h>
#import <Security/Security.h>

namespace {
NSString *service() { return @"cz.thinkhome.thsip"; }
NSString *string(const QString &value) { return [NSString stringWithUTF8String:value.toUtf8().constData()]; }
}

bool SecretStore::save(const QString &account, const QString &secret)
{
    NSDictionary *identity = @{(__bridge id)kSecClass:(__bridge id)kSecClassGenericPassword,
                               (__bridge id)kSecAttrService:service(),
                               (__bridge id)kSecAttrAccount:string(account)};
    SecItemDelete((__bridge CFDictionaryRef)identity);
    if (secret.isEmpty()) return true;
    NSMutableDictionary *item = [identity mutableCopy];
    item[(__bridge id)kSecValueData] = [string(secret) dataUsingEncoding:NSUTF8StringEncoding];
    item[(__bridge id)kSecAttrAccessible] = (__bridge id)kSecAttrAccessibleAfterFirstUnlockThisDeviceOnly;
    return SecItemAdd((__bridge CFDictionaryRef)item, nullptr) == errSecSuccess;
}

QString SecretStore::load(const QString &account)
{
    NSDictionary *query = @{(__bridge id)kSecClass:(__bridge id)kSecClassGenericPassword,
                            (__bridge id)kSecAttrService:service(),
                            (__bridge id)kSecAttrAccount:string(account),
                            (__bridge id)kSecReturnData:@YES,
                            (__bridge id)kSecMatchLimit:(__bridge id)kSecMatchLimitOne};
    CFTypeRef result = nullptr;
    if (SecItemCopyMatching((__bridge CFDictionaryRef)query, &result) != errSecSuccess) return {};
    NSData *data = CFBridgingRelease(result);
    return QString::fromUtf8(static_cast<const char *>(data.bytes), qsizetype(data.length));
}
