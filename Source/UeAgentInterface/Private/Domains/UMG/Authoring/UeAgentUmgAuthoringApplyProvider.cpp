// Copyright Epic Games, Inc. All Rights Reserved.

#include "Domains/UMG/Authoring/UeAgentUmgAuthoringApplyProvider.h"

#include "AssetRegistry/AssetRegistryModule.h"
#include "Blueprint/UserWidget.h"
#include "Core/HjsonWorkspace/DomainAuthoring/UeAgentDomainHjsonAuthoringKit.h"
#include "Domains/Blueprint/Authoring/UeAgentBlueprintAssetLifecycle.h"
#include "Domains/UMG/Authoring/UeAgentUmgAuthoringDescriptors.h"
#include "Domains/UMG/Authoring/UeAgentUmgAuthoringState.h"
#include "Domains/UMG/Authoring/UeAgentUmgAuthoringUtilities.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Kismet2/KismetEditorUtilities.h"
#include "Misc/PackageName.h"
#include "ObjectTools.h"
#include "UObject/StrongObjectPtr.h"
#include "Utilities/AuthoringOperationApply/UeAgentAuthoringAssetRemovalStage.h"
#include "Utilities/AuthoringOperationApply/UeAgentAuthoringObjectApplyProvider.h"
#include "Utilities/AuthoringOperationApply/UeAgentAuthoringOperationCoordinator.h"
#include "Utilities/AuthoringCore/UeAgentAuthoringAssetIdentity.h"
#include "Utilities/AuthoringCore/UeAgentAuthoringDiagnostics.h"
#include "Utilities/AuthoringWorkspaceApply/UeAgentApplyWorkspaceContract.h"
#include "Utilities/AuthoringWorkspaceApply/UeAgentAuthoringAssetWorkspaceApply.h"
#include "Utilities/AuthoringWorkspaceApply/UeAgentAuthoringAssetWorkspaceInput.h"
#include "Utilities/AuthoringWorkspaceApply/UeAgentAuthoringWorkspaceDependencyGraph.h"
#include "Utilities/AuthoringWorkspaceApply/UeAgentAuthoringWorkspaceLoader.h"
#include "Utilities/AuthoringWorkspaceApply/UeAgentAuthoringWorkspaceTransaction.h"
#include "Utilities/AuthoringWorkspaceApply/UeAgentAuthoringWorkspaceRouter.h"
#include "WidgetBlueprint.h"

namespace UeAgentUmgAuthoring
{
	namespace
	{
		using namespace UeAgentApplyInputContract;
		using namespace UeAgentAuthoringOperationApply;

		void SetFailure(FOperationFailure& OutFailure, const FString& Code,
    const FString& Reason, const FString& TargetPath = FString()) {
  OutFailure.Diagnostic = UeAgentAuthoringDiagnostics::FDiagnostic::Failure(Code, Reason, TEXT("umg_apply"));
  OutFailure.Diagnostic.TargetPath = TargetPath;
}

void SetFailure(FOperationFailure& OutFailure, const UE::UAI::Contract::FDiagnostic& Diagnostic) {
  OutFailure.Diagnostic = Diagnostic;
}


void SetFailure(FOperationFailure& OutFailure, const FString& FallbackCode,
    const FString& FallbackReason, UeAgentAuthoringDiagnostics::FDiagnostic Diagnostic) {
  OutFailure.Diagnostic = Diagnostic.IsEmpty()
      ? UeAgentAuthoringDiagnostics::FDiagnostic::Failure(FallbackCode, FallbackReason, TEXT("umg_apply"))
      : MoveTemp(Diagnostic);
}

		UWidgetBlueprint* LoadWidgetBlueprint(const FString& InPath)
		{
			return Cast<UWidgetBlueprint>(
				UeAgentBlueprintAssetLifecycle::LoadBlueprint(InPath));
		}

		void SetMutationFailure(FOperationFailure& OutFailure,
    UeAgentAuthoringDiagnostics::FDiagnostic Diagnostic) {
  OutFailure.Diagnostic = MoveTemp(Diagnostic);
}

