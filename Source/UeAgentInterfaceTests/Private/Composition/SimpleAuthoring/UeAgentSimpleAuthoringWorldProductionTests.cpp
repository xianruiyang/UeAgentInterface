#include "Misc/AutomationTest.h"

#include "Composition/SimpleAuthoring/Families/World/UeAgentWorldSimpleAuthoringProduction.h"
#include "Composition/SimpleAuthoring/UeAgentSimpleAuthoringComposition.h"
#include "Core/BindingGateway/Ticket/UeAgentBindingTicket.h"
#include "Core/Projection/UeAgentProjection.h"
#include "DomainAdapters/WorldSimpleEffect/UeAgentWorldSimpleEffectCapabilities.h"
#include "EditorLevelUtils.h"
#include "EditorValidatorSubsystem.h"
#include "Engine/StaticMesh.h"
#include "Engine/StaticMeshActor.h"
#include "Engine/Level.h"
#include "Engine/LevelStreaming.h"
#include "Engine/LevelStreamingDynamic.h"
#include "Engine/World.h"
#include "EngineUtils.h"
#include "GameFramework/Actor.h"
#include "Misc/Guid.h"
#include "UObject/Package.h"
#include "WorldPartition/DataLayer/DataLayerAsset.h"
#include "WorldPartition/DataLayer/DataLayerInstance.h"
#include "WorldPartition/DataLayer/DataLayerInstanceWithAsset.h"
#include "WorldPartition/DataLayer/WorldDataLayers.h"
#include "WorldPartition/HLOD/HLODLayer.h"
#include "WorldPartition/WorldPartition.h"

#include "Composition/SimpleAuthoring/UeAgentSimpleAuthoringDurableService.h"



#include "HAL/FileManager.h"
#include "Misc/PackageName.h"
#include "Misc/Paths.h"
#include "PackageTools.h"
#include "UObject/GarbageCollection.h"
#include "UObject/StrongObjectPtr.h"

namespace
{
	using namespace UE::UAI::AssetDurable;
	using namespace UE::UAI::BindingGateway::Ticket;
	using namespace UE::UAI::Composition::SimpleAuthoring;
	using namespace UE::UAI::Composition::SimpleAuthoring::World;
	using namespace UE::UAI::ContextResource;
	using namespace UE::UAI::Contract;
	using namespace UE::UAI::Discovery;
	using namespace UE::UAI::DomainAdapters::WorldSimpleEffect;
	using namespace UE::UAI::Projection;
	using namespace UE::UAI::StateSemantic;
	using namespace UE::UAI::Verification;

	static_assert(
		UAI_WORLD_SIMPLE_AUTHORING_PRODUCTION_V1 == 1,
		"World Simple Authoring production contract changed");
	static_assert(UAI_SIMPLE_AUTHORING_CHANGE_TARGETS_V1 == 1);
	static_assert(UAI_SIMPLE_AUTHORING_VERIFICATION_TARGETS_V1 == 1);

	class FOwnedWorld final
	{
	public:
		FOwnedWorld()
		{
			Package = CreatePackage(
				*(TEXT("/Game/UAI_Automation/SA008/WorldProduction_")
					+ FGuid::NewGuid().ToString(
						EGuidFormats::Digits)));
			World = UWorld::CreateWorld(
				EWorldType::EditorPreview,
				false,
				FName(TEXT("SA008WorldProduction")),
				Package);
			if (!World || !World->PersistentLevel)
			{
				return;
			}
			World->PersistentLevel->SetUseExternalActors(true);
			WorldPartition =
				UWorldPartition::CreateOrRepairWorldPartition(
					World->GetWorldSettings());
			if (WorldPartition
				&& !WorldPartition->IsInitialized())
			{
				WorldPartition->Initialize(
					World,
					FTransform::Identity);
			}
			World->SetCurrentLevel(World->PersistentLevel);
			Actor = World->SpawnActor<AActor>();
			WorldDataLayers = World->GetWorldDataLayers();
			DataLayerAsset = NewObject<UDataLayerAsset>(
				Package,
				TEXT("SA008WorldProductionDataLayer"),
				RF_Transactional);
			if (DataLayerAsset)
			{
				DataLayerAsset->AddToRoot();
				DataLayerAsset->SetType(EDataLayerType::Runtime);
			}
			if (WorldDataLayers && DataLayerAsset)
			{
				DataLayerInstance =
					WorldDataLayers->CreateDataLayer<
						UDataLayerInstanceWithAsset>(
							DataLayerAsset);
			}
			if (DataLayerInstance)
			{
				DataLayerInstance->SetInitialRuntimeState(
					EDataLayerRuntimeState::Unloaded);
				DataLayerInstance->SetVisible(true);
			}
			CreateDataLayerAsset = NewObject<UDataLayerAsset>(
				Package,
				TEXT("SA008WorldProductionCreateDataLayer"),
				RF_Transactional);
			if (CreateDataLayerAsset)
			{
				CreateDataLayerAsset->AddToRoot();
				CreateDataLayerAsset->SetType(
					EDataLayerType::Runtime);
			}
			HlodLayer = NewObject<UHLODLayer>(
				Package,
				TEXT("SA008WorldProductionHLOD"),
				RF_Transactional);
			if (HlodLayer)
			{
				HlodLayer->AddToRoot();
			}
			Mesh = LoadObject<UStaticMesh>(
				nullptr,
				TEXT("/Engine/BasicShapes/Cube.Cube"));
			StreamingAsset = LoadObject<UWorld>(
				nullptr,
				TEXT(
					"/Engine/Maps/Templates/"
					"Template_Default.Template_Default"));
			if (StreamingAsset)
			{
				StreamingLevel =
					UEditorLevelUtils::AddLevelToWorld(
						World,
						*StreamingAsset
							->GetOutermost()
							->GetName(),
						ULevelStreamingDynamic::StaticClass(),
						FTransform::Identity);
			}
			ContextRevision = FString::Printf(
				TEXT("sa008-world-production:%u"),
				World->GetUniqueID());
			Package->SetDirtyFlag(false);
		}

		~FOwnedWorld()
		{
			if (Package)
			{
				Package->SetDirtyFlag(false);
			}
			if (World)
			{
				World->DestroyWorld(false);
				World->MarkAsGarbage();
			}
			if (HlodLayer && HlodLayer->IsRooted())
			{
				HlodLayer->RemoveFromRoot();
			}
			if (DataLayerAsset
				&& DataLayerAsset->IsRooted())
			{
				DataLayerAsset->RemoveFromRoot();
			}
			if (CreateDataLayerAsset
				&& CreateDataLayerAsset->IsRooted())
			{
				CreateDataLayerAsset->RemoveFromRoot();
			}
			if (Package)
			{
				Package->MarkAsGarbage();
			}
		}

		bool IsValid() const
		{
			return Package
				&& World
				&& World->PersistentLevel
				&& WorldPartition
				&& WorldPartition->IsInitialized()
				&& Actor
				&& WorldDataLayers
				&& DataLayerAsset
				&& DataLayerInstance
				&& CreateDataLayerAsset
				&& HlodLayer
				&& Mesh
				&& StreamingAsset
				&& StreamingLevel
				&& !ContextRevision.IsEmpty();
		}

		UPackage* Package = nullptr;
		UWorld* World = nullptr;
		UWorldPartition* WorldPartition = nullptr;
		AActor* Actor = nullptr;
		AWorldDataLayers* WorldDataLayers = nullptr;
		UDataLayerAsset* DataLayerAsset = nullptr;
		UDataLayerInstance* DataLayerInstance = nullptr;
		UDataLayerAsset* CreateDataLayerAsset = nullptr;
		UHLODLayer* HlodLayer = nullptr;
		UStaticMesh* Mesh = nullptr;
		UWorld* StreamingAsset = nullptr;
		ULevelStreaming* StreamingLevel = nullptr;
		FString ContextRevision;
	};

	class FOwnedWorldCatalog final
		: public IUeAgentWorldSimpleEffectWorldCatalog
	{
	public:
		explicit FOwnedWorldCatalog(FOwnedWorld& InOwner)
			: Owner(InOwner)
		{
		}

		bool TryEnumerate(
			TArray<FUeAgentWorldSimpleEffectWorldCandidate>&
				OutCandidates,
			UE::UAI::ExecutionMutation::FExecutionMutationError&
				OutError) override
		{
			++EnumerateCount;
			OutCandidates.Reset();
			OutCandidates.Emplace(
				Owner.World,
				Owner.World->GetPathName(),
				Owner.World->PersistentLevel->GetPathName(),
				EUeAgentExecutionWorldType::Preview,
				TOptional<int32>(),
				Owner.ContextRevision);
			return true;
		}

		int32 EnumerateCount = 0;

	private:
		FOwnedWorld& Owner;
	};

