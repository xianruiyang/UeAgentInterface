#include "Misc/AutomationTest.h"

#include "Composition/DestructiveImport/UeAgentDestructiveImportActivation.h"
#include "Core/AssetDurable/UeAgentPackageHeader.h"
#include "DomainAdapters/ContentOperations/UeAgentDestructiveContentPlanningBoundary.h"
#include "DomainAdapters/WorldDestructive/UeAgentWorldDestructiveOwner.h"
#include "Dom/JsonObject.h"
#include "Domains/Level/Authoring/UeAgentLevelAuthoringState.h"
#include "Engine/Level.h"
#include "Engine/World.h"
#include "GameFramework/Actor.h"
#include "HAL/FileManager.h"
#include "HAL/PlatformMisc.h"
#include "Misc/PackageName.h"
#include "Misc/Paths.h"
#include "OperationOwners/DestructiveContent/UeAgentWorldDestructiveDurableRunner.h"
#include "UObject/GarbageCollection.h"
#include "UObject/Package.h"
#include "UObject/SavePackage.h"
#include "UObject/UObjectGlobals.h"
#include "WorldPartition/DataLayer/DataLayerAsset.h"
#include "WorldPartition/DataLayer/DataLayerInstanceWithAsset.h"
#include "WorldPartition/DataLayer/WorldDataLayers.h"
#include "WorldPartition/WorldPartition.h"

namespace
{
	class FNeverCancelled final
		: public UE::UAI::Contract::IUeAgentCancellationSignal
	{
	public:
		bool IsCancelled() const override { return false; }
	};

	class FRollbackAfterApplyRunner final
		: public UE::UAI::DomainAdapters::WorldDestructive::
			IUeAgentWorldDestructiveDurableRunner
	{
	public:
		UE::UAI::DomainAdapters::WorldDestructive::
			FUeAgentWorldDestructiveResult Execute(
				UWorld& ExplicitWorld,
				const UE::UAI::DomainAdapters::WorldDestructive::
					FUeAgentWorldDestructiveRequest& Request,
				TSharedRef<UE::UAI::DomainAdapters::WorldDestructive::
					IUeAgentWorldDestructiveDurableMutation> Mutation,
				const TSharedRef<UE::UAI::Contract::
					IUeAgentCancellationSignal>& Cancellation) override
		{
			(void)ExplicitWorld;
			(void)Cancellation;
			using namespace UE::UAI::DomainAdapters::WorldDestructive;
			FUeAgentWorldDestructiveResult Result;
			Result.CommandId = Request.CommandId;
			Result.TargetRef = Request.TargetRef;
			FString Error;
			if (!Mutation->Apply(Error))
			{
				Result.TerminalState = TEXT("state_unknown");
				Result.ErrorCode = TEXT("fixture_apply_failed");
				Result.ErrorDetail = Error;
				return Result;
			}
			bApplyObserved = true;
			if (!Mutation->Rollback(Error))
			{
				Result.TerminalState = TEXT("state_unknown");
				Result.ErrorCode = TEXT("fixture_rollback_failed");
				Result.ErrorDetail = Error;
				return Result;
			}
			bRollbackObserved = true;
			Result.TerminalState = TEXT("rolled_back");
			Result.ErrorCode = TEXT("fixture_post_mutation_failure");
			return Result;
		}

		bool bApplyObserved = false;
		bool bRollbackObserved = false;
	};

	bool TryCreateProductionWorldDestructiveOwner(
		UWorld& World,
		const FString& JournalRoot,
		const FString& OwnerId,
		TSharedPtr<UE::UAI::DomainAdapters::WorldDestructive::
			IUeAgentWorldDestructiveOwner>& OutOwner,
		FString& OutError)
	{
		using namespace UE::UAI::DomainAdapters::WorldDestructive;
		TSharedPtr<IUeAgentWorldDestructiveDurableRunner> DurableRunner;
		return UE::UAI::OperationOwners::DestructiveContent::
				TryCreateWorldDestructiveDurableRunner(
					JournalRoot,
					OwnerId,
					DurableRunner,
					OutError)
			&& DurableRunner.IsValid()
			&& TryCreateWorldDestructiveOwner(
				World,
				JournalRoot,
				OwnerId,
				DurableRunner.ToSharedRef(),
				OutOwner,
				OutError);
	}

