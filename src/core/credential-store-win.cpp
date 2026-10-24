// SPDX-License-Identifier: GPL-2.0-or-later
// MatchFrame Multistream - Copyright (C) 2026 MatchFrame contributors

#include "credential-store.hpp"
#include "redaction.hpp"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <wincred.h>

#include <string>

namespace mf {

namespace {

std::wstring widen(const std::string &utf8)
{
	if (utf8.empty())
		return {};
	int length = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, utf8.data(), (int)utf8.size(), nullptr, 0);
	if (length <= 0)
		return {};
	std::wstring out((size_t)length, L'\0');
	MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, utf8.data(), (int)utf8.size(), out.data(), length);
	return out;
}

std::string describe(DWORD error)
{
	switch (error) {
	case ERROR_NOT_FOUND:
		return "Not stored.";
	case ERROR_NO_SUCH_LOGON_SESSION:
		return "Windows Credential Manager is not available for this logon session.";
	case ERROR_INVALID_PARAMETER:
	case ERROR_INVALID_FLAGS:
		return "Windows Credential Manager rejected the request.";
	case ERROR_BAD_USERNAME:
		return "Windows Credential Manager rejected the entry name.";
	default:
		return "Windows Credential Manager error " + std::to_string((unsigned long)error) + ".";
	}
}

/*
 * Generic credentials persisted per user on this machine (CRED_PERSIST_LOCAL_MACHINE: not
 * roamed to other computers). The blob is the UTF-8 secret; Credential Manager protects it
 * with the user's logon credentials (DPAPI).
 */
class WindowsCredentialStore : public CredentialStore {
public:
	bool available() const override { return true; }
	std::string backendName() const override { return "Windows Credential Manager"; }

	CredResult write(const std::string &target, const std::string &secret) override
	{
		if (secret.size() > CRED_MAX_CREDENTIAL_BLOB_SIZE)
			return {CredStatus::Failed, "The secret is too long for Windows Credential Manager."};

		std::wstring targetName = widen(target);
		std::wstring userName = L"MatchFrame Multistream";
		std::wstring comment = L"Stream credential stored by the MatchFrame Multistream OBS plugin";

		CREDENTIALW credential = {};
		credential.Type = CRED_TYPE_GENERIC;
		credential.TargetName = targetName.data();
		credential.Comment = comment.data();
		credential.CredentialBlobSize = (DWORD)secret.size();
		credential.CredentialBlob = (LPBYTE)secret.data();
		credential.Persist = CRED_PERSIST_LOCAL_MACHINE;
		credential.UserName = userName.data();

		if (!CredWriteW(&credential, 0))
			return {CredStatus::Failed, describe(GetLastError())};
		return {CredStatus::Ok, {}};
	}

	CredResult read(const std::string &target, std::string &secret) override
	{
		std::wstring targetName = widen(target);
		PCREDENTIALW credential = nullptr;
		if (!CredReadW(targetName.c_str(), CRED_TYPE_GENERIC, 0, &credential)) {
			DWORD error = GetLastError();
			return {error == ERROR_NOT_FOUND ? CredStatus::NotFound : CredStatus::Failed, describe(error)};
		}
		secret.assign((const char *)credential->CredentialBlob, credential->CredentialBlobSize);
		SecureZeroMemory(credential->CredentialBlob, credential->CredentialBlobSize);
		CredFree(credential);
		return {CredStatus::Ok, {}};
	}

	CredResult remove(const std::string &target) override
	{
		std::wstring targetName = widen(target);
		if (!CredDeleteW(targetName.c_str(), CRED_TYPE_GENERIC, 0)) {
			DWORD error = GetLastError();
			return {error == ERROR_NOT_FOUND ? CredStatus::NotFound : CredStatus::Failed, describe(error)};
		}
		return {CredStatus::Ok, {}};
	}
};

} // namespace

std::unique_ptr<CredentialStore> create_platform_credential_store()
{
	return std::make_unique<WindowsCredentialStore>();
}

} // namespace mf