		void LocateMemberNameFailure(FOperationFailure& Failure, const FPrepareRequest& Request)
		{
			if (!Request.Unit || !Request.Input || Request.Unit->SourceLocations.IsEmpty()) return;
			auto& Diagnostic = Failure.Diagnostic;
			if (!Diagnostic.Source.SourceFile.IsEmpty()) return;
			const bool bAnimation = Diagnostic.Code == TEXT("umg_animation_name_invalid");
			if ((!bAnimation && Diagnostic.Code != TEXT("umg_widget_name_invalid"))
				|| Diagnostic.CandidateName.IsEmpty()) return;
			const auto Extension = Request.Input->ProviderExtension;
			if (!Extension) return;
			const TSharedPtr<FJsonObject>* Collection = nullptr;
			FString Prefix = TEXT("/provider_extension/");
			if (bAnimation)
			{
				if (!Extension->TryGetObjectField(TEXT("animations"), Collection)) return;
				Prefix += TEXT("animations/");
			}
			else
			{
				const TSharedPtr<FJsonObject>* Tree = nullptr;
				if (!Extension->TryGetObjectField(TEXT("widget_tree"), Tree)
					|| !(*Tree)->TryGetObjectField(TEXT("widgets"), Collection)) return;
				Prefix += TEXT("widget_tree/widgets/");
			}
			for (const auto& Row : (*Collection)->Values)
			{
				if (!Row.Value || Row.Value->Type != EJson::Object) continue;
				FString Name;
				const bool bExplicitName = Row.Value->AsObject()->TryGetStringField(TEXT("name"), Name)
					&& !Name.IsEmpty();
				if (!bExplicitName) Name = Row.Key;
				if (Name != Diagnostic.CandidateName) continue;
				FString Pointer = Prefix + Row.Key.Replace(TEXT("~"), TEXT("~0")).Replace(TEXT("/"), TEXT("~1"));
				if (bExplicitName) Pointer += TEXT("/name");
				const auto* Source = Request.Unit->FindSourceLocation(Pointer);
				if (!Source && bExplicitName)
				{
					Pointer.RemoveFromEnd(TEXT("/name"));
					Source = Request.Unit->FindSourceLocation(Pointer);
				}
				if (!Source || Source->FilePath.StartsWith(TEXT("inline://"))) return;
				Diagnostic.Source.SourceFile = Source->FilePath;
				Diagnostic.Source.JsonPath = Pointer;
				Diagnostic.Source.Line = Source->Line;
				Diagnostic.Source.Column = Source->Column;
				return;
			}
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
#define UAI_SET_UMG_COUNTER(Field, Value) Result->SetNumberField(TEXT(Field), Value)
			UAI_SET_UMG_COUNTER("widgets_added", Counters.WidgetsAdded);
			UAI_SET_UMG_COUNTER("widgets_updated", Counters.WidgetsUpdated);
			UAI_SET_UMG_COUNTER("widgets_removed", Counters.WidgetsRemoved);
			UAI_SET_UMG_COUNTER("widget_properties_applied", Counters.WidgetPropertiesApplied);
			UAI_SET_UMG_COUNTER("slot_properties_applied", Counters.SlotPropertiesApplied);
			UAI_SET_UMG_COUNTER("bindings_added", Counters.BindingsAdded);
			UAI_SET_UMG_COUNTER("bindings_updated", Counters.BindingsUpdated);
			UAI_SET_UMG_COUNTER("bindings_removed", Counters.BindingsRemoved);
			UAI_SET_UMG_COUNTER("animations_added", Counters.AnimationsAdded);
			UAI_SET_UMG_COUNTER("animations_updated", Counters.AnimationsUpdated);
			UAI_SET_UMG_COUNTER("animations_removed", Counters.AnimationsRemoved);
			UAI_SET_UMG_COUNTER("animation_keys_applied", Counters.AnimationKeysApplied);
			UAI_SET_UMG_COUNTER("variables_added", Counters.Blueprint.VariablesAdded);
			UAI_SET_UMG_COUNTER("variables_updated", Counters.Blueprint.VariablesUpdated);
			UAI_SET_UMG_COUNTER("variables_removed", Counters.Blueprint.VariablesRemoved);
			UAI_SET_UMG_COUNTER("graphs_added", Counters.Blueprint.GraphsAdded);
			UAI_SET_UMG_COUNTER("graphs_updated", Counters.Blueprint.GraphsUpdated);
			UAI_SET_UMG_COUNTER("graphs_removed", Counters.Blueprint.GraphsRemoved);
#undef UAI_SET_UMG_COUNTER
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
			Read(TEXT("widgets_added"), OutCounters.WidgetsAdded);
			Read(TEXT("widgets_updated"), OutCounters.WidgetsUpdated);
			Read(TEXT("widgets_removed"), OutCounters.WidgetsRemoved);
			Read(TEXT("widget_properties_applied"), OutCounters.WidgetPropertiesApplied);
			Read(TEXT("slot_properties_applied"), OutCounters.SlotPropertiesApplied);
			Read(TEXT("bindings_added"), OutCounters.BindingsAdded);
			Read(TEXT("bindings_updated"), OutCounters.BindingsUpdated);
			Read(TEXT("bindings_removed"), OutCounters.BindingsRemoved);
			Read(TEXT("animations_added"), OutCounters.AnimationsAdded);
			Read(TEXT("animations_updated"), OutCounters.AnimationsUpdated);
			Read(TEXT("animations_removed"), OutCounters.AnimationsRemoved);
			Read(TEXT("animation_keys_applied"), OutCounters.AnimationKeysApplied);
			Read(TEXT("variables_added"), OutCounters.Blueprint.VariablesAdded);
			Read(TEXT("variables_updated"), OutCounters.Blueprint.VariablesUpdated);
			Read(TEXT("variables_removed"), OutCounters.Blueprint.VariablesRemoved);
			Read(TEXT("graphs_added"), OutCounters.Blueprint.GraphsAdded);
			Read(TEXT("graphs_updated"), OutCounters.Blueprint.GraphsUpdated);
			Read(TEXT("graphs_removed"), OutCounters.Blueprint.GraphsRemoved);
		}

