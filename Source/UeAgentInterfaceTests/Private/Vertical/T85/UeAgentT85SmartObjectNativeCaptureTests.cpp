#include "Misc/AutomationTest.h"
#include "Core/Contract/UeAgentDiagnostic.h"

#include "Domains/SmartObject/Authoring/UeAgentSmartObjectNativeCapture.h"
#include "Domains/SmartObject/Authoring/UeAgentSmartObjectAuthoringState.h"
#include "Domains/SmartObject/Authoring/UeAgentSmartObjectDocument.h"
#include "Domains/Reflection/Authoring/UeAgentReflectionAuthoringRegistration.h"
#include "Core/HjsonWorkspace/UeAgentAuthoringComposition.h"
#include "Core/HjsonWorkspace/DomainAuthoring/UeAgentDomainHjsonAuthoringKit.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "GameplayInteractionSmartObjectBehaviorDefinition.h"
#include "Annotations/SmartObjectSlotLinkAnnotation.h"
#include "StateTree.h"
#include "Serialization/ObjectWriter.h"
#include "UObject/Package.h"
#include "UObject/UnrealType.h"

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FUeAgentT85SmartObjectNativeCaptureTest,
	"UAI.Root.T85.SmartObject.NativeCapture", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FUeAgentT85SmartObjectNativeCaptureTest::RunTest(const FString& Parameters)
{
	auto* Definition = NewObject<USmartObjectDefinition>(GetTransientPackage());
	const auto* BagProperty = FindFProperty<FStructProperty>(Definition->GetClass(), TEXT("Parameters"));
	const auto* BindingProperty = FindFProperty<FStructProperty>(Definition->GetClass(), TEXT("BindingCollection"));
	const auto* RootIdProperty = FindFProperty<FStructProperty>(Definition->GetClass(), TEXT("RootID"));
	const auto* ParameterIdProperty = FindFProperty<FStructProperty>(Definition->GetClass(), TEXT("ParametersID"));
	const auto* DefaultsProperty = FindFProperty<FArrayProperty>(Definition->GetClass(), TEXT("DefaultBehaviorDefinitions"));
	const auto* DataProperty = FindFProperty<FArrayProperty>(Definition->GetClass(), TEXT("DefinitionData"));
	if (!TestTrue(TEXT("native definition storage exists"), BagProperty && BindingProperty && RootIdProperty
		&& ParameterIdProperty && DefaultsProperty && DataProperty)) return false;
	auto& Bag = *BagProperty->ContainerPtrToValuePtr<FInstancedPropertyBag>(Definition);
	Bag.AddProperty(TEXT("Threshold"), EPropertyBagPropertyType::Float);
	Bag.SetValueFloat(TEXT("Threshold"), 42.5f);
	const FGuid ParametersId = *ParameterIdProperty->ContainerPtrToValuePtr<FGuid>(Definition);
	// An uninitialized identity must remain observable; capture must not repair it.
	RootIdProperty->ContainerPtrToValuePtr<FGuid>(Definition)->Invalidate();
	auto& Defaults = *DefaultsProperty->ContainerPtrToValuePtr<TArray<TObjectPtr<USmartObjectBehaviorDefinition>>>(Definition);
	Defaults.Add(NewObject<UGameplayInteractionSmartObjectBehaviorDefinition>(Definition));
	Defaults.Add(nullptr);
	auto* BehaviorStateTree = NewObject<UStateTree>(GetTransientPackage());
	CastChecked<UGameplayInteractionSmartObjectBehaviorDefinition>(Defaults[0])->SetStateTree(BehaviorStateTree);
	auto& Data = *DataProperty->ContainerPtrToValuePtr<TArray<FSmartObjectDefinitionDataProxy>>(Definition);
	Data.AddDefaulted();
	Data[0].ID = FGuid::NewGuid();
	Definition->DebugAddSlot();
	Definition->DebugAddSlot();
	auto& First = Definition->GetMutableSlot(0);
	First.ID = FGuid::NewGuid(); First.Name = TEXT("Interaction"); First.bEnabled = false;
	First.BehaviorDefinitions.Add(NewObject<UGameplayInteractionSmartObjectBehaviorDefinition>(Definition));
	First.BehaviorDefinitions.Add(NewObject<UGameplayInteractionSmartObjectBehaviorDefinition>(Definition));
	First.DefinitionData.AddDefaulted(); First.DefinitionData[0].ID = FGuid::NewGuid();
	Definition->GetMutableSlot(1).ID = FGuid::NewGuid();
	Definition->GetMutableSlot(1).Name = TEXT("Untouched");
	First.DefinitionData[0].Data.InitializeAsScriptStruct(FSmartObjectSlotLinkAnnotation::StaticStruct());
	auto& Link = First.DefinitionData[0].Data.GetMutable<FSmartObjectSlotLinkAnnotation>();
	Link.LinkedSlot.SetIndex(1);
	FindFProperty<FStructProperty>(FSmartObjectSlotReference::StaticStruct(), TEXT("SlotID"))->ContainerPtrToValuePtr<FGuid>(&Link.LinkedSlot)[0]
		= Definition->GetSlots()[1].ID;
	auto& Bindings = *BindingProperty->ContainerPtrToValuePtr<FSmartObjectBindingCollection>(Definition);
	Bindings.AddBindableStruct(FSmartObjectDefinitionBindableStructDescriptor(
		TEXT("Captured Parameters"), Bag.GetPropertyBagStruct(), ParametersId, FSmartObjectDefinitionDataHandle::Parameters)).Category = TEXT("Preserved Category");
	Bindings.AddBindableStruct(FSmartObjectDefinitionBindableStructDescriptor(
		TEXT("Captured Slot"), FSmartObjectSlotDefinition::StaticStruct(), First.ID, FSmartObjectDefinitionDataHandle(0)));
	Bindings.AddSmartObjectBinding(FSmartObjectDefinitionPropertyBinding(
		FPropertyBindingPath(ParametersId, TEXT("Threshold")), FPropertyBindingPath(First.ID, TEXT("DEBUG_DrawSize"))));

	TArray<uint8> Before;
	FObjectWriter BeforeWriter(Definition, Before);
	UeAgentSmartObjectAuthoring::FDefinitionCapture Capture;
	UE::UAI::Contract::FDiagnostic Error;
	if (!UeAgentSmartObjectAuthoring::CaptureDefinition(*Definition, Capture, Error))
	{ AddError(Error.ToPublicDetail()); return false; }
	TestEqual(TEXT("all slots are borrowed"), Capture.Slots.Num(), 2);
	TestTrue(TEXT("slot state and both owned behaviors remain available"), Capture.Slots[0].Name == TEXT("Interaction")
		&& !Capture.Slots[0].bEnabled && Capture.Slots[0].BehaviorDefinitions.Num() == 2
		&& Capture.Slots[0].BehaviorDefinitions[1]->GetOuter() == Definition);
	TestTrue(TEXT("root and slot definition data keep their identities"), Capture.DefinitionData.Num() == 1
		&& Capture.DefinitionData[0].ID == Data[0].ID
		&& Capture.Slots[0].DefinitionData[0].ID == First.DefinitionData[0].ID);
	TestTrue(TEXT("default behavior collection retains empty entries"), Capture.DefaultBehaviors.Num() == 2
		&& Capture.DefaultBehaviors[0] == Defaults[0] && !Capture.DefaultBehaviors[1]);
	TestEqual(TEXT("parameter value is available"), Capture.Parameters->GetValueFloat(TEXT("Threshold")).GetValue(), 42.5f);
	TestTrue(TEXT("capture preserves existing and invalid identities"), Capture.ParametersId == ParametersId && !Capture.RootId.IsValid());
	TestEqual(TEXT("binding collection is retained"), Capture.Bindings->GetNumBindings(), 1);
	TestTrue(TEXT("persistent bindable descriptors retain nondefault metadata and native handles"), Capture.BindableStructs.Num() == 2
		&& Capture.BindableStructs[0].Name == TEXT("Captured Parameters")
		&& Capture.BindableStructs[0].Category == TEXT("Preserved Category")
		&& Capture.BindableStructs[0].ID == ParametersId
		&& Capture.BindableStructs[0].DataHandle == FSmartObjectDefinitionDataHandle::Parameters
		&& Capture.BindableStructs[1].DataHandle == FSmartObjectDefinitionDataHandle(0));
	Capture.Bindings->ForEachBinding([&](const FPropertyBindingBinding& Binding) {
		TestTrue(TEXT("binding source and target are not rebuilt"), Binding.GetSourcePath().GetStructID() == ParametersId
			&& Binding.GetSourcePath().ToString() == TEXT("Threshold") && Binding.GetTargetPath().GetStructID() == First.ID
			&& Binding.GetTargetPath().ToString() == TEXT("DEBUG_DrawSize"));
	});
	TArray<uint8> After;
	FObjectWriter AfterWriter(Definition, After);
	TestTrue(TEXT("native capture does not change persistent storage"), Before == After);
	if (!UeAgentReflectionAuthoring::RegisterAuthoring(Error)) { AddError(Error.ToPublicDetail()); return false; }
	TArray<UeAgentAuthoringState::FReadRequest> Requests;
	if (!UeAgentSmartObjectAuthoring::AppendStructuralReadRequests(*Definition, Definition->GetPathName(), Requests, Error))
	{ AddError(Error.ToPublicDetail()); return false; }
	UeAgentAuthoringState::FCaptureResult Frozen;
	if (!UeAgentAuthoringState::CaptureSnapshot(Requests, Frozen))
	{ AddError(Frozen.Diagnostic.Code + TEXT(":") + Frozen.Diagnostic.InternalContext); return false; }
	int32 ItemCount = 0;
	bool bDisabled = false, bParameter = false, bBehavior = false, bBinding = false;
	for (const auto& Entry : Frozen.Snapshot.CopyEntries())
	{
		if (Entry.SemanticId == UeAgentSmartObjectAuthoring::ItemSemanticId)
		{
			++ItemCount;
			bBinding |= Entry.Value->AsObject()->GetStringField(TEXT("kind")) == TEXT("binding")
				&& Entry.Value->AsObject()->HasField(TEXT("binding_state"));
		}
		if (Entry.SemanticId != UeAgentSmartObjectAuthoring::ItemPropertySemanticId) continue;
		const FString Path = Entry.TargetIdentity.GetSelector(TEXT("property_path"));
		if (Path == TEXT("slot.bEnabled") && Entry.TargetIdentity.GetSelector(TEXT("node_id")) == First.ID.ToString(EGuidFormats::DigitsWithHyphensLower))
		{
			const auto* Bool = Entry.NativeValue.IsValid() ? CastField<FBoolProperty>(Entry.NativeValue->GetProperty()) : nullptr;
			bDisabled = Bool && !Bool->GetPropertyValue(Entry.NativeValue->GetValuePtr());
		}
		bParameter |= Path == TEXT("parameters.Parameters") && UeAgentAuthoringState::MakeStableCanonicalValueKey(Entry.Value).Contains(TEXT("42.5"));
		bBehavior |= Path == TEXT("behavior.StateTreeReference");
	}
	TestEqual(TEXT("root, parameters, slots, all behavior entries, data and binding reach common capture"), ItemCount, 11);
	TestTrue(TEXT("scoped native values and complete binding state are captured"), bDisabled && bParameter && bBehavior && bBinding);
	using namespace UE::UAI;
	auto Resources = HjsonWorkspace::MakeSemanticWorkspaceResourceTracker();
	StateSemantic::FUeAgentCanonicalSnapshotPtr Canonical;
	HjsonWorkspace::FAuthoringModelPtr Model;
	HjsonWorkspace::FExportWorkspacePlanPtr Plan;
	HjsonWorkspace::FWorkspaceError WorkspaceError;
	if (!HjsonWorkspace::TryBuildCanonicalAuthoringCapture(TEXT("ue.smart_object.asset"), TEXT("llm_authoring"),
		Frozen.Snapshot, Resources, Canonical, Model, WorkspaceError)
		|| !HjsonWorkspace::TryBuildCompositionExportPlan(Model.ToSharedRef(), {}, Resources, Plan, WorkspaceError))
	{ AddError(WorkspaceError.GetCode() + TEXT(":") + WorkspaceError.GetSourceUnit()); return false; }
	FString Ordinary;
	for (const auto& File : Plan->GetFiles())
	{
		if (File.GetRelativePath().StartsWith(TEXT(".uai/"))) continue;
		const FUTF8ToTCHAR Text(reinterpret_cast<const ANSICHAR*>(File.GetCanonicalUtf8().GetData()), File.GetCanonicalUtf8().Num());
		Ordinary += FString(Text.Length(), Text.Get());
	}
	TestTrue(TEXT("ordinary keyed collection and nondefault parameter are visible"), Ordinary.Contains(TEXT("smart_object_items"))
		&& Ordinary.Contains(TEXT("Interaction")) && Ordinary.Contains(TEXT("42.5")));
	TestFalse(TEXT("slot identity stays out of ordinary HJSON"), Ordinary.Contains(First.ID.ToString(EGuidFormats::DigitsWithHyphensLower)));
	TestFalse(TEXT("parameter identity stays out of ordinary HJSON"), Ordinary.Contains(ParametersId.ToString(EGuidFormats::DigitsWithHyphensLower)));
	TestFalse(TEXT("internal bindable descriptor metadata stays out of ordinary HJSON"), Ordinary.Contains(TEXT("Preserved Category")));
	TArray<uint8> AfterProjection;
	FObjectWriter AfterProjectionWriter(Definition, AfterProjection);
	TestTrue(TEXT("canonical and ordinary capture remain read-only"), Before == AfterProjection);
	TSharedPtr<FJsonValue> BaselineDocument;
	if (!UeAgentSmartObjectAuthoring::ReadDefinitionDocument(*Definition, BaselineDocument, Error))
	{ AddError(Error.ToPublicDetail()); return false; }
	const FString BaselineKey = UeAgentAuthoringState::MakeStableCanonicalValueKey(BaselineDocument);
	First.bEnabled = true;
	Bag.SetValueFloat(TEXT("Threshold"), 83.25f);
	TSharedPtr<FJsonValue> DesiredDocument;
	if (!UeAgentSmartObjectAuthoring::ReadDefinitionDocument(*Definition, DesiredDocument, Error))
	{ AddError(Error.ToPublicDetail()); return false; }
	TestEqual(TEXT("document retains frozen values after native edits"),
		UeAgentAuthoringState::MakeStableCanonicalValueKey(BaselineDocument), BaselineKey);
	if (!UeAgentSmartObjectAuthoring::ApplyDefinitionDocumentProperties(*Definition, BaselineDocument, Error))
	{ AddError(Error.ToPublicDetail()); return false; }
	TestFalse(TEXT("scoped replay restores slot bool"), First.bEnabled);
	TestEqual(TEXT("scoped replay restores parameter bag value"), Bag.GetValueFloat(TEXT("Threshold")).GetValue(), 42.5f);
	const auto InvalidDocument = UeAgentAuthoringState::CloneCanonicalValue(DesiredDocument);
	const auto SlotItem = InvalidDocument->AsObject()->GetObjectField(TEXT("items"))->GetObjectField(
		First.ID.ToString(EGuidFormats::DigitsWithHyphensLower));
	const auto PropertyDocument = SlotItem->GetObjectField(TEXT("properties"));
	auto InvalidDescriptors = PropertyDocument->GetArrayField(TEXT("properties"));
	if (!TestFalse(TEXT("slot property descriptors exist"), InvalidDescriptors.IsEmpty())) return false;
	InvalidDescriptors.Last()->AsObject()->SetStringField(TEXT("property_path"), TEXT("slot.MissingProperty"));
	TestFalse(TEXT("invalid scoped binding rejects the complete replay"),
		UeAgentSmartObjectAuthoring::ApplyDefinitionDocumentProperties(*Definition, InvalidDocument, Error));
	TestTrue(TEXT("invalid binding has a specific error"), Error.Code == TEXT("smart_object_property_binding_missing"));
	TSharedPtr<FJsonValue> AfterRejectedDocument;
	if (!UeAgentSmartObjectAuthoring::ReadDefinitionDocument(*Definition, AfterRejectedDocument, Error))
	{ AddError(Error.ToPublicDetail()); return false; }
	TestEqual(TEXT("all property groups remain unchanged after binding rejection"),
		UeAgentAuthoringState::MakeStableCanonicalValueKey(AfterRejectedDocument), BaselineKey);
	if (!UeAgentSmartObjectAuthoring::ApplyDefinitionDocumentProperties(*Definition, DesiredDocument, Error))
	{ AddError(Error.ToPublicDetail()); return false; }
	TestTrue(TEXT("scoped replay applies desired slot bool"), First.bEnabled);
	TestEqual(TEXT("scoped replay applies desired parameter value"), Bag.GetValueFloat(TEXT("Threshold")).GetValue(), 83.25f);
	TSharedPtr<FJsonValue> AfterAppliedDocument;
	if (!UeAgentSmartObjectAuthoring::ReadDefinitionDocument(*Definition, AfterAppliedDocument, Error))
	{ AddError(Error.ToPublicDetail()); return false; }
	TestEqual(TEXT("replay preserves complete captured document including bindings and owned behaviors"),
		UeAgentAuthoringState::MakeStableCanonicalValueKey(AfterAppliedDocument),
		UeAgentAuthoringState::MakeStableCanonicalValueKey(DesiredDocument));
	const auto InvalidBindings = UeAgentAuthoringState::CloneCanonicalValue(AfterAppliedDocument);
	InvalidBindings->AsObject()->SetArrayField(TEXT("bindable_structs"), {MakeShared<FJsonValueString>(TEXT("invalid"))});
	TestFalse(TEXT("malformed descriptor rejects exact binding replay"),
		UeAgentSmartObjectAuthoring::ApplyDefinitionBindingState(*Definition, InvalidBindings, Error));
	TSharedPtr<FJsonValue> AfterBindingRejected;
	if (!UeAgentSmartObjectAuthoring::ReadDefinitionDocument(*Definition, AfterBindingRejected, Error))
	{ AddError(Error.ToPublicDetail()); return false; }
	TestEqual(TEXT("binding decode rejection leaves the original document unchanged"),
		UeAgentAuthoringState::MakeStableCanonicalValueKey(AfterBindingRejected),
		UeAgentAuthoringState::MakeStableCanonicalValueKey(AfterAppliedDocument));
	Bindings.Reset();
	TestEqual(TEXT("fixture cleared persistent binding descriptors"), Bindings.GetNumBindableStructDescriptors(), 0);
	TestEqual(TEXT("fixture cleared bindings"), Bindings.GetNumBindings(), 0);
	if (!UeAgentSmartObjectAuthoring::ApplyDefinitionBindingState(*Definition, AfterAppliedDocument, Error))
	{ AddError(Error.ToPublicDetail()); return false; }
	TSharedPtr<FJsonValue> AfterBindingRestored;
	if (!UeAgentSmartObjectAuthoring::ReadDefinitionDocument(*Definition, AfterBindingRestored, Error))
	{ AddError(Error.ToPublicDetail()); return false; }
	TestEqual(TEXT("binding arrays restore exactly without native regeneration"),
		UeAgentAuthoringState::MakeStableCanonicalValueKey(AfterBindingRestored),
		UeAgentAuthoringState::MakeStableCanonicalValueKey(AfterAppliedDocument));
	const auto InvalidStructure = UeAgentAuthoringState::CloneCanonicalValue(AfterAppliedDocument);
	InvalidStructure->AsObject()->GetObjectField(TEXT("items"))->GetObjectField(
		First.ID.ToString(EGuidFormats::DigitsWithHyphensLower))->SetStringField(TEXT("parent"), TEXT("missing"));
	TestFalse(TEXT("invalid structure is rejected before mutation"),
		UeAgentSmartObjectAuthoring::ApplyDefinitionDocument(*Definition, InvalidStructure, Error));
	TestEqual(TEXT("invalid parent has a specific error"), Error.Code, FString(TEXT("smart_object_document_parent_invalid")));
	TSharedPtr<FJsonValue> AfterStructureRejected;
	if (!UeAgentSmartObjectAuthoring::ReadDefinitionDocument(*Definition, AfterStructureRejected, Error))
	{ AddError(Error.ToPublicDetail()); return false; }
	TestEqual(TEXT("invalid structure preserves the complete source document"),
		UeAgentAuthoringState::MakeStableCanonicalValueKey(AfterStructureRejected),
		UeAgentAuthoringState::MakeStableCanonicalValueKey(AfterAppliedDocument));
	auto* Rebuilt = NewObject<USmartObjectDefinition>(GetTransientPackage());
	if (!UeAgentSmartObjectAuthoring::ApplyDefinitionDocument(*Rebuilt, AfterAppliedDocument, Error))
	{ AddError(Error.ToPublicDetail()); return false; }
	TSharedPtr<FJsonValue> RebuiltDocument;
	if (!UeAgentSmartObjectAuthoring::ReadDefinitionDocument(*Rebuilt, RebuiltDocument, Error))
	{ AddError(Error.ToPublicDetail()); return false; }
	TestEqual(TEXT("empty native asset reconstructs the complete frozen document"),
		UeAgentAuthoringState::MakeStableCanonicalValueKey(RebuiltDocument),
		UeAgentAuthoringState::MakeStableCanonicalValueKey(AfterAppliedDocument));
	TestTrue(TEXT("recreated behaviors belong to the rebuilt asset"), Rebuilt->GetSlots().Num() == 2
		&& Rebuilt->GetSlots()[0].BehaviorDefinitions.Num() == 2
		&& Rebuilt->GetSlots()[0].BehaviorDefinitions[0]->GetOuter() == Rebuilt
		&& Rebuilt->GetSlots()[0].BehaviorDefinitions[0] != Definition->GetSlots()[0].BehaviorDefinitions[0]);
	const auto& RebuiltLink = Rebuilt->GetSlots()[0].DefinitionData[0].Data.Get<FSmartObjectSlotLinkAnnotation>();
	TestTrue(TEXT("nonempty definition data preserves native slot reference identity and index"),
		RebuiltLink.LinkedSlot.GetIndex() == 1 && RebuiltLink.LinkedSlot.GetSlotID() == Rebuilt->GetSlots()[1].ID);
	UeAgentSmartObjectAuthoring::FDefinitionCapture RebuiltCapture;
	if (!UeAgentSmartObjectAuthoring::CaptureDefinition(*Rebuilt, RebuiltCapture, Error))
	{ AddError(Error.ToPublicDetail()); return false; }
	TestTrue(TEXT("recreated default behavior retains its nondefault StateTree reference"),
		CastChecked<UGameplayInteractionSmartObjectBehaviorDefinition>(RebuiltCapture.DefaultBehaviors[0])->GetStateTree() == BehaviorStateTree);
	// Delete native collections and recover the same asset; this exercises lookup
	// of existing owned objects as well as reconstruction of value-type storage.
	FindFProperty<FArrayProperty>(Definition->GetClass(), TEXT("Slots"))->ContainerPtrToValuePtr<TArray<FSmartObjectSlotDefinition>>(Definition)->Reset();
	Defaults.Reset(); Data.Reset(); Bindings.Reset();
	Bag.Reset();
	if (!UeAgentSmartObjectAuthoring::ApplyDefinitionDocument(*Definition, AfterAppliedDocument, Error))
	{ AddError(Error.ToPublicDetail()); return false; }
	TSharedPtr<FJsonValue> RestoredDocument;
	if (!UeAgentSmartObjectAuthoring::ReadDefinitionDocument(*Definition, RestoredDocument, Error))
	{ AddError(Error.ToPublicDetail()); return false; }
	TestEqual(TEXT("deleted native collections reconstruct on the same asset"),
		UeAgentAuthoringState::MakeStableCanonicalValueKey(RestoredDocument),
		UeAgentAuthoringState::MakeStableCanonicalValueKey(AfterAppliedDocument));
	return !HasAnyErrors();
}
