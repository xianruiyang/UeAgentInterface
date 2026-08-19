// Copyright Epic Games, Inc. All Rights Reserved.

#include "Domains/UMG/Authoring/UeAgentUmgAuthoringApplyProvider.h"

#include "AssetRegistry/AssetRegistryModule.h"
#include "Blueprint/UserWidget.h"
#include "Commands/UMG/UeAgentHttpServer_UMGOps.h"
#include "Domains/Blueprint/Authoring/UeAgentBlueprintAssetLifecycle.h"
#include "Domains/UMG/Authoring/UeAgentUmgAuthoringDescriptors.h"
#include "Domains/UMG/Authoring/UeAgentUmgAuthoringState.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Kismet2/KismetEditorUtilities.h"
#include "Misc/PackageName.h"
#include "ObjectTools.h"
#include "UObject/StrongObjectPtr.h"
#include "Utilities/AuthoringOperationApply/UeAgentAuthoringAssetRemovalStage.h"
#include "Utilities/AuthoringOperationApply/UeAgentAuthoringObjectApplyProvider.h"
#include "Utilities/AuthoringOperationApply/UeAgentAuthoringOperationCoordinator.h"
#include "Utilities/AuthoringWorkspaceApply/UeAgentApplyWorkspaceContract.h"
#include "Utilities/AuthoringWorkspaceApply/UeAgentAuthoringWorkspaceDependencyGraph.h"
#include "Utilities/AuthoringWorkspaceApply/UeAgentAuthoringWorkspaceLoader.h"
#include "Utilities/AuthoringWorkspaceApply/UeAgentAuthoringWorkspaceTransaction.h"
#include "WidgetBlueprint.h"

namespace UeAgentUmgAuthoring
{
	namespace
	{
		using namespace UeAgentApplyInputContract;
		using namespace UeAgentAuthoringOperationApply;

		void SetFailure(
			FOperationFailure& OutFailure,
			const FString& Code,
			const FString& Detail = FString())
		{
			OutFailure = FOperationFailure();
			OutFailure.ErrorCode = Code;
			OutFailure.ErrorDetail = Detail.IsEmpty() ? Code : Detail;
		}

