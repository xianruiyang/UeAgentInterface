// Copyright Epic Games, Inc. All Rights Reserved.

#include "Domains/UMG/Authoring/UeAgentUmgMutationModel.h"
#include "Core/ContentDiscovery/UeAgentReadOnlyCandidateBatch.h"
#include "MovieScenePossessable.h"
#include "Tracks/MovieScenePropertyTrack.h"
#include "Utilities/AuthoringCore/UeAgentMovieSceneScalarChannel.h"
#include "Utilities/AuthoringCore/UeAgentMovieSceneAuthoring.h"
#include "Domains/UMG/Authoring/UeAgentUmgAnimationState.h"
#include "Domains/Blueprint/Authoring/UeAgentBlueprintAssetLifecycle.h"
#include "Utilities/AuthoringCore/UeAgentAuthoringDiagnostics.h"
#include "Utilities/AuthoringCore/UeAgentAuthoringJsonFields.h"
#include "Utilities/UeAgentPropertyValueCodec.h"
#include "UObject/UnrealType.h"
#include "WidgetBlueprintEditorUtils.h"

#include "Animation/MovieScene2DTransformSection.h"
#include "Animation/MovieScene2DTransformTrack.h"
#include "Animation/WidgetAnimation.h"
#include "Blueprint/WidgetTree.h"
#include "Channels/MovieSceneFloatChannel.h"
#include "Components/NamedSlotInterface.h"
#include "Components/PanelSlot.h"
#include "Components/PanelWidget.h"
#include "Components/Widget.h"
#include "Core/HjsonWorkspace/DomainAuthoring/UeAgentDomainHjsonAuthoringKit.h"
#include "Domains/UMG/Authoring/UeAgentUmgAuthoringUtilities.h"
#include "Domains/UMG/Authoring/UeAgentUmgAuthoringDescriptors.h"
#include "Domains/UMG/Authoring/UeAgentUmgAuthoringState.h"
#include "Dom/JsonObject.h"
#include "Kismet2/Kismet2NameValidators.h"
#include "Dom/JsonValue.h"
#include "Misc/ScopeExit.h"
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
#include "Utilities/AuthoringOperationApply/UeAgentApplyInputContract.h"
#include "UObject/StrongObjectPtr.h"
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

void SetError(UE::UAI::Contract::FDiagnostic& OutError, const FString& Code, const FString& Reason, const FString& Name = FString())
{
	OutError = UE::UAI::Contract::FDiagnostic::Failure(Code, Reason, TEXT("widget_authoring"));
	OutError.CandidateName = Name;
}

void LocateWidgetFailure(UWidgetBlueprint& Blueprint, const FMapEntry& Entry, UE::UAI::Contract::FDiagnostic& OutError)
{
	if (!OutError.IsEmpty() && Entry.Spec.IsValid())
		UeAgentAuthoringDiagnostics::LocateFailure(OutError, *Entry.Spec,
			TEXT("widget_tree_apply"), UeAgentBlueprintAssetLifecycle::GetAuthoringTargetPath(Blueprint));
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
	UE::UAI::Contract::FDiagnostic& OutError)
{
	OutEntries.Reset();
	bOutPresent = Object.IsValid() && Object->HasField(Field);
	if (!bOutPresent) return true;
	const TSharedPtr<FJsonObject> Map = GetObject(Object, Field);
	if (!Map.IsValid())
	{
		SetError(OutError, TEXT("umg_object_map_invalid"), TEXT("This field must be an object containing named entries."), Field);
		OutError.ActualValue = Object->TryGetField(Field);
		OutError.ExpectedValue = MakeShared<FJsonValueString>(TEXT("object"));
		UeAgentAuthoringDiagnostics::LocateFailure(OutError, *Object, TEXT("input_decode"), FString(), Field);
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
			SetError(OutError, TEXT("umg_object_map_item_invalid"), TEXT("Each named entry must be an object."), Key);
			OutError.ActualValue = Value;
			OutError.ExpectedValue = MakeShared<FJsonValueString>(TEXT("object"));
			UeAgentAuthoringDiagnostics::LocateFailure(OutError, *Map, TEXT("input_decode"), FString(), Key);
			return false;
		}
		if (!UeAgentAuthoringJsonFields::ValidateOptionalFields(**Spec,
			{{TEXT("operation"), EJson::String}}, OutError)) return false;
		FString OperationText;
		if ((*Spec)->TryGetStringField(TEXT("operation"), OperationText))
		{
			using UeAgentApplyInputContract::EOperation;
			EOperation Parsed;
			if (!UeAgentApplyInputContract::ParseOperation(OperationText, Parsed)
				|| (Parsed != EOperation::Create && Parsed != EOperation::Update
					&& Parsed != EOperation::Replace && Parsed != EOperation::Upsert
					&& Parsed != EOperation::Remove))
			{
				SetError(OutError, TEXT("umg_item_operation_invalid"), TEXT("The item operation must be create, update, replace, upsert or remove."), OperationText);
				OutError.CandidateName = Key;
				OutError.ActualValue = MakeShared<FJsonValueString>(OperationText);
				OutError.ExpectedValue = MakeShared<FJsonValueString>(TEXT("create|update|replace|upsert|remove"));
				UeAgentAuthoringDiagnostics::LocateFailure(OutError, **Spec,
					TEXT("input_decode"), FString(), TEXT("operation"));
				return false;
			}
		}
		OutEntries.Add({Key, *Spec});
	}
	return true;
}

FString Operation(const TSharedPtr<const FJsonObject>& Spec)
{
	FString Value = TEXT("upsert");
	TryGetString(Spec, TEXT("operation"), Value);
	UeAgentApplyInputContract::EOperation Parsed;
	return UeAgentApplyInputContract::ParseOperation(Value, Parsed)
		? UeAgentApplyInputContract::OperationToString(Parsed) : Value;
}

bool ValidateKnownFields(const TSharedPtr<const FJsonObject>& Object,
	const TSet<FString>& Allowed,
	const TCHAR* ErrorCode,
	UE::UAI::Contract::FDiagnostic& OutError)
{
	if (!Object.IsValid()) return true;
	for (const TPair<FString, TSharedPtr<FJsonValue>>& Field : Object->Values)
	{
		if (!Allowed.Contains(Field.Key))
		{
			SetError(OutError, ErrorCode, TEXT("This field is not supported in the widget authoring entry."), Field.Key);
			OutError.ActualValue = MakeShared<FJsonValueString>(Field.Key);
			TArray<FString> Names = Allowed.Array();
			Names.Sort();
			TArray<TSharedPtr<FJsonValue>> Expected;
			for (const FString& Name : Names) Expected.Add(MakeShared<FJsonValueString>(Name));
			OutError.ExpectedValue = MakeShared<FJsonValueArray>(Expected);
			UeAgentAuthoringDiagnostics::LocateFailure(OutError, *Object, TEXT("input_decode"), FString(), Field.Key);
			return false;
		}
	}
	return true;
}

bool ValidateAuthoredEntryFields(const FMapEntry& Entry, TSet<FString> Allowed,
	const TCHAR* ErrorCode, UE::UAI::Contract::FDiagnostic& OutError)
{
	// Workspace lowering attached provenance to this entry after public field validation.
	Allowed.Add(TEXT("authoring_source"));
	return ValidateKnownFields(Entry.Spec, Allowed, ErrorCode, OutError);
}

UClass* ResolveClass(const FString& ClassPath, UClass* RequiredBase)
{
	UClass* Class =
		ClassPath.IsEmpty() ? nullptr : StaticLoadClass(RequiredBase, nullptr, *ClassPath);
	return Class && Class->IsChildOf(RequiredBase) ? Class : nullptr;
}

bool CompileBlueprint(UWidgetBlueprint& Blueprint, UE::UAI::Contract::FDiagnostic& OutError)
{
	FCompilerResultsLog CompilerLog;
	CompilerLog.bLogInfoOnly = false;
	UeAgentUmgAuthoringUtilities::SyncWidgetVariableGuidMap(&Blueprint);
	FKismetEditorUtilities::CompileBlueprint(
		&Blueprint, EBlueprintCompileOptions::None, &CompilerLog);
	if (CompilerLog.NumErrors > 0 || Blueprint.Status == BS_Error)
	{
		OutError = UeAgentAuthoringDiagnostics::MakeCompileFailure(TEXT("umg_compile_failed"),
			UeAgentBlueprintAssetLifecycle::GetAuthoringTargetPath(Blueprint), CompilerLog, &Blueprint);
		return false;
	}
	return true;
}

void CollectSubtree(UWidget* Widget, TArray<UWidget*>& OutWidgets)
{
	if (!Widget) return;
	OutWidgets.Add(Widget);
	UWidgetTree::GetChildWidgets(Widget, OutWidgets);
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
				return UeAgentUmgAuthoringUtilities::FindWidgetByAnyName(
					&Blueprint, Pair.Key.ToString());
			}
		}
	}
	FString Name;
	TryGetString(Spec, TEXT("name"), Name);
	if (Name.IsEmpty() && Key.StartsWith(TEXT("widget:"))) Name = Key.Mid(7);
	return Name.IsEmpty()
		? nullptr
		: UeAgentUmgAuthoringUtilities::FindWidgetByAnyName(&Blueprint, Name);
}

bool RemoveWidget(
	UWidgetBlueprint& Blueprint, UWidget& Widget, FMutationCounters& OutCounters, UE::UAI::Contract::FDiagnostic& OutError,
	TSet<UWidget*>* OutRemovedWidgets = nullptr)
{
	TArray<UWidget*> Removed;
	CollectSubtree(&Widget, Removed);
	// DeleteWidgets synchronously skeleton-compiles when it detaches a widget. During
	// a batch, retained widgets may still be detached and that compiler trashes them.
	// Detach through native tree/slot APIs first, retaining DeleteWidgets' variable,
	// binding and focus cleanup. The complete tree is structurally marked below.
	Blueprint.Modify();
	Blueprint.WidgetTree->Modify();
	Widget.SetFlags(RF_Transactional);
	Widget.Modify();
	if (UPanelWidget* Parent = Widget.GetParent())
	{
		Parent->SetFlags(RF_Transactional);
		Parent->Modify();
	}
	Blueprint.WidgetTree->RemoveWidget(&Widget);
	if (TScriptInterface<INamedSlotInterface> Host =
		FWidgetBlueprintEditorUtils::FindNamedSlotHostForContent(&Widget, Blueprint.WidgetTree))
	{
		FWidgetBlueprintEditorUtils::RemoveNamedSlotHostContent(&Widget, Host);
	}
	FWidgetBlueprintEditorUtils::DeleteWidgets(&Blueprint, {&Widget},
		FWidgetBlueprintEditorUtils::EDeleteWidgetWarningType::DeleteSilently);
	if (Widget.GetOuter() == Blueprint.WidgetTree)
	{
		SetError(OutError, TEXT("umg_widget_remove_failed"), TEXT("The native widget removal left the widget in the tree."), Widget.GetName());
		return false;
	}
	for (UWidget* RemovedWidget : Removed)
	{
		if (!RemovedWidget) continue;
		if (OutRemovedWidgets) OutRemovedWidgets->Add(RemovedWidget);
		++OutCounters.WidgetsRemoved;
	}
	return true;
}

