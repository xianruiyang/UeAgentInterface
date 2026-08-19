// Copyright Epic Games, Inc. All Rights Reserved.

#include "Domains/Niagara/Authoring/UeAgentNiagaraStackApplyProvider.h"

#include "Domains/Niagara/Authoring/UeAgentNiagaraAuthoringApplyProvider.h"
#include "Domains/Niagara/Authoring/UeAgentNiagaraAuthoringDescriptors.h"
#include "Domains/Niagara/Authoring/UeAgentNiagaraAuthoringScriptResolver.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Misc/PackageName.h"
#include "NiagaraGraph.h"
#include "NiagaraNodeFunctionCall.h"
#include "NiagaraNodeOutput.h"
#include "NiagaraScript.h"
#include "NiagaraScriptSource.h"
#include "UObject/StrongObjectPtr.h"
#include "UObject/UObjectGlobals.h"
#include "Utilities/AuthoringCore/UeAgentAuthoringIdentity.h"
#include "Utilities/AuthoringOperationApply/UeAgentAuthoringOperationCoordinator.h"
#include "Utilities/AuthoringWorkspaceApply/UeAgentApplyWorkspaceContract.h"
#include "Utilities/AuthoringWorkspaceApply/UeAgentAuthoringWorkspaceDependencyGraph.h"
#include "Utilities/AuthoringWorkspaceApply/UeAgentAuthoringWorkspaceLoader.h"

namespace UeAgentNiagaraAuthoring
{
	namespace
	{
		using namespace UeAgentApplyInputContract;
		using namespace UeAgentAuthoringOperationApply;
		using namespace UeAgentNiagaraStageModuleAuthoring;

		void SetFailure(FOperationFailure& OutFailure, const FString& Code, const FString& Detail = FString())
		{
			OutFailure = FOperationFailure();
			OutFailure.ErrorCode = Code;
			OutFailure.ErrorDetail = Detail.IsEmpty() ? Code : Detail;
		}

		bool ReadString(const TSharedPtr<const FJsonObject>& Object, const TCHAR* Field, FString& OutValue)
		{
			return Object.IsValid() && Object->TryGetStringField(Field, OutValue) && !OutValue.IsEmpty();
		}

		bool ReadBool(const TSharedPtr<const FJsonObject>& Object, const TCHAR* Field, const bool DefaultValue)
		{
			bool Value = DefaultValue;
			if (Object.IsValid()) Object->TryGetBoolField(Field, Value);
			return Value;
		}

		int32 ReadIndex(const TSharedPtr<const FJsonObject>& Object, const TCHAR* Field)
		{
			double Value = INDEX_NONE;
			return Object.IsValid() && Object->TryGetNumberField(Field, Value)
				? FMath::RoundToInt(Value)
				: INDEX_NONE;
		}

		bool ParseGuidField(
			const TSharedPtr<const FJsonObject>& Object,
			const TCHAR* Field,
			FGuid& OutGuid,
			FString& OutError)
		{
			FString Text;
			if (!ReadString(Object, Field, Text)) return true;
			if (!FGuid::Parse(Text, OutGuid))
			{
				OutError = FString::Printf(TEXT("niagara_guid_invalid:%s"), Field);
				return false;
			}
			return true;
		}

		bool ParseModuleSelector(
			const TSharedPtr<const FJsonObject>& Object,
			const FString& Prefix,
			FModuleSelector& OutSelector,
			FString& OutError)
		{
			OutSelector = FModuleSelector();
			const FString GuidField = Prefix + TEXT("module_guid");
			if (!ParseGuidField(Object, *GuidField, OutSelector.NodeGuid, OutError)) return false;
			const TCHAR* ScriptPathField = Prefix.IsEmpty()
				? TEXT("module_selector_script_asset_path")
				: TEXT("source_module_selector_script_asset_path");
			ReadString(Object, ScriptPathField, OutSelector.ScriptPath);
			ReadString(Object, *(Prefix + TEXT("module_name")), OutSelector.Name);
			double Occurrence = 0.0;
			if (Object.IsValid() && Object->TryGetNumberField(*(Prefix + TEXT("occurrence")), Occurrence))
			{
				OutSelector.Occurrence = FMath::RoundToInt(Occurrence);
				OutSelector.bOccurrenceExplicit = true;
			}
			return true;
		}

		bool ParseStageTarget(
			const TSharedPtr<const FJsonObject>& Target,
			FStageTarget& OutTarget,
			FString& OutError)
		{
			OutTarget = FStageTarget();
			if (!ReadString(Target, TEXT("asset_path"), OutTarget.AssetPath))
			{
				OutError = TEXT("niagara_stage_asset_path_missing");
				return false;
			}
			ReadString(Target, TEXT("emitter_name"), OutTarget.EmitterName);
			FString EmitterHandleIdText;
			if (ReadString(Target, TEXT("emitter_handle_id"), EmitterHandleIdText))
			{
				if (!FGuid::Parse(EmitterHandleIdText, OutTarget.EmitterHandleId))
				{
					OutError = TEXT("niagara_emitter_handle_id_invalid");
					return false;
				}
				OutTarget.bEmitterHandleIdExplicit = true;
			}
			FString UsageText;
			if (ReadString(Target, TEXT("usage"), UsageText))
			{
				if (!TryParseScriptUsage(UsageText, OutTarget.Usage))
				{
					OutError = TEXT("niagara_script_usage_invalid");
					return false;
				}
				OutTarget.bUsageExplicit = true;
			}
			FString UsageIdText;
			if (ReadString(Target, TEXT("usage_id"), UsageIdText))
			{
				if (!FGuid::Parse(UsageIdText, OutTarget.UsageId))
				{
					OutError = TEXT("niagara_script_usage_id_invalid");
					return false;
				}
				OutTarget.bUsageIdExplicit = true;
			}
			return true;
		}

