#include "DomainAdapters/WorldDestructive/UeAgentWorldDestructiveOwner.h"

#include "Components/InstancedStaticMeshComponent.h"
#include "Components/StaticMeshComponent.h"
#include "Core/AssetDurable/UeAgentAssetDurable.h"
#include "Core/AssetDurable/UeAgentPackageHeader.h"
#include "Core/ContextResource/UeAgentContextResource.h"
#include "Core/Contract/UeAgentContractJson.h"
#include "Core/PlanningPreflight/UeAgentPlanningPreflight.h"
#include "Core/StateSemantic/UeAgentStateSemantic.h"
#include "Domains/Level/Authoring/UeAgentLevelAuthoringState.h"
#include "EditorLevelUtils.h"
#include "Engine/LevelStreaming.h"
#include "Engine/StaticMesh.h"
#include "Engine/StaticMeshActor.h"
#include "Engine/World.h"
#include "EngineUtils.h"
#include "GameFramework/Actor.h"
#include "HAL/FileManager.h"
#include "HAL/ThreadSafeCounter.h"
#include "Misc/PackageName.h"
#include "PCGComponent.h"
#include "ScopedTransaction.h"
#include "UObject/GarbageCollection.h"
#include "UObject/Package.h"
#include "UObject/SavePackage.h"
#include "PackageTools.h"
#include "WorldPartition/DataLayer/DataLayerInstance.h"
#include "WorldPartition/DataLayer/WorldDataLayers.h"

namespace UE::UAI::DomainAdapters::WorldDestructive
{
	using namespace AssetDurable;
	using namespace ContextResource;
	using namespace Contract;
	using namespace PlanningPreflight;
	using namespace StateSemantic;

	namespace
	{
		FUeAgentResourceTracker MakeTracker()
		{
			return FUeAgentResourceTracker(FUeAgentResourceBudget({
				{EUeAgentResourceDimension::InputBytes, 1024 * 1024},
				{EUeAgentResourceDimension::OutputBytes, 1024 * 1024},
				{EUeAgentResourceDimension::StringBytes, 1024 * 1024},
				{EUeAgentResourceDimension::Nodes, 8192},
				{EUeAgentResourceDimension::Items, 8192},
				{EUeAgentResourceDimension::Files, 512},
				{EUeAgentResourceDimension::ReferenceEdges, 8192},
				{EUeAgentResourceDimension::WorkUnits, 131072},
				{EUeAgentResourceDimension::EstimatedMemoryBytes, 32 * 1024 * 1024},
			}));
		}

		bool TryPackageName(FString Ref, FString& OutPackageName)
		{
			Ref.TrimStartAndEndInline();
			OutPackageName = Ref.Contains(TEXT("."))
				? FPackageName::ObjectPathToPackageName(Ref)
				: MoveTemp(Ref);
			return OutPackageName.StartsWith(TEXT("/Game/"))
				&& FPackageName::IsValidLongPackageName(OutPackageName, true);
		}

		FString ObjectPath(const FString& PackageName)
		{
			return PackageName + TEXT(".")
				+ FPackageName::GetLongPackageAssetName(PackageName);
		}

		UObject* ReloadForIndependentReadback(const FString& PackageName)
		{
			if (UPackage* const Package = FindPackage(nullptr, *PackageName))
			{
				Package->SetDirtyFlag(false);
				FText Error;
				if (!UPackageTools::UnloadPackages({Package}, Error, true))
				{
					return nullptr;
				}
			}
			CollectGarbage(RF_NoFlags, true);
			return LoadObject<UObject>(nullptr, *ObjectPath(PackageName));
		}

		FUeAgentCanonicalIdentity IdentityFor(const FString& PackageName)
		{
			return FUeAgentCanonicalIdentity(
				TEXT("uai.world-destructive.asset.state"),
				TEXT("asset"), PackageName, TEXT("state"), TEXT("asset"),
				TEXT("world-destructive-v1"));
		}

