// SPDX-License-Identifier: GPL-2.0-or-later
// MatchFrame Multistream - Copyright (C) 2026 MatchFrame contributors

#include "credential-store.hpp"

#include <CoreFoundation/CoreFoundation.h>
#include <Security/Security.h>

#include <string>

namespace mf {

namespace {

constexpr const char *kService = "MatchFrame Multistream";

/* Owns one Core Foundation reference. */
template<typename T> class CFRef {
public:
	explicit CFRef(T ref = nullptr) : ref_(ref) {}
	~CFRef()
	{
		if (ref_)
			CFRelease(ref_);
	}
	CFRef(const CFRef &) = delete;
	CFRef &operator=(const CFRef &) = delete;
	T get() const { return ref_; }
	T *out() { return &ref_; }

private:
	T ref_;
};

CFStringRef make_string(const std::string &utf8)
{
	return CFStringCreateWithBytes(kCFAllocatorDefault, (const UInt8 *)utf8.data(), (CFIndex)utf8.size(),
				       kCFStringEncodingUTF8, false);
}

std::string describe(OSStatus status)
{
	switch (status) {
	case errSecItemNotFound:
		return "Not stored.";
	case errSecInteractionNotAllowed:
		return "The macOS Keychain is locked or cannot be used by OBS right now.";
	case errSecAuthFailed:
	case errSecUserCanceled:
		return "Access to the macOS Keychain was denied.";
	case errSecNoSuchKeychain:
	case errSecNotAvailable:
		return "No usable macOS Keychain is available for this user.";
	default:
		break;
	}
	std::string text = "macOS Keychain error " + std::to_string((long)status);
	CFRef<CFStringRef> message(SecCopyErrorMessageString(status, nullptr));
	char buffer[256];
	if (message.get() && CFStringGetCString(message.get(), buffer, sizeof(buffer), kCFStringEncodingUTF8))
		text += std::string(": ") + buffer;
	return text + ".";
}

/* The attributes that identify one item: generic password, our service, the target as account. */
CFMutableDictionaryRef make_query(const std::string &target)
{
	CFMutableDictionaryRef query = CFDictionaryCreateMutable(kCFAllocatorDefault, 0, &kCFTypeDictionaryKeyCallBacks,
								 &kCFTypeDictionaryValueCallBacks);
	CFRef<CFStringRef> service(make_string(kService));
	CFRef<CFStringRef> account(make_string(target));
	CFDictionarySetValue(query, kSecClass, kSecClassGenericPassword);
	CFDictionarySetValue(query, kSecAttrService, service.get());
	CFDictionarySetValue(query, kSecAttrAccount, account.get());
	return query;
}

/*
 * Generic passwords in the user's login keychain, which macOS encrypts with the login password
 * and never syncs (only the data-protection keychain syncs, and that needs app entitlements a
 * plugin cannot add). Items are created by OBS, so OBS can read them back without a prompt.
 */
class MacKeychainStore : public CredentialStore {
public:
	bool available() const override { return true; }
	std::string backendName() const override { return "macOS Keychain"; }

	CredResult write(const std::string &target, const std::string &secret) override
	{
		CFRef<CFMutableDictionaryRef> query(make_query(target));
		CFRef<CFDataRef> data(
			CFDataCreate(kCFAllocatorDefault, (const UInt8 *)secret.data(), (CFIndex)secret.size()));
		if (!query.get() || !data.get())
			return {CredStatus::Failed, "Out of memory."};

		CFRef<CFMutableDictionaryRef> update(CFDictionaryCreateMutable(
			kCFAllocatorDefault, 0, &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks));
		CFDictionarySetValue(update.get(), kSecValueData, data.get());
		OSStatus status = SecItemUpdate(query.get(), update.get());
		if (status == errSecItemNotFound) {
			CFRef<CFStringRef> label(make_string("MatchFrame Multistream stream credential"));
			CFRef<CFStringRef> description(
				make_string("Stream credential stored by the MatchFrame Multistream OBS plugin"));
			CFDictionarySetValue(query.get(), kSecValueData, data.get());
			CFDictionarySetValue(query.get(), kSecAttrLabel, label.get());
			CFDictionarySetValue(query.get(), kSecAttrDescription, description.get());
			status = SecItemAdd(query.get(), nullptr);
		}
		if (status != errSecSuccess)
			return {CredStatus::Failed, describe(status)};
		return {CredStatus::Ok, {}};
	}

	CredResult read(const std::string &target, std::string &secret) override
	{
		CFRef<CFMutableDictionaryRef> query(make_query(target));
		if (!query.get())
			return {CredStatus::Failed, "Out of memory."};
		CFDictionarySetValue(query.get(), kSecReturnData, kCFBooleanTrue);
		CFDictionarySetValue(query.get(), kSecMatchLimit, kSecMatchLimitOne);
		CFRef<CFTypeRef> result;
		OSStatus status = SecItemCopyMatching(query.get(), result.out());
		if (status != errSecSuccess)
			return {status == errSecItemNotFound ? CredStatus::NotFound : CredStatus::Failed,
				describe(status)};
		if (!result.get() || CFGetTypeID(result.get()) != CFDataGetTypeID())
			return {CredStatus::Failed, "The macOS Keychain returned an unexpected item."};
		CFDataRef data = (CFDataRef)result.get();
		secret.assign((const char *)CFDataGetBytePtr(data), (size_t)CFDataGetLength(data));
		return {CredStatus::Ok, {}};
	}

	CredResult remove(const std::string &target) override
	{
		CFRef<CFMutableDictionaryRef> query(make_query(target));
		if (!query.get())
			return {CredStatus::Failed, "Out of memory."};
		OSStatus status = SecItemDelete(query.get());
		if (status != errSecSuccess)
			return {status == errSecItemNotFound ? CredStatus::NotFound : CredStatus::Failed,
				describe(status)};
		return {CredStatus::Ok, {}};
	}
};

} // namespace

std::unique_ptr<CredentialStore> create_platform_credential_store()
{
	return std::make_unique<MacKeychainStore>();
}

} // namespace mf
