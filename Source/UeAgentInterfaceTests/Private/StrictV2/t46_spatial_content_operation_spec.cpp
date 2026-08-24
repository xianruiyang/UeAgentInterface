#include "Misc/AutomationTest.h"

#include "Core/Discovery/Command/UeAgentCommandDiscovery.h"
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
			TEXT("world.object_transform.update"),
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
	const auto* TransformUpdate = Registry->FindExact(
		TEXT("world.object_transform.update"));
	if (!TestNotNull(
			TEXT("transform update freezes one public contract"),
			TransformUpdate))
	{
		return false;
	}
	const CommandContract::FUeAgentCommandFieldDeclaration* InputField =
		TransformUpdate->GetInputFields().FindByPredicate(
			[](const CommandContract::FUeAgentCommandFieldDeclaration& Field)
			{
				return Field.Name == TEXT("input");
			});
	TestNotNull(TEXT("transform update exposes its descriptor input"), InputField);
	if (InputField)
	{
		TestFalse(TEXT("transform update input schema is non-empty"),
			InputField->SchemaJson.IsEmpty());
		TestTrue(TEXT("transform update input schema keeps recursive components"),
			InputField->SchemaJson.Contains(TEXT("\"components\"")));
		TestTrue(TEXT("transform update input schema keeps conversion evidence"),
			InputField->SchemaJson.Contains(
				TEXT("\"conversion_evidence_ids\"")));
	}
	TestEqual(TEXT("transform update keeps its public operation wrapper"),
		TransformUpdate->GetResultFields().Num(), 2);
	const CommandContract::FUeAgentCommandFieldDeclaration* ProjectionField =
		TransformUpdate->GetResultFields().FindByPredicate(
			[](const CommandContract::FUeAgentCommandFieldDeclaration& Field)
			{
				return Field.Name == TEXT("projection");
			});
	TestNotNull(TEXT("transform update exposes projection payload"), ProjectionField);
	if (ProjectionField)
	{
		const FString& ResultSchema = ProjectionField->SchemaJson;
		TestFalse(TEXT("transform update result schema is non-empty"),
			ResultSchema.IsEmpty());
		TestTrue(TEXT("descriptor result remains nested under projection payload"),
			ResultSchema.Contains(TEXT("\"payload\"")));
		TestTrue(TEXT("transform update result exposes transform observations"),
			ResultSchema.Contains(TEXT("\"transforms\"")));
		TestTrue(TEXT("transform update result schema keeps recursive components"),
			ResultSchema.Contains(TEXT("\"components\"")));
		TestTrue(TEXT("transform update result schema keeps conversion evidence"),
			ResultSchema.Contains(TEXT("\"conversion_evidence_ids\"")));
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
	Search.Intent = TEXT("move actor transform");
	Search.Limit = 3;
	Discovery::Command::FCommandSearchPage Page;
	if (!TestTrue(TEXT("natural transform intent searches"),
		DiscoveryService->Search(Search, Page, DiscoveryError)))
	{
		AddError(DiscoveryError.Code + TEXT(":") + DiscoveryError.Detail);
		return false;
	}
	TestTrue(TEXT("move actor transform ranks update in top three"),
		Page.Candidates.ContainsByPredicate(
			[](const Discovery::Command::FCommandCandidate& Candidate)
			{
				return Candidate.CommandId
					== TEXT("world.object_transform.update");
			}));
	return !HasAnyErrors();
}
