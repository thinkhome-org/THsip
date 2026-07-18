#include "contactstore.h"

#include <Contacts/Contacts.h>
#include <QMetaObject>
#include <QPointer>

void requestSystemContacts(QObject *context, ContactStoreCallback callback)
{
    QPointer<QObject> guard(context);
    CNContactStore *store = [[CNContactStore alloc] init];
    [store requestAccessForEntityType:CNEntityTypeContacts completionHandler:^(BOOL granted, NSError *permissionError) {
        QVariantList contacts;
        QString error;
        if (!granted) {
            error = permissionError ? QString::fromNSString(permissionError.localizedDescription)
                                    : QStringLiteral("Přístup ke kontaktům nebyl povolen");
        } else {
            NSArray *keys = @[CNContactIdentifierKey, [CNContactFormatter descriptorForRequiredKeysForStyle:CNContactFormatterStyleFullName],
                              CNContactOrganizationNameKey, CNContactPhoneNumbersKey];
            CNContactFetchRequest *request = [[CNContactFetchRequest alloc] initWithKeysToFetch:keys];
            NSError *fetchError = nil;
            QVariantList *contactsOut = &contacts;
            [store enumerateContactsWithFetchRequest:request error:&fetchError usingBlock:^(CNContact *contact, BOOL *) {
                QVariantList numbers;
                for (CNLabeledValue<CNPhoneNumber *> *entry in contact.phoneNumbers) {
                    numbers << QVariantMap{{QStringLiteral("number"), QString::fromNSString(entry.value.stringValue)},
                                           {QStringLiteral("label"), QString::fromNSString([CNLabeledValue localizedStringForLabel:entry.label] ?: @"")}};
                }
                if (!numbers.isEmpty())
                    *contactsOut << QVariantMap{{QStringLiteral("id"), QStringLiteral("mac:") + QString::fromNSString(contact.identifier)},
                                            {QStringLiteral("name"), QString::fromNSString([CNContactFormatter stringFromContact:contact style:CNContactFormatterStyleFullName] ?: @"")},
                                            {QStringLiteral("organization"), QString::fromNSString(contact.organizationName ?: @"")},
                                            {QStringLiteral("numbers"), numbers}};
            }];
            if (fetchError) error = QString::fromNSString(fetchError.localizedDescription);
        }
        if (guard) QMetaObject::invokeMethod(guard, [guard, callback, contacts = std::move(contacts), error = std::move(error)]() mutable {
            if (guard) callback(std::move(contacts), std::move(error));
        }, Qt::QueuedConnection);
    }];
}
