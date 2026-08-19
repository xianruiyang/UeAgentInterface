// Copyright Epic Games, Inc. All Rights Reserved.

#include "Domains/Niagara/Authoring/UeAgentNiagaraAuthoringExport.h"

#include "Domains/Niagara/Authoring/UeAgentNiagaraAuthoringDescriptors.h"
#include "Domains/Niagara/Authoring/UeAgentNiagaraAuthoringStateAccessors.h"
#include "Core/HjsonWorkspace/UeAgentAuthoringComposition.h"
#include "Utilities/AuthoringCore/UeAgentAuthoringState.h"
#include "Utilities/AuthoringExport/UeAgentCompoundProjectionExtension.h"
#include "Utilities/AuthoringExport/UeAgentExportContract.h"
#include "Utilities/AuthoringExport/UeAgentSnapshotProjector.h"

#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"

namespace UeAgentNiagaraAuthoring
{
	namespace
	{
		using namespace UeAgentExportContract;
		// Editable workspaces are durable, re-importable artifacts rather than a
		// response projection. Keep their proxy gate no stricter than the shared
		// 64 MiB canonical-output budget (ceil(chars / 4) <= 16 Mi tokens), so
		// token policy cannot become a semantic nesting-depth limit.
		constexpr int32 EditableWorkspaceTokenBudget = 16 * 1024 * 1024;
		constexpr int32 AuthoringTokenBudget = EditableWorkspaceTokenBudget;
		constexpr int32 ReadbackTokenBudget = EditableWorkspaceTokenBudget;
		constexpr int32 DebugTokenBudget = 32 * 1024 * 1024;
		constexpr int32 ValidationTokenBudget = 32768;
		constexpr const TCHAR* EmitterOwnerTargetProjectorId = TEXT("uai.export.niagara.emitter_owner.target");
		constexpr const TCHAR* EmitterOwnerEnabledProjectorId = TEXT("uai.export.niagara.emitter_owner.enabled");
		constexpr const TCHAR* StageSnapshotOperationProjectorId = TEXT("uai.export.niagara.stage.operation_replace");

		FProfileDescriptor Profile(
			const TCHAR* Id,
			const TArray<FString>& Roles,
			const int32 TokenBudget)
		{
			FProfileDescriptor Result;
			Result.ProfileId = Id;
			Result.IncludedRoles = Roles;
			Result.TokenBudget = TokenBudget;
			return Result;
		}

		FFieldDescriptor Field(
			const TCHAR* Id,
			const TCHAR* OutputPath,
			const EFieldSourceKind SourceKind,
			const TCHAR* SemanticId,
			const TCHAR* ProjectorId,
			const TCHAR* Role,
			const bool bEditable,
			const bool bRequired,
			const TCHAR* EmptyPolicy)
		{
			FFieldDescriptor Result;
			Result.FieldId = Id;
			Result.OutputPath = OutputPath;
			Result.Source.Kind = SourceKind;
			Result.Source.SemanticId = SemanticId;
			Result.Source.ProjectorId = ProjectorId;
			Result.Role = Role;
			Result.bEditable = bEditable;
			Result.bRequired = bRequired;
			Result.Representation = TEXT("canonical");
			Result.EmptyPolicy = EmptyPolicy;
			Result.DefaultPolicy = TEXT("emit");
			Result.Externalization = TEXT("inline");
			return Result;
		}

