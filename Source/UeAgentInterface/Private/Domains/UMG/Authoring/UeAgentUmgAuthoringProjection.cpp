// Copyright Epic Games, Inc. All Rights Reserved.

#include "Domains/UMG/Authoring/UeAgentUmgAuthoringProjection.h"

#include "Domains/Blueprint/Authoring/UeAgentBlueprintAuthoringDescriptors.h"
#include "Domains/Blueprint/Authoring/UeAgentBlueprintAuthoringProjection.h"
#include "Domains/UMG/Authoring/UeAgentUmgAuthoringDescriptors.h"
#include "Utilities/AuthoringExport/UeAgentCompoundProjectionExtension.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"

namespace UeAgentUmgAuthoring
{
	namespace
	{
		using namespace UeAgentExportContract;

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
				Profile(TEXT("llm_authoring"), { TEXT("identity"), TEXT("authoring") }),
				Profile(TEXT("readback"), { TEXT("identity"), TEXT("authoring"), TEXT("readback") }),
				Profile(TEXT("debug"), {
					TEXT("identity"), TEXT("authoring"), TEXT("readback"),
					TEXT("diagnostics"), TEXT("validation") }),
				Profile(TEXT("validation"), { TEXT("identity"), TEXT("validation") }),
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

		bool GetProjectionKeys(const FString& FieldId, const UeAgentAuthoringState::FStateEntry& Entry,
			TArray<FString>& OutKeys, UE::UAI::Contract::FDiagnostic& OutError)
		{
			OutKeys.Reset(); OutError.Reset();
FString Required, Expected = TEXT("nonempty captured selector");
			if (FieldId == TEXT("widgets") || FieldId == TEXT("widget_properties") || FieldId == TEXT("slots") || FieldId == TEXT("slot_properties")) {
OutKeys.Add(WidgetKey(Entry)); Required = TEXT("widget_name");
				Expected = TEXT("captured widget_guid or nonempty widget_name");
			} else if (FieldId == TEXT("bindings")) {
				const FString WidgetName = Entry.TargetIdentity.GetSelector(TEXT("widget_name"));
				const FString PropertyName = Entry.TargetIdentity.GetSelector(TEXT("property_name"));
				if (!WidgetName.IsEmpty() && !PropertyName.IsEmpty()) OutKeys.Add(WidgetName + TEXT(".") + PropertyName);
				Required = WidgetName.IsEmpty() ? TEXT("widget_name") : TEXT("property_name");
			} else if (FieldId == TEXT("animations")) {
				OutKeys.Add(Entry.TargetIdentity.GetSelector(TEXT("animation_name"))); Required = TEXT("animation_name");
			} else return UeAgentBlueprintAuthoring::GetReusableProjectionKeys(FieldId, Entry, OutKeys, OutError);
			if (!OutKeys.IsEmpty() && !OutKeys.ContainsByPredicate([](const FString& Key) { return Key.IsEmpty(); })) return true;
			OutError = UE::UAI::Contract::FDiagnostic::Failure(TEXT("umg_projection_identity_missing"),
TEXT("Projection field '") + FieldId + TEXT("' has no usable identity; expected ") + Expected + TEXT("."), TEXT("export_projection"));
			OutError.SourceKind = TEXT("native_result");
			OutError.TargetPath = Entry.TargetIdentity.GetSelector(TEXT("asset_path"));
			OutError.PropertyPath = TEXT("/identity/") + Required;
			OutError.ActualValue = MakeShared<FJsonValueString>(Entry.TargetIdentity.GetSelector(Required));
OutError.ExpectedValue = MakeShared<FJsonValueString>(Expected);
			return false;
		}

