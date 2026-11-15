// Copyright Epic Games, Inc. All Rights Reserved.

#include "Domains/Blueprint/Authoring/UeAgentBlueprintAuthoringApplyProvider.h"

#include "AssetRegistry/AssetRegistryModule.h"
#include "Core/HjsonWorkspace/DomainAuthoring/UeAgentDomainHjsonAuthoringKit.h"
#include "Domains/Blueprint/Authoring/UeAgentBlueprintAuthoringOps.h"
#include "Domains/Blueprint/Authoring/UeAgentBlueprintAssetLifecycle.h"
#include "Domains/Blueprint/Authoring/UeAgentBlueprintAuthoringDescriptors.h"
#include "Domains/Blueprint/Authoring/UeAgentBlueprintAuthoringState.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Engine/Blueprint.h"
#include "GameFramework/Actor.h"
#include "Kismet2/KismetEditorUtilities.h"
#include "Misc/Guid.h"
#include "Misc/PackageName.h"
#include "ObjectTools.h"
#include "UObject/StrongObjectPtr.h"
#include "Utilities/AuthoringCore/UeAgentAuthoringAssetIdentity.h"
#include "Utilities/AuthoringCore/UeAgentAuthoringDiagnostics.h"
#include "Utilities/AuthoringOperationApply/UeAgentAuthoringAssetRemovalStage.h"
#include "Utilities/AuthoringOperationApply/UeAgentAuthoringObjectApplyProvider.h"
#include "Utilities/AuthoringOperationApply/UeAgentAuthoringOperationCoordinator.h"
#include "Utilities/AuthoringWorkspaceApply/UeAgentApplyWorkspaceContract.h"
#include "Utilities/AuthoringWorkspaceApply/UeAgentAuthoringAssetWorkspaceApply.h"
#include "Utilities/AuthoringWorkspaceApply/UeAgentAuthoringAssetWorkspaceInput.h"
#include "Utilities/AuthoringWorkspaceApply/UeAgentAuthoringWorkspaceDependencyGraph.h"
#include "Utilities/AuthoringWorkspaceApply/UeAgentAuthoringWorkspaceLoader.h"
#include "Utilities/AuthoringWorkspaceApply/UeAgentAuthoringWorkspaceRouter.h"
#include "Utilities/AuthoringWorkspaceApply/UeAgentAuthoringWorkspaceTransaction.h"

namespace UeAgentBlueprintAuthoring
{
	namespace
	{
		using namespace UeAgentApplyInputContract;
		using namespace UeAgentAuthoringOperationApply;
		using namespace UeAgentBlueprintAssetLifecycle;

		void SetFailure(FOperationFailure& OutFailure, FDiagnostic Diagnostic)
		{
			OutFailure = FOperationFailure();
			OutFailure.Diagnostic = MoveTemp(Diagnostic);
		}

		FDiagnostic AssetFailure(const FString& Target, const FString& Code, const FString& Reason,
			const FString& Field, TSharedPtr<FJsonValue> Actual, TSharedPtr<FJsonValue> Expected)
		{
			FDiagnostic Error = FDiagnostic::Failure(Code, Reason, TEXT("blueprint_apply"));
			Error.TargetPath = Target;
			Error.PropertyPath = Field;
			Error.ActualValue = MoveTemp(Actual);
			Error.ExpectedValue = MoveTemp(Expected);
			return Error;
		}

		FDiagnostic NativeAssetFailure(const FString& Target, const FString& Code, const FString& Reason,
			const FString& Field, TSharedPtr<FJsonValue> Actual, TSharedPtr<FJsonValue> Expected)
		{
			FDiagnostic Error = AssetFailure(Target, Code, Reason, Field, MoveTemp(Actual), MoveTemp(Expected));
			Error.SourceKind = TEXT("native_result");
			return Error;
		}

		void LocateInputFailure(FDiagnostic& Error, const FPrepareRequest& Request, const FString& Pointer)
		{
			Error.SemanticPointer = Pointer;
			if (Request.Unit) Error.Source = Request.Unit->ResolveDiagnosticSource(Pointer);
		}

		bool ReadBool(
			const TSharedPtr<const FJsonObject>& Object,
			const TCHAR* Field,
			const bool DefaultValue)
		{
			bool Value = DefaultValue;
			if (Object.IsValid()) Object->TryGetBoolField(Field, Value);
			return Value;
		}

		TSharedRef<FJsonObject> BuildMutationResultMetadata(const FMutationCounters& Counters)
		{
			TSharedRef<FJsonObject> Result = MakeShared<FJsonObject>();
#define UAI_SET_BLUEPRINT_COUNTER(Field, Value) Result->SetNumberField(TEXT(Field), Value)
			UAI_SET_BLUEPRINT_COUNTER("variables_added", Counters.VariablesAdded);
			UAI_SET_BLUEPRINT_COUNTER("variables_updated", Counters.VariablesUpdated);
			UAI_SET_BLUEPRINT_COUNTER("variables_removed", Counters.VariablesRemoved);
			UAI_SET_BLUEPRINT_COUNTER("delegates_added", Counters.DelegatesAdded);
			UAI_SET_BLUEPRINT_COUNTER("delegates_updated", Counters.DelegatesUpdated);
			UAI_SET_BLUEPRINT_COUNTER("delegates_removed", Counters.DelegatesRemoved);
			UAI_SET_BLUEPRINT_COUNTER("interfaces_added", Counters.InterfacesAdded);
			UAI_SET_BLUEPRINT_COUNTER("interfaces_updated", Counters.InterfacesUpdated);
			UAI_SET_BLUEPRINT_COUNTER("interfaces_removed", Counters.InterfacesRemoved);
			UAI_SET_BLUEPRINT_COUNTER("components_added", Counters.ComponentsAdded);
			UAI_SET_BLUEPRINT_COUNTER("components_updated", Counters.ComponentsUpdated);
			UAI_SET_BLUEPRINT_COUNTER("components_removed", Counters.ComponentsRemoved);
			UAI_SET_BLUEPRINT_COUNTER("graphs_added", Counters.GraphsAdded);
			UAI_SET_BLUEPRINT_COUNTER("graphs_updated", Counters.GraphsUpdated);
			UAI_SET_BLUEPRINT_COUNTER("graphs_removed", Counters.GraphsRemoved);
			UAI_SET_BLUEPRINT_COUNTER("nodes_added", Counters.NodesAdded);
			UAI_SET_BLUEPRINT_COUNTER("nodes_updated", Counters.NodesUpdated);
			UAI_SET_BLUEPRINT_COUNTER("nodes_removed", Counters.NodesRemoved);
			UAI_SET_BLUEPRINT_COUNTER("pin_groups_resized", Counters.PinGroupsResized);
			UAI_SET_BLUEPRINT_COUNTER("pin_defaults_applied", Counters.PinDefaultsApplied);
			UAI_SET_BLUEPRINT_COUNTER("edges_added", Counters.EdgesAdded);
			UAI_SET_BLUEPRINT_COUNTER("edges_removed", Counters.EdgesRemoved);
			UAI_SET_BLUEPRINT_COUNTER("class_defaults_applied", Counters.ClassDefaultsApplied);
#undef UAI_SET_BLUEPRINT_COUNTER
			return Result;
		}