	class FOwnedDurableWorld final
	{
	public:
		FOwnedDurableWorld()
		{
			const FString Unique = FGuid::NewGuid().ToString(
				EGuidFormats::Digits);
			Root = FPaths::ConvertRelativePathToFull(
				FPaths::ProjectSavedDir()
				/ TEXT("UeAgentInterface/SA008")
				/ (TEXT("WorldE4_") + Unique));
			PackageRoot = Root / TEXT("Package");
			JournalRoot = Root / TEXT("Journal");
			MountPoint = TEXT("/UaiSa008WorldE4_")
				+ Unique
				+ TEXT("/");
			PackageName = MountPoint + TEXT("WorldE4");
			ObjectPath = PackageName + TEXT(".WorldE4");
			ContextRevision = TEXT("sa008-world-e4:") + Unique;

			IFileManager::Get().DeleteDirectory(*Root, false, true);
			if (!IFileManager::Get().MakeDirectory(
					*PackageRoot,
					true))
			{
				return;
			}
			FString RootWithSlash = PackageRoot / TEXT("");
			FPaths::NormalizeDirectoryName(RootWithSlash);
			RootWithSlash += TEXT("/");
			RegisteredRoot = RootWithSlash;
			FPackageName::RegisterMountPoint(
				MountPoint,
				RegisteredRoot);
			bMounted = true;

			Package.Reset(CreatePackage(*PackageName));
			World.Reset(UWorld::CreateWorld(
				EWorldType::EditorPreview,
				false,
				FName(TEXT("WorldE4")),
				Package.Get(),
				false));
			if (!World.IsValid()
				|| !World->PersistentLevel)
			{
				return;
			}
			World->SetFlags(RF_Public | RF_Standalone);
			World->PersistentLevel->SetUseExternalActors(false);
			WorldPartition =
				UWorldPartition::CreateOrRepairWorldPartition(
					World->GetWorldSettings());
			if (WorldPartition
				&& !WorldPartition->IsInitialized())
			{
				WorldPartition->Initialize(
					World.Get(),
					FTransform::Identity);
			}
			World->SetCurrentLevel(World->PersistentLevel);
			WorldIdentity = World->GetPathName();
			LevelIdentity =
				World->PersistentLevel->GetPathName();
			InitialUniqueId = World->GetUniqueID();
			InitialWorldHandle = World.Get();
			Package->SetDirtyFlag(false);
		}

		~FOwnedDurableWorld()
		{
			ReleaseLoadedWorld();
			if (bMounted)
			{
				FPackageName::UnRegisterMountPoint(
					MountPoint,
					RegisteredRoot);
			}
			IFileManager::Get().DeleteDirectory(
				*Root,
				false,
				true);
		}

		bool IsValid() const
		{
			return bMounted
				&& Package.IsValid()
				&& World.IsValid()
				&& World->PersistentLevel
				&& WorldPartition
				&& WorldPartition->IsInitialized()
				&& InitialUniqueId != 0;
		}

		bool TryReload(FString& OutError)
		{
			OutError.Reset();
			if (bReloaded)
			{
				return World.IsValid()
					&& WorldPartition;
			}
			ReleaseLoadedWorld();
			UPackage* LoadedPackage = LoadPackage(
				nullptr,
				*PackageName,
				LOAD_None);
			UWorld* LoadedWorld = LoadedPackage
				? UWorld::FindWorldInPackage(LoadedPackage)
				: nullptr;
			if (!LoadedPackage
				|| !LoadedWorld
				|| !LoadedWorld->PersistentLevel
				|| !LoadedWorld->GetWorldPartition())
			{
				OutError =
					TEXT("sa008_world_e4_reload_failed");
				return false;
			}
			Package.Reset(LoadedPackage);
			World.Reset(LoadedWorld);
			WorldPartition = LoadedWorld->GetWorldPartition();
			ReloadedUniqueId = LoadedWorld->GetUniqueID();
			const TWeakObjectPtr<UWorld> ReloadedWorldHandle(
				LoadedWorld);
			bReloaded =
				!InitialWorldHandle.IsValid()
				&& !InitialWorldHandle
					.HasSameIndexAndSerialNumber(
						ReloadedWorldHandle);
			if (!bReloaded)
			{
				OutError =
					TEXT("sa008_world_e4_read_owner_reused");
			}
			return bReloaded;
		}

		FString HeaderPath() const
		{
			return FPackageName::LongPackageNameToFilename(
				PackageName,
				FPackageName::GetMapPackageExtension());
		}

		bool WasReloaded() const
		{
			return bReloaded;
		}

		uint32 GetReloadedUniqueId() const
		{
			return ReloadedUniqueId;
		}

		bool WasOriginalWorldCollected() const
		{
			return !InitialWorldHandle.IsValid();
		}

		void ReleaseLoadedWorld()
		{
			UPackage* PackageToUnload = Package.Get();
			if (Package.IsValid())
			{
				Package->SetDirtyFlag(false);
			}
			World.Reset();
			Package.Reset();
			WorldPartition = nullptr;
			if (PackageToUnload)
			{
				FText UnloadError;
				UPackageTools::UnloadPackages(
					{PackageToUnload},
					UnloadError,
					true);
			}
			CollectGarbage(RF_NoFlags, true);
		}

		FString Root;
		FString PackageRoot;
		FString JournalRoot;
		FString PackageName;
		FString ObjectPath;
		FString WorldIdentity;
		FString LevelIdentity;
		FString ContextRevision;
		uint32 InitialUniqueId = 0;

		TStrongObjectPtr<UPackage> Package;
		TStrongObjectPtr<UWorld> World;
		UWorldPartition* WorldPartition = nullptr;

	private:
		FString MountPoint;
		FString RegisteredRoot;
		TWeakObjectPtr<UWorld> InitialWorldHandle;
		uint32 ReloadedUniqueId = 0;
		bool bMounted = false;
		bool bReloaded = false;
	};

	class FOwnedDurableWorldCatalog final
		: public IUeAgentWorldSimpleEffectWorldCatalog
	{
	public:
		explicit FOwnedDurableWorldCatalog(
			FOwnedDurableWorld& InOwner)
			: Owner(InOwner)
		{
		}

		bool TryEnumerate(
			TArray<FUeAgentWorldSimpleEffectWorldCandidate>&
				OutCandidates,
			UE::UAI::ExecutionMutation::FExecutionMutationError&
				OutError) override
		{
			++EnumerateCount;
			OutCandidates.Reset();
			if (!Owner.World.IsValid())
			{
				OutError =
					UE::UAI::ExecutionMutation::
						FExecutionMutationError(
							TEXT(
								"sa008_world_e4_source_missing"));
				return false;
			}
			OutCandidates.Emplace(
				Owner.World.Get(),
				Owner.World->GetPathName(),
				Owner.World->PersistentLevel->GetPathName(),
				EUeAgentExecutionWorldType::Preview,
				TOptional<int32>(),
				Owner.ContextRevision);
			return true;
		}

		int32 EnumerateCount = 0;

	private:
		FOwnedDurableWorld& Owner;
	};

	class FReloadedDurableWorldCatalog final
		: public IUeAgentWorldSimpleEffectWorldCatalog
	{
	public:
		explicit FReloadedDurableWorldCatalog(
			FOwnedDurableWorld& InOwner)
			: Owner(InOwner)
		{
		}

		bool TryEnumerate(
			TArray<FUeAgentWorldSimpleEffectWorldCandidate>&
				OutCandidates,
			UE::UAI::ExecutionMutation::FExecutionMutationError&
				OutError) override
		{
			++EnumerateCount;
			OutCandidates.Reset();
			FString ReloadError;
			if (!Owner.TryReload(ReloadError))
			{
				OutError =
					UE::UAI::ExecutionMutation::
						FExecutionMutationError(
							ReloadError);
				return false;
			}
			OutCandidates.Emplace(
				Owner.World.Get(),
				Owner.World->GetPathName(),
				Owner.World->PersistentLevel->GetPathName(),
				EUeAgentExecutionWorldType::Preview,
				TOptional<int32>(),
				Owner.ContextRevision);
			return true;
		}

		int32 EnumerateCount = 0;

	private:
		FOwnedDurableWorld& Owner;
	};

	class FRejectingReferenceResolver final
		: public IUeAgentWorldSimpleEffectReferenceResolver
	{
	public:
		bool TryResolveReference(
			UWorld& World,
			const FString& CanonicalRef,
			UObject*& OutObject,
			UE::UAI::ExecutionMutation::FExecutionMutationError&
				OutError) override
		{
			++ResolveCount;
			OutObject = nullptr;
			OutError =
				UE::UAI::ExecutionMutation::FExecutionMutationError(
					TEXT(
						"sa008_world_reference_must_not_be_used"),
					CanonicalRef);
			return false;
		}

		int32 ResolveCount = 0;
	};