		bool ProjectUmgStructure(
			const UeAgentCompoundProjection::FRequest& Request,
			TArray<UeAgentCompoundProjection::FExpansionSelection>& OutSelections,
			UE::UAI::Contract::FDiagnostic& OutError)
		{
			OutSelections.Reset();
			OutError.Reset();
			if (!Request.IsValid()) {
				const TCHAR* Missing = !Request.Contract ? TEXT("contract") : !Request.Profile ? TEXT("profile")
					: !Request.File ? TEXT("file") : !Request.Field ? TEXT("field") : TEXT("wildcard_count");
				OutError = UE::UAI::Contract::FDiagnostic::Failure(TEXT("umg_projection_request_invalid"),
					FString::Printf(TEXT("Export projection requires %s."), Missing), TEXT("export_projection"));
				OutError.SourceKind = TEXT("native_result");
				OutError.PropertyPath = TEXT("/") + FString(Missing);
				if (Request.Contract && Request.Profile && Request.File && Request.Field) {
					OutError.ActualValue = MakeShared<FJsonValueNumber>(Request.WildcardCount);
					OutError.ExpectedValue = MakeShared<FJsonValueString>(TEXT("positive wildcard count"));
				} else {
					OutError.ActualValue = MakeShared<FJsonValueBoolean>(false);
					OutError.ExpectedValue = MakeShared<FJsonValueBoolean>(true);
				}
				return false;
			}
			if (Request.File->ProjectionId != ProjectionId) {
				OutError = UE::UAI::Contract::FDiagnostic::Failure(TEXT("umg_projection_request_invalid"),
					TEXT("The export file is assigned to a different projection."), TEXT("export_projection"));
				OutError.SourceKind = TEXT("native_result");
				OutError.PropertyPath = TEXT("/file/projection_id");
				OutError.ActualValue = MakeShared<FJsonValueString>(Request.File->ProjectionId);
				OutError.ExpectedValue = MakeShared<FJsonValueString>(ProjectionId);
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
				if (!GetProjectionKeys(Request.Field->FieldId, Request.Entries[Index], Keys, OutError)) {
					OutError.SourceKind = TEXT("native_result");
					if (OutError.TargetPath.IsEmpty()) OutError.TargetPath = Request.Entries[Index].TargetIdentity.GetSelector(TEXT("asset_path"));
					OutError.PropertyPath = TEXT("/entries/") + FString::FromInt(Index) + OutError.PropertyPath;
					return false;
				}
				if (Keys.Num() != Request.WildcardCount) {
					OutError = UE::UAI::Contract::FDiagnostic::Failure(TEXT("umg_projection_key_count_mismatch"),
						TEXT("Projection field '") + Request.Field->FieldId + TEXT("' produced a key count different from its wildcard count."), TEXT("export_projection"));
					OutError.SourceKind = TEXT("native_result");
					OutError.TargetPath = Request.Entries[Index].TargetIdentity.GetSelector(TEXT("asset_path"));
					OutError.PropertyPath = TEXT("/entries/") + FString::FromInt(Index) + TEXT("/keys/count");
					OutError.ActualValue = MakeShared<FJsonValueNumber>(Keys.Num());
					OutError.ExpectedValue = MakeShared<FJsonValueNumber>(Request.WildcardCount);
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

	bool RegisterExportAuthoring(UE::UAI::Contract::FDiagnostic& OutError)
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

	bool GetExportContract(UeAgentExportContract::FContract& OutContract, UE::UAI::Contract::FDiagnostic& OutError)
	{
		if (!UeAgentExportContract::FRegistry::Get().FindContract(ExportContractId, OutContract))
		{
			OutError = UE::UAI::Contract::FDiagnostic::Failure(TEXT("umg_export_contract_missing"),
				TEXT("The UMG export contract is not registered."), TEXT("export_projection"));
			OutError.SourceKind = TEXT("native_result");
			OutError.PropertyPath = TEXT("/export_contract");
			OutError.ActualValue = MakeShared<FJsonValueBoolean>(false);
			OutError.ExpectedValue = MakeShared<FJsonValueString>(ExportContractId);
			return false;
		}
		OutError.Reset();
		return true;
	}
}
