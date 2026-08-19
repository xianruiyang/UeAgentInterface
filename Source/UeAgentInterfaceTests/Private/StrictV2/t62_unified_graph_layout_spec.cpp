#include "Misc/AutomationTest.h"

#include "Module/CommandRegistration/Active/UeAgentActiveCommandComposition.h"

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUeAgentStrictV2T62UnifiedGraphLayoutContractTest,
	"UAI.StrictV2.T62.UNIFIED.GRAPH.LAYOUT.ContractAndOwnership",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FUeAgentStrictV2T62UnifiedGraphLayoutContractTest::RunTest(
	const FString& Parameters)
{
	using namespace UE::UAI;
	using namespace UE::UAI::Module::CommandRegistration::Active;
	TSharedPtr<const CommandRegistry::FUeAgentImmutableCommandRegistry> Registry;
	TSharedPtr<const CommandEdge::FUeAgentExactCommandDispatch> Dispatch;
	FString Error;
	if (!TestTrue(TEXT("the active composition freezes"),
		TryComposeProcessActiveCommandComposition(Registry, Dispatch, Error))
		|| !Registry.IsValid())
	{
		AddError(Error);
		return false;
	}
	const auto* Contract = Registry->FindExact(TEXT("graph.layout.apply"));
	if (!TestNotNull(TEXT("graph.layout.apply is active"), Contract))
	{
		return false;
	}
	TestEqual(TEXT("T62 is the unique owner"), Contract->GetPublicOwner(),
		FString(TEXT("unified-graph-layout-owner")));
	const TArray<FString> ExpectedParameters = {
		TEXT("targets"), TEXT("strategy"), TEXT("parameters"),
		TEXT("authorization_id"), TEXT("projection")};
	TArray<FString> ActualParameters;
	for (const CommandContract::FUeAgentCommandFieldDeclaration& Field
		: Contract->GetInputFields())
	{
		ActualParameters.Add(Field.Name);
	}
	TestEqual(TEXT("the public contract uses one multi-target shape"),
		ActualParameters, ExpectedParameters);
	return !HasAnyErrors();
}