	class FBoundReferenceResolver final
		: public IUeAgentWorldSimpleEffectReferenceResolver
	{
	public:
		void Bind(
			FString CanonicalRef,
			UObject* Object)
		{
			Bindings.Add(MoveTemp(CanonicalRef), Object);
		}

		bool TryResolveReference(
			UWorld& World,
			const FString& CanonicalRef,
			UObject*& OutObject,
			UE::UAI::ExecutionMutation::FExecutionMutationError&
				OutError) override
		{
			++ResolveCount;
			OutObject = Bindings.FindRef(CanonicalRef).Get();
			if (!OutObject)
			{
				OutObject = nullptr;
				OutError =
					UE::UAI::ExecutionMutation::
						FExecutionMutationError(
							TEXT(
								"sa008_world_reference_unresolved"),
							CanonicalRef);
				return false;
			}
			return true;
		}

		int32 ResolveCount = 0;

	private:
		TMap<FString, TWeakObjectPtr<UObject>> Bindings;
	};

	class FRecordingDurableService final
		: public IUeAgentSimpleAuthoringAssetDurableService
	{
	public:
		bool TryPrepare(
			const FAssetDurableRequest& Request,
			FUeAgentResourceTracker& Resources,
			FAssetDurableSessionPtr& OutSession,
			FAssetDurableError& OutError) override
		{
			++PrepareCount;
			if (!Request.IsValid()
				|| Request.GetTargets().Num() != 1
				|| Request.GetTargets()[0].GetArtifactKind()
					!= EDurableArtifactKind::Package)
			{
				OutError = FAssetDurableError(
					TEXT("sa008_world_durable_target_invalid"));
				return false;
			}
			SeenPackageName =
				Request.GetTargets()[0].GetPackageName();
			TArray<FDurableArtifact> Artifacts;
			Artifacts.Emplace(
				TEXT("artifact:sa008-world-production"),
				Request.GetTargets()[0].GetIdentity(),
				SeenPackageName,
				TEXT(
					"fixture://sa008-world-production.umap"),
				EAssetDurableIntent::Save,
				TEXT("world_package"));
			FString InventoryHash;
			if (!TryComputeDurableInventoryHash(
					Artifacts,
					Resources,
					InventoryHash,
					OutError))
			{
				return false;
			}
			OutSession = MakeShared<const FAssetDurableSession>(
				Request,
				FDurableInventory(
					MoveTemp(Artifacts),
					MoveTemp(InventoryHash)),
				TEXT("journal:sa008-world-production"),
				TEXT(
					"runtimeLogs/SA008WorldProduction/"
					"journal.hjson"),
				TEXT("runtimeLogs/SA008WorldProduction"),
				TEXT(
					"runtimeLogs/SA008WorldProduction/"
					"transaction"),
				TEXT(
					"runtimeLogs/SA008WorldProduction/staging"),
				TArray<FString>{TEXT("prepared")});
			return OutSession.IsValid();
		}

		bool TryEnlist(
			const FAssetDurableSessionRef& Session,
			const TArray<FAssetDurableTarget>& MutationTargets,
			const int64 CurrentFencingToken,
			FUeAgentResourceTracker& Resources,
			FAssetDurableSessionPtr& OutSession,
			FAssetDurableError& OutError) override
		{
			++EnlistCount;
			OutError = FAssetDurableError();
			OutSession = Session;
			return CurrentFencingToken
				== Session->GetRequest().GetFencingToken();
		}

		bool TryPersist(
			const FAssetDurableSessionRef& Session,
			const int64 CurrentFencingToken,
			FUeAgentResourceTracker& Resources,
			FDurableReceipt& OutReceipt,
			FAssetDurableError& OutError) override
		{
			++PersistCount;
			OutReceipt = FDurableReceipt(
				Session->GetJournalId(),
				EDurableStatus::PersistedUnverified,
				Session->GetRequest().GetPlan()->GetFingerprint(),
				Session->GetInventory().GetInventoryHash(),
				{
					TEXT("prepared"),
					TEXT("persisted_unverified"),
				},
				ERecoveryAction::None,
				true,
				false);
			return CurrentFencingToken
					== Session->GetRequest().GetFencingToken()
				&& OutReceipt.RequiresIndependentVerification();
		}

		bool TryRollback(
			const FAssetDurableSessionRef& Session,
			const int64 CurrentFencingToken,
			const bool bMemoryRollbackConfirmed,
			FUeAgentResourceTracker& Resources,
			FDurableReceipt& OutReceipt,
			FAssetDurableError& OutError) override
		{
			OutError = FAssetDurableError(
				TEXT("sa008_world_unexpected_durable_rollback"));
			return false;
		}

		bool TryAcknowledge(
			const FAssetDurableSessionRef& Session,
			const FDurableVerificationAcknowledgement&
				Acknowledgement,
			FUeAgentResourceTracker& Resources,
			FAssetDurableError& OutError) override
		{
			++AcknowledgeCount;
			return Acknowledgement.IsValid()
				&& Acknowledgement.GetJournalId()
					== Session->GetJournalId();
		}

		int32 PrepareCount = 0;
		int32 EnlistCount = 0;
		int32 PersistCount = 0;
		int32 AcknowledgeCount = 0;
		FString SeenPackageName;
	};

	class FUnusedAbsenceOracle final : public IAbsenceOracle
	{
	public:
		bool TryObserve(
			const FUeAgentCanonicalIdentity& Identity,
			const TArray<FDurableArtifact>& Artifacts,
			FUeAgentResourceTracker& Resources,
			const FUeAgentOperationDeadline& Deadline,
			const FUeAgentCancellationToken& Cancellation,
			FAbsenceObservation& OutObservation,
			FVerificationError& OutError) override
		{
			OutObservation = FAbsenceObservation(
				Identity,
				false,
				true,
				TEXT("unused"));
			return true;
		}
	};

	class FUnusedReferenceOracle final : public IReferenceOracle
	{
	public:
		bool TryObserve(
			const FUeAgentCanonicalIdentity& Identity,
			FUeAgentResourceTracker& Resources,
			const FUeAgentOperationDeadline& Deadline,
			const FUeAgentCancellationToken& Cancellation,
			FReferenceObservation& OutObservation,
			FVerificationError& OutError) override
		{
			OutObservation = FReferenceObservation(
				Identity,
				{},
				true,
				TEXT("unused"));
			return true;
		}
	};

	class FTokenEstimator final : public IUeAgentTokenEstimator
	{
	public:
		bool TryEstimate(
			const FUeAgentProjectedPayload& Payload,
			const TArray<FUeAgentArtifactDescriptor>& Artifacts,
			const TArray<FUeAgentProjectionDiagnostic>& Diagnostics,
			FUeAgentResourceTracker& Resources,
			const FUeAgentOperationDeadline& Deadline,
			const FUeAgentCancellationToken& Cancellation,
			FUeAgentTokenEstimate& OutEstimate,
			FUeAgentProjectionError& OutError) override
		{
			OutEstimate = FUeAgentTokenEstimate(
				EUeAgentTokenEstimateKind::Exact,
				32,
				128);
			return true;
		}
	};

	class FNeverCancelled final
		: public IUeAgentCancellationSignal
	{
	public:
		bool IsCancelled() const override
		{
			return false;
		}
	};

	int32 CountActorsWithLabelPrefix(
		UWorld& World,
		const FString& Prefix)
	{
		int32 Count = 0;
		for (TActorIterator<AActor> It(&World); It; ++It)
		{
			if (It->GetActorLabel().StartsWith(Prefix))
			{
				++Count;
			}
		}
		return Count;
	}

	AActor* FindActorWithExactLabel(
		UWorld& World,
		const FString& Label)
	{
		AActor* Match = nullptr;
		for (TActorIterator<AActor> It(&World); It; ++It)
		{
			if (It->GetActorLabel() == Label)
			{
				if (Match)
				{
					return nullptr;
				}
				Match = *It;
			}
		}
		return Match;
	}

	UDataLayerInstance* FindPrivateDataLayer(
		AWorldDataLayers& WorldDataLayers,
		const FString& ShortName)
	{
		UDataLayerInstance* Match = nullptr;
		WorldDataLayers.ForEachDataLayerInstance(
			[&Match, &ShortName](UDataLayerInstance* Instance)
			{
			if (Instance
				&& Instance->GetAsset()
				&& Instance->GetAsset()->IsPrivate()
				&& Instance->GetDataLayerShortName()
					== ShortName)
			{
				Match = Instance;
				return false;
			}
			return true;
			});
		return Match;
	}

	ULevelStreaming* FindStreamingRelation(
		UWorld& World,
		const FString& LevelPackage,
		const FVector& Location)
	{
		for (ULevelStreaming* Relation
			: World.GetStreamingLevels())
		{
			if (Relation
				&& Relation->GetWorldAssetPackageName()
					== LevelPackage
				&& Relation->LevelTransform.GetLocation().Equals(
					Location))
			{
				return Relation;
			}
		}
		return nullptr;
	}