		TSharedRef<FJsonObject> MergeScriptReference(
			const TSharedPtr<const FJsonObject>& Target,
			const TSharedPtr<const FJsonObject>& Extension)
		{
			TSharedRef<FJsonObject> Result = MakeShared<FJsonObject>();
			for (const TSharedPtr<const FJsonObject>& Source : { Target, Extension })
			{
				if (!Source.IsValid()) continue;
				for (const TPair<FString, TSharedPtr<FJsonValue>>& Pair : Source->Values) Result->SetField(Pair.Key, Pair.Value);
			}
			return Result;
		}

		FInputFieldDescriptor Field(const TCHAR* Name, const TCHAR* Role, const TCHAR* TypeId)
		{
			FInputFieldDescriptor Result;
			Result.Name = Name;
			Result.Role = Role;
			Result.TypeId = TypeId;
			return Result;
		}

		FOperationDescriptor OperationDescriptor(const EOperation Operation)
		{
			FOperationDescriptor Result;
			Result.Operation = Operation;
			Result.IdentityOneOf = { { TEXT("asset_path") } };
			Result.AcceptedSections = { TEXT("target"), TEXT("provider_extension") };
			Result.OmittedFieldPolicy = Operation == EOperation::Update
				? TEXT("preserve_current")
				: Operation == EOperation::Remove
					? TEXT("not_applicable")
					: TEXT("ue_native_default");
			Result.NativeDefaultPolicy = Operation == EOperation::Update
				? TEXT("forbidden")
				: Operation == EOperation::Remove
					? TEXT("not_applicable")
					: TEXT("allowed");
			Result.bDestructive = Operation == EOperation::Remove;
			return Result;
		}

		FContract MakeInputContract(const EStackTargetKind TargetKind)
		{
			UeAgentApplyInputContract::FContract Result;
			Result.Schema = TEXT("uai.apply_input_contract.v1");
			Result.ContractId = TargetKind == EStackTargetKind::Module
				? ModuleInputContractId
				: DynamicInputInputContractId;
			Result.ContractVersion = TEXT("1.0.0");
			Result.ProfileId = TargetKind == EStackTargetKind::Module
				? TEXT("niagara.module")
				: TEXT("niagara.dynamic_input");
			Result.TargetKind = TargetKind == EStackTargetKind::Module
				? TEXT("niagara_module")
				: TEXT("niagara_dynamic_input");
			Result.SemanticDescriptorRefs = TargetKind == EStackTargetKind::Module
				? TArray<FString>{ StackSemanticId, ModuleSemanticId }
				: TArray<FString>{ StackSemanticId, DynamicInputSemanticId };
			Result.ReflectionBaseline.bEnabled = true;
			Result.ReflectionBaseline.RootKinds = { TEXT("uobject"), TEXT("ustruct") };
			Result.ReflectionBaseline.RecursiveNodes = {
				TEXT("scalar"), TEXT("struct"), TEXT("owned_object"), TEXT("reference"),
				TEXT("array"), TEXT("set"), TEXT("map"), TEXT("fixed_array"), TEXT("optional")
			};
			Result.ReflectionBaseline.UnknownFieldPolicy = TEXT("reject");
			Result.ReflectionBaseline.ExternalReferencePolicy = TEXT("replace_reference_only");
			Result.ReflectionBaseline.OwnedObjectPolicy = TEXT("create_or_locate_with_real_outer_then_recurse");
			Result.ReflectionBaseline.RuntimeLimitsSource = TEXT("shared_runtime_limits");
			Result.PropertyPayload.Mode = TEXT("reflection");
			Result.PropertyPayload.PropertyPathField = TEXT("property_path");
			Result.PropertyPayload.ValueField = TEXT("value");
			Result.PropertyPayload.OptionalAssertionFields = {
				TEXT("value_kind"), TEXT("struct_path"), TEXT("enum_path"), TEXT("element_type"),
				TEXT("key_type"), TEXT("mapped_type"), TEXT("inner_type"), TEXT("fixed_length"), TEXT("allowed_class")
			};
			Result.PropertyPayload.OmittedPatchFieldPolicy = TEXT("preserve_current");
			Result.InputFields = {
				Field(TEXT("asset_path"), TEXT("identity"), TEXT("ue_asset_path")),
				Field(TEXT("emitter_handle_id"), TEXT("selector"), TEXT("guid")),
				Field(TEXT("emitter_name"), TEXT("selector"), TEXT("string")),
				Field(TEXT("usage"), TEXT("selector"), TEXT("niagara_script_usage")),
				Field(TEXT("usage_id"), TEXT("selector"), TEXT("guid")),
				Field(TEXT("module_guid"), TEXT("selector"), TEXT("guid")),
				Field(TEXT("module_selector_script_asset_path"), TEXT("selector"), TEXT("ue_asset_path")),
				Field(TEXT("module_name"), TEXT("selector"), TEXT("string")),
				Field(TEXT("occurrence"), TEXT("selector"), TEXT("int32")),
			};
			if (TargetKind == EStackTargetKind::Module)
			{
				Result.InputFields.Append({
					Field(TEXT("source_module_guid"), TEXT("selector"), TEXT("guid")),
					Field(TEXT("source_module_selector_script_asset_path"), TEXT("selector"), TEXT("ue_asset_path")),
					Field(TEXT("source_module_name"), TEXT("selector"), TEXT("string")),
					Field(TEXT("source_occurrence"), TEXT("selector"), TEXT("int32")),
				});
			}
			else
			{
				Result.InputFields.Append({
					Field(TEXT("input_name"), TEXT("selector"), TEXT("string")),
					Field(TEXT("dynamic_input_guid"), TEXT("selector"), TEXT("guid")),
				});
			}
			Result.InputFields.Append({
				Field(TEXT("script_asset_path"), TEXT("provider_extension"), TEXT("ue_asset_path")),
				Field(TEXT("path"), TEXT("provider_extension"), TEXT("ue_asset_path")),
				Field(TEXT("catalog_name"), TEXT("provider_extension"), TEXT("string")),
				Field(TEXT("name"), TEXT("provider_extension"), TEXT("string")),
				Field(TEXT("suggested_name"), TEXT("provider_extension"), TEXT("string")),
				Field(TEXT("compile"), TEXT("provider_extension"), TEXT("bool")),
				Field(TEXT("save"), TEXT("provider_extension"), TEXT("bool")),
			});
			Result.InputFields.Add(TargetKind == EStackTargetKind::Module
				? Field(TEXT("module_script_path"), TEXT("provider_extension"), TEXT("ue_asset_path"))
				: Field(TEXT("dynamic_input_script_path"), TEXT("provider_extension"), TEXT("ue_asset_path")));
			Result.InputFields.Add(TargetKind == EStackTargetKind::Module
				? Field(TEXT("module_script_asset_path"), TEXT("provider_extension"), TEXT("ue_asset_path"))
				: Field(TEXT("dynamic_input_script_asset_path"), TEXT("provider_extension"), TEXT("ue_asset_path")));
			if (TargetKind == EStackTargetKind::Module)
			{
				Result.InputFields.Add(Field(TEXT("target_index"), TEXT("provider_extension"), TEXT("int32")));
			}
			TArray<EOperation> SupportedOperations = {
				EOperation::Create, EOperation::Update, EOperation::Upsert, EOperation::Remove
			};
			if (TargetKind == EStackTargetKind::Module) SupportedOperations.Add(EOperation::Clone);
			for (const EOperation Operation : SupportedOperations)
			{
				Result.Operations.Add(OperationDescriptor(Operation));
			}
			Result.ProviderId = ApplyProviderId;
			Result.UnknownInputFieldPolicy = TEXT("reject");
			Result.Verification.StateSource = TEXT("shared_state_snapshot");
			Result.Verification.CompareSource = TEXT("shared_semantic_compare");
			Result.Verification.bRequired = true;
			return Result;
		}