		FPlanningPreflightPlanRef MakePlan(
			const FUeAgentWorldDestructiveRequest& Request,
			const FString& PackageName,
			const FString& OwnerId,
			const FUeAgentCanonicalIdentity& Identity)
		{
			TArray<FPlannedChange> Changes;
			Changes.Emplace(
				Identity, EChangeIntent::Update, FUeAgentCanonicalValuePtr(),
				FUeAgentCanonicalValuePtr(),
				TArray<FString>{TEXT("uai:asset:write")},
				TEXT("world-destructive:direct"));
			TArray<FImpact> Impacts;
			Impacts.Emplace(
				Identity, Identity, TEXT("direct"),
				TEXT("world-destructive:direct"));
			return MakeShared<FPlanningPreflightPlan>(
				FUeAgentDescriptorIdentity(
					TEXT("uai.world.destructive/v1"), 1,
					Json::Sha256Utf8(TEXT("uai.world.destructive/v1"))),
				Request.RequestId,
				TEXT("world-destructive:") + Request.RequestId,
				Request.CallerId,
				TArray<FString>{TEXT("uai:asset:write")},
				TEXT("world-destructive:preflight:") + Request.RequestId,
				FUeAgentStateRevision(OwnerId, TEXT("world-destructive-v1"), 1),
				TEXT("world-destructive:preflight:native"),
				FChangeSet(MoveTemp(Changes)), FImpactSet(MoveTemp(Impacts)),
				TArray<FPrecondition>(),
				Json::Sha256Utf8(
					Request.CommandId + TEXT(":") + Request.RequestId
					+ TEXT(":") + PackageName));
		}

		class FAssetDurableMutation final
		{
		public:
			bool Prepare(
				const FUeAgentWorldDestructiveRequest& Request,
				const FString& PackageName,
				const FString& OwnerId,
				const FString& JournalRoot,
				const int64 FencingToken,
				FString& OutError)
			{
				Identity = IdentityFor(PackageName);
				Token = FencingToken;
				Plan = MakePlan(Request, PackageName, OwnerId, Identity);
				const FAssetDurableRequest DurableRequest(
					Plan.ToSharedRef(),
					{FAssetDurableTarget(
						Identity, PackageName, EAssetDurableIntent::Save,
						ERollbackGrade::DurableBackup)},
					Token, OwnerId, EAtomicityMode::CompensatedSaga);
				FAssetDurableError Error;
				if (!TryPrepareAssetDurability(
						DurableRequest, Backend, JournalRoot, Resources, Session, Error)
					|| !Session.IsValid())
				{
					OutError = Error.GetCode() + TEXT(":") + Error.GetDetail();
					return false;
				}
				return true;
			}

			bool Persist(FString& OutError)
			{
				FAssetDurableError Error;
				if (!TryPersistAssetDurability(
						Session.ToSharedRef(), Backend, Token, Resources, Receipt, Error))
				{
					OutError = Error.GetCode() + TEXT(":") + Error.GetDetail();
					return false;
				}
				return true;
			}

			bool Acknowledge(const FString& VerificationId, FString& OutError)
			{
				const FAssetDurableSessionRef Persisted =
					MakeShared<const FAssetDurableSession>(
						Session->GetRequest(), Session->GetInventory(),
						Session->GetJournalId(), Session->GetJournalPath(),
						Session->GetJournalRoot(), Session->GetTransactionRoot(),
						Session->GetStagingRoot(), Receipt.GetPhaseHistory());
				const FDurableVerificationAcknowledgement Ack(
					Receipt.GetJournalId(), Receipt.GetPlanFingerprint(),
					Receipt.GetInventoryHash(), VerificationId,
					Session->GetRequest().GetJournalOwnerId(), Token);
				FAssetDurableError Error;
				if (!TryAcknowledgeAssetDurability(
						Persisted, Ack, Token, Resources, Error))
				{
					OutError = Error.GetCode() + TEXT(":") + Error.GetDetail();
					return false;
				}
				return true;
			}