	bool IsTaskOwnedRetainedFixturePackage(const FString& PackageName)
	{
		if (!FPackageName::IsValidLongPackageName(PackageName, true))
		{
			return false;
		}
		FString Relative = PackageName;
		if (!Relative.RemoveFromStart(TEXT("/Game/UAI/Tests/T")))
		{
			return false;
		}
		int32 SeparatorIndex = INDEX_NONE;
		if (!Relative.FindChar(TEXT('/'), SeparatorIndex)
			|| SeparatorIndex <= 0
			|| SeparatorIndex >= Relative.Len() - 1)
		{
			return false;
		}
		return Relative.Left(SeparatorIndex).IsNumeric();
	}
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
	FUeAgentDataLayerExactIdentitySelectionTest,
	"UeAgentInterface.Domains.Level.DataLayerExactIdentitySelection",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FUeAgentDataLayerExactIdentitySelectionTest::RunTest(
	const FString& Parameters)
{
	(void)Parameters;
	const TArray<TArray<FString>> CandidateIdentities{
		{TEXT("First"), TEXT("SharedIdentity")},
		{TEXT("Second"), TEXT("SharedIdentity")},
	};
	int32 CandidateIndex = 7;
	FString Error;
	TestFalse(
		TEXT("two candidates sharing an accepted identity are rejected"),
		UeAgentLevelAuthoring::TrySelectDataLayerIdentityExact(
			CandidateIdentities,
			TEXT("SharedIdentity"),
			CandidateIndex,
			Error));
	TestEqual(
		TEXT("ambiguous exact identity keeps its stable error"),
		Error,
		TEXT("world_data_layer_identity_ambiguous"));
	TestEqual(
		TEXT("ambiguous exact identity publishes no candidate"),
		CandidateIndex,
		INDEX_NONE);
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
		TryCreateProductionWorldDestructiveOwner(
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

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUeAgentWorldDestructiveDataLayerDurableBoundaryTest,
	"UeAgentInterface.DomainAdapters.WorldDestructive.DataLayerDurableBoundary",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FUeAgentWorldDestructiveDataLayerDurableBoundaryTest::RunTest(
	const FString& Parameters)
{
	(void)Parameters;
	using namespace UE::UAI::DomainAdapters::WorldDestructive;

	const FString FixtureId =
		FGuid::NewGuid().ToString(EGuidFormats::Digits);
	FString RetainedFixturePackage = FPlatformMisc::GetEnvironmentVariable(
		TEXT("UAI_WORLD_DESTRUCTIVE_PUBLIC_FIXTURE_PACKAGE"));
	RetainedFixturePackage.TrimStartAndEndInline();
	const bool bRetainPublicFixture = !RetainedFixturePackage.IsEmpty();
	if (bRetainPublicFixture
		&& !IsTaskOwnedRetainedFixturePackage(RetainedFixturePackage))
	{
		AddError(
			TEXT(
				"retained fixture package must be an exact "
				"/Game/UAI/Tests/T<digits>/... package"));
		return false;
	}
	const FString PackageName = bRetainPublicFixture
		? RetainedFixturePackage
		: TEXT("/Game/UAI/Tests/WorldDestructive/DataLayerDurable_")
			+ FixtureId;
	const FString DataLayerPackageName = PackageName + TEXT("_Layer");
	FString RelativePackageName = PackageName;
	RelativePackageName.RemoveFromStart(TEXT("/Game/"));
	const FString ExternalActorDirectory =
		FPaths::ProjectContentDir()
		/ TEXT("__ExternalActors__")
		/ RelativePackageName;
	FString MapFilename;
	FString DataLayerFilename;
	TestTrue(
		TEXT("resolve owned DataLayer map filename"),
		FPackageName::TryConvertLongPackageNameToFilename(
			PackageName,
			MapFilename,
			FPackageName::GetMapPackageExtension()));
	if (bRetainPublicFixture
		&& (IFileManager::Get().FileExists(*MapFilename)
			|| IFileManager::Get().DirectoryExists(
				*ExternalActorDirectory)))
	{
		AddError(TEXT("retained public fixture target already exists"));
		return false;
	}
	IFileManager::Get().MakeDirectory(*FPaths::GetPath(MapFilename), true);
	IFileManager::Get().Delete(*MapFilename, false, true, true);
	TestTrue(
		TEXT("resolve owned DataLayer asset filename"),
		FPackageName::TryConvertLongPackageNameToFilename(
			DataLayerPackageName,
			DataLayerFilename,
			FPackageName::GetAssetPackageExtension()));
	IFileManager::Get().Delete(*DataLayerFilename, false, true, true);

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
	if (World && World->PersistentLevel)
	{
		World->PersistentLevel->SetUseExternalActors(true);
	}
	UWorldPartition* const WorldPartition = World
		? UWorldPartition::CreateOrRepairWorldPartition(
			World->GetWorldSettings())
		: nullptr;
	if (WorldPartition && !WorldPartition->IsInitialized())
	{
		WorldPartition->Initialize(World, FTransform::Identity);
	}
	AWorldDataLayers* const WorldDataLayers = World
		? World->GetWorldDataLayers()
		: nullptr;
	UPackage* const DataLayerPackage = CreatePackage(
		*DataLayerPackageName);
	UDataLayerAsset* const DataLayerAsset = DataLayerPackage
		? NewObject<UDataLayerAsset>(
			DataLayerPackage,
			TEXT("OwnedDataLayer"),
			RF_Public | RF_Standalone | RF_Transactional)
		: nullptr;
	if (DataLayerAsset)
	{
		DataLayerAsset->SetType(EDataLayerType::Runtime);
	}
	UDataLayerInstanceWithAsset* const DataLayer =
		WorldDataLayers && DataLayerAsset
			? WorldDataLayers->CreateDataLayer<UDataLayerInstanceWithAsset>(
				DataLayerAsset)
			: nullptr;
	if (!TestNotNull(TEXT("owned DataLayer world exists"), World)
		|| !TestNotNull(TEXT("owned WorldPartition exists"), WorldPartition)
		|| !TestNotNull(TEXT("owned WorldDataLayers exists"), WorldDataLayers)
		|| !TestNotNull(TEXT("owned DataLayer exists"), DataLayer))
	{
		return false;
	}
	Package->SetPackageFlags(PKG_ContainsMap);
	FSavePackageArgs InitialSaveArgs;
	InitialSaveArgs.TopLevelFlags = RF_Public | RF_Standalone;
	TestTrue(
		TEXT("persist owned DataLayer asset fixture"),
		UPackage::SavePackage(
			DataLayerPackage,
			DataLayerAsset,
			*DataLayerFilename,
			InitialSaveArgs));
	TestTrue(
		TEXT("persist owned map fixture with DataLayer present"),
		UPackage::SavePackage(
			Package,
			World,
			*MapFilename,
			InitialSaveArgs));
	Package->SetDirtyFlag(false);
	DataLayerPackage->SetDirtyFlag(false);

	const FString DurablePackageName =
		WorldDataLayers->GetOutermost()->GetName();
	TestNotEqual(
		TEXT("WorldPartition WorldDataLayers uses its external actor package"),
		DurablePackageName,
		PackageName);
	FString DurableHeaderFilename;
	FString DurableHeaderError;
	const bool bDurableHeaderResolved =
		UE::UAI::AssetDurable::TryResolvePackageHeaderFilename(
			DurablePackageName,
			UE::UAI::AssetDurable::EPackageHeaderKind::EvidenceRequired,
			DurableHeaderFilename,
			DurableHeaderError);
	TestTrue(
		TEXT("WorldDataLayers durable target has exact header evidence: ")
			+ DurableHeaderError,
		bDurableHeaderResolved);
	TestTrue(
		TEXT("WorldDataLayers external actor durable target is an asset header"),
		bDurableHeaderResolved
			&& DurableHeaderFilename.EndsWith(
				FPackageName::GetAssetPackageExtension(),
				ESearchCase::IgnoreCase));
	TestTrue(
		TEXT("persist the WorldDataLayers carrier before public planning"),
		bDurableHeaderResolved
			&& UPackage::SavePackage(
				WorldDataLayers->GetOutermost(),
				WorldDataLayers,
				*DurableHeaderFilename,
				InitialSaveArgs));
	WorldDataLayers->GetOutermost()->SetDirtyFlag(false);

	UE::UAI::DomainAdapters::ContentOperations::
		FUeAgentDestructiveContentPlan PublicPlan;
	const TSharedRef<FJsonObject> PublicParams = MakeShared<FJsonObject>();
	PublicParams->SetStringField(
		TEXT("layer"),
		DataLayer->GetDataLayerShortName());
	FString PublicPlanError;
	const bool bPublicPlanBuilt =
		UE::UAI::DomainAdapters::ContentOperations::
			TryBuildDestructiveContentPlan(
				TEXT("world.data_layer.delete"),
				PublicParams,
				World,
				PublicPlan,
				PublicPlanError);
	TestTrue(
		TEXT("public destructive plan resolves the DataLayer short name: ")
			+ PublicPlanError,
		bPublicPlanBuilt);
	TestTrue(
		TEXT("public destructive plan protects the primary map package"),
		PublicPlan.ImpactScope.Contains(TEXT("package:") + PackageName));
	TestTrue(
		TEXT("public destructive plan protects the WorldDataLayers carrier"),
		PublicPlan.ImpactScope.Contains(
			TEXT("package:") + DurablePackageName));
	TestTrue(
		TEXT("public destructive plan binds the resolved DataLayer identity"),
		PublicPlan.ImpactScope.Contains(
			TEXT("data_layer:") + DataLayer->GetPathName()));

	TSharedPtr<IUeAgentWorldDestructiveOwner> Owner;
	FString Error;
	TestTrue(
		TEXT("world destructive owner is created"),
		TryCreateProductionWorldDestructiveOwner(
			*World,
			FPaths::ProjectSavedDir()
				/ TEXT("UeAgentInterface/Tests/WorldDestructive"),
			TEXT("world-destructive-data-layer-durable-test"),
			Owner,
			Error));
	FUeAgentWorldDestructiveRequest Request;
	Request.RequestId = TEXT("data-layer-durable-") + FixtureId;
	Request.CommandId = TEXT("world.data_layer.delete");
	Request.TargetRef = DataLayer->GetDataLayerShortName();
	Request.CallerId = TEXT("automation");
	const FUeAgentWorldDestructiveResult Result = Owner.IsValid()
		? Owner->Execute(Request, MakeShared<FNeverCancelled>())
		: FUeAgentWorldDestructiveResult();
	if (!Result.IsSuccess())
	{
		AddError(
			TEXT("world DataLayer deletion failed before durable oracle: ")
			+ Result.ErrorCode + TEXT(":") + Result.ErrorDetail);
	}
	TestTrue(TEXT("owned DataLayer deletion commits"), Result.IsSuccess());
	TestTrue(TEXT("owned DataLayer deletion persists"), Result.bPersisted);
	TestTrue(
		TEXT("owned DataLayer is absent after command readback"),
		UeAgentLevelAuthoring::FindDataLayerInstancesExact(
			*WorldDataLayers,
			Request.TargetRef).IsEmpty());

	TestFalse(
		TEXT("successful DataLayer deletion publishes AssetDurable journal id"),
		Result.JournalId.IsEmpty());

	Owner.Reset();
	Package->SetDirtyFlag(false);
	World->DestroyWorld(false);
	World->MarkAsGarbage();
	Package->MarkAsGarbage();
	DataLayerPackage->MarkAsGarbage();
	CollectGarbage(RF_NoFlags, true);
	IFileManager::Get().Delete(*DataLayerFilename, false, true, true);
	if (bRetainPublicFixture)
	{
		TestTrue(
			TEXT("retained public fixture map remains persisted"),
			IFileManager::Get().FileExists(*MapFilename));
		TestTrue(
			TEXT("retained public fixture WorldDataLayers carrier remains persisted"),
			IFileManager::Get().FileExists(*DurableHeaderFilename));
	}
	else
	{
		IFileManager::Get().Delete(*MapFilename, false, true, true);
		if (!DurableHeaderFilename.IsEmpty())
		{
			IFileManager::Get().Delete(
				*DurableHeaderFilename,
				false,
				true,
				true);
		}
		IFileManager::Get().DeleteDirectory(
			*ExternalActorDirectory,
			false,
			true);
	}
	return !HasAnyErrors();
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUeAgentWorldDestructiveDataLayerRollbackBoundaryTest,
	"UeAgentInterface.DomainAdapters.WorldDestructive.DataLayerRollbackBoundary",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FUeAgentWorldDestructiveDataLayerRollbackBoundaryTest::RunTest(
	const FString& Parameters)
{
	(void)Parameters;
	using namespace UE::UAI::DomainAdapters::WorldDestructive;

	UPackage* const Package = CreatePackage(
		*(TEXT("/Temp/UAI/WorldDestructive/DataLayerRollback/")
			+ FGuid::NewGuid().ToString(EGuidFormats::Digits)));
	UWorld* const World = UWorld::CreateWorld(
		EWorldType::EditorPreview,
		false,
		FName(TEXT("UAIDataLayerRollback")),
		Package);
	if (World)
	{
		World->SetFlags(RF_Public | RF_Standalone | RF_Transactional);
	}
	if (World && World->PersistentLevel)
	{
		World->PersistentLevel->SetUseExternalActors(true);
	}
	UWorldPartition* const WorldPartition = World
		? UWorldPartition::CreateOrRepairWorldPartition(
			World->GetWorldSettings())
		: nullptr;
	if (WorldPartition && !WorldPartition->IsInitialized())
	{
		WorldPartition->Initialize(World, FTransform::Identity);
	}
	AWorldDataLayers* const WorldDataLayers = World
		? World->GetWorldDataLayers()
		: nullptr;
	UDataLayerAsset* const DataLayerAsset = Package
		? NewObject<UDataLayerAsset>(
			Package,
			TEXT("RollbackDataLayer"),
			RF_Transactional)
		: nullptr;
	UDataLayerInstanceWithAsset* const DataLayer =
		WorldDataLayers && DataLayerAsset
		? WorldDataLayers->CreateDataLayer<UDataLayerInstanceWithAsset>(
			DataLayerAsset)
		: nullptr;
	if (!TestNotNull(TEXT("rollback world exists"), World)
		|| !TestNotNull(TEXT("rollback WorldPartition exists"), WorldPartition)
		|| !TestNotNull(TEXT("rollback WorldDataLayers exists"), WorldDataLayers)
		|| !TestNotNull(TEXT("rollback DataLayer exists"), DataLayer))
	{
		return false;
	}

	const TSharedRef<FRollbackAfterApplyRunner> Runner =
		MakeShared<FRollbackAfterApplyRunner>();
	TSharedPtr<IUeAgentWorldDestructiveOwner> Owner;
	FString Error;
	TestTrue(
		TEXT("world destructive owner accepts the durable runner port"),
		TryCreateWorldDestructiveOwner(
			*World,
			FPaths::ProjectSavedDir()
				/ TEXT("UeAgentInterface/Tests/WorldDestructiveRollback"),
			TEXT("world-destructive-data-layer-rollback-test"),
			Runner,
			Owner,
			Error));
	FUeAgentWorldDestructiveRequest Request;
	Request.RequestId = TEXT("data-layer-rollback-")
		+ FGuid::NewGuid().ToString(EGuidFormats::Digits);
	Request.CommandId = TEXT("world.data_layer.delete");
	Request.TargetRef = DataLayer->GetDataLayerShortName();
	Request.CallerId = TEXT("automation");
	const FUeAgentWorldDestructiveResult Result = Owner.IsValid()
		? Owner->Execute(Request, MakeShared<FNeverCancelled>())
		: FUeAgentWorldDestructiveResult();
	TestTrue(TEXT("failure probe reached native apply"), Runner->bApplyObserved);
	TestTrue(
		TEXT("failure probe used the native rollback callback"),
		Runner->bRollbackObserved);
	TestEqual(
		TEXT("post-mutation failure is explicitly rolled back"),
		Result.TerminalState,
		TEXT("rolled_back"));
	TestEqual(
		TEXT("post-mutation failure keeps its stable cause"),
		Result.ErrorCode,
		TEXT("fixture_post_mutation_failure"));
	const TArray<UDataLayerInstance*> RestoredDataLayers =
		UeAgentLevelAuthoring::FindDataLayerInstancesExact(
			*WorldDataLayers,
			Request.TargetRef);
	TestEqual(
		TEXT("rollback restores exactly one DataLayer in memory"),
		RestoredDataLayers.Num(),
		1);

	Owner.Reset();
	Package->SetDirtyFlag(false);
	World->DestroyWorld(false);
	World->MarkAsGarbage();
	Package->MarkAsGarbage();
	CollectGarbage(RF_NoFlags, true);
	return !HasAnyErrors();
}
