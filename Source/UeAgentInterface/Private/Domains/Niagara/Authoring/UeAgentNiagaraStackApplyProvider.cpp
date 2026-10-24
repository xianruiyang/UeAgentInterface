// Copyright Epic Games, Inc. All Rights Reserved.

#include "Domains/Niagara/Authoring/UeAgentNiagaraStackApplyProvider.h"

#include "Utilities/AuthoringCore/UeAgentAuthoringDiagnostics.h"

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
#include "Utilities/AuthoringCore/UeAgentAuthoringJsonFields.h"
#include "Utilities/AuthoringOperationApply/UeAgentAuthoringOperationCoordinator.h"
#include "Utilities/AuthoringWorkspaceApply/UeAgentApplyWorkspaceContract.h"
#include "Utilities/AuthoringWorkspaceApply/UeAgentAuthoringAssetWorkspaceApply.h"
#include "Utilities/AuthoringWorkspaceApply/UeAgentAuthoringWorkspaceDependencyGraph.h"
#include "Utilities/AuthoringWorkspaceApply/UeAgentAuthoringWorkspaceLoader.h"

namespace UeAgentNiagaraAuthoring
{
	namespace
	{
		using namespace UeAgentApplyInputContract;
		using namespace UeAgentAuthoringOperationApply;
		using namespace UeAgentNiagaraStageModuleAuthoring;

		using UE::UAI::Contract::FDiagnostic;

		void SetFailure(FOperationFailure& OutFailure, FDiagnostic Diagnostic,
			const FString& Subject = FString())
		{
			OutFailure.Diagnostic = MoveTemp(Diagnostic);
			if (OutFailure.Diagnostic.SubjectId.IsEmpty()) OutFailure.Diagnostic.SubjectId = Subject;
		}

		FDiagnostic StackValueFailure(const TCHAR* Code, const FString& Reason, const FString& Field,
			TSharedPtr<FJsonValue> Actual, TSharedPtr<FJsonValue> Expected, const bool bNativeResult = false)
		{
			FDiagnostic Error = FDiagnostic::Failure(Code, Reason, TEXT("authoring_apply"));
			Error.PropertyPath = Field;
			Error.ActualValue = MoveTemp(Actual);
			Error.ExpectedValue = MoveTemp(Expected);
			Error.ActualValuePresent = Error.ActualValue.IsValid();
			Error.ExpectedValuePresent = Error.ExpectedValue.IsValid();
			if (bNativeResult) Error.SourceKind = TEXT("native_result");
			return Error;
		}

		FDiagnostic StackInputFailure(const TCHAR* Code, const FString& Reason,
			const TSharedPtr<const FJsonObject>& Object, const FString& Field, const FString& Expected)
		{
			FDiagnostic Error = StackValueFailure(Code, Reason, Field,
				Object.IsValid() ? Object->TryGetField(Field) : nullptr, MakeShared<FJsonValueString>(Expected));
			if (Object.IsValid()) UeAgentAuthoringDiagnostics::LocateAuthoringSource(Error, *Object, Field);
			return Error;
		}

