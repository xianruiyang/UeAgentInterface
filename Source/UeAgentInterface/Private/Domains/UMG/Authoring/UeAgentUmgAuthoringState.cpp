// Copyright Epic Games, Inc. All Rights Reserved.

#include "Domains/UMG/Authoring/UeAgentUmgAuthoringState.h"
#include "Domains/UMG/Authoring/UeAgentUmgAnimationState.h"

#include "Animation/MovieScene2DTransformSection.h"
#include "Animation/MovieScene2DTransformTrack.h"
#include "Animation/WidgetAnimation.h"
#include "Animation/WidgetAnimationBinding.h"
#include "Blueprint/UserWidget.h"
#include "Blueprint/WidgetTree.h"
#include "Channels/MovieSceneFloatChannel.h"
#include "Components/PanelSlot.h"
#include "Components/PanelWidget.h"
#include "Components/Widget.h"
#include "Domains/Blueprint/Authoring/UeAgentBlueprintAssetLifecycle.h"
#include "Domains/Blueprint/Authoring/UeAgentBlueprintAuthoringState.h"
#include "Domains/UMG/Authoring/UeAgentUmgAuthoringDescriptors.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Kismet2/KismetEditorUtilities.h"
#include "MovieScene.h"
#include "Sections/MovieSceneColorSection.h"
#include "Sections/MovieSceneFloatSection.h"
#include "Tracks/MovieSceneColorTrack.h"
#include "Tracks/MovieSceneFloatTrack.h"
#include "Tracks/MovieScenePropertyTrack.h"
#include "Utilities/AuthoringCore/UeAgentAuthoringIdentity.h"
#include "Utilities/AuthoringCore/UeAgentAuthoringNativeDefaults.h"
#include "WidgetBlueprint.h"
#include "Blueprint/WidgetBlueprintGeneratedClass.h"

namespace UeAgentUmgAuthoring
{
	TSet<FName> WidgetStructuralProperties() { return {TEXT("Slot"), TEXT("bIsVariable")}; }
	namespace
	{
		FString GuidText(const FGuid& Guid)
		{
			return Guid.IsValid()
				? Guid.ToString(EGuidFormats::DigitsWithHyphensLower)
				: FString();
		}

		bool BuildIdentity(
			const FString& ModelId,
			const FString& TargetKind,
			const TMap<FString, FString>& Selectors,
			UeAgentAuthoringIdentity::FCanonicalIdentity& OutIdentity,
			UE::UAI::Contract::FDiagnostic& OutError)
		{
			return UeAgentAuthoringIdentity::BuildCanonicalIdentity(
				ModelId, TargetKind, Selectors, OutIdentity, OutError);
		}

		bool AddReadRequest(
			const FString& SemanticId,
			const FString& ModelId,
			const FString& TargetKind,
			const TMap<FString, FString>& Selectors,
			const UObject* RootObject,
			const TMap<FString, FString>& Context,
			TArray<UeAgentAuthoringState::FReadRequest>& OutRequests,
			UE::UAI::Contract::FDiagnostic& OutError)
		{
			UeAgentAuthoringState::FReadRequest Request;
			Request.SemanticId = SemanticId;
			if (!BuildIdentity(ModelId, TargetKind, Selectors, Request.TargetIdentity, OutError))
			{
				return false;
			}
			Request.TargetQuery.ModelId = Request.TargetIdentity.ModelId;
			Request.TargetQuery.TargetKind = Request.TargetIdentity.TargetKind;
			Request.TargetQuery.CanonicalKey = Request.TargetIdentity.CanonicalKey;
			Request.RootObject = RootObject;
			Request.RootOwnerPath = Selectors.FindRef(TEXT("asset_path"));
			Request.Context = Context;
			OutRequests.Add(MoveTemp(Request));
			return true;
		}

		bool AppendPropertyRequests(
			const UObject* Object,
			const FString& SemanticId,
			const FString& ModelId,
			const FString& TargetKind,
			const TMap<FString, FString>& BaseSelectors,
			const TSet<FName>& Excluded,
			TArray<UeAgentAuthoringState::FReadRequest>& OutRequests,
			UE::UAI::Contract::FDiagnostic& OutError)
		{
			if (!Object) return true;
			UeAgentAuthoringState::FEditablePropertyReadOptions Options;
			Options.SemanticId = SemanticId;
			// Designer widget/slot selection uses instance fields, even in an archetype preview tree.
			// Root class defaults are captured separately by the Blueprint owner.
			Options.Limits.bUseInstanceEditability = true;
			Options.ExcludedProperties = Excluded;
			Options.BuildSnapshotIdentity =
				[ModelId, TargetKind, BaseSelectors](
					const FString& PropertyPath,
					UeAgentAuthoringIdentity::FCanonicalIdentity& OutIdentity,
					UE::UAI::Contract::FDiagnostic& IdentityError)
			{
				TMap<FString, FString> Selectors = BaseSelectors;
				Selectors.Add(TEXT("property_path"), PropertyPath);
				return BuildIdentity(ModelId, TargetKind, Selectors, OutIdentity, IdentityError);
			};
			return UeAgentAuthoringState::AppendEditableObjectPropertyReadRequests(
				Object, Options, OutRequests, OutError);
		}

