#include "Misc/AutomationTest.h"

#include "Core/AssetDurable/UeAgentAssetDurable.h"
#include "Core/Contract/UeAgentContractSchema.h"
#include "Core/Contract/UeAgentDescriptorRegistry.h"
#include "DomainAdapters/WorldSimpleEffect/UeAgentWorldSimpleEffectCapabilities.h"



namespace
{
	using namespace UE::UAI::AssetDurable;
	using namespace UE::UAI::Contract;
	using namespace UE::UAI::DomainAdapters::WorldSimpleEffect;
	using namespace UE::UAI::StateSemantic;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUeAgentSimpleAuthoringCompiledSchemaAccessTest,
	"UeAgentInterface.Composition.SimpleAuthoring.E3."
		"CompiledOperationSchemaAccess",
	EAutomationTestFlags::EditorContext
		| EAutomationTestFlags::EngineFilter)

bool FUeAgentSimpleAuthoringCompiledSchemaAccessTest::RunTest(
	const FString& Parameters)
{
	FString Error;
	TSharedPtr<const FUeAgentDescriptorRegistry> Registry;
	TestTrue(
		TEXT("production operation registry composes"),
		TryCreateWorldSimpleEffectDescriptorRegistry(
			Registry,
			Error));
	TestTrue(TEXT("operation registry exists"), Registry.IsValid());
	if (!Registry.IsValid())
	{
		AddError(TEXT("sa008_operation_registry_invalid:") + Error);
		return false;
	}
	const FUeAgentOperationDescriptor& Operation =
		Registry->GetDescriptors()[0];

	const FUeAgentContractSchema* Schema =
		Registry->FindCompiledInputSchemaExact(Operation.GetIdentity());
	TestNotNull(
		TEXT("exact operation descriptor exposes its registry-owned compiled schema"),
		Schema);
	if (Schema)
	{
		TOptional<FUeAgentContractValue> Canonical;
		FUeAgentContractSchemaError SchemaError;
		TestFalse(
			TEXT("compiled operation schema rejects an incomplete public payload"),
			Schema->TryValidateAndCanonicalize(
				FUeAgentContractValue(TEXT("{}")),
				[](int64,
					const FString&,
					FUeAgentContractSchemaError&)
				{
					return true;
				},
				Canonical,
				SchemaError));
		TestTrue(
			TEXT("schema rejection is typed"),
			SchemaError.IsValid());
		TestFalse(
			TEXT("rejected payload has no canonical value"),
			Canonical.IsSet());
	}

	const FUeAgentDescriptorIdentity WrongHash(
		Operation.GetIdentity().GetDescriptorId(),
		Operation.GetIdentity().GetVersion(),
		TEXT("sha256:wrong"));
	TestTrue(
		TEXT("hash mismatch cannot access the compiled schema"),
		Registry->FindCompiledInputSchemaExact(WrongHash) == nullptr);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUeAgentSimpleAuthoringTypedDurableArtifactTest,
	"UeAgentInterface.Composition.SimpleAuthoring.E3."
		"TypedDurableArtifactContract",
	EAutomationTestFlags::EditorContext
		| EAutomationTestFlags::EngineFilter)

bool FUeAgentSimpleAuthoringTypedDurableArtifactTest::RunTest(
	const FString& Parameters)
{
	const FUeAgentCanonicalIdentity Identity(
		TEXT("sa008-config"),
		TEXT("project_config"),
		TEXT("GptProjectTest"),
		TEXT("/Script/UeAgentInterfaceTests.SA008"),
		TEXT("config_section"),
		TEXT("revision-1"));
	const FAssetDurableTarget Target(
		Identity,
		EDurableArtifactKind::ProjectConfig,
		TEXT("Config/SA008Test.ini"),
		EAssetDurableIntent::Save,
		ERollbackGrade::DurableBackup);
	TestTrue(TEXT("typed config target is valid"), Target.IsValid());
	TestEqual(
		TEXT("artifact kind is explicit"),
		Target.GetArtifactKind(),
		EDurableArtifactKind::ProjectConfig);
	TestEqual(
		TEXT("durable location is not disguised as a package name"),
		Target.GetDurableLocation(),
		FString(TEXT("Config/SA008Test.ini")));
	TestTrue(
		TEXT("config target has no package name"),
		Target.GetPackageName().IsEmpty());
	return true;
}
