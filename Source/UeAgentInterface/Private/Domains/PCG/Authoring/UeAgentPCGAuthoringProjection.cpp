// Copyright Epic Games, Inc. All Rights Reserved.

#include "Domains/PCG/Authoring/UeAgentPCGAuthoringProjection.h"

#include "Domains/PCG/Authoring/UeAgentPCGAuthoringDescriptors.h"
#include "Utilities/AuthoringExport/UeAgentCompoundProjectionExtension.h"

namespace UeAgentPCGAuthoring
{
	namespace
	{
		using namespace UeAgentExportContract;

		FProfileDescriptor Profile(const TCHAR* Id, const TArray<FString>& Roles, const int32 TokenBudget)
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
				Profile(TEXT("llm_authoring"), { TEXT("identity"), TEXT("authoring") }, 32768),
				Profile(TEXT("readback"), { TEXT("identity"), TEXT("authoring"), TEXT("readback") }, 49152),
				Profile(TEXT("debug"), { TEXT("identity"), TEXT("authoring"), TEXT("readback"),
					TEXT("diagnostics"), TEXT("validation") }, 131072),
				Profile(TEXT("validation"), { TEXT("identity"), TEXT("validation") }, 32768),
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
				Field(TEXT("input_pins"), TEXT("index.hjson#/provider_extension/graph/nodes/*/input_pins/*"),
					EFieldSourceKind::Semantic, ExportPinSemanticId, TEXT("uai.export.semantic.value"),
					TEXT("authoring"), false, true, TEXT("emit_empty")),
				Field(TEXT("output_pins"), TEXT("index.hjson#/provider_extension/graph/nodes/*/output_pins/*"),
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
			FString& OutError)
		{
			OutSelections.Reset();
			OutError.Reset();
			if (!Request.IsValid() || Request.File->ProjectionId != ProjectionId
				|| (Request.WildcardCount != 1 && Request.WildcardCount != 2))
			{
				OutError = TEXT("pcg_projection_request_invalid");
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
					const FString Direction = Entry.TargetIdentity.GetSelector(TEXT("direction"));
					const FString RequiredDirection = Request.Field->FieldId == TEXT("input_pins")
						? TEXT("input") : TEXT("output");
					if (Direction != RequiredDirection) continue;
					const FString Label = Entry.TargetIdentity.GetSelector(TEXT("pin_label"));
					if (!NodeId.IsEmpty() && !Label.IsEmpty()) Key = NodeId + TEXT("\n") + Label;
				}
				if (Key.IsEmpty())
				{
					OutError = TEXT("pcg_projection_identity_missing:") + Entry.SemanticId;
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
						OutError = TEXT("pcg_projection_pin_identity_invalid");
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

	bool RegisterExportAuthoring(FString& OutError)
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

	bool GetExportContract(UeAgentExportContract::FContract& OutContract, FString& OutError)
	{
		if (!UeAgentExportContract::FRegistry::Get().FindContract(ExportContractId, OutContract))
		{
			OutError = TEXT("pcg_export_contract_missing");
			return false;
		}
		OutError.Reset();
		return true;
	}
}