			void Rollback(const bool bMemoryConfirmed)
			{
				if (!Session.IsValid()) return;
				FDurableReceipt RollbackReceipt;
				FAssetDurableError Error;
				TryRollbackAssetDurability(
					Session.ToSharedRef(), Backend, Token, bMemoryConfirmed,
					Resources, RollbackReceipt, Error);
			}

			FString JournalId() const
			{
				return Session.IsValid() ? Session->GetJournalId() : FString();
			}

		private:
			FUeAgentResourceTracker Resources = MakeTracker();
			FPackageAssetDurableBackend Backend;
			FUeAgentCanonicalIdentity Identity = IdentityFor(TEXT("/Game/Invalid"));
			FPlanningPreflightPlanPtr Plan;
			FAssetDurableSessionPtr Session;
			FDurableReceipt Receipt;
			int64 Token = -1;
		};

		AActor* FindActor(UWorld& World, const FString& ActorRef)
		{
			for (TActorIterator<AActor> It(&World); It; ++It)
			{
				if (It->GetPathName() == ActorRef)
				{
					return *It;
				}
			}
			return nullptr;
		}

		UActorComponent* FindComponent(AActor& Actor, const FString& Name)
		{
			TArray<UActorComponent*> Components;
			Actor.GetComponents(Components);
			for (UActorComponent* const Component : Components)
			{
				if (Component && Component->GetName() == Name)
				{
					return Component;
				}
			}
			return nullptr;
		}

		ULevelStreaming* FindStreamingLevel(
			UWorld& World, FString PackageName)
		{
			PackageName = FPackageName::ObjectPathToPackageName(PackageName);
			for (ULevelStreaming* const Streaming : World.GetStreamingLevels())
			{
				if (Streaming
					&& FPackageName::ObjectPathToPackageName(
						Streaming->GetWorldAssetPackageName()) == PackageName)
				{
					return Streaming;
				}
			}
			return nullptr;
		}

		bool PersistPackages(
			const TArray<UPackage*>& Packages,
			bool& bOutPersisted,
			FString& OutError)
		{
			bOutPersisted = false;
			for (UPackage* const Package : Packages)
			{
				if (!Package)
				{
					continue;
				}
				const FString PackageName = Package->GetName();
				if (!PackageName.StartsWith(TEXT("/Game/")))
				{
					continue;
				}
				UObject* TopLevelAsset = Package->FindAssetInPackage();
				FString Filename;
				FString HeaderErrorCode;
				if (!TryPrepareLoadedPackageHeaderForSave(
						*Package,
						TopLevelAsset,
						Filename,
						HeaderErrorCode))
				{
					OutError = (HeaderErrorCode.IsEmpty()
						? TEXT("world_destructive_package_header_unavailable")
						: HeaderErrorCode)
						+ TEXT(":") + PackageName;
					return false;
				}
				FSavePackageArgs Args;
				Args.TopLevelFlags = RF_Public | RF_Standalone;
				if (!UPackage::SavePackage(
						Package,
						TopLevelAsset,
						*Filename,
						Args))
				{
					OutError = TEXT("world_destructive_package_save_failed:")
						+ PackageName;
					return false;
				}
				FString ObservedFilename;
				HeaderErrorCode.Reset();
				if (!TryResolvePackageHeaderFilename(
						PackageName,
						ObservedFilename,
						HeaderErrorCode)
					|| !ObservedFilename.Equals(
						Filename,
						ESearchCase::IgnoreCase)
					|| !IFileManager::Get().FileExists(*ObservedFilename))
				{
					OutError = (HeaderErrorCode.IsEmpty()
						? TEXT(
							"world_destructive_package_save_readback_failed")
						: HeaderErrorCode)
						+ TEXT(":") + PackageName;
					return false;
				}
				Package->SetDirtyFlag(false);
				bOutPersisted = true;
			}
			return true;
		}

