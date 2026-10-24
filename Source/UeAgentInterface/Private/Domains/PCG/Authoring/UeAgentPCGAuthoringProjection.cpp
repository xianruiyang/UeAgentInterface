// Copyright Epic Games, Inc. All Rights Reserved.

#include "Domains/PCG/Authoring/UeAgentPCGAuthoringProjection.h"

#include "Domains/PCG/Authoring/UeAgentPCGAuthoringDescriptors.h"
#include "Dom/JsonValue.h"
#include "Utilities/AuthoringExport/UeAgentCompoundProjectionExtension.h"

namespace UeAgentPCGAuthoring
{
	namespace
	{
		using namespace UeAgentExportContract;

		UE::UAI::Contract::FDiagnostic NativeFailure(const TCHAR* Code, const TCHAR* Reason,
			const FString& Field, TSharedPtr<FJsonValue> Actual, const TCHAR* Expected,
			const FString& Target = FString())
		{
			auto Error = UE::UAI::Contract::FDiagnostic::Failure(Code, Reason, TEXT("export_projection"));
			Error.SourceKind = TEXT("native_result");
			Error.PropertyPath = Field;
			Error.ActualValue = MoveTemp(Actual);
			Error.ActualValuePresent = Error.ActualValue.IsValid();
			Error.ExpectedValue = MakeShared<FJsonValueString>(Expected);
			Error.TargetPath = Target;
			return Error;
		}

		FProfileDescriptor Profile(const TCHAR* Id, const TArray<FString>& Roles)
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

