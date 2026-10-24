#include "Misc/AutomationTest.h"

#include "Core/Contract/Command/UeAgentCommandLifecyclePolicy.h"
#include "Core/Discovery/Command/UeAgentCommandDiscovery.h"
#include "Module/CommandRegistration/Active/UeAgentActiveCommandComposition.h"

namespace UE::UAI::Tests::StrictV2::T46
{
	const TArray<FString>& ExpectedCommands()
	{
		static const TArray<FString> Commands = {
			TEXT("mesh.element.select_by_ray"),
			TEXT("mesh.transform.bake"),
			TEXT("reference.origin.resolve"),
			TEXT("world.object_align"),
			TEXT("world.object_snap_to_surface"),
		};
		return Commands;
	}

	const TArray<FString>& DormantHjsonCommands()
	{
		static const TArray<FString> Commands = {
			TEXT("mesh.pivot.update"),
			TEXT("world.object_transform.read"),
			TEXT("world.object_transform.update"),
		};
		return Commands;
	}

}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUeAgentStrictV2T46SpatialContentInventoryTest,
	"UAI.StrictV2.T46.SPATIAL.CONTENT.OPERATION.InventoryAndActivation",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FUeAgentStrictV2T46SpatialContentInventoryTest::RunTest(
	const FString& Parameters)
{
	using namespace UE::UAI;
	using namespace UE::UAI::Module::CommandRegistration::Active;
	using namespace UE::UAI::Tests::StrictV2::T46;
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
	for (const FString& CommandId : ExpectedCommands())
	{
		const auto* Contract = Registry->FindExact(CommandId);
		TestNotNull(*FString::Printf(TEXT("%s is active"), *CommandId), Contract);
		if (Contract)
		{
			const FString ExpectedOwner = CommandId == TEXT("reference.origin.resolve")
				? FString(TEXT("query-command-owner"))
				: FString(TEXT("spatial-content-operation-owner"));
			TestEqual(*FString::Printf(TEXT("%s uses its unique long-term owner"), *CommandId),
				Contract->GetPublicOwner(),
				ExpectedOwner);
		}
	}
	for (const FString& DormantId : DormantHjsonCommands())
	{
		const auto* Contract = Registry->FindExact(DormantId);
		if (TestNotNull(
			*FString::Printf(TEXT("%s remains compatibility-executable"), *DormantId),
			Contract))
		{
			TestEqual(
				*FString::Printf(TEXT("%s is centrally dormant"), *DormantId),
				Contract->GetAvailability(),
				CommandContract::EUeAgentCommandAvailability::Dormant);
		}
	}

	TSharedPtr<const Discovery::Command::FCommandDiscoveryService>
		DiscoveryService;
	Discovery::Command::FCommandDiscoveryError DiscoveryError;
	if (!TestTrue(TEXT("T46 discovery consumes the frozen registry"),
		Discovery::Command::FCommandDiscoveryService::TryCreate(
			Registry.ToSharedRef(), DiscoveryService, DiscoveryError))
		|| !DiscoveryService.IsValid())
	{
		AddError(DiscoveryError.Code + TEXT(":") + DiscoveryError.Detail);
		return false;
	}
	Discovery::Command::FCommandSearchRequest Search;
	Search.Intent = TEXT("align actor bounds");
	Discovery::Command::FCommandSearchPage Page;
	if (!TestTrue(TEXT("transform keywords search"),
		DiscoveryService->Search(Search, Page, DiscoveryError)))
	{
		AddError(DiscoveryError.Code + TEXT(":") + DiscoveryError.Detail);
		return false;
	}
	TestTrue(TEXT("align actor bounds ranks object align in top three"),
		Page.Candidates.ContainsByPredicate(
			[](const Discovery::Command::FCommandCandidate& Candidate)
			{
				return Candidate.CommandId
					== TEXT("world.object_align");
			}));
	return !HasAnyErrors();
}
