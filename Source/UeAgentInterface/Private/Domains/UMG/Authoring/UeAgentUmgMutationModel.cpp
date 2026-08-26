// Copyright Epic Games, Inc. All Rights Reserved.

#include "Domains/UMG/Authoring/UeAgentUmgMutationModel.h"

#include "Animation/MovieScene2DTransformSection.h"
#include "Animation/MovieScene2DTransformTrack.h"
#include "Animation/WidgetAnimation.h"
#include "Blueprint/WidgetTree.h"
#include "Channels/MovieSceneFloatChannel.h"
#include "Commands/Sequence/UeAgentHttpServer_SequenceOps.h"
#include "Commands/UMG/UeAgentHttpServer_UMGOps.h"
#include "Components/PanelSlot.h"
#include "Components/PanelWidget.h"
#include "Components/Widget.h"
#include "Core/HjsonWorkspace/DomainAuthoring/UeAgentDomainHjsonAuthoringKit.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Kismet2/BlueprintEditorUtils.h"
#include "Kismet2/CompilerResultsLog.h"
#include "Kismet2/KismetEditorUtilities.h"
#include "MovieScene.h"
#include "QueryOwners/UmgWidget/UeAgentUmgWidgetQueryOwner.h"
#include "Sections/MovieSceneColorSection.h"
#include "Sections/MovieSceneFloatSection.h"
#include "Tracks/MovieSceneColorTrack.h"
#include "Tracks/MovieSceneFloatTrack.h"
#include "Templates/WidgetTemplateClass.h"
#include "Utilities/AuthoringCore/UeAgentAuthoringState.h"
#include "Utilities/AuthoringCore/UeAgentDetailsAuthoringService.h"
#include "WidgetBlueprint.h"