		bool ProjectEmitterOwnerTarget(
			const UeAgentSnapshotProjector::FProjectorRequest& Request,
			UeAgentSnapshotProjector::FProjectedValue& OutValue,
			FString& OutError)
		{
			OutValue = UeAgentSnapshotProjector::FProjectedValue();
			OutError.Reset();
			if (!Request.IsValid() || Request.Entries.Num() != 1)
			{
				OutError = TEXT("niagara_emitter_owner_target_projection_cardinality_invalid");
				return false;
			}
			const TSharedPtr<FJsonObject> State = Request.Entries[0].Value.IsValid()
				&& Request.Entries[0].Value->Type == EJson::Object
				? Request.Entries[0].Value->AsObject()
				: nullptr;
			if (!State.IsValid())
			{
				OutError = TEXT("niagara_emitter_owner_target_state_invalid");
				return false;
			}

			TSharedRef<FJsonObject> Target = MakeShared<FJsonObject>();
			for (const TCHAR* FieldName : { TEXT("emitter_name"), TEXT("emitter_handle_id") })
			{
				FString FieldValue;
				if (State->TryGetStringField(FieldName, FieldValue) && !FieldValue.IsEmpty())
				{
					Target->SetStringField(FieldName, FieldValue);
				}
			}
			OutValue.Value = MakeShared<FJsonValueObject>(Target);
			OutValue.bHasValue = true;
			return true;
		}

		bool ProjectEmitterOwnerEnabled(
			const UeAgentSnapshotProjector::FProjectorRequest& Request,
			UeAgentSnapshotProjector::FProjectedValue& OutValue,
			FString& OutError)
		{
			OutValue = UeAgentSnapshotProjector::FProjectedValue();
			OutError.Reset();
			if (!Request.IsValid() || Request.Entries.Num() != 1)
			{
				OutError = TEXT("niagara_emitter_owner_enabled_projection_cardinality_invalid");
				return false;
			}
			const TSharedPtr<FJsonObject> State = Request.Entries[0].Value.IsValid()
				&& Request.Entries[0].Value->Type == EJson::Object
				? Request.Entries[0].Value->AsObject()
				: nullptr;
			bool bEnabled = true;
			if (!State.IsValid() || !State->TryGetBoolField(TEXT("enabled"), bEnabled))
			{
				return true;
			}
			OutValue.Value = MakeShared<FJsonValueBoolean>(bEnabled);
			OutValue.bHasValue = true;
			return true;
		}

		bool ProjectStageSnapshotOperation(
			const UeAgentSnapshotProjector::FProjectorRequest& Request,
			UeAgentSnapshotProjector::FProjectedValue& OutValue,
			FString& OutError)
		{
			OutValue = UeAgentSnapshotProjector::FProjectedValue();
			OutError.Reset();
			if (!Request.IsValid() || Request.Entries.Num() != 1)
			{
				OutError = TEXT("niagara_stage_snapshot_operation_projection_cardinality_invalid");
				return false;
			}
			OutValue.Value = MakeShared<FJsonValueString>(TEXT("replace"));
			OutValue.bHasValue = true;
			return true;
		}

		bool RegisterEmitterOwnerProjector(
			const FString& ProjectorId,
			UeAgentSnapshotProjector::FProjector Projector,
			FString& OutError)
		{
			UeAgentSnapshotProjector::FProjectorDescriptor Descriptor;
			Descriptor.ProjectorId = ProjectorId;
			Descriptor.ImplementationId = ProjectorId + TEXT(".v1");
			Descriptor.SourceKinds = { EFieldSourceKind::Semantic };
			Descriptor.bListOutput = false;
			UeAgentSnapshotProjector::ERegistrationResult Result;
			return UeAgentSnapshotProjector::FProjectorRegistry::Get().RegisterProjector(
				Descriptor,
				MoveTemp(Projector),
				Result,
				OutError);
		}