		class FWorldDestructiveOwner final : public IUeAgentWorldDestructiveOwner
		{
		public:
			FWorldDestructiveOwner(
				UWorld& InWorld, FString InJournalRoot, FString InOwnerId)
				: World(&InWorld)
				, JournalRoot(MoveTemp(InJournalRoot))
				, OwnerId(MoveTemp(InOwnerId))
			{
			}

			FUeAgentWorldDestructiveResult Execute(
				const FUeAgentWorldDestructiveRequest& Request,
				const TSharedRef<IUeAgentCancellationSignal>& Cancellation) override
			{
				FUeAgentWorldDestructiveResult Result;
				Result.CommandId = Request.CommandId;
				Result.TargetRef = Request.TargetRef;
				auto Fail = [&Result](const FString& Code, const FString& Detail = FString())
				{
					Result.ErrorCode = Code;
					Result.ErrorDetail = Detail;
					return Result;
				};
				if (!IsInGameThread())
				{
					return Fail(TEXT("world_destructive_game_thread_required"));
				}
				if (!World.IsValid())
				{
					return Fail(TEXT("world_destructive_world_unavailable"));
				}
				if (Cancellation->IsCancelled())
				{
					return Fail(TEXT("world_destructive_cancelled"), Request.RequestId);
				}
				if (Request.CommandId == TEXT("world.data_layer.delete"))
					return DeleteDataLayer(Request);
				if (Request.CommandId == TEXT("world.level_content.delete_scope"))
					return DeleteFolder(Request);
				if (Request.CommandId == TEXT("world.pcg.cleanup"))
					return CleanupPcg(Request);
				if (Request.CommandId == TEXT("world.level_streaming.move_actors"))
					return MoveActor(Request);
				if (Request.CommandId == TEXT("world.level_streaming.remove"))
					return RemoveStreamingLevel(Request);
				if (Request.CommandId == TEXT("modeling.actor.mesh.replace"))
					return ReplaceActorMesh(Request);
				if (Request.CommandId == TEXT("modeling.asset.material_slot.remove"))
					return RemoveMaterialSlot(Request);
				if (Request.CommandId == TEXT("modeling.voxel.bounds.remove"))
					return RemoveVoxelBounds(Request);
				return Fail(TEXT("world_destructive_command_unknown"), Request.CommandId);
			}

		private:
			FUeAgentWorldDestructiveResult Base(
				const FUeAgentWorldDestructiveRequest& Request) const
			{
				FUeAgentWorldDestructiveResult Result;
				Result.CommandId = Request.CommandId;
				Result.TargetRef = Request.TargetRef;
				Result.ResultRef = Request.OperandRef;
				return Result;
			}

			FUeAgentWorldDestructiveResult CommitWorld(
				const FUeAgentWorldDestructiveRequest& Request,
				const TArray<UPackage*>& Packages,
				const bool bChanged,
				const bool bReadback)
			{
				FUeAgentWorldDestructiveResult Result = Base(Request);
				Result.bChanged = bChanged;
				Result.bReadbackVerified = bReadback;
				Result.VerificationId = TEXT("world-destructive-readback:")
					+ Request.RequestId;
				FString Error;
				if (!bReadback)
				{
					Result.TerminalState = TEXT("state_unknown");
					Result.ErrorCode = TEXT("world_destructive_readback_failed");
					return Result;
				}
				if (!PersistPackages(Packages, Result.bPersisted, Error))
				{
					Result.TerminalState = TEXT("state_unknown");
					Result.ErrorCode = TEXT("world_destructive_persist_failed");
					Result.ErrorDetail = Error;
					return Result;
				}
				Result.PersistenceScope = Result.bPersisted
					? TEXT("package") : TEXT("editor_transaction");
				Result.TerminalState = TEXT("committed");
				return Result;
			}