namespace UeAgentUmgAuthoring
{
namespace
{
struct FMapEntry
{
	FString Key;
	TSharedPtr<FJsonObject> Spec;
};

void SetError(FString& OutError, const FString& Code, const FString& Detail = FString())
{
	OutError = Detail.IsEmpty() ? Code : Code + TEXT(":") + Detail;
}

bool TryGetString(
	const TSharedPtr<const FJsonObject>& Object, const TCHAR* Field, FString& OutValue)
{
	return Object.IsValid() && Object->TryGetStringField(Field, OutValue);
}

bool TryGetBool(const TSharedPtr<const FJsonObject>& Object, const TCHAR* Field, bool& OutValue)
{
	return Object.IsValid() && Object->TryGetBoolField(Field, OutValue);
}

bool TryGetNumber(const TSharedPtr<const FJsonObject>& Object, const TCHAR* Field, double& OutValue)
{
	return Object.IsValid() && Object->TryGetNumberField(Field, OutValue);
}

TSharedPtr<FJsonObject> GetObject(const TSharedPtr<const FJsonObject>& Object, const TCHAR* Field)
{
	const TSharedPtr<FJsonObject>* Value = nullptr;
	return Object.IsValid() && Object->TryGetObjectField(Field, Value) && Value ? *Value : nullptr;
}

bool GetObjectMap(const TSharedPtr<const FJsonObject>& Object,
	const TCHAR* Field,
	TArray<FMapEntry>& OutEntries,
	bool& bOutPresent,
	FString& OutError)
{
	OutEntries.Reset();
	bOutPresent = Object.IsValid() && Object->HasField(Field);
	if (!bOutPresent) return true;
	const TSharedPtr<FJsonObject> Map = GetObject(Object, Field);
	if (!Map.IsValid())
	{
		SetError(OutError, TEXT("umg_object_map_invalid"), Field);
		return false;
	}
	TArray<FString> Keys;
	Map->Values.GenerateKeyArray(Keys);
	Keys.Sort();
	for (const FString& Key : Keys)
	{
		const TSharedPtr<FJsonValue> Value = Map->Values.FindRef(Key);
		const TSharedPtr<FJsonObject>* Spec = nullptr;
		if (!Value.IsValid() || !Value->TryGetObject(Spec) || !Spec || !Spec->IsValid())
		{
			SetError(OutError, TEXT("umg_object_map_item_invalid"), Key);
			return false;
		}
		OutEntries.Add({Key, *Spec});
	}
	return true;
}

FString Operation(const TSharedPtr<const FJsonObject>& Spec)
{
	FString Value = TEXT("upsert");
	TryGetString(Spec, TEXT("operation"), Value);
	Value.TrimStartAndEndInline();
	Value.ToLowerInline();
	return Value;
}

bool ValidateKnownFields(const TSharedPtr<const FJsonObject>& Object,
	const TSet<FString>& Allowed,
	const TCHAR* ErrorCode,
	FString& OutError)
{
	if (!Object.IsValid()) return true;
	for (const TPair<FString, TSharedPtr<FJsonValue>>& Field : Object->Values)
	{
		if (!Allowed.Contains(Field.Key))
		{
			SetError(OutError, ErrorCode, Field.Key);
			return false;
		}
	}
	return true;
}

UClass* ResolveClass(const FString& ClassPath, UClass* RequiredBase)
{
	UClass* Class =
		ClassPath.IsEmpty() ? nullptr : StaticLoadClass(RequiredBase, nullptr, *ClassPath);
	return Class && Class->IsChildOf(RequiredBase) ? Class : nullptr;
}

bool CompileBlueprint(UWidgetBlueprint& Blueprint, FString& OutError)
{
	FCompilerResultsLog CompilerLog;
	CompilerLog.bLogInfoOnly = false;
	UeAgentUmgOps::SyncWidgetVariableGuidMap(&Blueprint);
	FKismetEditorUtilities::CompileBlueprint(
		&Blueprint, EBlueprintCompileOptions::None, &CompilerLog);
	if (CompilerLog.NumErrors > 0 || Blueprint.Status == BS_Error)
	{
		SetError(OutError, TEXT("umg_compile_failed"), LexToString(CompilerLog.NumErrors));
		return false;
	}
	return true;
}

bool ApplyProperties(UObject& Object,
	const TSharedPtr<const FJsonObject>& Spec,
	int32& OutApplied,
	FString& OutError)
{
	const TArray<TSharedPtr<FJsonValue>>* Properties = nullptr;
	if (!Spec.IsValid() || !Spec->TryGetArrayField(TEXT("properties"), Properties)) return true;
	if (!Properties)
	{
		SetError(OutError, TEXT("umg_properties_invalid"), Object.GetPathName());
		return false;
	}
	TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
	Payload->SetStringField(TEXT("schema"), TEXT("uai.uobject_properties.v2"));
	Payload->SetArrayField(TEXT("properties"), *Properties);
	UeAgentAuthoringObjectApply::FOptions Options;
	Options.bAllowTemplateOwners = true;
	Options.bAllowDomainAuthorizedOwners = true;
	Options.bEmitEditorNotifications = false;
	Options.bMarkPackageDirty = false;
	UeAgentAuthoringDirectApply::FResult ApplyResult;
	if (!UeAgentDetailsAuthoringService::ApplyObjectProperties(
			&Object, Payload, Options, ApplyResult, OutError))
	{
		SetError(OutError,
			ApplyResult.ErrorCode.IsEmpty() ? TEXT("umg_property_apply_failed")
											: ApplyResult.ErrorCode,
			ApplyResult.ErrorDetail.IsEmpty() ? Object.GetPathName() : ApplyResult.ErrorDetail);
		return false;
	}
	OutApplied += ApplyResult.PropertiesSeen;
	return true;
}

void CollectSubtree(UWidget* Widget, TArray<UWidget*>& OutWidgets)
{
	if (!Widget) return;
	OutWidgets.Add(Widget);
	if (UPanelWidget* Panel = Cast<UPanelWidget>(Widget))
	{
		for (int32 Index = 0; Index < Panel->GetChildrenCount(); ++Index)
		{
			CollectSubtree(Panel->GetChildAt(Index), OutWidgets);
		}
	}
}

UWidget* FindWidgetByGuidOrName(
	UWidgetBlueprint& Blueprint, const FString& Key, const TSharedPtr<const FJsonObject>& Spec)
{
	FGuid Guid;
	const FString GuidText = Key.StartsWith(TEXT("widget:")) ? FString() : Key;
	if (FGuid::Parse(GuidText, Guid))
	{
		for (const TPair<FName, FGuid>& Pair : Blueprint.WidgetVariableNameToGuidMap)
		{
			if (Pair.Value == Guid)
			{
				return UeAgentUmgOps::FindWidgetByAnyName(&Blueprint, Pair.Key.ToString());
			}
		}
	}
	FString Name;
	TryGetString(Spec, TEXT("name"), Name);
	if (Name.IsEmpty() && Key.StartsWith(TEXT("widget:"))) Name = Key.Mid(7);
	return Name.IsEmpty() ? nullptr : UeAgentUmgOps::FindWidgetByAnyName(&Blueprint, Name);
}

bool RemoveWidget(
	UWidgetBlueprint& Blueprint, UWidget& Widget, FMutationCounters& OutCounters, FString& OutError)
{
	TArray<UWidget*> Removed;
	CollectSubtree(&Widget, Removed);
	Blueprint.Modify();
	Blueprint.WidgetTree->Modify();
	const bool bRemoved = Blueprint.WidgetTree->RootWidget == &Widget
							  ? (Blueprint.WidgetTree->RootWidget = nullptr, true)
							  : Blueprint.WidgetTree->RemoveWidget(&Widget);
	if (!bRemoved)
	{
		SetError(OutError, TEXT("umg_widget_remove_failed"), Widget.GetName());
		return false;
	}
	for (UWidget* RemovedWidget : Removed)
	{
		if (!RemovedWidget) continue;
		Blueprint.OnVariableRemoved(RemovedWidget->GetFName());
		RemovedWidget->Rename(
			nullptr, GetTransientPackage(), REN_DontCreateRedirectors | REN_NonTransactional);
		++OutCounters.WidgetsRemoved;
	}
	return true;
}

bool AttachWidget(UWidgetBlueprint& Blueprint,
	UWidget& Widget,
	const FString& ParentName,
	const int32 InsertIndex,
	const bool bParentSpecified,
	FString& OutError)
{
	if (!bParentSpecified) return true;
	if (Widget.Slot && Widget.Slot->Parent)
	{
		Widget.Slot->Parent->RemoveChild(&Widget);
	}
	else if (Blueprint.WidgetTree->RootWidget == &Widget)
	{
		Blueprint.WidgetTree->RootWidget = nullptr;
	}

	if (ParentName.IsEmpty())
	{
		if (Blueprint.WidgetTree->RootWidget && Blueprint.WidgetTree->RootWidget != &Widget)
		{
			SetError(OutError, TEXT("umg_multiple_root_widgets"), Widget.GetName());
			return false;
		}
		Blueprint.WidgetTree->RootWidget = &Widget;
		return true;
	}

	UPanelWidget* Parent =
		Cast<UPanelWidget>(UeAgentUmgOps::FindWidgetByAnyName(&Blueprint, ParentName));
	if (!Parent)
	{
		SetError(OutError, TEXT("umg_parent_not_found_or_not_panel"), ParentName);
		return false;
	}
	UPanelSlot* Slot =
		InsertIndex >= 0 ? Parent->InsertChildAt(InsertIndex, &Widget) : Parent->AddChild(&Widget);
	if (!Slot)
	{
		SetError(OutError, TEXT("umg_native_slot_create_failed"), Widget.GetName());
		return false;
	}
	return true;
}

class FUeAgentNamedWidgetTemplateClass final : public FWidgetTemplateClass
{
public:
	explicit FUeAgentNamedWidgetTemplateClass(UClass* InWidgetClass)
		: FWidgetTemplateClass(InWidgetClass)
	{
	}