		UeAgentApplyWorkspaceContract::FContract MakeWorkspaceContract(const EStackTargetKind TargetKind)
		{
			using namespace UeAgentApplyWorkspaceContract;
			UeAgentApplyWorkspaceContract::FContract Result;
			Result.Schema = TEXT("uai.apply_workspace_contract.v1");
			Result.ContractId = TargetKind == EStackTargetKind::Module
				? ModuleWorkspaceContractId
				: DynamicInputWorkspaceContractId;
			Result.ContractVersion = TEXT("1.0.0");
			Result.ProfileId = TargetKind == EStackTargetKind::Module
				? TEXT("niagara.module")
				: TEXT("niagara.dynamic_input");
			Result.Sources = {
				{ ESourceKind::Directory, TEXT("physical"), TEXT("index") },
				{ ESourceKind::SingleFile, TEXT("virtual"), TEXT("object") },
				{ ESourceKind::Inline, TEXT("virtual"), TEXT("object") },
			};
			Result.Entry.DirectoryEntry = TEXT("index.hjson");
			Result.Entry.VirtualUnitId = TargetKind == EStackTargetKind::Module ? TEXT("module") : TEXT("dynamic_input");
			FUnitDescriptor Unit;
			Unit.UnitId = Result.Entry.VirtualUnitId;
			Unit.PathPattern = TEXT("index.hjson");
			Unit.Role = TEXT("object");
			Unit.Cardinality = TEXT("one");
			Unit.bRequired = true;
			Unit.RouteId = TargetKind == EStackTargetKind::Module ? TEXT("niagara.module") : TEXT("niagara.dynamic_input");
			Unit.ApplyContractRef = TargetKind == EStackTargetKind::Module
				? ModuleInputContractId
				: DynamicInputInputContractId;
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
				TEXT("mutation_failed"), TEXT("compile_failed"), TEXT("save_failed"), TEXT("verification_failed")
			};
			Result.Execution.Compile = TEXT("provider_hook");
			Result.Execution.Save = TEXT("caller_controlled");
			Result.Execution.Verification = TEXT("shared_snapshot_required");
			Result.UnknownFilePolicy = TEXT("reject");
			Result.DuplicateTargetPolicy = TEXT("reject");
			return Result;
		}

		class FStackPreparedState final : public IPreparedOperationState
		{
		public:
			FStageTarget Target;
			FResolvedStage Stage;
			EStackTargetKind TargetKind = EStackTargetKind::Module;
			EOperation Operation = EOperation::Update;
			FModuleSelector ModuleSelector;
			FModuleSelector SourceModuleSelector;
			TStrongObjectPtr<UNiagaraScript> Script;
			FString InputName;
			FString SuggestedName;
			int32 TargetIndex = INDEX_NONE;
			bool bCompile = false;
			bool bSave = false;

