// Copyright Epic Games, Inc. All Rights Reserved.

#include "Domains/Material/Authoring/UeAgentMaterialAuthoringProjection.h"

#include "Domains/Material/Authoring/UeAgentMaterialAuthoringDescriptors.h"
#include "Utilities/AuthoringExport/UeAgentCompoundProjectionExtension.h"

namespace UeAgentMaterialAuthoring
{
	namespace
	{
		using namespace UeAgentExportContract;

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

		FContract MakeExportContract(const bool bFunction)
		{
			FContract Result;
			Result.Schema = TEXT("uai.export_contract.v2");
			Result.ContractId = bFunction ? MaterialFunctionExportContractId : MaterialExportContractId;
			Result.ContractVersion = TEXT("2.0.0");
			Result.TypeId = bFunction ? TEXT("material_function") : TEXT("material_asset");
			Result.SemanticDescriptorRefs = {
				bFunction ? MaterialFunctionSemanticId : MaterialSemanticId,
				bFunction ? ExportFunctionTargetSemanticId : ExportTargetSemanticId,
				ExportExpressionSemanticId,
				ExportExpressionPropertySemanticId,
				ExportEdgeSemanticId,
				bFunction ? ExportFunctionSettingsSemanticId : ExportRootInputSemanticId,
			};
			Result.DefaultProfile = TEXT("llm_authoring");
			Result.Profiles = {
				Profile(TEXT("llm_authoring"), { TEXT("identity"), TEXT("authoring") }, 32768),
				Profile(TEXT("readback"), { TEXT("identity"), TEXT("authoring"), TEXT("readback") }, 49152),
				Profile(TEXT("debug"), { TEXT("identity"), TEXT("authoring"), TEXT("readback"), TEXT("diagnostics"), TEXT("validation") }, 131072),
				Profile(TEXT("validation"), { TEXT("identity"), TEXT("validation") }, 32768),
			};

			FFileDescriptor IndexFile;
			IndexFile.FileId = TEXT("index");
			IndexFile.Path = TEXT("index.hjson");
			IndexFile.Role = TEXT("identity");
			IndexFile.Profiles = { TEXT("llm_authoring"), TEXT("readback"), TEXT("debug"), TEXT("validation") };
			IndexFile.bRequired = true;
			IndexFile.ProjectionId = MaterialProjectionId;
			IndexFile.MergePolicy = TEXT("inline");
			Result.Files = { MoveTemp(IndexFile) };

			const TCHAR* TargetSemantic = bFunction
				? ExportFunctionTargetSemanticId
				: ExportTargetSemanticId;
			Result.Fields = {
				Field(TEXT("operation"), TEXT("index.hjson#/operation"),
					EFieldSourceKind::FormatConstant, TEXT(""), TEXT("uai.export.format.operation_update"),
					TEXT("identity"), false, true, TEXT("emit_null")),
				Field(TEXT("asset_target"), TEXT("index.hjson#/target"),
					EFieldSourceKind::Semantic, TargetSemantic, TEXT("uai.export.semantic.value"),
					TEXT("identity"), false, true, TEXT("emit_null")),
				Field(TEXT("asset_properties"), TEXT("index.hjson#/properties"),
					EFieldSourceKind::Semantic, TEXT("core.property"), TEXT("uai.export.semantic.editable_properties"),
					TEXT("authoring"), true, false, TEXT("omit")),
				Field(TEXT("expressions"), TEXT("index.hjson#/provider_extension/graph/expressions/*"),
					EFieldSourceKind::Semantic, ExportExpressionSemanticId, TEXT("uai.export.semantic.value"),
					TEXT("authoring"), true, true, TEXT("emit_empty")),
				Field(TEXT("expression_properties"), TEXT("index.hjson#/provider_extension/graph/expressions/*/properties"),
					EFieldSourceKind::Semantic, ExportExpressionPropertySemanticId,
					TEXT("uai.export.semantic.editable_properties"),
					TEXT("authoring"), true, false, TEXT("omit")),
				Field(TEXT("edges"), TEXT("index.hjson#/provider_extension/graph/edges/*"),
					EFieldSourceKind::Semantic, ExportEdgeSemanticId, TEXT("uai.export.semantic.value"),
					TEXT("authoring"), true, true, TEXT("emit_empty")),
			};
			if (bFunction)
			{
				Result.Fields.Add(Field(
					TEXT("function_settings"), TEXT("index.hjson#/provider_extension/function_settings"),
					EFieldSourceKind::Semantic, ExportFunctionSettingsSemanticId,
					TEXT("uai.export.semantic.value"), TEXT("authoring"), true, true, TEXT("emit_empty")));
			}
			else
			{
				Result.Fields.Add(Field(
					TEXT("root_inputs"), TEXT("index.hjson#/provider_extension/graph/root_inputs/*"),
					EFieldSourceKind::Semantic, ExportRootInputSemanticId,
					TEXT("uai.export.semantic.value"), TEXT("authoring"), true, true, TEXT("emit_empty")));
			}

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

		FString ProjectionKey(const UeAgentAuthoringState::FStateEntry& Entry)
		{
			if (Entry.SemanticId == ExportExpressionSemanticId
				|| Entry.SemanticId == ExportExpressionPropertySemanticId)
			{
				return Entry.TargetIdentity.GetSelector(TEXT("expression_guid"));
			}
			if (Entry.SemanticId == ExportEdgeSemanticId)
			{
				return Entry.TargetIdentity.GetSelector(TEXT("edge_key"));
			}
			if (Entry.SemanticId == ExportRootInputSemanticId)
			{
				return Entry.TargetIdentity.GetSelector(TEXT("material_property"));
			}
			return FString();
		}

		bool ProjectMaterialStructure(
			const UeAgentCompoundProjection::FRequest& Request,
			TArray<UeAgentCompoundProjection::FExpansionSelection>& OutSelections,
			FString& OutError)
		{
			OutSelections.Reset();
			OutError.Reset();
			if (!Request.IsValid() || Request.File->ProjectionId != MaterialProjectionId
				|| Request.WildcardCount != 1)
			{
				OutError = TEXT("material_projection_request_invalid");
				return false;
			}

			TMap<FString, TArray<int32>> IndicesByKey;
			for (int32 Index = 0; Index < Request.Entries.Num(); ++Index)
			{
				const FString Key = ProjectionKey(Request.Entries[Index]);
				if (Key.IsEmpty())
				{
					OutError = TEXT("material_projection_identity_missing");
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
				Selection.WildcardValues = { Key };
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
			MakeExportContract(false), ContractResult, OutError)
			|| !UeAgentExportContract::FRegistry::Get().RegisterContract(
				MakeExportContract(true), ContractResult, OutError))
		{
			return false;
		}
		UeAgentCompoundProjection::FExtensionDescriptor Descriptor;
		Descriptor.ProjectionId = MaterialProjectionId;
		Descriptor.ImplementationId = TEXT("material.snapshot.graph.v1");
		Descriptor.SourceKinds = { UeAgentExportContract::EFieldSourceKind::Semantic };
		UeAgentCompoundProjection::ERegistrationResult ExtensionResult;
		return UeAgentCompoundProjection::FRegistry::Get().RegisterExtension(
			Descriptor, ProjectMaterialStructure, ExtensionResult, OutError);
	}

	bool GetExportContract(
		const FString& AssetKind,
		UeAgentExportContract::FContract& OutContract,
		FString& OutError)
	{
		const FString ContractId = AssetKind == TEXT("material")
			? MaterialExportContractId
			: AssetKind == TEXT("material_function")
				? MaterialFunctionExportContractId
				: FString();
		if (ContractId.IsEmpty())
		{
			OutError = TEXT("material_export_asset_kind_invalid");
			return false;
		}
		if (!UeAgentExportContract::FRegistry::Get().FindContract(ContractId, OutContract))
		{
			OutError = TEXT("material_export_contract_missing");
			return false;
		}
		OutError.Reset();
		return true;
	}
}
