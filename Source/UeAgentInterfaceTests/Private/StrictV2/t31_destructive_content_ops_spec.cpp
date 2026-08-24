#include "Misc/AutomationTest.h"

#include "Core/Discovery/Command/UeAgentCommandDiscovery.h"
#include "Module/CommandRegistration/Active/UeAgentActiveCommandComposition.h"
#include "OperationOwners/AssetSimple/UeAgentAssetSimpleOperationOwner.h"

namespace UE::UAI::Tests::StrictV2::T31
{
	const TArray<FString>& ExpectedCommands()
	{
		static const TArray<FString> Commands = {
			TEXT("asset.delete"),
			TEXT("asset.fixup_redirectors"),
			TEXT("asset.move"),
			TEXT("asset.object.clone"),
			TEXT("modeling.actor.mesh.replace"),
			TEXT("modeling.asset.material_slot.remove"),
			TEXT("modeling.voxel.bounds.remove"),
			TEXT("skeletal_mesh.remove_morph_target"),
			TEXT("skeletal_mesh.remove_skin_weight_profile"),
			TEXT("world.data_layer.delete"),
			TEXT("world.level_content.delete_scope"),
			TEXT("world.level_streaming.move_actors"),
			TEXT("world.level_streaming.remove"),
			TEXT("world.pcg.cleanup"),
		};
		return Commands;
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUeAgentStrictV2T31DestructiveContentInventoryTest,
	"UAI.StrictV2.T31.DESTRUCTIVE.CONTENT.OPS.InventoryAndActivation",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FUeAgentStrictV2T31DestructiveContentInventoryTest::RunTest(
	const FString& Parameters)
{
	using namespace UE::UAI;
	using namespace UE::UAI::Module::CommandRegistration::Active;
	using namespace UE::UAI::Tests::StrictV2::T31;
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
	TestEqual(TEXT("T31 owns exactly 14 inventory commands"),
		ExpectedCommands().Num(), 14);
	for (const FString& CommandId : ExpectedCommands())
	{
		const auto* Contract = Registry->FindExact(CommandId);
		TestNotNull(*FString::Printf(TEXT("%s is active"), *CommandId), Contract);
		if (Contract)
		{
			TestEqual(*FString::Printf(TEXT("%s has one T31 owner"), *CommandId),
				Contract->GetPublicOwner(),
				FString(TEXT("destructive-content-owner")));
		}
	}
	TestTrue(
		TEXT("destructive clone delegates mechanics to the shared OperationHost"),
		OperationOwners::AssetSimple::FindOperationInvocationFactory(
			TEXT("asset.object.clone")).IsValid());
	TSharedPtr<const Discovery::Command::FCommandDiscoveryService>
		DiscoveryService;
	Discovery::Command::FCommandDiscoveryError DiscoveryError;
	if (!TestTrue(
		TEXT("active command discovery composes"),
		Discovery::Command::FCommandDiscoveryService::TryCreate(
			Registry.ToSharedRef(), DiscoveryService, DiscoveryError))
		|| !DiscoveryService.IsValid())
	{
		AddError(DiscoveryError.Code + TEXT(":") + DiscoveryError.Detail);
		return false;
	}
	const auto* DataLayerContract =
		Registry->FindExact(TEXT("world.data_layer.delete"));
	if (DataLayerContract)
	{
		Discovery::Command::FCommandDetail Detail;
		TestTrue(
			TEXT("data-layer delete detail projects its exact tutorial"),
			DiscoveryService->GetDetail(
				DataLayerContract->GetCommandId(),
				DataLayerContract->GetContractHash(),
				Detail,
				DiscoveryError));
		TestTrue(
			TEXT("data-layer delete has the destructive workflow tutorial"),
			Detail.TutorialReferences.Contains(
				TEXT("uai-docs:t31-destructive-content-ops#destructive-plan-authorize-commit")));
		TestTrue(
			TEXT("data-layer delete has command-specific persisted-state tutorial"),
			Detail.TutorialReferences.Contains(
				TEXT("uai-docs:t31-destructive-content-ops#world-data-layer-delete")));
	}
	for (const FString& Intent : TArray<FString>{
		TEXT("duplicate asset"),
		TEXT("clone owned map asset"),
	})
	{
		Discovery::Command::FCommandSearchRequest Search;
		Search.Intent = Intent;
		Search.Limit = 3;
		Discovery::Command::FCommandSearchPage Page;
		if (!TestTrue(
			*FString::Printf(TEXT("natural clone intent searches: %s"), *Intent),
			DiscoveryService->Search(Search, Page, DiscoveryError)))
		{
			AddError(DiscoveryError.Code + TEXT(":") + DiscoveryError.Detail);
			continue;
		}
		TestTrue(
			*FString::Printf(TEXT("natural clone intent ranks exact clone in top three: %s"), *Intent),
			Page.Candidates.ContainsByPredicate(
				[](const Discovery::Command::FCommandCandidate& Candidate)
				{
					return Candidate.CommandId == TEXT("asset.object.clone");
				}));
	}
	return !HasAnyErrors();
}
