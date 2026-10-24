#include "Misc/AutomationTest.h"

#include "Module/CommandRegistration/Active/UeAgentActiveCommandComposition.h"

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUeAgentStrictV2T31DestructiveContentInventoryTest,
	"UAI.StrictV2.T31.DESTRUCTIVE.CONTENT.OPS.InventoryAndActivation",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FUeAgentStrictV2T31DestructiveContentInventoryTest::RunTest(
	const FString& Parameters)
{
	using namespace UE::UAI;
	using namespace UE::UAI::Module::CommandRegistration::Active;
	TSharedPtr<const CommandRegistry::FUeAgentImmutableCommandRegistry> Registry;
	TSharedPtr<const CommandEdge::FUeAgentExactCommandDispatch> Dispatch;
	FString Error;
	if (!TestTrue(TEXT("the process active composition freezes"),
		TryComposeProcessActiveCommandComposition(Registry, Dispatch, Error))
		|| !Registry.IsValid())
	{
		AddError(Error);
		return false;
	}
	const auto* Contract = Registry->FindExact(TEXT("asset.delete"));
	if (!TestNotNull(TEXT("asset.delete is active"), Contract))
	{
		return false;
	}
	TestEqual(TEXT("asset.delete has one destructive owner"),
		Contract->GetPublicOwner(), FString(TEXT("destructive-content-owner")));
	const auto& InputFields = Contract->GetInputFields();
	const auto* Targets = InputFields.FindByPredicate([](const auto& Field)
	{
		return Field.Name == TEXT("targets");
	});
	const auto* Recursive = InputFields.FindByPredicate([](const auto& Field)
	{
		return Field.Name == TEXT("recursive");
	});
	TestNotNull(TEXT("asset.delete exposes targets"), Targets);
	TestNotNull(TEXT("asset.delete exposes recursive"), Recursive);
	TestNull(TEXT("asset.delete no longer exposes target_ref"),
		InputFields.FindByPredicate([](const auto& Field)
		{
			return Field.Name == TEXT("target_ref");
		}));
	return !HasAnyErrors();
}