		class FAssetLifecycleState final : public IReflectionOperationLifecycleState
		{
		public:
			FAssetLifecycleState(
				FString InPackagePath,
				EOperation InOperation,
				UWidgetBlueprint* InExisting,
				UWidgetBlueprint* InProposed,
				TUniquePtr<FPreparedUmgMutation> InMutation,
				FMutationCounters InPreparedCounters,
				const bool bInCompile,
				const bool bInSave)
				: PackagePath(MoveTemp(InPackagePath))
				, Operation(InOperation)
				, Existing(InExisting)
				, Proposed(InProposed)
				, Mutation(MoveTemp(InMutation))
				, ActualCounters(MoveTemp(InPreparedCounters))
				, bCompile(bInCompile)
				, bSave(bInSave)
			{
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

			virtual bool CaptureRollback(FOperationFailure& OutFailure) override
			{
				OutFailure = FOperationFailure();
				if (Operation == EOperation::Update)
				{
					UPackage* Package = CreatePackage(*FString::Printf(
						TEXT("/Temp/UAI/UMG/Rollback_%s"),
						*FGuid::NewGuid().ToString(EGuidFormats::Digits)));
					Backup.Reset(Cast<UWidgetBlueprint>(
						UeAgentBlueprintAssetLifecycle::DuplicateBlueprint(Existing.Get(), Package, NAME_None, true, &OutFailure.Diagnostic)));
					if (!Backup.IsValid())
					{
						if (OutFailure.Diagnostic.IsEmpty())
							SetFailure(OutFailure, TEXT("umg_rollback_capture_failed"), TEXT("The existing widget Blueprint could not be duplicated for rollback."), PackagePath);
						return false;
					}
					return true;
				}
				if (Operation != EOperation::Remove || !Existing.IsValid()) return true;
				UE::UAI::Contract::FDiagnostic Error;
				if (!RemovalStage.Capture(Existing.Get(), Error))
				{
					SetFailure(OutFailure, TEXT("umg_remove_capture_failed"), TEXT("The widget Blueprint removal state could not be captured."), Error);
					return false;
				}
				return true;
			}

			virtual bool ApplyProposed(FOperationFailure& OutFailure) override
			{
				OutFailure = FOperationFailure();
				if (Operation != EOperation::Update || !Mutation.IsValid()) return true;
				UE::UAI::Contract::FDiagnostic Error;
				if (!Mutation->ApplyTo(*Existing, ActualCounters, Error))
				{
					SetMutationFailure(OutFailure, Error);
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
					UE::UAI::Contract::FDiagnostic Error;
					if (!RemovalStage.Stage(Error))
					{
						SetFailure(OutFailure, TEXT("umg_remove_stage_failed"), TEXT("The widget Blueprint could not be staged for removal."), Error);
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
					SetFailure(OutFailure, TEXT("umg_proposed_asset_missing"), TEXT("The prepared widget Blueprint is no longer available for publishing."), PackagePath);
					return false;
				}
				UPackage* TargetPackage = CreatePackage(*PackagePath);
				Actual = Cast<UWidgetBlueprint>(UeAgentBlueprintAssetLifecycle::PublishBlueprint(
					Proposed.Get(), TargetPackage, FPackageName::GetLongPackageAssetName(PackagePath), OutFailure.Diagnostic));
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
				UWidgetBlueprint* Target = Actual ? Actual : Existing.Get();
				UE::UAI::Contract::FDiagnostic Error;
				if ((bCompile || (Mutation.IsValid() && Mutation->RequiresCompile())))
				{
					UeAgentUmgAuthoringUtilities::SyncWidgetVariableGuidMap(Target);
					if (!UeAgentBlueprintAssetLifecycle::CompileBlueprint(Target, Error))
					{
						SetFailure(OutFailure, TEXT("umg_compile_failed_after_apply"), TEXT("The widget Blueprint failed to compile after applying the mutation."), Error);
						return false;
					}
				}
				if (Mutation.IsValid())
				{
					FMutationCounters Deferred;
					bool bSecondCompile = false;
					if (!Mutation->FinalizeAfterCompile(*Target, Deferred, bSecondCompile, Error))
					{
						SetFailure(OutFailure, TEXT("umg_finalize_failed"), TEXT("The widget Blueprint mutation could not be finalized after compilation."), Error);
						return false;
					}
					ActualCounters.Add(Deferred);
					if (bSecondCompile)
					{
						UeAgentUmgAuthoringUtilities::SyncWidgetVariableGuidMap(Target);
						if (!UeAgentBlueprintAssetLifecycle::CompileBlueprint(Target, Error))
						{
							SetFailure(OutFailure, TEXT("umg_binding_compile_failed"), TEXT("The widget Blueprint failed to compile after applying bindings."), Error);
							return false;
						}
					}
					if (!Mutation->VerifyIntent(*Target, Error))
					{
						UeAgentAuthoringDiagnostics::ReportConsistencyWarning(TEXT("umg_authored_intent_failed"), Error, TEXT("umg_intent_observation"));
					}
				}
				return UeAgentBlueprintAssetLifecycle::ValidateBlueprintPersistence(*Target, OutFailure.Diagnostic);
			}

			virtual bool Save(FOperationFailure& OutFailure) override
			{
				OutFailure = FOperationFailure();
				if (!bSave || Operation == EOperation::Remove) return true;
				UE::UAI::Contract::FDiagnostic Error;
				if (!UeAgentUmgAuthoringUtilities::SaveWidgetBlueprintPackage(
					Actual ? Actual : Existing.Get(), Error))
				{
					SetFailure(OutFailure, TEXT("umg_save_failed_after_apply"), TEXT("The updated widget Blueprint package could not be saved."), Error);
					return false;
				}
				return true;
			}

			virtual bool BuildDurablePackagePlans(
				TArray<FPackageDurablePlan>& OutPlans,
				FOperationFailure& OutFailure) const override
			{
				return BuildPackageDurablePlan(
					PackagePath,
					PackagePath,
					Operation == EOperation::Remove ? Existing.Get()
						: (Operation == EOperation::Create ? Actual : (Existing.IsValid() ? Existing.Get() : Proposed.Get())),
					Operation == EOperation::Remove ? EPackageDurableIntent::Delete
						: EPackageDurableIntent::Save,
					OutPlans,
					OutFailure, true, Operation == EOperation::Create ? EPackageBeforePresence::Absent : EPackageBeforePresence::Present,
					Proposed.IsValid() ? Proposed->GetClass() : nullptr);
			}

			virtual bool BuildDomainBeforeReadRequests(
				TArray<UeAgentAuthoringState::FReadRequest>& OutRequests,
				FOperationFailure& OutFailure) const override
			{
				return Operation == EOperation::Create && bSave
					? BuildReadRequests(nullptr, true, OutRequests, OutFailure)
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
						: Proposed.Get(),
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

			virtual bool SupportsPersistentReadback() const override
			{
				return (Operation == EOperation::Update || Operation == EOperation::Create) && bSave;
			}

			virtual bool RequiresCheckpointRollback() const override
			{
				return Operation == EOperation::Create && bSave;
			}

			virtual bool ReleaseForPersistentReadback(FOperationFailure& OutFailure) override
			{
				OutFailure = FOperationFailure();
				// The shared transaction owns the before-package checkpoint after unload.
				// Keep frozen request values/identities, which rebind to the loaded owner.
				// The mutation holds no strong native graph or property-storage pointers.
				Existing.Reset();
				Proposed.Reset();
				Backup.Reset();
				Actual = nullptr;
				return true;
			}

			virtual bool RebindAfterPersistentReadback(FOperationFailure& OutFailure) override
			{
				OutFailure = FOperationFailure();
				Existing.Reset(LoadWidgetBlueprint(PackagePath));
				Actual = Existing.Get();
				if (!Actual)
				{
					SetFailure(OutFailure, TEXT("umg_persistent_rebind_failed"), TEXT("The published widget Blueprint could not be loaded from its target package."), PackagePath);
					return false;
				}
				return true;
			}

			virtual bool PrepareCheckpointRestore(FOperationFailure& OutFailure) override
			{
				bMutationApplied = false;
				if (Operation != EOperation::Create) return ReleaseForPersistentReadback(OutFailure);
				UWidgetBlueprint* Created = Actual ? Actual : LoadWidgetBlueprint(PackagePath);
				ReleaseForPersistentReadback(OutFailure);
				if (bCreated && Created && ObjectTools::DeleteObjects({Created}, false) != 1)
				{
					SetFailure(OutFailure, TEXT("umg_created_restore_delete_failed"), TEXT("The newly created widget Blueprint could not be removed during rollback."), PackagePath);
					return false;
				}
				bCreated = false;
				return true;
			}

			virtual bool RebindAfterCheckpointRestore(FOperationFailure& OutFailure) override
			{
				if (Operation != EOperation::Create) return RebindAfterPersistentReadback(OutFailure);
				bool bPresent = false;
				if (!UE::UAI::HjsonWorkspace::DomainAuthoring::TryInspectObjectTargetPresence(PackagePath, bPresent) || bPresent)
				{
					SetFailure(OutFailure, TEXT("umg_absent_restore_unconfirmed"), TEXT("Rollback could not confirm that the target widget Blueprint is absent."), PackagePath);
					return false;
				}
				return true;
			}

			virtual bool Commit(FOperationFailure& OutFailure) override
			{
				OutFailure = FOperationFailure();
				if (Operation == EOperation::Remove && RemovalStage.IsStaged())
				{
					UE::UAI::Contract::FDiagnostic Error;
					if (!UeAgentBlueprintAssetLifecycle::DeleteBlueprint(
						Cast<UWidgetBlueprint>(RemovalStage.GetAsset()), PackagePath, Error))
					{
						SetFailure(OutFailure, TEXT("umg_remove_commit_failed"), TEXT("The staged widget Blueprint removal could not be committed."), Error);
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
				bMutationApplied = false;
				UE::UAI::Contract::FDiagnostic Error;
				if (Operation == EOperation::Update && Backup.IsValid())
				{
					if (!FPreparedUmgMutation::RestoreFromBackup(*Backup, *Existing, Error))
					{
						SetFailure(OutFailure, TEXT("umg_update_rollback_failed"), TEXT("The previous widget Blueprint state could not be restored."), Error);
						return false;
					}
					Actual = Existing.Get();
					return true;
				}
				if (Operation == EOperation::Create || Operation == EOperation::Clone)
				{
					if (Actual && !UeAgentBlueprintAssetLifecycle::DeleteBlueprint(Actual, PackagePath, Error))
					{
						TArray<UObject*> Objects = { Actual };
						if (ObjectTools::DeleteObjectsUnchecked(Objects) <= 0)
						{
							SetFailure(OutFailure, TEXT("umg_create_rollback_failed"), TEXT("The created widget Blueprint could not be rolled back."), Error);
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
						SetFailure(OutFailure, TEXT("umg_remove_rollback_failed"), TEXT("The removed widget Blueprint could not be restored."), Error);
						return false;
					}
					Actual = Existing.Get();
				}
				return true;
			}

		private:
			bool BuildReadRequests(
				UWidgetBlueprint* Root,
				const bool bExpectedAbsent,
				TArray<UeAgentAuthoringState::FReadRequest>& OutRequests,
				FOperationFailure& OutFailure, bool bVerifyIntent = false) const
			{
				OutFailure = FOperationFailure();
				UE::UAI::Contract::FDiagnostic Error;
				if (bVerifyIntent && Root && !bExpectedAbsent && Mutation
					&& !Mutation->VerifyIntent(*Root, Error))
				{ UeAgentAuthoringDiagnostics::ReportConsistencyWarning(TEXT("umg_authored_intent_failed"), Error, TEXT("umg_intent_observation")); Error.Reset(); }
				if (!BuildAssetSnapshotReadRequests(
					Root, PackagePath, bExpectedAbsent, OutRequests, Error))
				{
					SetFailure(OutFailure, TEXT("umg_snapshot_request_failed"), TEXT("The widget Blueprint snapshot read request could not be constructed."), Error);
					return false;
				}
				return true;
			}

			FString PackagePath;
			EOperation Operation = EOperation::Update;
			TStrongObjectPtr<UWidgetBlueprint> Existing;
			TStrongObjectPtr<UWidgetBlueprint> Proposed;
			TStrongObjectPtr<UWidgetBlueprint> Backup;
			FAssetRemovalStage RemovalStage;
			UWidgetBlueprint* Actual = nullptr;
			TUniquePtr<FPreparedUmgMutation> Mutation;
			FMutationCounters ActualCounters;
			bool bCompile = true;
			bool bSave = false;
			bool bCreated = false;
			bool bMutationApplied = false;
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
					SetFailure(OutFailure, TEXT("umg_apply_request_invalid"), TEXT("The widget Blueprint apply request requires valid operation input and a target object."));
					return false;
				}

				FString PackagePath;
				Request.Input->Target->TryGetStringField(TEXT("asset_path"), PackagePath);
				PackagePath = UeAgentAuthoringAssetIdentity::NormalizePackagePath(PackagePath);
				if (!FPackageName::IsValidLongPackageName(PackagePath)
					|| FPackageName::GetLongPackageAssetName(PackagePath).IsEmpty()
					|| (!PackagePath.Equals(TEXT("/Game")) && !PackagePath.StartsWith(TEXT("/Game/"))))
				{
					SetFailure(OutFailure, TEXT("invalid_asset_path"), TEXT("The widget Blueprint target must be a valid long package path under /Game."), PackagePath);
					OutFailure.Diagnostic.ActualValue = Request.Input->Target->TryGetField(TEXT("asset_path"));
					OutFailure.Diagnostic.ActualValuePresent = Request.Input->Target->HasField(TEXT("asset_path"));
					UeAgentAuthoringDiagnostics::LocateFailure(OutFailure.Diagnostic, *Request.Input->Target, TEXT("umg_prepare"), PackagePath, TEXT("asset_path"));
					return false;
				}

				UWidgetBlueprint* Existing = LoadWidgetBlueprint(PackagePath);
				EOperation Operation = Request.Input->RequestedOperation;
				if (Operation == EOperation::Upsert)
				{
					Operation = Existing ? EOperation::Update : EOperation::Create;
				}
				if ((Operation == EOperation::Create || Operation == EOperation::Clone) && Existing)
				{
					SetFailure(OutFailure, TEXT("asset_already_exists"), TEXT("The target widget Blueprint already exists; create and clone require an absent target."), PackagePath);
					OutFailure.Diagnostic.ActualValue = Request.Input->Target->TryGetField(TEXT("asset_path"));
					OutFailure.Diagnostic.ActualValuePresent = Request.Input->Target->HasField(TEXT("asset_path"));
					UeAgentAuthoringDiagnostics::LocateFailure(OutFailure.Diagnostic, *Request.Input->Target, TEXT("umg_prepare"), PackagePath, TEXT("asset_path"));
					return false;
				}
				if ((Operation == EOperation::Update || Operation == EOperation::Remove) && !Existing)
				{
					SetFailure(OutFailure, TEXT("asset_not_found"), TEXT("The target widget Blueprint could not be loaded for update or removal."), PackagePath);
					OutFailure.Diagnostic.ActualValue = Request.Input->Target->TryGetField(TEXT("asset_path"));
					OutFailure.Diagnostic.ActualValuePresent = Request.Input->Target->HasField(TEXT("asset_path"));
					UeAgentAuthoringDiagnostics::LocateFailure(OutFailure.Diagnostic, *Request.Input->Target, TEXT("umg_prepare"), PackagePath, TEXT("asset_path"));
					return false;
				}

				FString AssetKind;
				Request.Input->Target->TryGetStringField(TEXT("asset_kind"), AssetKind);
				AssetKind.TrimStartAndEndInline();
				AssetKind.ToLowerInline();
				if (AssetKind.IsEmpty()) AssetKind = TEXT("widget_blueprint");
				if (AssetKind != TEXT("widget_blueprint"))
				{
					SetFailure(OutFailure, TEXT("umg_asset_kind_conflict"), TEXT("The UMG authoring provider only accepts widget_blueprint assets."), PackagePath);
					OutFailure.Diagnostic.ExpectedValue = MakeShared<FJsonValueString>(TEXT("widget_blueprint"));
					OutFailure.Diagnostic.ActualValue = MakeShared<FJsonValueString>(AssetKind);
					UeAgentAuthoringDiagnostics::LocateFailure(OutFailure.Diagnostic, *Request.Input->Target, TEXT("umg_prepare"), PackagePath, TEXT("asset_kind"));
					return false;
				}

				UWidgetBlueprint* Source = nullptr;
				if (Operation == EOperation::Clone)
				{
					FString SourcePath;
					Request.Input->Target->TryGetStringField(TEXT("source_asset_path"), SourcePath);
					Source = LoadWidgetBlueprint(SourcePath);
					if (!Source)
					{
						SetFailure(OutFailure, TEXT("source_asset_not_found"), TEXT("The source widget Blueprint could not be loaded for cloning."), PackagePath);
						OutFailure.Diagnostic.ActualValue = MakeShared<FJsonValueString>(SourcePath);
						UeAgentAuthoringDiagnostics::LocateFailure(OutFailure.Diagnostic, *Request.Input->Target, TEXT("umg_prepare"), PackagePath, TEXT("source_asset_path"));
						return false;
					}
				}

				UClass* ParentClass = Existing ? Existing->ParentClass.Get() : UUserWidget::StaticClass();
				FString ParentClassPath;
				if (Request.Input->ProviderExtension.IsValid()
					&& Request.Input->ProviderExtension->TryGetStringField(
						TEXT("parent_class"), ParentClassPath))
				{
					ParentClass = StaticLoadClass(UUserWidget::StaticClass(), nullptr, *ParentClassPath);
					if (!ParentClass || !ParentClass->IsChildOf(UUserWidget::StaticClass())
						|| !FKismetEditorUtilities::CanCreateBlueprintOfClass(ParentClass))
					{
						SetFailure(OutFailure, TEXT("umg_parent_class_invalid"), TEXT("The parent class must resolve to a UserWidget class that allows Blueprint creation."), PackagePath);
						OutFailure.Diagnostic.ActualValue = MakeShared<FJsonValueString>(ParentClassPath);
						OutFailure.Diagnostic.ExpectedValue = MakeShared<FJsonValueString>(UUserWidget::StaticClass()->GetPathName());
						UeAgentAuthoringDiagnostics::LocateFailure(OutFailure.Diagnostic, *Request.Input->ProviderExtension, TEXT("umg_prepare"), PackagePath, TEXT("parent_class"));
						return false;
					}
				}

				UWidgetBlueprint* Proposed = Existing;
				UWidgetBlueprint* ValidationTarget = nullptr;
				UE::UAI::Contract::FDiagnostic Error;
				if (Operation != EOperation::Remove)
				{
					UPackage* ScratchPackage = CreatePackage(*FString::Printf(
						TEXT("/Temp/UAI/UMG/Proposed_%s"),
						*FGuid::NewGuid().ToString(EGuidFormats::Digits)));
					if (Operation == EOperation::Create)
					{
						ValidationTarget = CreateFactorySeed(
							ParentClass, ScratchPackage, NAME_None, RF_Transactional, Error);
						Proposed = ValidationTarget;
					}
					else if (Operation == EOperation::Clone)
					{
						ValidationTarget = Cast<UWidgetBlueprint>(
							UeAgentBlueprintAssetLifecycle::DuplicateBlueprint(Source, ScratchPackage, NAME_None, false, &Error));
						Proposed = ValidationTarget;
					}
					else
					{
						ValidationTarget = Cast<UWidgetBlueprint>(
							UeAgentBlueprintAssetLifecycle::DuplicateBlueprint(Existing, ScratchPackage, NAME_None, true, &Error));
						Proposed = ValidationTarget;
					}
					if (!ValidationTarget)
					{
						SetFailure(
							OutFailure,
							Error.IsEmpty() ? UE::UAI::Contract::FDiagnostic::Failure(TEXT("umg_proposed_asset_create_failed"), TEXT("A temporary widget Blueprint could not be created for validation."), TEXT("umg_prepare")) : Error);
						return false;
					}
				}

				if (ValidationTarget)
					UeAgentBlueprintAssetLifecycle::SetAuthoringContext(*ValidationTarget, PackagePath, Existing);
				TUniquePtr<FPreparedUmgMutation> Mutation;
				FMutationCounters ExpectedCounters;
				if (Operation != EOperation::Remove
					&& !FPreparedUmgMutation::Prepare(
						*ValidationTarget,
						Request.Input->ProviderExtension,
						Mutation,
						ExpectedCounters,
						Error, PackagePath))
				{
					SetMutationFailure(OutFailure, Error);
					LocateMemberNameFailure(OutFailure, Request);
					return false;
				}

				OutState = MakeUnique<FAssetLifecycleState>(
					PackagePath,
					Operation,
					Existing,
					Proposed,
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
			Result.ProfileId = TEXT("umg.asset");
			Result.TargetKind = TEXT("umg_asset");
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
				Field(TEXT("asset_kind"), TEXT("type_assertion"), TEXT("umg_asset_kind"),
					{ EOperation::Create }),
				Field(TEXT("source_asset_path"), TEXT("selector"), TEXT("ue_asset_path"),
					{ EOperation::Clone }),
				Field(TEXT("operation"), TEXT("operation_control"), TEXT("apply_operation"), All),
				Field(TEXT("properties"), TEXT("intent"), TEXT("property_descriptor_list")),
				Field(TEXT("parent_class"), TEXT("provider_extension"), TEXT("ue_class_path")),
				Field(TEXT("class_defaults"), TEXT("provider_extension"), TEXT("object")),
				Field(TEXT("members"), TEXT("provider_extension"), TEXT("object")),
				Field(TEXT("widget_tree"), TEXT("provider_extension"), TEXT("object")),
				Field(TEXT("bindings"), TEXT("provider_extension"), TEXT("object_map")),
				Field(TEXT("animations"), TEXT("provider_extension"), TEXT("object_map")),
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
			Result.ProfileId = TEXT("umg.asset");
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
			Unit.RouteId = TEXT("umg.asset");
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
			Target->SetStringField(TEXT("asset_kind"), TEXT("widget_blueprint"));
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
			const FString& RequestId,
			const TFunction<bool(FOperationFailure&)>& FinalReadback = {})
		{
			OutResult = FAssetApplyResult();
			OutResult.Status = TEXT("failed");
			UeAgentAuthoringAssetWorkspaceInput::FRequest ResolveRequest;
			ResolveRequest.WorkspaceContractId = AssetWorkspaceContractId;
			ResolveRequest.Source = Source;
			ResolveRequest.ExpectedAssetPath = ExpectedAssetPath;
			ResolveRequest.ExpectedAssetKind = TEXT("widget_blueprint");
			ResolveRequest.Errors.ContractMissing = TEXT("umg_workspace_contract_missing");
			ResolveRequest.Errors.EntryInvalid = TEXT("umg_workspace_entry_invalid");
			ResolveRequest.Errors.AssetKindConflict =
				TEXT("umg_workspace_asset_kind_conflict");
			ResolveRequest.Errors.AssetPathConflict =
				TEXT("umg_workspace_asset_path_conflict");
			UeAgentAuthoringAssetWorkspaceInput::FResult Resolved;
			if (!UeAgentAuthoringAssetWorkspaceInput::Resolve(ResolveRequest, Resolved))
			{
				OutResult.Phase = Resolved.Phase;
				OutResult.Diagnostic = Resolved.Diagnostic;
				return false;
			}

			FAssetApplyRequest Request;
			Request.Operation = Resolved.Input.RequestedOperation;
			Request.AssetPath = Resolved.AssetPath;
			Request.SourceAssetPath = Resolved.SourceAssetPath;
			const UeAgentApplyWorkspaceContract::FContract& WorkspaceContract =
				Resolved.WorkspaceContract;
			const UeAgentAuthoringWorkspaceLoader::FLoadResult& LoadResult = Resolved.Load;
			const UeAgentApplyInputContract::FOperationInput& Input = Resolved.Input;

			const bool bExisted = LoadWidgetBlueprint(Request.AssetPath) != nullptr;
			FBackendOptions BackendOptions;
			BackendOptions.FinalReadback = FinalReadback;
			BackendOptions.bUseEditorTransaction = Request.Operation != EOperation::Remove;
			BackendOptions.TransactionDescription = NSLOCTEXT(
				"UeAgentInterface", "UmgAssetWorkspaceApply", "Apply UMG asset workspace");
			FApplyWorkspaceResult ApplyResult;
			UeAgentAuthoringAssetWorkspaceApply::FFailure ApplyFailure;
			const bool bApplied = UeAgentAuthoringAssetWorkspaceApply::Execute(
				WorkspaceContract,
				LoadResult.Workspace,
				FProviderRegistry::Get(),
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
				: LoadWidgetBlueprint(Request.AssetPath);
			OutResult.ObjectPath = OutResult.Asset
				? OutResult.Asset->GetPathName()
				: UeAgentAuthoringAssetIdentity::ToObjectPath(Request.AssetPath);
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

	TSharedRef<IApplyProvider> MakeApplyProvider()
	{
		static const TSharedRef<IReflectionOperationLifecycle> Lifecycle = MakeShared<FAssetLifecycle>();
		UeAgentAuthoringObjectApply::FOptions Options;
		Options.bAllowDomainAuthorizedOwners = true;
		return MakeShared<FReflectionApplyProvider>(ApplyProviderId, Lifecycle, Options);
	}

	bool ApplyAsset(const FAssetApplyRequest& Request, FAssetApplyResult& OutResult, const FString& RequestId)
	{
		return ExecuteAssetWorkspaceSource(
			UeAgentAuthoringWorkspaceLoader::FSourceRequest::MakeInlineObject(
				MakeApplyDocument(Request), TEXT("inline://umg-asset-apply")),
			Request.AssetPath,
			OutResult,
			RequestId,
			Request.FinalReadback);
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
}