		FContract MakeExportContract()
		{
			FContract Result;
			Result.Schema = TEXT("uai.export_contract.v2");
			Result.ContractId = ExportContractId;
			Result.ContractVersion = TEXT("2.0.0");
			Result.TypeId = TEXT("niagara_asset");
			Result.SemanticDescriptorRefs = {
				AssetSemanticId,
				SystemSemanticId,
				EmitterSemanticId,
				ScriptSemanticId,
				StackSemanticId,
				ModuleSemanticId,
				DynamicInputSemanticId,
				EmitterOwnerSemanticId,
				EmitterPropertySemanticId,
				RendererSemanticId,
				EventHandlerSemanticId,
				UserParameterSemanticId,
			};
			Result.DefaultProfile = TEXT("llm_authoring");
			Result.Profiles = {
				Profile(TEXT("llm_authoring"), { TEXT("identity"), TEXT("authoring") }, AuthoringTokenBudget),
				Profile(TEXT("readback"), { TEXT("identity"), TEXT("authoring"), TEXT("readback") }, ReadbackTokenBudget),
				Profile(TEXT("debug"), { TEXT("identity"), TEXT("authoring"), TEXT("readback"), TEXT("diagnostics"), TEXT("validation") }, DebugTokenBudget),
				Profile(TEXT("validation"), { TEXT("identity"), TEXT("validation") }, ValidationTokenBudget),
			};

			FFileDescriptor IndexFile;
			IndexFile.FileId = TEXT("index");
			IndexFile.Path = TEXT("index.hjson");
			IndexFile.Role = TEXT("identity");
			IndexFile.Profiles = { TEXT("llm_authoring"), TEXT("readback"), TEXT("debug"), TEXT("validation") };
			IndexFile.bRequired = true;
			IndexFile.ProjectionId = StackProjectionId;
			IndexFile.MergePolicy = TEXT("inline");
			Result.Files = { MoveTemp(IndexFile) };

			Result.Fields = {
				Field(TEXT("operation"), TEXT("index.hjson#/operation"),
					EFieldSourceKind::FormatConstant, TEXT(""), TEXT("uai.export.format.operation_update"),
					TEXT("identity"), false, true, TEXT("emit_null")),
				Field(TEXT("asset_target"), TEXT("index.hjson#/target"),
					EFieldSourceKind::Semantic, AssetSemanticId, TEXT("uai.export.semantic.value"),
					TEXT("identity"), false, true, TEXT("emit_null")),
				Field(TEXT("stages"), TEXT("index.hjson#/provider_extension/stages/*/target"),
					EFieldSourceKind::Semantic, StackSemanticId, TEXT("uai.export.semantic.value"),
					TEXT("identity"), false, false, TEXT("omit")),
				Field(TEXT("stage_operations"), TEXT("index.hjson#/provider_extension/stages/*/operation"),
					EFieldSourceKind::Semantic, StackSemanticId, StageSnapshotOperationProjectorId,
					TEXT("identity"), false, false, TEXT("omit")),
				Field(TEXT("modules"), TEXT("index.hjson#/provider_extension/stages/*/modules/*"),
					EFieldSourceKind::Semantic, ModuleSemanticId, TEXT("uai.export.semantic.value"),
					TEXT("authoring"), true, false, TEXT("omit")),
				Field(TEXT("dynamic_inputs"), TEXT("index.hjson#/provider_extension/stages/*/modules/*/dynamic_inputs/*"),
					EFieldSourceKind::Semantic, DynamicInputSemanticId, TEXT("uai.export.semantic.value"),
					TEXT("authoring"), true, false, TEXT("omit")),
				Field(TEXT("emitter_targets"), TEXT("index.hjson#/provider_extension/emitters/*/target"),
					EFieldSourceKind::Semantic, EmitterOwnerSemanticId, EmitterOwnerTargetProjectorId,
					TEXT("identity"), false, false, TEXT("omit")),
				Field(TEXT("emitter_property_schema"), TEXT("index.hjson#/provider_extension/emitters/*/properties/schema"),
					EFieldSourceKind::Semantic, EmitterPropertySemanticId, TEXT("uai.export.semantic.property_document_schema"),
					TEXT("authoring"), false, false, TEXT("omit")),
				Field(TEXT("emitter_property_profile"), TEXT("index.hjson#/provider_extension/emitters/*/properties/profile"),
					EFieldSourceKind::Semantic, EmitterPropertySemanticId, TEXT("uai.export.semantic.property_document_profile"),
					TEXT("authoring"), false, false, TEXT("omit")),
				Field(TEXT("emitter_properties"), TEXT("index.hjson#/provider_extension/emitters/*/properties/properties"),
					EFieldSourceKind::Semantic, EmitterPropertySemanticId, TEXT("uai.export.semantic.editable_properties"),
					TEXT("authoring"), true, false, TEXT("omit")),
				Field(TEXT("emitter_enabled"), TEXT("index.hjson#/provider_extension/emitters/*/enabled"),
					EFieldSourceKind::Semantic, EmitterOwnerSemanticId, EmitterOwnerEnabledProjectorId,
					TEXT("authoring"), true, false, TEXT("omit")),
				Field(TEXT("renderers"), TEXT("index.hjson#/provider_extension/emitters/*/renderers/*"),
					EFieldSourceKind::Semantic, RendererSemanticId, TEXT("uai.export.semantic.value"),
					TEXT("authoring"), true, false, TEXT("omit")),
				Field(TEXT("event_handlers"), TEXT("index.hjson#/provider_extension/emitters/*/event_handlers/*"),
					EFieldSourceKind::Semantic, EventHandlerSemanticId, TEXT("uai.export.semantic.value"),
					TEXT("authoring"), true, false, TEXT("omit")),
				Field(TEXT("user_parameters"), TEXT("index.hjson#/provider_extension/user_parameters/*"),
					EFieldSourceKind::Semantic, UserParameterSemanticId, TEXT("uai.export.semantic.profiled_value"),
					TEXT("authoring"), true, false, TEXT("omit")),
			};

			Result.WriterPolicy.Format = TEXT("hjson");
			Result.WriterPolicy.InlineStructMaxFields = 6;
			Result.WriterPolicy.bListCountsAsOneField = true;
			Result.WriterPolicy.bScalarListInline = true;
			Result.WriterPolicy.KnownStructComponents = TEXT("array");
			Result.WriterPolicy.TransformShape = TEXT("named_arrays");
			Result.WriterPolicy.PreserveValueTextProfiles = { TEXT("readback"), TEXT("debug") };
			Result.WriterPolicy.RemoveFields = { TEXT("operations"), TEXT("control_type"), TEXT("category_path") };
			Result.WriterPolicy.PropertyPathPolicy = TEXT("only_when_needed_for_edit");
			Result.WriterPolicy.UnknownOutputFieldPolicy = TEXT("reject");
			Result.ExtensionPolicy.Input = TEXT("shared_state_snapshot_only");
			Result.ExtensionPolicy.bDirectUObjectRead = false;
			Result.SortPolicy.bDeterministic = true;
			Result.SortPolicy.bIdentityBeforeContent = true;
			return Result;
		}