			FUeAgentWorldDestructiveResult DeleteDataLayer(
				const FUeAgentWorldDestructiveRequest& Request)
			{
				FUeAgentWorldDestructiveResult Result = Base(Request);
				AWorldDataLayers* const Layers = World->GetWorldDataLayers();
				const UDataLayerInstance* const Layer = Layers
					? Layers->GetDataLayerInstance(FName(*Request.TargetRef)) : nullptr;
				if (!Layers || !Layer)
				{
					Result.ErrorCode = TEXT("world_data_layer_missing");
					return Result;
				}
				const FName LayerName = Layer->GetDataLayerFName();
				FScopedTransaction Transaction(
					NSLOCTEXT("UeAgentInterface", "WorldDeleteDataLayer",
						"UeAgentInterface Delete Data Layer"));
				Layers->Modify();
				if (!Layers->RemoveDataLayer(Layer, true))
				{
					Transaction.Cancel();
					Result.ErrorCode = TEXT("world_data_layer_delete_failed");
					return Result;
				}
				return CommitWorld(
					Request, {Layers->GetOutermost()}, true,
					Layers->GetDataLayerInstance(LayerName) == nullptr);
			}

			FUeAgentWorldDestructiveResult DeleteFolder(
				const FUeAgentWorldDestructiveRequest& Request)
			{
				FUeAgentWorldDestructiveResult Result = Base(Request);
				const FString Root = Request.TargetRef.TrimStartAndEnd();
				const FString ManagedId = Request.ManagedId.TrimStartAndEnd();
				if (Root.IsEmpty())
				{
					Result.ErrorCode = TEXT("world_folder_path_required");
					return Result;
				}
				TArray<AActor*> Actors;
				if (!ManagedId.IsEmpty())
				{
					AActor* Actor = nullptr;
					FString ResolveError;
					if (!UeAgentLevelAuthoring::TryFindManagedActorExact(
							*World,
							ManagedId,
							Root,
							Actor,
							ResolveError)
						|| !Actor)
					{
						Result.ErrorCode =
							TEXT("world_exact_managed_actor_unavailable");
						Result.ErrorDetail = ResolveError;
						return Result;
					}
					Result.ResultRef = Actor->GetPathName();
					Actors.Add(Actor);
				}
				else
				{
					for (TActorIterator<AActor> It(World.Get()); It; ++It)
					{
						const FString Folder = It->GetFolderPath().ToString();
						if (Folder == Root || Folder.StartsWith(Root + TEXT("/")))
						{
							Actors.Add(*It);
						}
					}
				}
				if (Actors.IsEmpty())
				{
					Result.ErrorCode = TEXT("world_folder_scope_empty");
					return Result;
				}
				FScopedTransaction Transaction(
					NSLOCTEXT("UeAgentInterface", "WorldDeleteFolder",
						"UeAgentInterface Delete Folder Content"));
				for (AActor* const Actor : Actors)
				{
					if (!World->DestroyActor(Actor, true, true))
					{
						Result.TerminalState = TEXT("state_unknown");
						Result.ErrorCode = TEXT("world_folder_delete_failed");
						return Result;
					}
				}
				bool bAbsent = true;
				for (TActorIterator<AActor> It(World.Get()); It; ++It)
				{
					if (!ManagedId.IsEmpty())
					{
						bAbsent &= !UeAgentLevelAuthoring::GetManagedActorId(**It)
							.Equals(ManagedId, ESearchCase::CaseSensitive);
					}
					else
					{
						const FString Folder = It->GetFolderPath().ToString();
						bAbsent &= Folder != Root
							&& !Folder.StartsWith(Root + TEXT("/"));
					}
				}
				return CommitWorld(
					Request, {World->GetOutermost()}, true, bAbsent);
			}