	UWidget* CreateNamedForAuthoring(UWidgetTree* Tree, const FName RequestedName)
	{
		return CreateNamed(Tree, RequestedName);
	}
};

bool TryResolveWidgetCandidates(
	UWidgetBlueprint& Blueprint,
	const TArray<FMapEntry>& Entries,
	TMap<FString, UE::UAI::QueryOwners::UmgWidget::FUmgWidgetCandidate>&
		OutCandidates,
	FString& OutError)
{
	using namespace UE::UAI::HjsonWorkspace;
	using namespace UE::UAI::HjsonWorkspace::DomainAuthoring;
	using namespace UE::UAI::QueryOwners::UmgWidget;

	OutCandidates.Reset();
	for (const FMapEntry& Entry : Entries)
	{
		const TSharedPtr<FJsonObject>* CandidateField = nullptr;
		if (!Entry.Spec->TryGetObjectField(TEXT("candidate"), CandidateField))
		{
			if (Entry.Spec->HasField(TEXT("candidate")))
			{
				SetError(OutError, TEXT("umg_widget_candidate_invalid"), Entry.Key);
				return false;
			}
			continue;
		}
		if (!CandidateField || !CandidateField->IsValid())
		{
			SetError(OutError, TEXT("umg_widget_candidate_invalid"), Entry.Key);
			return false;
		}

		FDomainHjsonCandidateSelector Selector;
		FWorkspaceError SelectorError;
		if (!TryReadCandidateSelector(
				*CandidateField,
				TEXT("/provider_extension/widget_tree/widgets/")
					+ Entry.Key + TEXT("/candidate"),
				Selector,
				SelectorError))
		{
			SetError(
				OutError,
				SelectorError.GetCode().IsEmpty()
					? FString(TEXT("umg_widget_candidate_selector_invalid"))
					: SelectorError.GetCode(),
				Entry.Key);
			return false;
		}

		FUmgWidgetCandidate Resolved;
		FUmgWidgetCandidateQueryError QueryError;
		if (!TryResolveUmgWidgetCandidate(
				Blueprint,
				Selector.Name,
				Selector.Id,
				Resolved,
				QueryError))
		{
			SetError(
				OutError,
				QueryError.Code.IsEmpty()
					? FString(TEXT("umg_widget_candidate_selector_invalid"))
					: QueryError.Code,
				QueryError.SubjectId.IsEmpty()
					? Entry.Key
					: QueryError.SubjectId);
			return false;
		}

		FString ClassAssertion;
		if (TryGetString(Entry.Spec, TEXT("class"), ClassAssertion)
			&& !ClassAssertion.IsEmpty()
			&& ClassAssertion != Resolved.ClassPath)
		{
			SetError(
				OutError,
				TEXT("umg_widget_candidate_class_conflict"),
				Entry.Key);
			return false;
		}
		OutCandidates.Add(Entry.Key, MoveTemp(Resolved));
	}
	return true;
}

bool TryCreateWidgetFromTemplate(
	UWidgetBlueprint& Blueprint,
	UClass& WidgetClass,
	const FString& Name,
	UWidget*& OutWidget,
	FString& OutError)
{
	OutWidget = nullptr;
	if (!Blueprint.WidgetTree || Name.IsEmpty())
	{
		SetError(OutError, TEXT("umg_widget_native_create_failed"), Name);
		return false;
	}
	FUeAgentNamedWidgetTemplateClass Template(&WidgetClass);
	OutWidget = Template.CreateNamedForAuthoring(
		Blueprint.WidgetTree, FName(*Name));
	if (!OutWidget)
	{
		SetError(OutError, TEXT("umg_widget_native_create_failed"), Name);
		return false;
	}
	return true;
}

bool ApplyWidgetTree(UWidgetBlueprint& Blueprint,
	const TSharedPtr<const FJsonObject>& Extension,
	FMutationCounters& OutCounters,
	FString& OutError)
{
	if (!Extension.IsValid() || !Extension->HasField(TEXT("widget_tree"))) return true;
	const TSharedPtr<FJsonObject> Tree = GetObject(Extension, TEXT("widget_tree"));
	if (!Tree.IsValid())
	{
		SetError(OutError, TEXT("umg_widget_tree_invalid"));
		return false;
	}
	if (!ValidateKnownFields(
			Tree, {TEXT("widgets")}, TEXT("umg_widget_tree_unknown_field"), OutError))
	{
		return false;
	}
	TArray<FMapEntry> Entries;
	bool bPresent = false;
	if (!GetObjectMap(Tree, TEXT("widgets"), Entries, bPresent, OutError) || !bPresent)
		return false;
	TMap<FString, UE::UAI::QueryOwners::UmgWidget::FUmgWidgetCandidate>
		ResolvedCandidates;

	for (const FMapEntry& Entry : Entries)
	{
		if (!ValidateKnownFields(Entry.Spec,
				{TEXT("operation"),
					TEXT("name"),
					TEXT("candidate"),
					TEXT("class"),
					TEXT("parent"),
					TEXT("index"),
					TEXT("make_variable"),
					TEXT("properties"),
					TEXT("slot")},
				TEXT("umg_widget_unknown_field"),
				OutError))
		{
			return false;
		}
	}
	if (!TryResolveWidgetCandidates(
			Blueprint, Entries, ResolvedCandidates, OutError))
	{
		return false;
	}
	UeAgentUmgOps::SyncWidgetVariableGuidMap(&Blueprint);

	for (const FMapEntry& Entry : Entries)
	{
		const FString Op = Operation(Entry.Spec);
		if (Op != TEXT("remove")) continue;
		UWidget* Existing = FindWidgetByGuidOrName(Blueprint, Entry.Key, Entry.Spec);
		if (!Existing)
		{
			SetError(OutError, TEXT("umg_widget_remove_target_not_found"), Entry.Key);
			return false;
		}
		if (!RemoveWidget(Blueprint, *Existing, OutCounters, OutError)) return false;
	}

	TSet<FString> Pending;
	for (const FMapEntry& Entry : Entries)
	{
		if (Operation(Entry.Spec) != TEXT("remove")) Pending.Add(Entry.Key);
	}
	while (!Pending.IsEmpty())
	{
		bool bProgress = false;
		for (const FMapEntry& Entry : Entries)
		{
			if (!Pending.Contains(Entry.Key)) continue;
			FString ParentName;
			const bool bParentSpecified = TryGetString(Entry.Spec, TEXT("parent"), ParentName);
			if (bParentSpecified && !ParentName.IsEmpty() &&
				!UeAgentUmgOps::FindWidgetByAnyName(&Blueprint, ParentName))
			{
				continue;
			}

			const FString Op = Operation(Entry.Spec);
			UWidget* Widget = FindWidgetByGuidOrName(Blueprint, Entry.Key, Entry.Spec);
			if (Op == TEXT("create") && Widget)
			{
				SetError(OutError, TEXT("umg_widget_already_exists"), Entry.Key);
				return false;
			}
			if (Op == TEXT("update") && !Widget)
			{
				SetError(OutError, TEXT("umg_widget_not_found"), Entry.Key);
				return false;
			}

			FString ClassPath;
			if (const UE::UAI::QueryOwners::UmgWidget::FUmgWidgetCandidate*
				ResolvedCandidate = ResolvedCandidates.Find(Entry.Key))
			{
				ClassPath = ResolvedCandidate->ClassPath;
			}
			else
			{
				TryGetString(Entry.Spec, TEXT("class"), ClassPath);
			}
			if (Widget && !ClassPath.IsEmpty() && Widget->GetClass()->GetPathName() != ClassPath)
			{
				if (Op != TEXT("replace"))
				{
					SetError(OutError, TEXT("umg_widget_class_conflict"), Entry.Key);
					return false;
				}
				if (!RemoveWidget(Blueprint, *Widget, OutCounters, OutError)) return false;
				Widget = nullptr;
			}

			if (!Widget)
			{
				UClass* WidgetClass = ResolveClass(ClassPath, UWidget::StaticClass());
				if (!WidgetClass)
				{
					SetError(OutError, TEXT("umg_widget_class_invalid"), ClassPath);
					return false;
				}
				FString Name;
				TryGetString(Entry.Spec, TEXT("name"), Name);
				if (Name.IsEmpty() && Entry.Key.StartsWith(TEXT("widget:")))
					Name = Entry.Key.Mid(7);
				if (Name.IsEmpty())
				{
					SetError(OutError, TEXT("umg_widget_name_missing"), Entry.Key);
					return false;
				}
				if (!TryCreateWidgetFromTemplate(
						Blueprint, *WidgetClass, Name, Widget, OutError))
				{
					return false;
				}
				bool bMakeVariable = true;
				TryGetBool(Entry.Spec, TEXT("make_variable"), bMakeVariable);
				Widget->bIsVariable = bMakeVariable;
				Blueprint.OnVariableAdded(Widget->GetFName());
				Blueprint.WidgetVariableNameToGuidMap.Add(Widget->GetFName(),
					UeAgentUmgOps::MakeStableWidgetVariableGuid(Widget->GetName()));
				++OutCounters.WidgetsAdded;
			}
			else
			{
				FString DesiredName;
				if (TryGetString(Entry.Spec, TEXT("name"), DesiredName) && !DesiredName.IsEmpty() &&
					Widget->GetName() != DesiredName)
				{
					if (UeAgentUmgOps::FindWidgetByAnyName(&Blueprint, DesiredName))
					{
						SetError(OutError, TEXT("umg_widget_name_conflict"), DesiredName);
						return false;
					}
					const FName OldName = Widget->GetFName();
					if (!Widget->Rename(
							*DesiredName, Blueprint.WidgetTree, REN_DontCreateRedirectors))
					{
						SetError(OutError, TEXT("umg_widget_rename_failed"), DesiredName);
						return false;
					}
					Blueprint.OnVariableRenamed(OldName, Widget->GetFName());
				}
				bool bMakeVariable = Widget->bIsVariable;
				if (TryGetBool(Entry.Spec, TEXT("make_variable"), bMakeVariable) &&
					bMakeVariable != Widget->bIsVariable)
				{
					Widget->Modify();
					Widget->bIsVariable = bMakeVariable;
				}
				++OutCounters.WidgetsUpdated;
			}

			double IndexNumber = -1.0;
			TryGetNumber(Entry.Spec, TEXT("index"), IndexNumber);
			if (!AttachWidget(Blueprint,
					*Widget,
					ParentName,
					static_cast<int32>(IndexNumber),
					bParentSpecified || !Widget->Slot,
					OutError))
			{
				return false;
			}

			if (!ApplyProperties(
					*Widget, Entry.Spec, OutCounters.WidgetPropertiesApplied, OutError))
			{
				return false;
			}
			const TSharedPtr<FJsonObject> SlotSpec = GetObject(Entry.Spec, TEXT("slot"));
			if (Entry.Spec->HasField(TEXT("slot")) && !SlotSpec.IsValid())
			{
				SetError(OutError, TEXT("umg_slot_spec_invalid"), Entry.Key);
				return false;
			}
			if (SlotSpec.IsValid())
			{
				if (!Widget->Slot)
				{
					SetError(OutError, TEXT("umg_root_widget_has_no_slot"), Widget->GetName());
					return false;
				}
				if (!ValidateKnownFields(SlotSpec,
						{TEXT("class"), TEXT("properties")},
						TEXT("umg_slot_unknown_field"),
						OutError))
				{
					return false;
				}
				FString SlotClass;
				if (TryGetString(SlotSpec, TEXT("class"), SlotClass) && !SlotClass.IsEmpty() &&
					Widget->Slot->GetClass()->GetPathName() != SlotClass)
				{
					SetError(OutError,
						TEXT("umg_native_slot_class_conflict"),
						Widget->Slot->GetClass()->GetPathName() + TEXT(" != ") + SlotClass);
					return false;
				}
				if (!ApplyProperties(
						*Widget->Slot, SlotSpec, OutCounters.SlotPropertiesApplied, OutError))
				{
					return false;
				}
			}
			Pending.Remove(Entry.Key);
			bProgress = true;
		}
		if (!bProgress)
		{
			TArray<FString> Remaining = Pending.Array();
			Remaining.Sort();
			SetError(OutError,
				TEXT("umg_widget_parent_cycle_or_missing"),
				FString::Join(Remaining, TEXT(",")));
			return false;
		}
	}
	UeAgentUmgOps::SyncWidgetVariableGuidMap(&Blueprint);
	FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(&Blueprint);
	return true;
}

bool ApplyBindings(UWidgetBlueprint& Blueprint,
	const TSharedPtr<const FJsonObject>& Extension,
	FMutationCounters& OutCounters,
	FString& OutError)
{
	TArray<FMapEntry> Entries;
	bool bPresent = false;
	if (!GetObjectMap(Extension, TEXT("bindings"), Entries, bPresent, OutError) || !bPresent)
	{
		return !bPresent || OutError.IsEmpty();
	}
	for (const FMapEntry& Entry : Entries)
	{
		if (!ValidateKnownFields(Entry.Spec,
				{TEXT("operation"),
					TEXT("widget_name"),
					TEXT("property_name"),
					TEXT("binding_kind"),
					TEXT("source_variable_name"),
					TEXT("function_name")},
				TEXT("umg_binding_unknown_field"),
				OutError))
		{
			return false;
		}
		FString WidgetName;
		FString PropertyName;
		TryGetString(Entry.Spec, TEXT("widget_name"), WidgetName);
		TryGetString(Entry.Spec, TEXT("property_name"), PropertyName);
		if (WidgetName.IsEmpty() || PropertyName.IsEmpty())
		{
			SetError(OutError, TEXT("umg_binding_identity_missing"), Entry.Key);
			return false;
		}
		const int32 ExistingIndex = Blueprint.Bindings.IndexOfByPredicate(
			[&WidgetName, &PropertyName](const FDelegateEditorBinding& Binding)
			{
				return Binding.ObjectName == WidgetName &&
					   Binding.PropertyName.ToString() == PropertyName;
			});
		const FString Op = Operation(Entry.Spec);
		if (Op == TEXT("remove"))
		{
			if (ExistingIndex == INDEX_NONE)
			{
				SetError(OutError, TEXT("umg_binding_remove_target_not_found"), Entry.Key);
				return false;
			}
			Blueprint.Bindings.RemoveAt(ExistingIndex);
			++OutCounters.BindingsRemoved;
			continue;
		}
		if (!UeAgentUmgOps::FindWidgetByAnyName(&Blueprint, WidgetName))
		{
			SetError(OutError, TEXT("umg_binding_widget_not_found"), WidgetName);
			return false;
		}
		FDelegateEditorBinding Binding;
		Binding.ObjectName = WidgetName;
		Binding.PropertyName = FName(*PropertyName);
		FString Kind;
		TryGetString(Entry.Spec, TEXT("binding_kind"), Kind);
		if (Kind.Equals(TEXT("property_variable"), ESearchCase::IgnoreCase))
		{
			FString SourceVariable;
			TryGetString(Entry.Spec, TEXT("source_variable_name"), SourceVariable);
			FProperty* SourceProperty =
				Blueprint.SkeletonGeneratedClass
					? FindFProperty<FProperty>(
						  Blueprint.SkeletonGeneratedClass, FName(*SourceVariable))
					: nullptr;
			if (!SourceProperty)
			{
				SetError(OutError, TEXT("umg_binding_source_variable_not_found"), SourceVariable);
				return false;
			}
			Binding.FunctionName = FName(*SourceVariable);
			Binding.SourceProperty = FName(*SourceVariable);
			Binding.SourcePath =
				FEditorPropertyPath(TArray<FFieldVariant>{FFieldVariant(SourceProperty)});
			Binding.Kind = EBindingKind::Property;
		}
		else
		{
			FString FunctionName;
			TryGetString(Entry.Spec, TEXT("function_name"), FunctionName);
			if (FunctionName.IsEmpty())
			{
				SetError(OutError, TEXT("umg_binding_function_missing"), Entry.Key);
				return false;
			}
			Binding.FunctionName = FName(*FunctionName);
			Binding.Kind = EBindingKind::Function;
		}
		if (ExistingIndex == INDEX_NONE)
		{
			Blueprint.Bindings.Add(Binding);
			++OutCounters.BindingsAdded;
		}
		else
		{
			Blueprint.Bindings[ExistingIndex] = Binding;
			++OutCounters.BindingsUpdated;
		}
	}
	FBlueprintEditorUtils::MarkBlueprintAsModified(&Blueprint);
	return true;
}

FFrameNumber KeyFrame(UMovieScene& MovieScene, const TSharedPtr<const FJsonObject>& Key)
{
	double TimeSeconds = 0.0;
	if (TryGetNumber(Key, TEXT("time_seconds"), TimeSeconds))
	{
		return MovieScene.GetTickResolution().AsFrameTime(TimeSeconds).RoundToFrame();
	}
	double Frame = 0.0;
	TryGetNumber(Key, TEXT("frame"), Frame);
	return FFrameNumber(static_cast<int32>(Frame));
}

bool AddTrackKeys(UWidgetBlueprint& Blueprint,
	UWidgetAnimation& Animation,
	const TSharedPtr<const FJsonObject>& Track,
	FMutationCounters& OutCounters,
	FString& OutError)
{
	FString WidgetName;
	FString Kind;
	TryGetString(Track, TEXT("widget_name"), WidgetName);
	TryGetString(Track, TEXT("track_kind"), Kind);
	UWidget* Widget = UeAgentUmgOps::FindWidgetByAnyName(&Blueprint, WidgetName);
	if (!Widget || Kind.IsEmpty())
	{
		SetError(OutError, TEXT("umg_animation_track_invalid"), WidgetName + TEXT(":") + Kind);
		return false;
	}
	const TArray<TSharedPtr<FJsonValue>>* Keys = nullptr;
	if (!Track->TryGetArrayField(TEXT("keys"), Keys) || !Keys)
	{
		SetError(OutError, TEXT("umg_animation_keys_invalid"), WidgetName);
		return false;
	}
	UMovieScene* MovieScene = nullptr;
	FGuid BindingGuid;
	bool bBindingExists = false;
	if (!UeAgentSequenceOps::FindOrCreateWidgetAnimationBinding(
			&Blueprint, &Animation, Widget, MovieScene, BindingGuid, bBindingExists, OutError))
	{
		return false;
	}

	if (Kind == TEXT("transform"))
	{
		UMovieScene2DTransformTrack* ResultTrack = nullptr;
		UMovieScene2DTransformSection* Section = nullptr;
		bool bTrackExists = false;
		if (!UeAgentSequenceOps::FindOrCreateUmgTransformSection(
				MovieScene, BindingGuid, ResultTrack, Section, bTrackExists, OutError))
			return false;
		EMovieScene2DTransformChannel Mask = Section->GetMask().GetChannels();
		for (const TSharedPtr<FJsonValue>& KeyValue : *Keys)
		{
			const TSharedPtr<FJsonObject>* KeyObject = nullptr;
			if (!KeyValue.IsValid() || !KeyValue->TryGetObject(KeyObject) || !KeyObject ||
				!KeyObject->IsValid())
			{
				SetError(OutError, TEXT("umg_animation_key_invalid"), WidgetName);
				return false;
			}
			const FFrameNumber Frame = KeyFrame(*MovieScene, *KeyObject);
			FMovieSceneFloatChannel* Channels[] = {&Section->Translation[0],
				&Section->Translation[1],
				&Section->Scale[0],
				&Section->Scale[1],
				&Section->Shear[0],
				&Section->Shear[1],
				&Section->Rotation};
			static const TCHAR* Fields[] = {TEXT("translation_x"),
				TEXT("translation_y"),
				TEXT("scale_x"),
				TEXT("scale_y"),
				TEXT("shear_x"),
				TEXT("shear_y"),
				TEXT("angle")};
			static const EMovieScene2DTransformChannel Masks[] = {
				EMovieScene2DTransformChannel::TranslationX,
				EMovieScene2DTransformChannel::TranslationY,
				EMovieScene2DTransformChannel::ScaleX,
				EMovieScene2DTransformChannel::ScaleY,
				EMovieScene2DTransformChannel::ShearX,
				EMovieScene2DTransformChannel::ShearY,
				EMovieScene2DTransformChannel::Rotation};
			for (int32 ChannelIndex = 0; ChannelIndex < UE_ARRAY_COUNT(Fields); ++ChannelIndex)
			{
				double Value = 0.0;
				if (TryGetNumber(*KeyObject, Fields[ChannelIndex], Value))
				{
					Channels[ChannelIndex]->GetData().UpdateOrAddKey(
						Frame, FMovieSceneFloatValue(static_cast<float>(Value)));
					Mask = Mask | Masks[ChannelIndex];
				}
			}
			++OutCounters.AnimationKeysApplied;
		}
		Section->SetMask(FMovieScene2DTransformMask(Mask));
		Section->SetRange(TRange<FFrameNumber>::All());
		return true;
	}

	if (Kind == TEXT("opacity") || Kind == TEXT("float_property"))
	{
		FString PropertyName = TEXT("RenderOpacity");
		FString PropertyPath = TEXT("RenderOpacity");
		TryGetString(Track, TEXT("property_name"), PropertyName);
		TryGetString(Track, TEXT("property_path"), PropertyPath);
		UMovieSceneFloatTrack* ResultTrack = nullptr;
		UMovieSceneFloatSection* Section = nullptr;
		bool bTrackExists = false;
		const bool bCreated =
			Kind == TEXT("opacity")
				? UeAgentSequenceOps::FindOrCreateUmgOpacitySection(
					  MovieScene, BindingGuid, ResultTrack, Section, bTrackExists, OutError)
				: UeAgentSequenceOps::FindOrCreateUmgFloatSection(MovieScene,
					  BindingGuid,
					  FName(*PropertyName),
					  PropertyPath,
					  ResultTrack,
					  Section,
					  bTrackExists,
					  OutError);
		if (!bCreated) return false;
		for (const TSharedPtr<FJsonValue>& KeyValue : *Keys)
		{
			const TSharedPtr<FJsonObject>* KeyObject = nullptr;
			if (!KeyValue.IsValid() || !KeyValue->TryGetObject(KeyObject) || !KeyObject ||
				!KeyObject->IsValid())
				return false;
			double Value = 0.0;
			if (!TryGetNumber(*KeyObject, TEXT("value"), Value))
			{
				TryGetNumber(*KeyObject, TEXT("opacity"), Value);
			}
			Section->GetChannel().GetData().UpdateOrAddKey(KeyFrame(*MovieScene, *KeyObject),
				FMovieSceneFloatValue(static_cast<float>(Value)));
			++OutCounters.AnimationKeysApplied;
		}
		Section->SetRange(TRange<FFrameNumber>::All());
		return true;
	}

	if (Kind == TEXT("color") || Kind == TEXT("background_color") || Kind == TEXT("color_property"))
	{
		FString PropertyName =
			Kind == TEXT("background_color") ? TEXT("BackgroundColor") : TEXT("ColorAndOpacity");
		FString PropertyPath = PropertyName;
		TryGetString(Track, TEXT("property_name"), PropertyName);
		TryGetString(Track, TEXT("property_path"), PropertyPath);
		UMovieSceneColorTrack* ResultTrack = nullptr;
		UMovieSceneColorSection* Section = nullptr;
		bool bTrackExists = false;
		if (!UeAgentSequenceOps::FindOrCreateUmgColorSection(MovieScene,
				BindingGuid,
				FName(*PropertyName),
				PropertyPath,
				ResultTrack,
				Section,
				bTrackExists,
				OutError))
			return false;
		FMovieSceneFloatChannel* Channels[] = {&Section->GetRedChannel(),
			&Section->GetGreenChannel(),
			&Section->GetBlueChannel(),
			&Section->GetAlphaChannel()};
		static const TCHAR* Fields[] = {TEXT("red"), TEXT("green"), TEXT("blue"), TEXT("alpha")};
		for (const TSharedPtr<FJsonValue>& KeyValue : *Keys)
		{
			const TSharedPtr<FJsonObject>* KeyObject = nullptr;
			if (!KeyValue.IsValid() || !KeyValue->TryGetObject(KeyObject) || !KeyObject ||
				!KeyObject->IsValid())
				return false;
			const FFrameNumber Frame = KeyFrame(*MovieScene, *KeyObject);
			for (int32 ChannelIndex = 0; ChannelIndex < UE_ARRAY_COUNT(Fields); ++ChannelIndex)
			{
				double Value = ChannelIndex == 3 ? 1.0 : 0.0;
				TryGetNumber(*KeyObject, Fields[ChannelIndex], Value);
				Channels[ChannelIndex]->GetData().UpdateOrAddKey(
					Frame, FMovieSceneFloatValue(static_cast<float>(Value)));
			}
			++OutCounters.AnimationKeysApplied;
		}
		Section->SetRange(TRange<FFrameNumber>::All());
		return true;
	}
	SetError(OutError, TEXT("umg_animation_track_kind_unsupported"), Kind);
	return false;
}

bool ApplyAnimations(UWidgetBlueprint& Blueprint,
	const TSharedPtr<const FJsonObject>& Extension,
	FMutationCounters& OutCounters,
	FString& OutError)
{
	TArray<FMapEntry> Entries;
	bool bPresent = false;
	if (!GetObjectMap(Extension, TEXT("animations"), Entries, bPresent, OutError) || !bPresent)
	{
		return !bPresent || OutError.IsEmpty();
	}
	for (const FMapEntry& Entry : Entries)
	{
		if (!ValidateKnownFields(Entry.Spec,
				{TEXT("operation"),
					TEXT("name"),
					TEXT("display_label"),
					TEXT("start_seconds"),
					TEXT("duration_seconds"),
					TEXT("display_rate_num"),
					TEXT("display_rate_den"),
					TEXT("tracks")},
				TEXT("umg_animation_unknown_field"),
				OutError))
		{
			return false;
		}
		FString Name;
		TryGetString(Entry.Spec, TEXT("name"), Name);
		if (Name.IsEmpty()) Name = Entry.Key;
		UWidgetAnimation* Animation =
			UeAgentSequenceOps::FindWidgetAnimationByAnyName(&Blueprint, Name);
		const FString Op = Operation(Entry.Spec);
		if (Op == TEXT("remove"))
		{
			if (!Animation)
			{
				SetError(OutError, TEXT("umg_animation_remove_target_not_found"), Name);
				return false;
			}
			Blueprint.Animations.Remove(Animation);
			Blueprint.OnVariableRemoved(Animation->GetFName());
			Animation->Rename(
				nullptr, GetTransientPackage(), REN_DontCreateRedirectors | REN_NonTransactional);
			++OutCounters.AnimationsRemoved;
			continue;
		}
		if (!Animation)
		{
			Animation = NewObject<UWidgetAnimation>(&Blueprint, FName(*Name), RF_Transactional);
			if (!Animation)
			{
				SetError(OutError, TEXT("umg_animation_create_failed"), Name);
				return false;
			}
			Animation->MovieScene =
				NewObject<UMovieScene>(Animation, FName(*Name), RF_Transactional);
			if (!Animation->MovieScene)
			{
				SetError(OutError, TEXT("umg_animation_movie_scene_create_failed"), Name);
				return false;
			}
			Blueprint.Animations.Add(Animation);
			Blueprint.OnVariableAdded(Animation->GetFName());
			++OutCounters.AnimationsAdded;
		}
		else
		{
			++OutCounters.AnimationsUpdated;
		}
		FString DisplayLabel;
		if (TryGetString(Entry.Spec, TEXT("display_label"), DisplayLabel))
			Animation->SetDisplayLabel(DisplayLabel);
		double Start = Animation->GetStartTime();
		double Duration = FMath::Max(0.001, Animation->GetEndTime() - Animation->GetStartTime());
		const bool bHasStart = TryGetNumber(Entry.Spec, TEXT("start_seconds"), Start);
		const bool bHasDuration = TryGetNumber(Entry.Spec, TEXT("duration_seconds"), Duration);
		if (bHasStart || bHasDuration)
		{
			if (!UeAgentSequenceOps::SetMovieScenePlaybackRangeSeconds(
					Animation->MovieScene, Start, Duration))
			{
				SetError(OutError, TEXT("umg_animation_range_invalid"), Name);
				return false;
			}
		}
		double RateNum = Animation->MovieScene->GetDisplayRate().Numerator;
		double RateDen = Animation->MovieScene->GetDisplayRate().Denominator;
		const bool bHasRateNum = TryGetNumber(Entry.Spec, TEXT("display_rate_num"), RateNum);
		const bool bHasRateDen = TryGetNumber(Entry.Spec, TEXT("display_rate_den"), RateDen);
		if (bHasRateNum || bHasRateDen)
		{
			if (RateNum <= 0.0 || RateDen <= 0.0)
			{
				SetError(OutError, TEXT("umg_animation_display_rate_invalid"), Name);
				return false;
			}
			Animation->MovieScene->SetDisplayRate(
				FFrameRate(static_cast<int32>(RateNum), static_cast<int32>(RateDen)));
		}

		const TArray<TSharedPtr<FJsonValue>>* Tracks = nullptr;
		if (Entry.Spec->TryGetArrayField(TEXT("tracks"), Tracks))
		{
			if (!Tracks)
			{
				SetError(OutError, TEXT("umg_animation_tracks_invalid"), Name);
				return false;
			}
			Animation->AnimationBindings.Reset();
			Animation->MovieScene =
				NewObject<UMovieScene>(Animation, Animation->GetFName(), RF_Transactional);
			if (!Animation->MovieScene)
			{
				SetError(OutError, TEXT("umg_animation_movie_scene_reset_failed"), Name);
				return false;
			}
			UeAgentSequenceOps::SetMovieScenePlaybackRangeSeconds(
				Animation->MovieScene, Start, Duration);
			Animation->MovieScene->SetDisplayRate(
				FFrameRate(static_cast<int32>(RateNum), static_cast<int32>(RateDen)));
			for (const TSharedPtr<FJsonValue>& TrackValue : *Tracks)
			{
				const TSharedPtr<FJsonObject>* Track = nullptr;
				if (!TrackValue.IsValid() || !TrackValue->TryGetObject(Track) || !Track ||
					!Track->IsValid() ||
					!AddTrackKeys(Blueprint, *Animation, *Track, OutCounters, OutError))
				{
					if (OutError.IsEmpty())
						SetError(OutError, TEXT("umg_animation_track_invalid"), Name);
					return false;
				}
			}
		}
	}
	FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(&Blueprint);
	return true;
}

TSharedPtr<FJsonObject> BuildBlueprintExtension(const TSharedPtr<const FJsonObject>& Extension)
{
	if (!Extension.IsValid()) return nullptr;
	TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
	for (const TCHAR* Field :
		{TEXT("parent_class"), TEXT("class_defaults"), TEXT("members"), TEXT("graphs")})
	{
		if (const TSharedPtr<FJsonValue>* Value = Extension->Values.Find(Field))
		{
			Result->SetField(Field, UeAgentAuthoringState::CloneCanonicalValue(*Value));
		}
	}
	return Result;
}

bool ApplyDomainBeforeCompile(UWidgetBlueprint& Blueprint,
	const TSharedPtr<const FJsonObject>& Extension,
	FMutationCounters& OutCounters,
	FString& OutError)
{
	return ApplyWidgetTree(Blueprint, Extension, OutCounters, OutError) &&
		   ApplyAnimations(Blueprint, Extension, OutCounters, OutError);
}
} // namespace

struct FPreparedUmgMutation::FImpl
{
	TSharedPtr<const FJsonObject> Extension;
	TUniquePtr<UeAgentBlueprintAuthoring::FPreparedBlueprintMutation> BlueprintMutation;
	bool bRequiresCompile = false;
	bool bHasBindings = false;
};

void FMutationCounters::Add(const FMutationCounters& Other)
{
	WidgetsAdded += Other.WidgetsAdded;
	WidgetsUpdated += Other.WidgetsUpdated;
	WidgetsRemoved += Other.WidgetsRemoved;
	WidgetPropertiesApplied += Other.WidgetPropertiesApplied;
	SlotPropertiesApplied += Other.SlotPropertiesApplied;
	BindingsAdded += Other.BindingsAdded;
	BindingsUpdated += Other.BindingsUpdated;
	BindingsRemoved += Other.BindingsRemoved;
	AnimationsAdded += Other.AnimationsAdded;
	AnimationsUpdated += Other.AnimationsUpdated;
	AnimationsRemoved += Other.AnimationsRemoved;
	AnimationKeysApplied += Other.AnimationKeysApplied;
	Blueprint.Add(Other.Blueprint);
}

FPreparedUmgMutation::FPreparedUmgMutation(TUniquePtr<FImpl> InImpl) : Impl(MoveTemp(InImpl))
{
}

FPreparedUmgMutation::~FPreparedUmgMutation() = default;

bool FPreparedUmgMutation::Prepare(UWidgetBlueprint& ScratchBlueprint,
	const TSharedPtr<const FJsonObject>& ProviderExtension,
	TUniquePtr<FPreparedUmgMutation>& OutMutation,
	FMutationCounters& OutExpectedCounters,
	FString& OutError)
{
	OutMutation.Reset();
	OutExpectedCounters = FMutationCounters();
	OutError.Reset();
	if (!ValidateKnownFields(ProviderExtension,
			{TEXT("parent_class"),
				TEXT("compile"),
				TEXT("save"),
				TEXT("class_defaults"),
				TEXT("members"),
				TEXT("graphs"),
				TEXT("widget_tree"),
				TEXT("bindings"),
				TEXT("animations")},
			TEXT("umg_provider_extension_unknown_field"),
			OutError))
	{
		return false;
	}

	TUniquePtr<FImpl> NewImpl = MakeUnique<FImpl>();
	NewImpl->Extension = ProviderExtension;
	const TSharedPtr<FJsonObject> BlueprintExtension = BuildBlueprintExtension(ProviderExtension);
	if (!UeAgentBlueprintAuthoring::FPreparedBlueprintMutation::Prepare(ScratchBlueprint,
			BlueprintExtension,
			NewImpl->BlueprintMutation,
			OutExpectedCounters.Blueprint,
			OutError))
	{
		return false;
	}
	if (!ApplyDomainBeforeCompile(
			ScratchBlueprint, ProviderExtension, OutExpectedCounters, OutError))
	{
		return false;
	}
	NewImpl->bHasBindings =
		ProviderExtension.IsValid() && ProviderExtension->HasField(TEXT("bindings"));
	NewImpl->bRequiresCompile =
		NewImpl->BlueprintMutation->RequiresCompile() ||
		(ProviderExtension.IsValid() &&
			(ProviderExtension->HasField(TEXT("widget_tree")) ||
				ProviderExtension->HasField(TEXT("animations")) || NewImpl->bHasBindings));
	if (NewImpl->bRequiresCompile && !CompileBlueprint(ScratchBlueprint, OutError)) return false;
	if (!ApplyBindings(ScratchBlueprint, ProviderExtension, OutExpectedCounters, OutError))
		return false;
	if (NewImpl->bHasBindings && !CompileBlueprint(ScratchBlueprint, OutError)) return false;
	OutMutation = TUniquePtr<FPreparedUmgMutation>(new FPreparedUmgMutation(MoveTemp(NewImpl)));
	return true;
}

bool FPreparedUmgMutation::ApplyTo(
	UWidgetBlueprint& Blueprint, FMutationCounters& OutCounters, FString& OutError) const
{
	OutCounters = FMutationCounters();
	if (!Impl.IsValid() || !Impl->BlueprintMutation.IsValid())
	{
		SetError(OutError, TEXT("umg_prepared_mutation_missing"));
		return false;
	}
	if (!Impl->BlueprintMutation->ApplyTo(Blueprint, OutCounters.Blueprint, OutError)) return false;
	return ApplyDomainBeforeCompile(Blueprint, Impl->Extension, OutCounters, OutError);
}

bool FPreparedUmgMutation::FinalizeAfterCompile(UWidgetBlueprint& Blueprint,
	FMutationCounters& OutCounters,
	bool& bOutRequiresSecondCompile,
	FString& OutError) const
{
	OutCounters = FMutationCounters();
	bOutRequiresSecondCompile = false;
	if (!Impl.IsValid() || !Impl->BlueprintMutation.IsValid())
	{
		SetError(OutError, TEXT("umg_prepared_mutation_missing"));
		return false;
	}
	if (!Impl->BlueprintMutation->FinalizeAfterCompile(Blueprint, OutCounters.Blueprint, OutError))
		return false;
	if (!ApplyBindings(Blueprint, Impl->Extension, OutCounters, OutError)) return false;
	bOutRequiresSecondCompile = Impl->bHasBindings;
	return true;
}

bool FPreparedUmgMutation::RestoreFromBackup(
	const UWidgetBlueprint& Backup, UWidgetBlueprint& Target, FString& OutError)
{
	if (!UeAgentBlueprintAuthoring::FPreparedBlueprintMutation::RestoreFromBackup(
			Backup, Target, OutError))
	{
		return false;
	}
	if (Target.WidgetTree)
	{
		Target.WidgetTree->Rename(
			nullptr, GetTransientPackage(), REN_DontCreateRedirectors | REN_NonTransactional);
	}
	Target.WidgetTree = Backup.WidgetTree
							? DuplicateObject<UWidgetTree>(
								  Backup.WidgetTree, &Target, Backup.WidgetTree->GetFName())
							: nullptr;
	for (UWidgetAnimation* Animation : Target.Animations)
	{
		if (Animation)
		{
			Animation->Rename(
				nullptr, GetTransientPackage(), REN_DontCreateRedirectors | REN_NonTransactional);
		}
	}
	Target.Animations.Reset();
	for (UWidgetAnimation* Animation : Backup.Animations)
	{
		if (Animation)
		{
			Target.Animations.Add(
				DuplicateObject<UWidgetAnimation>(Animation, &Target, Animation->GetFName()));
		}
	}
	Target.Bindings = Backup.Bindings;
	Target.WidgetVariableNameToGuidMap = Backup.WidgetVariableNameToGuidMap;
	FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(&Target);
	return CompileBlueprint(Target, OutError);
}

bool FPreparedUmgMutation::RequiresCompile() const
{
	return Impl.IsValid() && Impl->bRequiresCompile;
}
} // namespace UeAgentUmgAuthoring