bool AttachWidget(UWidgetBlueprint& Blueprint,
	UWidget& Widget,
	const FString& ParentName,
	const int32 InsertIndex,
	const bool bParentSpecified,
	UE::UAI::Contract::FDiagnostic& OutError,
	UPanelSlot* PreservedSlotTemplate = nullptr)
{
	if (!bParentSpecified) return true;
	if (ParentName.IsEmpty())
	{
		if (Blueprint.WidgetTree->RootWidget && Blueprint.WidgetTree->RootWidget != &Widget)
		{
			SetError(OutError, TEXT("umg_multiple_root_widgets"), TEXT("The widget tree already has a different root widget."), Widget.GetName());
			return false;
		}
		if (Widget.Slot && Widget.Slot->Parent)
			Widget.Slot->Parent->RemoveChild(&Widget);
		Blueprint.WidgetTree->RootWidget = &Widget;
		return true;
	}

	UPanelWidget* Parent =
		Cast<UPanelWidget>(UeAgentUmgAuthoringUtilities::FindWidgetByAnyName(
			&Blueprint, ParentName));
	if (!Parent)
	{
		SetError(OutError, TEXT("umg_parent_not_found_or_not_panel"), TEXT("The requested parent is missing or cannot contain child widgets."), ParentName);
		return false;
	}
	if (Widget.GetParent() == Parent)
	{
		return true;
	}
	// Native slot templates preserve layout when the destination uses the same slot class.
	TStrongObjectPtr<UPanelSlot> SlotTemplate(Widget.Slot ? Widget.Slot.Get() : PreservedSlotTemplate);
	if (Blueprint.WidgetTree->RootWidget == &Widget)
		Blueprint.WidgetTree->RootWidget = nullptr;
	UPanelSlot* Slot =
		InsertIndex >= 0
			? Parent->InsertChildAt(InsertIndex, &Widget, SlotTemplate.Get())
			: Parent->AddChild(&Widget, SlotTemplate.Get());
	if (!Slot)
	{
		SetError(OutError, TEXT("umg_native_slot_create_failed"), TEXT("The panel did not create a slot for the attached widget."), Widget.GetName());
		return false;
	}
	// OnSlotAdded can overwrite a cloned layout with the destination panel's defaults.
	// Keep native ownership and cloned instanced objects; restore only editable layout.
	if (SlotTemplate.Get() && SlotTemplate->GetClass() == Slot->GetClass())
	{
		bool bRestoredLayout = false;
		for (TFieldIterator<FProperty> Property(Slot->GetClass()); Property; ++Property)
		{
			if (Property->HasAnyPropertyFlags(CPF_Edit)
				&& !Property->HasAnyPropertyFlags(CPF_Transient | CPF_InstancedReference | CPF_ContainsInstancedReference)
				&& !Property->Identical_InContainer(Slot, SlotTemplate.Get()))
			{
				Property->CopyCompleteValue_InContainer(Slot, SlotTemplate.Get());
				bRestoredLayout = true;
			}
		}
		if (bRestoredLayout) Slot->SynchronizeProperties();
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
	UE::UAI::Contract::FDiagnostic& OutError)
{
	using namespace UE::UAI::HjsonWorkspace;
	using namespace UE::UAI::HjsonWorkspace::DomainAuthoring;
	using namespace UE::UAI::QueryOwners::UmgWidget;

	OutCandidates.Reset();
	UE::UAI::ContentDiscovery::FReadOnlyCandidateBatch CandidateBatch;
	for (const FMapEntry& Entry : Entries)
	{
		ON_SCOPE_EXIT { LocateWidgetFailure(Blueprint, Entry, OutError); };
		const TSharedPtr<FJsonObject>* CandidateField = nullptr;
		if (!Entry.Spec->TryGetObjectField(TEXT("candidate"), CandidateField))
		{
			if (Entry.Spec->HasField(TEXT("candidate")))
			{
				SetError(OutError, TEXT("umg_widget_candidate_invalid"), TEXT("The widget candidate selector must be an object."), Entry.Key);
				return false;
			}
			continue;
		}
		if (!CandidateField || !CandidateField->IsValid())
		{
			SetError(OutError, TEXT("umg_widget_candidate_invalid"), TEXT("The widget candidate selector must be an object."), Entry.Key);
			return false;
		}

		FDomainHjsonCandidateSelector Selector;
		FWorkspaceError SelectorError;
		if (!TryReadCandidateSelector(
				*CandidateField,
				TEXT("/provider_extension/widget_tree/widgets/")
					+ UE::UAI::HjsonWorkspace::EscapeJsonPointerSegment(Entry.Key) + TEXT("/candidate"),
				Selector,
				SelectorError))
		{
			OutError = SelectorError.GetDiagnostic();
			return false;
		}
		FUmgWidgetCandidate Resolved;
		FUmgWidgetCandidateQueryError QueryError;
		if (!TryResolveUmgWidgetCandidate(
				Blueprint,
				Selector.Name,
				Resolved,
				QueryError,
				Selector.Select, &CandidateBatch))
		{
			OutError = QueryError.IsValid() ? QueryError : UE::UAI::Contract::FDiagnostic::Failure(FString(TEXT("umg_widget_candidate_selector_invalid")), TEXT("The widget candidate selector could not be resolved."), TEXT("native_graph"));
			if (OutError.SubjectId.IsEmpty()) OutError.SubjectId = Entry.Key;
			if (OutError.CandidateName.IsEmpty()) OutError.CandidateName = Selector.Name;
			UeAgentAuthoringDiagnostics::LocateFailure(OutError, **CandidateField,
				TEXT("widget_tree_apply"), UeAgentBlueprintAssetLifecycle::GetAuthoringTargetPath(Blueprint));
			return false;
		}

		FString ClassAssertion;
		if (TryGetString(Entry.Spec, TEXT("class"), ClassAssertion)
			&& !ClassAssertion.IsEmpty()
			&& ClassAssertion != Resolved.ClassPath)
		{
			SetError(
				OutError,
				TEXT("umg_widget_candidate_class_conflict"), TEXT("The resolved widget class does not match the authored class assertion."),
				Entry.Key);
			OutError.ExpectedValue = MakeShared<FJsonValueString>(ClassAssertion);
			OutError.ActualValue = MakeShared<FJsonValueString>(Resolved.ClassPath);
			UeAgentAuthoringDiagnostics::LocateFailure(OutError, *Entry.Spec, TEXT("widget_tree_apply"),
				UeAgentBlueprintAssetLifecycle::GetAuthoringTargetPath(Blueprint), TEXT("class"));
			return false;
		}
		OutCandidates.Add(Entry.Key, MoveTemp(Resolved));
	}
	return true;
}

bool ValidateGeneratedMemberName(UWidgetBlueprint& Blueprint, UClass& MemberClass,
	const FString& Name, const FName CurrentName, const FJsonObject& Spec,
	const bool bAnimation, UE::UAI::Contract::FDiagnostic& OutError)
{
	// VerifyWidgetRename is editor-bound. Use its native validator and binding
	// exception here without opening an editor for a prepared authoring copy.
	FString Error;
	if (Name.TrimStartAndEnd().IsEmpty() || Name.Len() >= NAME_SIZE)
	{
		Error = TEXT("A generated member name must be nonempty and shorter than NAME_SIZE.");
	}
	else
	{
		const auto* Property = Blueprint.ParentClass
			? CastField<FObjectPropertyBase>(Blueprint.ParentClass->FindPropertyByName(FName(*Name))) : nullptr;
		const bool bBinding = Property && (bAnimation
			? FWidgetBlueprintEditorUtils::IsBindWidgetAnimProperty(Property)
			: FWidgetBlueprintEditorUtils::IsBindWidgetProperty(Property));
		if (bBinding)
		{
			if (!MemberClass.IsChildOf(Property->PropertyClass))
				Error = FString::Printf(TEXT("%s must match inherited binding type %s."),
					*Name, *Property->PropertyClass->GetName());
		}
		else
		{
			FKismetNameValidator Validator(&Blueprint, CurrentName);
			const EValidatorResult Result = Validator.IsValid(Name);
			if (Result != EValidatorResult::Ok)
				Error = FKismetNameValidator::GetErrorString(Name, Result);
		}
	}
	if (Error.IsEmpty()) return true;
	UeAgentAuthoringDiagnostics::FDiagnostic Diagnostic;
	Diagnostic.Code = bAnimation ? TEXT("umg_animation_name_invalid") : TEXT("umg_widget_name_invalid");
	Diagnostic.Stage = TEXT("workspace_preflight");
	Diagnostic.TargetPath = UeAgentBlueprintAssetLifecycle::GetAuthoringTargetPath(Blueprint);
	Diagnostic.CandidateName = Name;
	Diagnostic.PublicMessage = Error;
	UeAgentAuthoringDiagnostics::LocateAuthoringSource(Diagnostic, Spec);
	OutError = Diagnostic;
	return false;
}

bool TryCreateWidgetFromTemplate(
	UWidgetBlueprint& Blueprint,
	UClass& WidgetClass,
	const FString& Name,
	UWidget*& OutWidget,
	UE::UAI::Contract::FDiagnostic& OutError)
{
	OutWidget = nullptr;
	if (!Blueprint.WidgetTree || Name.IsEmpty())
	{
		SetError(OutError, TEXT("umg_widget_native_create_failed"), !Blueprint.WidgetTree
			? TEXT("The widget Blueprint has no widget tree for creating the widget.")
			: TEXT("Creating a widget requires a nonempty widget name."), Name);
		OutError.ExpectedValue = MakeShared<FJsonValueString>(WidgetClass.GetPathName());
		return false;
	}
	FUeAgentNamedWidgetTemplateClass Template(&WidgetClass);
	OutWidget = Template.CreateNamedForAuthoring(
		Blueprint.WidgetTree, FName(*Name));
	if (!OutWidget)
	{
		SetError(OutError, TEXT("umg_widget_native_create_failed"), TEXT("The native widget factory did not create the requested widget."), Name);
		OutError.ExpectedValue = MakeShared<FJsonValueString>(WidgetClass.GetPathName());
		return false;
	}
	return true;
}

bool ApplyWidgetTree(UWidgetBlueprint& Blueprint,
	const TSharedPtr<const FJsonObject>& Extension,
	FMutationCounters& OutCounters,
	UE::UAI::Contract::FDiagnostic& OutError,
	UPackage* TextIdentityPackage,
	TArray<UeAgentDetailsAuthoringService::FScopedPropertyIntent>& PropertyIntents)
{
	if (!Extension.IsValid() || !Extension->HasField(TEXT("widget_tree"))) return true;
	const TSharedPtr<FJsonObject> Tree = GetObject(Extension, TEXT("widget_tree"));
	if (!Tree.IsValid())
	{
		SetError(OutError, TEXT("umg_widget_tree_invalid"), TEXT("The widget_tree field must be an object."));
		OutError.ActualValue = Extension->TryGetField(TEXT("widget_tree"));
		OutError.ExpectedValue = MakeShared<FJsonValueString>(TEXT("object"));
		UeAgentAuthoringDiagnostics::LocateFailure(OutError, *Extension, TEXT("widget_tree_apply"),
			UeAgentBlueprintAssetLifecycle::GetAuthoringTargetPath(Blueprint), TEXT("widget_tree"));
		return false;
	}
	if (!ValidateKnownFields(
			Tree, {TEXT("widgets")}, TEXT("umg_widget_tree_unknown_field"), OutError))
	{
		return false;
	}
	TArray<FMapEntry> Entries;
	bool bPresent = false;
	if (!GetObjectMap(Tree, TEXT("widgets"), Entries, bPresent, OutError)) return false;
	if (!bPresent) {
		SetError(OutError, TEXT("umg_widget_map_missing"), TEXT("The widget_tree object requires a widgets map."));
		OutError.ActualValuePresent = false;
		UeAgentAuthoringDiagnostics::LocateFailure(OutError, *Tree, TEXT("widget_tree_apply"),
			UeAgentBlueprintAssetLifecycle::GetAuthoringTargetPath(Blueprint), TEXT("widgets"));
		return false;
	}
	TMap<FString, UE::UAI::QueryOwners::UmgWidget::FUmgWidgetCandidate>
		ResolvedCandidates;
	TMap<FString, TSharedPtr<FJsonObject>> WidgetProperties, SlotProperties;
	TArray<FString> WidgetPropertyOrder, SlotPropertyOrder;
	TMap<const FJsonObject*, TSharedPtr<FJsonObject>> NormalizedProperties;
	TMap<FString, int32> WidgetIndices;
	auto CollectProperties = [&NormalizedProperties, &OutError, &Blueprint](const FString& Name, const TSharedPtr<FJsonObject>& Spec,
		TMap<FString, TSharedPtr<FJsonObject>>& Documents, TArray<FString>& Order, int32& Count)
	{
		const auto Document = NormalizedProperties.FindRef(Spec.Get());
		if (!Document) {
			if (!Spec || !Spec->HasField(TEXT("properties"))) return true;
			SetError(OutError, TEXT("umg_normalized_properties_missing"), TEXT("The authored properties have no prepared property document."), Name);
			UeAgentAuthoringDiagnostics::LocateFailure(OutError, *Spec, TEXT("widget_tree_apply"),
				UeAgentBlueprintAssetLifecycle::GetAuthoringTargetPath(Blueprint), TEXT("properties"));
			return false;
		}
		Documents.Add(Name, Document);
		Order.Add(Name);
		Count += Document->GetArrayField(TEXT("properties")).Num();
		return true;
	};

	for (const FMapEntry& Entry : Entries)
	{
		ON_SCOPE_EXIT { LocateWidgetFailure(Blueprint, Entry, OutError); };
		int32 Index = INDEX_NONE;
		if (!UeAgentAuthoringJsonFields::ValidateOptionalFields(*Entry.Spec,
			{{TEXT("name"), EJson::String}, {TEXT("class"), EJson::String},
			 {TEXT("parent"), EJson::String}, {TEXT("candidate"), EJson::Object},
			 {TEXT("make_variable"), EJson::Boolean}, {TEXT("slot"), EJson::Object}}, OutError)
			|| !UeAgentAuthoringJsonFields::ReadInt32Field(*Entry.Spec, TEXT("index"), Index, OutError)) return false;
		WidgetIndices.Add(Entry.Key, Index);
		TSharedPtr<FJsonObject> PropertyDocument;
		if (!UeAgentDetailsAuthoringService::ReadPropertyPayload(Entry.Spec, TEXT("properties"), PropertyDocument, OutError)) return false;
		NormalizedProperties.Add(Entry.Spec.Get(), PropertyDocument);
		const auto Slot = GetObject(Entry.Spec, TEXT("slot"));
		if (Slot && !UeAgentDetailsAuthoringService::ReadPropertyPayload(Slot, TEXT("properties"), PropertyDocument, OutError)) return false;
		if (Slot) NormalizedProperties.Add(Slot.Get(), PropertyDocument);
		if (!ValidateAuthoredEntryFields(Entry,
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
	UeAgentUmgAuthoringUtilities::SyncWidgetVariableGuidMap(&Blueprint);
	auto DesiredClassPath = [&](const FMapEntry& Entry) {
		if (const auto* Candidate = ResolvedCandidates.Find(Entry.Key)) return Candidate->ClassPath;
		FString ClassPath;
		TryGetString(Entry.Spec, TEXT("class"), ClassPath);
		return ClassPath;
	};
	TMap<FString, UWidget*> ExistingWidgets;
	TSet<UWidget*> RemovalTargets;
	TSet<UWidget*> ReplacementTargets;
	TMap<UWidget*, FName> RetainedWidgetNames;
	TArray<UWidget*> OriginalWidgets;
	Blueprint.WidgetTree->GetAllWidgets(OriginalWidgets);
	TArray<TStrongObjectPtr<UWidget>> PreservedWidgets;
	for (UWidget* Widget : OriginalWidgets)
	{
		RetainedWidgetNames.Add(Widget, Widget->GetFName());
		PreservedWidgets.Emplace(Widget);
	}

	for (const FMapEntry& Entry : Entries)
	{
		ON_SCOPE_EXIT { LocateWidgetFailure(Blueprint, Entry, OutError); };
		UWidget* Existing = FindWidgetByGuidOrName(Blueprint, Entry.Key, Entry.Spec);
		ExistingWidgets.Add(Entry.Key, Existing);
		const FString Op = Operation(Entry.Spec);
		if (Op == TEXT("remove"))
		{
			if (!Existing)
			{
				SetError(OutError, TEXT("umg_widget_remove_target_not_found"), TEXT("The widget requested for removal does not exist."), Entry.Key);
				return false;
			}
			RemovalTargets.Add(Existing);
		}
		else if (Existing && Op == TEXT("replace"))
		{
			const FString ClassPath = DesiredClassPath(Entry);
			if (!ClassPath.IsEmpty() && Existing->GetClass()->GetPathName() != ClassPath)
				ReplacementTargets.Add(Existing);
		}
	}

	// Release changed relationships before creating final slots. A valid wrapper insertion
	// or exchange between single-child panels must not depend on widget-name order.
	TMap<UWidget*, TStrongObjectPtr<UPanelSlot>> PreservedSlots;
	for (const FMapEntry& Entry : Entries)
	{
		ON_SCOPE_EXIT { LocateWidgetFailure(Blueprint, Entry, OutError); };
		if (Operation(Entry.Spec) == TEXT("remove")) continue;
		UWidget* Widget = ExistingWidgets.FindRef(Entry.Key);
		if (!Widget || ReplacementTargets.Contains(Widget)) continue;
		UPanelWidget* Parent = Widget->GetParent();
		FString ParentName;
		const bool bParentSpecified = TryGetString(Entry.Spec, TEXT("parent"), ParentName);
		bool bDetach = bParentSpecified && (ParentName.IsEmpty()
			? Parent != nullptr
			: Parent != UeAgentUmgAuthoringUtilities::FindWidgetByAnyName(&Blueprint, ParentName)
				|| Blueprint.WidgetTree->RootWidget == Widget);
		for (UPanelWidget* Ancestor = Parent; !bDetach && Ancestor; Ancestor = Ancestor->GetParent())
		{
			bDetach = RemovalTargets.Contains(Ancestor) || ReplacementTargets.Contains(Ancestor);
		}
		if (!bDetach) continue;
		if (Widget->Slot) PreservedSlots.Add(Widget, TStrongObjectPtr<UPanelSlot>(Widget->Slot));
		Widget->Modify();
		if (Blueprint.WidgetTree->RootWidget == Widget)
		{
			Blueprint.WidgetTree->Modify();
			Blueprint.WidgetTree->RootWidget = nullptr;
		}
		else if (Parent)
		{
			Parent->Modify();
			if (!Parent->RemoveChild(Widget))
			{
				SetError(OutError, TEXT("umg_widget_detach_failed"), TEXT("The native widget could not be detached from its parent."), Entry.Key);
				return false;
			}
		}
	}

	// Coalesce parent/child removals after retained subtrees have been detached.
	TArray<UWidget*> RemovalRoots;
	for (const FMapEntry& Entry : Entries)
	{
		if (Operation(Entry.Spec) != TEXT("remove")) continue;
		UWidget* Widget = ExistingWidgets.FindRef(Entry.Key);
		bool bCoveredByAncestor = false;
		for (UPanelWidget* Parent = Widget->GetParent(); Parent; Parent = Parent->GetParent())
			if (RemovalTargets.Contains(Parent)) { bCoveredByAncestor = true; break; }
		if (!bCoveredByAncestor) RemovalRoots.AddUnique(Widget);
	}
	TSet<UWidget*> RemovedWidgets;
	for (UWidget* Widget : RemovalRoots)
		if (!RemoveWidget(Blueprint, *Widget, OutCounters, OutError, &RemovedWidgets))
		{
			if (const FMapEntry* Entry = Entries.FindByPredicate([&](const FMapEntry& Item)
				{ return ExistingWidgets.FindRef(Item.Key) == Widget; }))
				LocateWidgetFailure(Blueprint, *Entry, OutError);
			return false;
		}

	TSet<FString> Pending;
	struct FWidgetOrderIntent
	{
		int32 Index;
		UWidget* Widget;
		const FMapEntry* Source;
	};
	TArray<FWidgetOrderIntent> DesiredWidgetOrder;
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
			ON_SCOPE_EXIT { LocateWidgetFailure(Blueprint, Entry, OutError); };
			FString ParentName;
			const bool bParentSpecified = TryGetString(Entry.Spec, TEXT("parent"), ParentName);
			if (bParentSpecified && !ParentName.IsEmpty())
			{
				UWidget* Parent = UeAgentUmgAuthoringUtilities::FindWidgetByAnyName(&Blueprint, ParentName);
				if (!Parent || ReplacementTargets.Contains(Parent)) continue;
			}

			const FString Op = Operation(Entry.Spec);
			UWidget* Widget = ExistingWidgets.FindRef(Entry.Key);
			if (!Widget || RemovedWidgets.Contains(Widget))
				Widget = FindWidgetByGuidOrName(Blueprint, Entry.Key, Entry.Spec);
			TStrongObjectPtr<UPanelSlot> SlotTemplate(Widget ? Widget->Slot : nullptr);
			if (Widget && !SlotTemplate.Get())
				if (const auto* Preserved = PreservedSlots.Find(Widget)) SlotTemplate = *Preserved;
			if (Op == TEXT("create") && Widget)
			{
				SetError(OutError, TEXT("umg_widget_already_exists"), TEXT("The create operation targets a widget that already exists."), Entry.Key);
				return false;
			}
			if (Op == TEXT("update") && !Widget)
			{
				SetError(OutError, TEXT("umg_widget_not_found"), TEXT("The update operation targets a widget that does not exist."), Entry.Key);
				return false;
			}

			const FString ClassPath = DesiredClassPath(Entry);
			const FName PreviousName = Widget ? Widget->GetFName() : NAME_None;
			if (Widget && !ClassPath.IsEmpty() && Widget->GetClass()->GetPathName() != ClassPath)
			{
				if (Op != TEXT("replace"))
				{
					SetError(OutError, TEXT("umg_widget_class_conflict"), TEXT("Changing an existing widget class requires the replace operation."), Entry.Key);
					OutError.ExpectedValue = MakeShared<FJsonValueString>(ClassPath);
					OutError.ActualValue = MakeShared<FJsonValueString>(Widget->GetClass()->GetPathName());
					UeAgentAuthoringDiagnostics::LocateFailure(OutError, *Entry.Spec, TEXT("widget_tree_apply"),
						UeAgentBlueprintAssetLifecycle::GetAuthoringTargetPath(Blueprint), TEXT("class"));
					return false;
				}
				if (!RemoveWidget(Blueprint, *Widget, OutCounters, OutError, &RemovedWidgets)) return false;
				Widget = nullptr;
			}

			if (!Widget)
			{
				UClass* WidgetClass = ResolveClass(ClassPath, UWidget::StaticClass());
				if (!WidgetClass)
				{
					SetError(OutError, TEXT("umg_widget_class_invalid"), TEXT("The requested class could not be loaded as a Widget subclass."), ClassPath);
					OutError.CandidateName = Entry.Key;
					OutError.ActualValue = MakeShared<FJsonValueString>(ClassPath);
					OutError.ExpectedValue = MakeShared<FJsonValueString>(UWidget::StaticClass()->GetPathName());
					UeAgentAuthoringDiagnostics::LocateFailure(OutError, *Entry.Spec, TEXT("widget_tree_apply"),
						UeAgentBlueprintAssetLifecycle::GetAuthoringTargetPath(Blueprint), TEXT("class"));
					return false;
				}
				FString Name;
				TryGetString(Entry.Spec, TEXT("name"), Name);
				if (Name.IsEmpty() && Entry.Key.StartsWith(TEXT("widget:")))
					Name = Entry.Key.Mid(7);
				if (Name.IsEmpty())
				{
					SetError(OutError, TEXT("umg_widget_name_missing"), TEXT("Creating a widget requires a non-empty name."), Entry.Key);
					return false;
				}
				if (!ValidateGeneratedMemberName(Blueprint, *WidgetClass, Name,
					PreviousName, *Entry.Spec, false, OutError)) return false;
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
					UeAgentUmgAuthoringUtilities::MakeStableWidgetVariableGuid(
						Widget->GetName()));
				++OutCounters.WidgetsAdded;
			}
			else
			{
				FString DesiredName;
				if (TryGetString(Entry.Spec, TEXT("name"), DesiredName) && !DesiredName.IsEmpty() &&
					Widget->GetName() != DesiredName)
				{
					if (UeAgentUmgAuthoringUtilities::FindWidgetByAnyName(
						&Blueprint, DesiredName))
					{
						SetError(OutError, TEXT("umg_widget_name_conflict"), TEXT("Another object already uses the requested widget name."), DesiredName);
						return false;
					}
					if (!ValidateGeneratedMemberName(Blueprint, *Widget->GetClass(), DesiredName,
						Widget->GetFName(), *Entry.Spec, false, OutError)) return false;
					const FName OldName = Widget->GetFName();
					if (!Widget->Rename(
							*DesiredName, Blueprint.WidgetTree, REN_DontCreateRedirectors))
					{
						SetError(OutError, TEXT("umg_widget_rename_failed"), TEXT("The native widget rename did not produce the requested name."), DesiredName);
						return false;
					}
					Blueprint.OnVariableRenamed(OldName, Widget->GetFName());
					RetainedWidgetNames.FindOrAdd(Widget) = Widget->GetFName();
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

			const int32 Index = WidgetIndices.FindChecked(Entry.Key);
			if (!AttachWidget(Blueprint,
					*Widget,
					ParentName,
					Index,
					bParentSpecified || !Widget->Slot,
					OutError,
					SlotTemplate.Get()))
			{
				return false;
			}

			if (Index >= 0 && Widget->GetParent())
				DesiredWidgetOrder.Add({Index, Widget, &Entry});

			if (!CollectProperties(Widget->GetName(), Entry.Spec, WidgetProperties,
				WidgetPropertyOrder, OutCounters.WidgetPropertiesApplied)) return false;
			const TSharedPtr<FJsonObject> SlotSpec = GetObject(Entry.Spec, TEXT("slot"));
			if (Entry.Spec->HasField(TEXT("slot")) && !SlotSpec.IsValid())
			{
				SetError(OutError, TEXT("umg_slot_spec_invalid"), TEXT("The widget slot specification must be an object."), Entry.Key);
				return false;
			}
			if (SlotSpec.IsValid())
			{
				if (!Widget->Slot)
				{
					SetError(OutError, TEXT("umg_root_widget_has_no_slot"), TEXT("A root widget has no parent slot to configure."), Widget->GetName());
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
						TEXT("umg_native_slot_class_conflict"), TEXT("The native parent slot class does not match the authored slot class."),
						Widget->GetName());
					OutError.ExpectedValue = MakeShared<FJsonValueString>(SlotClass);
					OutError.ActualValue = MakeShared<FJsonValueString>(Widget->Slot->GetClass()->GetPathName());
					UeAgentAuthoringDiagnostics::LocateFailure(OutError, *SlotSpec, TEXT("widget_tree_apply"),
						UeAgentBlueprintAssetLifecycle::GetAuthoringTargetPath(Blueprint), TEXT("class"));
					return false;
				}
				if (!CollectProperties(Widget->GetName(), SlotSpec, SlotProperties,
					SlotPropertyOrder, OutCounters.SlotPropertiesApplied)) return false;
			}
			Pending.Remove(Entry.Key);
			bProgress = true;
		}
		if (!bProgress)
		{
			for (const FMapEntry& Entry : Entries) {
				if (!Pending.Contains(Entry.Key)) continue;
				FString ParentName;
				TryGetString(Entry.Spec, TEXT("parent"), ParentName);
				auto Failure = UE::UAI::Contract::FDiagnostic::Failure(TEXT("umg_widget_parent_cycle_or_missing"),
					TEXT("The widget cannot be attached because its parent is missing or belongs to an unresolved parent dependency cycle."), TEXT("widget_tree_apply"));
				Failure.CandidateName = Entry.Key;
				Failure.ActualValue = MakeShared<FJsonValueString>(ParentName);
				UeAgentAuthoringDiagnostics::LocateFailure(Failure, *Entry.Spec, TEXT("widget_tree_apply"),
					UeAgentBlueprintAssetLifecycle::GetAuthoringTargetPath(Blueprint), TEXT("parent"));
				OutError.Append(Failure);
			}
			return false;
		}
	}
	// Apply final sibling positions after all attachments; intermediate shifts can otherwise
	// invalidate positions already processed in a different widget-name order.
	DesiredWidgetOrder.StableSort([](const auto& Left, const auto& Right)
	{
		return Left.Index < Right.Index;
	});
	for (const auto& Desired : DesiredWidgetOrder)
	{
		UPanelWidget* Parent = Desired.Widget->GetParent();
		if (Parent && Parent->GetChildIndex(Desired.Widget) != Desired.Index)
			Parent->ShiftChild(Desired.Index, Desired.Widget);
	}
	for (const auto& Desired : DesiredWidgetOrder)
	{
		const UPanelWidget* Parent = Desired.Widget->GetParent();
		// Native ShiftChild clamps to the final panel bounds. Compare each original
		// request after the whole batch; a later sibling move can undo an earlier one.
		const int32 Expected = Parent ? FMath::Clamp(Desired.Index, 0, Parent->GetChildrenCount() - 1) : INDEX_NONE;
		if (!Parent || Parent->GetChildIndex(Desired.Widget) != Expected)
		{
			SetError(OutError, TEXT("umg_widget_order_intent_mismatch"), TEXT("The final native child index differs from the requested index after panel bounds are applied."), Desired.Widget->GetName());
			OutError.ExpectedValue = MakeShared<FJsonValueNumber>(Expected);
			OutError.ActualValuePresent = Parent != nullptr;
			if (Parent) OutError.ActualValue = MakeShared<FJsonValueNumber>(Parent->GetChildIndex(Desired.Widget));
			UeAgentAuthoringDiagnostics::LocateFailure(OutError, *Desired.Source->Spec,
				TEXT("widget_tree_apply"), UeAgentBlueprintAssetLifecycle::GetAuthoringTargetPath(Blueprint), TEXT("index"));
			return false;
		}
	}
	UeAgentUmgAuthoringUtilities::SyncWidgetVariableGuidMap(&Blueprint);
	if (!WidgetProperties.IsEmpty() || !SlotProperties.IsEmpty())
	{
		TArray<UeAgentAuthoringState::FReadRequest> Requests;
		if (!BuildAssetExportSnapshotReadRequests(Blueprint, Requests, OutError)) return false;
		auto OptionsForTarget = [](const UeAgentAuthoringState::FReadRequest&)
		{
			UeAgentAuthoringObjectApply::FOptions Options;
			Options.bAllowTemplateOwners = true;
			Options.bUseInstanceEditability = true;
			Options.bAllowDomainAuthorizedOwners = true;
			Options.bCompactValues = true;
			Options.bEmitEditorNotifications = false;
			Options.bMarkPackageDirty = false;
			return Options;
		};
		if (!UeAgentDetailsAuthoringService::ApplyScopedProperties(WidgetProperties, Requests,
			WidgetPropertySemanticId, TEXT("widget_name"), TEXT("umg_widget"), OptionsForTarget,
			OutError, TextIdentityPackage, WidgetPropertyOrder, &PropertyIntents)
			|| !UeAgentDetailsAuthoringService::ApplyScopedProperties(SlotProperties, Requests,
				SlotPropertySemanticId, TEXT("widget_name"), TEXT("umg_slot"), OptionsForTarget,
				OutError, TextIdentityPackage, SlotPropertyOrder, &PropertyIntents)) return false;
	}
	auto VerifyRetainedWidgets = [&]()
	{
		TArray<UWidget*> CurrentWidgets;
		Blueprint.WidgetTree->GetAllWidgets(CurrentWidgets);
		for (const auto& Expected : RetainedWidgetNames)
		{
			UWidget* Widget = Expected.Key;
			if (RemovedWidgets.Contains(Widget)) continue;
			const bool bReachable = CurrentWidgets.Contains(Widget);
			if (bReachable && Widget->GetOuter() == Blueprint.WidgetTree
				&& Widget->GetFName() == Expected.Value) continue;
			SetError(OutError, TEXT("umg_retained_widget_lost"),
				TEXT("The native structural edit lost or changed the identity of a widget that must be retained."),
				Expected.Value.ToString());
			auto ExpectedIdentity = MakeShared<FJsonObject>();
			ExpectedIdentity->SetStringField(TEXT("name"), Expected.Value.ToString());
			ExpectedIdentity->SetStringField(TEXT("outer"), Blueprint.WidgetTree->GetPathName());
			ExpectedIdentity->SetBoolField(TEXT("reachable"), true);
			auto ActualIdentity = MakeShared<FJsonObject>();
			ActualIdentity->SetStringField(TEXT("name"), Widget->GetName());
			ActualIdentity->SetStringField(TEXT("outer"), GetPathNameSafe(Widget->GetOuter()));
			ActualIdentity->SetBoolField(TEXT("reachable"), bReachable);
			OutError.ExpectedValue = MakeShared<FJsonValueObject>(ExpectedIdentity);
			OutError.ActualValue = MakeShared<FJsonValueObject>(ActualIdentity);
			if (const FMapEntry* Entry = Entries.FindByPredicate([&](const FMapEntry& Item)
				{ return ExistingWidgets.FindRef(Item.Key) == Widget; }))
				LocateWidgetFailure(Blueprint, *Entry, OutError);
			else
				UeAgentAuthoringDiagnostics::LocateFailure(OutError, *Tree, TEXT("widget_tree_apply"),
					UeAgentBlueprintAssetLifecycle::GetAuthoringTargetPath(Blueprint), TEXT("widgets"));
			return false;
		}
		return true;
	};
	if (!VerifyRetainedWidgets()) return false;
	FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(&Blueprint);
	return VerifyRetainedWidgets();
}

bool ApplyBindings(UWidgetBlueprint& Blueprint,
	const TSharedPtr<const FJsonObject>& Extension,
	FMutationCounters& OutCounters,
	UE::UAI::Contract::FDiagnostic& OutError,
	const bool bPreparedReplay = false)
{
	TArray<FMapEntry> Entries;
	bool bPresent = false;
	if (!GetObjectMap(Extension, TEXT("bindings"), Entries, bPresent, OutError) || !bPresent)
	{
		return !bPresent || OutError.IsEmpty();
	}
	for (const FMapEntry& Entry : Entries)
	{
		ON_SCOPE_EXIT { if (!OutError.IsEmpty()) UeAgentAuthoringDiagnostics::LocateFailure(OutError, *Entry.Spec,
			TEXT("binding_apply"), UeAgentBlueprintAssetLifecycle::GetAuthoringTargetPath(Blueprint)); };
		if (!ValidateAuthoredEntryFields(Entry,
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
		if (!UeAgentAuthoringJsonFields::ValidateOptionalFields(*Entry.Spec,
			{{TEXT("widget_name"), EJson::String}, {TEXT("property_name"), EJson::String},
			 {TEXT("binding_kind"), EJson::String}, {TEXT("source_variable_name"), EJson::String},
			 {TEXT("function_name"), EJson::String}}, OutError)) return false;
		TryGetString(Entry.Spec, TEXT("widget_name"), WidgetName);
		TryGetString(Entry.Spec, TEXT("property_name"), PropertyName);
		if (WidgetName.IsEmpty() || PropertyName.IsEmpty())
		{
			SetError(OutError, TEXT("umg_binding_identity_missing"), TEXT("A binding requires widget_name and property_name."), Entry.Key);
			UeAgentAuthoringDiagnostics::LocateFailure(OutError, *Entry.Spec, TEXT("binding_apply"),
				UeAgentBlueprintAssetLifecycle::GetAuthoringTargetPath(Blueprint), WidgetName.IsEmpty() ? TEXT("widget_name") : TEXT("property_name"));
			return false;
		}
		const int32 ExistingIndex = Blueprint.Bindings.IndexOfByPredicate(
			[&WidgetName, &PropertyName](const FDelegateEditorBinding& Binding)
			{
				return Binding.ObjectName == WidgetName &&
					   Binding.PropertyName.ToString() == PropertyName;
			});
		const FString Op = Operation(Entry.Spec);
		if (!bPreparedReplay && Op == TEXT("create") && ExistingIndex != INDEX_NONE)
		{ SetError(OutError, TEXT("umg_binding_create_target_exists"), TEXT("The binding requested for creation already exists."), Entry.Key); return false; }
		if (!bPreparedReplay && Op == TEXT("update") && ExistingIndex == INDEX_NONE)
		{ SetError(OutError, TEXT("umg_binding_update_target_not_found"), TEXT("The binding requested for update does not exist."), Entry.Key); return false; }
		if (Op == TEXT("remove"))
		{
			if (ExistingIndex == INDEX_NONE)
			{
				// Published new assets already contain the prepared binding state.
				if (bPreparedReplay) continue;
				SetError(OutError, TEXT("umg_binding_remove_target_not_found"), TEXT("The binding requested for removal does not exist."), Entry.Key);
				return false;
			}
			Blueprint.Bindings.RemoveAt(ExistingIndex);
			++OutCounters.BindingsRemoved;
			continue;
		}
		if (!UeAgentUmgAuthoringUtilities::FindWidgetByAnyName(
			&Blueprint, WidgetName))
		{
			SetError(OutError, TEXT("umg_binding_widget_not_found"), TEXT("The binding target widget does not exist."), WidgetName);
			return false;
		}
		FDelegateEditorBinding Binding;
		Binding.ObjectName = WidgetName;
		Binding.PropertyName = FName(*PropertyName);
		FString Kind;
		TryGetString(Entry.Spec, TEXT("binding_kind"), Kind);
		if (!Kind.IsEmpty() && !Kind.Equals(TEXT("property_variable"), ESearchCase::IgnoreCase)
			&& !Kind.Equals(TEXT("function"), ESearchCase::IgnoreCase))
		{
			SetError(OutError, TEXT("umg_binding_kind_invalid"), TEXT("The binding kind must be property_variable or function."), Kind);
			OutError.CandidateName = Entry.Key;
			OutError.ActualValue = MakeShared<FJsonValueString>(Kind);
			OutError.ExpectedValue = MakeShared<FJsonValueString>(TEXT("property_variable|function"));
			UeAgentAuthoringDiagnostics::LocateFailure(OutError, *Entry.Spec,
				TEXT("input_decode"), FString(), TEXT("binding_kind"));
			return false;
		}
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
				SetError(OutError, TEXT("umg_binding_source_variable_not_found"), TEXT("The binding source variable was not found in the generated Blueprint class."), SourceVariable);
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
				SetError(OutError, TEXT("umg_binding_function_missing"), TEXT("A function binding requires a non-empty function_name."), Entry.Key);
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

bool TryKeyFrame(UMovieScene& MovieScene, const TSharedPtr<const FJsonObject>& Key,
	FFrameNumber& OutFrame, UE::UAI::Contract::FDiagnostic& OutError)
{
	if (!UeAgentAuthoringJsonFields::ValidateOptionalFields(*Key,
		{{TEXT("time_seconds"), EJson::Number}, {TEXT("frame"), EJson::Number}}, OutError)) return false;
	int32 Frame = 0;
	if (Key->HasField(TEXT("time_seconds")))
	{
		const FFrameRate Rate = MovieScene.GetTickResolution();
		if (!Rate.IsValid() || !UeAgentPropertyValueCodec::TryConvertInt32(
			Key->GetNumberField(TEXT("time_seconds")) * Rate.Numerator / Rate.Denominator, Frame, OutError, true))
		{ UeAgentAuthoringDiagnostics::PrefixDocumentFailure(OutError, TEXT("/time_seconds")); return false; }
	}
	else if (!UeAgentAuthoringJsonFields::ReadInt32Field(*Key, TEXT("frame"), Frame, OutError)) return false;
	OutFrame = FFrameNumber(Frame);
	return true;
}

bool AddTrackKeys(UWidgetBlueprint& Blueprint,
	UWidgetAnimation& Animation,
	const TSharedPtr<const FJsonObject>& Track,
	FMutationCounters& OutCounters,
	UE::UAI::Contract::FDiagnostic& OutError)
{
	ON_SCOPE_EXIT { if (!OutError.IsEmpty() && Track.IsValid()) UeAgentAuthoringDiagnostics::LocateFailure(OutError, *Track,
		TEXT("animation_apply"), UeAgentBlueprintAssetLifecycle::GetAuthoringTargetPath(Blueprint)); };
	FString WidgetName;
	FString Kind;
	TryGetString(Track, TEXT("widget_name"), WidgetName);
	TryGetString(Track, TEXT("track_kind"), Kind);
	UWidget* Widget = UeAgentUmgAuthoringUtilities::FindWidgetByAnyName(
		&Blueprint, WidgetName);
	if (!Widget || Kind.IsEmpty())
	{
		SetError(OutError, TEXT("umg_animation_track_invalid"), !Widget
			? TEXT("The animation track widget_name does not resolve to an existing widget.")
			: TEXT("The animation track requires a nonempty track_kind."), Animation.GetName());
		const TCHAR* Field = !Widget ? TEXT("widget_name") : TEXT("track_kind");
		OutError.ActualValue = Track->TryGetField(Field);
		OutError.ActualValuePresent = Track->HasField(Field);
		UeAgentAuthoringDiagnostics::LocateFailure(OutError, *Track, TEXT("animation_apply"),
			UeAgentBlueprintAssetLifecycle::GetAuthoringTargetPath(Blueprint), Field);
		return false;
	}
	const TArray<TSharedPtr<FJsonValue>>* Keys = nullptr;
	if (!Track->TryGetArrayField(TEXT("keys"), Keys) || !Keys)
	{
		SetError(OutError, TEXT("umg_animation_keys_invalid"), TEXT("The animation track requires a keys array."), WidgetName);
		OutError.ActualValue = Track->TryGetField(TEXT("keys"));
		OutError.ActualValuePresent = Track->HasField(TEXT("keys"));
		OutError.ExpectedValue = MakeShared<FJsonValueString>(TEXT("array"));
		UeAgentAuthoringDiagnostics::LocateFailure(OutError, *Track, TEXT("animation_apply"),
			UeAgentBlueprintAssetLifecycle::GetAuthoringTargetPath(Blueprint), TEXT("keys"));
		return false;
	}
	UMovieScene* MovieScene = nullptr;
	FGuid BindingGuid;
	bool bBindingExists = false;
	if (!UeAgentUmgAuthoringUtilities::FindOrCreateWidgetAnimationBinding(
			&Blueprint, &Animation, Widget, MovieScene, BindingGuid, bBindingExists, OutError))
	{
		return false;
	}

	if (Kind == TEXT("transform"))
	{
		UMovieScene2DTransformTrack* ResultTrack = nullptr;
		UMovieScene2DTransformSection* Section = nullptr;
		bool bTrackExists = false;
		if (!UeAgentUmgAuthoringUtilities::FindOrCreateUmgTransformSection(
				MovieScene, BindingGuid, ResultTrack, Section, bTrackExists, OutError))
			return false;
		EMovieScene2DTransformChannel Mask = Section->GetMask().GetChannels();
		for (int32 KeyIndex = 0; KeyIndex < Keys->Num(); ++KeyIndex)
		{
			const TSharedPtr<FJsonValue>& KeyValue = (*Keys)[KeyIndex];
			ON_SCOPE_EXIT { if (!OutError.IsEmpty()) UeAgentAuthoringDiagnostics::PrefixDocumentFailure(OutError, TEXT("/keys/") + FString::FromInt(KeyIndex)); };
			const TSharedPtr<FJsonObject>* KeyObject = nullptr;
			if (!KeyValue.IsValid() || !KeyValue->TryGetObject(KeyObject) || !KeyObject ||
				!KeyObject->IsValid())
			{
				SetError(OutError, TEXT("umg_animation_key_invalid"), TEXT("Each animation key must be an object."), WidgetName);
				OutError.ActualValue = KeyValue;
				OutError.ExpectedValue = MakeShared<FJsonValueString>(TEXT("object"));
				return false;
			}
			FFrameNumber Frame;
			if (!TryKeyFrame(*MovieScene, *KeyObject, Frame, OutError)) return false;
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
				float Value = 0.0f;
				if ((*KeyObject)->HasField(Fields[ChannelIndex]))
				{
					if (!UeAgentAuthoringJsonFields::ReadFloat32Field(**KeyObject, Fields[ChannelIndex], Value, OutError)) return false;
					if (!UE::UAI::Authoring::Timeline::ScalarChannel::TryUpsertValue(*Channels[ChannelIndex], *Section,
						Frame, FMovieSceneFloatValue(Value), &OutError)) return false;
					Mask = Mask | Masks[ChannelIndex];
				}
			}
			++OutCounters.AnimationKeysApplied;
		}
		Section->SetMask(FMovieScene2DTransformMask(Mask));
		if (!UE::UAI::Authoring::Timeline::TrySetMovieSceneSectionRange(*Section, TRange<FFrameNumber>::All(), OutError)) return false;
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
				? UeAgentUmgAuthoringUtilities::FindOrCreateUmgOpacitySection(
					  MovieScene, BindingGuid, ResultTrack, Section, bTrackExists, OutError)
				: UeAgentUmgAuthoringUtilities::FindOrCreateUmgFloatSection(MovieScene,
					  BindingGuid,
					  FName(*PropertyName),
					  PropertyPath,
					  ResultTrack,
					  Section,
					  bTrackExists,
					  OutError);
		if (!bCreated) return false;
		for (int32 KeyIndex = 0; KeyIndex < Keys->Num(); ++KeyIndex)
		{
			const TSharedPtr<FJsonValue>& KeyValue = (*Keys)[KeyIndex];
			ON_SCOPE_EXIT { if (!OutError.IsEmpty()) UeAgentAuthoringDiagnostics::PrefixDocumentFailure(OutError, TEXT("/keys/") + FString::FromInt(KeyIndex)); };
			const TSharedPtr<FJsonObject>* KeyObject = nullptr;
			if (!KeyValue.IsValid() || !KeyValue->TryGetObject(KeyObject) || !KeyObject ||
				!KeyObject->IsValid())
			{
				SetError(OutError, TEXT("umg_animation_key_invalid"), TEXT("Each animation key must be an object."), WidgetName);
				OutError.ActualValue = KeyValue;
				OutError.ExpectedValue = MakeShared<FJsonValueString>(TEXT("object"));
				return false;
			}
			float Value = 0.0f;
			const TCHAR* Field = (*KeyObject)->HasField(TEXT("value")) ? TEXT("value") : TEXT("opacity");
			FFrameNumber Frame;
			if (!UeAgentAuthoringJsonFields::ReadFloat32Field(**KeyObject, Field, Value, OutError)
				|| !TryKeyFrame(*MovieScene, *KeyObject, Frame, OutError)) return false;
			if (!UE::UAI::Authoring::Timeline::ScalarChannel::TryUpsertValue(Section->GetChannel(), *Section, Frame,
				FMovieSceneFloatValue(Value), &OutError)) return false;
			++OutCounters.AnimationKeysApplied;
		}
		if (!UE::UAI::Authoring::Timeline::TrySetMovieSceneSectionRange(*Section, TRange<FFrameNumber>::All(), OutError)) return false;
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
		if (!UeAgentUmgAuthoringUtilities::FindOrCreateUmgColorSection(MovieScene,
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
		for (int32 KeyIndex = 0; KeyIndex < Keys->Num(); ++KeyIndex)
		{
			const TSharedPtr<FJsonValue>& KeyValue = (*Keys)[KeyIndex];
			ON_SCOPE_EXIT { if (!OutError.IsEmpty()) UeAgentAuthoringDiagnostics::PrefixDocumentFailure(OutError, TEXT("/keys/") + FString::FromInt(KeyIndex)); };
			const TSharedPtr<FJsonObject>* KeyObject = nullptr;
			if (!KeyValue.IsValid() || !KeyValue->TryGetObject(KeyObject) || !KeyObject ||
				!KeyObject->IsValid())
			{
				SetError(OutError, TEXT("umg_animation_key_invalid"), TEXT("Each animation key must be an object."), WidgetName);
				OutError.ActualValue = KeyValue;
				OutError.ExpectedValue = MakeShared<FJsonValueString>(TEXT("object"));
				return false;
			}
			FFrameNumber Frame;
			if (!TryKeyFrame(*MovieScene, *KeyObject, Frame, OutError)) return false;
			for (int32 ChannelIndex = 0; ChannelIndex < UE_ARRAY_COUNT(Fields); ++ChannelIndex)
			{
				float Value = ChannelIndex == 3 ? 1.0f : 0.0f;
				if (!UeAgentAuthoringJsonFields::ReadFloat32Field(**KeyObject, Fields[ChannelIndex], Value, OutError)) return false;
				if (!UE::UAI::Authoring::Timeline::ScalarChannel::TryUpsertValue(*Channels[ChannelIndex], *Section,
					Frame, FMovieSceneFloatValue(Value), &OutError)) return false;
			}
			++OutCounters.AnimationKeysApplied;
		}
		if (!UE::UAI::Authoring::Timeline::TrySetMovieSceneSectionRange(*Section, TRange<FFrameNumber>::All(), OutError)) return false;
		return true;
	}
	SetError(OutError, TEXT("umg_animation_track_kind_unsupported"), TEXT("This animation track kind is not supported by the widget animation authoring path."), Kind);
	OutError.CandidateName = WidgetName;
	OutError.ActualValue = MakeShared<FJsonValueString>(Kind);
	OutError.ExpectedValue = MakeShared<FJsonValueString>(TEXT("transform|opacity|float_property|color|background_color|color_property"));
	UeAgentAuthoringDiagnostics::LocateFailure(OutError, *Track, TEXT("animation_apply"),
		UeAgentBlueprintAssetLifecycle::GetAuthoringTargetPath(Blueprint), TEXT("track_kind"));
	return false;
}

bool ApplyAnimations(UWidgetBlueprint& Blueprint,
	const FString& CanonicalAssetPath,
	const TSharedPtr<const FJsonObject>& Extension,
	FMutationCounters& OutCounters,
	UE::UAI::Contract::FDiagnostic& OutError,
	TMap<FName, UE::UAI::Authoring::Timeline::FMovieSceneCreationState>& CreationStates)
{
	TArray<FMapEntry> Entries;
	bool bPresent = false;
	if (!GetObjectMap(Extension, TEXT("animations"), Entries, bPresent, OutError) || !bPresent)
	{
		return !bPresent || OutError.IsEmpty();
	}
	for (const FMapEntry& Entry : Entries)
	{
		ON_SCOPE_EXIT { if (!OutError.IsEmpty()) UeAgentAuthoringDiagnostics::LocateFailure(OutError, *Entry.Spec,
			TEXT("animation_apply"), UeAgentBlueprintAssetLifecycle::GetAuthoringTargetPath(Blueprint)); };
		if (!ValidateAuthoredEntryFields(Entry,
				{TEXT("operation"),
					TEXT("name"),
					TEXT("display_label"),
					TEXT("start_seconds"),
					TEXT("duration_seconds"),
					TEXT("display_rate_num"),
					TEXT("display_rate_den"),
					TEXT("tracks"), TEXT("display_rate"), TEXT("tick_resolution"), TEXT("playback_range"), TEXT("bindings"), TEXT("root_tracks")},
				TEXT("umg_animation_unknown_field"),
				OutError))
		{
			return false;
		}
		FString Name;
		if (!UeAgentAuthoringJsonFields::ValidateOptionalFields(*Entry.Spec,
			{{TEXT("name"), EJson::String}, {TEXT("display_label"), EJson::String}}, OutError)) return false;
		TryGetString(Entry.Spec, TEXT("name"), Name);
		if (Name.IsEmpty()) Name = Entry.Key;
		UWidgetAnimation* Animation =
			UeAgentUmgAuthoringUtilities::FindWidgetAnimationByAnyName(&Blueprint, Name);
		const FString Op = Operation(Entry.Spec);
		if (Op == TEXT("create") && Animation)
		{ SetError(OutError, TEXT("umg_animation_create_target_exists"), TEXT("The animation requested for creation already exists."), Name); return false; }
		if (Op == TEXT("update") && !Animation)
		{ SetError(OutError, TEXT("umg_animation_update_target_not_found"), TEXT("The animation requested for update does not exist."), Name); return false; }
		if (Op == TEXT("remove"))
		{
			if (!Animation)
			{
				SetError(OutError, TEXT("umg_animation_remove_target_not_found"), TEXT("The animation requested for removal does not exist."), Name);
				return false;
			}
			Blueprint.Animations.Remove(Animation);
			Blueprint.OnVariableRemoved(Animation->GetFName());
			Animation->Rename(
				nullptr, GetTransientPackage(), REN_DontCreateRedirectors | REN_NonTransactional);
			++OutCounters.AnimationsRemoved;
			continue;
		}
		const bool bCommonScene = Entry.Spec->HasField(TEXT("tick_resolution")) || Entry.Spec->HasField(TEXT("display_rate"))
			|| Entry.Spec->HasField(TEXT("playback_range")) || Entry.Spec->HasField(TEXT("bindings"))
			|| Entry.Spec->HasField(TEXT("root_tracks"));
		if (bCommonScene && (Entry.Spec->HasField(TEXT("tracks")) || Entry.Spec->HasField(TEXT("start_seconds"))
			|| Entry.Spec->HasField(TEXT("duration_seconds")) || Entry.Spec->HasField(TEXT("display_rate_num"))
			|| Entry.Spec->HasField(TEXT("display_rate_den"))))
		{ SetError(OutError, TEXT("umg_animation_scene_formats_mixed"), TEXT("Common movie-scene fields and legacy widget animation fields cannot be mixed in one animation."), Name); return false; }
		if (!Animation)
		{
			if (!ValidateGeneratedMemberName(Blueprint, *UWidgetAnimation::StaticClass(),
				Name, NAME_None, *Entry.Spec, true, OutError)) return false;
			Animation = NewObject<UWidgetAnimation>(&Blueprint, FName(*Name), RF_Transactional);
			if (!Animation)
			{
				SetError(OutError, TEXT("umg_animation_create_failed"), TEXT("The native widget animation object could not be created."), Name);
				return false;
			}
			Animation->MovieScene =
				NewObject<UMovieScene>(Animation, FName(*Name), RF_Transactional);
			if (!Animation->MovieScene)
			{
				SetError(OutError, TEXT("umg_animation_movie_scene_create_failed"), TEXT("The new widget animation movie scene could not be created."), Name);
				return false;
			}
			// Native deserialization canonicalizes an empty untracked scene to [0,0).
			// TRange equates all empty ranges, so transition through a nonempty range
			// to let the native setter replace the constructor's exclusive lower bound.
			Animation->MovieScene->SetPlaybackRange(0, 1);
			Animation->MovieScene->SetPlaybackRange(0, 0);
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
		if (bCommonScene)
		{
			if (!TryUpdateAnimationState(*Animation, CanonicalAssetPath, Entry.Spec, OutError,
				&CreationStates.FindOrAdd(FName(*Name)))) return false;
			continue;
		}
		double Start = Animation->GetStartTime();
		double Duration = FMath::Max(0.001, Animation->GetEndTime() - Animation->GetStartTime());
		if (!UeAgentAuthoringJsonFields::ValidateOptionalFields(*Entry.Spec,
			{{TEXT("start_seconds"), EJson::Number}, {TEXT("duration_seconds"), EJson::Number},
			 {TEXT("tracks"), EJson::Array}}, OutError)) return false;
		const bool bHasStart = TryGetNumber(Entry.Spec, TEXT("start_seconds"), Start);
		const bool bHasDuration = TryGetNumber(Entry.Spec, TEXT("duration_seconds"), Duration);
		if (bHasStart || bHasDuration)
		{
			if (!UeAgentUmgAuthoringUtilities::SetMovieScenePlaybackRangeSeconds(
					Animation->MovieScene, Start, Duration, OutError))
			{
				OutError.CandidateName = Name;
				return false;
			}
		}
		int32 RateNum = Animation->MovieScene->GetDisplayRate().Numerator;
		int32 RateDen = Animation->MovieScene->GetDisplayRate().Denominator;
		const bool bHasRateNum = Entry.Spec->HasField(TEXT("display_rate_num"));
		const bool bHasRateDen = Entry.Spec->HasField(TEXT("display_rate_den"));
		if (!UeAgentAuthoringJsonFields::ReadInt32Field(*Entry.Spec, TEXT("display_rate_num"), RateNum, OutError)
			|| !UeAgentAuthoringJsonFields::ReadInt32Field(*Entry.Spec, TEXT("display_rate_den"), RateDen, OutError)) return false;
		if (bHasRateNum || bHasRateDen)
		{
			if (RateNum <= 0.0 || RateDen <= 0.0)
			{
				SetError(OutError, TEXT("umg_animation_display_rate_invalid"), TEXT("The animation display-rate numerator and denominator must both be positive."), Name);
				OutError.ActualValue = MakeShared<FJsonValueNumber>(RateNum <= 0 ? RateNum : RateDen);
				OutError.ExpectedValue = MakeShared<FJsonValueString>(TEXT("positive integer"));
				UeAgentAuthoringDiagnostics::LocateFailure(OutError, *Entry.Spec, TEXT("animation_apply"), CanonicalAssetPath,
					RateNum <= 0 ? TEXT("display_rate_num") : TEXT("display_rate_den"));
				return false;
			}
			Animation->MovieScene->SetDisplayRate(
				FFrameRate(RateNum, RateDen));
		}

		const TArray<TSharedPtr<FJsonValue>>* Tracks = nullptr;
		if (Entry.Spec->TryGetArrayField(TEXT("tracks"), Tracks))
		{
			if (!Tracks)
			{
				SetError(OutError, TEXT("umg_animation_tracks_invalid"), TEXT("The animation tracks field must be an array."), Name);
				return false;
			}
			Animation->AnimationBindings.Reset();
			Animation->MovieScene =
				NewObject<UMovieScene>(Animation, Animation->GetFName(), RF_Transactional);
			if (!Animation->MovieScene)
			{
				SetError(OutError, TEXT("umg_animation_movie_scene_reset_failed"), TEXT("The replacement movie scene for the animation could not be created."), Name);
				return false;
			}
			if (!UeAgentUmgAuthoringUtilities::SetMovieScenePlaybackRangeSeconds(
				Animation->MovieScene, Start, Duration, OutError))
			{ OutError.CandidateName = Name; return false; }
			Animation->MovieScene->SetDisplayRate(
				FFrameRate(RateNum, RateDen));
			for (int32 TrackIndex = 0; TrackIndex < Tracks->Num(); ++TrackIndex)
			{
				const TSharedPtr<FJsonValue>& TrackValue = (*Tracks)[TrackIndex];
				ON_SCOPE_EXIT { if (!OutError.IsEmpty()) UeAgentAuthoringDiagnostics::PrefixDocumentFailure(OutError, TEXT("/tracks/") + FString::FromInt(TrackIndex)); };
				const TSharedPtr<FJsonObject>* Track = nullptr;
				if (!TrackValue.IsValid() || !TrackValue->TryGetObject(Track) || !Track ||
					!Track->IsValid() ||
					!AddTrackKeys(Blueprint, *Animation, *Track, OutCounters, OutError))
				{
					if (OutError.IsEmpty())
						SetError(OutError, TEXT("umg_animation_track_invalid"), TEXT("The animation track must be an object naming an existing widget and a track kind."), Name);
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

struct FAnimationSectionIdentity { FName Name; FString ClassPath; };
struct FAnimationTrackIdentity { FName Name; FString ClassPath; FString PropertyPath; TArray<FAnimationSectionIdentity> Sections; };
struct FAnimationBindingIdentity
{
	FGuid Guid; FName WidgetName; FName SlotWidgetName; bool bRoot = false;
	TArray<FAnimationTrackIdentity> Tracks;
};
struct FAnimationIdentity { TArray<FAnimationBindingIdentity> Bindings; TSharedPtr<const FJsonObject> Source; };

bool CaptureInitializedAnimationIdentities(UWidgetBlueprint& Blueprint, const TSharedPtr<const FJsonObject>& Extension,
	TMap<FName, FAnimationIdentity>& Out, UE::UAI::Contract::FDiagnostic& Error)
{
	TArray<FMapEntry> Entries; bool Present = false;
	if (!GetObjectMap(Extension, TEXT("animations"), Entries, Present, Error)) return false;
	for (const auto& Entry : Entries)
	{
		if ((!Entry.Spec->HasField(TEXT("tracks")) && !Entry.Spec->HasField(TEXT("bindings")))
			|| Operation(Entry.Spec) == TEXT("remove")) continue;
		FString Name; TryGetString(Entry.Spec, TEXT("name"), Name); if (Name.IsEmpty()) Name = Entry.Key;
		auto* Animation = UeAgentUmgAuthoringUtilities::FindWidgetAnimationByAnyName(&Blueprint, Name);
		ON_SCOPE_EXIT { if (!Error.IsEmpty()) UeAgentAuthoringDiagnostics::LocateFailure(Error, *Entry.Spec,
			TEXT("animation_identity_capture"), UeAgentBlueprintAssetLifecycle::GetAuthoringTargetPath(Blueprint)); };
		if (!Animation || !Animation->GetMovieScene()) {
			SetError(Error, TEXT("umg_animation_prepared_identity_capture_failed"), !Animation
				? TEXT("The prepared widget animation was not found.") : TEXT("The prepared widget animation has no movie scene."), Name);
			return false;
		}
		FAnimationIdentity Identity;
		Identity.Source = Entry.Spec;
		for (const auto& Binding : Animation->AnimationBindings)
		{
			const auto* Native = Animation->GetMovieScene()->FindBinding(Binding.AnimationGuid);
			if (!Native) {
				SetError(Error, TEXT("umg_animation_prepared_identity_capture_failed"), TEXT("The widget animation binding has no corresponding movie-scene binding."), Name);
				Error.ActualValue = MakeShared<FJsonValueString>(Binding.bIsRootWidget ? TEXT("root widget") : Binding.WidgetName.ToString());
				return false;
			}
			FAnimationBindingIdentity B; B.Guid = Binding.AnimationGuid; B.WidgetName = Binding.WidgetName; B.SlotWidgetName = Binding.SlotWidgetName; B.bRoot = Binding.bIsRootWidget;
			for (const auto* Track : Native->GetTracks())
			{
				const auto* Property = Cast<UMovieScenePropertyTrack>(Track);
				if (!Property) {
					SetError(Error, TEXT("umg_animation_prepared_identity_capture_failed"), TEXT("The widget animation binding contains a track that is not a property track."), Name);
					Error.ExpectedValue = MakeShared<FJsonValueString>(UMovieScenePropertyTrack::StaticClass()->GetPathName());
					Error.ActualValue = Track ? TSharedPtr<FJsonValue>(MakeShared<FJsonValueString>(Track->GetClass()->GetPathName())) : MakeShared<FJsonValueNull>();
					return false;
				}
				FAnimationTrackIdentity T; T.Name = Track->GetFName(); T.ClassPath = Track->GetClass()->GetPathName(); T.PropertyPath = Property->GetPropertyPath().ToString();
				for (const auto* Section : Track->GetAllSections()) T.Sections.Add({Section->GetFName(), Section->GetClass()->GetPathName()});
				B.Tracks.Add(MoveTemp(T));
			}
			Identity.Bindings.Add(MoveTemp(B));
		}
		Out.Add(Animation->GetFName(), MoveTemp(Identity));
	}
	return true;
}

bool ApplyInitializedAnimationIdentities(UWidgetBlueprint& Blueprint, const TMap<FName, FAnimationIdentity>& Identities, UE::UAI::Contract::FDiagnostic& Error)
{
	// Validate every ownership/count/collision before changing any prepared identity.
	struct FChange { TFunction<bool()> Apply; UE::UAI::Contract::FDiagnostic Failure; };
	TArray<FChange> Changes;
	for (const auto& Item : Identities)
	{
		const FString AnimationName = Item.Key.ToString();
		FString NativePath;
		auto Failure = [&](const TCHAR* Reason) {
			auto Result = UE::UAI::Contract::FDiagnostic::Failure(TEXT("umg_animation_prepared_identity_conflict"), Reason, TEXT("animation_identity_apply"));
			Result.CandidateName = AnimationName;
			Result.TargetPath = UeAgentBlueprintAssetLifecycle::GetAuthoringTargetPath(Blueprint);
			if (Item.Value.Source) UeAgentAuthoringDiagnostics::LocateFailure(Result, *Item.Value.Source, Result.Stage, Result.TargetPath);
			Result.PropertyPath = NativePath;
			return Result;
		};
		auto Fail = [&](const TCHAR* Reason) { Error = Failure(Reason); return false; };
		auto CountMismatch = [&](const TCHAR* Reason, int32 Expected, int32 Actual) {
			Error = Failure(Reason); Error.ExpectedValue = MakeShared<FJsonValueNumber>(Expected); Error.ActualValue = MakeShared<FJsonValueNumber>(Actual); return false;
		};
		auto ValueMismatch = [&](const TCHAR* Reason, const FString& Expected, const FString& Actual) {
			Error = Failure(Reason); Error.ExpectedValue = MakeShared<FJsonValueString>(Expected); Error.ActualValue = MakeShared<FJsonValueString>(Actual); return false;
		};
		auto* Animation = UeAgentUmgAuthoringUtilities::FindWidgetAnimationByAnyName(&Blueprint, AnimationName);
		if (!Animation) return Fail(TEXT("The prepared widget animation was not found."));
		auto* Scene = Animation->GetMovieScene();
		if (!Scene) return Fail(TEXT("The prepared widget animation has no movie scene."));
		NativePath = TEXT("animation_bindings");
		if (Animation->AnimationBindings.Num() != Item.Value.Bindings.Num())
			return CountMismatch(TEXT("The widget animation binding count no longer matches the prepared state."), Item.Value.Bindings.Num(), Animation->AnimationBindings.Num());
		NativePath = TEXT("movie_scene.bindings");
		if (Scene->GetBindings().Num() != Item.Value.Bindings.Num())
			return CountMismatch(TEXT("The movie-scene binding count no longer matches the prepared animation."), Item.Value.Bindings.Num(), Scene->GetBindings().Num());
		TSet<FGuid> Used;
		auto Rename = [&](UObject* Object, FName Name) {
			if (Object->GetFName() == Name) return true;
			if (FindObjectFast<UObject>(Object->GetOuter(), Name)) {
				Error = Failure(TEXT("The prepared animation object name is already occupied."));
				Error.ExpectedValue = MakeShared<FJsonValueString>(Name.ToString());
				Error.ActualValue = MakeShared<FJsonValueString>(Object->GetName());
				return false;
			}
			auto RenameFailure = Failure(TEXT("The animation object could not be renamed to its prepared name."));
			RenameFailure.ExpectedValue = MakeShared<FJsonValueString>(Name.ToString());
			RenameFailure.ActualValue = MakeShared<FJsonValueString>(Object->GetName());
			Changes.Add({[Object, Name] { return Object->Rename(*Name.ToString(), nullptr, REN_DontCreateRedirectors | REN_NonTransactional); }, MoveTemp(RenameFailure)});
			return true;
		};
		for (const auto& Expected : Item.Value.Bindings)
		{
			const FString BindingName = Expected.bRoot ? TEXT("root") : Expected.WidgetName.ToString() + (Expected.SlotWidgetName.IsNone() ? FString() : TEXT(".slot:") + Expected.SlotWidgetName.ToString());
			const FString BindingPath = TEXT("bindings[") + BindingName + TEXT("]");
			NativePath = BindingPath;
			auto* Binding = Animation->AnimationBindings.FindByPredicate([&](const FWidgetAnimationBinding& B) {
				return B.bIsRootWidget == Expected.bRoot
					&& (Expected.bRoot || (B.WidgetName == Expected.WidgetName && B.SlotWidgetName == Expected.SlotWidgetName));
			});
			if (!Binding) return Fail(TEXT("The prepared animation widget binding no longer exists."));
			if (Used.Contains(Binding->AnimationGuid)) return Fail(TEXT("More than one prepared widget binding resolves to the same movie-scene binding."));
			if (!Expected.Guid.IsValid()) return Fail(TEXT("The prepared animation binding identity is invalid."));
			Used.Add(Binding->AnimationGuid);
			const auto* Native = Scene->FindBinding(Binding->AnimationGuid);
			const auto* Possessable = Scene->FindPossessable(Binding->AnimationGuid);
			if (!Native) return Fail(TEXT("The widget binding has no corresponding movie-scene binding."));
			if (!Possessable) return Fail(TEXT("The widget binding has no corresponding movie-scene possessable."));
			if (Native->GetTracks().Num() != Expected.Tracks.Num())
				return CountMismatch(TEXT("The animation binding track count no longer matches the prepared state."), Expected.Tracks.Num(), Native->GetTracks().Num());
			if (Binding->AnimationGuid != Expected.Guid)
			{
				if (Scene->FindBinding(Expected.Guid) || Scene->FindPossessable(Expected.Guid)) return Fail(TEXT("The prepared animation binding identity is already occupied in the movie scene."));
				auto Replacement = *Possessable; Replacement.SetGuid(Expected.Guid); const auto Old = Binding->AnimationGuid;
				Changes.Add({[Scene, Binding, Replacement, Old] {
					if (!Scene->ReplacePossessable(Old, Replacement)) return false;
					Binding->AnimationGuid = Replacement.GetGuid(); return true;
				}, Failure(TEXT("The movie scene could not replace the widget binding with its prepared identity."))});
			}
			for (int32 Index = 0; Index < Expected.Tracks.Num(); ++Index)
			{
				auto* Track = Native->GetTracks()[Index]; const auto& T = Expected.Tracks[Index];
				NativePath = BindingPath + TEXT(".tracks[") + FString::FromInt(Index) + TEXT("]");
				const auto* Property = Cast<UMovieScenePropertyTrack>(Track);
				if (!Property) return ValueMismatch(TEXT("The prepared animation track is no longer a property track."), UMovieScenePropertyTrack::StaticClass()->GetPathName(), Track ? Track->GetClass()->GetPathName() : TEXT("missing"));
				if (Track->GetClass()->GetPathName() != T.ClassPath)
					return ValueMismatch(TEXT("The animation track class no longer matches the prepared state."), T.ClassPath, Track->GetClass()->GetPathName());
				if (Property->GetPropertyPath().ToString() != T.PropertyPath)
					return ValueMismatch(TEXT("The animation track property path no longer matches the prepared state."), T.PropertyPath, Property->GetPropertyPath().ToString());
				if (Track->GetAllSections().Num() != T.Sections.Num())
					return CountMismatch(TEXT("The animation track section count no longer matches the prepared state."), T.Sections.Num(), Track->GetAllSections().Num());
				if (!Rename(Track, T.Name)) return false;
				for (int32 SectionIndex = 0; SectionIndex < T.Sections.Num(); ++SectionIndex)
				{
					auto* Section = Track->GetAllSections()[SectionIndex]; const auto& S = T.Sections[SectionIndex];
					NativePath = BindingPath + TEXT(".tracks[") + FString::FromInt(Index) + TEXT("].sections[") + FString::FromInt(SectionIndex) + TEXT("]");
					if (!Section || Section->GetClass()->GetPathName() != S.ClassPath)
						return ValueMismatch(TEXT("The animation section class no longer matches the prepared state."), S.ClassPath, Section ? Section->GetClass()->GetPathName() : TEXT("missing"));
					if (!Rename(Section, S.Name)) return false;
				}
			}
		}
	}
	for (const auto& Change : Changes) if (!Change.Apply()) { Error = Change.Failure; Error.Code = TEXT("umg_animation_prepared_identity_failed"); return false; }
	return true;
}

bool ApplyDomainBeforeCompile(UWidgetBlueprint& Blueprint,
	const FString& CanonicalAssetPath,
	const TSharedPtr<const FJsonObject>& Extension,
	FMutationCounters& OutCounters,
	UE::UAI::Contract::FDiagnostic& OutError,
	TMap<FName, UE::UAI::Authoring::Timeline::FMovieSceneCreationState>& CreationStates,
	TArray<UeAgentDetailsAuthoringService::FScopedPropertyIntent>& PropertyIntents,
	UPackage* TextIdentityPackage = nullptr)
{
	PropertyIntents.Reset();
	return ApplyWidgetTree(Blueprint, Extension, OutCounters, OutError, TextIdentityPackage, PropertyIntents) &&
		   ApplyAnimations(Blueprint, CanonicalAssetPath, Extension, OutCounters, OutError, CreationStates);
}
} // namespace

struct FPreparedUmgMutation::FImpl
{
	FString CanonicalAssetPath;
	TMap<FName, FAnimationIdentity> AnimationIdentities;
	TMap<FName, UE::UAI::Authoring::Timeline::FMovieSceneCreationState> MovieSceneCreationStates;
	TSharedPtr<const FJsonObject> Extension;
	TUniquePtr<UeAgentBlueprintAuthoring::FPreparedBlueprintMutation> BlueprintMutation;
	TArray<UeAgentDetailsAuthoringService::FScopedPropertyIntent> PropertyIntents;
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
	UE::UAI::Contract::FDiagnostic& OutError,
	const FString& CanonicalAssetPath)
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
	NewImpl->Extension = ProviderExtension.IsValid()
		? UeAgentAuthoringState::CloneCanonicalValue(MakeShared<FJsonValueObject>(
			MakeShared<FJsonObject>(*ProviderExtension)))->AsObject() : nullptr;
	NewImpl->CanonicalAssetPath = CanonicalAssetPath.IsEmpty() ? ScratchBlueprint.GetOutermost()->GetName() : CanonicalAssetPath;
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
			ScratchBlueprint, NewImpl->CanonicalAssetPath, NewImpl->Extension, OutExpectedCounters,
			OutError, NewImpl->MovieSceneCreationStates, NewImpl->PropertyIntents, FindPackage(nullptr, *NewImpl->CanonicalAssetPath)))
	{
		return false;
	}
	if (!CaptureInitializedAnimationIdentities(ScratchBlueprint, ProviderExtension, NewImpl->AnimationIdentities, OutError))
	{ return false; }
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
	if (!NewImpl->BlueprintMutation->VerifyIntent(ScratchBlueprint, OutError)) {
		UeAgentAuthoringDiagnostics::ReportConsistencyWarning(TEXT("umg_authored_intent_failed"), OutError, TEXT("umg_intent_observation"));
		OutError.Reset();
	}
	TArray<UeAgentAuthoringState::FReadRequest> Requests;
	if (!NewImpl->PropertyIntents.IsEmpty()
		&& (!BuildAssetExportSnapshotReadRequests(ScratchBlueprint, Requests, OutError)
			|| !UeAgentDetailsAuthoringService::VerifyScopedPropertyIntents(NewImpl->PropertyIntents, Requests, OutError))) {
		UeAgentAuthoringDiagnostics::ReportConsistencyWarning(TEXT("umg_property_intent_failed"), OutError, TEXT("umg_intent_observation"));
		OutError.Reset();
	}
	OutMutation = TUniquePtr<FPreparedUmgMutation>(new FPreparedUmgMutation(MoveTemp(NewImpl)));
	return true;
}

bool FPreparedUmgMutation::ApplyTo(
	UWidgetBlueprint& Blueprint, FMutationCounters& OutCounters, UE::UAI::Contract::FDiagnostic& OutError) const
{
	OutCounters = FMutationCounters();
	if (!Impl.IsValid() || !Impl->BlueprintMutation.IsValid())
	{
		SetError(OutError, TEXT("umg_prepared_mutation_missing"), TEXT("No prepared widget Blueprint mutation is available for this operation."));
		return false;
	}
	if (!Impl->BlueprintMutation->ApplyTo(Blueprint, OutCounters.Blueprint, OutError)) return false;
	if (!ApplyDomainBeforeCompile(Blueprint, Impl->CanonicalAssetPath, Impl->Extension, OutCounters, OutError,
		Impl->MovieSceneCreationStates, Impl->PropertyIntents)) return false;
	if (!ApplyInitializedAnimationIdentities(Blueprint, Impl->AnimationIdentities, OutError))
	{ return false; }
	return true;
}

bool FPreparedUmgMutation::FinalizeAfterCompile(UWidgetBlueprint& Blueprint,
	FMutationCounters& OutCounters,
	bool& bOutRequiresSecondCompile,
	UE::UAI::Contract::FDiagnostic& OutError) const
{
	OutCounters = FMutationCounters();
	bOutRequiresSecondCompile = false;
	if (!Impl.IsValid() || !Impl->BlueprintMutation.IsValid())
	{
		SetError(OutError, TEXT("umg_prepared_mutation_missing"), TEXT("No prepared widget Blueprint mutation is available for this operation."));
		return false;
	}
	if (!Impl->BlueprintMutation->FinalizeAfterCompile(Blueprint, OutCounters.Blueprint, OutError))
		return false;
	// Prepare validates create/update/remove against the input state. Finalization
	// rebinds after compilation, including publication of an already prepared asset.
	if (!ApplyBindings(Blueprint, Impl->Extension, OutCounters, OutError, true)) return false;
	bOutRequiresSecondCompile = Impl->bHasBindings;
	return true;
}

bool FPreparedUmgMutation::VerifyPropertyIntent(UWidgetBlueprint& Blueprint, UE::UAI::Contract::FDiagnostic& OutError) const
{
	if (!Impl) { OutError = UE::UAI::Contract::FDiagnostic::Failure(TEXT("umg_prepared_mutation_missing"), TEXT("No prepared widget Blueprint mutation is available for this operation."), TEXT("widget_authoring")); return false; }
	if (Impl->PropertyIntents.IsEmpty()) { OutError.Reset(); return true; }
	TArray<UeAgentAuthoringState::FReadRequest> Requests;
	return BuildAssetExportSnapshotReadRequests(Blueprint, Requests, OutError)
		&& UeAgentDetailsAuthoringService::VerifyScopedPropertyIntents(Impl->PropertyIntents, Requests, OutError);
}

bool FPreparedUmgMutation::VerifyIntent(UWidgetBlueprint& Blueprint, UE::UAI::Contract::FDiagnostic& OutError) const
{
	if (!Impl || !Impl->BlueprintMutation) { OutError = UE::UAI::Contract::FDiagnostic::Failure(TEXT("umg_prepared_mutation_missing"), TEXT("No prepared widget Blueprint mutation is available for this operation."), TEXT("widget_authoring")); return false; }
	return Impl->BlueprintMutation->VerifyIntent(Blueprint, OutError)
		&& VerifyPropertyIntent(Blueprint, OutError);
}

bool FPreparedUmgMutation::RestoreFromBackup(
	const UWidgetBlueprint& Backup, UWidgetBlueprint& Target, UE::UAI::Contract::FDiagnostic& OutError)
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