			FUeAgentWorldDestructiveResult CleanupPcg(
				const FUeAgentWorldDestructiveRequest& Request)
			{
				FUeAgentWorldDestructiveResult Result = Base(Request);
				AActor* const Actor = FindActor(*World, Request.TargetRef);
				UPCGComponent* const Component = Actor
					? Cast<UPCGComponent>(FindComponent(*Actor, Request.ComponentName))
					: nullptr;
				if (!Component)
				{
					Result.ErrorCode = TEXT("world_pcg_component_missing");
					return Result;
				}
				FScopedTransaction Transaction(
					NSLOCTEXT("UeAgentInterface", "WorldPcgCleanup",
						"UeAgentInterface PCG Cleanup"));
				Component->Modify();
				Component->CleanupLocalImmediate(true, true);
				return CommitWorld(
					Request, {Actor->GetOutermost()}, true,
					!Component->IsGenerating());
			}

			FUeAgentWorldDestructiveResult MoveActor(
				const FUeAgentWorldDestructiveRequest& Request)
			{
				FUeAgentWorldDestructiveResult Result = Base(Request);
				AActor* const Actor = FindActor(*World, Request.TargetRef);
				ULevelStreaming* const Streaming =
					FindStreamingLevel(*World, Request.OperandRef);
				ULevel* const Level = Streaming ? Streaming->GetLoadedLevel() : nullptr;
				if (!Actor || !Level)
				{
					Result.ErrorCode = Actor
						? TEXT("world_streaming_level_missing")
						: TEXT("world_actor_missing");
					return Result;
				}
				const FGuid StableActorGuid = Actor->GetActorGuid();
				const FString StableActorLabel = Actor->GetActorLabel();
				FScopedTransaction Transaction(
					NSLOCTEXT("UeAgentInterface", "WorldMoveActor",
						"UeAgentInterface Move Actor To Level"));
				TArray<AActor*> Moved;
				const int32 Count = UEditorLevelUtils::MoveActorsToLevel(
					{Actor}, Level, false, false, true, &Moved);
				if (Count != 1)
				{
					Transaction.Cancel();
					Result.ErrorCode = TEXT("world_move_actor_failed");
					return Result;
				}
				AActor* ReadbackActor = nullptr;
				if (StableActorGuid.IsValid())
				{
					for (AActor* const Candidate : Level->Actors)
					{
						if (Candidate
							&& Candidate->GetActorGuid() == StableActorGuid)
						{
							ReadbackActor = Candidate;
							break;
						}
					}
				}
				if (!ReadbackActor && !StableActorLabel.IsEmpty())
				{
					for (AActor* const Candidate : Level->Actors)
					{
						if (Candidate
							&& Candidate->GetActorLabel() == StableActorLabel)
						{
							if (ReadbackActor)
							{
								ReadbackActor = nullptr;
								break;
							}
							ReadbackActor = Candidate;
						}
					}
				}
				FUeAgentWorldDestructiveResult Committed = CommitWorld(
					Request,
					{World->GetOutermost(), Level->GetOutermost()}, true,
					ReadbackActor && ReadbackActor->GetLevel() == Level);
				Committed.ResultRef = ReadbackActor
					? ReadbackActor->GetPathName() : FString();
				return Committed;
			}

			FUeAgentWorldDestructiveResult RemoveStreamingLevel(
				const FUeAgentWorldDestructiveRequest& Request)
			{
				FUeAgentWorldDestructiveResult Result = Base(Request);
				ULevelStreaming* const Streaming =
					FindStreamingLevel(*World, Request.TargetRef);
				ULevel* const Level = Streaming ? Streaming->GetLoadedLevel() : nullptr;
				if (!Level)
				{
					Result.ErrorCode = TEXT("world_streaming_level_missing");
					return Result;
				}
				const FString PackageName = Streaming->GetWorldAssetPackageName();
				FScopedTransaction Transaction(
					NSLOCTEXT("UeAgentInterface", "WorldRemoveStreaming",
						"UeAgentInterface Remove Streaming Level"));
				if (!UEditorLevelUtils::RemoveLevelFromWorld(Level, false, false))
				{
					Transaction.Cancel();
					Result.ErrorCode = TEXT("world_streaming_remove_failed");
					return Result;
				}
				return CommitWorld(
					Request, {World->GetOutermost()}, true,
					FindStreamingLevel(*World, PackageName) == nullptr);
			}