		FString ModuleSelectorField(const FModuleSelector& Selector, const FString& Prefix, const FDiagnostic& Error)
		{
			if (Error.PropertyPath == TEXT("occurrence") && Selector.bOccurrenceExplicit) return Prefix + TEXT("occurrence");
			if (Selector.NodeGuid.IsValid()) return Prefix + TEXT("module_guid");
			if (!Selector.ScriptPath.IsEmpty()) return Prefix + TEXT("module_selector_script_asset_path");
			if (!Selector.Name.IsEmpty()) return Prefix + TEXT("module_name");
			return FString();
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

		bool ParseGuidField(
			const TSharedPtr<const FJsonObject>& Object,
			const TCHAR* Field,
			FGuid& OutGuid,
			UE::UAI::Contract::FDiagnostic& OutError)
		{
			if (Object.IsValid() && !UeAgentAuthoringJsonFields::ValidateOptionalFields(*Object, {{Field, EJson::String}}, OutError)) return false;
			FString Text;
			if (!ReadString(Object, Field, Text)) return true;
			if (!FGuid::Parse(Text, OutGuid))
			{
				OutError = StackInputFailure(TEXT("niagara_guid_invalid"), TEXT("The supplied identity is not a valid GUID."), Object, Field, TEXT("a GUID string"));
				return false;
			}
			return true;
		}

		bool ParseModuleSelector(
			const TSharedPtr<const FJsonObject>& Object,
			const FString& Prefix,
			FModuleSelector& OutSelector,
			UE::UAI::Contract::FDiagnostic& OutError)
		{
			OutSelector = FModuleSelector();
			const FString GuidField = Prefix + TEXT("module_guid");
			if (!ParseGuidField(Object, *GuidField, OutSelector.NodeGuid, OutError)) return false;
			const TCHAR* ScriptPathField = Prefix.IsEmpty()
				? TEXT("module_selector_script_asset_path")
				: TEXT("source_module_selector_script_asset_path");
			const FString NameField = Prefix + TEXT("module_name");
			if (Object.IsValid() && !UeAgentAuthoringJsonFields::ValidateOptionalFields(*Object,
				{{ScriptPathField, EJson::String}, {*NameField, EJson::String}}, OutError)) return false;
			ReadString(Object, ScriptPathField, OutSelector.ScriptPath);
			ReadString(Object, *NameField, OutSelector.Name);
			const FString OccurrenceField = Prefix + TEXT("occurrence");
			if (Object.IsValid() && Object->HasField(OccurrenceField))
			{
				if (!UeAgentAuthoringJsonFields::ReadInt32Field(*Object, *OccurrenceField, OutSelector.Occurrence, OutError)) return false;
				OutSelector.bOccurrenceExplicit = true;
			}
			return true;
		}

		bool ParseStageTarget(
			const TSharedPtr<const FJsonObject>& Target,
			FStageTarget& OutTarget,
			UE::UAI::Contract::FDiagnostic& OutError)
		{
			OutTarget = FStageTarget();
			if (Target.IsValid() && !UeAgentAuthoringJsonFields::ValidateOptionalFields(*Target, {
				{TEXT("asset_path"), EJson::String}, {TEXT("emitter_name"), EJson::String},
				{TEXT("emitter_handle_id"), EJson::String}, {TEXT("usage"), EJson::String},
				{TEXT("usage_id"), EJson::String}}, OutError)) return false;
			if (!ReadString(Target, TEXT("asset_path"), OutTarget.AssetPath))
			{
				OutError = StackInputFailure(TEXT("niagara_stage_asset_path_missing"), TEXT("A non-empty Niagara asset path is required."), Target, TEXT("asset_path"), TEXT("a non-empty asset path"));
				return false;
			}
			ReadString(Target, TEXT("emitter_name"), OutTarget.EmitterName);
			FString EmitterHandleIdText;
			if (ReadString(Target, TEXT("emitter_handle_id"), EmitterHandleIdText))
			{
				if (!FGuid::Parse(EmitterHandleIdText, OutTarget.EmitterHandleId))
				{
					OutError = StackInputFailure(TEXT("niagara_emitter_handle_id_invalid"), TEXT("The emitter handle identity is not a valid GUID."), Target, TEXT("emitter_handle_id"), TEXT("a GUID string"));
					return false;
				}
				OutTarget.bEmitterHandleIdExplicit = true;
			}
			FString UsageText;
			if (ReadString(Target, TEXT("usage"), UsageText))
			{
				if (!TryParseScriptUsage(UsageText, OutTarget.Usage))
				{
					OutError = StackInputFailure(TEXT("niagara_script_usage_invalid"), TEXT("The script usage is not recognized."), Target, TEXT("usage"), TEXT("an ENiagaraScriptUsage enum name"));
					return false;
				}
				OutTarget.bUsageExplicit = true;
			}
			FString UsageIdText;
			if (ReadString(Target, TEXT("usage_id"), UsageIdText))
			{
				if (!FGuid::Parse(UsageIdText, OutTarget.UsageId))
				{
					OutError = StackInputFailure(TEXT("niagara_script_usage_id_invalid"), TEXT("The script usage identity is not a valid GUID."), Target, TEXT("usage_id"), TEXT("a GUID string"));
					return false;
				}
				OutTarget.bUsageIdExplicit = true;
			}
			return true;
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
			Result.PropertyPayload.Mode = TEXT("reflection");
			Result.PropertyPayload.PropertyPathField = TEXT("property_path");
			Result.PropertyPayload.ValueField = TEXT("value");
			Result.PropertyPayload.OptionalAssertionFields = {
				TEXT("kind"), TEXT("struct_path"), TEXT("enum_path"), TEXT("element_type"),
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
			FStackPreparedState(TSharedPtr<const FJsonObject> InDocument,
				TSharedPtr<const FJsonObject> InTarget, TSharedPtr<const FJsonObject> InExtension)
				: SourceDocument(MoveTemp(InDocument)), SourceTarget(MoveTemp(InTarget)), SourceExtension(MoveTemp(InExtension))
			{
				if (SourceDocument.IsValid())
				{
					UE::UAI::Contract::FSourceLocation Location;
					if (UeAgentAuthoringDiagnostics::FScopedSourceLocations::Locate(*SourceDocument, FString(), Location))
						DocumentSources.Add(TEXT("/"), MoveTemp(Location));
				}
				if (SourceTarget.IsValid()) TargetSources = UeAgentAuthoringDiagnostics::FScopedSourceLocations::CaptureProjection(*SourceTarget);
				if (SourceExtension.IsValid()) ExtensionSources = UeAgentAuthoringDiagnostics::FScopedSourceLocations::CaptureProjection(*SourceExtension);
			}

			void SetFailure(FOperationFailure& OutFailure, FDiagnostic Error,
				const FJsonObject* SourceObject = nullptr, const FString& Field = FString()) const
			{
				UeAgentAuthoringDiagnostics::FScopedSourceLocations DocumentScope(SourceDocument.Get(),
					[this](const FString& Pointer) { return UeAgentAuthoringDiagnostics::FScopedSourceLocations::ResolveProjection(DocumentSources, Pointer); });
				UeAgentAuthoringDiagnostics::FScopedSourceLocations TargetScope(SourceTarget.Get(),
					[this](const FString& Pointer) { return UeAgentAuthoringDiagnostics::FScopedSourceLocations::ResolveProjection(TargetSources, Pointer); });
				UeAgentAuthoringDiagnostics::FScopedSourceLocations ExtensionScope(SourceExtension.Get(),
					[this](const FString& Pointer) { return UeAgentAuthoringDiagnostics::FScopedSourceLocations::ResolveProjection(ExtensionSources, Pointer); });
				if (Error.TargetPath.IsEmpty()) Error.TargetPath = Target.AssetPath;
				if (Error.CandidateName.IsEmpty()) Error.CandidateName = Target.EmitterName;
				if (SourceObject) UeAgentAuthoringDiagnostics::LocateAuthoringSource(Error, *SourceObject, Field);
				OutFailure.Diagnostic = MoveTemp(Error);
			}

			void SetExtensionFailure(FOperationFailure& OutFailure, FDiagnostic Error, const FString& Field = FString()) const
			{
				SetFailure(OutFailure, MoveTemp(Error), SourceExtension.IsValid() ? SourceExtension.Get() : SourceDocument.Get(),
					SourceExtension.IsValid() ? Field : TEXT("provider_extension"));
			}

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
					SetFailure(OutFailure, StackValueFailure(TEXT("niagara_stack_rollback_context_invalid"), TEXT("The stack graph and script source must exist before rollback capture."),
						!Stage.Graph ? TEXT("graph_available") : TEXT("script_source_available"), MakeShared<FJsonValueBoolean>(false), MakeShared<FJsonValueBoolean>(true), true));
					return false;
				}
				BackupGraph.Reset(DuplicateObject<UNiagaraGraph>(Stage.Graph, GetTransientPackage()));
				if (!BackupGraph.IsValid())
				{
					SetFailure(OutFailure, StackValueFailure(TEXT("niagara_stack_rollback_capture_failed"), TEXT("Unreal could not duplicate the stack graph for rollback."),
						TEXT("backup_graph_created"), MakeShared<FJsonValueBoolean>(false), MakeShared<FJsonValueBoolean>(true), true));
					return false;
				}
				return true;
			}

			virtual bool ApplyProposed(FOperationFailure& OutFailure) override
			{
				OutFailure = FOperationFailure();
				if (!Stage.Asset || !Stage.Graph || !Stage.OutputNode)
				{
					SetFailure(OutFailure, StackValueFailure(TEXT("niagara_stack_apply_context_invalid"), TEXT("The stack asset, graph and output node must exist before applying the operation."),
						!Stage.Asset ? TEXT("asset_available") : !Stage.Graph ? TEXT("graph_available") : TEXT("output_node_available"),
						MakeShared<FJsonValueBoolean>(false), MakeShared<FJsonValueBoolean>(true), true));
					return false;
				}
				Stage.Asset->Modify();
				Stage.ScriptSource->Modify();
				Stage.Graph->Modify();
				Stage.OutputNode->Modify();
				UE::UAI::Contract::FDiagnostic Error;
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
								SetFailure(OutFailure, Error, SourceTarget.Get(), ModuleSelectorField(SourceModuleSelector, TEXT("source_"), Error));
								return false;
							}
							SourceScript = SourceModule->FunctionScript;
						}
						if (!SourceScript)
						{
							SetFailure(OutFailure, StackValueFailure(TEXT("niagara_module_script_missing"), TEXT("The module has no script available for creation or cloning."),
								TEXT("module_script_available"), MakeShared<FJsonValueBoolean>(false), MakeShared<FJsonValueBoolean>(true), true));
							return false;
						}
						AppliedModule = AddModule(*Stage.OutputNode, *SourceScript, SuggestedName, TargetIndex, Error);
						if (!AppliedModule)
						{
							SetExtensionFailure(OutFailure, Error);
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
							SetFailure(OutFailure, Error, SourceTarget.Get(), ModuleSelectorField(ModuleSelector, FString(), Error));
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
								SetExtensionFailure(OutFailure, Error, TEXT("target_index"));
								return false;
							}
						}
						else
						{
							SetExtensionFailure(OutFailure, StackValueFailure(TEXT("niagara_module_update_intent_missing"), TEXT("Updating an existing stack module requires a target index."),
								TEXT("target_index"), SourceExtension.IsValid() ? SourceExtension->TryGetField(TEXT("target_index")) : nullptr,
								MakeShared<FJsonValueString>(TEXT("a target index other than -1"))), TEXT("target_index"));
							return false;
						}
					}
					if (Operation != EOperation::Remove)
					{
						TArray<UNiagaraNodeFunctionCall*> Modules;
						if (!GetModules(*Stage.OutputNode, Modules, Error)) { SetFailure(OutFailure, Error); return false; }
						AppliedModuleIndex = Modules.IndexOfByKey(AppliedModule);
					}
				}
				else
				{
					int32 ModuleIndex = INDEX_NONE;
					if (!ResolveModule(*Stage.OutputNode, ModuleSelector, AppliedModule, ModuleIndex, Error))
					{
						SetFailure(OutFailure, Error, SourceTarget.Get(), ModuleSelectorField(ModuleSelector, FString(), Error));
						return false;
					}
					if (Operation == EOperation::Remove)
					{
						if (!ClearDynamicInput(*AppliedModule, InputName, ResolvedInputName, Error))
						{
							SetFailure(OutFailure, Error, SourceTarget.Get(), TEXT("input_name"));
							return false;
						}
					}
					else
					{
						if (!Script.IsValid())
						{
							SetFailure(OutFailure, StackValueFailure(TEXT("niagara_dynamic_input_script_missing"), TEXT("The dynamic input has no script available for assignment."),
								TEXT("dynamic_input_script_available"), MakeShared<FJsonValueBoolean>(false), MakeShared<FJsonValueBoolean>(true), true));
							return false;
						}
						if (!SetDynamicInput(*AppliedModule, InputName, *Script, SuggestedName, AppliedDynamicInput, ResolvedInputName, Error))
						{
							SetFailure(OutFailure, Error, SourceTarget.Get(), TEXT("input_name"));
							return false;
						}
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
				UE::UAI::Contract::FDiagnostic Error;
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
				UE::UAI::Contract::FDiagnostic Error;
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
				if (!BuildPackageDurablePlan(
					GetTargetKey(),
					Package ? Package->GetName() : FString(),
					Stage.Asset,
					EPackageDurableIntent::Save,
					OutPlans,
					OutFailure))
				{
					SetFailure(OutFailure, OutFailure.Diagnostic);
					return false;
				}
				return true;
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
					SetFailure(OutFailure, StackValueFailure(TEXT("niagara_stack_rollback_state_invalid"), TEXT("Rollback requires the captured backup, script source and current graph."),
						!BackupGraph.IsValid() ? TEXT("backup_graph_available") : !Stage.ScriptSource ? TEXT("script_source_available") : TEXT("graph_available"),
						MakeShared<FJsonValueBoolean>(false), MakeShared<FJsonValueBoolean>(true), true));
					return false;
				}
				UNiagaraGraph* MutatedGraph = Stage.Graph;
				const FName OriginalName = MutatedGraph->GetFName();
				const FString RetiredName = FString::Printf(TEXT("%s_UAIRetired_%s"), *OriginalName.ToString(), *FGuid::NewGuid().ToString(EGuidFormats::Digits));
				if (!MutatedGraph->Rename(*RetiredName, GetTransientPackage(), REN_DontCreateRedirectors | REN_NonTransactional))
				{
					SetFailure(OutFailure, StackValueFailure(TEXT("niagara_stack_rollback_retire_graph_failed"), TEXT("Unreal could not move the mutated stack graph out of its script source for rollback."),
						TEXT("graph_retired"), MakeShared<FJsonValueBoolean>(false), MakeShared<FJsonValueBoolean>(true), true));
					return false;
				}
				UNiagaraGraph* Restored = DuplicateObject<UNiagaraGraph>(BackupGraph.Get(), Stage.ScriptSource, OriginalName);
				if (!Restored)
				{
					SetFailure(OutFailure, StackValueFailure(TEXT("niagara_stack_rollback_restore_graph_failed"), TEXT("Unreal could not restore the captured stack graph into its script source."),
						TEXT("graph_restored"), MakeShared<FJsonValueBoolean>(false), MakeShared<FJsonValueBoolean>(true), true));
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
					{ TEXT("stage_owner_id"), GetStageOwnerId(Stage) },
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
				UE::UAI::Contract::FDiagnostic Error;
				if (!UeAgentAuthoringIdentity::BuildCanonicalIdentity(ModelId, TargetKindText, Selectors, Identity, Error))
				{
					if (Error.SourceKind.IsEmpty()) Error.SourceKind = TEXT("native_result");
					SetFailure(OutFailure, Error);
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

			TSharedPtr<const FJsonObject> SourceDocument;
			TMap<FString, UE::UAI::Contract::FSourceLocation> DocumentSources;
			TSharedPtr<const FJsonObject> SourceTarget;
			TSharedPtr<const FJsonObject> SourceExtension;
			TMap<FString, UE::UAI::Contract::FSourceLocation> TargetSources;
			TMap<FString, UE::UAI::Contract::FSourceLocation> ExtensionSources;
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
					SetFailure(OutFailure, StackValueFailure(TEXT("niagara_stack_apply_request_invalid"), TEXT("A stack operation requires its contract, workspace unit, parsed input and target object."),
						!Request.Contract ? TEXT("contract_available") : !Request.Unit ? TEXT("workspace_unit_available")
						: !Request.Input ? TEXT("input_available") : TEXT("target_available"),
						MakeShared<FJsonValueBoolean>(false), MakeShared<FJsonValueBoolean>(true), true));
					return false;
				}
				TUniquePtr<FStackPreparedState> State = MakeUnique<FStackPreparedState>(Request.Unit->Document, Request.Input->Target, Request.Input->ProviderExtension);
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
					SetFailure(OutFailure, StackValueFailure(TEXT("niagara_stack_contract_unsupported"), TEXT("The stack provider requires a module or dynamic-input operation contract."),
						TEXT("contract_id"), MakeShared<FJsonValueString>(Request.Contract->ContractId),
						MakeShared<FJsonValueString>(FString(ModuleInputContractId) + TEXT(" or ") + DynamicInputInputContractId), true), Request.Contract->ContractId);
					return false;
				}
				UE::UAI::Contract::FDiagnostic Error;
				if (!ParseStageTarget(Request.Input->Target, State->Target, Error)
					|| !ResolveStage(State->Target, State->Stage, Error)
					|| !ParseModuleSelector(Request.Input->Target, FString(), State->ModuleSelector, Error)
					|| !ParseModuleSelector(Request.Input->Target, TEXT("source_"), State->SourceModuleSelector, Error))
				{
					const FString Field = Request.Input->Target->HasField(Error.PropertyPath) ? Error.PropertyPath : FString();
					State->SetFailure(OutFailure, Error, Request.Input->Target.Get(), Field);
					return false;
				}
				State->Operation = Request.Input->RequestedOperation;
				if (State->TargetKind == EStackTargetKind::DynamicInput
					&& !UeAgentAuthoringJsonFields::ValidateOptionalFields(*Request.Input->Target, {{TEXT("input_name"), EJson::String}}, Error))
				{
					State->SetFailure(OutFailure, Error, Request.Input->Target.Get(), TEXT("input_name"));
					return false;
				}
				ReadString(Request.Input->Target, TEXT("input_name"), State->InputName);
				State->TargetIndex = INDEX_NONE;
				if (Request.Input->ProviderExtension.IsValid()
					&& !UeAgentAuthoringJsonFields::ReadInt32Field(*Request.Input->ProviderExtension, TEXT("target_index"), State->TargetIndex, Error))
				{
					State->SetFailure(OutFailure, Error, Request.Input->ProviderExtension.Get(), TEXT("target_index"));
					return false;
				}
				if (Request.Input->ProviderExtension.IsValid() && !UeAgentAuthoringJsonFields::ValidateOptionalFields(*Request.Input->ProviderExtension, {
					{TEXT("suggested_name"), EJson::String}, {TEXT("compile"), EJson::Boolean}, {TEXT("save"), EJson::Boolean}}, Error))
				{
					State->SetFailure(OutFailure, Error, Request.Input->ProviderExtension.Get());
					return false;
				}
				ReadString(Request.Input->ProviderExtension, TEXT("suggested_name"), State->SuggestedName);
				State->bCompile = ReadBool(Request.Input->ProviderExtension, TEXT("compile"), false);
				State->bSave = ReadBool(Request.Input->ProviderExtension, TEXT("save"), false);

				UNiagaraNodeFunctionCall* ExistingModule = nullptr;
				int32 ExistingIndex = INDEX_NONE;
				const bool bModuleExists = !State->ModuleSelector.IsEmpty()
					&& ResolveModule(*State->Stage.OutputNode, State->ModuleSelector, ExistingModule, ExistingIndex, Error);
				if (!State->ModuleSelector.IsEmpty() && !bModuleExists && Error.Code != TEXT("module_selector_not_found"))
				{
					State->SetFailure(OutFailure, Error, Request.Input->Target.Get(), ModuleSelectorField(State->ModuleSelector, FString(), Error));
					return false;
				}
				if (State->ModuleSelector.IsEmpty())
				{
					Error = StackValueFailure(TEXT("module_selector_missing"), TEXT("A module selector requires a node identity, script path or module name."),
						TEXT("module_selector_present"), MakeShared<FJsonValueBoolean>(false), MakeShared<FJsonValueBoolean>(true));
				}
				if (Request.Input->RequestedOperation == EOperation::Upsert)
				{
					State->Operation = bModuleExists ? EOperation::Update : EOperation::Create;
				}
				if (State->TargetKind == EStackTargetKind::Module)
				{
					if ((State->Operation == EOperation::Update || State->Operation == EOperation::Remove) && !bModuleExists)
					{
						State->SetFailure(OutFailure, Error, Request.Input->Target.Get(), ModuleSelectorField(State->ModuleSelector, FString(), Error));
						return false;
					}
					if (State->Operation == EOperation::Clone)
					{
						UNiagaraNodeFunctionCall* SourceModule = nullptr;
						int32 SourceIndex = INDEX_NONE;
						if (!ResolveModule(*State->Stage.OutputNode, State->SourceModuleSelector, SourceModule, SourceIndex, Error))
						{
							State->SetFailure(OutFailure, Error, Request.Input->Target.Get(), ModuleSelectorField(State->SourceModuleSelector, TEXT("source_"), Error));
							return false;
						}
					}
				}
				else
				{
					if (!bModuleExists)
					{
						State->SetFailure(OutFailure, Error, Request.Input->Target.Get(), ModuleSelectorField(State->ModuleSelector, FString(), Error));
						return false;
					}
					if (State->InputName.IsEmpty())
					{
						State->SetFailure(OutFailure, StackInputFailure(TEXT("niagara_dynamic_input_name_missing"), TEXT("A dynamic-input operation requires a non-empty module input name."),
							Request.Input->Target, TEXT("input_name"), TEXT("a non-empty input name")));
						return false;
					}
					FDynamicInputSelector DynamicSelector;
					DynamicSelector.Module = State->ModuleSelector;
					DynamicSelector.InputName = State->InputName;
					if (!ParseGuidField(Request.Input->Target, TEXT("dynamic_input_guid"), DynamicSelector.NodeGuid, Error))
					{
						State->SetFailure(OutFailure, Error, Request.Input->Target.Get(), TEXT("dynamic_input_guid"));
						return false;
					}
					UNiagaraNodeFunctionCall* ExistingDynamicInput = nullptr;
					FString ExistingInputName;
					const bool bDynamicExists = ResolveDynamicInput(
						State->Stage, DynamicSelector, ExistingDynamicInput, ExistingInputName, Error);
					if (!bDynamicExists && Error.Code != TEXT("niagara_dynamic_input_not_found"))
					{
						State->SetFailure(OutFailure, Error, Request.Input->Target.Get(), TEXT("input_name"));
						return false;
					}
					if (Request.Input->RequestedOperation == EOperation::Upsert)
					{
						State->Operation = bDynamicExists ? EOperation::Update : EOperation::Create;
					}
					if (State->Operation == EOperation::Create && bDynamicExists)
					{
						State->SetFailure(OutFailure, StackValueFailure(TEXT("niagara_dynamic_input_already_exists"), TEXT("The selected module input already has a dynamic input; create requires an unassigned input."),
							TEXT("dynamic_input_present"), MakeShared<FJsonValueBoolean>(true), MakeShared<FJsonValueBoolean>(false)), Request.Input->Target.Get(), TEXT("input_name"));
						return false;
					}
					if ((State->Operation == EOperation::Update || State->Operation == EOperation::Remove) && !bDynamicExists)
					{
						State->SetFailure(OutFailure, Error, Request.Input->Target.Get(), TEXT("input_name"));
						return false;
					}
					if (State->Operation == EOperation::Clone)
					{
						State->SetFailure(OutFailure, StackValueFailure(TEXT("niagara_dynamic_input_clone_unsupported"), TEXT("The dynamic-input contract does not support cloning."),
							TEXT("operation"), MakeShared<FJsonValueString>(OperationToString(State->Operation)),
							MakeShared<FJsonValueString>(TEXT("create, update, upsert or remove"))), Request.Unit->Document.Get(), TEXT("operation"));
						return false;
					}
				}

				if (State->Operation == EOperation::Create || State->Operation == EOperation::Update)
				{
					FScriptResolveRequest ResolveRequest;
					ResolveRequest.Kind = State->TargetKind == EStackTargetKind::Module
						? EScriptKind::Module
						: EScriptKind::DynamicInput;
					ResolveRequest.Input = Request.Input->ProviderExtension;
					FScriptResolveResult ResolveResult;
					if (!ResolveScript(ResolveRequest, ResolveResult, Error))
					{
						if (!(State->TargetKind == EStackTargetKind::Module && State->Operation == EOperation::Update))
						{
							State->SetExtensionFailure(OutFailure, Error);
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

	bool RegisterStackApplyContracts(UE::UAI::Contract::FDiagnostic& OutError)
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
			OutResult.Diagnostic = StackValueFailure(TEXT("niagara_stack_workspace_contract_missing"), TEXT("The required Niagara stack workspace contract is not registered."),
				TEXT("workspace_contract_id"), MakeShared<FJsonValueString>(WorkspaceContractId), MakeShared<FJsonValueString>(TEXT("a registered workspace contract")), true);
			OutResult.Diagnostic.Stage = OutResult.Phase;
			OutResult.Diagnostic.TargetPath = Request.Stage.AssetPath;
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
			OutResult.Diagnostic = LoadResult.Diagnostic;
			return false;
		}
		FBackendOptions BackendOptions;
		BackendOptions.TransactionDescription = NSLOCTEXT("UeAgentInterface", "NiagaraStackApply", "Apply Niagara stack operation");
		FApplyWorkspaceResult ApplyResult;
		UeAgentAuthoringAssetWorkspaceApply::FFailure ApplyFailure;
		if (!UeAgentAuthoringAssetWorkspaceApply::Execute(
			WorkspaceContract,
			LoadResult.Workspace,
			FProviderRegistry::Get(),
			RequestId,
			Request.bSave,
			BackendOptions,
			ApplyResult,
			ApplyFailure))
		{
			OutResult.Phase = ApplyFailure.Phase;
			OutResult.Diagnostic = ApplyFailure.Diagnostic;
			OutResult.bRollbackAttempted = ApplyResult.Execution.bRollbackAttempted;
			OutResult.bRollbackSucceeded = ApplyResult.Execution.bRollbackSucceeded;
			return false;
		}
		OutResult.Status = TEXT("applied");
		OutResult.Phase = TEXT("complete");
		OutResult.ResolvedOperation = OperationToString(Request.Operation);
		FResolvedStage Stage;
		UE::UAI::Contract::FDiagnostic ResolveError;
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
				if (!GetModules(*Stage.OutputNode, Modules, ResolveError)) { UeAgentAuthoringDiagnostics::ReportConsistencyWarning(ResolveError); return true; }
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
        if (!ResolveError.IsEmpty()) UeAgentAuthoringDiagnostics::ReportConsistencyWarning(ResolveError);
		return true;
	}
}