			virtual FString GetTargetKey() const override
			{
				return Target.AssetPath + TEXT("|") + Stage.StageKey + TEXT("|")
					+ (TargetKind == EStackTargetKind::Module ? TEXT("module") : TEXT("dynamic_input"))
					+ TEXT("|") + InputName + TEXT("|") + ModuleSelector.NodeGuid.ToString(EGuidFormats::Digits);
			}

			virtual EOperation GetResolvedOperation() const override { return Operation; }

			virtual void CollectRollbackObjects(TArray<UObject*>& OutObjects) const override
			{
				if (Stage.Asset) OutObjects.AddUnique(Stage.Asset);
			}

			virtual bool CaptureRollback(FOperationFailure& OutFailure) override
			{
				OutFailure = FOperationFailure();
				if (!Stage.Graph || !Stage.ScriptSource)
				{
					SetFailure(OutFailure, TEXT("niagara_stack_rollback_context_invalid"));
					return false;
				}
				BackupGraph.Reset(DuplicateObject<UNiagaraGraph>(Stage.Graph, GetTransientPackage()));
				if (!BackupGraph.IsValid())
				{
					SetFailure(OutFailure, TEXT("niagara_stack_rollback_capture_failed"));
					return false;
				}
				return true;
			}

			virtual bool ApplyProposed(FOperationFailure& OutFailure) override
			{
				OutFailure = FOperationFailure();
				if (!Stage.Asset || !Stage.Graph || !Stage.OutputNode)
				{
					SetFailure(OutFailure, TEXT("niagara_stack_apply_context_invalid"));
					return false;
				}
				Stage.Asset->Modify();
				Stage.ScriptSource->Modify();
				Stage.Graph->Modify();
				Stage.OutputNode->Modify();
				FString Error;
				if (TargetKind == EStackTargetKind::Module)
				{
					if (Operation == EOperation::Create || Operation == EOperation::Clone)
					{
						UNiagaraScript* SourceScript = Script.Get();
						if (Operation == EOperation::Clone)
						{
							UNiagaraNodeFunctionCall* SourceModule = nullptr;
							int32 SourceIndex = INDEX_NONE;
							if (!ResolveModule(*Stage.OutputNode, SourceModuleSelector, SourceModule, SourceIndex, Error))
							{
								SetFailure(OutFailure, Error);
								return false;
							}
							SourceScript = SourceModule->FunctionScript;
						}
						AppliedModule = SourceScript
							? AddModule(*Stage.OutputNode, *SourceScript, SuggestedName, TargetIndex, Error)
							: nullptr;
						if (!AppliedModule)
						{
							SetFailure(OutFailure, Error.IsEmpty() ? TEXT("niagara_module_script_missing") : Error);
							return false;
						}
						ModuleSelector = FModuleSelector();
						ModuleSelector.NodeGuid = AppliedModule->NodeGuid;
					}
					else
					{
						int32 CurrentIndex = INDEX_NONE;
						if (!ResolveModule(*Stage.OutputNode, ModuleSelector, AppliedModule, CurrentIndex, Error))
						{
							SetFailure(OutFailure, Error);
							return false;
						}
						if (Operation == EOperation::Remove)
						{
							if (!RemoveModule(*Stage.OutputNode, *AppliedModule, Error))
							{
								SetFailure(OutFailure, Error);
								return false;
							}
						}
						else if (TargetIndex != INDEX_NONE)
						{
							if (!MoveModule(*Stage.OutputNode, *AppliedModule, TargetIndex, AppliedModuleIndex, Error))
							{
								SetFailure(OutFailure, Error);
								return false;
							}
						}
						else
						{
							SetFailure(OutFailure, TEXT("niagara_module_update_intent_missing"));
							return false;
						}
					}
					if (Operation != EOperation::Remove)
					{
						TArray<UNiagaraNodeFunctionCall*> Modules;
						GetModules(*Stage.OutputNode, Modules);
						AppliedModuleIndex = Modules.IndexOfByKey(AppliedModule);
					}
				}
				else
				{
					int32 ModuleIndex = INDEX_NONE;
					if (!ResolveModule(*Stage.OutputNode, ModuleSelector, AppliedModule, ModuleIndex, Error))
					{
						SetFailure(OutFailure, Error);
						return false;
					}
					if (Operation == EOperation::Remove)
					{
						if (!ClearDynamicInput(*AppliedModule, InputName, ResolvedInputName, Error))
						{
							SetFailure(OutFailure, Error);
							return false;
						}
					}
					else if (!Script.IsValid()
						|| !SetDynamicInput(*AppliedModule, InputName, *Script, SuggestedName, AppliedDynamicInput, ResolvedInputName, Error))
					{
						SetFailure(OutFailure, Error.IsEmpty() ? TEXT("niagara_dynamic_input_script_missing") : Error);
						return false;
					}
				}
				Stage.Graph->NotifyGraphChanged();
				Stage.Asset->MarkPackageDirty();
				return true;
			}

			virtual bool BuildExpectedReadRequests(
				TArray<UeAgentAuthoringState::FReadRequest>& OutRequests,
				FOperationFailure& OutFailure) const override
			{
				return BuildReadRequest(OutRequests, OutFailure, Operation == EOperation::Remove);
			}

			virtual bool ApplyLifecycle(FOperationFailure& OutFailure) override
			{
				OutFailure = FOperationFailure();
				return true;
			}

			virtual bool Compile(FOperationFailure& OutFailure) override
			{
				OutFailure = FOperationFailure();
				if (!bCompile) return true;
				FString Error;
				if (!CompileAsset(Stage.Asset, Error))
				{
					SetFailure(OutFailure, Error);
					return false;
				}
				return true;
			}

