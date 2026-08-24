#include "Misc/AutomationTest.h"

#include "Composition/SimpleAuthoring/UeAgentSimpleAuthoringComposition.h"
#include "DomainAdapters/AssetSimple/UeAgentAssetSimpleMechanicsBoundary.h"
#include "OperationOwners/AssetSimple/UeAgentAssetSimpleOperationOwner.h"

#include "Module/CommandRegistration/Active/UeAgentActiveCommandComposition.h"

namespace UE::UAI::Tests::StrictV2::T37
{
	const TArray<FString>& ExpectedCommands()
	{
		static const TArray<FString> Commands = {
			TEXT("asset.audio_effect_preset.create"),
			TEXT("asset.audio_effect_preset.update"),
			TEXT("asset.curve.update"),
			TEXT("asset.data_layer.create"),
			TEXT("asset.data_object.create"),
			TEXT("asset.data_object.update"),
			TEXT("asset.data_table.replace"),
			TEXT("asset.input_action.create"),
			TEXT("asset.input_action.update"),
			TEXT("asset.input_mapping_context.create"),
			TEXT("asset.input_mapping_context.update"),
			TEXT("asset.media_player.create"),
			TEXT("asset.media_player.update"),
			TEXT("asset.media_source.create"),
			TEXT("asset.media_source.update"),
			TEXT("asset.media_texture.create"),
			TEXT("asset.media_texture.update"),
			TEXT("asset.paper_flipbook.create"),
			TEXT("asset.paper_flipbook.update"),
			TEXT("asset.paper_sprite.create"),
			TEXT("asset.paper_sprite.update"),
			TEXT("asset.paper_tilemap.update"),
			TEXT("asset.paper_tileset.update"),
			TEXT("asset.render_target.create"),
			TEXT("asset.render_target.update"),
			TEXT("asset.runtime_virtual_texture.create"),
			TEXT("asset.runtime_virtual_texture.update"),
			TEXT("asset.skeletal_mesh.update"),
			TEXT("asset.sound_attenuation.create"),
			TEXT("asset.sound_attenuation.update"),
			TEXT("asset.sound_class.create"),
			TEXT("asset.sound_class.update"),
			TEXT("asset.sound_concurrency.create"),
			TEXT("asset.sound_concurrency.update"),
			TEXT("asset.sound_mix.create"),
			TEXT("asset.sound_mix.update"),
			TEXT("asset.sound_submix.create"),
			TEXT("asset.sound_submix.update"),
			TEXT("asset.sparse_volume_texture.update"),
			TEXT("asset.static_mesh.update"),
			TEXT("asset.subuv_animation.create"),
			TEXT("asset.subuv_animation.update"),
			TEXT("asset.texture_array.create"),
			TEXT("asset.texture_array.replace"),
			TEXT("asset.texture_collection.create"),
			TEXT("asset.texture_collection.update"),
			TEXT("asset.texture_cube.create"),
			TEXT("asset.texture_cube.replace"),
			TEXT("asset.texture_cube_array.create"),
			TEXT("asset.texture_cube_array.replace"),
			TEXT("asset.volume_texture.create"),
			TEXT("asset.volume_texture.replace"),
		};
		return Commands;
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUeAgentStrictV2T37AssetSimpleInventoryTest,
	"UAI.StrictV2.T37.ASSET.SIMPLE.OPERATION.InventoryAndActivation",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FUeAgentStrictV2T37AssetSimpleInventoryTest::RunTest(
	const FString& Parameters)
{
	using namespace UE::UAI;
	using namespace UE::UAI::Module::CommandRegistration::Active;
	using namespace UE::UAI::Tests::StrictV2::T37;
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
	TestEqual(TEXT("T37 owns exactly 52 inventory commands"),
		ExpectedCommands().Num(), 52);
	for (const FString& CommandId : ExpectedCommands())
	{
		const auto* Contract = Registry->FindExact(CommandId);
		TestNotNull(*FString::Printf(TEXT("%s is active"), *CommandId), Contract);
		if (Contract)
		{
			TestEqual(*FString::Printf(TEXT("%s has one T37 owner"), *CommandId),
				Contract->GetPublicOwner(),
				FString(TEXT("asset-simple-operation-owner")));
		}
	}
	const TArray<FString>& PublicIds = DomainAdapters::AssetSimple::
		GetAssetSimpleMechanicsCommandIds();
	const TArray<FString>& DelegatedIds = DomainAdapters::AssetSimple::
		GetAssetSimpleDelegatedLifecycleCommandIds();
	const TArray<FString>& LifecycleIds = DomainAdapters::AssetSimple::
		GetAssetSimpleLifecycleCommandIds();
	TestEqual(TEXT("public mechanics remain the T37 inventory"),
		PublicIds.Num(), 52);
	TestEqual(TEXT("one lifecycle route is delegated"),
		DelegatedIds, TArray<FString>{TEXT("asset.object.clone")});
	TestEqual(TEXT("lifecycle is public plus delegated"),
		LifecycleIds.Num(), PublicIds.Num() + DelegatedIds.Num());
	for (const FString& PublicId : PublicIds)
	{
		TestTrue(TEXT("every public identity belongs to lifecycle"),
			LifecycleIds.Contains(PublicId));
		TestFalse(TEXT("public identity is not delegated"),
			DelegatedIds.Contains(PublicId));
	}
	for (const FString& DelegatedId : DelegatedIds)
	{
		TestFalse(TEXT("public and delegated identities are disjoint"),
			PublicIds.Contains(DelegatedId));
		TestTrue(TEXT("delegated identity belongs to lifecycle"),
			LifecycleIds.Contains(DelegatedId));
	}
	TestEqual(TEXT("public route projection remains 52"),
		OperationOwners::AssetSimple::
			GetAssetSimplePublicOperationRoutes().Num(), 52);
	TestEqual(TEXT("lifecycle route projection adds delegated clone"),
		OperationOwners::AssetSimple::GetAssetSimpleOperationRoutes().Num(),
		LifecycleIds.Num());
	TestTrue(TEXT("full SimpleAuthoring routes contain delegated clone"),
		Composition::SimpleAuthoring::GetSimpleAuthoringCommandRoutes().
			ContainsByPredicate([](const auto& Route)
			{
				return Route.CommandId == TEXT("asset.object.clone");
			}));
	TestTrue(TEXT("delegated clone reuses the OperationHost factory"),
		OperationOwners::AssetSimple::FindOperationInvocationFactory(
			TEXT("asset.object.clone")).IsValid());
	const auto* Clone = Registry->FindExact(TEXT("asset.object.clone"));
	TestNotNull(TEXT("clone remains public through destructive owner"), Clone);
	if (Clone)
	{
		TestEqual(TEXT("clone public owner remains destructive"),
			Clone->GetPublicOwner(),
			FString(TEXT("destructive-content-owner")));
	}
	return !HasAnyErrors();
}