		void CollectWidgets(UWidget* Widget, TArray<UWidget*>& OutWidgets)
		{
			if (!Widget) return;
			OutWidgets.Add(Widget);
			if (UPanelWidget* Panel = Cast<UPanelWidget>(Widget))
			{
				for (int32 Index = 0; Index < Panel->GetChildrenCount(); ++Index)
				{
					CollectWidgets(Panel->GetChildAt(Index), OutWidgets);
				}
			}
		}

		void BuildParentMaps(
			UWidget* Widget,
			const FString& ParentName,
			TMap<const UWidget*, FString>& OutParents,
			TMap<const UWidget*, int32>& OutIndices)
		{
			if (!Widget) return;
			OutParents.Add(Widget, ParentName);
			if (UPanelWidget* Panel = Cast<UPanelWidget>(Widget))
			{
				for (int32 Index = 0; Index < Panel->GetChildrenCount(); ++Index)
				{
					if (UWidget* Child = Panel->GetChildAt(Index))
					{
						OutIndices.Add(Child, Index);
						BuildParentMaps(Child, Widget->GetName(), OutParents, OutIndices);
					}
				}
			}
		}

		TMap<FString, FString> WidgetSelectors(
			const UWidgetBlueprint& Blueprint,
			const UWidget& Widget,
			const FString& CanonicalAssetPath)
		{
			const FGuid* Guid = Blueprint.WidgetVariableNameToGuidMap.Find(Widget.GetFName());
			return {
				{ TEXT("asset_path"), CanonicalAssetPath.IsEmpty()
					? Blueprint.GetOutermost()->GetName() : CanonicalAssetPath },
				{ TEXT("widget_guid"), Guid ? GuidText(*Guid) : FString() },
				{ TEXT("widget_class"), Widget.GetClass()->GetPathName() },
				{ TEXT("widget_name"), Widget.GetName() },
			};
		}

		TSharedRef<FJsonObject> BuildWidgetState(
			const UWidget& Widget,
			const FString& ParentName,
			const int32 Index)
		{
			TSharedRef<FJsonObject> State = MakeShared<FJsonObject>();
			State->SetStringField(TEXT("name"), Widget.GetName());
			State->SetStringField(TEXT("class"), Widget.GetClass()->GetPathName());
			State->SetBoolField(TEXT("make_variable"), Widget.bIsVariable);
			if (!ParentName.IsEmpty())
			{
				State->SetStringField(TEXT("parent"), ParentName);
				State->SetNumberField(TEXT("index"), Index);
			}
			return State;
		}

		TSharedRef<FJsonObject> BuildSlotState(const UPanelSlot& Slot)
		{
			TSharedRef<FJsonObject> State = MakeShared<FJsonObject>();
			State->SetStringField(TEXT("class"), Slot.GetClass()->GetPathName());
			return State;
		}

		TSharedRef<FJsonObject> BuildBindingState(const FDelegateEditorBinding& Binding)
		{
			TSharedRef<FJsonObject> State = MakeShared<FJsonObject>();
			State->SetStringField(TEXT("widget_name"), Binding.ObjectName);
			State->SetStringField(TEXT("property_name"), Binding.PropertyName.ToString());
			if (Binding.Kind == EBindingKind::Property)
			{
				State->SetStringField(TEXT("binding_kind"), TEXT("property_variable"));
				State->SetStringField(TEXT("source_variable_name"), Binding.SourceProperty.ToString());
			}
			else
			{
				State->SetStringField(TEXT("binding_kind"), TEXT("function"));
				State->SetStringField(TEXT("function_name"), Binding.FunctionName.ToString());
			}
			return State;
		}