			FUeAgentWorldDestructiveResult ReplaceActorMesh(
				const FUeAgentWorldDestructiveRequest& Request)
			{
				FUeAgentWorldDestructiveResult Result = Base(Request);
				AActor* const Actor = FindActor(*World, Request.TargetRef);
				UStaticMesh* const Mesh = LoadObject<UStaticMesh>(
					nullptr, *Request.OperandRef);
				UStaticMeshComponent* const Component = Actor
					? Actor->FindComponentByClass<UStaticMeshComponent>() : nullptr;
				if (!Component || !Mesh)
				{
					Result.ErrorCode = !Actor
						? TEXT("modeling_actor_missing")
						: (!Component
							? TEXT("modeling_static_mesh_component_missing")
							: TEXT("modeling_static_mesh_missing"));
					return Result;
				}
				FScopedTransaction Transaction(
					NSLOCTEXT("UeAgentInterface", "ModelingReplaceMesh",
						"UeAgentInterface Replace Actor Mesh"));
				Component->Modify();
				Component->SetStaticMesh(Mesh);
				Component->MarkRenderStateDirty();
				Actor->MarkPackageDirty();
				return CommitWorld(
					Request, {Actor->GetOutermost()}, true,
					Component->GetStaticMesh() == Mesh);
			}

			FUeAgentWorldDestructiveResult RemoveMaterialSlot(
				const FUeAgentWorldDestructiveRequest& Request)
			{
				FUeAgentWorldDestructiveResult Result = Base(Request);
				FString PackageName;
				if (!TryPackageName(Request.TargetRef, PackageName))
				{
					Result.ErrorCode = TEXT("modeling_material_asset_ref_invalid");
					return Result;
				}
				UStaticMesh* const Mesh = LoadObject<UStaticMesh>(
					nullptr, *Request.TargetRef);
				if (!Mesh)
				{
					Result.ErrorCode = TEXT("modeling_material_asset_missing");
					return Result;
				}
				if (!Mesh->GetStaticMaterials().IsValidIndex(Request.Index))
				{
					Result.ErrorCode = TEXT("modeling_material_index_invalid");
					return Result;
				}
				FAssetDurableMutation Durable;
				FString Error;
				if (!Durable.Prepare(
						Request, PackageName, OwnerId, JournalRoot,
						Counter.Increment(), Error))
				{
					Result.ErrorCode = TEXT("modeling_material_prepare_failed");
					Result.ErrorDetail = Error;
					return Result;
				}
				Result.JournalId = Durable.JournalId();
				const int32 ExpectedCount = Mesh->GetStaticMaterials().Num() - 1;
				{
					FScopedTransaction Transaction(
						NSLOCTEXT("UeAgentInterface", "ModelingRemoveMaterialSlot",
							"UeAgentInterface Remove Material Slot"));
					Mesh->Modify();
					Mesh->GetStaticMaterials().RemoveAt(Request.Index);
					Mesh->PostEditChange();
					Mesh->MarkPackageDirty();
				}
				if (!Durable.Persist(Error))
				{
					Durable.Rollback(false);
					Result.TerminalState = TEXT("state_unknown");
					Result.ErrorCode = TEXT("modeling_material_persist_failed");
					Result.ErrorDetail = Error;
					return Result;
				}
				UStaticMesh* const Reloaded = Cast<UStaticMesh>(
					ReloadForIndependentReadback(PackageName));
				Result.bReadbackVerified = Reloaded
					&& Reloaded->GetStaticMaterials().Num() == ExpectedCount;
				Result.VerificationId = TEXT("modeling-material-readback:")
					+ Request.RequestId;
				if (!Result.bReadbackVerified
					|| !Durable.Acknowledge(Result.VerificationId, Error))
				{
					Result.TerminalState = TEXT("state_unknown");
					Result.ErrorCode = TEXT("modeling_material_readback_failed");
					Result.ErrorDetail = Error;
					return Result;
				}
				Result.bChanged = true;
				Result.bPersisted = true;
				Result.PersistenceScope = TEXT("asset_package_durable");
				Result.TerminalState = TEXT("committed");
				return Result;
			}