		void ReadMutationResultMetadata(
			const TSharedPtr<FJsonObject>& Metadata,
			FMutationCounters& OutCounters)
		{
			OutCounters = FMutationCounters();
			if (!Metadata.IsValid()) return;
			auto Read = [&Metadata](const TCHAR* Field, int32& OutValue)
			{
				double Value = 0.0;
				if (Metadata->TryGetNumberField(Field, Value)) OutValue = FMath::RoundToInt(Value);
			};
#define UAI_READ_BLUEPRINT_COUNTER(Field, Value) Read(TEXT(Field), Value)
			UAI_READ_BLUEPRINT_COUNTER("variables_added", OutCounters.VariablesAdded);
			UAI_READ_BLUEPRINT_COUNTER("variables_updated", OutCounters.VariablesUpdated);
			UAI_READ_BLUEPRINT_COUNTER("variables_removed", OutCounters.VariablesRemoved);
			UAI_READ_BLUEPRINT_COUNTER("delegates_added", OutCounters.DelegatesAdded);
			UAI_READ_BLUEPRINT_COUNTER("delegates_updated", OutCounters.DelegatesUpdated);
			UAI_READ_BLUEPRINT_COUNTER("delegates_removed", OutCounters.DelegatesRemoved);
			UAI_READ_BLUEPRINT_COUNTER("interfaces_added", OutCounters.InterfacesAdded);
			UAI_READ_BLUEPRINT_COUNTER("interfaces_updated", OutCounters.InterfacesUpdated);
			UAI_READ_BLUEPRINT_COUNTER("interfaces_removed", OutCounters.InterfacesRemoved);
			UAI_READ_BLUEPRINT_COUNTER("components_added", OutCounters.ComponentsAdded);
			UAI_READ_BLUEPRINT_COUNTER("components_updated", OutCounters.ComponentsUpdated);
			UAI_READ_BLUEPRINT_COUNTER("components_removed", OutCounters.ComponentsRemoved);
			UAI_READ_BLUEPRINT_COUNTER("graphs_added", OutCounters.GraphsAdded);
			UAI_READ_BLUEPRINT_COUNTER("graphs_updated", OutCounters.GraphsUpdated);
			UAI_READ_BLUEPRINT_COUNTER("graphs_removed", OutCounters.GraphsRemoved);
			UAI_READ_BLUEPRINT_COUNTER("nodes_added", OutCounters.NodesAdded);
			UAI_READ_BLUEPRINT_COUNTER("nodes_updated", OutCounters.NodesUpdated);
			UAI_READ_BLUEPRINT_COUNTER("nodes_removed", OutCounters.NodesRemoved);
			UAI_READ_BLUEPRINT_COUNTER("pin_groups_resized", OutCounters.PinGroupsResized);
			UAI_READ_BLUEPRINT_COUNTER("pin_defaults_applied", OutCounters.PinDefaultsApplied);
			UAI_READ_BLUEPRINT_COUNTER("edges_added", OutCounters.EdgesAdded);
			UAI_READ_BLUEPRINT_COUNTER("edges_removed", OutCounters.EdgesRemoved);
			UAI_READ_BLUEPRINT_COUNTER("class_defaults_applied", OutCounters.ClassDefaultsApplied);
#undef UAI_READ_BLUEPRINT_COUNTER
		}

		class FAssetLifecycleState final : public IReflectionOperationLifecycleState
		{
		public:
			FAssetLifecycleState(
				FString InPackagePath,
				FString InAssetKind,
				EOperation InOperation,
				UBlueprint* InExisting,
				UBlueprint* InProposed,
				UBlueprint* InValidation,
				TUniquePtr<FScratchBlueprintPackages> InScratchPackages,
				TArray<UBlueprint*> InDependentBlueprints,
				const TArray<AActor*>& InAffectedActors,
				TUniquePtr<FPreparedBlueprintMutation> InMutation,
				FMutationCounters InPreparedCounters,
				bool bInCompile,
				bool bInSave)
				: PackagePath(MoveTemp(InPackagePath))
				, ScratchPackages(MoveTemp(InScratchPackages))
				, AssetKind(MoveTemp(InAssetKind))
				, Operation(InOperation)
				, Existing(InExisting)
				, Proposed(InProposed)
				, Validation(InValidation)
				, Mutation(MoveTemp(InMutation))
				, ActualCounters(MoveTemp(InPreparedCounters))
				, bCompile(bInCompile)
				, bSave(bInSave)
			{
				ComponentInstances.SetActors(InAffectedActors);
				for (UBlueprint* Dependent : InDependentBlueprints)
				{
					FDependentBlueprint& Entry = DependentBlueprints.AddDefaulted_GetRef();
					Entry.PackagePath = Dependent->GetOutermost()->GetName();
					Entry.AssetKind = GetAssetKind(Dependent);
					Entry.Target.Reset(Dependent);
				}
			}

			virtual FString GetTargetKey() const override { return PackagePath; }
			virtual EOperation GetResolvedOperation() const override { return Operation; }
			virtual FReflectionRoot GetProposedRoot() const override
			{
				return FReflectionRoot::MakeObject(
					Operation == EOperation::Update ? Existing.Get() : Proposed.Get());
			}
			virtual FReflectionRoot GetActualRoot() const override
			{
				return FReflectionRoot::MakeObject(Actual ? Actual : Existing.Get());
			}

			virtual void CollectRollbackObjects(TArray<UObject*>& OutObjects) const override
			{
				ComponentInstances.CollectObjects(OutObjects);
				for (const auto& Entry : DependentBlueprints)
					if (Entry.Target.IsValid()) OutObjects.AddUnique(Entry.Target.Get());
			}

			virtual bool CaptureRollback(FOperationFailure& OutFailure) override
			{
				OutFailure = FOperationFailure();
				FDiagnostic InstanceError;
				if (!ComponentInstances.Capture(InstanceError))
				{ SetFailure(OutFailure, MoveTemp(InstanceError)); return false; }
				if (RequiresCheckpointRollback()) return true;
				if (Operation == EOperation::Update)
				{
					UPackage* Package = ScratchPackages->Create(TEXT("Rollback"));
					Backup.Reset(DuplicateBlueprint(Existing.Get(), Package, NAME_None, true, &OutFailure.Diagnostic));
					if (!Backup.IsValid())
					{
						if (OutFailure.Diagnostic.IsEmpty()) SetFailure(OutFailure, NativeAssetFailure(PackagePath, TEXT("blueprint_rollback_capture_failed"),
							TEXT("The native editor could not duplicate the Blueprint for rollback."),
							TEXT("rollback_backup"), MakeShared<FJsonValueBoolean>(false), MakeShared<FJsonValueBoolean>(true)));
						return false;
					}
					for (auto& Entry : DependentBlueprints)
					{
						UPackage* DependentPackage = ScratchPackages->Create(TEXT("Rollback"));
						Entry.Backup.Reset(DuplicateBlueprint(Entry.Target.Get(), DependentPackage, NAME_None, true, &OutFailure.Diagnostic));
						if (!Entry.Backup.IsValid())
{
							if (OutFailure.Diagnostic.IsEmpty()) SetFailure(OutFailure, NativeAssetFailure(Entry.PackagePath, TEXT("blueprint_rollback_capture_failed"),
								TEXT("The native editor could not duplicate a dependent Blueprint for rollback."),
								TEXT("rollback_backup"), MakeShared<FJsonValueBoolean>(false), MakeShared<FJsonValueBoolean>(true)));
							return false;
						}
					}
					return true;
				}
				if (Operation != EOperation::Remove || !Existing.IsValid()) return true;
				FDiagnostic Error;
				if (!RemovalStage.Capture(Existing.Get(), Error))
				{
					SetFailure(OutFailure, MoveTemp(Error));
					return false;
				}
				return true;
			}

			virtual bool ApplyProposed(FOperationFailure& OutFailure) override
			{
				OutFailure = FOperationFailure();
				if (Operation != EOperation::Update || !Mutation.IsValid()) return true;
				FDiagnostic Error;
				if (!Mutation->ApplyTo(*Existing, ActualCounters, Error))
				{
					SetFailure(OutFailure, MoveTemp(Error));
					return false;
				}
				bMutationApplied = true;
				return true;
			}