		TSharedPtr<FJsonObject> BuildAssetVerificationState(
			const UWidgetBlueprint& Blueprint,
			const FString& CanonicalAssetPath, UE::UAI::Contract::FDiagnostic& OutError)
		{
			if (!UeAgentBlueprintAssetLifecycle::ValidateBlueprintStructure(Blueprint, OutError)) return nullptr;
			TSharedRef<FJsonObject> State = MakeShared<FJsonObject>();
			State->SetStringField(
				TEXT("asset_path"),
				CanonicalAssetPath.IsEmpty() ? Blueprint.GetOutermost()->GetName() : CanonicalAssetPath);
			State->SetStringField(TEXT("asset_kind"), TEXT("widget_blueprint"));
			State->SetStringField(
				TEXT("parent_class"), Blueprint.ParentClass ? Blueprint.ParentClass->GetPathName() : FString());

			TMap<const UWidget*, FString> Parents;
			TMap<const UWidget*, int32> Indices;
			BuildParentMaps(Blueprint.WidgetTree->RootWidget, FString(), Parents, Indices);
			TArray<FString> WidgetKeys;
			TArray<UWidget*> Widgets;
			CollectWidgets(Blueprint.WidgetTree->RootWidget, Widgets);
			for (const UWidget* Widget : Widgets)
			{
				if (!Widget) continue;
				WidgetKeys.Add(FString::Printf(
					TEXT("%s|%s|%s|%d|%s"),
					*Widget->GetName(),
					*Widget->GetClass()->GetPathName(),
					*Parents.FindRef(Widget),
					Indices.FindRef(Widget),
					Widget->Slot ? *Widget->Slot->GetClass()->GetPathName() : TEXT("")));
			}
			WidgetKeys.Sort();
			TArray<TSharedPtr<FJsonValue>> WidgetValues;
			for (const FString& Key : WidgetKeys) WidgetValues.Add(MakeShared<FJsonValueString>(Key));
			State->SetArrayField(TEXT("widgets"), WidgetValues);

			TArray<TSharedPtr<FJsonValue>> BindingValues;
			for (const FDelegateEditorBinding& Binding : Blueprint.Bindings)
			{
				BindingValues.Add(MakeShared<FJsonValueObject>(BuildBindingState(Binding)));
			}
			BindingValues.Sort([](const auto& A, const auto& B) {
				const FString AWidget = A->AsObject()->GetStringField(TEXT("widget_name"));
				const FString BWidget = B->AsObject()->GetStringField(TEXT("widget_name"));
				return AWidget == BWidget
					? A->AsObject()->GetStringField(TEXT("property_name")) < B->AsObject()->GetStringField(TEXT("property_name"))
					: AWidget < BWidget;
			});
			State->SetArrayField(TEXT("bindings"), BindingValues);

			TArray<TSharedPtr<FJsonValue>> AnimationValues;
			for (const UWidgetAnimation* Animation : Blueprint.Animations)
			{
				if (Animation)
				{
					TSharedPtr<FJsonObject> AnimationState;
					if (!TryReadAnimationState(*Animation, CanonicalAssetPath, AnimationState, OutError)) return nullptr;
					AnimationValues.Add(MakeShared<FJsonValueObject>(AnimationState));
				}
			}
			// Keep structure for the shared semantic comparator and leaf diagnostics.
			// A canonical string here turns a section/key difference into one opaque blob.
			AnimationValues.Sort([](const auto& A, const auto& B) {
				return A->AsObject()->GetStringField(TEXT("name")) < B->AsObject()->GetStringField(TEXT("name"));
			});
			State->SetArrayField(TEXT("animations"), AnimationValues);
			return State;
		}