			virtual bool Save(FOperationFailure& OutFailure) override
			{
				OutFailure = FOperationFailure();
				if (!bSave) return true;
				FString Error;
				if (!SaveAsset(Stage.Asset, Error))
				{
					SetFailure(OutFailure, Error);
					return false;
				}
				return true;
			}

			virtual bool BuildDurablePackagePlans(
				TArray<FPackageDurablePlan>& OutPlans,
				FOperationFailure& OutFailure) const override
			{
				UPackage* Package = Stage.Asset ? Stage.Asset->GetOutermost() : nullptr;
				return BuildPackageDurablePlan(
					GetTargetKey(),
					Package ? Package->GetName() : FString(),
					Stage.Asset,
					EPackageDurableIntent::Save,
					OutPlans,
					OutFailure);
			}

			virtual bool BuildActualReadRequests(
				TArray<UeAgentAuthoringState::FReadRequest>& OutRequests,
				FOperationFailure& OutFailure) const override
			{
				return BuildReadRequest(OutRequests, OutFailure, false);
			}

			virtual bool Rollback(FOperationFailure& OutFailure) override
			{
				OutFailure = FOperationFailure();
				if (!BackupGraph.IsValid() || !Stage.ScriptSource || !Stage.Graph)
				{
					SetFailure(OutFailure, TEXT("niagara_stack_rollback_state_invalid"));
					return false;
				}
				UNiagaraGraph* MutatedGraph = Stage.Graph;
				const FName OriginalName = MutatedGraph->GetFName();
				const FString RetiredName = FString::Printf(TEXT("%s_UAIRetired_%s"), *OriginalName.ToString(), *FGuid::NewGuid().ToString(EGuidFormats::Digits));
				if (!MutatedGraph->Rename(*RetiredName, GetTransientPackage(), REN_DontCreateRedirectors | REN_NonTransactional))
				{
					SetFailure(OutFailure, TEXT("niagara_stack_rollback_retire_graph_failed"));
					return false;
				}
				UNiagaraGraph* Restored = DuplicateObject<UNiagaraGraph>(BackupGraph.Get(), Stage.ScriptSource, OriginalName);
				if (!Restored)
				{
					SetFailure(OutFailure, TEXT("niagara_stack_rollback_restore_graph_failed"));
					return false;
				}
				Stage.ScriptSource->NodeGraph = Restored;
				Stage.Graph = Restored;
				Stage.Asset->MarkPackageDirty();
				return true;
			}

		private:
			bool BuildReadRequest(
				TArray<UeAgentAuthoringState::FReadRequest>& OutRequests,
				FOperationFailure& OutFailure,
				const bool bExpectedAbsent) const
			{
				TMap<FString, FString> Selectors = {
					{ TEXT("asset_path"), Target.AssetPath },
					{ TEXT("stage_owner_id"), Stage.Emitter
						? (Stage.EmitterHandleId.IsValid()
							? Stage.EmitterHandleId.ToString(EGuidFormats::DigitsWithHyphensLower)
							: FString(TEXT("standalone")))
						: FString(TEXT("system")) },
					{ TEXT("usage"), ScriptUsageToString(Stage.Usage) },
					{ TEXT("usage_id"), Stage.UsageId.ToString(EGuidFormats::DigitsWithHyphensLower) },
				};
				if (Target.EmitterHandleId.IsValid())
				{
					Selectors.Add(TEXT("emitter_handle_id"), Target.EmitterHandleId.ToString(EGuidFormats::DigitsWithHyphensLower));
				}
				if (!Target.EmitterName.IsEmpty()) Selectors.Add(TEXT("emitter_name"), Target.EmitterName);
				FString ModelId;
				FString TargetKindText;
				FString SemanticId;
				if (TargetKind == EStackTargetKind::Module)
				{
					ModelId = TEXT("niagara_module_identity");
					TargetKindText = TEXT("niagara_module");
					SemanticId = ModuleSemanticId;
					Selectors.Add(TEXT("module_guid"), ModuleSelector.NodeGuid.ToString(EGuidFormats::DigitsWithHyphensLower));
				}
				else
				{
					ModelId = TEXT("niagara_dynamic_input_identity");
					TargetKindText = TEXT("niagara_dynamic_input");
					SemanticId = DynamicInputSemanticId;
					Selectors.Add(TEXT("module_guid"), ModuleSelector.NodeGuid.ToString(EGuidFormats::DigitsWithHyphensLower));
					Selectors.Add(TEXT("input_name"), ResolvedInputName.IsEmpty() ? InputName : ResolvedInputName);
					if (AppliedDynamicInput)
					{
						Selectors.Add(TEXT("dynamic_input_guid"), AppliedDynamicInput->NodeGuid.ToString(EGuidFormats::DigitsWithHyphensLower));
					}
				}
				UeAgentAuthoringIdentity::FCanonicalIdentity Identity;
				FString Error;
				if (!UeAgentAuthoringIdentity::BuildCanonicalIdentity(ModelId, TargetKindText, Selectors, Identity, Error))
				{
					SetFailure(OutFailure, TEXT("niagara_stack_identity_invalid"), Error);
					return false;
				}
				UeAgentAuthoringState::FReadRequest Request;
				Request.SemanticId = SemanticId;
				Request.TargetIdentity = Identity;
				Request.TargetQuery.ModelId = Identity.ModelId;
				Request.TargetQuery.TargetKind = TargetKindText;
				Request.TargetQuery.CanonicalKey = Identity.CanonicalKey;
				Request.RootObject = Stage.Asset;
				Request.RootOwnerPath = Target.AssetPath;
				if (bExpectedAbsent) Request.Context.Add(TEXT("expected_absent"), TEXT("true"));
				OutRequests.Add(MoveTemp(Request));
				return true;
			}

