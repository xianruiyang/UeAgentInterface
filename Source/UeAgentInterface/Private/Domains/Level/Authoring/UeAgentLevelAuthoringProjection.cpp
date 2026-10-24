// Copyright Epic Games, Inc. All Rights Reserved.

#include "Domains/Level/Authoring/UeAgentLevelAuthoringProjection.h"

#include "Core/Contract/UeAgentDiagnostic.h"
#include "Dom/JsonValue.h"

#include "Domains/Level/Authoring/UeAgentLevelAuthoringDescriptors.h"
#include "Utilities/AuthoringExport/UeAgentExportWorkspaceWriter.h"

namespace UeAgentLevelAuthoring
{
	namespace
	{
		using namespace UeAgentExportContract;

		struct FLevelProfileSpec
		{
			FString ProfileId;
			TArray<FString> IncludedRoles;
		};

		const TArray<FLevelProfileSpec>& ProfileSpecs()
		{
			static const TArray<FLevelProfileSpec> Specs = {
				{ TEXT("llm_authoring"), { TEXT("identity"), TEXT("authoring") } },
				{ TEXT("readback"), { TEXT("identity"), TEXT("authoring"), TEXT("readback") } },
				{ TEXT("debug"), {
					TEXT("identity"), TEXT("authoring"), TEXT("readback"), TEXT("diagnostics") } },
				{ TEXT("validation"), { TEXT("identity"), TEXT("authoring") } },
			};
			return Specs;
		}

		const FLevelProfileSpec* FindProfileSpec(const FString& ProfileId)
		{
			return ProfileSpecs().FindByPredicate([&ProfileId](const FLevelProfileSpec& Spec)
			{
				return Spec.ProfileId == ProfileId;
			});
		}