		UeAgentAuthoringState::EReadOutcome ReadState(
			const UeAgentAuthoringState::FReadRequest& Request,
			UeAgentAuthoringState::FStateValue& OutValue,
			UE::UAI::Contract::FDiagnostic& OutError)
		{
			OutValue = UeAgentAuthoringState::FStateValue();
			OutValue.TargetIdentity = Request.TargetIdentity;
			OutValue.Provenance = StateAccessorId;
			OutValue.bEditable = Request.SemanticId != ExportTargetSemanticId;
			OutError.Reset();
			if (Request.Context.FindRef(TEXT("expected_absent")) == TEXT("true"))
			{
				OutValue.bAvailable = false;
				OutValue.AvailabilityReason = TEXT("target_not_found");
				return UeAgentAuthoringState::EReadOutcome::Unavailable;
			}

			if (Request.SemanticId == ParentClassSemanticId)
			{
				const UWidgetBlueprint* Blueprint = Cast<UWidgetBlueprint>(Request.RootObject);
				if (!Blueprint || !Blueprint->ParentClass)
				{
OutError = UE::UAI::Contract::FDiagnostic::Failure(TEXT("umg_parent_class_state_invalid"),
						!Blueprint ? TEXT("Parent-class capture requires a WidgetBlueprint root object.")
						: TEXT("The WidgetBlueprint has no parent class to capture."), TEXT("state_capture"));
					OutError.SourceKind = TEXT("native_result");
					OutError.TargetPath = Request.TargetIdentity.GetSelector(TEXT("asset_path"));
					OutError.PropertyPath = !Blueprint ? TEXT("/root_object/class") : TEXT("/parent_class");
					if (!Blueprint && Request.RootObject) OutError.ActualValue = MakeShared<FJsonValueString>(Request.RootObject->GetClass()->GetPathName());
OutError.ExpectedValue = MakeShared<FJsonValueString>(!Blueprint ? UWidgetBlueprint::StaticClass()->GetPathName() : FString(TEXT("valid parent class")));
					return UeAgentAuthoringState::EReadOutcome::Failed;
				}
				OutValue.Type.Kind = TEXT("string");
				OutValue.Value = MakeShared<FJsonValueString>(Blueprint->ParentClass->GetPathName());
				return UeAgentAuthoringState::EReadOutcome::Resolved;
			}

			OutValue.Type.Kind = TEXT("object");
			if (Request.SemanticId == AssetSemanticId || Request.SemanticId == ExportTargetSemanticId)
			{
				const UWidgetBlueprint* Blueprint = Cast<UWidgetBlueprint>(Request.RootObject);
				if (!Blueprint)
				{
					OutValue.bAvailable = false;
					OutValue.AvailabilityReason = TEXT("target_not_found");
					return UeAgentAuthoringState::EReadOutcome::Unavailable;
				}
				TSharedPtr<FJsonObject> State = Request.SemanticId == AssetSemanticId
					? BuildAssetVerificationState(
						*Blueprint, Request.TargetIdentity.GetSelector(TEXT("asset_path")), OutError)
					: MakeShared<FJsonObject>();
				if (!State) return UeAgentAuthoringState::EReadOutcome::Failed;
				if (Request.SemanticId == ExportTargetSemanticId)
				{
					State->SetStringField(
						TEXT("asset_path"), Request.TargetIdentity.GetSelector(TEXT("asset_path")));
					State->SetStringField(TEXT("asset_kind"), TEXT("widget_blueprint"));
				}
				OutValue.Value = MakeShared<FJsonValueObject>(State);
				return UeAgentAuthoringState::EReadOutcome::Resolved;
			}

			if (Request.SemanticId == WidgetSemanticId)
			{
				const UWidget* Widget = Cast<UWidget>(Request.RootObject);
				if (!Widget)
				{
					OutValue.bAvailable = false;
					OutValue.AvailabilityReason = TEXT("widget_not_found");
					return UeAgentAuthoringState::EReadOutcome::Unavailable;
				}
				OutValue.Value = MakeShared<FJsonValueObject>(BuildWidgetState(
					*Widget,
					Request.Context.FindRef(TEXT("parent_name")),
					FCString::Atoi(*Request.Context.FindRef(TEXT("index")))));
				return UeAgentAuthoringState::EReadOutcome::Resolved;
			}

			if (Request.SemanticId == SlotSemanticId)
			{
				const UPanelSlot* Slot = Cast<UPanelSlot>(Request.RootObject);
				if (!Slot)
				{
					OutValue.bAvailable = false;
					OutValue.AvailabilityReason = TEXT("slot_not_found");
					return UeAgentAuthoringState::EReadOutcome::Unavailable;
				}
				OutValue.Value = MakeShared<FJsonValueObject>(BuildSlotState(*Slot));
				return UeAgentAuthoringState::EReadOutcome::Resolved;
			}

			if (Request.SemanticId == BindingSemanticId)
			{
				const UWidgetBlueprint* Blueprint = Cast<UWidgetBlueprint>(Request.RootObject);
				if (!Blueprint)
				{
OutError = UE::UAI::Contract::FDiagnostic::Failure(TEXT("umg_binding_state_invalid"),
						FString::Printf(TEXT("Binding '%s.%s' requires a WidgetBlueprint root object."),
							*Request.Context.FindRef(TEXT("widget_name")), *Request.Context.FindRef(TEXT("property_name"))), TEXT("state_capture"));
					OutError.SourceKind = TEXT("native_result");
					OutError.TargetPath = Request.TargetIdentity.GetSelector(TEXT("asset_path"));
					OutError.PropertyPath = TEXT("/root_object/class");
					if (Request.RootObject) OutError.ActualValue = MakeShared<FJsonValueString>(Request.RootObject->GetClass()->GetPathName());
OutError.ExpectedValue = MakeShared<FJsonValueString>(UWidgetBlueprint::StaticClass()->GetPathName());
					return UeAgentAuthoringState::EReadOutcome::Failed;
				}
				for (const FDelegateEditorBinding& Binding : Blueprint->Bindings)
				{
					if (Binding.ObjectName == Request.Context.FindRef(TEXT("widget_name"))
						&& Binding.PropertyName.ToString() == Request.Context.FindRef(TEXT("property_name")))
					{
						OutValue.Value = MakeShared<FJsonValueObject>(BuildBindingState(Binding));
						return UeAgentAuthoringState::EReadOutcome::Resolved;
					}
				}
				OutValue.bAvailable = false;
				OutValue.AvailabilityReason = TEXT("binding_not_found");
				return UeAgentAuthoringState::EReadOutcome::Unavailable;
			}

			if (Request.SemanticId == AnimationSemanticId)
			{
				const UWidgetAnimation* Animation = Cast<UWidgetAnimation>(Request.RootObject);
				if (!Animation)
				{
					OutValue.bAvailable = false;
					OutValue.AvailabilityReason = TEXT("animation_not_found");
					return UeAgentAuthoringState::EReadOutcome::Unavailable;
				}
				TSharedPtr<FJsonObject> State;
				if (!TryReadAnimationState(*Animation, Request.TargetIdentity.GetSelector(TEXT("asset_path")), State, OutError))
					return UeAgentAuthoringState::EReadOutcome::Failed;
				OutValue.Value = MakeShared<FJsonValueObject>(State);
				return UeAgentAuthoringState::EReadOutcome::Resolved;
			}

			return UeAgentAuthoringState::EReadOutcome::Unavailable;
		}