			TStrongObjectPtr<UNiagaraGraph> BackupGraph;
			UNiagaraNodeFunctionCall* AppliedModule = nullptr;
			UNiagaraNodeFunctionCall* AppliedDynamicInput = nullptr;
			FString ResolvedInputName;
			int32 AppliedModuleIndex = INDEX_NONE;
		};

		class FStackApplyProvider final : public IApplyProvider
		{
		public:
			virtual FString GetProviderId() const override { return ApplyProviderId; }

			virtual bool PrepareOperation(
				const FPrepareRequest& Request,
				TUniquePtr<IPreparedOperationState>& OutState,
				FOperationFailure& OutFailure) const override
			{
				OutState.Reset();
				OutFailure = FOperationFailure();
				if (!Request.IsValid() || !Request.Input->Target.IsValid())
				{
					SetFailure(OutFailure, TEXT("niagara_stack_apply_request_invalid"));
					return false;
				}
				TUniquePtr<FStackPreparedState> State = MakeUnique<FStackPreparedState>();
				if (Request.Contract->ContractId == ModuleInputContractId)
				{
					State->TargetKind = EStackTargetKind::Module;
				}
				else if (Request.Contract->ContractId == DynamicInputInputContractId)
				{
					State->TargetKind = EStackTargetKind::DynamicInput;
				}
				else
				{
					SetFailure(OutFailure, TEXT("niagara_stack_contract_unsupported"), Request.Contract->ContractId);
					return false;
				}
				FString Error;
				if (!ParseStageTarget(Request.Input->Target, State->Target, Error)
					|| !ResolveStage(State->Target, State->Stage, Error)
					|| !ParseModuleSelector(Request.Input->Target, FString(), State->ModuleSelector, Error)
					|| !ParseModuleSelector(Request.Input->Target, TEXT("source_"), State->SourceModuleSelector, Error))
				{
					SetFailure(OutFailure, Error);
					return false;
				}
				State->Operation = Request.Input->RequestedOperation;
				ReadString(Request.Input->Target, TEXT("input_name"), State->InputName);
				State->TargetIndex = ReadIndex(Request.Input->ProviderExtension, TEXT("target_index"));
				ReadString(Request.Input->ProviderExtension, TEXT("suggested_name"), State->SuggestedName);
				State->bCompile = ReadBool(Request.Input->ProviderExtension, TEXT("compile"), false);
				State->bSave = ReadBool(Request.Input->ProviderExtension, TEXT("save"), false);

				UNiagaraNodeFunctionCall* ExistingModule = nullptr;
				int32 ExistingIndex = INDEX_NONE;
				const bool bModuleExists = !State->ModuleSelector.IsEmpty()
					&& ResolveModule(*State->Stage.OutputNode, State->ModuleSelector, ExistingModule, ExistingIndex, Error);
				if (Request.Input->RequestedOperation == EOperation::Upsert)
				{
					State->Operation = bModuleExists ? EOperation::Update : EOperation::Create;
				}
				if (State->TargetKind == EStackTargetKind::Module)
				{
					if ((State->Operation == EOperation::Update || State->Operation == EOperation::Remove) && !bModuleExists)
					{
						SetFailure(OutFailure, Error.IsEmpty() ? TEXT("module_selector_not_found") : Error);
						return false;
					}
					if (State->Operation == EOperation::Clone)
					{
						UNiagaraNodeFunctionCall* SourceModule = nullptr;
						int32 SourceIndex = INDEX_NONE;
						if (!ResolveModule(*State->Stage.OutputNode, State->SourceModuleSelector, SourceModule, SourceIndex, Error))
						{
							SetFailure(OutFailure, Error);
							return false;
						}
					}
				}
				else
				{
					if (!bModuleExists || State->InputName.IsEmpty())
					{
						SetFailure(OutFailure, bModuleExists ? TEXT("niagara_dynamic_input_name_missing") : TEXT("module_selector_not_found"));
						return false;
					}
					FDynamicInputSelector DynamicSelector;
					DynamicSelector.Module = State->ModuleSelector;
					DynamicSelector.InputName = State->InputName;
					ParseGuidField(Request.Input->Target, TEXT("dynamic_input_guid"), DynamicSelector.NodeGuid, Error);
					UNiagaraNodeFunctionCall* ExistingDynamicInput = nullptr;
					FString ExistingInputName;
					const bool bDynamicExists = ResolveDynamicInput(
						State->Stage, DynamicSelector, ExistingDynamicInput, ExistingInputName, Error);
					if (Request.Input->RequestedOperation == EOperation::Upsert)
					{
						State->Operation = bDynamicExists ? EOperation::Update : EOperation::Create;
					}
					if (State->Operation == EOperation::Create && bDynamicExists)
					{
						SetFailure(OutFailure, TEXT("niagara_dynamic_input_already_exists"));
						return false;
					}
					if ((State->Operation == EOperation::Update || State->Operation == EOperation::Remove) && !bDynamicExists)
					{
						SetFailure(OutFailure, TEXT("niagara_dynamic_input_not_found"));
						return false;
					}
					if (State->Operation == EOperation::Clone)
					{
						SetFailure(OutFailure, TEXT("niagara_dynamic_input_clone_unsupported"));
						return false;
					}
				}

				if (State->Operation == EOperation::Create || State->Operation == EOperation::Update)
				{
					FScriptResolveRequest ResolveRequest;
					ResolveRequest.Kind = State->TargetKind == EStackTargetKind::Module
						? EScriptKind::Module
						: EScriptKind::DynamicInput;
					ResolveRequest.Input = MergeScriptReference(nullptr, Request.Input->ProviderExtension);
					FScriptResolveResult ResolveResult;
					if (!ResolveScript(ResolveRequest, ResolveResult, Error))
					{
						if (!(State->TargetKind == EStackTargetKind::Module && State->Operation == EOperation::Update))
						{
							SetFailure(OutFailure, Error);
							return false;
						}
					}
					else
					{
						State->Script.Reset(ResolveResult.Script);
					}
				}
				OutState = MoveTemp(State);
				return true;
			}
		};

