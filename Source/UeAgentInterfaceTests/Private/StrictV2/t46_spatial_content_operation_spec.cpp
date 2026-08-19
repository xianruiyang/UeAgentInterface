#include "Misc/AutomationTest.h"

#include "Module/CommandRegistration/Active/UeAgentActiveCommandComposition.h"

namespace UE::UAI::Tests::StrictV2::T46
{
	const TArray<FString>& ExpectedCommands()
	{
		static const TArray<FString> Commands = {
			TEXT("mesh.element.select_by_ray"),
			TEXT("mesh.pivot.update"),
			TEXT("mesh.transform.bake"),
			TEXT("reference.origin.resolve"),
			TEXT("world.object_align"),
			TEXT("world.object_snap_to_surface"),
			TEXT("world.object_transform.read"),
			TEXT("world.transform_candidates.evaluate"),
		};
		return Commands;
	}

	const TArray<FString>& HistoricalCommands()
	{
		static const TArray<FString> Commands = {
			TEXT("level_align_actor_by_bounds"),
			TEXT("level_align_actor_vertex_to_vertex"),
			TEXT("level_align_face_to_face"),
			TEXT("level_snap_to_surface"),
			TEXT("node_origin_resolve"),
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
	TestEqual(TEXT("T46 owns exactly eight commands"), ExpectedCommands().Num(), 8);
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
	for (const FString& HistoricalId : HistoricalCommands())
	{
		TestNull(
			*FString::Printf(TEXT("%s has no active public contract"),
				*HistoricalId),
			Registry->FindExact(HistoricalId));
	}
	return !HasAnyErrors();
}
