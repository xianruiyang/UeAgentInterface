// Copyright Epic Games, Inc. All Rights Reserved.

#include "Domains/Rigging/Authoring/UeAgentRiggingAuthoringProjection.h"

#include "Core/Contract/UeAgentDiagnostic.h"
#include "Dom/JsonValue.h"
#include "Utilities/AuthoringCore/UeAgentAuthoringDiagnostics.h"

namespace UeAgentRiggingAuthoring
{
	namespace
	{
		using namespace UeAgentExportContract;

		FProfileDescriptor ExportProfile(const TCHAR* Id, const TArray<FString>& Roles)
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
			const FString& SemanticId,
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

		FContract MakeContract(const FProfile& Profile)
		{
			FContract Result;
			Result.Schema = TEXT("uai.export_contract.v2");
			Result.ContractId = Profile.ExportContractId;
			Result.ContractVersion = TEXT("2.0.0");
			Result.TypeId = Profile.TypeId;
			Result.SemanticDescriptorRefs = {
				Profile.DocumentSemanticId, Profile.ExportTargetSemanticId
			};
			Result.DefaultProfile = TEXT("llm_authoring");
			Result.Profiles = {
				ExportProfile(TEXT("llm_authoring"), { TEXT("identity"), TEXT("authoring") }),
				ExportProfile(TEXT("readback"), { TEXT("identity"), TEXT("authoring"), TEXT("readback") }),
				ExportProfile(TEXT("debug"), { TEXT("identity"), TEXT("authoring"), TEXT("readback"), TEXT("diagnostics") }),
				ExportProfile(TEXT("validation"), { TEXT("identity"), TEXT("validation") }),
			};
			FFileDescriptor Index;
			Index.FileId = TEXT("index");
			Index.Path = TEXT("index.hjson");
			Index.Role = TEXT("identity");
			Index.Profiles = { TEXT("llm_authoring"), TEXT("readback"), TEXT("debug"), TEXT("validation") };
			Index.bRequired = true;
			Index.ProjectionId = Profile.ProfileId + TEXT(".direct");
			Index.MergePolicy = TEXT("inline");
			Result.Files = { MoveTemp(Index) };
			Result.Fields = {
				Field(TEXT("operation"), TEXT("index.hjson#/operation"),
					EFieldSourceKind::FormatConstant, TEXT(""), TEXT("uai.export.format.operation_update"),
					TEXT("identity"), false, true, TEXT("emit_null")),
				Field(TEXT("target"), TEXT("index.hjson#/target"),
					EFieldSourceKind::Semantic, Profile.ExportTargetSemanticId, TEXT("uai.export.semantic.value"),
					TEXT("identity"), false, true, TEXT("emit_null")),
				Field(TEXT("properties"), TEXT("index.hjson#/properties"),
					EFieldSourceKind::Semantic, TEXT("core.property"), TEXT("uai.export.semantic.editable_properties"),
					TEXT("authoring"), true, false, TEXT("omit")),
				Field(TEXT("provider_extension"), TEXT("index.hjson#/provider_extension"),
					EFieldSourceKind::Semantic, Profile.DocumentSemanticId, TEXT("uai.export.semantic.value"),
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
	}

	bool RegisterExportAuthoring(UE::UAI::Contract::FDiagnostic& OutError)
	{
		for (const FProfile& Profile : GetProfiles())
		{
			UeAgentExportContract::ERegistrationResult Result;
			if (!UeAgentExportContract::FRegistry::Get().RegisterContract(
				MakeContract(Profile), Result, OutError)) return false;
		}
		return true;
	}

	bool GetExportContract(
		const FProfile& Profile,
		UeAgentExportContract::FContract& OutContract,
		UE::UAI::Contract::FDiagnostic& OutError)
	{
		if (!UeAgentExportContract::FRegistry::Get().FindContract(Profile.ExportContractId, OutContract))
		{
			OutError = UeAgentAuthoringDiagnostics::MakeValueFailure(TEXT("rigging_export_contract_missing"), TEXT("export_contract_id"),
                MakeShared<FJsonValueString>(TEXT("registered export contract")), MakeShared<FJsonValueString>(Profile.ExportContractId),
                TEXT("export_prepare"), TEXT("The export contract for Rigging profile '") + Profile.TypeId + TEXT("' is not registered."));
            OutError.SourceKind = TEXT("native_result");
			return false;
		}
		OutError.Reset();
		return true;
	}
}