		FProfileDescriptor Profile(const FLevelProfileSpec& Spec)
		{
			FProfileDescriptor Result;
			Result.ProfileId = Spec.ProfileId;
			Result.IncludedRoles = Spec.IncludedRoles;
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

		FContract MakeContract(const EExportTargetKind Kind)
		{
			const bool bWorld = Kind == EExportTargetKind::World;
			const bool bActor = Kind == EExportTargetKind::Actor;
			const TCHAR* ContractId = bWorld
				? WorldExportContractId
				: bActor ? ActorExportContractId : ComponentExportContractId;
			const TCHAR* TypeId = bWorld ? TEXT("level_world") : bActor ? TEXT("level_actor") : TEXT("level_component");
			const TCHAR* FileId = bWorld ? TEXT("index") : bActor ? TEXT("actor") : TEXT("component");
			const TCHAR* FilePath = bWorld ? TEXT("index.hjson") : bActor ? TEXT("actor.hjson") : TEXT("component.hjson");
			const TCHAR* TargetSemantic = bWorld
				? WorldTargetSemanticId
				: bActor ? ActorTargetSemanticId : ComponentTargetSemanticId;
			const TCHAR* StateSemantic = bActor ? ActorStateSemanticId : ComponentStateSemanticId;
			const TCHAR* PropertySemantic = bWorld ? WorldPropertySemanticId
				: bActor ? ActorPropertySemanticId : ComponentPropertySemanticId;

			FContract Result;
			Result.Schema = TEXT("uai.export_contract.v2");
			Result.ContractId = ContractId;
			Result.ContractVersion = TEXT("2.0.0");
			Result.TypeId = TypeId;
			Result.SemanticDescriptorRefs = bWorld
				? TArray<FString>{ WorldTargetSemanticId, WorldPropertySemanticId }
				: TArray<FString>{ TargetSemantic, StateSemantic, PropertySemantic };
			Result.DefaultProfile = TEXT("llm_authoring");
			for (const FLevelProfileSpec& Spec : ProfileSpecs())
			{
				Result.Profiles.Add(Profile(Spec));
			}

			FFileDescriptor File;
			File.FileId = FileId;
			File.Path = FilePath;
			File.Role = bWorld ? TEXT("identity") : TEXT("authoring");
			File.Profiles = { TEXT("llm_authoring"), TEXT("readback"), TEXT("debug"), TEXT("validation") };
			File.bRequired = true;
			File.ProjectionId = ProjectionId;
			File.MergePolicy = TEXT("inline");
			Result.Files = { MoveTemp(File) };

			const FString Prefix = FString(FilePath) + TEXT("#");
			Result.Fields.Add(Field(
				TEXT("operation"), *(Prefix + TEXT("/operation")),
				EFieldSourceKind::FormatConstant, TEXT(""),
				TEXT("uai.export.format.operation_update"),
				TEXT("identity"), false, true, TEXT("emit_null")));
			Result.Fields.Add(Field(
				TEXT("target"), *(Prefix + TEXT("/target")),
				EFieldSourceKind::Semantic, TargetSemantic,
				TEXT("uai.export.semantic.value"),
				TEXT("identity"), false, true, TEXT("emit_null")));
			Result.Fields.Add(Field(
					TEXT("properties"), *(Prefix + TEXT("/properties")),
					EFieldSourceKind::Semantic, PropertySemantic,
					TEXT("uai.export.semantic.editable_properties"),
					TEXT("authoring"), true, false, TEXT("emit_empty")));
			if (!bWorld)
			{
				Result.Fields.Add(Field(
					TEXT("provider_extension"), *(Prefix + TEXT("/provider_extension")),
					EFieldSourceKind::Semantic, StateSemantic,
					TEXT("uai.export.semantic.value"),
					TEXT("authoring"), true, true, TEXT("emit_null")));
			}

			Result.WriterPolicy.Format = TEXT("hjson");
			Result.WriterPolicy.InlineStructMaxFields = 6;
			Result.WriterPolicy.bListCountsAsOneField = true;
			Result.WriterPolicy.bScalarListInline = true;
			Result.WriterPolicy.KnownStructComponents = TEXT("array");
			Result.WriterPolicy.TransformShape = TEXT("named_arrays");
			Result.WriterPolicy.PreserveValueTextProfiles = { TEXT("readback"), TEXT("debug") };
			Result.WriterPolicy.RemoveFields = {
				TEXT("operations"), TEXT("control_type"), TEXT("category_path")
			};
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
		UeAgentExportContract::ERegistrationResult Result;
		return UeAgentExportContract::FRegistry::Get().RegisterContract(
				MakeContract(EExportTargetKind::World), Result, OutError)
			&& UeAgentExportContract::FRegistry::Get().RegisterContract(
				MakeContract(EExportTargetKind::Actor), Result, OutError)
			&& UeAgentExportContract::FRegistry::Get().RegisterContract(
				MakeContract(EExportTargetKind::Component), Result, OutError);
	}

	bool GetExportContract(
		const EExportTargetKind Kind,
		UeAgentExportContract::FContract& OutContract,
		UE::UAI::Contract::FDiagnostic& OutError)
	{
		const TCHAR* ContractId = Kind == EExportTargetKind::World
			? WorldExportContractId
			: Kind == EExportTargetKind::Actor
				? ActorExportContractId
				: ComponentExportContractId;
		if (!UeAgentExportContract::FRegistry::Get().FindContract(ContractId, OutContract))
		{
			OutError = UE::UAI::Contract::FDiagnostic::Failure(TEXT("level_export_contract_missing"),
				TEXT("The export contract for this Level target kind is not registered."), TEXT("level_export"), ContractId);
			OutError.PropertyPath = TEXT("export_contract_registered");
			OutError.ActualValue = MakeShared<FJsonValueBoolean>(false);
			OutError.ExpectedValue = MakeShared<FJsonValueBoolean>(true);
			OutError.CandidateName = Kind == EExportTargetKind::World ? TEXT("world")
				: Kind == EExportTargetKind::Actor ? TEXT("actor") : TEXT("component");
			OutError.SourceKind = TEXT("native_result");
			return false;
		}
		OutError.Reset();
		return true;
	}

	bool GetWorkspaceExportPolicy(
		const FString& ProfileId,
		UeAgentExportWorkspaceWriter::FCompoundPlanPolicy& OutPolicy,
		UE::UAI::Contract::FDiagnostic& OutError)
	{
		OutPolicy = UeAgentExportWorkspaceWriter::FCompoundPlanPolicy();
		const FLevelProfileSpec* ProfileSpec = FindProfileSpec(ProfileId);
		if (!ProfileSpec)
		{
			OutError = UE::UAI::Contract::FDiagnostic::Failure(TEXT("level_export_profile_unknown"),
				TEXT("The requested Level export profile is not supported."), TEXT("level_export"));
			OutError.PropertyPath = TEXT("profile");
			OutError.ActualValue = MakeShared<FJsonValueString>(ProfileId);
			TArray<TSharedPtr<FJsonValue>> Choices;
			for (const auto& Spec : ProfileSpecs()) Choices.Add(MakeShared<FJsonValueString>(Spec.ProfileId));
			OutError.ExpectedValue = MakeShared<FJsonValueArray>(MoveTemp(Choices));
			return false;
		}

		OutPolicy.ContractId = TEXT("uai.export.level.workspace.v2");
		OutPolicy.ContractVersion = TEXT("2.0.0");
		OutPolicy.ProfileId = ProfileSpec->ProfileId;
		OutPolicy.EntryFile = TEXT("index.hjson");

		UeAgentExportWorkspaceWriter::FRoleCardinality IndexRole;
		IndexRole.Role = TEXT("index");
		IndexRole.PathPattern = TEXT("index.hjson");
		IndexRole.Minimum = 1;
		IndexRole.Maximum = 1;
		OutPolicy.RoleCardinalities.Add(MoveTemp(IndexRole));

		UeAgentExportWorkspaceWriter::FRoleCardinality ActorRole;
		ActorRole.Role = TEXT("actor");
		ActorRole.PathPattern = TEXT("actors/items/{actor_guid}/actor.hjson");
		OutPolicy.RoleCardinalities.Add(MoveTemp(ActorRole));

		UeAgentExportWorkspaceWriter::FRoleCardinality ComponentRole;
		ComponentRole.Role = TEXT("component");
		ComponentRole.PathPattern = TEXT(
			"actors/items/{actor_guid}/components/{component_name}/component.hjson");
		OutPolicy.RoleCardinalities.Add(MoveTemp(ComponentRole));

		OutError.Reset();
		return true;
	}
}