		FString MakeStageProjectionKey(const UeAgentAuthoringState::FStateEntry& Entry)
		{
			const FString EmitterName = Entry.TargetIdentity.GetSelector(TEXT("emitter_name"));
			const FString EmitterHandleId = Entry.TargetIdentity.GetSelector(TEXT("emitter_handle_id"));
			const FString StageOwnerId = Entry.TargetIdentity.GetSelector(TEXT("stage_owner_id"));
			const FString Usage = Entry.TargetIdentity.GetSelector(TEXT("usage"));
			const FString UsageId = Entry.TargetIdentity.GetSelector(TEXT("usage_id"));
			FString OwnerKey;
			if (!EmitterName.IsEmpty())
			{
				OwnerKey = EmitterName;
				if (!EmitterHandleId.IsEmpty()) OwnerKey += TEXT("@") + EmitterHandleId;
			}
			else
			{
				OwnerKey = StageOwnerId.IsEmpty() ? FString(TEXT("system")) : StageOwnerId;
			}
			return OwnerKey
				+ TEXT("|") + Usage + TEXT("|") + UsageId;
		}

		FString MakeEmitterProjectionKey(const UeAgentAuthoringState::FStateEntry& Entry)
		{
			const FString EmitterName = Entry.TargetIdentity.GetSelector(TEXT("emitter_name"));
			const FString EmitterHandleId = Entry.TargetIdentity.GetSelector(TEXT("emitter_handle_id"));
			FString OwnerKey = EmitterName.IsEmpty() ? FString(TEXT("emitter")) : EmitterName;
			if (!EmitterHandleId.IsEmpty()) OwnerKey += TEXT("@") + EmitterHandleId;
			return OwnerKey;
		}