	const FUeAgentSimpleAuthoringCommandRoute* FindRoute(
		const FString& CapabilityId =
			TEXT(
				"world.world_partition_streaming.update"))
	{
		return GetSimpleAuthoringCommandRoutes().FindByPredicate(
			[&CapabilityId](
				const FUeAgentSimpleAuthoringCommandRoute& Route)
			{
				return Route.CapabilityId == CapabilityId;
			});
	}
}


IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUeAgentSimpleAuthoringWorldProductionContractTest,
	"UeAgentInterface.Composition.SimpleAuthoring.E3.WorldProductionContract",
	EAutomationTestFlags::EditorContext
		| EAutomationTestFlags::EngineFilter)

bool FUeAgentSimpleAuthoringWorldProductionContractTest::RunTest(
	const FString& Parameters)
{
	FOwnedWorld Owner;
	TestTrue(TEXT("owned exact world is valid"), Owner.IsValid());
	if (!Owner.IsValid())
	{
		return false;
	}
	const bool bBefore =
		Owner.WorldPartition->IsStreamingEnabledInEditor();
	const bool bActorSpatialBefore =
		Owner.Actor->GetIsSpatiallyLoaded();
	const FName ActorGridBefore =
		Owner.Actor->GetRuntimeGrid();
	const EDataLayerRuntimeState DataLayerRuntimeStateBefore =
		Owner.DataLayerInstance->GetInitialRuntimeState();
	const bool bDataLayerVisibleBefore =
		Owner.DataLayerInstance->IsVisible();
	UHLODLayer* const ActorHlodBefore =
		Owner.Actor->GetHLODLayer();
	const bool bActorInDataLayerBefore =
		Owner.Actor->GetDataLayerInstanceNames().Contains(
			Owner.DataLayerInstance->GetDataLayerFName());
	const bool bStreamingLoadedBefore =
		Owner.StreamingLevel->ShouldBeLoaded();
	const bool bStreamingVisibleBefore =
		Owner.StreamingLevel->ShouldBeVisible();
	const FTransform StreamingTransformBefore =
		Owner.StreamingLevel->LevelTransform;

	TSharedPtr<const FUeAgentDescriptorRegistry> Registry;
	FString Error;
	TestTrue(
		TEXT("Simple Authoring descriptors compose"),
		TryCreateSimpleAuthoringDescriptorRegistry(
			Registry,
			Error));
	const FUeAgentSimpleAuthoringCommandRoute* Route = FindRoute();
	const FUeAgentSimpleAuthoringCommandRoute* ActorRoute =
		FindRoute(
			TEXT("world.actor_streaming_policy.update"));
	const FUeAgentSimpleAuthoringCommandRoute* DataLayerStateRoute =
		FindRoute(
			TEXT("world.data_layer_state.update"));
	const FUeAgentSimpleAuthoringCommandRoute* ActorHlodRoute =
		FindRoute(
			TEXT("world.actor_hlod_layer.update"));
	const FUeAgentSimpleAuthoringCommandRoute* DataLayerRelationRoute =
		FindRoute(
			TEXT("world.data_layer_relation.update"));
	const FUeAgentSimpleAuthoringCommandRoute*
		StreamingRelationStateRoute =
			FindRoute(
				TEXT(
					"world.streaming_relation.state.update"));
	const FUeAgentSimpleAuthoringCommandRoute* WallCreateRoute =
		FindRoute(
			TEXT("world.wall.create_with_opening"));
	const FUeAgentSimpleAuthoringCommandRoute* ActorCreateRoute =
		FindRoute(TEXT("world.actor.create"));
	const FUeAgentSimpleAuthoringCommandRoute* LevelGameModeRoute =
		FindRoute(TEXT("world.level_game_mode.update"));
	const FUeAgentSimpleAuthoringCommandRoute*
		DataLayerCreateRoute =
			FindRoute(
				TEXT("world.data_layer_instance.create"));
	const FUeAgentSimpleAuthoringCommandRoute*
		StreamingRelationCreateRoute =
			FindRoute(
				TEXT("world.streaming_relation.create"));
	TestNotNull(
		TEXT("World Partition streaming has one exact route"),
		Route);
	TestNotNull(
		TEXT("Actor streaming policy has one exact route"),
		ActorRoute);
	TestNotNull(
		TEXT("DataLayer state has one exact route"),
		DataLayerStateRoute);
	TestNotNull(
		TEXT("Actor HLOD membership has one exact route"),
		ActorHlodRoute);
	TestNotNull(
		TEXT("DataLayer membership has one exact route"),
		DataLayerRelationRoute);
	TestNotNull(
		TEXT("Streaming relation state has one exact route"),
		StreamingRelationStateRoute);
	TestNotNull(
		TEXT("Wall create has one exact route"),
		WallCreateRoute);
	TestNotNull(
		TEXT("Actor create has one exact route"),
		ActorCreateRoute);
	TestNotNull(
		TEXT("Level GameMode override has one exact route"),
		LevelGameModeRoute);
	TestNotNull(
		TEXT("DataLayer create has one exact route"),
		DataLayerCreateRoute);
	TestNotNull(
		TEXT("Streaming relation create has one exact route"),
		StreamingRelationCreateRoute);
	if (!Registry.IsValid()
		|| !Route
		|| !ActorRoute
		|| !DataLayerStateRoute
		|| !ActorHlodRoute
		|| !DataLayerRelationRoute
		|| !StreamingRelationStateRoute
		|| !WallCreateRoute
		|| !ActorCreateRoute
		|| !LevelGameModeRoute
		|| !DataLayerCreateRoute
		|| !StreamingRelationCreateRoute)
	{
		AddError(TEXT("sa008_world_route_missing:") + Error);
		return false;
	}

	const TSharedRef<FOwnedWorldCatalog, ESPMode::ThreadSafe>
		Catalog = MakeShared<
			FOwnedWorldCatalog,
			ESPMode::ThreadSafe>(Owner);
	const TSharedRef<
		FBoundReferenceResolver,
		ESPMode::ThreadSafe> ReferenceResolver =
			MakeShared<
				FBoundReferenceResolver,
				ESPMode::ThreadSafe>();
	ReferenceResolver->Bind(
		TEXT("uai://actor/sa008-owned"),
		Owner.Actor);
	ReferenceResolver->Bind(
		TEXT("uai://data-layer/sa008-owned"),
		Owner.DataLayerInstance);
	ReferenceResolver->Bind(
		TEXT("uai://hlod/sa008-owned"),
		Owner.HlodLayer);
	ReferenceResolver->Bind(
		TEXT("uai://streaming-relation/sa008-owned"),
		Owner.StreamingLevel);
	ReferenceResolver->Bind(
		TEXT("/Script/Engine.StaticMeshActor"),
		AStaticMeshActor::StaticClass());
	ReferenceResolver->Bind(
		TEXT("/Engine/BasicShapes/Cube.Cube"),
		Owner.Mesh);
	ReferenceResolver->Bind(
		TEXT("uai://data-layer-asset/sa008-create"),
		Owner.CreateDataLayerAsset);
	ReferenceResolver->Bind(
		TEXT("uai://streaming-asset/sa008-create"),
		Owner.StreamingAsset);
	ReferenceResolver->Bind(
		TEXT("/Script/Engine.LevelStreamingDynamic"),
		ULevelStreamingDynamic::StaticClass());
	const TSharedRef<
		FRecordingDurableService,
		ESPMode::ThreadSafe> Durable =
			MakeShared<
				FRecordingDurableService,
				ESPMode::ThreadSafe>();

	FUeAgentWorldSimpleAuthoringProviderConfig ProviderConfig;
	ProviderConfig.DescriptorRegistry = Registry;
	ProviderConfig.WorldCatalog = Catalog;
	ProviderConfig.PersistedWorldCatalog = Catalog;
	ProviderConfig.ReferenceResolver = ReferenceResolver;
	ProviderConfig.DurableService = Durable;
	ProviderConfig.AbsenceOracle =
		MakeShared<FUnusedAbsenceOracle>();
	ProviderConfig.ReferenceOracle =
		MakeShared<FUnusedReferenceOracle>();
	ProviderConfig.OwnerLeaseId =
		TEXT("owner:sa008-world-production");
	ProviderConfig.Projection.Emplace(
		FUeAgentProjectionProfile(
			TEXT("compact"),
			false,
			false,
			false),
		FUeAgentPageRequest(),
		TOptional<int64>(),
		TArray<FUeAgentArtifactDescriptor>(),
		TArray<FUeAgentProjectionDiagnostic>());

	TSharedPtr<IUeAgentSimpleAuthoringCapabilityProvider> Provider;
	FUeAgentWorldSimpleAuthoringError ProviderError;
	TestTrue(
		TEXT("World production family composes"),
		FUeAgentWorldSimpleAuthoringProviderFactory::TryCreate(
			ProviderConfig,
			Provider,
			ProviderError));
	if (!Provider.IsValid())
	{
		AddError(
			TEXT("sa008_world_provider_invalid:")
			+ ProviderError.Code);
		return false;
	}

	TSharedPtr<const FUeAgentOperationTicketService> TicketService;
	FUeAgentOperationTicketError TicketError;
	TestTrue(
		TEXT("ticket service creates"),
		FUeAgentOperationTicketService::TryCreate(
			TEXT("sa008-world-production-integrity"),
			30.0,
			[]()
			{
				return 100.0;
			},
			[]()
			{
				return FString(
					TEXT("ticket:sa008-world-production"));
			},
			TicketService,
			TicketError));
	TSharedPtr<const FUeAgentResponseProjector> Projector;
	FUeAgentProjectionError ProjectionError;
	TestTrue(
		TEXT("response projector creates"),
		FUeAgentResponseProjector::TryCreate(
			MakeShared<FTokenEstimator>(),
			Projector,
			ProjectionError));
	if (!TicketService.IsValid() || !Projector.IsValid())
	{
		return false;
	}

	FUeAgentSimpleAuthoringApplicationConfig ApplicationConfig;
	ApplicationConfig.DescriptorRegistry = Registry;
	ApplicationConfig.ExecutionContext.ProjectInstanceId =
		TEXT("project:sa008-world-production");
	ApplicationConfig.ExecutionContext.EditorInstanceId =
		TEXT("editor:sa008-world-production");
	ApplicationConfig.ExecutionContext.StatePlane =
		EUeAgentStatePlane::AuthoringMemory;
	ApplicationConfig.ExecutionContext.WorldType =
		EUeAgentExecutionWorldType::Preview;
	ApplicationConfig.ExecutionContext.WorldIdentity =
		Owner.World->GetPathName();
	ApplicationConfig.ExecutionContext.LevelIdentity =
		Owner.World->PersistentLevel->GetPathName();
	ApplicationConfig.ExecutionContext.WorldPartitionIdentity =
		Owner.WorldPartition->GetPathName();
	ApplicationConfig.ExecutionContext.ContextRevision =
		Owner.ContextRevision;
	ApplicationConfig.ExecutionContext.bResolvedFromCurrent = false;
	ApplicationConfig.ServerSessionId =
		TEXT("session:sa008-world-production");
	ApplicationConfig.RegistryEpoch = 1;
	ApplicationConfig.TimeoutSeconds = 30.0;
	ApplicationConfig.MaxFrameBytes = 1024 * 1024;

	TSharedPtr<FUeAgentSimpleAuthoringApplication> Application;
	FUeAgentSimpleAuthoringApplicationError ApplicationError;
	TestTrue(
		TEXT("application accepts the exact World route"),
		FUeAgentSimpleAuthoringApplication::TryCreate(
			ApplicationConfig,
			TArray<FUeAgentSimpleAuthoringCommandRoute>{
				*Route,
				*ActorRoute,
				*DataLayerStateRoute,
				*ActorHlodRoute,
				*DataLayerRelationRoute,
				*StreamingRelationStateRoute,
				*WallCreateRoute,
				*ActorCreateRoute,
				*DataLayerCreateRoute,
				*StreamingRelationCreateRoute,
			},
			{Provider.ToSharedRef()},
			TicketService.ToSharedRef(),
			Projector.ToSharedRef(),
			Application,
			ApplicationError));
	if (!Application.IsValid())
	{
		AddError(
			TEXT("sa008_world_application_invalid:")
			+ ApplicationError.Code);
		return false;
	}

	const FUeAgentOperationResult Result = Application->Submit(
		FUeAgentOperationRequest(
			Route->DescriptorIdentity,
			TEXT("request:sa008-world-production"),
			TEXT("idempotency:sa008-world-production"),
			FUeAgentContractValue(
				bBefore
					? TEXT("{\"enabled\":false}")
					: TEXT("{\"enabled\":true}"))),
		FUeAgentCallerContext(
			TEXT("caller:sa008-world-production"),
			{TEXT("uai:world:write")},
			TEXT("correlation:sa008-world-production")),
		MakeShared<FNeverCancelled>());

	const FString ResultJson =
		Result.GetValue().GetJsonValue();
	const bool bCompleted = ResultJson.Contains(
			TEXT("\"effect_outcome\":\"complete\""))
		&& ResultJson.Contains(TEXT("\"status\":\"verified\""));
	TestTrue(
		TEXT("real World production operation completes"),
		bCompleted);
	if (!bCompleted)
	{
		AddError(TEXT("sa008_world_result:") + ResultJson);
		AddError(
			FString::Printf(
				TEXT("sa008_world_catalog_enumerate_count:%d"),
				Catalog->EnumerateCount));
	}
	TestEqual(
		TEXT("real World native hook mutates the exact owned world"),
		Owner.WorldPartition->IsStreamingEnabledInEditor(),
		!bBefore);
	TestEqual(
		TEXT("C8D receives the exact owned world package"),
		Durable->SeenPackageName,
		Owner.Package->GetName());
	TestEqual(
		TEXT("World durability prepares once"),
		Durable->PrepareCount,
		1);
	TestEqual(
		TEXT("World durability persists once"),
		Durable->PersistCount,
		1);
	TestEqual(
		TEXT("C9 acknowledges only after independent World readback"),
		Durable->AcknowledgeCount,
		1);
	TestEqual(
		TEXT("reference resolver is not consulted by the no-ref profile"),
		ReferenceResolver->ResolveCount,
		0);
	TestTrue(
		TEXT("target, pre-state, mutation and C9 re-resolve exact world"),
		Catalog->EnumerateCount >= 4);

	const FUeAgentOperationResult ActorResult =
		Application->Submit(
			FUeAgentOperationRequest(
				ActorRoute->DescriptorIdentity,
				TEXT("request:sa008-world-actor-policy"),
				TEXT("idempotency:sa008-world-actor-policy"),
				FUeAgentContractValue(
					FString::Printf(
						TEXT(
							"{\"changes\":{"
							"\"is_spatially_loaded\":%s,"
							"\"runtime_grid\":{"
							"\"intent\":\"set\","
							"\"value\":\"SA008Grid\"}},"
							"\"target_refs\":["
							"\"uai://actor/sa008-owned\"]}"),
						bActorSpatialBefore
							? TEXT("false")
							: TEXT("true")))),
			FUeAgentCallerContext(
				TEXT("caller:sa008-world-production"),
				{TEXT("uai:world:write")},
				TEXT(
					"correlation:sa008-world-actor-policy")),
			MakeShared<FNeverCancelled>());
	const FString ActorResultJson =
		ActorResult.GetValue().GetJsonValue();
	const bool bActorCompleted =
		ActorResultJson.Contains(
			TEXT("\"effect_outcome\":\"complete\""))
		&& ActorResultJson.Contains(
			TEXT("\"status\":\"verified\""));
	TestTrue(
		TEXT(
			"second World profile uses the same production lifecycle"),
		bActorCompleted);
	if (!bActorCompleted)
	{
		AddError(
			TEXT("sa008_world_actor_policy_result:")
				+ ActorResultJson);
	}
	TestEqual(
		TEXT("Actor streaming policy mutates the exact bound actor"),
		Owner.Actor->GetRuntimeGrid(),
		FName(TEXT("SA008Grid")));
	TestEqual(
		TEXT("Actor spatial policy reads back independently"),
		Owner.Actor->GetIsSpatiallyLoaded(),
		!bActorSpatialBefore);
	TestTrue(
		TEXT(
			"reference-bound profile resolves pre-state, mutation and C9"),
		ReferenceResolver->ResolveCount >= 3);
	TestEqual(
		TEXT("both World profiles prepare through one C8D port"),
		Durable->PrepareCount,
		2);
	TestEqual(
		TEXT("both World profiles persist through one C8D port"),
		Durable->PersistCount,
		2);
	TestEqual(
		TEXT("both World profiles acknowledge only after C9"),
		Durable->AcknowledgeCount,
		2);

	const auto SubmitWorldProfile =
		[this,
		 &Application](
			const FUeAgentSimpleAuthoringCommandRoute& ProfileRoute,
			const FString& RequestId,
			const FString& Payload,
			const TCHAR* Assertion) -> bool
		{
			const FUeAgentOperationResult ProfileResult =
				Application->Submit(
					FUeAgentOperationRequest(
						ProfileRoute.DescriptorIdentity,
						RequestId,
						TEXT("idempotency:") + RequestId,
						FUeAgentContractValue(Payload)),
					FUeAgentCallerContext(
						TEXT("caller:sa008-world-production"),
						{TEXT("uai:world:write")},
						TEXT("correlation:") + RequestId),
					MakeShared<FNeverCancelled>());
			const FString ProfileResultJson =
				ProfileResult.GetValue().GetJsonValue();
			const bool bProfileCompleted =
				ProfileResultJson.Contains(
					TEXT("\"effect_outcome\":\"complete\""))
				&& ProfileResultJson.Contains(
					TEXT("\"status\":\"verified\""));
			TestTrue(Assertion, bProfileCompleted);
			if (!bProfileCompleted)
			{
				AddError(
					RequestId
					+ TEXT(":")
					+ ProfileResultJson);
			}
			return bProfileCompleted;
		};
	const auto SubmitWorldCreateConflict =
		[this,
		 &Application](
			const FUeAgentSimpleAuthoringCommandRoute& ProfileRoute,
			const FString& RequestId,
			const FString& Payload,
			const TCHAR* Assertion) -> bool
		{
			const FUeAgentOperationResult ProfileResult =
				Application->Submit(
					FUeAgentOperationRequest(
						ProfileRoute.DescriptorIdentity,
						RequestId,
						TEXT("idempotency:") + RequestId,
						FUeAgentContractValue(Payload)),
					FUeAgentCallerContext(
						TEXT("caller:sa008-world-production"),
						{TEXT("uai:world:write")},
						TEXT("correlation:") + RequestId),
					MakeShared<FNeverCancelled>());
			const FString ProfileResultJson =
				ProfileResult.GetValue().GetJsonValue();
			const bool bRejected =
				ProfileResultJson.Contains(
					TEXT("world_simple_create_target_exists"))
				&& !ProfileResultJson.Contains(
					TEXT("\"effect_outcome\":\"complete\""));
			TestTrue(Assertion, bRejected);
			if (!bRejected)
			{
				AddError(
					RequestId
					+ TEXT(":")
					+ ProfileResultJson);
			}
			return bRejected;
		};

	SubmitWorldProfile(
		*DataLayerStateRoute,
		TEXT("request:sa008-world-data-layer-state"),
		TEXT(
			"{\"changes\":{"
			"\"initial_runtime_state\":\"loaded\","
			"\"visible\":false},"
			"\"target_refs\":["
			"\"uai://data-layer/sa008-owned\"]}"),
		TEXT(
			"DataLayer state uses the same production lifecycle"));
	TestEqual(
		TEXT("DataLayer runtime state reads back from the bound instance"),
		Owner.DataLayerInstance->GetInitialRuntimeState(),
		EDataLayerRuntimeState::Loaded);
	TestFalse(
		TEXT("DataLayer visibility reads back from the bound instance"),
		Owner.DataLayerInstance->IsVisible());

	SubmitWorldProfile(
		*ActorHlodRoute,
		TEXT("request:sa008-world-actor-hlod"),
		TEXT(
			"{\"collection_ref\":"
			"\"uai://hlod/sa008-owned\","
			"\"mode\":\"replace\","
			"\"subject_refs\":["
			"\"uai://actor/sa008-owned\"]}"),
		TEXT(
			"Actor HLOD membership uses the same production lifecycle"));
	TestEqual(
		TEXT("Actor HLOD reads back from the exact bound actor"),
		Owner.Actor->GetHLODLayer(),
		Owner.HlodLayer);

	SubmitWorldProfile(
		*DataLayerRelationRoute,
		TEXT("request:sa008-world-data-layer-relation"),
		TEXT(
			"{\"collection_refs\":["
			"\"uai://data-layer/sa008-owned\"],"
			"\"mode\":\"replace\","
			"\"relation_kind\":\"actor_membership\","
			"\"subject_refs\":["
			"\"uai://actor/sa008-owned\"]}"),
		TEXT(
			"DataLayer membership uses the same production lifecycle"));
	TestTrue(
		TEXT("Actor DataLayer membership reads back independently"),
		Owner.Actor->GetDataLayerInstanceNames().Contains(
			Owner.DataLayerInstance->GetDataLayerFName()));

	SubmitWorldProfile(
		*StreamingRelationStateRoute,
		TEXT("request:sa008-world-streaming-relation-state"),
		FString::Printf(
			TEXT(
				"{\"changes\":{\"loaded\":true,"
				"\"placement\":{"
				"\"basis_evidence\":\"explicit UE world frame\","
				"\"frame_id\":\"ue.world\","
				"\"rotation_quaternion_xyzw\":[0,0,0,1],"
				"\"scale\":[1,1,1],"
				"\"translation\":[10,20,30],"
				"\"unit\":\"centimeter\"},"
				"\"visible\":true},"
				"\"target_ref\":"
				"\"uai://streaming-relation/sa008-owned\"}")),
		TEXT(
			"Streaming relation state uses the same production lifecycle"));
	TestEqual(
		TEXT("Streaming relation loaded state reads back independently"),
		Owner.StreamingLevel->ShouldBeLoaded(),
		true);
	TestEqual(
		TEXT("Streaming relation visible state reads back independently"),
		Owner.StreamingLevel->ShouldBeVisible(),
		true);
	TestTrue(
		TEXT("Streaming relation placement reads back independently"),
		Owner.StreamingLevel->LevelTransform
			.GetLocation()
			.Equals(FVector(10.0, 20.0, 30.0)));

	const FString ActorCreatePayload = TEXT(
			"{\"actor_class_ref\":\"/Script/Engine.StaticMeshActor\"," 
			"\"folder_path\":\"UAI/SA008\"," 
			"\"label\":\"SA008ProductionActor\"," 
			"\"placement\":{\"basis_evidence\":"
			"\"explicit UE world frame\"," 
			"\"frame_id\":\"ue.world\"," 
			"\"rotation_quaternion_xyzw\":[0,0,0,1],"
			"\"scale\":[1,1,1],"
			"\"translation\":[125,-75,50],"
			"\"unit\":\"centimeter\"},"
			"\"tags\":[\"SA008ActorCreate\"]}");
	SubmitWorldProfile(
		*ActorCreateRoute,
		TEXT("request:sa008-world-actor-create"),
		ActorCreatePayload,
		TEXT(
			"Actor create uses stable label identity and independent readback"));
	AActor* const CreatedActor = FindActorWithExactLabel(
		*Owner.World,
		TEXT("SA008ProductionActor"));
	TestNotNull(
		TEXT("Actor create reads back the exact label-bound actor"),
		CreatedActor);
	if (CreatedActor)
	{
		TestEqual(
			TEXT("Actor create reads back the exact class"),
			CreatedActor->GetClass(),
			AStaticMeshActor::StaticClass());
		TestTrue(
			TEXT("Actor create reads back the exact placement"),
			CreatedActor->GetActorLocation().Equals(
				FVector(125.0, -75.0, 50.0)));
		TestTrue(
			TEXT("Actor create reads back the requested tag"),
			CreatedActor->ActorHasTag(TEXT("SA008ActorCreate")));
	}
	SubmitWorldCreateConflict(
		*ActorCreateRoute,
		TEXT("request:sa008-world-actor-create-conflict"),
		ActorCreatePayload,
		TEXT(
			"Actor create rejects an existing label identity before mutation"));

	const FString WallCreatePayload = TEXT(
			"{\"opening\":{\"center\":[0,0],"
			"\"size\":[100,200]},"
			"\"plane\":{\"basis_evidence\":"
			"\"explicit UE world frame\","
			"\"center\":[0,0,100],"
			"\"frame_id\":\"ue.world\","
			"\"normal\":[1,0,0],"
			"\"unit\":\"centimeter\","
			"\"up\":[0,0,1]},"
			"\"segment_template\":{"
			"\"actor_class_ref\":"
			"\"/Script/Engine.StaticMeshActor\","
			"\"folder_path\":\"UAI/SA008\","
			"\"label_prefix\":\"SA008ProductionWall_\","
			"\"mesh_ref\":"
			"\"/Engine/BasicShapes/Cube.Cube\"},"
			"\"wall\":{\"height\":300,"
			"\"thickness\":20,\"width\":400}}");
	SubmitWorldProfile(
		*WallCreateRoute,
		TEXT("request:sa008-world-wall-create"),
		WallCreatePayload,
		TEXT(
			"Wall create uses stable actor identity and independent readback"));
	TestEqual(
		TEXT("Wall create reads back all four exact actors"),
		CountActorsWithLabelPrefix(
			*Owner.World,
			TEXT("SA008ProductionWall_")),
		4);
	SubmitWorldCreateConflict(
		*WallCreateRoute,
		TEXT("request:sa008-world-wall-create-conflict"),
		WallCreatePayload,
		TEXT(
			"Wall create rejects an existing logical actor identity before mutation"));

	const FString PublicDataLayerCreatePayload = TEXT(
			"{\"asset_ref\":"
			"\"uai://data-layer-asset/sa008-create\","
			"\"initial_state\":{"
			"\"initial_runtime_state\":\"activated\","
			"\"initially_visible\":true,"
			"\"loaded_in_editor\":true,"
			"\"visible\":false}}");
	SubmitWorldProfile(
		*DataLayerCreateRoute,
		TEXT("request:sa008-world-data-layer-create"),
		PublicDataLayerCreatePayload,
		TEXT(
			"Public DataLayer create uses asset identity and independent readback"));
	UDataLayerInstance* const CreatedDataLayer =
		const_cast<UDataLayerInstance*>(
			Owner.WorldDataLayers->GetDataLayerInstance(
				Owner.CreateDataLayerAsset));
	TestNotNull(
		TEXT("Public DataLayer create reads back the asset-bound instance"),
		CreatedDataLayer);
	if (CreatedDataLayer)
	{
		TestEqual(
			TEXT("Public DataLayer create reads back initial runtime state"),
			CreatedDataLayer->GetInitialRuntimeState(),
			EDataLayerRuntimeState::Activated);
		TestFalse(
			TEXT("Public DataLayer create reads back visibility"),
			CreatedDataLayer->IsVisible());
	}
	SubmitWorldCreateConflict(
		*DataLayerCreateRoute,
		TEXT("request:sa008-world-data-layer-create-conflict"),
		PublicDataLayerCreatePayload,
		TEXT(
			"Public DataLayer create rejects an existing asset-bound identity before mutation"));

	const FString PrivateDataLayerCreatePayload = TEXT(
			"{\"private\":true,"
			"\"short_name\":\"SA008PrivateLayer\","
			"\"initial_state\":{"
			"\"initially_visible\":false,"
			"\"loaded_in_editor\":true,"
			"\"visible\":true}}");
	SubmitWorldProfile(
		*DataLayerCreateRoute,
		TEXT("request:sa008-world-private-data-layer-create"),
		PrivateDataLayerCreatePayload,
		TEXT(
			"Private DataLayer create uses short-name identity and independent readback"));
	UDataLayerInstance* const CreatedPrivateDataLayer =
		FindPrivateDataLayer(
			*Owner.WorldDataLayers,
			TEXT("SA008PrivateLayer"));
	TestNotNull(
		TEXT("Private DataLayer create reads back the exact private instance"),
		CreatedPrivateDataLayer);
	if (CreatedPrivateDataLayer)
	{
		TestTrue(
			TEXT("Private DataLayer create reads back private asset semantics"),
			CreatedPrivateDataLayer->GetAsset()
				&& CreatedPrivateDataLayer->GetAsset()->IsPrivate());
		TestTrue(
			TEXT("Private DataLayer create reads back visibility"),
			CreatedPrivateDataLayer->IsVisible());
	}
	SubmitWorldCreateConflict(
		*DataLayerCreateRoute,
		TEXT(
			"request:sa008-world-private-data-layer-create-conflict"),
		PrivateDataLayerCreatePayload,
		TEXT(
			"Private DataLayer create rejects an existing short-name identity before mutation"));

	Owner.StreamingLevel->SetShouldBeLoaded(
		bStreamingLoadedBefore);
	Owner.StreamingLevel->SetShouldBeVisible(
		bStreamingVisibleBefore);
	Owner.StreamingLevel->LevelTransform =
		StreamingTransformBefore;
	Owner.World->RemoveStreamingLevel(Owner.StreamingLevel);
	Owner.StreamingLevel = nullptr;
	const FVector CreatedStreamingLocation(100.0, 200.0, 0.0);
	const FString StreamingCreatePayload = TEXT(
			"{\"level_asset_ref\":"
			"\"uai://streaming-asset/sa008-create\","
			"\"placement\":{\"basis_evidence\":"
			"\"explicit UE world frame\","
			"\"frame_id\":\"ue.world\","
			"\"rotation_quaternion_xyzw\":[0,0,0,1],"
			"\"scale\":[1,1,1],"
			"\"translation\":[100,200,0],"
			"\"unit\":\"centimeter\"},"
			"\"streaming_class_ref\":"
			"\"/Script/Engine.LevelStreamingDynamic\"}");
	SubmitWorldProfile(
		*StreamingRelationCreateRoute,
		TEXT("request:sa008-world-streaming-relation-create"),
		StreamingCreatePayload,
		TEXT(
			"Streaming relation create uses stable relation identity and independent readback"));
	ULevelStreaming* const CreatedStreamingRelation =
		FindStreamingRelation(
			*Owner.World,
			Owner.StreamingAsset->GetOutermost()->GetName(),
			CreatedStreamingLocation);
	TestNotNull(
		TEXT("Streaming relation create reads back the exact relation"),
		CreatedStreamingRelation);
	if (CreatedStreamingRelation)
	{
		TestEqual(
			TEXT("Streaming relation create reads back the exact class"),
			CreatedStreamingRelation->GetClass(),
			ULevelStreamingDynamic::StaticClass());
	}
	SubmitWorldCreateConflict(
		*StreamingRelationCreateRoute,
		TEXT(
			"request:sa008-world-streaming-relation-create-conflict"),
		StreamingCreatePayload,
		TEXT(
			"Streaming relation create rejects an existing relation identity before mutation"));
	TestEqual(
		TEXT("all World create and update operations prepare through one C8D port"),
		Durable->PrepareCount,
		11);
	TestEqual(
		TEXT("all World create and update operations persist through one C8D port"),
		Durable->PersistCount,
		11);
	TestEqual(
		TEXT("all World create and update operations acknowledge only after C9"),
		Durable->AcknowledgeCount,
		11);

	if (bActorInDataLayerBefore)
	{
		Owner.Actor->AddDataLayer(
			Owner.DataLayerInstance);
	}
	else
	{
		Owner.Actor->RemoveDataLayer(
			Owner.DataLayerInstance);
	}
	Owner.Actor->SetHLODLayer(ActorHlodBefore);
	Owner.DataLayerInstance->SetInitialRuntimeState(
		DataLayerRuntimeStateBefore);
	Owner.DataLayerInstance->SetVisible(
		bDataLayerVisibleBefore);
	Owner.Actor->SetRuntimeGrid(ActorGridBefore);
	Owner.Actor->SetIsSpatiallyLoaded(bActorSpatialBefore);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUeAgentSimpleAuthoringWorldOwnedPackageTest,
	"UeAgentInterface.Composition.SimpleAuthoring.E4.WorldOwnedPackage",
	EAutomationTestFlags::EditorContext
		| EAutomationTestFlags::EngineFilter)

