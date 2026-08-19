// Copyright Epic Games, Inc. All Rights Reserved.

#include "Domains/Blueprint/Authoring/UeAgentBlueprintAuthoringApplyProvider.h"

#include "AssetRegistry/AssetRegistryModule.h"
#include "Commands/Blueprint/UeAgentHttpServer_BlueprintOps.h"
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
#include "Utilities/AuthoringOperationApply/UeAgentAuthoringAssetRemovalStage.h"
#include "Utilities/AuthoringOperationApply/UeAgentAuthoringObjectApplyProvider.h"
#include "Utilities/AuthoringOperationApply/UeAgentAuthoringOperationCoordinator.h"
#include "Utilities/AuthoringWorkspaceApply/UeAgentApplyWorkspaceContract.h"
#include "Utilities/AuthoringWorkspaceApply/UeAgentAuthoringWorkspaceDependencyGraph.h"
#include "Utilities/AuthoringWorkspaceApply/UeAgentAuthoringWorkspaceLoader.h"
#include "Utilities/AuthoringWorkspaceApply/UeAgentAuthoringWorkspaceTransaction.h"

namespace UeAgentBlueprintAuthoring
{
	namespace
	{
		using namespace UeAgentApplyInputContract;
		using namespace UeAgentAuthoringOperationApply;
		using namespace UeAgentBlueprintAssetLifecycle;

