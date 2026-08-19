#include "Misc/AutomationTest.h"

#include "Core/Contract/UeAgentContractSchema.h"
#include "Core/Contract/UeAgentDescriptorRegistry.h"

namespace
{
	using namespace UE::UAI::Contract;

	FUeAgentExecutionContextPolicy QueryPolicy()
	{
		return FUeAgentExecutionContextPolicy(
			{EUeAgentStatePlane::AuthoringMemory},
			{EUeAgentExecutionContextFacet::World},
			EUeAgentContextResolutionPolicy::ExplicitOnly,
			EUeAgentTransientContextPolicy::ReadOnly);
	}

	FUeAgentContractValue StrictInputSchema()
	{
		return FUeAgentContractValue(TEXT(
			"{\"additionalProperties\":false,"
			"\"properties\":{"
				"\"filter\":{\"items\":{\"type\":\"string\"},"
					"\"maxItems\":4,\"type\":\"array\"},"
				"\"mode\":{\"enum\":[\"path\",\"ray\"],"
					"\"type\":\"string\"},"
				"\"path\":{\"minLength\":1,\"type\":\"string\"}},"
			"\"required\":[\"mode\",\"path\"],"
			"\"type\":\"object\"}"));
	}

	auto CountingCheckpoint(int64& Visits)
	{
		return [&Visits](
			const int64 WorkUnits,
			const FString& InstancePath,
			FUeAgentContractSchemaError& OutError)
		{
			Visits += WorkUnits;
			return true;
		};
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUeAgentContractSchemaVocabularyTest,
	"UeAgentInterface.Core.Contract.ContractSchema.VocabularyAndCanonicalization",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FUeAgentContractSchemaVocabularyTest::RunTest(
	const FString& Parameters)
{
	using namespace UE::UAI::Contract;

	TSharedPtr<const FUeAgentContractSchema> Schema;
	FUeAgentContractSchemaError Error;
	TestTrue(
		TEXT("supported schema compiles"),
		FUeAgentContractSchema::TryCompile(
			StrictInputSchema(),
			Schema,
			Error));
	if (!Schema.IsValid())
	{
		return false;
	}
	TestEqual(
		TEXT("compiled schema preserves canonical truth"),
		Schema->GetCanonicalSchema().GetJsonValue(),
		StrictInputSchema().GetJsonValue());

	int64 Visits = 0;
	TOptional<FUeAgentContractValue> CanonicalInput;
	TestTrue(
		TEXT("valid non-canonical input validates"),
		Schema->TryValidateAndCanonicalize(
			FUeAgentContractValue(TEXT(
				"{\"path\":\"/Game/Maps/TestMap\","
				"\"mode\":\"path\","
				"\"filter\":[\"actor\",\"component\"]}")),
			CountingCheckpoint(Visits),
			CanonicalInput,
			Error));
	TestTrue(TEXT("canonical input returned"), CanonicalInput.IsSet());
	if (CanonicalInput.IsSet())
	{
		TestEqual(
			TEXT("input is canonicalized before consumers"),
			CanonicalInput->GetJsonValue(),
			FString(TEXT(
				"{\"filter\":[\"actor\",\"component\"],"
				"\"mode\":\"path\","
				"\"path\":\"/Game/Maps/TestMap\"}")));
	}
	TestTrue(TEXT("validation uses checkpoints"), Visits > 0);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUeAgentContractSchemaDecimalBoundsTest,
	"UeAgentInterface.Core.Contract.ContractSchema.DecimalBoundsAndCanonicalization",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FUeAgentContractSchemaDecimalBoundsTest::RunTest(
	const FString& Parameters)
{
	using namespace UE::UAI::Contract;

	TSharedPtr<const FUeAgentContractSchema> Schema;
	FUeAgentContractSchemaError Error;
	TestTrue(
		TEXT("decimal bound schema compiles"),
		FUeAgentContractSchema::TryCompile(
			FUeAgentContractValue(TEXT(
				"{\"additionalProperties\":false,"
				"\"properties\":{\"value\":{\"maximum\":1,"
				"\"minimum\":0,\"type\":\"number\"}},"
				"\"required\":[\"value\"],\"type\":\"object\"}")),
			Schema,
			Error));
	if (!Schema.IsValid())
	{
		return false;
	}

	int64 Visits = 0;
	TOptional<FUeAgentContractValue> CanonicalInput;
	TestTrue(
		TEXT("positive decimal above zero validates"),
		Schema->TryValidateAndCanonicalize(
			FUeAgentContractValue(TEXT("{\"value\":0.25}")),
			CountingCheckpoint(Visits),
			CanonicalInput,
			Error));
	TestTrue(
		TEXT("positive decimal canonical value returned"),
		CanonicalInput.IsSet());
	if (CanonicalInput.IsSet())
	{
		TestEqual(
			TEXT("positive decimal lexeme is preserved"),
			CanonicalInput->GetJsonValue(),
			FString(TEXT("{\"value\":0.25}")));
	}

	CanonicalInput.Reset();
	Error = {};
	TestFalse(
		TEXT("negative decimal below zero is rejected"),
		Schema->TryValidateAndCanonicalize(
			FUeAgentContractValue(TEXT("{\"value\":-0.1}")),
			CountingCheckpoint(Visits),
			CanonicalInput,
			Error));
	TestEqual(
		TEXT("negative decimal fails minimum"),
		Error.Keyword,
		FString(TEXT("minimum")));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUeAgentContractSchemaValidationTest,
	"UeAgentInterface.Core.Contract.ContractSchema.ValidationAndNoDepthLimit",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FUeAgentContractSchemaValidationTest::RunTest(
	const FString& Parameters)
{
	using namespace UE::UAI::Contract;

	TSharedPtr<const FUeAgentContractSchema> Schema;
	FUeAgentContractSchemaError Error;
	TestTrue(
		TEXT("strict schema compiles"),
		FUeAgentContractSchema::TryCompile(
			StrictInputSchema(),
			Schema,
			Error));
	if (!Schema.IsValid())
	{
		return false;
	}

	int64 Visits = 0;
	TOptional<FUeAgentContractValue> CanonicalInput;
	TestFalse(
		TEXT("missing required field is rejected"),
		Schema->TryValidateAndCanonicalize(
			FUeAgentContractValue(TEXT("{\"mode\":\"path\"}")),
			CountingCheckpoint(Visits),
			CanonicalInput,
			Error));
	TestEqual(
		TEXT("required mismatch code"),
		Error.Code,
		FString(TEXT("contract_input_schema_mismatch")));
	TestEqual(
		TEXT("required instance path"),
		Error.InstancePath,
		FString(TEXT("/path")));
	TestEqual(
		TEXT("required keyword"),
		Error.Keyword,
		FString(TEXT("required")));

	TestFalse(
		TEXT("unknown field is rejected when additional properties false"),
		Schema->TryValidateAndCanonicalize(
			FUeAgentContractValue(TEXT(
				"{\"mode\":\"path\",\"path\":\"target\","
				"\"unexpected\":true}")),
			CountingCheckpoint(Visits),
			CanonicalInput,
			Error));
	TestEqual(
		TEXT("additional property path"),
		Error.InstancePath,
		FString(TEXT("/unexpected")));
	TestEqual(
		TEXT("additional property keyword"),
		Error.Keyword,
		FString(TEXT("additionalProperties")));

	TestFalse(
		TEXT("nested type mismatch is rejected"),
		Schema->TryValidateAndCanonicalize(
			FUeAgentContractValue(TEXT(
				"{\"filter\":[7],\"mode\":\"path\","
				"\"path\":\"target\"}")),
			CountingCheckpoint(Visits),
			CanonicalInput,
			Error));
	TestEqual(
		TEXT("nested mismatch path"),
		Error.InstancePath,
		FString(TEXT("/filter/0")));
	TestEqual(
		TEXT("nested mismatch keyword"),
		Error.Keyword,
		FString(TEXT("type")));

	constexpr int32 NestedLevels = 256;
	FString DeepSchema;
	FString DeepInput;
	for (int32 Index = 0; Index < NestedLevels; ++Index)
	{
		DeepSchema += TEXT("{\"items\":");
		DeepInput += TEXT("[");
	}
	DeepSchema += TEXT("{\"type\":\"string\"}");
	DeepInput += TEXT("\"leaf\"");
	for (int32 Index = 0; Index < NestedLevels; ++Index)
	{
		DeepSchema += TEXT(",\"type\":\"array\"}");
		DeepInput += TEXT("]");
	}

	TSharedPtr<const FUeAgentContractSchema> DeepContract;
	TestTrue(
		TEXT("deep schema compiles without a depth gate"),
		FUeAgentContractSchema::TryCompile(
			FUeAgentContractValue(DeepSchema),
			DeepContract,
			Error));
	if (DeepContract.IsValid())
	{
		Visits = 0;
		TestTrue(
			TEXT("deep input validates without a depth gate"),
			DeepContract->TryValidateAndCanonicalize(
				FUeAgentContractValue(DeepInput),
				CountingCheckpoint(Visits),
				CanonicalInput,
				Error));
		TestTrue(
			TEXT("deep traversal is checkpointed"),
			Visits >= NestedLevels);
	}

	TSharedPtr<const FUeAgentContractSchema> Unsupported;
	TestFalse(
		TEXT("unsupported vocabulary fails closed"),
		FUeAgentContractSchema::TryCompile(
			FUeAgentContractValue(TEXT(
				"{\"$ref\":\"https://example.invalid/schema\"}")),
			Unsupported,
			Error));
	TestEqual(
		TEXT("unsupported keyword code"),
		Error.Code,
		FString(TEXT("contract_schema_keyword_unsupported")));
	TestEqual(
		TEXT("unsupported keyword identity"),
		Error.Keyword,
		FString(TEXT("$ref")));
	TestEqual(
		TEXT("unsupported keyword path"),
		Error.SchemaPath,
		FString(TEXT("/$ref")));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUeAgentContractSchemaRegistryTest,
	"UeAgentInterface.Core.Contract.ContractSchema.RegistryCompilation",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FUeAgentContractSchemaRegistryTest::RunTest(
	const FString& Parameters)
{
	using namespace UE::UAI::Contract;

	const FUeAgentOperationDescriptor Descriptor(
		FUeAgentDescriptorIdentity(
			TEXT("fixture.unsupported-schema"),
			1,
			TEXT(
				"sha256:cccccccccccccccccccccccccccccccc"
				"cccccccccccccccccccccccccccccccc")),
		TEXT("fixture.unsupported-schema"),
		TEXT("fixture"),
		EUeAgentDescriptorOperationKind::Query,
		QueryPolicy(),
		FUeAgentContractValue(TEXT("{\"pattern\":\".*\"}")),
		FUeAgentContractValue(TEXT("{\"type\":\"object\"}")),
		FUeAgentQueryBinding(
			TEXT("fixture.provider"),
			EUeAgentQueryKind::State,
			EUeAgentQueryMaterializationPolicy::LoadReadonly,
			EUeAgentQuerySideEffectPolicy::NoPersistentMutation));
	TSharedPtr<const FUeAgentDescriptorRegistry> Registry;
	FString Error;
	TestFalse(
		TEXT("registry rejects unsupported schema before hash acceptance"),
		FUeAgentDescriptorRegistry::TryCreate(
			{Descriptor},
			Registry,
			Error));
	TestEqual(
		TEXT("registry preserves schema failure identity"),
		Error,
		FString(TEXT(
			"descriptor_input_schema_keyword_unsupported")));
	return true;
}