		UClass* ResolveClass(const FString& ClassPath, UClass* RequiredBase)
		{
			UClass* Class = ClassPath.IsEmpty()
				? nullptr
				: StaticLoadClass(RequiredBase, nullptr, *ClassPath);
			return Class && Class->IsChildOf(RequiredBase) ? Class : nullptr;
		}

		UeAgentAuthoringNativeDefaults::EProbeOutcome ProbeNativeDefault(
			const UeAgentAuthoringNativeDefaults::FRequest& Request,
			UeAgentAuthoringNativeDefaults::FScratchScope& Scratch,
			UeAgentAuthoringNativeDefaults::FProbeValue& OutValue,
			UE::UAI::Contract::FDiagnostic& OutError)
		{
			OutValue = UeAgentAuthoringNativeDefaults::FProbeValue();
			OutError.Reset();
			UPackage* Package = Scratch.CreateScratchPackage(OutError);
			if (!Package) return UeAgentAuthoringNativeDefaults::EProbeOutcome::Failed;

			if (Request.TargetIdentity.TargetKind == TEXT("umg_asset"))
			{
				UWidgetBlueprint* Seed = CreateFactorySeed(
					UUserWidget::StaticClass(), Package, NAME_None, RF_Transactional | RF_Transient, OutError);
				if (!Seed) return UeAgentAuthoringNativeDefaults::EProbeOutcome::Failed;
				auto State = BuildAssetVerificationState(*Seed, Request.TargetIdentity.GetSelector(TEXT("asset_path")), OutError);
				if (!State) return UeAgentAuthoringNativeDefaults::EProbeOutcome::Failed;
				OutValue.Value = MakeShared<FJsonValueObject>(State);
			}
			else if (Request.TargetIdentity.TargetKind == TEXT("umg_widget"))
			{
				UClass* WidgetClass = ResolveClass(
					Request.TargetIdentity.GetSelector(TEXT("widget_class")), UWidget::StaticClass());
				if (!WidgetClass) return UeAgentAuthoringNativeDefaults::EProbeOutcome::Unavailable;
				UWidgetBlueprint* Seed = CreateFactorySeed(
					UUserWidget::StaticClass(), Package, NAME_None, RF_Transactional, OutError);
				UWidget* Widget = Seed && Seed->WidgetTree
					? Seed->WidgetTree->ConstructWidget<UWidget>(WidgetClass, NAME_None) : nullptr;
				if (!Widget)
				{
if (OutError.IsEmpty()) {
						OutError = UE::UAI::Contract::FDiagnostic::Failure(TEXT("umg_widget_native_default_create_failed"),
							!Seed ? TEXT("The native-default probe could not create its WidgetBlueprint seed.")
							: !Seed->WidgetTree ? TEXT("The native-default seed has no WidgetTree.")
							: TEXT("The WidgetTree could not construct the requested widget class."), TEXT("state_capture"));
						OutError.SourceKind = TEXT("native_result");
						OutError.TargetPath = Request.TargetIdentity.GetSelector(TEXT("asset_path"));
						OutError.PropertyPath = TEXT("/widget_class");
						OutError.ActualValue = MakeShared<FJsonValueString>(WidgetClass->GetPathName());
						OutError.ExpectedValue = MakeShared<FJsonValueString>(TEXT("widget class constructible in a WidgetTree"));
					}
					return UeAgentAuthoringNativeDefaults::EProbeOutcome::Failed;
				}
				OutValue.Value = MakeShared<FJsonValueObject>(BuildWidgetState(*Widget, FString(), INDEX_NONE));
			}
			else if (Request.TargetIdentity.TargetKind == TEXT("umg_slot"))
			{
				UClass* ParentClass = ResolveClass(Request.Context.FindRef(TEXT("parent_class")), UPanelWidget::StaticClass());
				UClass* WidgetClass = ResolveClass(
					Request.TargetIdentity.GetSelector(TEXT("widget_class")), UWidget::StaticClass());
				if (!ParentClass || !WidgetClass)
				{
					return UeAgentAuthoringNativeDefaults::EProbeOutcome::Unavailable;
				}
				UWidgetBlueprint* Seed = CreateFactorySeed(
					UUserWidget::StaticClass(), Package, NAME_None, RF_Transactional, OutError);
				UPanelWidget* Parent = Seed && Seed->WidgetTree
					? Seed->WidgetTree->ConstructWidget<UPanelWidget>(ParentClass, NAME_None) : nullptr;
				UWidget* Child = Seed && Seed->WidgetTree
					? Seed->WidgetTree->ConstructWidget<UWidget>(WidgetClass, NAME_None) : nullptr;
				UPanelSlot* Slot = Parent && Child ? Parent->AddChild(Child) : nullptr;
				if (!Slot)
				{
if (OutError.IsEmpty()) {
						OutError = UE::UAI::Contract::FDiagnostic::Failure(TEXT("umg_slot_native_default_create_failed"),
							!Seed ? TEXT("The slot probe could not create its WidgetBlueprint seed.")
							: !Seed->WidgetTree ? TEXT("The slot probe seed has no WidgetTree.")
							: !Parent ? TEXT("The WidgetTree could not construct the requested parent panel.")
							: !Child ? TEXT("The WidgetTree could not construct the requested child widget.")
							: TEXT("The parent panel did not create a slot for the child widget."), TEXT("state_capture"));
						OutError.SourceKind = TEXT("native_result");
						OutError.TargetPath = Request.TargetIdentity.GetSelector(TEXT("asset_path"));
						OutError.PropertyPath = TEXT("/slot");
						const auto Actual = MakeShared<FJsonObject>();
						Actual->SetStringField(TEXT("parent_class"), ParentClass->GetPathName());
						Actual->SetStringField(TEXT("widget_class"), WidgetClass->GetPathName());
						Actual->SetBoolField(TEXT("parent_created"), Parent != nullptr);
						Actual->SetBoolField(TEXT("child_created"), Child != nullptr);
						OutError.ActualValue = MakeShared<FJsonValueObject>(Actual);
						OutError.ExpectedValue = MakeShared<FJsonValueString>(TEXT("constructed parent and child with a panel slot"));
					}
					return UeAgentAuthoringNativeDefaults::EProbeOutcome::Failed;
				}
				OutValue.Value = MakeShared<FJsonValueObject>(BuildSlotState(*Slot));
			}
			else
			{
				return UeAgentAuthoringNativeDefaults::EProbeOutcome::Unavailable;
			}
			OutValue.Provenance = NativeDefaultProviderId;
			return UeAgentAuthoringNativeDefaults::EProbeOutcome::Resolved;
		}
	}