		bool ProjectStackStructure(
			const UeAgentCompoundProjection::FRequest& Request,
			TArray<UeAgentCompoundProjection::FExpansionSelection>& OutSelections,
			FString& OutError)
		{
			OutSelections.Reset();
			OutError.Reset();
			if (!Request.IsValid() || Request.File->ProjectionId != StackProjectionId)
			{
				OutError = TEXT("niagara_stack_projection_request_invalid");
				return false;
			}
			if (Request.Field->Source.SemanticId == EmitterPropertySemanticId)
			{
				TMap<FString, int32> SelectionByEmitter;
				for (int32 EntryIndex = 0; EntryIndex < Request.Entries.Num(); ++EntryIndex)
				{
					const FString EmitterKey = MakeEmitterProjectionKey(Request.Entries[EntryIndex]);
					if (EmitterKey.IsEmpty())
					{
						OutError = TEXT("niagara_emitter_property_projection_identity_missing");
						return false;
					}
					int32* SelectionIndex = SelectionByEmitter.Find(EmitterKey);
					if (!SelectionIndex)
					{
						UeAgentCompoundProjection::FExpansionSelection& Selection = OutSelections.AddDefaulted_GetRef();
						Selection.WildcardValues.Add(EmitterKey);
						SelectionIndex = &SelectionByEmitter.Add(EmitterKey, OutSelections.Num() - 1);
					}
					OutSelections[*SelectionIndex].EntryIndices.Add(EntryIndex);
				}
				OutSelections.Sort([](
					const UeAgentCompoundProjection::FExpansionSelection& Left,
					const UeAgentCompoundProjection::FExpansionSelection& Right)
				{
					return Left.WildcardValues[0] < Right.WildcardValues[0];
				});
				return true;
			}

			for (int32 EntryIndex = 0; EntryIndex < Request.Entries.Num(); ++EntryIndex)
			{
				const UeAgentAuthoringState::FStateEntry& Entry = Request.Entries[EntryIndex];
				UeAgentCompoundProjection::FExpansionSelection Selection;
				if (Request.Field->Source.SemanticId == UserParameterSemanticId)
				{
					const FString ParameterName = Entry.TargetIdentity.GetSelector(TEXT("parameter_name"));
					if (ParameterName.IsEmpty())
					{
						OutError = TEXT("niagara_user_parameter_projection_identity_missing");
						return false;
					}
					Selection.WildcardValues.Add(ParameterName);
					Selection.EntryIndices.Add(EntryIndex);
					OutSelections.Add(MoveTemp(Selection));
					continue;
				}
				if (Request.Field->Source.SemanticId == EmitterOwnerSemanticId)
				{
					Selection.WildcardValues.Add(MakeEmitterProjectionKey(Entry));
					Selection.EntryIndices.Add(EntryIndex);
					OutSelections.Add(MoveTemp(Selection));
					continue;
				}
				if (Request.Field->Source.SemanticId == RendererSemanticId
					|| Request.Field->Source.SemanticId == EventHandlerSemanticId)
				{
					Selection.WildcardValues.Add(MakeEmitterProjectionKey(Entry));
					const FString ItemKey = Request.Field->Source.SemanticId == RendererSemanticId
						? Entry.TargetIdentity.GetSelector(TEXT("renderer_name"))
						: Entry.TargetIdentity.GetSelector(TEXT("script_usage_id"));
					if (ItemKey.IsEmpty())
					{
						OutError = TEXT("niagara_emitter_structure_identity_missing");
						return false;
					}
					Selection.WildcardValues.Add(ItemKey);
					if (Selection.WildcardValues.Num() != Request.WildcardCount)
					{
						OutError = TEXT("niagara_emitter_structure_projection_wildcard_mismatch");
						return false;
					}
					Selection.EntryIndices.Add(EntryIndex);
					OutSelections.Add(MoveTemp(Selection));
					continue;
				}
				Selection.WildcardValues.Add(MakeStageProjectionKey(Entry));
				if (Request.Field->Source.SemanticId == ModuleSemanticId
					|| Request.Field->Source.SemanticId == DynamicInputSemanticId)
				{
					const FString ModuleGuid = Entry.TargetIdentity.GetSelector(TEXT("module_guid"));
					if (ModuleGuid.IsEmpty())
					{
						OutError = TEXT("niagara_stack_projection_module_identity_missing");
						return false;
					}
					Selection.WildcardValues.Add(ModuleGuid);
				}
				if (Request.Field->Source.SemanticId == DynamicInputSemanticId)
				{
					const FString InputName = Entry.TargetIdentity.GetSelector(TEXT("input_name"));
					if (InputName.IsEmpty())
					{
						OutError = TEXT("niagara_stack_projection_input_identity_missing");
						return false;
					}
					Selection.WildcardValues.Add(InputName);
				}
				if (Selection.WildcardValues.Num() != Request.WildcardCount)
				{
					OutError = TEXT("niagara_stack_projection_wildcard_mismatch");
					return false;
				}
				Selection.EntryIndices.Add(EntryIndex);
				OutSelections.Add(MoveTemp(Selection));
			}
			OutSelections.Sort([](
				const UeAgentCompoundProjection::FExpansionSelection& Left,
				const UeAgentCompoundProjection::FExpansionSelection& Right)
			{
				return FString::Join(Left.WildcardValues, TEXT("\x1f"))
					< FString::Join(Right.WildcardValues, TEXT("\x1f"));
			});
			return true;
		}
	}

