#include "Misc/AutomationTest.h"

#include "CommandEdge/UeAgentExactCommandDispatch.h"
#include "CommandRegistry/UeAgentActiveCommandManifest.h"
#include "CommandRegistry/UeAgentImmutableCommandRegistry.h"
#include "Core/Contract/Command/UeAgentCommandContractModel.h"
#include "Dom/JsonObject.h"
#include "Module/CommandRegistration/Active/UeAgentActiveCommandComposition.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"

namespace
{
	using namespace UE::UAI::CommandContract;
	using namespace UE::UAI::CommandEdge;
	using namespace UE::UAI::CommandRegistry;
	using namespace UE::UAI::Module::CommandRegistration::Active;

	bool TryGetProcessRegistry(
		TSharedPtr<const FUeAgentImmutableCommandRegistry>& OutRegistry,
		TSharedPtr<const FUeAgentExactCommandDispatch>& OutDispatch,
		FString& OutError)
	{
		return TryComposeProcessActiveCommandComposition(
			OutRegistry,
			OutDispatch,
			OutError);
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUeAgentCommandContractRegistryTest,
	"UeAgentInterface.CommandEdge.Contract.Registry",
	EAutomationTestFlags::EditorContext
		| EAutomationTestFlags::EngineFilter)

bool FUeAgentCommandContractRegistryTest::RunTest(
	const FString& Parameters)
{
	(void)Parameters;
	TSharedPtr<const FUeAgentImmutableCommandRegistry> Registry;
	TSharedPtr<const FUeAgentExactCommandDispatch> Dispatch;
	FString Error;
	if (!TestTrue(
			TEXT("process immutable command authority is available"),
			TryGetProcessRegistry(Registry, Dispatch, Error))
		|| !Registry.IsValid()
		|| !Dispatch.IsValid())
	{
		AddError(Error);
		return false;
	}

	TestTrue(
		TEXT("active registry is non-empty"),
		!Registry->GetContracts().IsEmpty());
	TestTrue(
		TEXT("registry hash is SHA-256"),
		Registry->GetRegistryHash().StartsWith(TEXT("sha256:"))
			&& Registry->GetRegistryHash().Len() == 71);

	const FUeAgentFrozenCommandContract* Search =
		Registry->FindExact(TEXT("query.command.search"));
	const FUeAgentFrozenCommandContract* Detail =
		Registry->FindExact(TEXT("query.command.detail"));
	const FUeAgentFrozenCommandContract* Read =
		Registry->FindExact(TEXT("reference.asset_read"));
	const FUeAgentFrozenCommandContract* Effect =
		Registry->FindExact(TEXT("reference.asset_apply"));
	TestNotNull(TEXT("command search contract"), Search);
	TestNotNull(TEXT("command detail contract"), Detail);
	TestNotNull(TEXT("asset read contract"), Read);
	TestNotNull(TEXT("asset apply contract"), Effect);
	TestNull(
		TEXT("retired global reference catalog is absent"),
		Registry->FindExact(TEXT("reference.catalog_search")));
	if (!Search || !Detail || !Read || !Effect)
	{
		return false;
	}

	TestEqual(
		TEXT("search namespace"),
		Search->GetPythonNamespace(),
		FString(TEXT("query")));
	TestEqual(
		TEXT("search method"),
		Search->GetPythonFunction(),
		FString(TEXT("command_search")));
	TestEqual(
		TEXT("detail method"),
		Detail->GetPythonFunction(),
		FString(TEXT("command_detail")));
	TestEqual(
		TEXT("read method"),
		Read->GetPythonFunction(),
		FString(TEXT("asset_read")));
	TestEqual(
		TEXT("read exposes object path plus optional context/projection"),
		Read->GetInputFields().Num(),
		3);
	TestEqual(
		TEXT("read object path field"),
		Read->GetInputFields()[0].Name,
		FString(TEXT("object_path")));
	TestTrue(
		TEXT("read object path is required and not nullable"),
		Read->GetInputFields()[0].bRequired
			&& !Read->GetInputFields()[0].bNullable);
	TestEqual(
		TEXT("effect method"),
		Effect->GetPythonFunction(),
		FString(TEXT("asset_apply")));
	TestTrue(
		TEXT("command-specific contract hashes differ"),
		Search->GetContractHash() != Read->GetContractHash()
			&& Read->GetContractHash() != Effect->GetContractHash());

	TSet<FString> CommandIds;
	for (const FUeAgentFrozenCommandContract& Contract :
		Registry->GetContracts())
	{
		CommandIds.Add(Contract.GetCommandId());
	}
	TestEqual(
		TEXT("active command identities are unique"),
		CommandIds.Num(),
		Registry->GetContracts().Num());
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
	(void)Parameters;
	TSharedPtr<const FUeAgentImmutableCommandRegistry> Registry;
	TSharedPtr<const FUeAgentExactCommandDispatch> Dispatch;
	FString Error;
	if (!TestTrue(
			TEXT("process immutable command authority is available"),
			TryGetProcessRegistry(Registry, Dispatch, Error))
		|| !Registry.IsValid())
	{
		AddError(Error);
		return false;
	}

	FString Manifest;
	TestTrue(
		TEXT("active registry deterministically builds its manifest"),
		FUeAgentActiveCommandManifest::TryBuild(
			Registry.ToSharedRef(),
			GetUaiCommandManifestPluginVersion(),
			GetUaiCommandManifestBuildId(),
			Manifest,
			Error));
	FString SecondManifest;
	TestTrue(
		TEXT("second projection succeeds"),
		FUeAgentActiveCommandManifest::TryBuild(
			Registry.ToSharedRef(),
			GetUaiCommandManifestPluginVersion(),
			GetUaiCommandManifestBuildId(),
			SecondManifest,
			Error));
	TestEqual(
		TEXT("manifest bytes are deterministic"),
		SecondManifest,
		Manifest);
	TestTrue(
		TEXT("committed manifest equals the active registry projection"),
		FUeAgentActiveCommandManifest::TryVerify(
			Registry.ToSharedRef(),
			GetUaiCommandManifestPluginVersion(),
			GetUaiCommandManifestBuildId(),
			GetDefaultCommandManifestPath(),
			Error));
	if (!Error.IsEmpty())
	{
		AddError(Error);
	}

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
		TEXT("manifest source is the immutable C++ registry"),
		Root->GetObjectField(TEXT("source"))
			->GetStringField(TEXT("kind")),
		FString(TEXT("cpp_immutable_command_registry_export")));
	const TArray<TSharedPtr<FJsonValue>>& Commands =
		Root->GetArrayField(TEXT("commands"));
	TestEqual(
		TEXT("manifest contains exactly the active registry commands"),
		Commands.Num(),
		Registry->GetContracts().Num());
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