			virtual bool ApplyLifecycle(FOperationFailure& OutFailure) override
			{
				OutFailure = FOperationFailure();
				if (Operation == EOperation::Remove)
				{
					FDiagnostic Error;
					if (!RemovalStage.Stage(Error))
					{
						SetFailure(OutFailure, MoveTemp(Error));
						return false;
					}
					Actual = nullptr;
					return true;
				}
				if (Operation == EOperation::Update)
				{
					Actual = Existing.Get();
					return true;
				}
				if (!Proposed.IsValid())
				{
					SetFailure(OutFailure, NativeAssetFailure(PackagePath, TEXT("blueprint_proposed_asset_missing"),
						TEXT("No prepared Blueprint is available for publishing."),
						TEXT("proposed_blueprint"), MakeShared<FJsonValueBoolean>(false), MakeShared<FJsonValueBoolean>(true)));
					return false;
				}
				UPackage* TargetPackage = CreatePackage(*PackagePath);
				Actual = PublishBlueprint(
					Proposed.Get(), TargetPackage, FPackageName::GetLongPackageAssetName(PackagePath), OutFailure.Diagnostic);
				if (!Actual)
				{
					return false;
				}
				FAssetRegistryModule::AssetCreated(Actual);
				bCreated = Operation == EOperation::Create;
				Actual->MarkPackageDirty();
				bMutationApplied = true;
				return true;
			}

			virtual bool Compile(FOperationFailure& OutFailure) override
			{
				OutFailure = FOperationFailure();
				if (Operation == EOperation::Remove) return true;
				UBlueprint* Target = Actual ? Actual : Existing.Get();
				FDiagnostic Error;
				if ((bCompile || (Mutation.IsValid() && Mutation->RequiresCompile()))
					&& !CompileBlueprint(Target, Error))
				{
					if (Error.TargetPath.IsEmpty()) Error.TargetPath = PackagePath;
					SetFailure(OutFailure, MoveTemp(Error));
					return false;
				}
				if (Mutation.IsValid())
				{
					FMutationCounters Deferred;
					if (!Mutation->FinalizeAfterCompile(*Target, Deferred, Error))
					{
						SetFailure(OutFailure, MoveTemp(Error));
						return false;
					}
					ActualCounters.Add(Deferred);
				}
				return ValidateBlueprintPersistence(*Target, OutFailure.Diagnostic);
			}

			virtual bool Save(FOperationFailure& OutFailure) override
			{
				OutFailure = FOperationFailure();
				if (!bSave || Operation == EOperation::Remove) return true;
				FDiagnostic Error;
				if (!UeAgentBlueprintAuthoringOps::SaveBlueprintPackage(Actual ? Actual : Existing.Get(), Error))
				{
					if (Error.TargetPath.IsEmpty()) Error.TargetPath = PackagePath;
					SetFailure(OutFailure, MoveTemp(Error));
					return false;
				}
				for (const auto& Entry : DependentBlueprints)
					if (!UeAgentBlueprintAuthoringOps::SaveBlueprintPackage(Entry.Target.Get(), Error))
						{ if (Error.TargetPath.IsEmpty()) Error.TargetPath = Entry.PackagePath; SetFailure(OutFailure, MoveTemp(Error)); return false; }
				return true;
			}

			virtual bool RequiresCheckpointRollback() const override { return (Operation == EOperation::Update || Operation == EOperation::Create) && bSave; }
			virtual bool SupportsPersistentReadback() const override { return RequiresCheckpointRollback(); }
			virtual bool ReleaseForPersistentReadback(FOperationFailure& OutFailure) override
			{
				OutFailure = FOperationFailure();
				Existing.Reset();
				ScratchPackages->Reset();
				Proposed.Reset();
				Validation.Reset();
				Backup.Reset();
				for (auto& Entry : DependentBlueprints) { Entry.Target.Reset(); Entry.Backup.Reset(); }
				Actual = nullptr;
				return true;
			}
			virtual bool RebindAfterPersistentReadback(FOperationFailure& OutFailure) override
			{
				OutFailure = FOperationFailure();
				Existing.Reset(LoadBlueprint(PackagePath));
				Actual = Existing.Get();
				if (!Actual)
				{
					SetFailure(OutFailure, NativeAssetFailure(PackagePath, TEXT("blueprint_persistent_reload_failed"),
						TEXT("The Blueprint could not be loaded after persistent readback."),
						TEXT("blueprint_loaded"), MakeShared<FJsonValueBoolean>(false), MakeShared<FJsonValueBoolean>(true)));
					return false;
				}
				for (auto& Entry : DependentBlueprints)
				{
					Entry.Target.Reset(LoadBlueprint(Entry.PackagePath));
					if (!Entry.Target.IsValid())
{
						SetFailure(OutFailure, NativeAssetFailure(Entry.PackagePath, TEXT("blueprint_persistent_reload_failed"),
							TEXT("A dependent Blueprint could not be loaded after persistent readback."),
							TEXT("blueprint_loaded"), MakeShared<FJsonValueBoolean>(false), MakeShared<FJsonValueBoolean>(true)));
						return false;
					}
				}
				return true;
			}
			virtual bool PrepareCheckpointRestore(FOperationFailure& OutFailure) override
			{
				if (Operation != EOperation::Create) return ReleaseForPersistentReadback(OutFailure);
				UBlueprint* Created = Actual ? Actual : LoadBlueprint(PackagePath);
				ReleaseForPersistentReadback(OutFailure);
				if (bCreated && Created)
				{
					const int32 DeletedCount = ObjectTools::DeleteObjects({Created}, false);
					if (DeletedCount != 1)
					{
						SetFailure(OutFailure, NativeAssetFailure(PackagePath, TEXT("blueprint_created_restore_delete_failed"),
							TEXT("Restoring the absent checkpoint did not delete exactly one created Blueprint."),
							TEXT("deleted_count"), MakeShared<FJsonValueNumber>(DeletedCount), MakeShared<FJsonValueNumber>(1)));
						return false;
					}
				}
				bCreated = false;
				return true;
			}
			virtual bool RebindAfterCheckpointRestore(FOperationFailure& OutFailure) override
			{
				if (Operation != EOperation::Create)
				{
					if (!RebindAfterPersistentReadback(OutFailure)) return false;
					FDiagnostic Error;
					if (!ComponentInstances.Restore(Error)) { SetFailure(OutFailure, MoveTemp(Error)); return false; }
					return true;
				}
				bool bPresent = false;
				const bool bPresenceKnown = UE::UAI::HjsonWorkspace::DomainAuthoring::TryInspectObjectTargetPresence(PackagePath, bPresent);
				if (!bPresenceKnown || bPresent)
				{
					SetFailure(OutFailure, NativeAssetFailure(PackagePath, TEXT("blueprint_absent_restore_unconfirmed"),
						!bPresenceKnown ? TEXT("The native query could not confirm whether the Blueprint is absent after checkpoint restore.")
							: TEXT("The Blueprint is still present after restoring its absent checkpoint."),
						!bPresenceKnown ? TEXT("presence_query_succeeded") : TEXT("asset_present"),
						MakeShared<FJsonValueBoolean>(bPresenceKnown && bPresent), MakeShared<FJsonValueBoolean>(!bPresenceKnown)));
					return false;
				}
				return true;
			}

			virtual bool BuildDurablePackagePlans(
				TArray<FPackageDurablePlan>& OutPlans,
				FOperationFailure& OutFailure) const override
			{
				UBlueprint* Target = Operation == EOperation::Remove ? Existing.Get()
					: (Operation == EOperation::Create ? Actual : (Existing.IsValid() ? Existing.Get() : Proposed.Get()));
				if (!BuildPackageDurablePlan(
					PackagePath,
					PackagePath,
					Target,
					Operation == EOperation::Remove ? EPackageDurableIntent::Delete
						: EPackageDurableIntent::Save,
					OutPlans,
					OutFailure, true, Operation == EOperation::Create ? EPackageBeforePresence::Absent : EPackageBeforePresence::Present,
					Proposed.IsValid() ? Proposed->GetClass() : nullptr)) return false;
				for (const auto& Entry : DependentBlueprints)
					if (!BuildPackageDurablePlan(Entry.PackagePath, Entry.PackagePath, Entry.Target.Get(),
						EPackageDurableIntent::Save, OutPlans, OutFailure, true, EPackageBeforePresence::Present)) return false;
				return true;
			}