		TSharedRef<FJsonObject> MakeApplyDocument(const FStackApplyRequest& Request)
		{
			TSharedRef<FJsonObject> Target = MakeShared<FJsonObject>();
			Target->SetStringField(TEXT("asset_path"), Request.Stage.AssetPath);
			if (Request.Stage.EmitterHandleId.IsValid()) Target->SetStringField(TEXT("emitter_handle_id"), Request.Stage.EmitterHandleId.ToString(EGuidFormats::DigitsWithHyphensLower));
			if (!Request.Stage.EmitterName.IsEmpty()) Target->SetStringField(TEXT("emitter_name"), Request.Stage.EmitterName);
			if (Request.Stage.bUsageExplicit) Target->SetStringField(TEXT("usage"), ScriptUsageToString(Request.Stage.Usage));
			if (Request.Stage.bUsageIdExplicit) Target->SetStringField(TEXT("usage_id"), Request.Stage.UsageId.ToString(EGuidFormats::DigitsWithHyphensLower));
			if (Request.Module.NodeGuid.IsValid()) Target->SetStringField(TEXT("module_guid"), Request.Module.NodeGuid.ToString(EGuidFormats::DigitsWithHyphensLower));
			if (!Request.Module.ScriptPath.IsEmpty()) Target->SetStringField(TEXT("module_selector_script_asset_path"), Request.Module.ScriptPath);
			if (!Request.Module.Name.IsEmpty()) Target->SetStringField(TEXT("module_name"), Request.Module.Name);
			if (Request.Module.bOccurrenceExplicit) Target->SetNumberField(TEXT("occurrence"), Request.Module.Occurrence);
			if (Request.SourceModule.NodeGuid.IsValid()) Target->SetStringField(TEXT("source_module_guid"), Request.SourceModule.NodeGuid.ToString(EGuidFormats::DigitsWithHyphensLower));
			if (!Request.SourceModule.ScriptPath.IsEmpty()) Target->SetStringField(TEXT("source_module_selector_script_asset_path"), Request.SourceModule.ScriptPath);
			if (!Request.SourceModule.Name.IsEmpty()) Target->SetStringField(TEXT("source_module_name"), Request.SourceModule.Name);
			if (Request.SourceModule.bOccurrenceExplicit) Target->SetNumberField(TEXT("source_occurrence"), Request.SourceModule.Occurrence);
			if (!Request.InputName.IsEmpty()) Target->SetStringField(TEXT("input_name"), Request.InputName);

			TSharedRef<FJsonObject> Extension = MakeShared<FJsonObject>();
			if (Request.ScriptReference.IsValid())
			{
				static const TSet<FString> ScriptFields = {
					TEXT("path"), TEXT("script_asset_path"), TEXT("module_script_path"), TEXT("module_script_asset_path"),
					TEXT("dynamic_input_script_path"), TEXT("dynamic_input_script_asset_path"),
					TEXT("catalog_name"), TEXT("name")
				};
				for (const TPair<FString, TSharedPtr<FJsonValue>>& Pair : Request.ScriptReference->Values)
				{
					if (ScriptFields.Contains(Pair.Key)) Extension->SetField(Pair.Key, Pair.Value);
				}
			}
			if (Request.TargetIndex != INDEX_NONE) Extension->SetNumberField(TEXT("target_index"), Request.TargetIndex);
			if (!Request.SuggestedName.IsEmpty()) Extension->SetStringField(TEXT("suggested_name"), Request.SuggestedName);
			Extension->SetBoolField(TEXT("compile"), Request.bCompile);
			Extension->SetBoolField(TEXT("save"), Request.bSave);

			TSharedRef<FJsonObject> Document = MakeShared<FJsonObject>();
			Document->SetStringField(TEXT("operation"), OperationToString(Request.Operation));
			Document->SetObjectField(TEXT("target"), Target);
			Document->SetObjectField(TEXT("provider_extension"), Extension);
			return Document;
		}
	}

	bool RegisterStackApplyContracts(FString& OutError)
	{
		for (const EStackTargetKind TargetKind : { EStackTargetKind::Module, EStackTargetKind::DynamicInput })
		{
			UeAgentApplyInputContract::ERegistrationResult InputResult;
			if (!UeAgentApplyInputContract::FRegistry::Get().RegisterContract(MakeInputContract(TargetKind), InputResult, OutError)) return false;
			UeAgentApplyWorkspaceContract::ERegistrationResult WorkspaceResult;
			if (!UeAgentApplyWorkspaceContract::FRegistry::Get().RegisterContract(MakeWorkspaceContract(TargetKind), WorkspaceResult, OutError)) return false;
		}
		return true;
	}

	TSharedRef<IApplyProvider> MakeStackApplyProvider()
	{
		return MakeShared<FStackApplyProvider>();
	}

	bool ApplyStack(
		const FStackApplyRequest& Request,
		FStackApplyResult& OutResult,
		const FString& RequestId)
	{
		OutResult = FStackApplyResult();
		OutResult.Status = TEXT("failed");
		OutResult.AssetPath = Request.Stage.AssetPath;
		UeAgentApplyWorkspaceContract::FContract WorkspaceContract;
		const TCHAR* WorkspaceContractId = Request.TargetKind == EStackTargetKind::Module
			? ModuleWorkspaceContractId
			: DynamicInputWorkspaceContractId;
		if (!UeAgentApplyWorkspaceContract::FRegistry::Get().FindContract(WorkspaceContractId, WorkspaceContract))
		{
			OutResult.Phase = TEXT("registration");
			OutResult.ErrorCode = TEXT("niagara_stack_workspace_contract_missing");
			OutResult.ErrorDetail = OutResult.ErrorCode;
			return false;
		}
		UeAgentAuthoringWorkspaceLoader::FLoadResult LoadResult;
		if (!UeAgentAuthoringWorkspaceLoader::LoadWorkspace(
			WorkspaceContract,
			UeAgentAuthoringWorkspaceLoader::FSourceRequest::MakeInlineObject(
				MakeApplyDocument(Request), TEXT("inline://niagara-stack-apply")),
			LoadResult))
		{
			OutResult.Phase = TEXT("load");
			OutResult.ErrorCode = LoadResult.ErrorCode;
			OutResult.ErrorDetail = LoadResult.ErrorDetail;
			return false;
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
		FBackendOptions BackendOptions;
		BackendOptions.TransactionDescription = NSLOCTEXT("UeAgentInterface", "NiagaraStackApply", "Apply Niagara stack operation");
		UeAgentAuthoringWorkspaceTransaction::FExecutionOptions ExecutionOptions;
		FOperationFailure RequestFailure;
		if (!BuildApplyEntryExecutionOptions(RequestId, ExecutionOptions, RequestFailure))
		{
			OutResult.Phase = TEXT("preflight");
			OutResult.ErrorCode = RequestFailure.ErrorCode;
			OutResult.ErrorDetail = RequestFailure.ErrorDetail;
			return false;
		}
		ExecutionOptions.bSaveWhenCallerControlled = Request.bSave;
		FApplyWorkspaceResult ApplyResult;
		if (!ApplyWorkspace(
			WorkspaceContract,
			LoadResult.Workspace,
			Graph,
			UeAgentApplyInputContract::FRegistry::Get(),
			FProviderRegistry::Get(),
			ExecutionOptions,
			BackendOptions,
			ApplyResult))
		{
			const bool bPreflight = !ApplyResult.Preflight.IsReady();
			OutResult.Phase = bPreflight ? TEXT("preflight") : ApplyResult.Execution.Phase;
			OutResult.ErrorCode = bPreflight ? ApplyResult.Preflight.CauseCode : ApplyResult.Execution.CauseCode;
			if (OutResult.ErrorCode.IsEmpty()) OutResult.ErrorCode = bPreflight ? ApplyResult.Preflight.ErrorCode : ApplyResult.Execution.ErrorCode;
			OutResult.ErrorDetail = bPreflight ? ApplyResult.Preflight.ErrorDetail : ApplyResult.Execution.ErrorDetail;
			OutResult.bRollbackAttempted = ApplyResult.Execution.bRollbackAttempted;
			OutResult.bRollbackSucceeded = ApplyResult.Execution.bRollbackSucceeded;
			return false;
		}
		OutResult.Status = TEXT("applied");
		OutResult.Phase = TEXT("complete");
		OutResult.ResolvedOperation = OperationToString(Request.Operation);
		FResolvedStage Stage;
		FString ResolveError;
		if (ResolveStage(Request.Stage, Stage, ResolveError))
		{
			OutResult.StageKey = Stage.StageKey;
			UNiagaraNodeFunctionCall* Module = nullptr;
			int32 ModuleIndex = INDEX_NONE;
			FModuleSelector Selector = Request.Module;
			if ((Request.Operation == EOperation::Create || Request.Operation == EOperation::Clone)
				&& Request.TargetKind == EStackTargetKind::Module)
			{
				TArray<UNiagaraNodeFunctionCall*> Modules;
				GetModules(*Stage.OutputNode, Modules);
				const int32 Index = Request.TargetIndex == INDEX_NONE ? Modules.Num() - 1 : FMath::Clamp(Request.TargetIndex, 0, Modules.Num() - 1);
				Module = Modules.IsValidIndex(Index) ? Modules[Index] : nullptr;
				ModuleIndex = Index;
			}
			else
			{
				ResolveModule(*Stage.OutputNode, Selector, Module, ModuleIndex, ResolveError);
			}
			if (Module)
			{
				OutResult.TargetGuid = Module->NodeGuid.ToString(EGuidFormats::DigitsWithHyphensLower);
				OutResult.ModuleIndex = ModuleIndex;
				if (Request.TargetKind == EStackTargetKind::DynamicInput)
				{
					FDynamicInputSelector DynamicSelector;
					DynamicSelector.Module.NodeGuid = Module->NodeGuid;
					DynamicSelector.InputName = Request.InputName;
					UNiagaraNodeFunctionCall* DynamicInput = nullptr;
					ResolveDynamicInput(Stage, DynamicSelector, DynamicInput, OutResult.ResolvedInputName, ResolveError);
					if (DynamicInput) OutResult.TargetGuid = DynamicInput->NodeGuid.ToString(EGuidFormats::DigitsWithHyphensLower);
				}
			}
		}
		return true;
	}
}
