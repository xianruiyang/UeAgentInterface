// Copyright Epic Games, Inc. All Rights Reserved.

#include "Domains/Blueprint/Authoring/UeAgentBlueprintAuthoringProjection.h"

#include "Domains/Blueprint/Authoring/UeAgentBlueprintAuthoringDescriptors.h"
#include "Utilities/AuthoringExport/UeAgentCompoundProjectionExtension.h"

namespace UeAgentBlueprintAuthoring
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

		FContract MakeExportContract()
		{
			FContract Result;
			Result.Schema = TEXT("uai.export_contract.v2");
			Result.ContractId = BlueprintExportContractId;
			Result.ContractVersion = TEXT("2.0.0");
			Result.TypeId = TEXT("blueprint");
			Result.SemanticDescriptorRefs = {
				AssetSemanticId,
				ExportTargetSemanticId,
				ParentClassSemanticId,
				VariableSemanticId,
				DelegateSemanticId,
				ComponentSemanticId,
				GraphSemanticId,
				NodeSemanticId,
				PinDefaultSemanticId,
				EdgeSemanticId,
				ClassDefaultPropertySemanticId,
				ComponentPropertySemanticId,
				NodePropertySemanticId,
			};
			Result.DefaultProfile = TEXT("llm_authoring");
			Result.Profiles = {
				Profile(TEXT("llm_authoring"), { TEXT("identity"), TEXT("authoring") }, 65536),
				Profile(TEXT("readback"), { TEXT("identity"), TEXT("authoring"), TEXT("readback") }, 98304),
				Profile(TEXT("debug"), {
					TEXT("identity"), TEXT("authoring"), TEXT("readback"),
					TEXT("diagnostics"), TEXT("validation") }, 196608),
				Profile(TEXT("validation"), { TEXT("identity"), TEXT("validation") }, 32768),
			};

			FFileDescriptor IndexFile;
			IndexFile.FileId = TEXT("index");
			IndexFile.Path = TEXT("index.hjson");
			IndexFile.Role = TEXT("identity");
			IndexFile.Profiles = {
				TEXT("llm_authoring"), TEXT("readback"), TEXT("debug"), TEXT("validation")
			};
			IndexFile.bRequired = true;
			IndexFile.ProjectionId = BlueprintProjectionId;
			IndexFile.MergePolicy = TEXT("inline");
			Result.Files = { MoveTemp(IndexFile) };

			Result.Fields = {
				Field(TEXT("operation"), TEXT("index.hjson#/operation"),
					EFieldSourceKind::FormatConstant, TEXT(""),
					TEXT("uai.export.format.operation_update"),
					TEXT("identity"), false, true, TEXT("emit_null")),
				Field(TEXT("asset_target"), TEXT("index.hjson#/target"),
					EFieldSourceKind::Semantic, ExportTargetSemanticId,
					TEXT("uai.export.semantic.value"),
					TEXT("identity"), false, true, TEXT("emit_null")),
				Field(TEXT("asset_properties"), TEXT("index.hjson#/properties"),
					EFieldSourceKind::Semantic, TEXT("core.property"),
					TEXT("uai.export.semantic.editable_properties"),
					TEXT("authoring"), true, false, TEXT("omit")),
				Field(TEXT("parent_class"), TEXT("index.hjson#/provider_extension/parent_class"),
					EFieldSourceKind::Semantic, ParentClassSemanticId,
					TEXT("uai.export.semantic.value"),
					TEXT("authoring"), true, true, TEXT("emit_null")),
				Field(TEXT("interfaces"), TEXT("index.hjson#/provider_extension/interfaces/*"),
					EFieldSourceKind::Semantic, InterfaceSemanticId,
					TEXT("uai.export.semantic.value"),
					TEXT("authoring"), true, true, TEXT("emit_empty")),
				Field(TEXT("variables"), TEXT("index.hjson#/provider_extension/members/variables/*"),
					EFieldSourceKind::Semantic, VariableSemanticId,
					TEXT("uai.export.semantic.value"),
					TEXT("authoring"), true, true, TEXT("emit_empty")),
				Field(TEXT("delegates"), TEXT("index.hjson#/provider_extension/members/delegates/*"),
					EFieldSourceKind::Semantic, DelegateSemanticId,
					TEXT("uai.export.semantic.value"),
					TEXT("authoring"), true, true, TEXT("emit_empty")),
				Field(TEXT("class_defaults"), TEXT("index.hjson#/provider_extension/class_defaults/properties"),
					EFieldSourceKind::Semantic, ClassDefaultPropertySemanticId,
					TEXT("uai.export.semantic.editable_properties"),
					TEXT("authoring"), true, false, TEXT("emit_empty")),
				Field(TEXT("components"), TEXT("index.hjson#/provider_extension/components/*"),
					EFieldSourceKind::Semantic, ComponentSemanticId,
					TEXT("uai.export.semantic.value"),
					TEXT("authoring"), true, true, TEXT("emit_empty")),
				Field(TEXT("component_properties"), TEXT("index.hjson#/provider_extension/components/*/properties"),
					EFieldSourceKind::Semantic, ComponentPropertySemanticId,
					TEXT("uai.export.semantic.editable_properties"),
					TEXT("authoring"), true, false, TEXT("omit")),
				Field(TEXT("graphs"), TEXT("index.hjson#/provider_extension/graphs/*"),
					EFieldSourceKind::Semantic, GraphSemanticId,
					TEXT("uai.export.semantic.value"),
					TEXT("authoring"), true, true, TEXT("emit_empty")),
				Field(TEXT("nodes"), TEXT("index.hjson#/provider_extension/graphs/*/nodes/*"),
					EFieldSourceKind::Semantic, NodeSemanticId,
					TEXT("uai.export.semantic.value"),
					TEXT("authoring"), true, true, TEXT("emit_empty")),
				Field(TEXT("node_properties"), TEXT("index.hjson#/provider_extension/graphs/*/nodes/*/properties"),
					EFieldSourceKind::Semantic, NodePropertySemanticId,
					TEXT("uai.export.semantic.editable_properties"),
					TEXT("authoring"), true, false, TEXT("omit")),
				Field(TEXT("pin_defaults"), TEXT("index.hjson#/provider_extension/graphs/*/nodes/*/pin_defaults/*"),
					EFieldSourceKind::Semantic, PinDefaultSemanticId,
					TEXT("uai.export.semantic.value"),
					TEXT("authoring"), true, false, TEXT("omit")),
				Field(TEXT("edges"), TEXT("index.hjson#/provider_extension/graphs/*/edges/*"),
					EFieldSourceKind::Semantic, EdgeSemanticId,
					TEXT("uai.export.semantic.value"),
					TEXT("authoring"), true, true, TEXT("emit_empty")),
			};

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

		FString StableIdentityKey(
			const UeAgentAuthoringIdentity::FCanonicalIdentity& Identity,
			const TCHAR* GuidSelector,
			const TCHAR* KindSelector,
			const TCHAR* NameSelector,
			const TCHAR* OccurrenceSelector = nullptr)
		{
			const FString Guid = Identity.GetSelector(GuidSelector);
			if (!Guid.IsEmpty()) return Guid.ToLower();
			const FString Kind = Identity.GetSelector(KindSelector);
			const FString Name = Identity.GetSelector(NameSelector);
			if (Kind.IsEmpty() || Name.IsEmpty()) return FString();
			FString Result = Kind + TEXT(":") + Name;
			if (OccurrenceSelector)
			{
				const FString Occurrence = Identity.GetSelector(OccurrenceSelector);
				if (!Occurrence.IsEmpty()) Result += TEXT(":") + Occurrence;
			}
			return Result;
		}

		FString GraphKey(const UeAgentAuthoringState::FStateEntry& Entry)
		{
			return StableIdentityKey(
				Entry.TargetIdentity, TEXT("graph_guid"), TEXT("graph_kind"), TEXT("graph_name"));
		}

		FString NodeKey(const UeAgentAuthoringState::FStateEntry& Entry)
		{
			return StableIdentityKey(
				Entry.TargetIdentity, TEXT("node_guid"), TEXT("node_kind"), TEXT("node_name"),
				TEXT("node_occurrence"));
		}

		bool GetProjectionKeys(
			const FString& FieldId,
			const UeAgentAuthoringState::FStateEntry& Entry,
			TArray<FString>& OutKeys)
		{
			OutKeys.Reset();
			if (FieldId == TEXT("interfaces"))
			{
				OutKeys.Add(Entry.TargetIdentity.GetSelector(TEXT("interface_class")));
			}
			else if (FieldId == TEXT("variables") || FieldId == TEXT("delegates"))
			{
				OutKeys.Add(StableIdentityKey(
					Entry.TargetIdentity, TEXT("member_guid"), TEXT("member_kind"), TEXT("member_name")));
			}
			else if (FieldId == TEXT("components") || FieldId == TEXT("component_properties"))
			{
				OutKeys.Add(StableIdentityKey(
					Entry.TargetIdentity, TEXT("component_guid"), TEXT("component_kind"),
					TEXT("component_name")));
			}
			else if (FieldId == TEXT("graphs"))
			{
				OutKeys.Add(GraphKey(Entry));
			}
			else if (FieldId == TEXT("nodes") || FieldId == TEXT("node_properties"))
			{
				OutKeys = { GraphKey(Entry), NodeKey(Entry) };
			}
			else if (FieldId == TEXT("pin_defaults"))
			{
				OutKeys = {
					GraphKey(Entry),
					NodeKey(Entry),
					StableIdentityKey(
						Entry.TargetIdentity, TEXT("pin_guid"), TEXT("node_kind"), TEXT("pin_name"))
				};
			}
			else if (FieldId == TEXT("edges"))
			{
				OutKeys = { GraphKey(Entry), Entry.TargetIdentity.GetSelector(TEXT("edge_key")) };
			}
			return !OutKeys.IsEmpty() && !OutKeys.ContainsByPredicate([](const FString& Key)
			{
				return Key.IsEmpty();
			});
		}

		bool ProjectBlueprintStructure(
			const UeAgentCompoundProjection::FRequest& Request,
			TArray<UeAgentCompoundProjection::FExpansionSelection>& OutSelections,
			FString& OutError)
		{
			OutSelections.Reset();
			OutError.Reset();
			if (!Request.IsValid() || Request.File->ProjectionId != BlueprintProjectionId)
			{
				OutError = TEXT("blueprint_projection_request_invalid");
				return false;
			}

			struct FSelectionGroup
			{
				TArray<FString> Keys;
				TArray<int32> Indices;
			};
			TMap<FString, FSelectionGroup> Groups;
			for (int32 Index = 0; Index < Request.Entries.Num(); ++Index)
			{
				TArray<FString> Keys;
				if (!GetProjectionKeys(Request.Field->FieldId, Request.Entries[Index], Keys)
					|| Keys.Num() != Request.WildcardCount)
				{
					OutError = TEXT("blueprint_projection_identity_missing");
					return false;
				}
				const FString GroupKey = FString::Join(Keys, TEXT("\x1f"));
				FSelectionGroup& Group = Groups.FindOrAdd(GroupKey);
				if (Group.Keys.IsEmpty()) Group.Keys = Keys;
				Group.Indices.Add(Index);
			}

			TArray<FString> GroupKeys;
			Groups.GenerateKeyArray(GroupKeys);
			GroupKeys.Sort();
			for (const FString& GroupKey : GroupKeys)
			{
				const FSelectionGroup& Group = Groups.FindChecked(GroupKey);
				UeAgentCompoundProjection::FExpansionSelection Selection;
				Selection.WildcardValues = Group.Keys;
				Selection.EntryIndices = Group.Indices;
				OutSelections.Add(MoveTemp(Selection));
			}
			return true;
		}
	}

	bool GetReusableProjectionKeys(
		const FString& FieldId,
		const UeAgentAuthoringState::FStateEntry& Entry,
		TArray<FString>& OutKeys)
	{
		return GetProjectionKeys(FieldId, Entry, OutKeys);
	}

	bool RegisterExportAuthoring(FString& OutError)
	{
		UeAgentExportContract::ERegistrationResult ContractResult;
		if (!UeAgentExportContract::FRegistry::Get().RegisterContract(
			MakeExportContract(), ContractResult, OutError))
		{
			return false;
		}
		UeAgentCompoundProjection::FExtensionDescriptor Descriptor;
		Descriptor.ProjectionId = BlueprintProjectionId;
		Descriptor.ImplementationId = TEXT("blueprint.snapshot.compound.v1");
		Descriptor.SourceKinds = { UeAgentExportContract::EFieldSourceKind::Semantic };
		UeAgentCompoundProjection::ERegistrationResult ExtensionResult;
		return UeAgentCompoundProjection::FRegistry::Get().RegisterExtension(
			Descriptor, ProjectBlueprintStructure, ExtensionResult, OutError);
	}

	bool GetExportContract(UeAgentExportContract::FContract& OutContract, FString& OutError)
	{
		if (!UeAgentExportContract::FRegistry::Get().FindContract(
			BlueprintExportContractId, OutContract))
		{
			OutError = TEXT("blueprint_export_contract_missing");
			return false;
		}
		OutError.Reset();
		return true;
	}
}