			virtual bool BuildDomainBeforeReadRequests(TArray<UeAgentAuthoringState::FReadRequest>& OutRequests, FOperationFailure& OutFailure) const override
			{
				return Operation == EOperation::Create && bSave ? BuildReadRequests(nullptr, true, OutRequests, OutFailure)
					: BuildReadRequests(Operation == EOperation::Remove ? nullptr : (Actual ? Actual : Existing.Get()),
						Operation == EOperation::Remove, OutRequests, OutFailure);
			}

			virtual bool BuildDomainExpectedReadRequests(
				TArray<UeAgentAuthoringState::FReadRequest>& OutRequests,
				FOperationFailure& OutFailure) const override
			{
				return BuildReadRequests(
					Operation == EOperation::Remove
						? nullptr
						: (Operation == EOperation::Update ? Existing.Get() : Proposed.Get()),
					Operation == EOperation::Remove,
					OutRequests,
					OutFailure, true);
			}

			virtual bool BuildDomainActualReadRequests(
				TArray<UeAgentAuthoringState::FReadRequest>& OutRequests,
				FOperationFailure& OutFailure) const override
			{
				return BuildReadRequests(
					Operation == EOperation::Remove ? nullptr : (Actual ? Actual : Existing.Get()),
					Operation == EOperation::Remove || (Operation == EOperation::Create && bSave && !Actual && !Existing.IsValid()),
					OutRequests,
					OutFailure, bMutationApplied);
			}

			virtual TSharedPtr<FJsonObject> BuildResultMetadata() const override
			{
				return BuildMutationResultMetadata(ActualCounters);
			}

			virtual bool Commit(FOperationFailure& OutFailure) override
			{
				OutFailure = FOperationFailure();
				if (Operation == EOperation::Remove && RemovalStage.IsStaged())
				{
					FDiagnostic Error;
					if (!DeleteBlueprint(Cast<UBlueprint>(RemovalStage.GetAsset()), PackagePath, Error))
					{
						if (Error.TargetPath.IsEmpty()) Error.TargetPath = PackagePath;
						SetFailure(OutFailure, MoveTemp(Error));
						return false;
					}
					RemovalStage.Reset();
					Existing.Reset();
				}
				return true;
			}

			virtual bool Rollback(FOperationFailure& OutFailure) override
			{
				OutFailure = FOperationFailure();
				FDiagnostic Error;
				if (Operation == EOperation::Update && Backup.IsValid())
				{
					if (!FPreparedBlueprintMutation::RestoreFromBackup(*Backup, *Existing, Error))
					{
						SetFailure(OutFailure, MoveTemp(Error));
						return false;
					}
					Actual = Existing.Get();
					for (auto& Entry : DependentBlueprints)
						if (Entry.Backup.IsValid() && !FPreparedBlueprintMutation::RestoreFromBackup(*Entry.Backup, *Entry.Target, Error))
						{ SetFailure(OutFailure, MoveTemp(Error)); return false; }
					if (!ComponentInstances.Restore(Error)) { SetFailure(OutFailure, MoveTemp(Error)); return false; }
					return true;
				}
				if (Operation == EOperation::Create || Operation == EOperation::Clone)
				{
					if (Actual && !DeleteBlueprint(Actual, PackagePath, Error))
					{
						TArray<UObject*> Objects = { Actual };
						if (ObjectTools::DeleteObjectsUnchecked(Objects) <= 0)
						{
							if (Error.TargetPath.IsEmpty()) Error.TargetPath = PackagePath;
							SetFailure(OutFailure, MoveTemp(Error));
							return false;
						}
					}
					Actual = nullptr;
					return true;
				}
				if (Operation == EOperation::Remove && RemovalStage.IsCaptured())
				{
					if (!RemovalStage.Rollback(Error))
					{
						SetFailure(OutFailure, MoveTemp(Error));
						return false;
					}
					Actual = Existing.Get();
				}
				return true;
			}

		private:
			bool BuildReadRequests(
				UBlueprint* Root,
				const bool bExpectedAbsent,
				TArray<UeAgentAuthoringState::FReadRequest>& OutRequests,
				FOperationFailure& OutFailure, bool bVerifyIntent = false) const
			{
				OutFailure = FOperationFailure();
				FDiagnostic Error;
				if (bVerifyIntent && Root && !bExpectedAbsent && Mutation
					&& !Mutation->VerifyIntent(*Root, Error))
				{
					UeAgentAuthoringDiagnostics::ReportConsistencyWarning(TEXT("blueprint_authored_intent_failed"), Error, TEXT("blueprint_readback"));
					Error.Reset();
				}
				if (!BuildAssetSnapshotReadRequest(
					Root,
					PackagePath,
					AssetKind,
					bExpectedAbsent,
					OutRequests,
					Error))
				{
					SetFailure(OutFailure, MoveTemp(Error));
					return false;
				}
				if (Operation == EOperation::Create && bExpectedAbsent)
				{
					// Create before/restore reads observe absence; Remove expected reads describe a future state.
					OutRequests.Last().Context.Add(TEXT("observe_absence"), TEXT("true"));
				}
				if (Root && Operation == EOperation::Update && Mutation) {
					// The published workspace belongs to this target. Dependent package
					// verification is appended separately and does not change its scope.
					auto& Context = OutRequests.Last().Context;
					if (Mutation->ReparentsBlueprint()) Context.Add(ObservedReparentHierarchy, TEXT("true"));
					Mutation->AppendGraphObservationContext(bVerifyIntent, Context);
					if (!Mutation->GetObservedVariableDefaults().IsEmpty()) {
						Context.Add(ObservedMemberCapture, TEXT("defaults"));
						for (const auto& Member : Mutation->GetObservedVariableDefaults())
							Context.Add(ObservedDefaultMemberPrefix + Member.Key.ToString(EGuidFormats::DigitsWithHyphensLower), Member.Value.ToString());
					}
					Mutation->AppendMemberObservationContext(bVerifyIntent, Context);
					if (!Mutation->AppendComponentObservationContext(bVerifyIntent, Context, Error)) {
						SetFailure(OutFailure, MoveTemp(Error));
						return false;
					}
				}
				for (const auto& Entry : DependentBlueprints)
					if (!BuildAssetSnapshotReadRequest(Entry.Target.Get(), Entry.PackagePath, Entry.AssetKind,
						false, OutRequests, Error))
					{ SetFailure(OutFailure, MoveTemp(Error)); return false; }
				return true;
			}

			FString PackagePath;
			TUniquePtr<FScratchBlueprintPackages> ScratchPackages;
			struct FDependentBlueprint
			{
				FString PackagePath;
				FString AssetKind;
				TStrongObjectPtr<UBlueprint> Target;
				TStrongObjectPtr<UBlueprint> Backup;
			};
			TArray<FDependentBlueprint> DependentBlueprints;
			FComponentInstanceRollback ComponentInstances;
			FString AssetKind;
			bool bMutationApplied = false;
			EOperation Operation = EOperation::Update;
			TStrongObjectPtr<UBlueprint> Existing;
			TStrongObjectPtr<UBlueprint> Proposed;
			// Updates publish Existing, but their separate validation duplicate can
			// participate in native dependent compilation and must survive its GC.
			TStrongObjectPtr<UBlueprint> Validation;
			TStrongObjectPtr<UBlueprint> Backup;
			FAssetRemovalStage RemovalStage;
			UBlueprint* Actual = nullptr;
			TUniquePtr<FPreparedBlueprintMutation> Mutation;
			FMutationCounters ActualCounters;
			bool bCompile = true;
			bool bSave = false;
			bool bCreated = false;
		};