		FContract MakeExportContract()
		{
			FContract Result;
			Result.Schema = TEXT("uai.export_contract.v2");
			Result.ContractId = ExportContractId;
			Result.ContractVersion = TEXT("2.0.0");
			Result.TypeId = TEXT("pcg_graph");
			Result.SemanticDescriptorRefs = {
				GraphSemanticId, ExportTargetSemanticId, ExportNodeSemanticId,
				ExportNodePropertySemanticId, ExportPinSemanticId,
				ExportEdgeSemanticId, ExportParametersSemanticId
			};
			Result.DefaultProfile = TEXT("llm_authoring");
			Result.Profiles = {
				Profile(TEXT("llm_authoring"), { TEXT("identity"), TEXT("authoring") }),
				Profile(TEXT("readback"), { TEXT("identity"), TEXT("authoring"), TEXT("readback") }),
				Profile(TEXT("debug"), { TEXT("identity"), TEXT("authoring"), TEXT("readback"),
					TEXT("diagnostics"), TEXT("validation") }),
				Profile(TEXT("validation"), { TEXT("identity"), TEXT("validation") }),
			};

			FFileDescriptor Index;
			Index.FileId = TEXT("index");
			Index.Path = TEXT("index.hjson");
			Index.Role = TEXT("identity");
			Index.Profiles = { TEXT("llm_authoring"), TEXT("readback"), TEXT("debug"), TEXT("validation") };
			Index.bRequired = true;
			Index.ProjectionId = ProjectionId;
			Index.MergePolicy = TEXT("inline");
			Result.Files = { MoveTemp(Index) };

			Result.Fields = {
				Field(TEXT("operation"), TEXT("index.hjson#/operation"),
					EFieldSourceKind::FormatConstant, TEXT(""), TEXT("uai.export.format.operation_update"),
					TEXT("identity"), false, true, TEXT("emit_null")),
				Field(TEXT("target"), TEXT("index.hjson#/target"),
					EFieldSourceKind::Semantic, ExportTargetSemanticId, TEXT("uai.export.semantic.value"),
					TEXT("identity"), false, true, TEXT("emit_null")),
				Field(TEXT("properties"), TEXT("index.hjson#/properties"),
					EFieldSourceKind::Semantic, TEXT("core.property"), TEXT("uai.export.semantic.editable_properties"),
					TEXT("authoring"), true, false, TEXT("omit")),
				Field(TEXT("nodes"), TEXT("index.hjson#/provider_extension/graph/nodes/*"),
					EFieldSourceKind::Semantic, ExportNodeSemanticId, TEXT("uai.export.semantic.value"),
					TEXT("authoring"), true, true, TEXT("emit_empty")),
				Field(TEXT("node_properties"), TEXT("index.hjson#/provider_extension/graph/nodes/*/properties"),
					EFieldSourceKind::Semantic, ExportNodePropertySemanticId,
					TEXT("uai.export.semantic.editable_properties"),
					TEXT("authoring"), true, false, TEXT("omit")),
				Field(TEXT("pin_interface"), TEXT("index.hjson#/provider_extension/graph/nodes/*/pin_interface/pins/*"),
					EFieldSourceKind::Semantic, ExportPinSemanticId, TEXT("uai.export.semantic.value"),
					TEXT("authoring"), false, true, TEXT("emit_empty")),
				Field(TEXT("edges"), TEXT("index.hjson#/provider_extension/graph/edges/*"),
					EFieldSourceKind::Semantic, ExportEdgeSemanticId, TEXT("uai.export.semantic.value"),
					TEXT("authoring"), true, true, TEXT("emit_empty")),
				Field(TEXT("parameters"), TEXT("index.hjson#/provider_extension/graph/parameters"),
					EFieldSourceKind::Semantic, ExportParametersSemanticId, TEXT("uai.export.semantic.value"),
					TEXT("authoring"), true, true, TEXT("emit_empty")),
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

		bool ProjectStructure(
			const UeAgentCompoundProjection::FRequest& Request,
			TArray<UeAgentCompoundProjection::FExpansionSelection>& OutSelections,
			UE::UAI::Contract::FDiagnostic& OutError)
		{
			OutSelections.Reset();
			OutError.Reset();
			if (!Request.IsValid() || Request.File->ProjectionId != ProjectionId
				|| (Request.WildcardCount != 1 && Request.WildcardCount != 2))
			{
				const TCHAR* Missing = !Request.Contract ? TEXT("contract") : !Request.Profile ? TEXT("profile")
					: !Request.File ? TEXT("file") : !Request.Field ? TEXT("field") : nullptr;
				if (Missing)
				{
					OutError = NativeFailure(TEXT("pcg_projection_request_invalid"),
						TEXT("The PCG projection request is missing a required descriptor."),
						Missing, nullptr, TEXT("a registered projection descriptor"));
				}
				else if (Request.File->ProjectionId != ProjectionId)
				{
					OutError = NativeFailure(TEXT("pcg_projection_request_invalid"),
						TEXT("The PCG structure projector received a different projection identifier."),
						TEXT("projection_id"), MakeShared<FJsonValueString>(Request.File->ProjectionId), ProjectionId);
				}
				else
				{
					OutError = NativeFailure(TEXT("pcg_projection_request_invalid"),
						TEXT("The PCG structure projector requires one or two wildcard levels."),
						TEXT("wildcard_count"), MakeShared<FJsonValueNumber>(Request.WildcardCount), TEXT("1 or 2"));
				}
				OutError.InternalContext = Request.JsonPointerPattern;
				return false;
			}

			TMap<FString, TArray<int32>> IndicesByKey;
			for (int32 Index = 0; Index < Request.Entries.Num(); ++Index)
			{
				const UeAgentAuthoringState::FStateEntry& Entry = Request.Entries[Index];
				const FString NodeId = Entry.TargetIdentity.GetSelector(TEXT("node_id"));
				FString Key;
				if (Entry.SemanticId == ExportNodeSemanticId
					|| Entry.SemanticId == ExportNodePropertySemanticId)
				{
					Key = NodeId;
				}
				else if (Entry.SemanticId == ExportEdgeSemanticId)
				{
					Key = Entry.TargetIdentity.GetSelector(TEXT("edge_key"));
				}
				else if (Entry.SemanticId == ExportPinSemanticId)
				{
					const FString Label = Entry.TargetIdentity.GetSelector(TEXT("pin_label"));
					if (!NodeId.IsEmpty() && !Label.IsEmpty()) Key = NodeId + TEXT("\n") + Label;
				}
				if (Key.IsEmpty())
				{
					const bool bNode = Entry.SemanticId == ExportNodeSemanticId || Entry.SemanticId == ExportNodePropertySemanticId;
					const bool bPin = Entry.SemanticId == ExportPinSemanticId;
					const bool bEdge = Entry.SemanticId == ExportEdgeSemanticId;
					const FString Field = bNode || (bPin && NodeId.IsEmpty()) ? TEXT("node_id")
						: bPin ? TEXT("pin_label") : bEdge ? TEXT("edge_key") : TEXT("semantic_id");
					OutError = NativeFailure(TEXT("pcg_projection_identity_missing"),
						bNode || bPin || bEdge ? TEXT("The PCG snapshot entry is missing a required identity selector.")
							: TEXT("The PCG structure projector does not support this snapshot semantic."),
						Field, MakeShared<FJsonValueString>(bNode || bPin || bEdge
							? Entry.TargetIdentity.GetSelector(Field) : Entry.SemanticId),
						bNode || bPin || bEdge ? TEXT("a non-empty identity selector")
							: TEXT("a PCG node, node property, edge or pin export semantic"),
						Entry.TargetIdentity.GetSelector(TEXT("asset_path")));
					OutError.SubjectId = Entry.SemanticId;
					return false;
				}
				IndicesByKey.FindOrAdd(Key).Add(Index);
			}
			TArray<FString> Keys;
			IndicesByKey.GenerateKeyArray(Keys);
			Keys.Sort();
			for (const FString& Key : Keys)
			{
				UeAgentCompoundProjection::FExpansionSelection Selection;
				if (Request.WildcardCount == 2)
				{
					FString NodeId;
					FString Label;
					if (!Key.Split(TEXT("\n"), &NodeId, &Label) || NodeId.IsEmpty() || Label.IsEmpty())
					{
						OutError = NativeFailure(TEXT("pcg_projection_pin_identity_invalid"),
							TEXT("A two-level PCG pin projection requires non-empty node and pin identities separated by a newline."),
							TEXT("pin_identity"), MakeShared<FJsonValueString>(Key), TEXT("node_id\\npin_label"),
							Request.Entries[IndicesByKey.FindChecked(Key)[0]].TargetIdentity.GetSelector(TEXT("asset_path")));
						return false;
					}
					Selection.WildcardValues = { NodeId, Label };
				}
				else
				{
					Selection.WildcardValues = { Key };
				}
				Selection.EntryIndices = IndicesByKey.FindChecked(Key);
				OutSelections.Add(MoveTemp(Selection));
			}
			return true;
		}
	}

	bool RegisterExportAuthoring(UE::UAI::Contract::FDiagnostic& OutError)
	{
		UeAgentExportContract::ERegistrationResult ContractResult;
		if (!UeAgentExportContract::FRegistry::Get().RegisterContract(
			MakeExportContract(), ContractResult, OutError)) return false;
		UeAgentCompoundProjection::FExtensionDescriptor Descriptor;
		Descriptor.ProjectionId = ProjectionId;
		Descriptor.ImplementationId = TEXT("pcg.snapshot.graph.v1");
		Descriptor.SourceKinds = { UeAgentExportContract::EFieldSourceKind::Semantic };
		UeAgentCompoundProjection::ERegistrationResult ExtensionResult;
		return UeAgentCompoundProjection::FRegistry::Get().RegisterExtension(
			Descriptor, ProjectStructure, ExtensionResult, OutError);
	}

	bool GetExportContract(UeAgentExportContract::FContract& OutContract, UE::UAI::Contract::FDiagnostic& OutError)
	{
		if (!UeAgentExportContract::FRegistry::Get().FindContract(ExportContractId, OutContract))
		{
			OutError = NativeFailure(TEXT("pcg_export_contract_missing"),
				TEXT("The PCG export contract is not registered."), TEXT("contract_id"),
				MakeShared<FJsonValueString>(ExportContractId), TEXT("a registered PCG export contract"));
			return false;
		}
		OutError.Reset();
		return true;
	}
}
