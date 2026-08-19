// Copyright Epic Games, Inc. All Rights Reserved.

#include "Domains/UMG/Authoring/UeAgentUmgAuthoringProjection.h"

#include "Domains/Blueprint/Authoring/UeAgentBlueprintAuthoringDescriptors.h"
#include "Domains/Blueprint/Authoring/UeAgentBlueprintAuthoringProjection.h"
#include "Domains/UMG/Authoring/UeAgentUmgAuthoringDescriptors.h"
#include "Utilities/AuthoringExport/UeAgentCompoundProjectionExtension.h"

namespace UeAgentUmgAuthoring
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
			Result.ContractId = ExportContractId;
			Result.ContractVersion = TEXT("2.0.0");
			Result.TypeId = TEXT("umg");
			Result.SemanticDescriptorRefs = {
				AssetSemanticId,
				ExportTargetSemanticId,
				ParentClassSemanticId,
				WidgetSemanticId,
				WidgetPropertySemanticId,
				SlotSemanticId,
				SlotPropertySemanticId,
				BindingSemanticId,
				AnimationSemanticId,
				UeAgentBlueprintAuthoring::VariableSemanticId,
				UeAgentBlueprintAuthoring::DelegateSemanticId,
				UeAgentBlueprintAuthoring::ClassDefaultPropertySemanticId,
				UeAgentBlueprintAuthoring::GraphSemanticId,
				UeAgentBlueprintAuthoring::NodeSemanticId,
				UeAgentBlueprintAuthoring::NodePropertySemanticId,
				UeAgentBlueprintAuthoring::PinDefaultSemanticId,
				UeAgentBlueprintAuthoring::EdgeSemanticId,
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
			IndexFile.ProjectionId = ProjectionId;
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
				Field(TEXT("parent_class"), TEXT("index.hjson#/provider_extension/parent_class"),
					EFieldSourceKind::Semantic, ParentClassSemanticId,
					TEXT("uai.export.semantic.value"),
					TEXT("authoring"), true, true, TEXT("emit_null")),
				Field(TEXT("variables"), TEXT("index.hjson#/provider_extension/members/variables/*"),
					EFieldSourceKind::Semantic, UeAgentBlueprintAuthoring::VariableSemanticId,
					TEXT("uai.export.semantic.value"),
					TEXT("authoring"), true, true, TEXT("emit_empty")),
				Field(TEXT("delegates"), TEXT("index.hjson#/provider_extension/members/delegates/*"),
					EFieldSourceKind::Semantic, UeAgentBlueprintAuthoring::DelegateSemanticId,
					TEXT("uai.export.semantic.value"),
					TEXT("authoring"), true, true, TEXT("emit_empty")),
				Field(TEXT("class_defaults"), TEXT("index.hjson#/provider_extension/class_defaults/properties"),
					EFieldSourceKind::Semantic, UeAgentBlueprintAuthoring::ClassDefaultPropertySemanticId,
					TEXT("uai.export.semantic.editable_properties"),
					TEXT("authoring"), true, false, TEXT("emit_empty")),
				Field(TEXT("widgets"), TEXT("index.hjson#/provider_extension/widget_tree/widgets/*"),
					EFieldSourceKind::Semantic, WidgetSemanticId,
					TEXT("uai.export.semantic.value"),
					TEXT("authoring"), true, true, TEXT("emit_empty")),
				Field(TEXT("widget_properties"), TEXT("index.hjson#/provider_extension/widget_tree/widgets/*/properties"),
					EFieldSourceKind::Semantic, WidgetPropertySemanticId,
					TEXT("uai.export.semantic.editable_properties"),
					TEXT("authoring"), true, false, TEXT("omit")),
				Field(TEXT("slots"), TEXT("index.hjson#/provider_extension/widget_tree/widgets/*/slot"),
					EFieldSourceKind::Semantic, SlotSemanticId,
					TEXT("uai.export.semantic.value"),
					TEXT("authoring"), true, false, TEXT("omit")),
				Field(TEXT("slot_properties"), TEXT("index.hjson#/provider_extension/widget_tree/widgets/*/slot/properties"),
					EFieldSourceKind::Semantic, SlotPropertySemanticId,
					TEXT("uai.export.semantic.editable_properties"),
					TEXT("authoring"), true, false, TEXT("omit")),
				Field(TEXT("bindings"), TEXT("index.hjson#/provider_extension/bindings/*"),
					EFieldSourceKind::Semantic, BindingSemanticId,
					TEXT("uai.export.semantic.value"),
					TEXT("authoring"), true, true, TEXT("emit_empty")),
				Field(TEXT("animations"), TEXT("index.hjson#/provider_extension/animations/*"),
					EFieldSourceKind::Semantic, AnimationSemanticId,
					TEXT("uai.export.semantic.value"),
					TEXT("authoring"), true, true, TEXT("emit_empty")),
				Field(TEXT("graphs"), TEXT("index.hjson#/provider_extension/graphs/*"),
					EFieldSourceKind::Semantic, UeAgentBlueprintAuthoring::GraphSemanticId,
					TEXT("uai.export.semantic.value"),
					TEXT("authoring"), true, true, TEXT("emit_empty")),
				Field(TEXT("nodes"), TEXT("index.hjson#/provider_extension/graphs/*/nodes/*"),
					EFieldSourceKind::Semantic, UeAgentBlueprintAuthoring::NodeSemanticId,
					TEXT("uai.export.semantic.value"),
					TEXT("authoring"), true, true, TEXT("emit_empty")),
				Field(TEXT("node_properties"), TEXT("index.hjson#/provider_extension/graphs/*/nodes/*/properties"),
					EFieldSourceKind::Semantic, UeAgentBlueprintAuthoring::NodePropertySemanticId,
					TEXT("uai.export.semantic.editable_properties"),
					TEXT("authoring"), true, false, TEXT("omit")),
				Field(TEXT("pin_defaults"), TEXT("index.hjson#/provider_extension/graphs/*/nodes/*/pin_defaults/*"),
					EFieldSourceKind::Semantic, UeAgentBlueprintAuthoring::PinDefaultSemanticId,
					TEXT("uai.export.semantic.value"),
					TEXT("authoring"), true, false, TEXT("omit")),
				Field(TEXT("edges"), TEXT("index.hjson#/provider_extension/graphs/*/edges/*"),
					EFieldSourceKind::Semantic, UeAgentBlueprintAuthoring::EdgeSemanticId,
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

		FString WidgetKey(const UeAgentAuthoringState::FStateEntry& Entry)
		{
			const FString Guid = Entry.TargetIdentity.GetSelector(TEXT("widget_guid"));
			if (!Guid.IsEmpty()) return Guid.ToLower();
			const FString Name = Entry.TargetIdentity.GetSelector(TEXT("widget_name"));
			return Name.IsEmpty() ? FString() : FString(TEXT("widget:")) + Name;
		}

		bool GetProjectionKeys(
			const FString& FieldId,
			const UeAgentAuthoringState::FStateEntry& Entry,
			TArray<FString>& OutKeys)
		{
			OutKeys.Reset();
			if (FieldId == TEXT("widgets")
				|| FieldId == TEXT("widget_properties")
				|| FieldId == TEXT("slots")
				|| FieldId == TEXT("slot_properties"))
			{
				OutKeys.Add(WidgetKey(Entry));
			}
			else if (FieldId == TEXT("bindings"))
			{
				const FString WidgetName = Entry.TargetIdentity.GetSelector(TEXT("widget_name"));
				const FString PropertyName = Entry.TargetIdentity.GetSelector(TEXT("property_name"));
				if (!WidgetName.IsEmpty() && !PropertyName.IsEmpty())
				{
					OutKeys.Add(WidgetName + TEXT(".") + PropertyName);
				}
			}
			else if (FieldId == TEXT("animations"))
			{
				OutKeys.Add(Entry.TargetIdentity.GetSelector(TEXT("animation_name")));
			}
			else
			{
				return UeAgentBlueprintAuthoring::GetReusableProjectionKeys(FieldId, Entry, OutKeys);
			}
			return !OutKeys.IsEmpty() && !OutKeys.ContainsByPredicate([](const FString& Key)
			{
				return Key.IsEmpty();
			});
		}

		bool ProjectUmgStructure(
			const UeAgentCompoundProjection::FRequest& Request,
			TArray<UeAgentCompoundProjection::FExpansionSelection>& OutSelections,
			FString& OutError)
		{
			OutSelections.Reset();
			OutError.Reset();
			if (!Request.IsValid() || Request.File->ProjectionId != ProjectionId)
			{
				OutError = TEXT("umg_projection_request_invalid");
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
					OutError = TEXT("umg_projection_identity_missing");
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

	bool RegisterExportAuthoring(FString& OutError)
	{
		UeAgentExportContract::ERegistrationResult ContractResult;
		if (!UeAgentExportContract::FRegistry::Get().RegisterContract(
			MakeExportContract(), ContractResult, OutError))
		{
			return false;
		}
		UeAgentCompoundProjection::FExtensionDescriptor Descriptor;
		Descriptor.ProjectionId = ProjectionId;
		Descriptor.ImplementationId = TEXT("umg.snapshot.compound.v2");
		Descriptor.SourceKinds = { UeAgentExportContract::EFieldSourceKind::Semantic };
		UeAgentCompoundProjection::ERegistrationResult ExtensionResult;
		return UeAgentCompoundProjection::FRegistry::Get().RegisterExtension(
			Descriptor, ProjectUmgStructure, ExtensionResult, OutError);
	}

	bool GetExportContract(UeAgentExportContract::FContract& OutContract, FString& OutError)
	{
		if (!UeAgentExportContract::FRegistry::Get().FindContract(ExportContractId, OutContract))
		{
			OutError = TEXT("umg_export_contract_missing");
			return false;
		}
		OutError.Reset();
		return true;
	}
}