		class FAssetLifecycle final : public IReflectionOperationLifecycle
		{
		public:
			virtual bool PrepareLifecycle(
				const FPrepareRequest& Request,
				TUniquePtr<IReflectionOperationLifecycleState>& OutState,
				FOperationFailure& OutFailure) const override
			{
				OutState.Reset();
				OutFailure = FOperationFailure();
				if (!Request.IsValid() || !Request.Input->Target.IsValid())
				{
					FDiagnostic Error = AssetFailure(FString(), TEXT("blueprint_apply_request_invalid"),
						!Request.IsValid() ? TEXT("The Blueprint apply request is missing its contract, workspace unit, or parsed input.")
							: TEXT("The Blueprint apply input has no target object."),
						!Request.IsValid() ? TEXT("request") : TEXT("target"), MakeShared<FJsonValueBoolean>(false), MakeShared<FJsonValueBoolean>(true));
					auto Facts = MakeShared<FJsonObject>();
					Facts->SetBoolField(TEXT("contract_present"), Request.Contract != nullptr);
					Facts->SetBoolField(TEXT("unit_present"), Request.Unit != nullptr);
					Facts->SetBoolField(TEXT("input_present"), Request.Input != nullptr);
					Facts->SetBoolField(TEXT("target_present"), Request.Input && Request.Input->Target.IsValid());
					Error.ActualEvidence = MakeShared<FJsonValueObject>(Facts);
					LocateInputFailure(Error, Request, Request.IsValid() ? TEXT("/target") : TEXT("/"));
					SetFailure(OutFailure, MoveTemp(Error));
					return false;
				}

				FString PackagePath;
				Request.Input->Target->TryGetStringField(TEXT("asset_path"), PackagePath);
				PackagePath = UeAgentAuthoringAssetIdentity::NormalizePackagePath(PackagePath);
				if (!FPackageName::IsValidLongPackageName(PackagePath)
					|| FPackageName::GetLongPackageAssetName(PackagePath).IsEmpty()
					|| (!PackagePath.Equals(TEXT("/Game")) && !PackagePath.StartsWith(TEXT("/Game/"))))
				{
					const auto InputValue = Request.Input->Target->TryGetField(TEXT("asset_path"));
					FDiagnostic Error = AssetFailure(PackagePath, TEXT("invalid_asset_path"),
						TEXT("The asset path must normalize to a valid long package name with an asset name under /Game."),
						TEXT("asset_path"), InputValue.IsValid() ? InputValue : TSharedPtr<FJsonValue>(MakeShared<FJsonValueNull>()),
						MakeShared<FJsonValueString>(TEXT("a valid /Game package path with an asset name")));
					LocateInputFailure(Error, Request, TEXT("/target/asset_path"));
					SetFailure(OutFailure, MoveTemp(Error));
					return false;
				}

				UBlueprint* Existing = LoadBlueprint(PackagePath);
				EOperation Operation = Request.Input->RequestedOperation;
				if (Operation == EOperation::Upsert)
				{
					Operation = Existing ? EOperation::Update : EOperation::Create;
				}
				if ((Operation == EOperation::Create || Operation == EOperation::Clone) && Existing)
				{
					FDiagnostic Error = AssetFailure(PackagePath, TEXT("asset_already_exists"),
						TEXT("Create or clone requires an absent target, but a Blueprint already exists at this asset path."),
						TEXT("asset_present"), MakeShared<FJsonValueBoolean>(true), MakeShared<FJsonValueBoolean>(false));
					LocateInputFailure(Error, Request, TEXT("/target/asset_path"));
					SetFailure(OutFailure, MoveTemp(Error));
					return false;
				}
				if ((Operation == EOperation::Update || Operation == EOperation::Remove) && !Existing)
				{
					FDiagnostic Error = AssetFailure(PackagePath, TEXT("asset_not_found"),
						TEXT("Update or remove requires an existing Blueprint, but no Blueprint could be loaded at this asset path."),
						TEXT("blueprint_loaded"), MakeShared<FJsonValueBoolean>(false), MakeShared<FJsonValueBoolean>(true));
					LocateInputFailure(Error, Request, TEXT("/target/asset_path"));
					SetFailure(OutFailure, MoveTemp(Error));
					return false;
				}

				UBlueprint* Source = nullptr;
				if (Operation == EOperation::Clone)
				{
					FString SourcePath;
					Request.Input->Target->TryGetStringField(TEXT("source_asset_path"), SourcePath);
					Source = LoadBlueprint(SourcePath);
					if (!Source || GetAssetKind(Source).IsEmpty())
					{
						FDiagnostic Error = AssetFailure(PackagePath, TEXT("source_asset_not_found"),
							!Source ? TEXT("The clone source path does not load a Blueprint.") : TEXT("The loaded clone source is not a supported Blueprint asset kind."),
							TEXT("source_asset_path"), MakeShared<FJsonValueString>(SourcePath),
							MakeShared<FJsonValueString>(TEXT("a loadable Blueprint or Actor Blueprint asset")));
						LocateInputFailure(Error, Request, TEXT("/target/source_asset_path"));
						SetFailure(OutFailure, MoveTemp(Error));
						return false;
					}
				}

				UClass* ParentClass = Existing
					? Existing->ParentClass.Get()
					: (Source ? Source->ParentClass.Get() : AActor::StaticClass());
				FString ParentClassPath;
				if (Request.Input->ProviderExtension.IsValid()
					&& Request.Input->ProviderExtension->TryGetStringField(
						TEXT("parent_class"), ParentClassPath))
				{
					ParentClass = UeAgentBlueprintAuthoringOps::ResolveClassByPath(ParentClassPath);
					if (!ParentClass
						|| !FKismetEditorUtilities::CanCreateBlueprintOfClass(ParentClass))
					{
						FDiagnostic Error = AssetFailure(PackagePath, TEXT("blueprint_parent_class_invalid"),
							!ParentClass ? TEXT("The requested parent class could not be resolved.") : TEXT("The native editor does not allow Blueprint creation from the requested parent class."),
							TEXT("parent_class"), MakeShared<FJsonValueString>(ParentClassPath),
							MakeShared<FJsonValueString>(TEXT("a resolved class that permits Blueprint creation")));
						LocateInputFailure(Error, Request, TEXT("/provider_extension/parent_class"));
						SetFailure(OutFailure, MoveTemp(Error));
						return false;
					}
				}

				const FString ResolvedAssetKind = ParentClass
					&& ParentClass->IsChildOf(AActor::StaticClass())
					? TEXT("actor_blueprint")
					: TEXT("blueprint");
				FString RequestedAssetKind;
				Request.Input->Target->TryGetStringField(TEXT("asset_kind"), RequestedAssetKind);
				RequestedAssetKind.TrimStartAndEndInline();
				RequestedAssetKind.ToLowerInline();
				if (RequestedAssetKind.IsEmpty()) RequestedAssetKind = ResolvedAssetKind;
				if ((RequestedAssetKind != TEXT("blueprint")
						&& RequestedAssetKind != TEXT("actor_blueprint"))
					|| (RequestedAssetKind == TEXT("actor_blueprint")
						&& ResolvedAssetKind != TEXT("actor_blueprint")))
				{
					FDiagnostic Error = AssetFailure(PackagePath, TEXT("blueprint_asset_kind_conflict"),
						FString::Printf(TEXT("Requested asset kind '%s' is incompatible with parent class kind '%s'. Generic blueprint accepts either kind; actor_blueprint requires an Actor parent."),
							*RequestedAssetKind, *ResolvedAssetKind),
						TEXT("asset_kind"), MakeShared<FJsonValueString>(RequestedAssetKind),
						MakeShared<FJsonValueString>(ResolvedAssetKind == TEXT("actor_blueprint") ? TEXT("blueprint or actor_blueprint") : TEXT("blueprint")));
					LocateInputFailure(Error, Request, TEXT("/target/asset_kind"));
					SetFailure(OutFailure, MoveTemp(Error));
					return false;
				}

				FScratchBlueprintPreflightTransaction ScratchTransaction;
				FDiagnostic Error;
				if (Operation != EOperation::Remove && !ScratchTransaction.Begin(PackagePath, Error))
				{
					SetFailure(OutFailure, MoveTemp(Error));
					return false;
				}

				UBlueprint* Proposed = Existing;
				UBlueprint* ValidationTarget = nullptr;
				UPackage* ScratchPackage = nullptr;
				auto ScratchPackages = MakeUnique<FScratchBlueprintPackages>();
				if (Existing && Operation == EOperation::Update
					&& (!FPreparedBlueprintMutation::ValidateGraphNames(*Existing, Request.Input->ProviderExtension, Error)
						|| !FPreparedBlueprintMutation::ValidateMemberNames(*Existing, Request.Input->ProviderExtension, Error)
						|| !FPreparedBlueprintMutation::ValidateCustomEventNames(*Existing, Request.Input->ProviderExtension, Error)
						|| !FPreparedBlueprintMutation::ValidateRemovals(*Existing, Request.Input->ProviderExtension, Error)))
				{
					UeAgentAuthoringDiagnostics::LocateFailure(Error, *Request.Input->ProviderExtension,
						TEXT("blueprint_preflight"), PackagePath);
					SetFailure(OutFailure, MoveTemp(Error));
					return false;
				}
				if (Operation != EOperation::Remove)
				{
					ScratchPackage = ScratchPackages->Create(TEXT("Proposed"));
					if (Operation == EOperation::Create)
					{
						ValidationTarget = Cast<UBlueprint>(CreateFactorySeed(
							ResolvedAssetKind,
							ParentClass,
							ScratchPackage,
							NAME_None,
							RF_Transactional,
							Error));
						Proposed = ValidationTarget;
					}
					else if (Operation == EOperation::Clone)
					{
						ValidationTarget = DuplicateBlueprint(Source, ScratchPackage, NAME_None, false, &Error);
						Proposed = ValidationTarget;
					}
					else
					{
						ValidationTarget = DuplicateBlueprint(Existing, ScratchPackage, NAME_None, true, &Error);
					}
					if (!ValidationTarget)
					{
						if (Error.IsEmpty())
						{
							Error = NativeAssetFailure(PackagePath, TEXT("blueprint_proposed_asset_create_failed"),
								Operation == EOperation::Clone ? TEXT("The native editor could not duplicate the clone source as a proposed Blueprint.")
									: Operation == EOperation::Update ? TEXT("The native editor could not duplicate the existing Blueprint for validation.")
									: TEXT("The Blueprint factory did not return a Blueprint validation target."),
								TEXT("validation_blueprint"), MakeShared<FJsonValueBoolean>(false), MakeShared<FJsonValueString>(ResolvedAssetKind));
						}
						else if (Error.TargetPath.IsEmpty()) Error.TargetPath = PackagePath;
						SetFailure(OutFailure, MoveTemp(Error));
						return false;
					}
				}

				TStrongObjectPtr<UBlueprint> ValidationGuard(ValidationTarget);
				if (ValidationTarget)
					SetAuthoringContext(*ValidationTarget, PackagePath, Existing);
				TUniquePtr<FPreparedBlueprintMutation> Mutation;
				FMutationCounters ExpectedCounters;
				if (Operation != EOperation::Remove
					&& !FPreparedBlueprintMutation::Prepare(
						*ValidationTarget,
						Request.Input->ProviderExtension,
						Mutation,
						ExpectedCounters,
						Error))
				{
					UeAgentAuthoringDiagnostics::LocateFailure(Error, *Request.Input->ProviderExtension,
						TEXT("blueprint_preflight"), PackagePath);
					SetFailure(OutFailure, MoveTemp(Error));
					return false;
				}

				TArray<UBlueprint*> DependentBlueprints;
				TArray<AActor*> AffectedActors;
				if (Operation == EOperation::Update && Existing
					&& !FPreparedBlueprintMutation::CollectNativeDependentBlueprints(
						*Existing, Request.Input->ProviderExtension, DependentBlueprints, AffectedActors, Error))
				{ SetFailure(OutFailure, MoveTemp(Error)); return false; }
				OutState = MakeUnique<FAssetLifecycleState>(
					PackagePath,
					ResolvedAssetKind,
					Operation,
					Existing,
					Proposed,
					ValidationTarget,
					MoveTemp(ScratchPackages),
					MoveTemp(DependentBlueprints),
					AffectedActors,
					MoveTemp(Mutation),
					MoveTemp(ExpectedCounters),
					ReadBool(Request.Input->ProviderExtension, TEXT("compile"), true),
					ReadBool(Request.Input->ProviderExtension, TEXT("save"), false));
				return true;
			}
		};

