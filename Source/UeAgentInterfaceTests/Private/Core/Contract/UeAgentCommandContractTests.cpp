#include "Misc/AutomationTest.h"

#include "Core/Contract/UeAgentCommandContract.h"


#include "Dom/JsonObject.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"

using namespace UE::UAI::CommandContract;

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUeAgentCommandContractRegistryTest,
	"UeAgentInterface.CommandEdge.Contract.Registry",
	EAutomationTestFlags::EditorContext
		| EAutomationTestFlags::EngineFilter)

bool FUeAgentCommandContractRegistryTest::RunTest(
	const FString& Parameters)
{
	const TSharedRef<const FUeAgentCommandContractRegistry> Registry =
		GetDefaultCommandContractRegistry();
	TestEqual(
		TEXT("base registry exposes 3 reference, 46 query and 1 export commands"),
		Registry->GetContracts().Num(),
		50);
	TestTrue(
		TEXT("registry hash is SHA-256"),
		Registry->GetRegistryHash().StartsWith(TEXT("sha256:"))
			&& Registry->GetRegistryHash().Len() == 71);

	const FUeAgentCommandContract* Query =
		Registry->FindExact(TEXT("reference.catalog_search"));
	const FUeAgentCommandContract* Read =
		Registry->FindExact(TEXT("reference.asset_read"));
	const FUeAgentCommandContract* Effect =
		Registry->FindExact(TEXT("reference.asset_apply"));
	TestNotNull(TEXT("catalog search contract"), Query);
	TestNotNull(TEXT("asset read contract"), Read);
	TestNotNull(TEXT("asset apply contract"), Effect);
	if (!Query || !Read || !Effect)
	{
		return false;
	}
	TestEqual(
		TEXT("query namespace"),
		Query->GetNamespace(),
		FString(TEXT("reference")));
	TestEqual(
		TEXT("read method"),
		Read->GetMethod(),
		FString(TEXT("asset_read")));
	TestEqual(
		TEXT("read has one object path parameter"),
		Read->GetParameters().Num(),
		1);
	TestEqual(
		TEXT("read object path is required"),
		Read->GetParameters()[0].GetName(),
		FString(TEXT("object_path")));
	TestTrue(
		TEXT("read object path is required and not nullable"),
		Read->GetParameters()[0].IsRequired()
			&& !Read->GetParameters()[0].IsNullable());
	TestEqual(
		TEXT("effect method"),
		Effect->GetMethod(),
		FString(TEXT("asset_apply")));
	TestTrue(
		TEXT("command-specific binding hashes differ"),
		Query->GetBindingHash() != Read->GetBindingHash()
			&& Read->GetBindingHash() != Effect->GetBindingHash());

	TArray<FUeAgentCommandContract> DuplicateContracts =
		Registry->GetContracts();
	const FUeAgentCommandContract Duplicate =
		DuplicateContracts[0];
	DuplicateContracts.Add(Duplicate);
	TSharedPtr<const FUeAgentCommandContractRegistry> Rejected;
	FString Error;
	TestFalse(
		TEXT("duplicate exact command ID is rejected"),
		FUeAgentCommandContractRegistry::TryCreate(
			MoveTemp(DuplicateContracts),
			Rejected,
			Error));
	TestEqual(
		TEXT("duplicate rejection is stable"),
		Error,
		FString(TEXT("command_contract_id_duplicate")));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUeAgentCommandManifestProjectionTest,
	"UeAgentInterface.CommandEdge.Contract.ManifestProjection",
	EAutomationTestFlags::EditorContext
		| EAutomationTestFlags::EngineFilter)

bool FUeAgentCommandManifestProjectionTest::RunTest(
	const FString& Parameters)
{
	const TSharedRef<const FUeAgentCommandContractRegistry> Registry =
		GetDefaultCommandContractRegistry();
	FString Manifest;
	FString Error;
	TestTrue(
		TEXT("registry deterministically builds its manifest"),
		Registry->BuildManifest(
			GetUaiCommandManifestPluginVersion(),
			GetUaiCommandManifestBuildId(),
			Manifest,
			Error));
	FString SecondManifest;
	TestTrue(
		TEXT("second projection succeeds"),
		Registry->BuildManifest(
			GetUaiCommandManifestPluginVersion(),
			GetUaiCommandManifestBuildId(),
			SecondManifest,
			Error));
	TestEqual(
		TEXT("manifest bytes are deterministic"),
		SecondManifest,
		Manifest);

	TSharedPtr<FJsonObject> Root;
	const TSharedRef<TJsonReader<>> Reader =
		TJsonReaderFactory<>::Create(Manifest);
	TestTrue(
		TEXT("manifest is valid JSON"),
		FJsonSerializer::Deserialize(
			Reader,
			Root,
			FJsonSerializer::EFlags::StoreNumbersAsStrings)
			&& Root.IsValid());
	if (!Root.IsValid())
	{
		return false;
	}
	TestEqual(
		TEXT("manifest source is C++ registry export"),
		Root->GetObjectField(TEXT("source"))
			->GetStringField(TEXT("kind")),
		FString(TEXT("cpp_command_registry_export")));
	const TArray<TSharedPtr<FJsonValue>>& Commands =
		Root->GetArrayField(TEXT("commands"));
	TestEqual(
		TEXT("manifest contains 3 reference, 46 query and 1 export commands"),
		Commands.Num(),
		50);
	const TSet<FString> ForbiddenFields{
		TEXT("descriptor_id"),
		TEXT("descriptor_hash"),
		TEXT("route"),
		TEXT("route_path"),
		TEXT("default"),
		TEXT("success_condition"),
		TEXT("verification"),
	};
	for (const TSharedPtr<FJsonValue>& Value : Commands)
	{
		const TSharedPtr<FJsonObject> Command = Value->AsObject();
		for (const FString& Forbidden : ForbiddenFields)
		{
			TestFalse(
				TEXT("manifest excludes business and transport truth"),
				Command->HasField(Forbidden));
		}
	}

	return true;
}