	bool RegisterExportAuthoring(FString& OutError)
	{
		UeAgentExportContract::ERegistrationResult ContractResult;
		if (!UeAgentExportContract::FRegistry::Get().RegisterContract(
			MakeExportContract(), ContractResult, OutError))
		{
			return false;
		}
		if (!RegisterEmitterOwnerProjector(
			EmitterOwnerTargetProjectorId,
			ProjectEmitterOwnerTarget,
			OutError)) return false;
		if (!RegisterEmitterOwnerProjector(
			EmitterOwnerEnabledProjectorId,
			ProjectEmitterOwnerEnabled,
			OutError)) return false;
		if (!RegisterEmitterOwnerProjector(
			StageSnapshotOperationProjectorId,
			ProjectStageSnapshotOperation,
			OutError)) return false;
		UeAgentCompoundProjection::FExtensionDescriptor Descriptor;
		Descriptor.ProjectionId = StackProjectionId;
		Descriptor.ImplementationId = TEXT("niagara.snapshot.stack.v1");
		Descriptor.SourceKinds = { UeAgentExportContract::EFieldSourceKind::Semantic };
		UeAgentCompoundProjection::ERegistrationResult ExtensionResult;
		return UeAgentCompoundProjection::FRegistry::Get().RegisterExtension(
			Descriptor,
			ProjectStackStructure,
			ExtensionResult,
			OutError);
	}

	bool ExportAssetWorkspace(
		UObject& Asset,
		const FString& WorkspaceRoot,
		const FString& ProfileId,
		UeAgentExportWorkspaceWriter::FWorkspaceResult& OutResult,
		FString& OutError)
	{
		OutResult = UeAgentExportWorkspaceWriter::FWorkspaceResult();
		OutError.Reset();

		TArray<UeAgentAuthoringState::FReadRequest> Requests;
		if (!BuildAssetSnapshotReadRequests(Asset, true, Requests, OutError)) return false;
		UeAgentAuthoringState::FCaptureResult Capture;
		if (!UeAgentAuthoringState::CaptureSnapshot(Requests, Capture))
		{
			OutError = Capture.ErrorCode + TEXT(":") + Capture.ErrorDetail;
			return false;
		}
		const FString CompositionKind =
			UE::UAI::HjsonWorkspace::ResolveTargetCompositionKind(
				TEXT("niagara"),
				GetAssetKind(&Asset));
		return UE::UAI::HjsonWorkspace::TryExportCompositionWorkspace(
			CompositionKind,
			ProfileId,
			Capture.Snapshot,
			WorkspaceRoot,
			OutResult,
			OutError);
	}
}