	UWidgetBlueprint* CreateFactorySeed(
		UClass* ParentClass,
		UObject* Outer,
		FName Name,
		const EObjectFlags Flags,
		UE::UAI::Contract::FDiagnostic& OutError)
	{
		OutError.Reset();
		if (!Outer || !ParentClass || !ParentClass->IsChildOf(UUserWidget::StaticClass())
			|| !FKismetEditorUtilities::CanCreateBlueprintOfClass(ParentClass))
		{
OutError = UE::UAI::Contract::FDiagnostic::Failure(TEXT("umg_factory_input_invalid"),
				!Outer ? TEXT("Creating a WidgetBlueprint seed requires an outer object.")
				: !ParentClass ? TEXT("Creating a WidgetBlueprint seed requires a parent class.")
				: !ParentClass->IsChildOf(UUserWidget::StaticClass()) ? TEXT("The seed parent class must derive from UserWidget.")
				: TEXT("Unreal does not allow creating a Blueprint of the requested parent class."), TEXT("state_capture"));
			OutError.SourceKind = TEXT("native_result");
			OutError.PropertyPath = !Outer ? TEXT("/outer") : TEXT("/parent_class");
if (!Outer) OutError.ActualValue = MakeShared<FJsonValueBoolean>(false);
			else if (ParentClass) OutError.ActualValue = MakeShared<FJsonValueString>(ParentClass->GetPathName());
			else OutError.ActualValuePresent = false;
			OutError.ExpectedValue = MakeShared<FJsonValueString>(!Outer ? TEXT("existing outer object") : TEXT("UserWidget class that permits Blueprint creation"));
			return nullptr;
		}
		if (Name.IsNone())
		{
			Name = MakeUniqueObjectName(Outer, UWidgetBlueprint::StaticClass(), TEXT("WidgetBlueprintSeed"));
		}
		UWidgetBlueprint* Result = Cast<UWidgetBlueprint>(FKismetEditorUtilities::CreateBlueprint(
			ParentClass,
			Outer,
			Name,
			BPTYPE_Normal,
			UWidgetBlueprint::StaticClass(),
			UWidgetBlueprintGeneratedClass::StaticClass(),
			FName(TEXT("UeAgentInterface"))));
		if (!Result || !Result->WidgetTree)
		{
OutError = UE::UAI::Contract::FDiagnostic::Failure(TEXT("umg_factory_create_failed"),
				!Result ? TEXT("Unreal did not create a WidgetBlueprint seed of the requested parent class.")
				: TEXT("The created WidgetBlueprint seed has no WidgetTree."), TEXT("state_capture"));
			OutError.SourceKind = TEXT("native_result");
			OutError.TargetPath = ParentClass->GetPathName();
			OutError.PropertyPath = !Result ? TEXT("/seed") : TEXT("/seed/widget_tree");
			OutError.ActualValue = MakeShared<FJsonValueBoolean>(false);
			OutError.ExpectedValue = MakeShared<FJsonValueBoolean>(true);
			OutError.SubjectId = Name.ToString();
			return nullptr;
		}
		Result->SetFlags(Flags);
		return Result;
	}