			FUeAgentWorldDestructiveResult RemoveVoxelBounds(
				const FUeAgentWorldDestructiveRequest& Request)
			{
				FUeAgentWorldDestructiveResult Result = Base(Request);
				if (Request.Space != TEXT("component_local")
					|| Request.BoundsMin.X > Request.BoundsMax.X
					|| Request.BoundsMin.Y > Request.BoundsMax.Y
					|| Request.BoundsMin.Z > Request.BoundsMax.Z)
				{
					Result.ErrorCode = TEXT("modeling_voxel_space_or_bounds_invalid");
					return Result;
				}
				AActor* const Actor = FindActor(*World, Request.TargetRef);
				UInstancedStaticMeshComponent* const Component = Actor
					? Cast<UInstancedStaticMeshComponent>(
						FindComponent(*Actor, Request.ComponentName)) : nullptr;
				if (!Component)
				{
					Result.ErrorCode = TEXT("modeling_voxel_component_missing");
					return Result;
				}
				const FBox Bounds(Request.BoundsMin, Request.BoundsMax);
				TArray<int32> Indices;
				for (int32 Index = 0; Index < Component->GetInstanceCount(); ++Index)
				{
					FTransform Transform;
					if (!Component->GetInstanceTransform(Index, Transform, false))
					{
						Result.ErrorCode = TEXT("modeling_voxel_read_failed");
						return Result;
					}
					if (Bounds.IsInsideOrOn(Transform.GetLocation()))
					{
						Indices.Add(Index);
					}
				}
				if (Indices.IsEmpty())
				{
					Result.ErrorCode = TEXT("modeling_voxel_bounds_empty");
					return Result;
				}
				FScopedTransaction Transaction(
					NSLOCTEXT("UeAgentInterface", "ModelingRemoveVoxelBounds",
						"UeAgentInterface Remove Voxel Bounds"));
				Component->Modify();
				for (int32 Offset = Indices.Num() - 1; Offset >= 0; --Offset)
				{
					if (!Component->RemoveInstance(Indices[Offset]))
					{
						Result.TerminalState = TEXT("state_unknown");
						Result.ErrorCode = TEXT("modeling_voxel_remove_failed");
						return Result;
					}
				}
				bool bAbsent = true;
				for (int32 Index = 0; Index < Component->GetInstanceCount(); ++Index)
				{
					FTransform Transform;
					bAbsent &= Component->GetInstanceTransform(Index, Transform, false)
						&& !Bounds.IsInsideOrOn(Transform.GetLocation());
				}
				return CommitWorld(
					Request, {Actor->GetOutermost()}, true, bAbsent);
			}

			TWeakObjectPtr<UWorld> World;
			FString JournalRoot;
			FString OwnerId;
			FThreadSafeCounter Counter;
		};
	}

	bool TryCreateWorldDestructiveOwner(
		UWorld& ExplicitWorld,
		FString JournalRoot,
		FString JournalOwnerId,
		TSharedPtr<IUeAgentWorldDestructiveOwner>& OutOwner,
		FString& OutError)
	{
		OutOwner.Reset();
		OutError.Reset();
		if (!IsValid(&ExplicitWorld))
		{
			OutError = TEXT("world_destructive_world_invalid");
			return false;
		}
		JournalRoot.TrimStartAndEndInline();
		JournalOwnerId.TrimStartAndEndInline();
		if (JournalRoot.IsEmpty() || JournalOwnerId.IsEmpty())
		{
			OutError = TEXT("world_destructive_journal_config_invalid");
			return false;
		}
		OutOwner = MakeShared<FWorldDestructiveOwner>(
			ExplicitWorld, MoveTemp(JournalRoot), MoveTemp(JournalOwnerId));
		return true;
	}
}