		FInputFieldDescriptor Field(
			const TCHAR* Name,
			const TCHAR* Role,
			const TCHAR* TypeId,
			const TArray<EOperation>& RequiredFor = {})
		{
			FInputFieldDescriptor Result;
			Result.Name = Name;
			Result.Role = Role;
			Result.TypeId = TypeId;
			Result.RequiredFor = RequiredFor;
			return Result;
		}

		FOperationDescriptor OperationDescriptor(const EOperation Operation)
		{
			FOperationDescriptor Result;
			Result.Operation = Operation;
			Result.IdentityOneOf = { { TEXT("asset_path") } };
			Result.AcceptedSections = { TEXT("target"), TEXT("provider_extension") };
			if (Operation != EOperation::Remove) Result.AcceptedSections.Add(TEXT("properties"));
			if (Operation == EOperation::Create)
			{
				Result.OmittedFieldPolicy = TEXT("ue_native_default");
				Result.NativeDefaultPolicy = TEXT("required");
			}
			else if (Operation == EOperation::Update)
			{
				Result.OmittedFieldPolicy = TEXT("preserve_current");
				Result.NativeDefaultPolicy = TEXT("forbidden");
			}
			else if (Operation == EOperation::Clone || Operation == EOperation::Upsert)
			{
				Result.OmittedFieldPolicy = TEXT("ue_native_default");
				Result.NativeDefaultPolicy = TEXT("allowed");
			}
			else
			{
				Result.OmittedFieldPolicy = TEXT("not_applicable");
				Result.NativeDefaultPolicy = TEXT("not_applicable");
			}
			Result.bDestructive = Operation == EOperation::Remove;
			return Result;
		}

