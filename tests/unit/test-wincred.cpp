// SPDX-License-Identifier: GPL-2.0-or-later
// MatchFrame Multistream - Copyright (C) 2026 MatchFrame contributors
//
// Windows only: exercises the real Windows Credential Manager backend.

#include "common/test-framework.hpp"

#include "core/credential-store.hpp"
#include "core/profile-config.hpp"

using namespace mf;

MF_TEST(wincred_roundtrip_overwrite_delete)
{
	std::unique_ptr<CredentialStore> store = create_platform_credential_store();
	REQUIRE(store->available());
	CHECK_EQ(store->backendName(), std::string("Windows Credential Manager"));

	std::string target = credential_target("ci-" + new_uuid(), new_uuid(), SecretField::StreamKey);
	std::string value;
	CHECK(store->read(target, value).status == CredStatus::NotFound);

	REQUIRE(store->write(target, "live_abc123_SECRET").ok());
	REQUIRE(store->read(target, value).ok());
	CHECK_EQ(value, std::string("live_abc123_SECRET"));

	/* Overwrite in place; UTF-8 survives the round trip. */
	std::string unicode = "cl\xC3\xA9-\xE2\x82\xAC-key";
	REQUIRE(store->write(target, unicode).ok());
	REQUIRE(store->read(target, value).ok());
	CHECK_EQ(value, unicode);

	REQUIRE(store->remove(target).ok());
	CHECK(store->read(target, value).status == CredStatus::NotFound);
	CHECK(store->remove(target).status == CredStatus::NotFound);
}

MF_TEST(wincred_rejects_oversized_secret)
{
	std::unique_ptr<CredentialStore> store = create_platform_credential_store();
	std::string target = credential_target("ci-" + new_uuid(), new_uuid(), SecretField::Password);
	std::string huge(6000, 'x');
	CredResult result = store->write(target, huge);
	CHECK(!result.ok());
	CHECK(result.message.find("too long") != std::string::npos);
	std::string value;
	CHECK(store->read(target, value).status == CredStatus::NotFound);
}

MF_TEST(wincred_vault_persists_across_vault_instances)
{
	std::unique_ptr<CredentialStore> store = create_platform_credential_store();
	std::string profile = "ci-" + new_uuid();
	std::string destination = new_uuid();
	SecretVault::StoreResult stored;
	{
		SecretVault vault(store.get());
		stored = vault.store(profile, destination, SecretField::StreamKey, "persisted-key", false);
		REQUIRE(stored.location == SecretLocation::Persistent);
	}
	/* A new vault (as after an OBS restart) finds the key through the reference only. */
	SecretVault restarted(store.get());
	std::string value;
	SecretLocation where = SecretLocation::None;
	REQUIRE(restarted.load(profile, destination, SecretField::StreamKey, stored.target, value, &where).ok());
	CHECK(where == SecretLocation::Persistent);
	CHECK_EQ(value, std::string("persisted-key"));
	restarted.remove(profile, destination, SecretField::StreamKey, stored.target);
	CHECK(store->read(stored.target, value).status == CredStatus::NotFound);
}