		void SetFailure(
			FOperationFailure& OutFailure,
			const FString& Code,
			const FString& Detail = FString())
		{
			OutFailure = FOperationFailure();
			OutFailure.ErrorCode = Code;
			OutFailure.ErrorDetail = Detail.IsEmpty() ? Code : Detail;
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
				TUniquePtr<FPreparedBlueprintMutation> InMutation,
				FMutationCounters InPreparedCounters,
				bool bInCompile,
				bool bInSave)
				: PackagePath(MoveTemp(InPackagePath))
				, AssetKind(MoveTemp(InAssetKind))
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
						TEXT("/Temp/UAI/Blueprint/Rollback_%s"),
						*FGuid::NewGuid().ToString(EGuidFormats::Digits)));
					Backup.Reset(DuplicateBlueprint(Existing.Get(), Package));
					if (!Backup.IsValid())
					{
						SetFailure(OutFailure, TEXT("blueprint_rollback_capture_failed"));
						return false;
					}
					return true;
				}
				if (Operation != EOperation::Remove || !Existing.IsValid()) return true;
				FString Error;
				if (!RemovalStage.Capture(Existing.Get(), Error))
				{
					SetFailure(OutFailure, TEXT("blueprint_remove_capture_failed"), Error);
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
					SetFailure(OutFailure, TEXT("blueprint_mutation_apply_failed"), Error);
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
						SetFailure(OutFailure, TEXT("blueprint_remove_stage_failed"), Error);
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
					SetFailure(OutFailure, TEXT("blueprint_proposed_asset_missing"));
					return false;
				}
				UPackage* TargetPackage = CreatePackage(*PackagePath);
				Actual = PublishBlueprint(
					Proposed.Get(), TargetPackage, FPackageName::GetLongPackageAssetName(PackagePath));
				if (!Actual)
				{
					SetFailure(OutFailure, TEXT("blueprint_asset_publish_failed"));
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
				UBlueprint* Target = Actual ? Actual : Existing.Get();
				FString Error;
				if ((bCompile || (Mutation.IsValid() && Mutation->RequiresCompile()))
					&& !CompileBlueprint(Target, Error))
				{
					SetFailure(OutFailure, TEXT("blueprint_compile_failed_after_apply"), Error);
					return false;
				}
				if (Mutation.IsValid())
				{
					FMutationCounters Deferred;
					if (!Mutation->FinalizeAfterCompile(*Target, Deferred, Error))
					{
						SetFailure(OutFailure, TEXT("blueprint_defaults_finalize_failed"), Error);
						return false;
					}
					ActualCounters.Add(Deferred);
				}
				return true;
			}

			virtual bool Save(FOperationFailure& OutFailure) override
			{
				OutFailure = FOperationFailure();
				if (!bSave || Operation == EOperation::Remove) return true;
				FString Error;
				if (!UeAgentBlueprintOps::SaveBlueprintPackage(Actual ? Actual : Existing.Get(), Error))
				{
					SetFailure(OutFailure, TEXT("blueprint_save_failed_after_apply"), Error);
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
						: (Operation == EOperation::Update ? Existing.Get() : Proposed.Get()),
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
					if (!DeleteBlueprint(Cast<UBlueprint>(RemovalStage.GetAsset()), Error))
					{
						SetFailure(OutFailure, TEXT("blueprint_remove_commit_failed"), Error);
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
					if (!FPreparedBlueprintMutation::RestoreFromBackup(*Backup, *Existing, Error))
					{
						SetFailure(OutFailure, TEXT("blueprint_update_rollback_failed"), Error);
						return false;
					}
					Actual = Existing.Get();
					return true;
				}
				if (Operation == EOperation::Create || Operation == EOperation::Clone)
				{
					if (Actual && !DeleteBlueprint(Actual, Error))
					{
						TArray<UObject*> Objects = { Actual };
						if (ObjectTools::DeleteObjectsUnchecked(Objects) <= 0)
						{
							SetFailure(OutFailure, TEXT("blueprint_create_rollback_failed"), Error);
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
						SetFailure(OutFailure, TEXT("blueprint_remove_rollback_failed"), Error);
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
				FOperationFailure& OutFailure) const
			{
				OutFailure = FOperationFailure();
				FString Error;
				if (!BuildAssetSnapshotReadRequest(
					Root,
					PackagePath,
					AssetKind,
					bExpectedAbsent,
					OutRequests,
					Error))
				{
					SetFailure(OutFailure, TEXT("blueprint_snapshot_request_failed"), Error);
					return false;
				}
				return true;
			}

			FString PackagePath;
			FString AssetKind;
			EOperation Operation = EOperation::Update;
			TStrongObjectPtr<UBlueprint> Existing;
			TStrongObjectPtr<UBlueprint> Proposed;
			TStrongObjectPtr<UBlueprint> Backup;
			FAssetRemovalStage RemovalStage;
			UBlueprint* Actual = nullptr;
			TUniquePtr<FPreparedBlueprintMutation> Mutation;
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
					SetFailure(OutFailure, TEXT("blueprint_apply_request_invalid"));
					return false;
				}

				FString PackagePath;
				Request.Input->Target->TryGetStringField(TEXT("asset_path"), PackagePath);
				PackagePath = NormalizePackagePath(PackagePath);
				if (!FPackageName::IsValidLongPackageName(PackagePath)
					|| FPackageName::GetLongPackageAssetName(PackagePath).IsEmpty()
					|| (!PackagePath.Equals(TEXT("/Game")) && !PackagePath.StartsWith(TEXT("/Game/"))))
				{
					SetFailure(OutFailure, TEXT("invalid_asset_path"), PackagePath);
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
					SetFailure(OutFailure, TEXT("asset_already_exists"), PackagePath);
					return false;
				}
				if ((Operation == EOperation::Update || Operation == EOperation::Remove) && !Existing)
				{
					SetFailure(OutFailure, TEXT("asset_not_found"), PackagePath);
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
						SetFailure(OutFailure, TEXT("source_asset_not_found"), SourcePath);
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
					ParentClass = UeAgentBlueprintOps::ResolveClassByPath(ParentClassPath);
					if (!ParentClass
						|| !FKismetEditorUtilities::CanCreateBlueprintOfClass(ParentClass))
					{
						SetFailure(OutFailure, TEXT("blueprint_parent_class_invalid"), ParentClassPath);
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
					SetFailure(
						OutFailure,
						TEXT("blueprint_asset_kind_conflict"),
						RequestedAssetKind + TEXT(" != ") + ResolvedAssetKind);
					return false;
				}

				UBlueprint* Proposed = Existing;
				UBlueprint* ValidationTarget = nullptr;
				UPackage* ScratchPackage = nullptr;
				FString Error;
				if (Operation != EOperation::Remove)
				{
					ScratchPackage = CreatePackage(*FString::Printf(
						TEXT("/Temp/UAI/Blueprint/Proposed_%s"),
						*FGuid::NewGuid().ToString(EGuidFormats::Digits)));
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
						ValidationTarget = DuplicateBlueprint(Source, ScratchPackage);
						Proposed = ValidationTarget;
					}
					else
					{
						ValidationTarget = DuplicateBlueprint(Existing, ScratchPackage);
					}
					if (!ValidationTarget)
					{
						SetFailure(
							OutFailure,
							Error.IsEmpty() ? TEXT("blueprint_proposed_asset_create_failed") : Error);
						return false;
					}
				}

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
					SetFailure(OutFailure, TEXT("blueprint_mutation_preflight_failed"), Error);
					return false;
				}

				OutState = MakeUnique<FAssetLifecycleState>(
					PackagePath,
					ResolvedAssetKind,
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
			const FString& RequestId)
		{
			OutResult = FAssetApplyResult();
			OutResult.Status = TEXT("failed");
			UeAgentApplyWorkspaceContract::FContract WorkspaceContract;
			if (!UeAgentApplyWorkspaceContract::FRegistry::Get().FindContract(
				AssetWorkspaceContractId, WorkspaceContract))
			{
				OutResult.Phase = TEXT("registration");
				OutResult.ErrorCode = TEXT("blueprint_workspace_contract_missing");
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
				OutResult.ErrorCode = TEXT("blueprint_workspace_entry_invalid");
				OutResult.ErrorDetail = OutResult.ErrorCode;
				return false;
			}

			(*Target)->TryGetStringField(TEXT("asset_path"), Request.AssetPath);
			Request.AssetPath = NormalizePackagePath(Request.AssetPath);
			(*Target)->TryGetStringField(TEXT("source_asset_path"), Request.SourceAssetPath);
			FString AssetKind;
			(*Target)->TryGetStringField(TEXT("asset_kind"), AssetKind);
			AssetKind.TrimStartAndEndInline();
			AssetKind.ToLowerInline();
			if (!AssetKind.IsEmpty()
				&& AssetKind != TEXT("blueprint")
				&& AssetKind != TEXT("actor_blueprint"))
			{
				OutResult.Phase = TEXT("preflight");
				OutResult.ErrorCode = TEXT("blueprint_workspace_asset_kind_conflict");
				OutResult.ErrorDetail = AssetKind;
				return false;
			}
			const FString NormalizedExpectedPath = NormalizePackagePath(ExpectedAssetPath);
			if (!NormalizedExpectedPath.IsEmpty()
				&& !Request.AssetPath.Equals(NormalizedExpectedPath, ESearchCase::IgnoreCase))
			{
				OutResult.Phase = TEXT("preflight");
				OutResult.ErrorCode = TEXT("blueprint_workspace_asset_path_conflict");
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

			const bool bExisted = LoadBlueprint(Request.AssetPath) != nullptr;
			FBackendOptions BackendOptions;
			BackendOptions.bUseEditorTransaction = Request.Operation != EOperation::Remove;
			BackendOptions.TransactionDescription = NSLOCTEXT(
				"UeAgentInterface", "BlueprintAssetWorkspaceApply", "Apply Blueprint asset workspace");
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
				: LoadBlueprint(Request.AssetPath);
			OutResult.ObjectPath = OutResult.Asset
				? OutResult.Asset->GetPathName()
				: ToObjectPath(Request.AssetPath);
			OutResult.AssetKind = OutResult.Asset
				? GetAssetKind(OutResult.Asset)
				: AssetKind;
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
				MakeApplyDocument(Request), TEXT("inline://blueprint-asset-apply")),
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

	bool ApplyAssetInlineWorkspace(
		const TSharedPtr<const FJsonObject>& Document,
		const FString& ExpectedAssetPath,
		FAssetApplyResult& OutResult,
		const FString& RequestId)
	{
		return ExecuteAssetWorkspaceSource(
			UeAgentAuthoringWorkspaceLoader::FSourceRequest::MakeInlineObject(
				Document, TEXT("inline://blueprint-surface-domain")),
			ExpectedAssetPath,
			OutResult,
			RequestId);
	}
}