		FContract MakeInputContract()
		{
			FContract Result;
			Result.Schema = TEXT("uai.apply_input_contract.v1");
			Result.ContractId = AssetInputContractId;
			Result.ContractVersion = TEXT("1.0.0");
			Result.ProfileId = TEXT("blueprint.asset");
			Result.TargetKind = TEXT("blueprint_asset");
			Result.SemanticDescriptorRefs = { AssetSemanticId };
			Result.ReflectionBaseline.bEnabled = true;
			Result.ReflectionBaseline.RootKinds = { TEXT("uobject"), TEXT("ustruct") };
			Result.ReflectionBaseline.RecursiveNodes = {
				TEXT("scalar"), TEXT("struct"), TEXT("owned_object"), TEXT("reference"),
				TEXT("array"), TEXT("set"), TEXT("map"), TEXT("fixed_array"), TEXT("optional")
			};
			Result.ReflectionBaseline.UnknownFieldPolicy = TEXT("reject");
			Result.ReflectionBaseline.ExternalReferencePolicy = TEXT("replace_reference_only");
			Result.ReflectionBaseline.OwnedObjectPolicy =
				TEXT("create_or_locate_with_real_outer_then_recurse");
			Result.PropertyPayload.Mode = TEXT("reflection");
			Result.PropertyPayload.PropertyPathField = TEXT("property_path");
			Result.PropertyPayload.ValueField = TEXT("value");
			Result.PropertyPayload.OptionalAssertionFields = {
				TEXT("kind"), TEXT("struct_path"), TEXT("enum_path"), TEXT("element_type"),
				TEXT("key_type"), TEXT("mapped_type"), TEXT("inner_type"), TEXT("fixed_length"),
				TEXT("allowed_class")
			};
			Result.PropertyPayload.OmittedPatchFieldPolicy = TEXT("preserve_current");
			const TArray<EOperation> All = {
				EOperation::Create, EOperation::Update, EOperation::Upsert,
				EOperation::Remove, EOperation::Clone
			};
			Result.InputFields = {
				Field(TEXT("asset_path"), TEXT("identity"), TEXT("ue_asset_path"), All),
				Field(TEXT("asset_kind"), TEXT("type_assertion"), TEXT("blueprint_asset_kind"),
					{ EOperation::Create }),
				Field(TEXT("source_asset_path"), TEXT("selector"), TEXT("ue_asset_path"),
					{ EOperation::Clone }),
				Field(TEXT("operation"), TEXT("operation_control"), TEXT("apply_operation"), All),
				Field(TEXT("properties"), TEXT("intent"), TEXT("property_descriptor_list")),
				Field(TEXT("parent_class"), TEXT("provider_extension"), TEXT("ue_class_path")),
				Field(TEXT("interfaces"), TEXT("provider_extension"), TEXT("object_map")),
				Field(TEXT("class_defaults"), TEXT("provider_extension"), TEXT("object")),
				Field(TEXT("members"), TEXT("provider_extension"), TEXT("object")),
				Field(TEXT("components"), TEXT("provider_extension"), TEXT("object_map")),
				Field(TEXT("graphs"), TEXT("provider_extension"), TEXT("object_map")),
				Field(TEXT("compile"), TEXT("provider_extension"), TEXT("bool")),
				Field(TEXT("save"), TEXT("provider_extension"), TEXT("bool")),
			};
			for (const EOperation Item : All) Result.Operations.Add(OperationDescriptor(Item));
			Result.ProviderId = ApplyProviderId;
			Result.UnknownInputFieldPolicy = TEXT("reject");
			Result.Verification.StateSource = TEXT("shared_state_snapshot");
			Result.Verification.CompareSource = TEXT("shared_semantic_compare");
			Result.Verification.bRequired = true;
			return Result;
		}

		UeAgentApplyWorkspaceContract::FContract MakeWorkspaceContract()
		{
			using namespace UeAgentApplyWorkspaceContract;
			UeAgentApplyWorkspaceContract::FContract Result;
			Result.Schema = TEXT("uai.apply_workspace_contract.v1");
			Result.ContractId = AssetWorkspaceContractId;
			Result.ContractVersion = TEXT("1.0.0");
			Result.ProfileId = TEXT("blueprint.asset");
			Result.Sources = {
				{ ESourceKind::Directory, TEXT("physical"), TEXT("index") },
				{ ESourceKind::SingleFile, TEXT("virtual"), TEXT("object") },
				{ ESourceKind::Inline, TEXT("virtual"), TEXT("object") },
			};
			Result.Entry.DirectoryEntry = TEXT("index.hjson");
			Result.Entry.VirtualUnitId = TEXT("asset");
			FUnitDescriptor Unit;
			Unit.UnitId = TEXT("asset");
			Unit.PathPattern = TEXT("index.hjson");
			Unit.Role = TEXT("object");
			Unit.Cardinality = TEXT("one");
			Unit.bRequired = true;
			Unit.RouteId = TEXT("blueprint.asset");
			Unit.ApplyContractRef = AssetInputContractId;
			Unit.MergePolicy = TEXT("replace");
			Unit.AbsentSemantics = TEXT("error");
			Unit.EmptySemantics = TEXT("error");
			Result.Units = { Unit };
			Result.ReferencePolicy.Base = TEXT("workspace_root");
			Result.ReferencePolicy.RootContainment = TEXT("required");
			Result.ReferencePolicy.ExternalReferences = TEXT("reject");
			Result.ReferencePolicy.CaseCollision = TEXT("reject");
			Result.DependencyPolicy.EdgeSource = TEXT("contract_and_references");
			Result.DependencyPolicy.CyclePolicy = TEXT("reject");
			Result.DependencyPolicy.UnresolvedPolicy = TEXT("reject");
			Result.DependencyPolicy.Order = TEXT("stable_topological");
			Result.Preflight.Scope = TEXT("whole_workspace");
			Result.Preflight.MutationBeforeSuccess = TEXT("forbid");
			Result.Preflight.bDiagnosticsIncludeSource = true;
			Result.Transaction.Mode = TEXT("atomic");
			Result.Transaction.RollbackOn = {
				TEXT("mutation_failed"), TEXT("compile_failed"), TEXT("save_failed"),
				TEXT("verification_failed")
			};
			Result.Execution.Compile = TEXT("provider_hook");
			Result.Execution.Save = TEXT("caller_controlled");
			Result.Execution.Verification = TEXT("shared_snapshot_required");
			Result.UnknownFilePolicy = TEXT("reject");
			Result.DuplicateTargetPolicy = TEXT("reject");
			return Result;
		}

		TSharedRef<FJsonObject> MakeApplyDocument(const FAssetApplyRequest& Request)
		{
			TSharedRef<FJsonObject> Target = MakeShared<FJsonObject>();
			Target->SetStringField(TEXT("asset_path"), Request.AssetPath);
			if (!Request.AssetKind.IsEmpty())
			{
				Target->SetStringField(TEXT("asset_kind"), Request.AssetKind);
			}
			if (!Request.SourceAssetPath.IsEmpty())
			{
				Target->SetStringField(TEXT("source_asset_path"), Request.SourceAssetPath);
			}
			TSharedRef<FJsonObject> Document = MakeShared<FJsonObject>();
			Document->SetStringField(TEXT("operation"), OperationToString(Request.Operation));
			Document->SetObjectField(TEXT("target"), Target);
			if (!Request.Properties.IsEmpty()) Document->SetArrayField(TEXT("properties"), Request.Properties);
			if (Request.ProviderExtension.IsValid())
			{
				Document->SetObjectField(TEXT("provider_extension"), Request.ProviderExtension);
			}
			return Document;
		}