bool FUeAgentSimpleAuthoringWorldOwnedPackageTest::RunTest(
	const FString& Parameters)
{
	static_assert(UAI_SIMPLE_AUTHORING_DURABLE_SERVICE_V1 == 1);

	FString OwnedRoot;
	{
		FOwnedDurableWorld Owner;
		OwnedRoot = Owner.Root;
		TestTrue(
			TEXT("owned durable World is valid"),
			Owner.IsValid());
		if (!Owner.IsValid())
		{
			return false;
		}
		const bool bBefore =
			Owner.WorldPartition->IsStreamingEnabledInEditor();

		FUeAgentSimpleAuthoringDurableServiceConfig EmptyConfig;
		TSharedPtr<IUeAgentSimpleAuthoringAssetDurableService>
			EmptyService;
		FUeAgentSimpleAuthoringProductionError DurableError;
		TestFalse(
			TEXT("durable service rejects a missing journal root"),
			FUeAgentSimpleAuthoringDurableServiceFactory::TryCreate(
				EmptyConfig,
				EmptyService,
				DurableError));
		TestEqual(
			TEXT("missing journal root has a stable semantic"),
			DurableError.Code,
			FString(
				TEXT(
					"simple_authoring_durable_journal_root_missing")));

		FUeAgentSimpleAuthoringDurableServiceConfig DurableConfig;
		DurableConfig.JournalRoot = Owner.JournalRoot;
		DurableConfig.StableJournalOwnerId = TEXT("test:world:durable");
		DurableConfig.PackageBackend =
			MakeShared<
				FPackageAssetDurableBackend,
				ESPMode::ThreadSafe>(Owner.PackageRoot);
		TSharedPtr<IUeAgentSimpleAuthoringAssetDurableService>
			Durable;
		DurableError =
			FUeAgentSimpleAuthoringProductionError();
		TestTrue(
			TEXT("real C8D production service composes"),
			FUeAgentSimpleAuthoringDurableServiceFactory::TryCreate(
				DurableConfig,
				Durable,
				DurableError));
		if (!Durable.IsValid())
		{
			AddError(
				TEXT("sa008_world_e4_durable_invalid:")
				+ DurableError.Code);
			return false;
		}

		TSharedPtr<const FUeAgentDescriptorRegistry> Registry;
		FString Error;
		TestTrue(
			TEXT("Simple Authoring descriptors compose"),
			TryCreateSimpleAuthoringDescriptorRegistry(
				Registry,
				Error));
		const FUeAgentSimpleAuthoringCommandRoute* Route =
			FindRoute();
		TestNotNull(
			TEXT("World Partition streaming has one exact route"),
			Route);
		if (!Registry.IsValid() || !Route)
		{
			AddError(
				TEXT("sa008_world_e4_route_missing:")
				+ Error);
			return false;
		}

		const TSharedRef<
			FOwnedDurableWorldCatalog,
			ESPMode::ThreadSafe> SourceCatalog =
				MakeShared<
					FOwnedDurableWorldCatalog,
					ESPMode::ThreadSafe>(Owner);
		const TSharedRef<
			FReloadedDurableWorldCatalog,
			ESPMode::ThreadSafe> ReloadCatalog =
				MakeShared<
					FReloadedDurableWorldCatalog,
					ESPMode::ThreadSafe>(Owner);
		const TSharedRef<
			FRejectingReferenceResolver,
			ESPMode::ThreadSafe> ReferenceResolver =
				MakeShared<
					FRejectingReferenceResolver,
					ESPMode::ThreadSafe>();

		FUeAgentWorldSimpleAuthoringProviderConfig ProviderConfig;
		ProviderConfig.DescriptorRegistry = Registry;
		ProviderConfig.WorldCatalog = SourceCatalog;
		ProviderConfig.PersistedWorldCatalog = ReloadCatalog;
		ProviderConfig.ReferenceResolver = ReferenceResolver;
		ProviderConfig.DurableService = Durable;
		ProviderConfig.AbsenceOracle =
			MakeShared<FUnusedAbsenceOracle>();
		ProviderConfig.ReferenceOracle =
			MakeShared<FUnusedReferenceOracle>();
		ProviderConfig.OwnerLeaseId =
			TEXT("owner:sa008-world-e4");
		ProviderConfig.Projection.Emplace(
			FUeAgentProjectionProfile(
				TEXT("compact"),
				false,
				false,
				false),
			FUeAgentPageRequest(),
			TOptional<int64>(),
			TArray<FUeAgentArtifactDescriptor>(),
			TArray<FUeAgentProjectionDiagnostic>());

		TSharedPtr<IUeAgentSimpleAuthoringCapabilityProvider>
			Provider;
		FUeAgentWorldSimpleAuthoringError ProviderError;
		TestTrue(
			TEXT("World E4 family composes"),
			FUeAgentWorldSimpleAuthoringProviderFactory::TryCreate(
				ProviderConfig,
				Provider,
				ProviderError));
		if (!Provider.IsValid())
		{
			AddError(
				TEXT("sa008_world_e4_provider_invalid:")
				+ ProviderError.Code);
			return false;
		}

		TSharedPtr<const FUeAgentOperationTicketService>
			TicketService;
		FUeAgentOperationTicketError TicketError;
		TestTrue(
			TEXT("E4 ticket service creates"),
			FUeAgentOperationTicketService::TryCreate(
				TEXT("sa008-world-e4-integrity"),
				30.0,
				[]()
				{
					return 100.0;
				},
				[]()
				{
					return FString(
						TEXT("ticket:sa008-world-e4"));
				},
				TicketService,
				TicketError));
		TSharedPtr<const FUeAgentResponseProjector> Projector;
		FUeAgentProjectionError ProjectionError;
		TestTrue(
			TEXT("E4 response projector creates"),
			FUeAgentResponseProjector::TryCreate(
				MakeShared<FTokenEstimator>(),
				Projector,
				ProjectionError));
		if (!TicketService.IsValid() || !Projector.IsValid())
		{
			return false;
		}

		FUeAgentSimpleAuthoringApplicationConfig
			ApplicationConfig;
		ApplicationConfig.DescriptorRegistry = Registry;
		ApplicationConfig.ExecutionContext.ProjectInstanceId =
			TEXT("project:sa008-world-e4");
		ApplicationConfig.ExecutionContext.EditorInstanceId =
			TEXT("editor:sa008-world-e4");
		ApplicationConfig.ExecutionContext.StatePlane =
			EUeAgentStatePlane::AuthoringMemory;
		ApplicationConfig.ExecutionContext.WorldType =
			EUeAgentExecutionWorldType::Preview;
		ApplicationConfig.ExecutionContext.WorldIdentity =
			Owner.WorldIdentity;
		ApplicationConfig.ExecutionContext.LevelIdentity =
			Owner.LevelIdentity;
		ApplicationConfig.ExecutionContext
				.WorldPartitionIdentity =
			Owner.WorldPartition->GetPathName();
		ApplicationConfig.ExecutionContext.ContextRevision =
			Owner.ContextRevision;
		ApplicationConfig.ExecutionContext
			.bResolvedFromCurrent = false;
		ApplicationConfig.ServerSessionId =
			TEXT("session:sa008-world-e4");
		ApplicationConfig.RegistryEpoch = 1;
		ApplicationConfig.TimeoutSeconds = 30.0;
		ApplicationConfig.MaxFrameBytes = 1024 * 1024;

		TSharedPtr<FUeAgentSimpleAuthoringApplication>
			Application;
		FUeAgentSimpleAuthoringApplicationError
			ApplicationError;
		TestTrue(
			TEXT("application accepts World E4 route"),
			FUeAgentSimpleAuthoringApplication::TryCreate(
				ApplicationConfig,
				TArray<FUeAgentSimpleAuthoringCommandRoute>{
					*Route,
				},
				{Provider.ToSharedRef()},
				TicketService.ToSharedRef(),
				Projector.ToSharedRef(),
				Application,
				ApplicationError));
		if (!Application.IsValid())
		{
			AddError(
				TEXT("sa008_world_e4_application_invalid:")
				+ ApplicationError.Code);
			return false;
		}

		const FUeAgentOperationResult Result =
			Application->Submit(
				FUeAgentOperationRequest(
					Route->DescriptorIdentity,
					TEXT("request:sa008-world-e4"),
					TEXT("idempotency:sa008-world-e4"),
					FUeAgentContractValue(
						bBefore
							? TEXT("{\"enabled\":false}")
							: TEXT("{\"enabled\":true}"))),
				FUeAgentCallerContext(
					TEXT("caller:sa008-world-e4"),
					{TEXT("uai:world:write")},
					TEXT("correlation:sa008-world-e4")),
				MakeShared<FNeverCancelled>());
		const FString ResultJson =
			Result.GetValue().GetJsonValue();
		const bool bCompleted =
			ResultJson.Contains(
				TEXT("\"effect_outcome\":\"complete\""))
			&& ResultJson.Contains(
				TEXT("\"status\":\"verified\""));
		TestTrue(
			TEXT(
				"World E4 completes only after reloaded readback"),
			bCompleted);
		if (!bCompleted)
		{
			AddError(
				TEXT("sa008_world_e4_result:")
				+ ResultJson);
		}

		TestTrue(
			TEXT("C9 uses a reloaded World owner"),
			Owner.WasReloaded());
		TestTrue(
			TEXT("original World UObject is collected before readback"),
			Owner.WasOriginalWorldCollected());
		TestTrue(
			TEXT("published map package exists"),
			IFileManager::Get().FileExists(
				*Owner.HeaderPath()));
		TestEqual(
			TEXT(
				"reloaded WorldPartition preserves the mutation"),
			Owner.WorldPartition
				->IsStreamingEnabledInEditor(),
			!bBefore);
		TestEqual(
			TEXT(
				"source catalog owns target/pre-state/mutation only"),
			SourceCatalog->EnumerateCount,
			3);
		TestEqual(
			TEXT("C9 uses the dedicated reload catalog once"),
			ReloadCatalog->EnumerateCount,
			1);
		TestEqual(
			TEXT(
				"no-ref profile still bypasses resolver"),
			ReferenceResolver->ResolveCount,
			0);

		TArray<FString> CompletedJournals;
		IFileManager::Get().FindFiles(
			CompletedJournals,
			*(Owner.JournalRoot
				/ TEXT("completed/*.json")),
			true,
			false);
		TArray<FString> ActiveTransactions;
		IFileManager::Get().FindFiles(
			ActiveTransactions,
			*(Owner.JournalRoot / TEXT("active/*")),
			false,
			true);
		TestEqual(
			TEXT("verified C8D journal is archived once"),
			CompletedJournals.Num(),
			1);
		TestEqual(
			TEXT("verified C8D has no active transaction"),
			ActiveTransactions.Num(),
			0);
	}

	TestFalse(
		TEXT("World E4 removes all owned disk artifacts"),
		IFileManager::Get().DirectoryExists(*OwnedRoot));
	return true;
}