	static bool AppendAssetContentReadRequests(
		UWidgetBlueprint& Asset,
		const FString& CanonicalAssetPath,
		TArray<UeAgentAuthoringState::FReadRequest>& OutRequests,
		UE::UAI::Contract::FDiagnostic& OutError,
		const bool bForVerification)
	{
		const auto AppendBlueprintRequests = bForVerification
			? UeAgentBlueprintAuthoring::AppendReusableBlueprintVerificationReadRequests
			: UeAgentBlueprintAuthoring::AppendReusableBlueprintExportSnapshotReadRequests;
		if (!AppendBlueprintRequests(
			Asset, OutRequests, OutError, CanonicalAssetPath))
		{
			return false;
		}

		TMap<const UWidget*, FString> Parents;
		TMap<const UWidget*, int32> Indices;
		if (Asset.WidgetTree) BuildParentMaps(Asset.WidgetTree->RootWidget, FString(), Parents, Indices);
		TArray<UWidget*> Widgets;
		if (Asset.WidgetTree) CollectWidgets(Asset.WidgetTree->RootWidget, Widgets);
		for (UWidget* Widget : Widgets)
		{
			if (!Widget) continue;
			const TMap<FString, FString> Selectors = WidgetSelectors(
				Asset, *Widget, CanonicalAssetPath);
			const TMap<FString, FString> Context = {
				{ TEXT("parent_name"), Parents.FindRef(Widget) },
				{ TEXT("index"), LexToString(Indices.FindRef(Widget)) },
			};
			if (!AddReadRequest(
				WidgetSemanticId, WidgetIdentityModelId, TEXT("umg_widget"),
				Selectors, Widget, Context, OutRequests, OutError)
				|| !AppendPropertyRequests(
					Widget,
					WidgetPropertySemanticId,
					WidgetPropertyIdentityModelId,
					TEXT("umg_widget_property"),
					Selectors,
					WidgetStructuralProperties(),
					OutRequests,
					OutError))
			{
				return false;
			}
			if (Widget->Slot
				&& (!AddReadRequest(
					SlotSemanticId, SlotIdentityModelId, TEXT("umg_slot"),
					Selectors, Widget->Slot, {}, OutRequests, OutError)
					|| !AppendPropertyRequests(
						Widget->Slot,
						SlotPropertySemanticId,
						SlotPropertyIdentityModelId,
						TEXT("umg_slot_property"),
						Selectors,
						{},
						OutRequests,
						OutError)))
			{
				return false;
			}
		}

		for (const FDelegateEditorBinding& Binding : Asset.Bindings)
		{
			const TMap<FString, FString> Selectors = {
				{ TEXT("asset_path"), CanonicalAssetPath },
				{ TEXT("widget_name"), Binding.ObjectName },
				{ TEXT("property_name"), Binding.PropertyName.ToString() },
			};
			if (!AddReadRequest(
				BindingSemanticId, BindingIdentityModelId, TEXT("umg_binding"),
				Selectors, &Asset, Selectors, OutRequests, OutError))
			{
				return false;
			}
		}

		for (UWidgetAnimation* Animation : Asset.Animations)
		{
			if (!Animation) continue;
			const TMap<FString, FString> Selectors = {
				{ TEXT("asset_path"), CanonicalAssetPath },
				{ TEXT("animation_name"), Animation->GetName() },
			};
			if (!AddReadRequest(
				AnimationSemanticId, AnimationIdentityModelId, TEXT("umg_animation"),
				Selectors, Animation, {}, OutRequests, OutError))
			{
				return false;
			}
		}
		return true;
	}

