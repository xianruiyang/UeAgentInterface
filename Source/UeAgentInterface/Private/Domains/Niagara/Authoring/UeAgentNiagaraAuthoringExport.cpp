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
		constexpr const TCHAR* EmitterOwnerTargetProjectorId = TEXT("uai.export.niagara.emitter_owner.target");
		constexpr const TCHAR* EmitterOwnerEnabledProjectorId = TEXT("uai.export.niagara.emitter_owner.enabled");
		constexpr const TCHAR* StageSnapshotOperationProjectorId = TEXT("uai.export.niagara.stage.operation_replace");

		UE::UAI::Contract::FDiagnostic ProjectionValueFailure(const TCHAR* Code,
			const FString& Reason, const FString& Field, const TSharedPtr<FJsonValue>& Actual,
			const TSharedPtr<FJsonValue>& Expected)
		{
			auto Error = UE::UAI::Contract::FDiagnostic::Failure(Code, Reason, TEXT("export_projection"));
			Error.SourceKind = TEXT("native_result");
			Error.PropertyPath = Field;
			Error.ActualValue = Actual;
			Error.ActualValuePresent = Actual.IsValid();
			Error.ExpectedValue = Expected;
			Error.ExpectedValuePresent = Expected.IsValid();
			return Error;
		}

		UE::UAI::Contract::FDiagnostic ProjectorRequestFailure(const TCHAR* Code,
			const UeAgentSnapshotProjector::FProjectorRequest& Request)
		{
			if (!Request.IsValid())
				return ProjectionValueFailure(Code,
					TEXT("Niagara field projection requires its contract, profile, and field descriptor."),
					!Request.Contract ? TEXT("contract.available") : !Request.Profile ? TEXT("profile.available") : TEXT("field.available"),
					MakeShared<FJsonValueBoolean>(false), MakeShared<FJsonValueBoolean>(true));
			return ProjectionValueFailure(Code,
				TEXT("This Niagara field projection requires exactly one snapshot entry."),
				TEXT("entry_count"), MakeShared<FJsonValueNumber>(Request.Entries.Num()),
				MakeShared<FJsonValueNumber>(1));
		}

		UE::UAI::Contract::FDiagnostic ProjectionEntryFailure(const TCHAR* Code,
			const FString& Reason, const FString& Field,
			const UeAgentAuthoringState::FStateEntry& Entry, const int32 EntryIndex,
			const TSharedPtr<FJsonValue>& Actual, const TSharedPtr<FJsonValue>& Expected)
		{
			auto Error = ProjectionValueFailure(Code, Reason, Field, Actual, Expected);
			Error.TargetPath = Entry.TargetIdentity.GetSelector(TEXT("asset_path"));
			Error.CandidateName = Entry.TargetIdentity.GetSelector(TEXT("emitter_name"));
			Error.OperationIndex = EntryIndex;
			return Error;
		}

		FProfileDescriptor Profile(
			const TCHAR* Id,
			const TArray<FString>& Roles)
		{
			FProfileDescriptor Result;
			Result.ProfileId = Id;
			Result.IncludedRoles = Roles;
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
			UE::UAI::Contract::FDiagnostic& OutError)
		{
			OutValue = UeAgentSnapshotProjector::FProjectedValue();
			OutError.Reset();
			if (!Request.IsValid() || Request.Entries.Num() != 1)
			{
				OutError = ProjectorRequestFailure(TEXT("niagara_emitter_owner_target_projection_cardinality_invalid"), Request);
				return false;
			}
			const TSharedPtr<FJsonObject> State = Request.Entries[0].Value.IsValid()
				&& Request.Entries[0].Value->Type == EJson::Object
				? Request.Entries[0].Value->AsObject()
				: nullptr;
			if (!State.IsValid())
			{
				OutError = ProjectionEntryFailure(TEXT("niagara_emitter_owner_target_state_invalid"),
					TEXT("The emitter owner snapshot entry must contain an object state."), TEXT("state"),
					Request.Entries[0], 0, Request.Entries[0].Value, MakeShared<FJsonValueString>(TEXT("object")));
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
			UE::UAI::Contract::FDiagnostic& OutError)
		{
			OutValue = UeAgentSnapshotProjector::FProjectedValue();
			OutError.Reset();
			if (!Request.IsValid() || Request.Entries.Num() != 1)
			{
				OutError = ProjectorRequestFailure(TEXT("niagara_emitter_owner_enabled_projection_cardinality_invalid"), Request);
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
			UE::UAI::Contract::FDiagnostic& OutError)
		{
			OutValue = UeAgentSnapshotProjector::FProjectedValue();
			OutError.Reset();
			if (!Request.IsValid() || Request.Entries.Num() != 1)
			{
				OutError = ProjectorRequestFailure(TEXT("niagara_stage_snapshot_operation_projection_cardinality_invalid"), Request);
				return false;
			}
			OutValue.Value = MakeShared<FJsonValueString>(TEXT("replace"));
			OutValue.bHasValue = true;
			return true;
		}

		bool RegisterEmitterOwnerProjector(
			const FString& ProjectorId,
			UeAgentSnapshotProjector::FProjector Projector,
			UE::UAI::Contract::FDiagnostic& OutError)
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
				Profile(TEXT("llm_authoring"), { TEXT("identity"), TEXT("authoring") }),
				Profile(TEXT("readback"), { TEXT("identity"), TEXT("authoring"), TEXT("readback") }),
				Profile(TEXT("debug"), { TEXT("identity"), TEXT("authoring"), TEXT("readback"), TEXT("diagnostics"), TEXT("validation") }),
				Profile(TEXT("validation"), { TEXT("identity"), TEXT("validation") }),
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
			UE::UAI::Contract::FDiagnostic& OutError)
		{
			OutSelections.Reset();
			OutError.Reset();
			if (!Request.IsValid() || Request.File->ProjectionId != StackProjectionId)
			{
				if (!Request.Contract || !Request.Profile || !Request.File || !Request.Field)
					OutError = ProjectionValueFailure(TEXT("niagara_stack_projection_request_invalid"),
						TEXT("Niagara stack projection requires its contract, profile, file, and field descriptors."),
						!Request.Contract ? TEXT("contract.available") : !Request.Profile ? TEXT("profile.available")
							: !Request.File ? TEXT("file.available") : TEXT("field.available"),
						MakeShared<FJsonValueBoolean>(false), MakeShared<FJsonValueBoolean>(true));
				else if (Request.WildcardCount <= 0)
					OutError = ProjectionValueFailure(TEXT("niagara_stack_projection_request_invalid"),
						TEXT("Niagara stack projection requires a positive wildcard count."), TEXT("wildcard_count"),
						MakeShared<FJsonValueNumber>(Request.WildcardCount), MakeShared<FJsonValueString>(TEXT("greater than zero")));
				else
					OutError = ProjectionValueFailure(TEXT("niagara_stack_projection_request_invalid"),
						TEXT("The file descriptor does not select the Niagara stack projector."), TEXT("projection_id"),
						MakeShared<FJsonValueString>(Request.File->ProjectionId), MakeShared<FJsonValueString>(StackProjectionId));
				return false;
			}
			if (Request.Field->Source.SemanticId == EmitterPropertySemanticId)
			{
				TMap<FString, int32> SelectionByEmitter;
				for (int32 EntryIndex = 0; EntryIndex < Request.Entries.Num(); ++EntryIndex)
				{
					const FString EmitterKey = MakeEmitterProjectionKey(Request.Entries[EntryIndex]);
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
						OutError = ProjectionEntryFailure(TEXT("niagara_user_parameter_projection_identity_missing"),
							TEXT("The user parameter snapshot identity is missing its parameter name."), TEXT("parameter_name"),
							Entry, EntryIndex, MakeShared<FJsonValueString>(ParameterName),
							MakeShared<FJsonValueString>(TEXT("non-empty parameter name")));
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
						const bool bRenderer = Request.Field->Source.SemanticId == RendererSemanticId;
						OutError = ProjectionEntryFailure(TEXT("niagara_emitter_structure_identity_missing"),
							bRenderer ? TEXT("The renderer snapshot identity is missing its renderer name.")
								: TEXT("The event handler snapshot identity is missing its script usage identifier."),
							bRenderer ? TEXT("renderer_name") : TEXT("script_usage_id"),
							Entry, EntryIndex, MakeShared<FJsonValueString>(ItemKey),
							MakeShared<FJsonValueString>(TEXT("non-empty identity selector")));
						return false;
					}
					Selection.WildcardValues.Add(ItemKey);
					if (Selection.WildcardValues.Num() != Request.WildcardCount)
					{
						OutError = ProjectionEntryFailure(TEXT("niagara_emitter_structure_projection_wildcard_mismatch"),
							TEXT("The renderer or event handler projection produced a different wildcard count from its field pattern."),
							TEXT("wildcard_count"), Entry, EntryIndex,
							MakeShared<FJsonValueNumber>(Selection.WildcardValues.Num()), MakeShared<FJsonValueNumber>(Request.WildcardCount));
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
						OutError = ProjectionEntryFailure(TEXT("niagara_stack_projection_module_identity_missing"),
							TEXT("The module snapshot identity is missing its module node identifier."), TEXT("module_guid"),
							Entry, EntryIndex, MakeShared<FJsonValueString>(ModuleGuid),
							MakeShared<FJsonValueString>(TEXT("non-empty module node identifier")));
						return false;
					}
					Selection.WildcardValues.Add(ModuleGuid);
				}
				if (Request.Field->Source.SemanticId == DynamicInputSemanticId)
				{
					const FString InputName = Entry.TargetIdentity.GetSelector(TEXT("input_name"));
					if (InputName.IsEmpty())
					{
						OutError = ProjectionEntryFailure(TEXT("niagara_stack_projection_input_identity_missing"),
							TEXT("The dynamic input snapshot identity is missing its input name."), TEXT("input_name"),
							Entry, EntryIndex, MakeShared<FJsonValueString>(InputName),
							MakeShared<FJsonValueString>(TEXT("non-empty input name")));
						return false;
					}
					Selection.WildcardValues.Add(InputName);
				}
				if (Selection.WildcardValues.Num() != Request.WildcardCount)
				{
					OutError = ProjectionEntryFailure(TEXT("niagara_stack_projection_wildcard_mismatch"),
						TEXT("The Niagara stage projection produced a different wildcard count from its field pattern."),
						TEXT("wildcard_count"), Entry, EntryIndex,
						MakeShared<FJsonValueNumber>(Selection.WildcardValues.Num()), MakeShared<FJsonValueNumber>(Request.WildcardCount));
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

	bool RegisterExportAuthoring(UE::UAI::Contract::FDiagnostic& OutError)
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
		UeAgentExportWorkspaceWriter::FWorkspaceResult& OutResult)
	{
  auto& OutError = OutResult.Diagnostic;
		OutResult = UeAgentExportWorkspaceWriter::FWorkspaceResult();
		OutError.Reset();

		TArray<UeAgentAuthoringState::FReadRequest> Requests;
		if (!BuildAssetExportSnapshotReadRequests(Asset, Requests, OutError))
		{
			UE_LOG(LogTemp, Warning, TEXT("Niagara workspace export requests failed: %s"), *OutError.ToPublicDetail());
			return false;
		}
		UeAgentAuthoringState::FCaptureResult Capture;
		if (!UeAgentAuthoringState::CaptureSnapshot(Requests, Capture))
		{
			OutError = Capture.Diagnostic;
			UE_LOG(LogTemp, Warning, TEXT("Niagara workspace export capture failed: %s"), *OutError.ToPublicDetail());
			return false;
		}
		const FString CompositionKind =
			UE::UAI::HjsonWorkspace::ResolveTargetCompositionKind(
				TEXT("niagara"),
				GetAssetKind(&Asset));
		return UE::UAI::HjsonWorkspace::TryExportCompositionWorkspace(CompositionKind, ProfileId, Capture.Snapshot, WorkspaceRoot, OutResult);
	}
}