		bool ExecuteAssetWorkspaceSource(
			const UeAgentAuthoringWorkspaceLoader::FSourceRequest& Source,
			const FString& ExpectedAssetPath,
			FAssetApplyResult& OutResult,
			const FString& RequestId, bool bCompactValues = false,
			TFunction<bool(FOperationFailure&)> FinalReadback = {},
			const TSharedPtr<IDurablePackageStager>& PackageStager = nullptr)
		{
			OutResult = FAssetApplyResult();
			OutResult.Status = TEXT("failed");
			UeAgentAuthoringAssetWorkspaceInput::FRequest ResolveRequest;
			ResolveRequest.WorkspaceContractId = AssetWorkspaceContractId;
			ResolveRequest.Source = Source;
			ResolveRequest.ExpectedAssetPath = ExpectedAssetPath;
			ResolveRequest.Errors.ContractMissing = TEXT("blueprint_workspace_contract_missing");
			ResolveRequest.Errors.EntryInvalid = TEXT("blueprint_workspace_entry_invalid");
			ResolveRequest.Errors.AssetKindConflict =
				TEXT("blueprint_workspace_asset_kind_conflict");
			ResolveRequest.Errors.AssetPathConflict =
				TEXT("blueprint_workspace_asset_path_conflict");
			UeAgentAuthoringAssetWorkspaceInput::FResult Resolved;
			if (!UeAgentAuthoringAssetWorkspaceInput::Resolve(ResolveRequest, Resolved))
			{
				OutResult.Phase = Resolved.Phase;
				OutResult.Diagnostic = Resolved.Diagnostic;
				return false;
			}
			const UeAgentApplyWorkspaceContract::FContract& WorkspaceContract =
				Resolved.WorkspaceContract;
			const UeAgentAuthoringWorkspaceLoader::FLoadResult& LoadResult = Resolved.Load;
			const UeAgentApplyInputContract::FOperationInput& Input = Resolved.Input;

			FAssetApplyRequest Request;
			Request.Operation = Input.RequestedOperation;
			Request.AssetPath = Resolved.AssetPath;
			Request.SourceAssetPath = Resolved.SourceAssetPath;
			FString AssetKind = Resolved.AssetKind;
			AssetKind.TrimStartAndEndInline();
			AssetKind.ToLowerInline();
			if (!AssetKind.IsEmpty()
				&& AssetKind != TEXT("blueprint")
				&& AssetKind != TEXT("actor_blueprint"))
			{
				OutResult.Phase = TEXT("preflight");
				OutResult.Diagnostic = AssetFailure(Resolved.AssetPath, TEXT("blueprint_workspace_asset_kind_conflict"),
					TEXT("This workspace requires a Blueprint or Actor Blueprint asset kind."), TEXT("asset_kind"),
					MakeShared<FJsonValueString>(AssetKind), MakeShared<FJsonValueString>(TEXT("blueprint or actor_blueprint")));
				OutResult.Diagnostic.Stage = OutResult.Phase;
				OutResult.Diagnostic.SemanticPointer = TEXT("/target/asset_kind");
				if (const auto* Unit = LoadResult.Workspace.FindSingleUnit(WorkspaceContract.Entry.VirtualUnitId))
					OutResult.Diagnostic.Source = Unit->ResolveDiagnosticSource(TEXT("/target/asset_kind"));
				else if (Input.Target.IsValid())
					UeAgentAuthoringDiagnostics::LocateAuthoringSource(OutResult.Diagnostic, *Input.Target, TEXT("asset_kind"));
				return false;
			}
			const bool bExisted = LoadBlueprint(Request.AssetPath) != nullptr;
			FBackendOptions BackendOptions;
			BackendOptions.FinalReadback = MoveTemp(FinalReadback);
			if (PackageStager.IsValid()) BackendOptions.PackageStager = PackageStager;
			BackendOptions.bUseEditorTransaction = Request.Operation != EOperation::Remove;
			BackendOptions.TransactionDescription = NSLOCTEXT(
				"UeAgentInterface", "BlueprintAssetWorkspaceApply", "Apply Blueprint asset workspace");
			FApplyWorkspaceResult ApplyResult;
			UeAgentAuthoringAssetWorkspaceApply::FFailure ApplyFailure;
			FProviderRegistry LocalProviders;
			const FProviderRegistry* Providers = &FProviderRegistry::Get();
			if (bCompactValues)
			{
				UeAgentAuthoringOperationApply::ERegistrationResult Registered;
				UE::UAI::Contract::FDiagnostic RegistrationError;
				if (!LocalProviders.RegisterProvider(MakeApplyProvider(true), Registered, RegistrationError))
				{ OutResult.Phase = TEXT("preflight"); OutResult.Diagnostic = RegistrationError; return false; }
				Providers = &LocalProviders;
			}
			const bool bApplied = UeAgentAuthoringAssetWorkspaceApply::Execute(
				WorkspaceContract,
				LoadResult.Workspace,
				*Providers,
				RequestId,
				ReadBool(Input.ProviderExtension, TEXT("save"), false),
				BackendOptions,
				ApplyResult,
				ApplyFailure);
			OutResult.UnitsChecked = ApplyResult.Preflight.UnitsChecked;
			OutResult.MutationsApplied = ApplyResult.Execution.MutationsApplied;
			OutResult.bRollbackAttempted = ApplyResult.Execution.bRollbackAttempted;
			OutResult.bRollbackSucceeded = ApplyResult.Execution.bRollbackSucceeded;
			if (!bApplied)
			{
				OutResult.Phase = ApplyFailure.Phase;
				OutResult.Diagnostic = ApplyFailure.Diagnostic;
				return false;
			}

			OutResult.Status = TEXT("applied");
			OutResult.Phase = TEXT("complete");
			const EOperation ResolvedOperation = Request.Operation == EOperation::Upsert
				? (bExisted ? EOperation::Update : EOperation::Create)
				: Request.Operation;
			OutResult.ResolvedOperation = OperationToString(ResolvedOperation);
			OutResult.AssetPath = Request.AssetPath;
			OutResult.Asset = ResolvedOperation == EOperation::Remove
				? nullptr
				: LoadBlueprint(Request.AssetPath);
			OutResult.ObjectPath = OutResult.Asset
				? OutResult.Asset->GetPathName()
				: UeAgentAuthoringAssetIdentity::ToObjectPath(Request.AssetPath);
			OutResult.AssetKind = OutResult.Asset
				? GetAssetKind(OutResult.Asset)
				: AssetKind;
			const TSharedPtr<FJsonObject>* ResultMetadata =
				ApplyResult.UnitResultMetadata.Find(Resolved.CanonicalUnitKey);
			ReadMutationResultMetadata(
				ResultMetadata ? *ResultMetadata : nullptr,
				OutResult.Domain);
			return true;
		}
	}

	bool RegisterApplyContracts(UE::UAI::Contract::FDiagnostic& OutError)
	{
		UeAgentApplyInputContract::ERegistrationResult InputResult;
		if (!UeAgentApplyInputContract::FRegistry::Get().RegisterContract(
			MakeInputContract(), InputResult, OutError))
		{
			return false;
		}
		UeAgentApplyWorkspaceContract::ERegistrationResult WorkspaceResult;
		return UeAgentApplyWorkspaceContract::FRegistry::Get().RegisterContract(
			MakeWorkspaceContract(), WorkspaceResult, OutError);
	}

	TSharedRef<IApplyProvider> MakeApplyProvider(bool bCompactValues)
	{
		static const TSharedRef<IReflectionOperationLifecycle> Lifecycle = MakeShared<FAssetLifecycle>();
		UeAgentAuthoringObjectApply::FOptions Options;
		Options.bAllowDomainAuthorizedOwners = true;
		Options.bCompactValues = bCompactValues;
		return MakeShared<FReflectionApplyProvider>(ApplyProviderId, Lifecycle, Options);
	}

	bool ApplyAsset(const FAssetApplyRequest& Request, FAssetApplyResult& OutResult, const FString& RequestId)
	{
		return ExecuteAssetWorkspaceSource(
			UeAgentAuthoringWorkspaceLoader::FSourceRequest::MakeInlineObject(
				MakeApplyDocument(Request), TEXT("inline://blueprint-asset-apply")),
			Request.AssetPath,
			OutResult,
			RequestId, Request.bCompactValues, Request.FinalReadback, Request.PackageStager);
	}

	bool ApplyAssetWorkspace(
		const FString& WorkspaceRoot,
		const FString& ExpectedAssetPath,
		FAssetApplyResult& OutResult,
		const FString& RequestId)
	{
		return ExecuteAssetWorkspaceSource(
			UeAgentAuthoringWorkspaceLoader::FSourceRequest::MakeDirectory(WorkspaceRoot),
			ExpectedAssetPath,
			OutResult,
			RequestId);
	}

	bool ApplyAssetInlineWorkspace(
		const TSharedPtr<const FJsonObject>& Document,
		const FString& ExpectedAssetPath,
		FAssetApplyResult& OutResult,
		const FString& RequestId,
		TFunction<bool(FOperationFailure&)> FinalReadback)
	{
		return ExecuteAssetWorkspaceSource(
			UeAgentAuthoringWorkspaceLoader::FSourceRequest::MakeInlineObject(
				Document, TEXT("inline://blueprint-surface-domain")),
			ExpectedAssetPath,
			OutResult,
			RequestId, false, MoveTemp(FinalReadback));
	}
}