	bool BuildAssetSnapshotReadRequests(
		UWidgetBlueprint* Asset,
		const FString& CanonicalAssetPath,
		const bool bExpectedAbsent,
		TArray<UeAgentAuthoringState::FReadRequest>& OutRequests,
		UE::UAI::Contract::FDiagnostic& OutError)
	{
		OutRequests.Reset();
		OutError.Reset();
		if (CanonicalAssetPath.IsEmpty() || (!Asset && !bExpectedAbsent))
		{
OutError = UE::UAI::Contract::FDiagnostic::Failure(TEXT("umg_snapshot_identity_invalid"),
				CanonicalAssetPath.IsEmpty() ? TEXT("Asset snapshot capture requires a canonical asset path.")
				: TEXT("Snapshot capture expected an existing WidgetBlueprint but no asset was provided."), TEXT("state_capture"));
			OutError.SourceKind = TEXT("native_result");
			OutError.TargetPath = CanonicalAssetPath;
			OutError.PropertyPath = CanonicalAssetPath.IsEmpty() ? TEXT("/asset_path") : TEXT("/asset_present");
			if (CanonicalAssetPath.IsEmpty()) {
				OutError.ActualValue = MakeShared<FJsonValueString>(CanonicalAssetPath);
				OutError.ExpectedValue = MakeShared<FJsonValueString>(TEXT("nonempty canonical asset path"));
			} else {
				OutError.ActualValue = MakeShared<FJsonValueBoolean>(false);
				OutError.ExpectedValue = MakeShared<FJsonValueBoolean>(true);
			}
			return false;
		}
		UeAgentAuthoringState::FReadRequest Request;
		Request.SemanticId = AssetSemanticId;
		if (!BuildIdentity(
			AssetIdentityModelId,
			TEXT("umg_asset"),
			{ { TEXT("asset_path"), CanonicalAssetPath } },
			Request.TargetIdentity,
			OutError))
		{
			return false;
		}
		Request.TargetQuery.ModelId = Request.TargetIdentity.ModelId;
		Request.TargetQuery.TargetKind = Request.TargetIdentity.TargetKind;
		Request.TargetQuery.CanonicalKey = Request.TargetIdentity.CanonicalKey;
		Request.RootObject = Asset;
		Request.RootOwnerPath = CanonicalAssetPath;
		if (bExpectedAbsent) Request.Context.Add(TEXT("expected_absent"), TEXT("true"));
		OutRequests.Add(MoveTemp(Request));
		return !Asset || AppendAssetContentReadRequests(
			*Asset, CanonicalAssetPath, OutRequests, OutError, true);
	}

	bool BuildAssetExportSnapshotReadRequests(
		UWidgetBlueprint& Asset,
		TArray<UeAgentAuthoringState::FReadRequest>& OutRequests,
		UE::UAI::Contract::FDiagnostic& OutError)
	{
		OutRequests.Reset();
		OutError.Reset();
		const FString AssetPath = Asset.GetOutermost()->GetName();
		const TMap<FString, FString> AssetSelectors = { { TEXT("asset_path"), AssetPath } };
		if (!AddReadRequest(
			ExportTargetSemanticId, AssetIdentityModelId, TEXT("umg_asset"),
			AssetSelectors, &Asset, {}, OutRequests, OutError)
			|| !AddReadRequest(
				ParentClassSemanticId, AssetIdentityModelId, TEXT("umg_asset"),
				AssetSelectors, &Asset, {}, OutRequests, OutError))
		{
			return false;
		}

		// Lifecycle snapshots receive root properties from the shared reflection owner.
		// Ordinary export captures those properties explicitly alongside UMG's own envelope.
		return UeAgentBlueprintAuthoring::AppendBlueprintRootPropertyReadRequests(
			Asset, OutRequests, OutError)
			&& AppendAssetContentReadRequests(Asset, AssetPath, OutRequests, OutError, false);
	}

	bool RegisterStateAccessor(UE::UAI::Contract::FDiagnostic& OutError)
	{
		UeAgentAuthoringState::FStateAccessorDescriptor Descriptor;
		Descriptor.AccessorId = StateAccessorId;
		Descriptor.TargetKinds = {
			TEXT("umg_asset"), TEXT("umg_widget"), TEXT("umg_slot"),
			TEXT("umg_binding"), TEXT("umg_animation")
		};
		Descriptor.CapabilityTags = {
			TEXT("asset"), TEXT("widget_tree"), TEXT("slot"),
			TEXT("layout"), TEXT("binding"), TEXT("animation")
		};
		UeAgentAuthoringState::ERegistrationResult Result;
		return UeAgentAuthoringState::FStateAccessorRegistry::Get().RegisterAccessor(
			Descriptor, ReadState, Result, OutError);
	}

	bool RegisterNativeDefaultProvider(UE::UAI::Contract::FDiagnostic& OutError)
	{
		UeAgentAuthoringNativeDefaults::FProviderDescriptor Descriptor;
		Descriptor.ProviderId = NativeDefaultProviderId;
		Descriptor.SourceKind = UeAgentAuthoringSemanticRegistry::EDefaultSourceKind::DomainNative;
		Descriptor.TargetKinds = { TEXT("umg_asset"), TEXT("umg_widget"), TEXT("umg_slot") };
		Descriptor.CapabilityTags = {
			TEXT("factory"), TEXT("widget_tree"), TEXT("panel_slot"), TEXT("post_create")
		};
		Descriptor.bRequiresScratch = true;
		UeAgentAuthoringNativeDefaults::ERegistrationResult Result;
		return UeAgentAuthoringNativeDefaults::FProviderRegistry::Get().RegisterProvider(
			Descriptor, ProbeNativeDefault, Result, OutError);
	}
}
