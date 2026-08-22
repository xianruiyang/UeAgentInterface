#include "Misc/AutomationTest.h"

#include "Composition/DestructiveImport/UeAgentDestructiveImportActivation.h"
#include "DomainAdapters/WorldDestructive/UeAgentWorldDestructiveOwner.h"
#include "Domains/Level/Authoring/UeAgentLevelAuthoringState.h"
#include "Engine/World.h"
#include "GameFramework/Actor.h"
#include "HAL/FileManager.h"
#include "Misc/PackageName.h"
#include "Misc/Paths.h"
#include "UObject/GarbageCollection.h"
#include "UObject/Package.h"
#include "UObject/UObjectGlobals.h"

namespace
{
	class FNeverCancelled final
		: public UE::UAI::Contract::IUeAgentCancellationSignal
	{
	public:
		bool IsCancelled() const override { return false; }
	};
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUeAgentWorldDestructiveBoundaryTest,
	"UeAgentInterface.DomainAdapters.WorldDestructive.Boundary",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FUeAgentWorldDestructiveBoundaryTest::RunTest(const FString& Parameters)
{
	(void)Parameters;
	TSharedPtr<const UE::UAI::CommandContract::
		FUeAgentCommandContractRegistry> Registry;
	FString Error;
	TestTrue(TEXT("formal destructive/import registry is created"),
		UE::UAI::Composition::DestructiveImport::
			TryCreateDestructiveImportProductionCommandContractRegistry(
				Registry, Error));
	const TArray<FString> CanonicalIds{
		TEXT("world.data_layer.delete"),
		TEXT("world.level_content.delete_scope"),
		TEXT("world.pcg.cleanup"),
		TEXT("world.level_streaming.move_actors"),
		TEXT("world.level_streaming.remove"),
		TEXT("modeling.actor.mesh.replace"),
		TEXT("modeling.asset.material_slot.remove"),
		TEXT("modeling.voxel.bounds.remove"),
	};
	for (const FString& CommandId : CanonicalIds)
	{
		TestNotNull(TEXT("canonical world command contract: ") + CommandId,
			Registry.IsValid() ? Registry->FindExact(CommandId) : nullptr);
	}
	const TArray<FString> LegacyIds{
		TEXT("data_layer_delete_instance"),
		TEXT("level_content_delete_scope"),
		TEXT("pcg_cleanup"),
		TEXT("level_streaming_move_actors"),
		TEXT("level_streaming_remove"),
		TEXT("modeling_replace_actor_mesh"),
		TEXT("modeling_remove_material_slot"),
		TEXT("modeling_voxel_grid_remove_bounds"),
	};
	for (const FString& LegacyId : LegacyIds)
	{
		TestNull(TEXT("legacy identity is absent: ") + LegacyId,
			Registry.IsValid() ? Registry->FindExact(LegacyId) : nullptr);
	}
	return !HasAnyErrors();
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUeAgentManagedWorldActorIdentityBoundaryTest,
	"UeAgentInterface.Domains.Level.ManagedWorldActorIdentity",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FUeAgentManagedWorldActorIdentityBoundaryTest::RunTest(
	const FString& Parameters)
{
	(void)Parameters;
	UPackage* const Package = CreatePackage(
		*(TEXT("/Temp/UAI/ManagedActorIdentity/")
			+ FGuid::NewGuid().ToString(EGuidFormats::Digits)));
	UWorld* const World = UWorld::CreateWorld(
		EWorldType::EditorPreview,
		false,
		FName(TEXT("UAIManagedActorIdentity")),
		Package);
	AActor* const First = World ? World->SpawnActor<AActor>() : nullptr;
	AActor* const Second = World ? World->SpawnActor<AActor>() : nullptr;
	if (!TestNotNull(TEXT("owned identity world exists"), World)
		|| !TestNotNull(TEXT("first managed actor exists"), First)
		|| !TestNotNull(TEXT("second managed actor exists"), Second))
	{
		return false;
	}

	const FString OwnerId = TEXT("managed-identity-test");
	const FString Folder =
		UeAgentLevelAuthoring::MakeManagedActorFolderPath(OwnerId);
	const FString FirstId = TEXT("uai://managed-actor/first");
	const FString SecondId = TEXT("uai://managed-actor/second");
	FString Error;
	TestTrue(
		TEXT("first actor binds shared managed identity"),
		UeAgentLevelAuthoring::TryBindManagedActorOwnership(
			*First, FirstId, OwnerId, Error));
	TestFalse(
		TEXT("duplicate managed id is rejected before mutation"),
		UeAgentLevelAuthoring::TryBindManagedActorOwnership(
			*Second, FirstId, OwnerId, Error));
	TestTrue(
		TEXT("duplicate rejection has the stable failure"),
		Error.StartsWith(TEXT("level_managed_actor_id_duplicate:")));
	TestTrue(
		TEXT("duplicate rejection leaves second actor identity empty"),
		UeAgentLevelAuthoring::GetManagedActorId(*Second).IsEmpty());
	TestFalse(
		TEXT("duplicate rejection leaves second actor unmarked"),
		Second->ActorHasTag(
			UeAgentLevelAuthoring::GetManagedActorMarkerTag()));
	TestTrue(
		TEXT("second actor binds a distinct managed id"),
		UeAgentLevelAuthoring::TryBindManagedActorOwnership(
			*Second, SecondId, OwnerId, Error));

	AActor* Resolved = nullptr;
	TestTrue(
		TEXT("exact resolver requires id marker and folder"),
		UeAgentLevelAuthoring::TryFindManagedActorExact(
			*World, FirstId, Folder, Resolved, Error));
	TestTrue(TEXT("exact resolver returns the first actor"), Resolved == First);
	Resolved = nullptr;
	TestFalse(
		TEXT("exact resolver rejects a mismatched folder"),
		UeAgentLevelAuthoring::TryFindManagedActorExact(
			*World, FirstId, Folder + TEXT("/wrong"), Resolved, Error));
	TestTrue(
		TEXT("folder mismatch has the stable failure"),
		Error.StartsWith(TEXT("level_managed_actor_folder_mismatch:")));
	TestNull(TEXT("failed exact resolution publishes no actor"), Resolved);

	Package->SetDirtyFlag(false);
	World->DestroyWorld(false);
	World->MarkAsGarbage();
	Package->MarkAsGarbage();
	CollectGarbage(RF_NoFlags, true);
	return !HasAnyErrors();
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUeAgentWorldDestructiveMapHeaderPersistenceTest,
	"UeAgentInterface.DomainAdapters.WorldDestructive.MapHeaderPersistence",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FUeAgentWorldDestructiveMapHeaderPersistenceTest::RunTest(
	const FString& Parameters)
{
	(void)Parameters;
	using namespace UE::UAI::DomainAdapters::WorldDestructive;

	const FString FixtureId =
		FGuid::NewGuid().ToString(EGuidFormats::Digits);
	const FString PackageName =
		TEXT("/Game/UAI/Tests/WorldDestructive/MapHeader_") + FixtureId;
	FString MapFilename;
	FString AssetFilename;
	TestTrue(
		TEXT("resolve owned map filename"),
		FPackageName::TryConvertLongPackageNameToFilename(
			PackageName,
			MapFilename,
			FPackageName::GetMapPackageExtension()));
	TestTrue(
		TEXT("resolve forbidden duplicate filename"),
		FPackageName::TryConvertLongPackageNameToFilename(
			PackageName,
			AssetFilename,
			FPackageName::GetAssetPackageExtension()));
	IFileManager::Get().MakeDirectory(*FPaths::GetPath(MapFilename), true);
	IFileManager::Get().Delete(*MapFilename, false, true, true);
	IFileManager::Get().Delete(*AssetFilename, false, true, true);

	UPackage* const Package = CreatePackage(*PackageName);
	UWorld* const World = UWorld::CreateWorld(
		EWorldType::EditorPreview,
		false,
		FName(*FPackageName::GetLongPackageAssetName(PackageName)),
		Package);
	if (World)
	{
		World->SetFlags(RF_Public | RF_Standalone | RF_Transactional);
	}
	AActor* Actor = World ? World->SpawnActor<AActor>() : nullptr;
	if (Actor)
	{
		Actor->SetFolderPath(TEXT("UAI/T50/MapHeader"));
	}
	TestNotNull(TEXT("owned world is created"), World);
	TestNotNull(TEXT("owned actor is created"), Actor);
	if (!World || !Actor)
	{
		return false;
	}
	Package->ClearPackageFlags(PKG_ContainsMap);

	TSharedPtr<IUeAgentWorldDestructiveOwner> Owner;
	FString Error;
	TestTrue(
		TEXT("world destructive owner is created"),
		TryCreateWorldDestructiveOwner(
			*World,
			FPaths::ProjectSavedDir()
				/ TEXT("UeAgentInterface/Tests/WorldDestructive"),
			TEXT("world-destructive-map-header-test"),
			Owner,
			Error));
	FUeAgentWorldDestructiveRequest Request;
	Request.RequestId = TEXT("map-header-") + FixtureId;
	Request.CommandId = TEXT("world.level_content.delete_scope");
	Request.TargetRef = TEXT("UAI/T50/MapHeader");
	Request.CallerId = TEXT("automation");
	const FUeAgentWorldDestructiveResult Result = Owner.IsValid()
		? Owner->Execute(Request, MakeShared<FNeverCancelled>())
		: FUeAgentWorldDestructiveResult();
	if (!Result.IsSuccess())
	{
		AddError(
			TEXT("world destructive map persistence failed: ")
			+ Result.ErrorCode + TEXT(":") + Result.ErrorDetail);
	}
	TestTrue(TEXT("owned map deletion commits"), Result.IsSuccess());
	TestTrue(TEXT("owned map deletion is persisted"), Result.bPersisted);
	TestTrue(
		TEXT("map persistence repairs missing map metadata"),
		Package->ContainsMap());
	TestTrue(
		TEXT("world destructive persists the .umap header"),
		IFileManager::Get().FileExists(*MapFilename));
	TestFalse(
		TEXT("world destructive does not create a sibling .uasset"),
		IFileManager::Get().FileExists(*AssetFilename));

	Owner.Reset();
	Package->SetDirtyFlag(false);
	World->DestroyWorld(false);
	World->MarkAsGarbage();
	Package->MarkAsGarbage();
	CollectGarbage(RF_NoFlags, true);
	IFileManager::Get().Delete(*MapFilename, false, true, true);
	IFileManager::Get().Delete(*AssetFilename, false, true, true);
	return !HasAnyErrors();
}