		UWidgetBlueprint* LoadWidgetBlueprint(const FString& InPath)
		{
			return Cast<UWidgetBlueprint>(
				UeAgentBlueprintAssetLifecycle::LoadBlueprint(InPath));
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
						UeAgentBlueprintAssetLifecycle::DuplicateBlueprint(Existing.Get(), Package)));
					if (!Backup.IsValid())
					{
						SetFailure(OutFailure, TEXT("umg_rollback_capture_failed"));
						return false;
					}
					return true;
				}
				if (Operation != EOperation::Remove || !Existing.IsValid()) return true;
				FString Error;
				if (!RemovalStage.Capture(Existing.Get(), Error))
				{
					SetFailure(OutFailure, TEXT("umg_remove_capture_failed"), Error);
					return false;
				}
				return true;
			}

			virtual bool ApplyProposed(FOperationFailure& OutFailure) override
			{
				OutFailure = FOperationFailure();
				if (Operation != EOperation::Update || !Mutation.IsValid()) return true;
				FString Error;
				if (!Mutation->ApplyTo(*Existing, ActualCounters, Error))
				{
					SetFailure(OutFailure, TEXT("umg_mutation_apply_failed"), Error);
					return false;
				}
				return true;
			}

			virtual bool ApplyLifecycle(FOperationFailure& OutFailure) override
			{
				OutFailure = FOperationFailure();
				if (Operation == EOperation::Remove)
				{
					FString Error;
					if (!RemovalStage.Stage(Error))
					{
						SetFailure(OutFailure, TEXT("umg_remove_stage_failed"), Error);
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
					SetFailure(OutFailure, TEXT("umg_proposed_asset_missing"));
					return false;
				}
				UPackage* TargetPackage = CreatePackage(*PackagePath);
				Actual = Cast<UWidgetBlueprint>(UeAgentBlueprintAssetLifecycle::PublishBlueprint(
					Proposed.Get(), TargetPackage, FPackageName::GetLongPackageAssetName(PackagePath)));
				if (!Actual)
				{
					SetFailure(OutFailure, TEXT("umg_asset_publish_failed"));
					return false;
				}
				FAssetRegistryModule::AssetCreated(Actual);
				Actual->MarkPackageDirty();
				return true;
			}

			virtual bool Compile(FOperationFailure& OutFailure) override
			{
				OutFailure = FOperationFailure();
				if (Operation == EOperation::Remove) return true;
				UWidgetBlueprint* Target = Actual ? Actual : Existing.Get();
				FString Error;
				if ((bCompile || (Mutation.IsValid() && Mutation->RequiresCompile())))
				{
					UeAgentUmgOps::SyncWidgetVariableGuidMap(Target);
					if (!UeAgentBlueprintAssetLifecycle::CompileBlueprint(Target, Error))
					{
						SetFailure(OutFailure, TEXT("umg_compile_failed_after_apply"), Error);
						return false;
					}
				}
				if (Mutation.IsValid())
				{
					FMutationCounters Deferred;
					bool bSecondCompile = false;
					if (!Mutation->FinalizeAfterCompile(*Target, Deferred, bSecondCompile, Error))
					{
						SetFailure(OutFailure, TEXT("umg_finalize_failed"), Error);
						return false;
					}
					ActualCounters.Add(Deferred);
					if (bSecondCompile)
					{
						UeAgentUmgOps::SyncWidgetVariableGuidMap(Target);
						if (!UeAgentBlueprintAssetLifecycle::CompileBlueprint(Target, Error))
						{
							SetFailure(OutFailure, TEXT("umg_binding_compile_failed"), Error);
							return false;
						}
					}
				}
				return true;
			}

			virtual bool Save(FOperationFailure& OutFailure) override
			{
				OutFailure = FOperationFailure();
				if (!bSave || Operation == EOperation::Remove) return true;
				FString Error;
				if (!UeAgentUmgOps::SaveWidgetBlueprintPackage(
					Actual ? Actual : Existing.Get(), Error))
				{
					SetFailure(OutFailure, TEXT("umg_save_failed_after_apply"), Error);
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
						: (Existing.IsValid() ? Existing.Get() : Proposed.Get()),
					Operation == EOperation::Remove ? EPackageDurableIntent::Delete
						: EPackageDurableIntent::Save,
					OutPlans,
					OutFailure);
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
					OutFailure);
			}

			virtual bool BuildDomainActualReadRequests(
				TArray<UeAgentAuthoringState::FReadRequest>& OutRequests,
				FOperationFailure& OutFailure) const override
			{
				return BuildReadRequests(
					Operation == EOperation::Remove ? nullptr : (Actual ? Actual : Existing.Get()),
					Operation == EOperation::Remove,
					OutRequests,
					OutFailure);
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
					FString Error;
					if (!UeAgentBlueprintAssetLifecycle::DeleteBlueprint(
						Cast<UWidgetBlueprint>(RemovalStage.GetAsset()), Error))
					{
						SetFailure(OutFailure, TEXT("umg_remove_commit_failed"), Error);
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
				FString Error;
				if (Operation == EOperation::Update && Backup.IsValid())
				{
					if (!FPreparedUmgMutation::RestoreFromBackup(*Backup, *Existing, Error))
					{
						SetFailure(OutFailure, TEXT("umg_update_rollback_failed"), Error);
						return false;
					}
					Actual = Existing.Get();
					return true;
				}
				if (Operation == EOperation::Create || Operation == EOperation::Clone)
				{
					if (Actual && !UeAgentBlueprintAssetLifecycle::DeleteBlueprint(Actual, Error))
					{
						TArray<UObject*> Objects = { Actual };
						if (ObjectTools::DeleteObjectsUnchecked(Objects) <= 0)
						{
							SetFailure(OutFailure, TEXT("umg_create_rollback_failed"), Error);
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
						SetFailure(OutFailure, TEXT("umg_remove_rollback_failed"), Error);
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
				FOperationFailure& OutFailure) const
			{
				OutFailure = FOperationFailure();
				FString Error;
				if (!BuildAssetSnapshotReadRequests(
					Root, PackagePath, bExpectedAbsent, OutRequests, Error))
				{
					SetFailure(OutFailure, TEXT("umg_snapshot_request_failed"), Error);
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
					SetFailure(OutFailure, TEXT("umg_apply_request_invalid"));
					return false;
				}

				FString PackagePath;
				Request.Input->Target->TryGetStringField(TEXT("asset_path"), PackagePath);
				PackagePath = UeAgentBlueprintAssetLifecycle::NormalizePackagePath(PackagePath);
				if (!FPackageName::IsValidLongPackageName(PackagePath)
					|| FPackageName::GetLongPackageAssetName(PackagePath).IsEmpty()
					|| (!PackagePath.Equals(TEXT("/Game")) && !PackagePath.StartsWith(TEXT("/Game/"))))
				{
					SetFailure(OutFailure, TEXT("invalid_asset_path"), PackagePath);
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
					SetFailure(OutFailure, TEXT("asset_already_exists"), PackagePath);
					return false;
				}
				if ((Operation == EOperation::Update || Operation == EOperation::Remove) && !Existing)
				{
					SetFailure(OutFailure, TEXT("asset_not_found"), PackagePath);
					return false;
				}

				FString AssetKind;
				Request.Input->Target->TryGetStringField(TEXT("asset_kind"), AssetKind);
				AssetKind.TrimStartAndEndInline();
				AssetKind.ToLowerInline();
				if (AssetKind.IsEmpty()) AssetKind = TEXT("widget_blueprint");
				if (AssetKind != TEXT("widget_blueprint"))
				{
					SetFailure(OutFailure, TEXT("umg_asset_kind_conflict"), AssetKind);
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
						SetFailure(OutFailure, TEXT("source_asset_not_found"), SourcePath);
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
						SetFailure(OutFailure, TEXT("umg_parent_class_invalid"), ParentClassPath);
						return false;
					}
				}

				UWidgetBlueprint* Proposed = Existing;
				UWidgetBlueprint* ValidationTarget = nullptr;
				FString Error;
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
							UeAgentBlueprintAssetLifecycle::DuplicateBlueprint(Source, ScratchPackage));
						Proposed = ValidationTarget;
					}
					else
					{
						ValidationTarget = Cast<UWidgetBlueprint>(
							UeAgentBlueprintAssetLifecycle::DuplicateBlueprint(Existing, ScratchPackage));
						Proposed = ValidationTarget;
					}
					if (!ValidationTarget)
					{
						SetFailure(
							OutFailure,
							Error.IsEmpty() ? TEXT("umg_proposed_asset_create_failed") : Error);
						return false;
					}
				}

				TUniquePtr<FPreparedUmgMutation> Mutation;
				FMutationCounters ExpectedCounters;
				if (Operation != EOperation::Remove
					&& !FPreparedUmgMutation::Prepare(
						*ValidationTarget,
						Request.Input->ProviderExtension,
						Mutation,
						ExpectedCounters,
						Error))
				{
					SetFailure(OutFailure, TEXT("umg_mutation_preflight_failed"), Error);
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
			Result.ReflectionBaseline.RuntimeLimitsSource = TEXT("shared_runtime_limits");
			Result.PropertyPayload.Mode = TEXT("reflection");
			Result.PropertyPayload.PropertyPathField = TEXT("property_path");
			Result.PropertyPayload.ValueField = TEXT("value");
			Result.PropertyPayload.OptionalAssertionFields = {
				TEXT("value_kind"), TEXT("struct_path"), TEXT("enum_path"), TEXT("element_type"),
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
			const FString& RequestId)
		{
			OutResult = FAssetApplyResult();
			OutResult.Status = TEXT("failed");
			UeAgentApplyWorkspaceContract::FContract WorkspaceContract;
			if (!UeAgentApplyWorkspaceContract::FRegistry::Get().FindContract(
				AssetWorkspaceContractId, WorkspaceContract))
			{
				OutResult.Phase = TEXT("registration");
				OutResult.ErrorCode = TEXT("umg_workspace_contract_missing");
				return false;
			}

			UeAgentAuthoringWorkspaceLoader::FLoadResult LoadResult;
			if (!UeAgentAuthoringWorkspaceLoader::LoadWorkspace(
				WorkspaceContract, Source, LoadResult))
			{
				OutResult.Phase = TEXT("load");
				OutResult.ErrorCode = LoadResult.ErrorCode;
				OutResult.ErrorDetail = LoadResult.ErrorDetail;
				return false;
			}

			const UeAgentAuthoringWorkspaceRouter::FWorkspaceUnit* AssetUnit =
				LoadResult.Workspace.FindSingleUnit(TEXT("asset"));
			const TSharedPtr<const FJsonObject> Document = AssetUnit ? AssetUnit->Document : nullptr;
			const TSharedPtr<FJsonObject>* Target = nullptr;
			FString OperationText;
			FAssetApplyRequest Request;
			if (!Document.IsValid()
				|| !Document->TryGetStringField(TEXT("operation"), OperationText)
				|| !ParseOperation(OperationText, Request.Operation)
				|| !Document->TryGetObjectField(TEXT("target"), Target)
				|| !Target || !Target->IsValid())
			{
				OutResult.Phase = TEXT("preflight");
				OutResult.ErrorCode = TEXT("umg_workspace_entry_invalid");
				OutResult.ErrorDetail = OutResult.ErrorCode;
				return false;
			}

			(*Target)->TryGetStringField(TEXT("asset_path"), Request.AssetPath);
			Request.AssetPath = UeAgentBlueprintAssetLifecycle::NormalizePackagePath(Request.AssetPath);
			(*Target)->TryGetStringField(TEXT("source_asset_path"), Request.SourceAssetPath);
			FString AssetKind;
			(*Target)->TryGetStringField(TEXT("asset_kind"), AssetKind);
			AssetKind.TrimStartAndEndInline();
			AssetKind.ToLowerInline();
			if (!AssetKind.IsEmpty() && AssetKind != TEXT("widget_blueprint"))
			{
				OutResult.Phase = TEXT("preflight");
				OutResult.ErrorCode = TEXT("umg_workspace_asset_kind_conflict");
				OutResult.ErrorDetail = AssetKind;
				return false;
			}
			const FString NormalizedExpectedPath =
				UeAgentBlueprintAssetLifecycle::NormalizePackagePath(ExpectedAssetPath);
			if (!NormalizedExpectedPath.IsEmpty()
				&& !Request.AssetPath.Equals(NormalizedExpectedPath, ESearchCase::IgnoreCase))
			{
				OutResult.Phase = TEXT("preflight");
				OutResult.ErrorCode = TEXT("umg_workspace_asset_path_conflict");
				OutResult.ErrorDetail = Request.AssetPath + TEXT(" != ") + NormalizedExpectedPath;
				return false;
			}

			const TArray<TSharedPtr<FJsonValue>>* Properties = nullptr;
			if (Document->TryGetArrayField(TEXT("properties"), Properties) && Properties)
			{
				Request.Properties = *Properties;
			}
			const TSharedPtr<FJsonObject>* ProviderExtension = nullptr;
			if (Document->TryGetObjectField(TEXT("provider_extension"), ProviderExtension)
				&& ProviderExtension && ProviderExtension->IsValid())
			{
				Request.ProviderExtension = *ProviderExtension;
			}

			UeAgentAuthoringWorkspaceDependencyGraph::FDependencyGraph Graph;
			UeAgentAuthoringWorkspaceDependencyGraph::FGraphFailure GraphFailure;
			if (!UeAgentAuthoringWorkspaceDependencyGraph::BuildDependencyGraph(
				WorkspaceContract,
				LoadResult.Workspace,
				UeAgentAuthoringWorkspaceDependencyGraph::FDependencyInputs(),
				Graph,
				GraphFailure))
			{
				OutResult.Phase = TEXT("preflight");
				OutResult.ErrorCode = GraphFailure.ErrorCode;
				OutResult.ErrorDetail = GraphFailure.ErrorDetail;
				return false;
			}

			const bool bExisted = LoadWidgetBlueprint(Request.AssetPath) != nullptr;
			FBackendOptions BackendOptions;
			BackendOptions.bUseEditorTransaction = Request.Operation != EOperation::Remove;
			BackendOptions.TransactionDescription = NSLOCTEXT(
				"UeAgentInterface", "UmgAssetWorkspaceApply", "Apply UMG asset workspace");
			UeAgentAuthoringWorkspaceTransaction::FExecutionOptions ExecutionOptions;
			FOperationFailure RequestFailure;
			if (!BuildApplyEntryExecutionOptions(RequestId, ExecutionOptions, RequestFailure))
			{
				OutResult.Phase = TEXT("preflight");
				OutResult.ErrorCode = RequestFailure.ErrorCode;
				OutResult.ErrorDetail = RequestFailure.ErrorDetail;
				return false;
			}
			ExecutionOptions.bSaveWhenCallerControlled =
				ReadBool(Request.ProviderExtension, TEXT("save"), false);
			FApplyWorkspaceResult ApplyResult;
			const bool bApplied = ApplyWorkspace(
				WorkspaceContract,
				LoadResult.Workspace,
				Graph,
				UeAgentApplyInputContract::FRegistry::Get(),
				FProviderRegistry::Get(),
				ExecutionOptions,
				BackendOptions,
				ApplyResult);
			OutResult.UnitsChecked = ApplyResult.Preflight.UnitsChecked;
			OutResult.MutationsApplied = ApplyResult.Execution.MutationsApplied;
			OutResult.bRollbackAttempted = ApplyResult.Execution.bRollbackAttempted;
			OutResult.bRollbackSucceeded = ApplyResult.Execution.bRollbackSucceeded;
			if (!bApplied)
			{
				const bool bPreflight = !ApplyResult.Preflight.IsReady();
				OutResult.Phase = bPreflight ? TEXT("preflight") : ApplyResult.Execution.Phase;
				OutResult.ErrorCode = bPreflight
					? ApplyResult.Preflight.CauseCode
					: ApplyResult.Execution.CauseCode;
				if (OutResult.ErrorCode.IsEmpty())
				{
					OutResult.ErrorCode = bPreflight
						? ApplyResult.Preflight.ErrorCode
						: ApplyResult.Execution.ErrorCode;
				}
				OutResult.ErrorDetail = bPreflight
					? ApplyResult.Preflight.ErrorDetail
					: ApplyResult.Execution.ErrorDetail;
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
				: UeAgentBlueprintAssetLifecycle::ToObjectPath(Request.AssetPath);
			const TSharedPtr<FJsonObject>* ResultMetadata =
				ApplyResult.UnitResultMetadata.Find(AssetUnit->CanonicalUnitKey);
			ReadMutationResultMetadata(
				ResultMetadata ? *ResultMetadata : nullptr,
				OutResult.Domain);
			return true;
		}
	}

	bool RegisterApplyContracts(FString& OutError)
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
			RequestId);
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
