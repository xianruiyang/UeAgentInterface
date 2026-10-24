#include "DomainAdapters/WorldDestructive/UeAgentWorldDestructiveOwner.h"
#include "DomainAdapters/WorldDestructive/UeAgentPcgCleanupMutation.h"
#include "DomainAdapters/ContentDiscovery/UeAgentEditorContentDiscovery.h"

#include "Core/Contract/UeAgentDiagnostic.h"

#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Components/InstancedStaticMeshComponent.h"
#include "Components/StaticMeshComponent.h"
#include "Core/AssetDurable/UeAgentAssetDurable.h"
#include "Core/AssetDurable/UeAgentPackageHeader.h"
#include "Core/ContextResource/UeAgentContextResource.h"
#include "Core/Contract/UeAgentContractJson.h"
#include "Core/PlanningPreflight/UeAgentPlanningPreflight.h"
#include "Core/StateSemantic/UeAgentStateSemantic.h"
#include "DataLayer/DataLayerEditorSubsystem.h"
#include "Domains/Level/Authoring/UeAgentLevelAuthoringState.h"
#include "Domains/Level/Authoring/UeAgentLevelActorDeletion.h"
#include "Editor.h"
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
#include "UObject/Package.h"
#include "UObject/SavePackage.h"
#include "Core/AssetDurable/UeAgentPackageSave.h"
#include "Utilities/AuthoringOperationApply/UeAgentAuthoringPackageReadback.h"
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
		FDiagnostic WorldFailure(const FString& Code, const FString& Reason, const FString& Target,
			const FString& Field, TSharedPtr<FJsonValue> Actual, TSharedPtr<FJsonValue> Expected, const FString& InternalContext = FString())
		{
			auto Diagnostic = FDiagnostic::Failure(Code, Reason, TEXT("world_destructive"), InternalContext);
			Diagnostic.SourceKind = TEXT("native_result");
			Diagnostic.TargetPath = Target;
			Diagnostic.PropertyPath = Field;
			Diagnostic.ActualValue = MoveTemp(Actual);
			Diagnostic.ExpectedValue = MoveTemp(Expected);
			return Diagnostic;
		}

		FDiagnostic WorldInputFailure(const FString& Code, const FString& Reason, const FString& Target,
			const FString& Field, TSharedPtr<FJsonValue> Actual, TSharedPtr<FJsonValue> Expected)
		{
			auto Diagnostic = WorldFailure(Code, Reason, Target, TEXT("/") + Field, MoveTemp(Actual), MoveTemp(Expected));
			Diagnostic.SourceKind = TEXT("command_input");
			Diagnostic.Source.JsonPath = TEXT("/params/") + Field;
			return Diagnostic;
		}

		void SupplementWorldDiagnostic(FDiagnostic& Diagnostic, const FString& Target)
		{
			if (Diagnostic.IsEmpty()) return;
			if (Diagnostic.TargetPath.IsEmpty()) Diagnostic.TargetPath = Target;
			if (Diagnostic.Stage.IsEmpty()) Diagnostic.Stage = TEXT("world_destructive");
			if (Diagnostic.SourceKind.IsEmpty() && Diagnostic.Source.SourceFile.IsEmpty()) Diagnostic.SourceKind = TEXT("native_result");
			for (auto& Child : Diagnostic.Diagnostics) SupplementWorldDiagnostic(Child, Target);
			if (Diagnostic.PublicCause)
			{
				auto Cause = MakeShared<FDiagnostic>(*Diagnostic.PublicCause);
				SupplementWorldDiagnostic(*Cause, Target);
				Diagnostic.PublicCause = MoveTemp(Cause);
			}
		}

		TSharedPtr<FJsonValue> WorldVectorValue(const FVector& Value)
		{
			return MakeShared<FJsonValueArray>(TArray<TSharedPtr<FJsonValue>>{
				MakeShared<FJsonValueNumber>(Value.X), MakeShared<FJsonValueNumber>(Value.Y), MakeShared<FJsonValueNumber>(Value.Z)});
		}

		FUeAgentResourceTracker MakeTracker()
		{
			return FUeAgentResourceTracker();
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

		UObject* ReloadForIndependentReadback(const FString& PackageName, UE::UAI::Contract::FDiagnostic& Error)
		{
			if (!UeAgentAuthoringOperationApply::TryReloadPersistedPackage(PackageName, Error))
			{
				// This owner supplies a validated authored package reference. The
				// shared readback helper also serves private transaction packages.
				if (Error.TargetPath.IsEmpty()) Error.TargetPath = PackageName;
				return nullptr;
			}
			UObject* Reloaded = FindObject<UObject>(nullptr, *ObjectPath(PackageName));
			if (!Reloaded)
				Error = WorldFailure(TEXT("world_destructive_reloaded_asset_missing"), TEXT("Persisted package reloaded without its expected top-level asset."), PackageName, TEXT("/asset"), MakeShared<FJsonValueBoolean>(false), MakeShared<FJsonValueBoolean>(true));
			return Reloaded;
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
				UE::UAI::Contract::FDiagnostic& OutError)
			{
				Identity = IdentityFor(PackageName);
				Token = FencingToken;
				Plan = MakePlan(Request, PackageName, OwnerId, Identity);
				const FAssetDurableRequest DurableRequest(
					Plan.ToSharedRef(),
					{FAssetDurableTarget(
						Identity, PackageName, EPackageHeaderKind::Asset,
						EAssetDurableIntent::Save,
						ERollbackGrade::DurableBackup)},
					Token, OwnerId, EAtomicityMode::CompensatedSaga);
				FAssetDurableError Error;
				if (!TryPrepareAssetDurability(
						DurableRequest, Backend, JournalRoot, Resources, Session, Error))
				{
					OutError = Error;
					return false;
				}
				return true;
			}

			bool Persist(UE::UAI::Contract::FDiagnostic& OutError)
			{
				FAssetDurableError Error;
				if (!TryPersistAssetDurabilityForVerification(
						Session.ToSharedRef(), Backend, Token, Resources, Receipt, Error))
				{
					OutError = Error;
					return false;
				}
				return true;
			}

			bool Acknowledge(const FString& VerificationId, UE::UAI::Contract::FDiagnostic& OutError)
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
					OutError = Error;
					return false;
				}
				return true;
			}

			void Rollback(const bool bMemoryConfirmed, FDiagnostic& OutError)
			{
				OutError.Reset();
				if (!Session.IsValid())
				{
					OutError = WorldFailure(TEXT("world_destructive_rollback_session_unavailable"), TEXT("Durable rollback has no prepared session."), FString(), TEXT("/rollback/session"), MakeShared<FJsonValueBoolean>(false), MakeShared<FJsonValueBoolean>(true));
					return;
				}
				FDurableReceipt RollbackReceipt;
				FAssetDurableError Error;
				if (!TryRollbackAssetDurability(Session.ToSharedRef(), Backend, Token, bMemoryConfirmed,
					Resources, RollbackReceipt, Error)) OutError = MoveTemp(Error);
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
			UE::UAI::Contract::FDiagnostic& OutError)
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
				UE::UAI::Contract::FDiagnostic HeaderError;
				if (!TryPrepareLoadedPackageHeaderForSave(
						*Package,
						TopLevelAsset,
						Filename,
						HeaderError))
				{
					OutError = HeaderError;
                    if (OutError.TargetPath.IsEmpty()) OutError.TargetPath = PackageName;
					return false;
				}
				FSavePackageArgs Args;
				Args.TopLevelFlags = RF_Public | RF_Standalone;
				UE::UAI::Contract::FDiagnostic SaveDetail;
				if (!TrySaveLoadedPackage(
						*Package,
						TopLevelAsset,
						Filename,
						Args, SaveDetail))
				{
					OutError = SaveDetail;
					return false;
				}
				FString ObservedFilename;
				HeaderError.Reset();
				if (!TryResolvePackageHeaderFilename(PackageName, EPackageHeaderKind::EvidenceRequired, ObservedFilename, HeaderError))
				{
					OutError = MoveTemp(HeaderError);
					SupplementWorldDiagnostic(OutError, PackageName);
					return false;
				}
				if (!ObservedFilename.Equals(Filename, ESearchCase::IgnoreCase))
				{
					OutError = WorldFailure(TEXT("world_destructive_package_save_readback_failed"), TEXT("Saved package header resolved to a different filename."), PackageName, TEXT("/package/header"), MakeShared<FJsonValueString>(ObservedFilename), MakeShared<FJsonValueString>(Filename));
					return false;
				}
				if (!IFileManager::Get().FileExists(*ObservedFilename))
				{
					OutError = WorldFailure(TEXT("world_destructive_package_save_readback_failed"), TEXT("Saved package header file is missing on disk."), PackageName, TEXT("/package/header"), MakeShared<FJsonValueBoolean>(false), MakeShared<FJsonValueBoolean>(true));
					OutError.ActualEvidence = MakeShared<FJsonValueString>(ObservedFilename);
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
				UWorld& InWorld,
				FString InJournalRoot,
				FString InOwnerId,
				TSharedRef<IUeAgentWorldDestructiveDurableRunner>
					InDurableRunner)
				: World(&InWorld)
				, JournalRoot(MoveTemp(InJournalRoot))
				, OwnerId(MoveTemp(InOwnerId))
				, DurableRunner(MoveTemp(InDurableRunner))
			{
			}

			FUeAgentWorldDestructiveResult Execute(
				const FUeAgentWorldDestructiveRequest& Request,
				const TSharedRef<IUeAgentCancellationSignal>& Cancellation) override
			{
				FUeAgentWorldDestructiveResult Result;
				Result.CommandId = Request.CommandId;
				Result.TargetRef = Request.TargetRef;
				auto Finish = [&Request](FUeAgentWorldDestructiveResult Value)
				{
					SupplementWorldDiagnostic(Value.Diagnostic, Request.TargetRef);
					if (!Value.Diagnostic.IsEmpty()) Value.Diagnostic.TerminalStatus = Value.TerminalState;
					return Value;
				};
				auto Fail = [&Result, &Finish](FDiagnostic Diagnostic)
				{
					Result.Diagnostic = MoveTemp(Diagnostic);
					return Finish(MoveTemp(Result));
				};
				if (!IsInGameThread())
				{
					return Fail(WorldFailure(TEXT("world_destructive_game_thread_required"), TEXT("World destructive operations must run on the game thread."), Request.TargetRef, TEXT("/thread"), MakeShared<FJsonValueString>(TEXT("worker thread")), MakeShared<FJsonValueString>(TEXT("game thread"))));
				}
				if (!World.IsValid())
				{
					return Fail(WorldFailure(TEXT("world_destructive_world_unavailable"), TEXT("The world bound to this destructive operation is no longer available."), Request.TargetRef, TEXT("/world"), MakeShared<FJsonValueBoolean>(false), MakeShared<FJsonValueBoolean>(true)));
				}
				if (Cancellation->IsCancelled())
				{
					return Fail(WorldFailure(TEXT("world_destructive_cancelled"), TEXT("The destructive world operation was cancelled before execution."), Request.TargetRef, TEXT("/cancellation"), MakeShared<FJsonValueBoolean>(true), MakeShared<FJsonValueBoolean>(false), Request.RequestId));
				}
				if (Request.CommandId == TEXT("world.data_layer.delete"))
					return Finish(DeleteDataLayer(Request, Cancellation));
				if (Request.CommandId == TEXT("world.level_content.delete_scope"))
					return Finish(DeleteFolder(Request));
				if (Request.CommandId == TEXT("world.pcg.cleanup"))
					return Finish(CleanupPcg(Request, Cancellation));
				if (Request.CommandId == TEXT("world.level_streaming.move_actors"))
					return Finish(MoveActor(Request));
				if (Request.CommandId == TEXT("world.level_streaming.remove"))
					return Finish(RemoveStreamingLevel(Request));
				if (Request.CommandId == TEXT("modeling.actor.mesh.replace"))
					return Finish(ReplaceActorMesh(Request));
				if (Request.CommandId == TEXT("modeling.asset.material_slot.remove"))
					return Finish(RemoveMaterialSlot(Request));
				if (Request.CommandId == TEXT("modeling.voxel.bounds.remove"))
					return Finish(RemoveVoxelBounds(Request));
				return Fail(WorldFailure(TEXT("world_destructive_command_unknown"), TEXT("No destructive world operation is registered for this command."), Request.TargetRef, TEXT("/command_id"), MakeShared<FJsonValueString>(Request.CommandId), MakeShared<FJsonValueString>(TEXT("registered world destructive command"))));
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
				FDiagnostic ReadbackError)
			{
				FUeAgentWorldDestructiveResult Result = Base(Request);
				Result.bChanged = bChanged;
				Result.bReadbackVerified = ReadbackError.IsEmpty();
				Result.VerificationId = TEXT("world-destructive-readback:")
					+ Request.RequestId;
				UE::UAI::Contract::FDiagnostic Error;
				if (!ReadbackError.IsEmpty())
				{
					Result.TerminalState = TEXT("state_unknown");
					Result.Diagnostic = MoveTemp(ReadbackError);
					return Result;
				}
				if (!PersistPackages(Packages, Result.bPersisted, Error))
				{
					Result.TerminalState = TEXT("state_unknown");
					Result.Diagnostic = MoveTemp(Error);
					return Result;
				}
				Result.PersistenceScope = Result.bPersisted
					? TEXT("package") : TEXT("editor_transaction");
				Result.TerminalState = TEXT("committed");
				return Result;
			}

			class FDataLayerDeleteMutation final
				: public IUeAgentWorldDestructiveDurableMutation
			{
			public:
				FDataLayerDeleteMutation(
					AWorldDataLayers& InLayers,
					FName InLayerName,
					FString InLayerRef,
					FString InMapPackageName,
					FString InMutationPackageName,
					const FString& RequestId)
					: Layers(&InLayers)
					, LayerName(InLayerName)
					, LayerRef(MoveTemp(InLayerRef))
					, PackageName(MoveTemp(InMapPackageName))
					, TransactionTitle(
						TEXT("UeAgentInterface Delete Data Layer ")
						+ RequestId)
				{
					MutationPackages.Add({PackageName, EPackageHeaderKind::Map, EAssetDurableIntent::Save});
					if (InMutationPackageName != PackageName)
						MutationPackages.Add({MoveTemp(InMutationPackageName), EPackageHeaderKind::EvidenceRequired, EAssetDurableIntent::Save});
				}

				const FString& GetPackageName() const override
				{
					return PackageName;
				}

				const TArray<FWorldDestructivePackage>& GetMutationPackages() const override
				{
					return MutationPackages;
				}

				const FString& GetPersistenceScope() const override
				{
					static const FString Scope =
						TEXT("world_map_asset_durable");
					return Scope;
				}

				bool Apply(UE::UAI::Contract::FDiagnostic& OutError) override
				{
					AWorldDataLayers* const Current = Layers.Get();
					const UDataLayerInstance* const Layer = Current
						? Current->GetDataLayerInstance(LayerName)
						: nullptr;
					if (!Current || !Layer)
					{
						OutError = LayerFailure(TEXT("world_data_layer_missing"), TEXT("The requested data layer or its WorldDataLayers owner is no longer available."), MakeShared<FJsonValueString>(!Current ? TEXT("owner missing") : TEXT("layer missing")), MakeShared<FJsonValueString>(TEXT("data layer and owner present")));
						return false;
					}
					if (!Layer->CanBeRemoved())
					{
						OutError = LayerFailure(TEXT("world_data_layer_not_removable"), TEXT("The data layer does not allow removal."), MakeShared<FJsonValueBoolean>(false), MakeShared<FJsonValueBoolean>(true));
						return false;
					}
					UDataLayerEditorSubsystem* const DataLayerEditor =
						UDataLayerEditorSubsystem::Get();
					if (!DataLayerEditor)
					{
						OutError = LayerFailure(TEXT("world_data_layer_editor_subsystem_unavailable"), TEXT("Data-layer deletion requires the DataLayer Editor subsystem."), MakeShared<FJsonValueBoolean>(false), MakeShared<FJsonValueBoolean>(true));
						return false;
					}
					if (!GEditor
						|| !GEditor->CanTransact()
						|| GEditor->IsTransactionActive())
					{
						auto Actual = MakeShared<FJsonObject>();
						Actual->SetBoolField(TEXT("editor_available"), GEditor != nullptr);
						Actual->SetBoolField(TEXT("can_transact"), GEditor && GEditor->CanTransact());
						Actual->SetBoolField(TEXT("transaction_active"), GEditor && GEditor->IsTransactionActive());
						OutError = LayerFailure(TEXT("world_data_layer_transaction_unavailable"), TEXT("Data-layer deletion requires an Editor that can transact with no active transaction."), MakeShared<FJsonValueObject>(Actual), MakeShared<FJsonValueString>(TEXT("Editor available, can transact, no active transaction")));
						return false;
					}
					{
						FScopedTransaction Transaction(
							FText::FromString(TransactionTitle));
						Current->Modify();
						DataLayerEditor->DeleteDataLayer(
							const_cast<UDataLayerInstance*>(Layer));
					}
					if (Current->GetDataLayerInstance(LayerName))
					{
						OutError = LayerFailure(TEXT("world_data_layer_delete_readback_failed"), TEXT("The data layer is still present after native deletion."), MakeShared<FJsonValueBoolean>(true), MakeShared<FJsonValueBoolean>(false));
						return false;
					}
					return true;
				}

				bool Rollback(UE::UAI::Contract::FDiagnostic& OutError) override
				{
					AWorldDataLayers* const Current = Layers.Get();
					if (!Current || !GEditor
						|| GEditor->IsTransactionActive())
					{
						auto Actual = MakeShared<FJsonObject>();
						Actual->SetBoolField(TEXT("owner_available"), Current != nullptr);
						Actual->SetBoolField(TEXT("editor_available"), GEditor != nullptr);
						Actual->SetBoolField(TEXT("transaction_active"), GEditor && GEditor->IsTransactionActive());
						OutError = LayerFailure(TEXT("world_data_layer_rollback_unavailable"), TEXT("Data-layer rollback requires the original owner and an Editor with no active transaction."), MakeShared<FJsonValueObject>(Actual), MakeShared<FJsonValueString>(TEXT("owner and Editor present, no active transaction")));
						return false;
					}
					if (GEditor->GetTransactionName().ToString()
						!= TransactionTitle)
					{
						OutError = LayerFailure(TEXT("world_data_layer_rollback_fence_stale"), TEXT("The latest Editor transaction no longer belongs to this data-layer deletion."), MakeShared<FJsonValueBoolean>(false), MakeShared<FJsonValueBoolean>(true));
						OutError.InternalContext = GEditor->GetTransactionName().ToString() + TEXT(" != ") + TransactionTitle;
						return false;
					}
					const bool bUndoSucceeded = GEditor->UndoTransaction(false);
					if (!bUndoSucceeded || !Current->GetDataLayerInstance(LayerName))
					{
						OutError = LayerFailure(TEXT("world_data_layer_rollback_unconfirmed"), TEXT("Data-layer rollback did not restore the deleted layer."), MakeShared<FJsonValueString>(!bUndoSucceeded ? TEXT("Editor undo failed") : TEXT("layer still missing")), MakeShared<FJsonValueString>(TEXT("undo succeeded and layer present")));
						return false;
					}
					return true;
				}

				bool VerifyPersisted(
					UWorld& PersistedWorld,
					const FPersistedWorldReferenceMap& References,
					UE::UAI::Contract::FDiagnostic& OutError) const override
				{
					const AWorldDataLayers* const PersistedLayers =
						PersistedWorld.GetWorldDataLayers();
					if (!PersistedLayers)
					{
						OutError = LayerFailure(TEXT("world_data_layer_persisted_owner_missing"), TEXT("Independent persisted world readback has no WorldDataLayers owner."), MakeShared<FJsonValueBoolean>(false), MakeShared<FJsonValueBoolean>(true));
						return false;
					}
					if (PersistedLayers->GetDataLayerInstance(LayerName))
					{
						OutError = LayerFailure(TEXT("world_data_layer_persisted_instance_present"), TEXT("The deleted data layer is still present in independent persisted world readback."), MakeShared<FJsonValueBoolean>(true), MakeShared<FJsonValueBoolean>(false));
						return false;
					}
					return true;
				}

			private:
				FDiagnostic LayerFailure(const FString& Code, const FString& Reason, TSharedPtr<FJsonValue> Actual, TSharedPtr<FJsonValue> Expected) const
				{
					auto Diagnostic = WorldFailure(Code, Reason, PackageName, TEXT("/layer"), MoveTemp(Actual), MoveTemp(Expected));
					Diagnostic.Stage = TEXT("authoring");
					Diagnostic.CandidateName = LayerRef;
					return Diagnostic;
				}

				TWeakObjectPtr<AWorldDataLayers> Layers;
				FName LayerName;
				FString LayerRef;
				FString PackageName;
				TArray<FWorldDestructivePackage> MutationPackages;
				FString TransactionTitle;
			};

			FUeAgentWorldDestructiveResult DeleteDataLayer(
				const FUeAgentWorldDestructiveRequest& Request,
				const TSharedRef<IUeAgentCancellationSignal>& Cancellation)
			{
				FUeAgentWorldDestructiveResult Result = Base(Request);
				AWorldDataLayers* const WorldLayers =
					World->GetWorldDataLayers();
				UDataLayerInstance* Layer = nullptr;
				UE::UAI::Contract::FDiagnostic ResolveError;
				if (!WorldLayers)
				{
					Result.Diagnostic = WorldFailure(TEXT("world_data_layers_missing"), TEXT("The current world has no WorldDataLayers owner."), Request.TargetRef, TEXT("/layer"), MakeShared<FJsonValueBoolean>(false), MakeShared<FJsonValueBoolean>(true));
					return Result;
				}
				if (!UeAgentLevelAuthoring::TryResolveDataLayerInstanceExact(*WorldLayers, Request.TargetRef, Layer, ResolveError))
				{
					Result.Diagnostic = MoveTemp(ResolveError);
					return Result;
				}
				AWorldDataLayers* const MutationLayers =
					Layer->GetDirectOuterWorldDataLayers();
				if (!MutationLayers)
				{
					Result.Diagnostic = WorldFailure(TEXT("world_data_layer_owner_missing"), TEXT("The resolved data layer has no direct WorldDataLayers owner."), Request.TargetRef, TEXT("/layer"), MakeShared<FJsonValueBoolean>(false), MakeShared<FJsonValueBoolean>(true));
					return Result;
				}
				const FName LayerName = Layer->GetDataLayerFName();
				return DurableRunner->Execute(
					*World,
					Request,
					MakeShared<FDataLayerDeleteMutation>(
						*MutationLayers,
						LayerName,
						Request.TargetRef,
						World->GetOutermost()->GetName(),
						MutationLayers->GetOutermost()->GetName(),
						Request.RequestId),
					Cancellation);
			}

			FUeAgentWorldDestructiveResult DeleteFolder(
				const FUeAgentWorldDestructiveRequest& Request)
			{
				FUeAgentWorldDestructiveResult Result = Base(Request);
				const FString Root = Request.TargetRef.TrimStartAndEnd();
				const FString ManagedId = Request.ManagedId.TrimStartAndEnd();
				if (Root.IsEmpty())
				{
					Result.Diagnostic = WorldInputFailure(TEXT("world_folder_path_required"),
						TEXT("Folder deletion requires a non-empty folder path."), Request.TargetRef, TEXT("folder_path"),
						MakeShared<FJsonValueString>(Request.TargetRef), MakeShared<FJsonValueString>(TEXT("non-empty folder path")));
					return Result;
				}
				TArray<AActor*> Actors;
				if (!ManagedId.IsEmpty())
				{
					AActor* Actor = nullptr;
					UE::UAI::Contract::FDiagnostic ResolveError;
					if (!UeAgentLevelAuthoring::TryFindManagedActorExact(
							*World,
							ManagedId,
							Root,
							Actor,
							ResolveError))
					{
						Result.Diagnostic = MoveTemp(ResolveError);
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
					Result.Diagnostic = WorldFailure(TEXT("world_folder_scope_empty"), TEXT("No actors were found in the requested folder scope."), Request.TargetRef, TEXT("/folder_path"), MakeShared<FJsonValueNumber>(Actors.Num()), MakeShared<FJsonValueString>(TEXT("at least one actor")));
					return Result;
				}
				FScopedTransaction Transaction(
					NSLOCTEXT("UeAgentInterface", "WorldDeleteFolder",
						"UeAgentInterface Delete Folder Content"));
				UeAgentLevelAuthoring::FActorDeletion Deletion;
				if (!Deletion.Apply(*World, Actors, Result.Diagnostic))
				{

					return Result;
				}
				FDiagnostic ReadbackError;
				for (TActorIterator<AActor> It(World.Get()); It; ++It)
				{
					FString Folder;
					bool bStillPresent;
					if (!ManagedId.IsEmpty())
						bStillPresent = UeAgentLevelAuthoring::GetManagedActorId(**It).Equals(ManagedId, ESearchCase::CaseSensitive);
					else
					{
						Folder = It->GetFolderPath().ToString();
						bStillPresent = Folder == Root || Folder.StartsWith(Root + TEXT("/"));
					}
					if (bStillPresent && ReadbackError.IsEmpty())
					{
						if (!ManagedId.IsEmpty()) Folder = It->GetFolderPath().ToString();
						ReadbackError = WorldFailure(TEXT("world_destructive_readback_failed"),
							TEXT("An actor remains in the requested deletion scope after native deletion."),
							It->GetPathName(), TEXT("/folder_path"), MakeShared<FJsonValueString>(Folder),
							MakeShared<FJsonValueString>(TEXT("actor absent from the requested deletion scope")));
					}
				}
				return CommitWorld(Request, {World->GetOutermost()}, true, MoveTemp(ReadbackError));
			}

			FUeAgentWorldDestructiveResult CleanupPcg(
				const FUeAgentWorldDestructiveRequest& Request,
				const TSharedRef<IUeAgentCancellationSignal>& Cancellation)
			{
				FUeAgentWorldDestructiveResult Result = Base(Request);
				TArray<UObject*> Actors;
				for (TActorIterator<AActor> It(World.Get()); It; ++It) if (IsValid(*It)) Actors.Add(*It);
				UObject* Resolved = nullptr;
				if (!DomainAdapters::ContentDiscovery::TryResolveScopedObjectReference(Actors, Request.TargetRef, Resolved, Result.Diagnostic))
				{
					Result.Diagnostic.Source.JsonPath = TEXT("/actor_ref");
					return Result;
				}
				AActor* Actor = CastChecked<AActor>(Resolved);
				TArray<UPCGComponent*> NativeComponents;
				Actor->GetComponents(NativeComponents);
				TArray<UObject*> Components;
				for (UPCGComponent* Component : NativeComponents) if (IsValid(Component)) Components.Add(Component);
				if (!DomainAdapters::ContentDiscovery::TryResolveScopedObjectReference(Components, Request.ComponentName, Resolved, Result.Diagnostic))
				{
					Result.Diagnostic.Source.JsonPath = TEXT("/component_name");
					return Result;
				}
				TSharedPtr<IUeAgentWorldDestructiveDurableMutation> Mutation;
				if (!TryCreatePcgCleanupMutation(*CastChecked<UPCGComponent>(Resolved), Mutation, Result.Diagnostic))
				{
					if (Result.Diagnostic.MutationMayHaveStarted.Get(false))
					{
						Result.TerminalState = TEXT("state_unknown");
						Result.bChanged = true;
					}
					return Result;
				}
				return DurableRunner->Execute(*World, Request, Mutation.ToSharedRef(), Cancellation);
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
					Result.Diagnostic = WorldFailure(Actor ? TEXT("world_streaming_level_missing") : TEXT("world_actor_missing"),
						Actor ? TEXT("The destination streaming level is not loaded.") : TEXT("The actor to move was not found."),
						Request.TargetRef, Actor ? TEXT("/level_package") : TEXT("/actor_ref"),
						MakeShared<FJsonValueString>(Actor ? Request.OperandRef : Request.TargetRef),
						MakeShared<FJsonValueString>(Actor ? TEXT("loaded streaming level") : TEXT("actor in the current world")));
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
					Result.Diagnostic = WorldFailure(TEXT("world_move_actor_failed"), TEXT("Native move did not move exactly one actor to the requested streaming level."), Request.TargetRef, TEXT("/level_package"), MakeShared<FJsonValueNumber>(Count), MakeShared<FJsonValueNumber>(1));
					Result.Diagnostic.CandidateName = Request.OperandRef;
					return Result;
				}
				AActor* ReadbackActor = nullptr;
				bool bLabelAmbiguous = false;
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
								bLabelAmbiguous = true;
								ReadbackActor = nullptr;
								break;
							}
							ReadbackActor = Candidate;
						}
					}
				}
				FDiagnostic ReadbackError;
				if (!ReadbackActor)
					ReadbackError = WorldFailure(TEXT("world_destructive_readback_failed"),
						bLabelAmbiguous ? TEXT("Multiple actors in the destination level share the moved actor label.")
							: TEXT("The moved actor is missing from the destination level."),
						Request.TargetRef, TEXT("/level_package"), MakeShared<FJsonValueString>(StableActorLabel),
						MakeShared<FJsonValueString>(Request.OperandRef));
				else if (ReadbackActor->GetLevel() != Level)
					ReadbackError = WorldFailure(TEXT("world_destructive_readback_failed"),
						TEXT("The moved actor is in a different level than requested."), ReadbackActor->GetPathName(),
						TEXT("/level_package"), MakeShared<FJsonValueString>(ReadbackActor->GetLevel() ? ReadbackActor->GetLevel()->GetPathName() : TEXT("<missing>")),
						MakeShared<FJsonValueString>(Level->GetPathName()));
				FUeAgentWorldDestructiveResult Committed = CommitWorld(
					Request, {World->GetOutermost(), Level->GetOutermost()}, true, MoveTemp(ReadbackError));
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
					Result.Diagnostic = WorldFailure(TEXT("world_streaming_level_missing"), TEXT("The streaming level to remove is not loaded."), Request.TargetRef, TEXT("/level_package"), MakeShared<FJsonValueString>(Request.TargetRef), MakeShared<FJsonValueString>(TEXT("loaded streaming level")));
					return Result;
				}
				const FString PackageName = Streaming->GetWorldAssetPackageName();
				FScopedTransaction Transaction(
					NSLOCTEXT("UeAgentInterface", "WorldRemoveStreaming",
						"UeAgentInterface Remove Streaming Level"));
				if (!UEditorLevelUtils::RemoveLevelFromWorld(Level, false, false))
				{
					Transaction.Cancel();
					Result.Diagnostic = WorldFailure(TEXT("world_streaming_remove_failed"), TEXT("Unreal Editor rejected removal of the loaded streaming level."), Request.TargetRef, TEXT("/level_package"), MakeShared<FJsonValueBoolean>(false), MakeShared<FJsonValueBoolean>(true));
					return Result;
				}
				FDiagnostic ReadbackError;
				if (FindStreamingLevel(*World, PackageName) != nullptr)
					ReadbackError = WorldFailure(TEXT("world_destructive_readback_failed"),
						TEXT("The removed streaming level is still registered in the world."), PackageName,
						TEXT("/streaming_levels"), MakeShared<FJsonValueBoolean>(true), MakeShared<FJsonValueBoolean>(false));
				return CommitWorld(Request, {World->GetOutermost()}, true, MoveTemp(ReadbackError));
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
					Result.Diagnostic = WorldFailure(!Actor ? TEXT("modeling_actor_missing")
						: !Component ? TEXT("modeling_static_mesh_component_missing") : TEXT("modeling_static_mesh_missing"),
						!Actor ? TEXT("The actor to receive the static mesh was not found.")
							: !Component ? TEXT("The actor has no StaticMeshComponent.") : TEXT("The replacement static mesh asset could not be loaded."),
						Request.TargetRef, !Actor ? TEXT("/actor_ref") : !Component ? TEXT("/component") : TEXT("/mesh_ref"),
						MakeShared<FJsonValueString>(!Actor ? Request.TargetRef : !Component ? TEXT("component missing") : Request.OperandRef),
						MakeShared<FJsonValueString>(!Actor ? TEXT("actor in current world") : !Component ? TEXT("StaticMeshComponent") : TEXT("loadable StaticMesh")));
					return Result;
				}
				FScopedTransaction Transaction(
					NSLOCTEXT("UeAgentInterface", "ModelingReplaceMesh",
						"UeAgentInterface Replace Actor Mesh"));
				Component->Modify();
				Component->SetStaticMesh(Mesh);
				Component->MarkRenderStateDirty();
				Actor->MarkPackageDirty();
				FDiagnostic ReadbackError;
				UStaticMesh* const ObservedMesh = Component->GetStaticMesh();
				if (ObservedMesh != Mesh)
					ReadbackError = WorldFailure(TEXT("world_destructive_readback_failed"),
						TEXT("The component static mesh differs from the requested replacement."), Component->GetPathName(),
						TEXT("/mesh_ref"), MakeShared<FJsonValueString>(ObservedMesh ? ObservedMesh->GetPathName() : TEXT("<missing>")),
						MakeShared<FJsonValueString>(Mesh->GetPathName()));
				return CommitWorld(Request, {Actor->GetOutermost()}, true, MoveTemp(ReadbackError));
			}

			FUeAgentWorldDestructiveResult RemoveMaterialSlot(
				const FUeAgentWorldDestructiveRequest& Request)
			{
				FUeAgentWorldDestructiveResult Result = Base(Request);
				FString PackageName;
				if (!TryPackageName(Request.TargetRef, PackageName))
				{
					Result.Diagnostic = WorldInputFailure(TEXT("modeling_material_asset_ref_invalid"),
						TEXT("Material-slot removal requires a valid project asset package under /Game/."), Request.TargetRef, TEXT("target_ref"),
						MakeShared<FJsonValueString>(Request.TargetRef), MakeShared<FJsonValueString>(TEXT("valid /Game/ asset package or object path")));
					return Result;
				}
				UStaticMesh* const Mesh = LoadObject<UStaticMesh>(
					nullptr, *Request.TargetRef);
				if (!Mesh)
				{
					Result.Diagnostic = WorldFailure(TEXT("modeling_material_asset_missing"), TEXT("The static mesh asset for material-slot removal could not be loaded."), Request.TargetRef, TEXT("/target_ref"), MakeShared<FJsonValueString>(Request.TargetRef), MakeShared<FJsonValueString>(TEXT("loadable StaticMesh")));
					return Result;
				}
				if (!Mesh->GetStaticMaterials().IsValidIndex(Request.Index))
				{
					auto Expected = MakeShared<FJsonObject>();
					Expected->SetNumberField(TEXT("minimum"), 0);
					Expected->SetNumberField(TEXT("exclusive_maximum"), Mesh->GetStaticMaterials().Num());
					Result.Diagnostic = WorldInputFailure(TEXT("modeling_material_index_invalid"),
						TEXT("The material-slot index is outside the static mesh material array."), Request.TargetRef, TEXT("index"),
						MakeShared<FJsonValueNumber>(Request.Index), MakeShared<FJsonValueObject>(Expected));
					return Result;
				}
				FAssetDurableMutation Durable;
				UE::UAI::Contract::FDiagnostic Error;
				if (!Durable.Prepare(
						Request, PackageName, OwnerId, JournalRoot,
						Counter.Increment(), Error))
				{
					Result.Diagnostic = MoveTemp(Error);
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
					FDiagnostic RollbackError;
					Durable.Rollback(false, RollbackError);
					Result.TerminalState = TEXT("state_unknown");
					Result.Diagnostic = MoveTemp(Error);
					if (!RollbackError.IsEmpty()) Result.Diagnostic.Diagnostics.Add(MoveTemp(RollbackError));
					return Result;
				}
				UObject* ReloadedObject = ReloadForIndependentReadback(PackageName, Error);
				UStaticMesh* const Reloaded = Cast<UStaticMesh>(ReloadedObject);
				Result.bReadbackVerified = Reloaded && Reloaded->GetStaticMaterials().Num() == ExpectedCount;
				Result.VerificationId = TEXT("modeling-material-readback:") + Request.RequestId;
				if (!Result.bReadbackVerified)
				{
					Result.TerminalState = TEXT("state_unknown");
					if (!ReloadedObject) Result.Diagnostic = MoveTemp(Error);
					else if (!Reloaded)
						Result.Diagnostic = WorldFailure(TEXT("modeling_material_readback_failed"), TEXT("Reloaded material-slot asset is not a StaticMesh."), Request.TargetRef, TEXT("/class"), MakeShared<FJsonValueString>(ReloadedObject->GetClass()->GetPathName()), MakeShared<FJsonValueString>(TEXT("StaticMesh")));
					else
						Result.Diagnostic = WorldFailure(TEXT("modeling_material_readback_failed"), TEXT("Persisted material-slot count does not match the expected removal result."), Request.TargetRef, TEXT("/materials"), MakeShared<FJsonValueNumber>(Reloaded->GetStaticMaterials().Num()), MakeShared<FJsonValueNumber>(ExpectedCount));
					return Result;
				}
				if (!Durable.Acknowledge(Result.VerificationId, Error))
				{
					Result.TerminalState = TEXT("state_unknown");
					Result.Diagnostic = MoveTemp(Error);
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
					const bool bSpaceInvalid = Request.Space != TEXT("component_local");
					const TCHAR* Axis = Request.BoundsMin.X > Request.BoundsMax.X ? TEXT("x")
						: Request.BoundsMin.Y > Request.BoundsMax.Y ? TEXT("y") : TEXT("z");
					const double Minimum = Axis[0] == TEXT('x') ? Request.BoundsMin.X : Axis[0] == TEXT('y') ? Request.BoundsMin.Y : Request.BoundsMin.Z;
					const double Maximum = Axis[0] == TEXT('x') ? Request.BoundsMax.X : Axis[0] == TEXT('y') ? Request.BoundsMax.Y : Request.BoundsMax.Z;
					Result.Diagnostic = WorldInputFailure(TEXT("modeling_voxel_space_or_bounds_invalid"),
						bSpaceInvalid ? TEXT("Voxel removal bounds must use component_local space.")
							: FString::Printf(TEXT("Minimum %s bound must not exceed maximum %s bound."), Axis, Axis),
						Request.TargetRef, bSpaceInvalid ? TEXT("space") : TEXT("min_") + FString(Axis),
						bSpaceInvalid ? TSharedPtr<FJsonValue>(MakeShared<FJsonValueString>(Request.Space)) : TSharedPtr<FJsonValue>(MakeShared<FJsonValueNumber>(Minimum)),
						bSpaceInvalid ? TSharedPtr<FJsonValue>(MakeShared<FJsonValueString>(TEXT("component_local"))) : TSharedPtr<FJsonValue>(MakeShared<FJsonValueNumber>(Maximum)));
					return Result;
				}
				AActor* const Actor = FindActor(*World, Request.TargetRef);
				UInstancedStaticMeshComponent* const Component = Actor
					? Cast<UInstancedStaticMeshComponent>(
						FindComponent(*Actor, Request.ComponentName)) : nullptr;
				if (!Component)
				{
					Result.Diagnostic = WorldFailure(TEXT("modeling_voxel_component_missing"), !Actor ? TEXT("The voxel removal actor was not found.") : TEXT("The requested component could not be resolved as an InstancedStaticMeshComponent."), Request.TargetRef, TEXT("/component_name"), MakeShared<FJsonValueString>(Request.ComponentName), MakeShared<FJsonValueString>(TEXT("InstancedStaticMeshComponent on the requested actor")));
					return Result;
				}
				const FBox Bounds(Request.BoundsMin, Request.BoundsMax);
				TArray<int32> Indices;
				for (int32 Index = 0; Index < Component->GetInstanceCount(); ++Index)
				{
					FTransform Transform;
					if (!Component->GetInstanceTransform(Index, Transform, false))
					{
						Result.Diagnostic = WorldFailure(TEXT("modeling_voxel_read_failed"), TEXT("The voxel instance transform could not be read in component-local space."), Request.TargetRef, TEXT("/instances"), MakeShared<FJsonValueBoolean>(false), MakeShared<FJsonValueBoolean>(true));
						Result.Diagnostic.OperationIndex = Index;
						Result.Diagnostic.CandidateName = Request.ComponentName;
						return Result;
					}
					if (Bounds.IsInsideOrOn(Transform.GetLocation()))
					{
						Indices.Add(Index);
					}
				}
				if (Indices.IsEmpty())
				{
					Result.Diagnostic = WorldFailure(TEXT("modeling_voxel_bounds_empty"), TEXT("No component instances were found inside the requested voxel removal bounds."), Request.TargetRef, TEXT("/bounds"), MakeShared<FJsonValueNumber>(Indices.Num()), MakeShared<FJsonValueString>(TEXT("at least one instance inside bounds")));
					Result.Diagnostic.CandidateName = Request.ComponentName;
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
						Result.Diagnostic = WorldFailure(TEXT("modeling_voxel_remove_failed"), TEXT("Native voxel instance removal failed."), Request.TargetRef, TEXT("/instances"), MakeShared<FJsonValueBoolean>(false), MakeShared<FJsonValueBoolean>(true));
						Result.Diagnostic.OperationIndex = Indices[Offset];
						Result.Diagnostic.CandidateName = Request.ComponentName;
						return Result;
					}
				}
				FDiagnostic ReadbackError;
				for (int32 Index = 0; Index < Component->GetInstanceCount(); ++Index)
				{
					FTransform Transform;
					const bool bReadable = Component->GetInstanceTransform(Index, Transform, false);
					const bool bInside = bReadable && Bounds.IsInsideOrOn(Transform.GetLocation());
					if ((!bReadable || bInside) && ReadbackError.IsEmpty())
					{
						auto Expected = MakeShared<FJsonObject>();
						Expected->SetStringField(TEXT("space"), TEXT("component_local"));
						Expected->SetStringField(TEXT("constraint"), TEXT("outside bounds"));
						Expected->SetField(TEXT("minimum"), WorldVectorValue(Request.BoundsMin));
						Expected->SetField(TEXT("maximum"), WorldVectorValue(Request.BoundsMax));
						ReadbackError = WorldFailure(TEXT("world_destructive_readback_failed"),
							bReadable ? TEXT("A voxel instance remains inside the removal bounds.") : TEXT("A remaining voxel instance transform could not be read."),
							Component->GetPathName(), TEXT("/instances/") + FString::FromInt(Index),
							bReadable ? WorldVectorValue(Transform.GetLocation()) : TSharedPtr<FJsonValue>(MakeShared<FJsonValueString>(TEXT("transform unreadable"))),
							bReadable ? TSharedPtr<FJsonValue>(MakeShared<FJsonValueObject>(Expected)) : TSharedPtr<FJsonValue>(MakeShared<FJsonValueString>(TEXT("readable instance transform"))));
					}
				}
				return CommitWorld(Request, {Actor->GetOutermost()}, true, MoveTemp(ReadbackError));
			}

			TWeakObjectPtr<UWorld> World;
			FString JournalRoot;
			FString OwnerId;
			TSharedRef<IUeAgentWorldDestructiveDurableRunner>
				DurableRunner;
			FThreadSafeCounter Counter;
		};
	}

	bool TryCreateWorldDestructiveOwner(
		UWorld& ExplicitWorld,
		FString JournalRoot,
		FString JournalOwnerId,
		TSharedRef<IUeAgentWorldDestructiveDurableRunner> DurableRunner,
		TSharedPtr<IUeAgentWorldDestructiveOwner>& OutOwner,
		UE::UAI::Contract::FDiagnostic& OutError)
	{
		OutOwner.Reset();
		OutError.Reset();
		if (!IsValid(&ExplicitWorld))
		{
			OutError = WorldFailure(TEXT("world_destructive_world_invalid"), TEXT("World destructive owner creation requires a live explicit world."), FString(), TEXT("/world"), MakeShared<FJsonValueBoolean>(false), MakeShared<FJsonValueBoolean>(true));
			return false;
		}
		JournalRoot.TrimStartAndEndInline();
		JournalOwnerId.TrimStartAndEndInline();
		if (JournalRoot.IsEmpty() || JournalOwnerId.IsEmpty())
		{
			auto Actual = MakeShared<FJsonObject>();
			Actual->SetBoolField(TEXT("journal_root_present"), !JournalRoot.IsEmpty());
			Actual->SetBoolField(TEXT("journal_owner_present"), !JournalOwnerId.IsEmpty());
			OutError = WorldFailure(TEXT("world_destructive_journal_config_invalid"), TEXT("World destructive owner requires a journal root and owner identity."), ExplicitWorld.GetPathName(), TEXT("/journal"), MakeShared<FJsonValueObject>(Actual), MakeShared<FJsonValueString>(TEXT("journal root and owner present")));
			return false;
		}
		OutOwner = MakeShared<FWorldDestructiveOwner>(
			ExplicitWorld,
			MoveTemp(JournalRoot),
			MoveTemp(JournalOwnerId),
			MoveTemp(DurableRunner));
		return true;
	}
}
