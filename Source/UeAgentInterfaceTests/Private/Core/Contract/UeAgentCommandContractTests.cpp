#include "Misc/AutomationTest.h"

#include "CommandEdge/UeAgentExactCommandDispatch.h"
#include "CommandRegistry/UeAgentImmutableCommandRegistry.h"
#include "Core/Contract/Command/UeAgentCommandContractModel.h"
#include "Module/CommandRegistration/Active/UeAgentActiveCommandComposition.h"

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
	const FUeAgentFrozenCommandContract* Usage =
		Registry->FindExact(TEXT("query.command.usage"));
	const FUeAgentFrozenCommandContract* Origin =
		Registry->FindExact(TEXT("reference.origin.resolve"));
	TestNotNull(TEXT("command search contract"), Search);
	TestNotNull(TEXT("command usage contract"), Usage);
	TestNotNull(TEXT("reference origin contract"), Origin);
	TestNull(
		TEXT("retired global reference catalog is absent"),
		Registry->FindExact(TEXT("reference.catalog_search")));
	if (!Search || !Usage || !Origin)
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
		TEXT("usage method"),
		Usage->GetPythonFunction(),
		FString(TEXT("command_usage")));
	TestTrue(
		TEXT("search exposes only the intent input"),
		Search->GetInputFields().Num() == 1
			&& Search->GetInputFields()[0].Name == TEXT("intent"));
	TestTrue(
		TEXT("search result is the canonical commands list"),
		Search->GetResultFields().Num() == 1
			&& Search->GetResultFields()[0].Name == TEXT("commands"));
	TestTrue(
		TEXT("usage accepts one commands array"),
		Usage->GetInputFields().Num() == 1
			&& Usage->GetInputFields()[0].Name == TEXT("commands")
			&& Usage->GetResultFields().Num() == 1
			&& Usage->GetResultFields()[0].Name == TEXT("usage"));
	TestEqual(
		TEXT("reference origin method"),
		Origin->GetPythonFunction(),
		FString(TEXT("origin_resolve")));
	TestTrue(
		TEXT("command-specific contract hashes differ"),
		Search->GetContractHash() != Usage->GetContractHash()
			&& Usage->GetContractHash() != Origin->GetContractHash());

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
