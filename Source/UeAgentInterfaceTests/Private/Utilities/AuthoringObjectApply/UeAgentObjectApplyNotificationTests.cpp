#include "Misc/AutomationTest.h"
#include "Utilities/AuthoringObjectApply/UeAgentAuthoringObjectApply.h"
#include "Utilities/AuthoringObjectApply/UeAgentTemplatePropertyPropagation.h"
#include "Misc/ScopeExit.h"
#include "Utilities/AuthoringObjectApply/UeAgentObjectApplyNotificationTestTypes.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Utilities/AuthoringCore/UeAgentAuthoringState.h"
#include "Utilities/UeAgentPropertyValueCodec.h"
#include "Core/HjsonWorkspace/UeAgentAuthoringComposition.h"
#include "Utilities/UeAgentStructuredHjsonFormatter.h"
#include "Containers/StringConv.h"
#include "Internationalization/TextPackageNamespaceUtil.h"
#include "UObject/MetaData.h"
#include "UObject/Package.h"
#include "UObject/UObjectGlobals.h"

namespace
{
	TSharedPtr<FJsonValue> Number(int32 Value)
	{
		auto Object = MakeShared<FJsonObject>();
		Object->SetNumberField(TEXT("literal"), Value);
		return MakeShared<FJsonValueObject>(Object);
	}
	TSharedPtr<FJsonValue> Descriptor(const FString& Path, TSharedPtr<FJsonValue> Value)
	{
		auto Object = MakeShared<FJsonObject>();
		Object->SetStringField(TEXT("property_path"), Path);
		Object->SetField(TEXT("value"), Value);
		return MakeShared<FJsonValueObject>(Object);
	}
	bool ProjectOrdinary(FAutomationTestBase& Test, UObject* Object, FName PropertyName, int32 ExpectedLeaves, TSharedPtr<FJsonValue>& Ordinary, TSharedPtr<FJsonValue>& Definition)
	{
	Ordinary.Reset(); Definition.Reset();
	using namespace UE::UAI::HjsonWorkspace;
	using namespace UE::UAI::ContextResource;
	FString Error;
	UeAgentAuthoringState::FEditablePropertyReadOptions ReadOptions;
	ReadOptions.IncludeProperty=[PropertyName](const FProperty* Property,const void*,const UObject*) { return Property->GetFName()==PropertyName; };
	TArray<UeAgentAuthoringState::FReadRequest> Reads;
	UeAgentAuthoringState::FCaptureResult Capture;
	if (!UeAgentAuthoringState::AppendEditableObjectPropertyReadRequests(Object,ReadOptions,Reads,Error)
		|| !UeAgentAuthoringState::CaptureSnapshot(Reads,Capture)) { Test.AddError(Error+Capture.Diagnostic.Code); return false; }
	auto Entries=Capture.Snapshot.CopyEntries();
	if (!Test.TestEqual(TEXT("one definition property capture"),Entries.Num(),1)) return false;
	Definition=Entries[0].ValueDefinition;
	if (!RegisterGraphTreeTargetCompositionContracts(Error) || !RegisterOtherTargetCompositionContracts(Error)) { Test.AddError(Error); return false; }
	FCompositionRequest Request; Request.TargetKind=TEXT("ue.level.asset"); Request.ProfileId=TEXT("llm_authoring"); Request.BaseRevision=TEXT("dynamic-definition:r1"); Request.SourceProvenance=TEXT("test"); Request.Entries=Entries;
	const TMap<EUeAgentResourceDimension,int64> Limits{{EUeAgentResourceDimension::InputBytes,4194304},{EUeAgentResourceDimension::OutputBytes,4194304},{EUeAgentResourceDimension::StringBytes,4194304},{EUeAgentResourceDimension::Nodes,16384},{EUeAgentResourceDimension::Items,16384},{EUeAgentResourceDimension::Files,4096},{EUeAgentResourceDimension::ReferenceEdges,4096},{EUeAgentResourceDimension::WorkUnits,65536},{EUeAgentResourceDimension::EstimatedMemoryBytes,33554432}};
	FUeAgentResourceTracker Resources;
	FAuthoringModelPtr Model; FWorkspaceError WorkspaceError; FExportWorkspacePlanPtr Plan;
	if (!TryComposeAuthoringModel(Request,Resources,Model,WorkspaceError)
		|| !TryBuildCompositionExportPlan(Model.ToSharedRef(),FCompositionSnapshotBinding(TEXT("snapshot:dynamic"),UE::UAI::StateSemantic::FUeAgentStateRevision(TEXT("test"),TEXT("r1"),1),TEXT("evidence:dynamic")),TMap<FString,int64>{},Resources,Plan,WorkspaceError)) { Test.AddError(WorkspaceError.GetCode()); return false; }
	FString Builtin;
	if (Definition.IsValid() && Definition->Type == EJson::Object
		&& Definition->AsObject()->TryGetStringField(TEXT("builtin"), Builtin) && Builtin == TEXT("property_bag"))
	{
		bool bRetained = false;
		for (const auto& Unit : Model->GetUnits())
			if (const auto* Value = Unit.GetValues().Find(TEXT("value_definition")); Value
				&& (*Value)->GetKind() == UE::UAI::StateSemantic::EUeAgentValueKind::Object)
				for (const auto& Field : (*Value)->GetObjectItems())
					bRetained |= Field.Key == TEXT("builtin") && Field.Value->GetKind() == UE::UAI::StateSemantic::EUeAgentValueKind::String
						&& Field.Value->GetText() == TEXT("property_bag");
		if (!Test.TestTrue(TEXT("composition retains the bag contract with or without nested definitions"), bRetained)) return false;
	}
	int32 LeafDefinitions=0;
	TFunction<void(const TSharedPtr<FJsonValue>&)> CountLeaves=[&](const TSharedPtr<FJsonValue>& Value) {
		if (Value->Type==EJson::Object) { const auto Obj=Value->AsObject(); if (Obj->HasField(TEXT("value_templates"))) ++LeafDefinitions; for(const auto& Field:Obj->Values) CountLeaves(Field.Value); }
		else if(Value->Type==EJson::Array) for(const auto& Item:Value->AsArray()) CountLeaves(Item);
	};
	for (const auto& File:Plan->GetFiles()) {
		if (!File.GetRelativePath().EndsWith(TEXT("/properties.hjson")) && !File.GetRelativePath().EndsWith(TEXT("/definitions.hjson"))) continue;
		FUTF8ToTCHAR Text(reinterpret_cast<const ANSICHAR*>(File.GetCanonicalUtf8().GetData()),File.GetCanonicalUtf8().Num());
		TSharedPtr<FJsonObject> Document;
		if (!FUeAgentStructuredHjsonParser::ParseObject(FString(Text.Length(),Text.Get()),File.GetRelativePath(),Document,Error)) { Test.AddError(Error); return false; }
		if (File.GetRelativePath().EndsWith(TEXT("/definitions.hjson"))) CountLeaves(MakeShared<FJsonValueObject>(Document));
		const TSharedPtr<FJsonObject>* Categories=nullptr;
		if(Document->TryGetObjectField(TEXT("categories"),Categories)) for(const auto& Category:(*Categories)->Values) {
			const TSharedPtr<FJsonObject>* Property=nullptr; if(Category.Value->AsObject()->TryGetObjectField(PropertyName.ToString(),Property)) Ordinary=(*Property)->TryGetField(TEXT("value"));
		}
	}
	Test.TestEqual(TEXT("public leaf definition count"),LeafDefinitions,ExpectedLeaves);
	return Ordinary.IsValid();
	}

}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FUeAgentTemplatePropagationTest,
    "UeAgentInterface.Utilities.AuthoringObjectApply.TemplatePropagation",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FUeAgentTemplatePropagationTest::RunTest(const FString& Parameters)
{
    using namespace UeAgentAuthoringObjectApply;
    auto* Template = GetMutableDefault<UUeAgentObjectApplyNotificationTarget>();
    const int32 OldValue = Template->Value;
    const auto OldSettings = Template->Settings;
    const int32 OldFixed0 = Template->Fixed[0], OldFixed1 = Template->Fixed[1];
    const bool OldDirty = Template->GetPackage()->IsDirty();
    UPackage* Package = CreatePackage(*(TEXT("/Temp/UAI/TemplatePropagation_") + FGuid::NewGuid().ToString(EGuidFormats::Digits)));
    auto* A = NewObject<UUeAgentObjectApplyNotificationTarget>(Package, TEXT("Inherited"), RF_Transactional);
    auto* B = NewObject<UUeAgentObjectApplyNotificationTarget>(Package, TEXT("Overridden"), RF_Transactional);
    B->Value = 77;
    B->Settings.Values = {8};
    B->Fixed[1] = 99;
    Package->SetDirtyFlag(false);
    UeAgentTemplatePropertyPropagation::FScope Scope;
    ON_SCOPE_EXIT {
        Template->Value = OldValue; Template->Settings = OldSettings;
        Template->Fixed[0] = OldFixed0; Template->Fixed[1] = OldFixed1;
        FString Ignored; Scope.Restore(Ignored);
        Template->GetPackage()->SetDirtyFlag(OldDirty);
        A->MarkAsGarbage(); B->MarkAsGarbage(); Package->SetDirtyFlag(false);
    };
    FOptions Options; Options.bCompactValues = true; Options.bAllowTemplateOwners = true;
    FPreparedApply Prepared; FFailure Failure; FApplyResult Result;
    const TArray<TSharedPtr<FJsonValue>> Input{
        Descriptor(TEXT("Value"), MakeShared<FJsonValueNumber>(12)),
        Descriptor(TEXT("Settings.Count"), MakeShared<FJsonValueNumber>(23)),
        Descriptor(TEXT("Fixed"), MakeShared<FJsonValueArray>(TArray<TSharedPtr<FJsonValue>>{
            MakeShared<FJsonValueNumber>(10), MakeShared<FJsonValueNumber>(12)}))};
    if (!TestTrue(TEXT("template prepare"), PrepareObject(Template, Input, Options, Prepared, Failure))) { AddError(Failure.Diagnostic.Code); return false; }
    if (!TestTrue(TEXT("template apply"), ApplyPrepared(Prepared, Result))) { AddError(Result.Diagnostic.Code); return false; }
    TestEqual(TEXT("inherited scalar updated"), A->Value, 12);
    TestEqual(TEXT("scalar override retained"), B->Value, 77);
    TestEqual(TEXT("nested field inherited despite sibling override"), B->Settings.Count, 23);
    TestTrue(TEXT("sibling override retained"), B->Settings.Values == TArray<int32>{8});
    TestTrue(TEXT("fixed array inherited"), A->Fixed[0] == 10 && A->Fixed[1] == 12);
    TestTrue(TEXT("fixed array override retained"), B->Fixed[0] == OldFixed0 && B->Fixed[1] == 99);
    TestTrue(TEXT("affected instance package dirty"), Package->IsDirty());
    if (!TestTrue(TEXT("template rollback"), RollbackPrepared(Prepared, Result))) { AddError(Result.Diagnostic.Code); return false; }
    TestEqual(TEXT("rollback restores inherited scalar"), A->Value, OldValue);
    TestEqual(TEXT("rollback restores nested value"), B->Settings.Count, OldSettings.Count);
    TestTrue(TEXT("rollback restores complete fixed array"), A->Fixed[0] == OldFixed0 && A->Fixed[1] == OldFixed1);
    TestFalse(TEXT("rollback restores instance package dirty"), Package->IsDirty());
    // A persistent reload may replace the instance object. Scope recovery must
    // resolve its original path rather than retaining the obsolete pointer.
    A->Rename(TEXT("Retired"), Package, REN_DontCreateRedirectors | REN_NonTransactional);
    auto* Rebound = NewObject<UUeAgentObjectApplyNotificationTarget>(Package, TEXT("Inherited"));
    Rebound->Fixed[0] = 100; Rebound->Fixed[1] = 200;
    FString Error;
    TestTrue(TEXT("scope recovery resolves replacement object"), Scope.Restore(Error));
    TestTrue(TEXT("scope restores both fixed-array elements"), Rebound->Fixed[0] == OldFixed0 && Rebound->Fixed[1] == OldFixed1);
    Rebound->MarkAsGarbage();
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FUeAgentObjectApplyTextIdentityTest,
	"UeAgentInterface.Utilities.AuthoringObjectApply.TextSourceIdentity",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FUeAgentObjectApplyTextIdentityTest::RunTest(const FString& Parameters)
{
	using namespace UeAgentAuthoringObjectApply;
	(void)Parameters;
	FOptions Options; Options.bCompactValues = true;
	Options.bEmitEditorNotifications = false;
	for (const bool bExistingNamespace : {false, true})
	{
		UPackage* Package = CreatePackage(*(TEXT("/Game/UAI/Transient/TextSourceDirect_") + FGuid::NewGuid().ToString(EGuidFormats::Digits)));
		auto* Actual = NewObject<UUeAgentObjectApplyNotificationTarget>(Package, TEXT("Actual"), RF_Transactional);
		if (bExistingNamespace) {
			const FString Namespace = TextNamespaceUtil::EnsurePackageNamespace(Package);
			Actual->Caption = FText::AsLocalizable_Advanced(TextNamespaceUtil::BuildFullNamespace(TEXT("author"), Namespace, true), TEXT("original-key"), TEXT("before"));
		}
		const FText Original = Actual->Caption;
		const auto MetadataBefore = Package->GetMetaData().RootMetaDataMap;
		Package->SetDirtyFlag(bExistingNamespace);
		TArray<TSharedPtr<FJsonValue>> Input{Descriptor(TEXT("Caption"), MakeShared<FJsonValueString>(TEXT("changed source")))};
		FPreparedApply Prepared; FFailure Failure; FApplyResult Result;
		if (!TestTrue(TEXT("text preparation succeeds"), PrepareObject(Actual, Input, Options, Prepared, Failure))) { AddError(Failure.Diagnostic.Code); return false; }
		TestTrue(TEXT("prepare does not touch native text"), Actual->Caption.IdenticalTo(Original));
		TestTrue(TEXT("prepare does not create or modify metadata"), Package->GetMetaData().RootMetaDataMap.OrderIndependentCompareEqual(MetadataBefore));
		TestEqual(TEXT("prepare retains dirty flag"), Package->IsDirty(), bExistingNamespace);
		if (!TestTrue(TEXT("source apply succeeds"), ApplyPrepared(Prepared, Result))) { AddError(Result.Diagnostic.Code); return false; }
		TestEqual(TEXT("source content is exact"), Actual->Caption.ToString(), FString(TEXT("changed source")));
		TestFalse(TEXT("source remains localizable"), Actual->Caption.IsCultureInvariant());
		TestFalse(TEXT("identity is resolved before any save"), FTextInspector::GetTextId(Actual->Caption).GetKey().IsEmpty());
		if (bExistingNamespace) TestTrue(TEXT("native unique-reference source edit retains identity"), FTextInspector::GetTextId(Actual->Caption) == FTextInspector::GetTextId(Original));
		TestTrue(TEXT("prepared verification accepts exact desired text"), VerifyPrepared(Prepared, Result));
		Actual->Caption = FText::ChangeKey(TEXT("wrong-namespace"), TEXT("wrong-key"), FText::FromString(TEXT("changed source")));
		TestFalse(TEXT("same source with incorrect identity is rejected"), VerifyPrepared(Prepared, Result));
		TestTrue(TEXT("rollback after failed verification succeeds"), RollbackPrepared(Prepared, Result));
		TestTrue(TEXT("rollback restores exact original text"), Actual->Caption.IdenticalTo(Original));
		TestTrue(TEXT("rollback restores exact metadata presence and values"), Package->GetMetaData().RootMetaDataMap.OrderIndependentCompareEqual(MetadataBefore));
		TestEqual(TEXT("rollback restores dirty flag"), Package->IsDirty(), bExistingNamespace);
		Actual->MarkAsGarbage();
		Package->SetDirtyFlag(false);
	}
	UPackage* RealPackage = CreatePackage(*(TEXT("/Game/UAI/Transient/TextSourceTransfer_") + FGuid::NewGuid().ToString(EGuidFormats::Digits)));
	auto* Actual = NewObject<UUeAgentObjectApplyNotificationTarget>(RealPackage, TEXT("Actual"), RF_Transactional);
	auto* Proposed = NewObject<UUeAgentObjectApplyNotificationTarget>();
	const auto Before = RealPackage->GetMetaData().RootMetaDataMap;
	TArray<TSharedPtr<FJsonValue>> Input{Descriptor(TEXT("Caption"), MakeShared<FJsonValueString>(TEXT("one resolved identity"))),
		Descriptor(TEXT("Captions"), MakeShared<FJsonValueArray>(TArray<TSharedPtr<FJsonValue>>{MakeShared<FJsonValueString>(TEXT("nested text"))}))};
	auto Owned = MakeShared<FJsonObject>();
	Owned->SetArrayField(TEXT("properties"), {Descriptor(TEXT("Caption"), MakeShared<FJsonValueString>(TEXT("new owned source")))});
	auto OwnedValue = MakeShared<FJsonObject>(); OwnedValue->SetObjectField(TEXT("owned_object"), Owned);
	Input.Add(Descriptor(TEXT("Child"), MakeShared<FJsonValueObject>(OwnedValue)));
	const TSharedPtr<FJsonValue> SharedLiteral = MakeShared<FJsonValueString>(TEXT("shared input at distinct positions"));
	Input.Add(Descriptor(TEXT("FixedCaptions"), MakeShared<FJsonValueArray>(TArray<TSharedPtr<FJsonValue>>{
		SharedLiteral, SharedLiteral})));
	FPreparedApply ProposedApply; FFailure Failure; FApplyResult Result;
	if (!TestTrue(TEXT("proposed prepare succeeds"), PrepareObject(Proposed, Input, Options, ProposedApply, Failure))) return false;
	TestTrue(TEXT("scratch owned preparation leaves real metadata unchanged"), RealPackage->GetMetaData().RootMetaDataMap.OrderIndependentCompareEqual(Before));
	ProposedApply.SetTextIdentityPackage(RealPackage);
	if (!TestTrue(TEXT("proposed resolves text using real package"), ApplyPrepared(ProposedApply, Result))) { AddError(Result.Diagnostic.Code); return false; }
	const auto Resolved = ProposedApply.GetResolvedPropertyDescriptors();
	FOptions Strict = Options; Strict.bCompactValues = false;
	// Non-text container envelopes stay in the original public shape, so use
	// compact completion while the resolved text leaves are already strict.
	FPreparedApply ActualApply;
	if (!TestTrue(TEXT("actual prepare consumes the frozen text descriptors"), PrepareObject(Actual, Resolved, Options, ActualApply, Failure))) { AddError(Failure.Diagnostic.Code); return false; }
	TestFalse(TEXT("actual has no source identity to regenerate"), ActualApply.HasTextSourceEdits());
	if (!TestTrue(TEXT("actual apply succeeds"), ApplyPrepared(ActualApply, Result))) return false;
	TestTrue(TEXT("proposed and actual share the exact text identity"), FTextInspector::GetTextId(Proposed->Caption) == FTextInspector::GetTextId(Actual->Caption));
	TestTrue(TEXT("recursive array text shares exact resolved identity"), Proposed->Captions.Num() == 1 && Actual->Captions.Num() == 1
		&& FTextInspector::GetTextId(Proposed->Captions[0]) == FTextInspector::GetTextId(Actual->Captions[0]));
	for (int32 Index = 0; Index < 2; ++Index)
		TestTrue(TEXT("fixed native array elements retain their individually resolved identities"),
			FTextInspector::GetTextId(Proposed->FixedCaptions[Index]) == FTextInspector::GetTextId(Actual->FixedCaptions[Index])
			&& !FTextInspector::GetTextId(Actual->FixedCaptions[Index]).GetKey().IsEmpty());
	TestTrue(TEXT("fixed native array elements have distinct identities"), FTextInspector::GetTextId(Actual->FixedCaptions[0]) != FTextInspector::GetTextId(Actual->FixedCaptions[1]));
	for (const FName Name : {FName(TEXT("Caption")), FName(TEXT("Captions")), FName(TEXT("FixedCaptions"))})
	{
		TSharedPtr<FJsonValue> Ordinary, Definition;
		if (!ProjectOrdinary(*this, Actual, Name, 0, Ordinary, Definition)) return false;
		if (Name == TEXT("Caption")) TestTrue(TEXT("ordinary scalar text exposes only source"),
			Ordinary->Type == EJson::String && Ordinary->AsString() == Actual->Caption.ToString());
		else
		{
			TestTrue(TEXT("ordinary recursive text array exposes source strings"), Ordinary->Type == EJson::Array);
			if (Ordinary->Type == EJson::Array) for (const auto& Item : Ordinary->AsArray()) TestTrue(TEXT("text item is a source string"), Item->Type == EJson::String);
		}
	}
	TestTrue(TEXT("new owned text resolves after materialization and transfers exact identity"), Proposed->Child && Actual->Child
		&& Actual->Child->GetOuter() == Actual && Proposed->Child->GetOuter() == Proposed
		&& FTextInspector::GetTextId(Proposed->Child->Caption) == FTextInspector::GetTextId(Actual->Child->Caption)
		&& !FTextInspector::GetTextId(Actual->Child->Caption).GetKey().IsEmpty());
	Actual->Caption = FText::ChangeKey(FTextInspector::GetTextId(Proposed->Caption).GetNamespace().ToString(),
		FTextInspector::GetTextId(Proposed->Caption).GetKey().ToString(), FText::FromString(TEXT("wrong source")));
	TestFalse(TEXT("incorrect source is rejected"), VerifyPrepared(ActualApply, Result));
	RollbackPrepared(ActualApply, Result);
	RollbackPrepared(ProposedApply, Result);
	TestTrue(TEXT("real package metadata is restored after transferred edits"), RealPackage->GetMetaData().RootMetaDataMap.OrderIndependentCompareEqual(Before));
	// Explicit localized input with unchanged source must not be erased by the
	// editor's display-string Identical shortcut.
	Actual->Caption = FText::ChangeKey(TEXT("explicit"), TEXT("before"), FText::FromString(TEXT("same source")));
	const FText ExplicitDesired = FText::ChangeKey(TEXT("explicit"), TEXT("after"), FText::FromString(TEXT("same source")));
	FTextProperty* TextProperty = FindFProperty<FTextProperty>(Actual->GetClass(), TEXT("Caption"));
	TArray<TSharedPtr<FJsonValue>> ExplicitInput{Descriptor(TEXT("Caption"), UeAgentPropertyValueCodec::Encode(TextProperty, &ExplicitDesired, Actual, {}))};
	FPreparedApply ExplicitApply;
	if (!TestTrue(TEXT("explicit identity edit prepares"), PrepareObject(Actual, ExplicitInput, Strict, ExplicitApply, Failure))) return false;
	TestEqual(TEXT("explicit same-source identity edit is not a no-op"), ExplicitApply.NumProperties(), 1);
	TestTrue(TEXT("explicit identity applies"), ApplyPrepared(ExplicitApply, Result));
	TestTrue(TEXT("explicit identity is exact"), FTextInspector::GetTextId(Actual->Caption) == FTextInspector::GetTextId(ExplicitDesired));
	RollbackPrepared(ExplicitApply, Result);
	Actual->MarkAsGarbage(); Proposed->MarkAsGarbage(); RealPackage->SetDirtyFlag(false);
	return !HasAnyErrors();
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FUeAgentObjectApplyNotificationTest,
	"UeAgentInterface.Utilities.AuthoringObjectApply.NativeNotifications",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FUeAgentObjectApplyCompactTest,
	"UeAgentInterface.Utilities.AuthoringObjectApply.CompactCompletion",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FUeAgentPropertyBagTextSourceIdentityTest,
	"UeAgentInterface.Utilities.AuthoringObjectApply.PropertyBagTextSourceIdentity",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FUeAgentPropertyBagTextSourceIdentityTest::RunTest(const FString&)
{
	using namespace UeAgentAuthoringObjectApply;
	FOptions Options; Options.bCompactValues = true; Options.bEmitEditorNotifications = false;
	for (bool bExisting : {false, true})
	{
		auto* Package = CreatePackage(*(TEXT("/Game/UAI/Transient/BagText_") + FGuid::NewGuid().ToString(EGuidFormats::Digits)));
		auto* Actual = NewObject<UUeAgentObjectApplyNotificationTarget>(Package, TEXT("Actual"), RF_Transactional);
		auto* Proposed = NewObject<UUeAgentObjectApplyNotificationTarget>();
		if (bExisting) {
			Actual->CurveBag.AddProperty(TEXT("Caption"), EPropertyBagPropertyType::Text);
			Actual->CurveBag.SetValueText(TEXT("Caption"), FText::AsLocalizable_Advanced(
				TextNamespaceUtil::BuildFullNamespace(TEXT("author"), TextNamespaceUtil::EnsurePackageNamespace(Package), true),
				TEXT("original"), TEXT("before")));
		}
		const FInstancedPropertyBag Before = Actual->CurveBag;
		Proposed->CurveBag = Before;
		const auto MetadataBefore = Package->GetMetaData().RootMetaDataMap;
		Package->SetDirtyFlag(bExisting);
		auto SchemaEntry = MakeShared<FJsonObject>();
		SchemaEntry->SetStringField(TEXT("name"), TEXT("Caption")); SchemaEntry->SetStringField(TEXT("kind"), TEXT("text"));
		auto Fields = MakeShared<FJsonObject>(); Fields->SetStringField(TEXT("Caption"), TEXT("edited bag source"));
		auto Bag = MakeShared<FJsonObject>(); Bag->SetArrayField(TEXT("schema"), {MakeShared<FJsonValueObject>(SchemaEntry)});
		Bag->SetObjectField(TEXT("fields"), Fields);
		FPreparedApply ProposedApply; FFailure Failure; FApplyResult Result;
		if (!PrepareObject(Proposed, {Descriptor(TEXT("CurveBag"), MakeShared<FJsonValueObject>(Bag))},
			Options, ProposedApply, Failure)) { AddError(Failure.Diagnostic.Code); return false; }
		TestTrue(TEXT("bag fields enter deferred text identity preparation"), ProposedApply.HasTextSourceEdits());
		TestTrue(TEXT("bag preparation does not change target metadata"), Package->GetMetaData().RootMetaDataMap.OrderIndependentCompareEqual(MetadataBefore));
		ProposedApply.SetTextIdentityPackage(Package);
		if (!ApplyPrepared(ProposedApply, Result)) { AddError(Result.Diagnostic.Code); return false; }
		FPreparedApply ActualApply;
		if (!PrepareObject(Actual, ProposedApply.GetResolvedPropertyDescriptors(), Options, ActualApply, Failure))
		{ AddError(Failure.Diagnostic.Code); return false; }
		TestFalse(TEXT("resolved bag descriptors do not regenerate text identity"), ActualApply.HasTextSourceEdits());
		if (!ApplyPrepared(ActualApply, Result)) { AddError(Result.Diagnostic.Code); return false; }
		const auto Desired = Proposed->CurveBag.GetValueText(TEXT("Caption"));
		const auto Readback = Actual->CurveBag.GetValueText(TEXT("Caption"));
		if (!TestTrue(TEXT("native text parameter exists"), Desired.IsValid() && Readback.IsValid())) return false;
		TestTrue(TEXT("actual text has the exact target-package identity resolved on proposed state"),
			Readback.GetValue().ToString() == TEXT("edited bag source") && !Readback.GetValue().IsCultureInvariant()
			&& !FTextInspector::GetTextId(Readback.GetValue()).GetKey().IsEmpty()
			&& FTextInspector::GetTextId(Readback.GetValue()) == FTextInspector::GetTextId(Desired.GetValue()));
		if (bExisting) TestTrue(TEXT("existing native parameter binding identity survives"),
			Actual->CurveBag.FindPropertyDescByName(TEXT("Caption"))->ID == Before.FindPropertyDescByName(TEXT("Caption"))->ID);
		TestTrue(TEXT("bag verification accepts the prepared identity"), VerifyPrepared(ActualApply, Result));
		Actual->CurveBag.SetValueText(TEXT("Caption"), FText::ChangeKey(TEXT("wrong"), TEXT("wrong"), FText::FromString(TEXT("edited bag source"))));
		TestFalse(TEXT("same bag text source with wrong identity is rejected"), VerifyPrepared(ActualApply, Result));
		TestTrue(TEXT("actual bag rollback succeeds"), RollbackPrepared(ActualApply, Result));
		TestTrue(TEXT("proposed text metadata rollback succeeds"), RollbackPrepared(ProposedApply, Result));
		auto* BagProperty = FindFProperty<FProperty>(Actual->GetClass(), TEXT("CurveBag"));
		TestTrue(TEXT("complete original bag is restored"), UeAgentPropertyValueCodec::AreSemanticallyEquivalent(BagProperty,
			&Before, &Actual->CurveBag, Actual, {}));
		TestTrue(TEXT("target metadata restored"), Package->GetMetaData().RootMetaDataMap.OrderIndependentCompareEqual(MetadataBefore));
		TestEqual(TEXT("target dirty state restored"), Package->IsDirty(), bExisting);
		Actual->MarkAsGarbage(); Proposed->MarkAsGarbage(); Package->SetDirtyFlag(false);
	}
	return !HasAnyErrors();
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FUeAgentDynamicValueDefinitionTest,
	"UeAgentInterface.Utilities.AuthoringObjectApply.DynamicValueDefinition",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FUeAgentDynamicValueDefinitionTest::RunTest(const FString&)
{
	using namespace UeAgentAuthoringObjectApply;
	using namespace UE::UAI::HjsonWorkspace;
	using namespace UE::UAI::ContextResource;
	auto* Object=NewObject<UUeAgentObjectApplyNotificationTarget>();
	Object->DynamicRecords.SetNum(2);
	Object->DynamicRecords[0].InitializeAs<FUeAgentAlternateNotificationSettings>();
	Object->DynamicRecords[0].GetMutable<FUeAgentAlternateNotificationSettings>().Value=31;
	Object->DynamicRecords[1].InitializeAs<FUeAgentDynamicCurveSettings>();
	auto& Curves=Object->DynamicRecords[1].GetMutable<FUeAgentDynamicCurveSettings>();
	Curves.Low.EditorCurveData.AddKey(0.0f,0.2f); Curves.Low.EditorCurveData.AddKey(1.0f,0.8f);
	Curves.Low.EditorCurveData.SetDefaultValue(0.4f);
	Curves.High.EditorCurveData.AddKey(0.0f,0.7f);
	const FRichCurve OriginalLow=Curves.Low.EditorCurveData, OriginalHigh=Curves.High.EditorCurveData;
	TSharedPtr<FJsonValue> Ordinary,Definition;
	if (!ProjectOrdinary(*this,Object,TEXT("DynamicRecords"),1,Ordinary,Definition)) return false;
	if (!TestTrue(TEXT("actual array definitions exist"),Definition.IsValid())) return false;
	const auto& Definitions=Definition->AsObject()->GetArrayField(TEXT("items"));
	if (!TestEqual(TEXT("definitions retain array cardinality"),Definitions.Num(),2)) return false;
	TestTrue(TEXT("first dynamic element retains its editable field type"),
		Definitions[0]->AsObject()->GetObjectField(TEXT("fields"))->GetObjectField(TEXT("Value"))->HasTypedField<EJson::Object>(TEXT("type")));
	TestTrue(TEXT("second dynamic type has both actual curve fields"),Definitions[1]->AsObject()->GetObjectField(TEXT("fields"))->HasField(TEXT("Low")) && Definitions[1]->AsObject()->GetObjectField(TEXT("fields"))->HasField(TEXT("High")));
	if (!TestTrue(TEXT("ordinary dynamic array exists"),Ordinary.IsValid() && Ordinary->Type==EJson::Array && Ordinary->AsArray().Num()==2)) return false;
	const auto Fields=Ordinary->AsArray()[1]->AsObject()->GetObjectField(TEXT("fields"));
	for(const TCHAR* Name:{TEXT("Low"),TEXT("High")}) {
		const auto Curve=Fields->GetObjectField(Name);
		TestTrue(TEXT("business channels.value survives ordinary projection"),Curve->GetObjectField(TEXT("channels"))->HasTypedField<EJson::Object>(TEXT("value")));
	}
	auto* Target=NewObject<UUeAgentObjectApplyNotificationTarget>();
	FOptions Options; Options.bCompactValues=true; Options.bEmitEditorNotifications=false;
	FPreparedApply Prepared; FFailure Failure; FApplyResult Result;
	if (!PrepareObject(Target,{Descriptor(TEXT("DynamicRecords"),Ordinary)},Options,Prepared,Failure)) {
		auto Evidence=MakeShared<FJsonObject>(); Evidence->SetField(TEXT("ordinary"),Ordinary);
		FString Text,WriteError; FUeAgentStructuredHjsonFormatter::TryWriteObject(Evidence,Text,WriteError);
		AddError(Failure.Diagnostic.Code+TEXT(" ordinary=")+Text); return false;
	}
	if (!TestTrue(TEXT("ordinary dynamic completion applies"),ApplyPrepared(Prepared,Result))) return false;
	if (!TestEqual(TEXT("native heterogeneous array restored"),Target->DynamicRecords.Num(),2)) return false;
	TestTrue(TEXT("first dynamic type and value restored"),Target->DynamicRecords[0].GetScriptStruct()==FUeAgentAlternateNotificationSettings::StaticStruct() && Target->DynamicRecords[0].Get<FUeAgentAlternateNotificationSettings>().Value==31);
	if (!TestTrue(TEXT("second dynamic type restored"),Target->DynamicRecords[1].GetScriptStruct()==FUeAgentDynamicCurveSettings::StaticStruct())) return false;
	const auto& Actual=Target->DynamicRecords[1].Get<FUeAgentDynamicCurveSettings>();
	TestTrue(TEXT("complete low curve restored"),Actual.Low.EditorCurveData==OriginalLow);
	TestTrue(TEXT("complete high curve restored"),Actual.High.EditorCurveData==OriginalHigh);
	return !HasAnyErrors();
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FUeAgentMapValueDefinitionTest,
	"UeAgentInterface.Utilities.AuthoringObjectApply.MapValueDefinition",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FUeAgentMapValueDefinitionTest::RunTest(const FString&)
{
	using namespace UeAgentAuthoringObjectApply;
	using namespace UE::UAI::HjsonWorkspace;
	using namespace UE::UAI::ContextResource;
	auto* Object=NewObject<UUeAgentObjectApplyNotificationTarget>();
	FRuntimeFloatCurve Low,High;
	Low.EditorCurveData.AddKey(0.0f,0.2f); Low.EditorCurveData.AddKey(1.0f,0.8f); Low.EditorCurveData.SetDefaultValue(0.4f);
	High.EditorCurveData.AddKey(0.0f,0.7f); High.EditorCurveData.PreInfinityExtrap=RCCE_Linear;
	Object->CurveMap.Add(TEXT("Low"),Low); Object->CurveMap.Add(TEXT("High"),High);
	TSharedPtr<FJsonValue> Ordinary,Definition;
	if (!ProjectOrdinary(*this,Object,TEXT("CurveMap"),1,Ordinary,Definition)) return false;
	const auto& Definitions=Definition->AsObject()->GetObjectField(TEXT("fields"))->GetObjectField(TEXT("entries"))->GetArrayField(TEXT("items"));
	TestEqual(TEXT("definition entry cardinality"),Definitions.Num(),2);
	for(const auto& EntryDefinition:Definitions) TestTrue(TEXT("only value needs leaf definition"),EntryDefinition->AsObject()->GetObjectField(TEXT("fields"))->Values.Num()==1 && EntryDefinition->AsObject()->GetObjectField(TEXT("fields"))->HasField(TEXT("value")));
	if(!TestTrue(TEXT("ordinary map exists"),Ordinary.IsValid() && Ordinary->Type==EJson::Object)) return false;
	const auto& OrdinaryEntries=Ordinary->AsObject()->GetArrayField(TEXT("entries"));
	TestEqual(TEXT("ordinary map entry cardinality"),OrdinaryEntries.Num(),2);
	for(const auto& Entry:OrdinaryEntries) TestTrue(TEXT("map business channels.value survives"),Entry->AsObject()->GetObjectField(TEXT("value"))->GetObjectField(TEXT("channels"))->HasTypedField<EJson::Object>(TEXT("value")));
	auto* Target=NewObject<UUeAgentObjectApplyNotificationTarget>(); FOptions Options; Options.bCompactValues=true; Options.bEmitEditorNotifications=false;
	FPreparedApply Prepared; FFailure Failure; FApplyResult Result;
	if(!PrepareObject(Target,{Descriptor(TEXT("CurveMap"),Ordinary)},Options,Prepared,Failure)) { AddError(Failure.Diagnostic.Code); return false; }
	if(!TestTrue(TEXT("ordinary map completion applies"),ApplyPrepared(Prepared,Result))) return false;
	TestEqual(TEXT("complete map key inventory"),Target->CurveMap.Num(),2);
	const auto* ActualLow=Target->CurveMap.Find(TEXT("Low")); const auto* ActualHigh=Target->CurveMap.Find(TEXT("High"));
	TestTrue(TEXT("low key and full curve restored"),ActualLow && ActualLow->EditorCurveData==Low.EditorCurveData && !ActualLow->ExternalCurve);
	TestTrue(TEXT("high key and full curve restored"),ActualHigh && ActualHigh->EditorCurveData==High.EditorCurveData && !ActualHigh->ExternalCurve);
	return !HasAnyErrors();
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FUeAgentContainerValueDefinitionTest,
	"UeAgentInterface.Utilities.AuthoringObjectApply.ContainerValueDefinition",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FUeAgentPairedContainerDefinitionTest,
	"UeAgentInterface.Utilities.AuthoringObjectApply.PairedContainerDefinitions",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FUeAgentPairedContainerDefinitionTest::RunTest(const FString&)
{
	using namespace UeAgentAuthoringObjectApply;
	auto* Object = NewObject<UUeAgentPairedDefinitionTarget>();
	FInstancedStruct Plain, Curved;
	Plain.InitializeAs<FUeAgentAlternateNotificationSettings>();
	Plain.GetMutable<FUeAgentAlternateNotificationSettings>().Value = 31;
	Curved.InitializeAs<FUeAgentDynamicCurveSettings>();
	Curved.GetMutable<FUeAgentDynamicCurveSettings>().Low.EditorCurveData.AddKey(0.f, 0.25f);
	Curved.GetMutable<FUeAgentDynamicCurveSettings>().High.EditorCurveData.AddKey(1.f, 0.75f);
	Object->Nested.Map.Add(TEXT("Z"), Curved); Object->Nested.Map.Add(TEXT("A"), Plain);
	FUeAgentKeyedDynamicSettings Z, A;
	Z.Key = TEXT("Z"); Z.Payload = Curved; A.Key = TEXT("A"); A.Payload = Plain;
	Object->Nested.Set.Add(Z); Object->Nested.Set.Add(A);
	Object->Fixed[0] = Object->Nested; Object->Fixed[1] = Object->Nested;
	Object->Array.Add(Object->Nested); Object->Optional = Object->Nested;
	auto Signature = [&](const TSharedPtr<FJsonValue>& Value) {
		auto Holder = MakeShared<FJsonObject>(); Holder->SetField(TEXT("schema"), Value ? Value : MakeShared<FJsonValueNull>());
		FString Text, Error; FUeAgentStructuredHjsonFormatter::TryWriteObject(Holder, Text, Error); return Text;
	};
	auto CheckPairs = [&](const TSharedPtr<FJsonValue>& RawValue, const TSharedPtr<FJsonValue>& RawDefinition,
		const TSharedPtr<FJsonValue>& Value, const TSharedPtr<FJsonValue>& Definition) {
		for (const FString Container : {FString(TEXT("Map")), FString(TEXT("Set"))}) {
			const bool bMap = Container == TEXT("Map");
			auto GetItems = [&](const TSharedPtr<FJsonValue>& Root) -> TArray<TSharedPtr<FJsonValue>> {
				return Root->AsObject()->GetObjectField(TEXT("fields"))->GetObjectField(Container)->GetArrayField(bMap ? TEXT("entries") : TEXT("items"));
			};
			auto GetSchemas = [&](const TSharedPtr<FJsonValue>& Root) -> TArray<TSharedPtr<FJsonValue>> {
				const auto Schema = Root->AsObject()->GetObjectField(TEXT("fields"))->GetObjectField(Container)->GetObjectField(TEXT("value_definition"));
				return bMap ? Schema->GetObjectField(TEXT("fields"))->GetObjectField(TEXT("entries"))->GetArrayField(TEXT("items")) : Schema->GetArrayField(TEXT("items"));
			};
			auto Key = [&](const TSharedPtr<FJsonValue>& Item) {
				return (bMap ? Item->AsObject()->GetObjectField(TEXT("key")) : Item->AsObject()->GetObjectField(TEXT("fields"))->GetObjectField(TEXT("Key")))->GetStringField(TEXT("literal"));
			};
			const auto RawItems = GetItems(RawValue), Items = GetItems(Value);
			const auto RawSchemas = GetSchemas(RawDefinition), Schemas = GetSchemas(Definition);
			if (!TestEqual(TEXT("native two-item fixture"), RawItems.Num(), 2) || !TestEqual(TEXT("normalized two-item fixture"), Items.Num(), 2)
				|| !TestEqual(TEXT("two original definitions"), RawSchemas.Num(), 2) || !TestEqual(TEXT("two normalized definitions"), Schemas.Num(), 2)) return false;
			TestEqual(TEXT("raw native traversal starts with Z"), Key(RawItems[0]), FString(TEXT("Z")));
			TestEqual(TEXT("stable canonical sort actually moves A first"), Key(Items[0]), FString(TEXT("A")));
			TestNotEqual(TEXT("schemas are genuinely heterogeneous"), Signature(RawSchemas[0]), Signature(RawSchemas[1]));
			for (int32 Index = 0; Index < 2; ++Index) {
				const int32 Original = Key(Items[Index]) == TEXT("Z") ? 0 : 1;
				TestEqual(TEXT("sorted value retains its exact original schema"), Signature(Schemas[Index]), Signature(RawSchemas[Original]));
			}
		}
		return true;
	};
	for (const FName Name : {FName(TEXT("Nested")), FName(TEXT("Fixed")), FName(TEXT("Array")), FName(TEXT("Optional"))}) {
		FProperty* Property = FindFProperty<FProperty>(Object->GetClass(), Name);
		UeAgentPropertyValueCodec::FOptions Codec;
		Codec.bCaptureStructFieldDescriptors = true;
		Codec.StructFieldFilter = [](const UScriptStruct*, const FProperty* Field) { return UeAgentPropertyValueCodec::IsStructFieldAuthoringEditable(Field); };
		TSharedPtr<FJsonObject> RawSchema;
		const auto Raw = UeAgentPropertyValueCodec::Encode(Property, Property->ContainerPtrToValuePtr<void>(Object), Object, Codec, 0, &RawSchema);
		UeAgentAuthoringState::FEditablePropertyReadOptions ReadOptions;
		ReadOptions.IncludeProperty = [Name](const FProperty* Field, const void*, const UObject*) { return Field->GetFName() == Name; };
		TArray<UeAgentAuthoringState::FReadRequest> Reads; FString Error;
		UeAgentAuthoringState::FCaptureResult Capture;
		if (!UeAgentAuthoringState::AppendEditableObjectPropertyReadRequests(Object, ReadOptions, Reads, Error)
			|| !UeAgentAuthoringState::CaptureSnapshot(Reads, Capture)) { AddError(Error + Capture.Diagnostic.Code); return false; }
		const auto Entry = Capture.Snapshot.CopyEntries()[0];
		auto Unwrap = [&](const TSharedPtr<FJsonValue>& Value, const TSharedPtr<FJsonValue>& Schema,
			TArray<TSharedPtr<FJsonValue>>& Values, TArray<TSharedPtr<FJsonValue>>& Schemas) {
			if (Name == TEXT("Fixed") || Name == TEXT("Array")) {
				Values = Value->AsObject()->GetArrayField(TEXT("items")); Schemas = Schema->AsObject()->GetArrayField(TEXT("items"));
			} else if (Name == TEXT("Optional")) {
				Values.Add(Value->AsObject()->TryGetField(TEXT("value"))); Schemas.Add(Schema->AsObject()->GetObjectField(TEXT("fields"))->TryGetField(TEXT("value")));
			} else { Values.Add(Value); Schemas.Add(Schema); }
		};
		TArray<TSharedPtr<FJsonValue>> RawValues, RawDefinitions, Values, Definitions;
		Unwrap(Raw, MakeShared<FJsonValueObject>(RawSchema), RawValues, RawDefinitions); Unwrap(Entry.Value, Entry.ValueDefinition, Values, Definitions);
		for (int32 Index = 0; Index < Values.Num(); ++Index) if (!CheckPairs(RawValues[Index], RawDefinitions[Index], Values[Index], Definitions[Index])) return false;
		TSharedPtr<FJsonValue> Ordinary, Definition;
		if (!ProjectOrdinary(*this, Object, Name, 1, Ordinary, Definition)) return false;
		// This consumer creates containers on a different, initially empty object.
		// Captured sources are therefore migrated to explicit native-default creation.
		TArray<TSharedPtr<FJsonValue>> NewContainers;
		if (Name == TEXT("Fixed") || Name == TEXT("Array")) NewContainers = Ordinary->AsArray();
		else if (Name == TEXT("Optional")) NewContainers.Add(Ordinary->AsObject()->TryGetField(TEXT("value")));
		else NewContainers.Add(Ordinary);
		for (const auto& Container : NewContainers)
			for (const auto& Item : Container->AsObject()->GetObjectField(TEXT("fields"))->GetArrayField(TEXT("Set")))
				Item->AsObject()->SetField(TEXT("from"), MakeShared<FJsonValueNull>());
		auto* Target = NewObject<UUeAgentPairedDefinitionTarget>();
		Target->Nested.ReadOnly = 91; Target->Fixed[0].ReadOnly = 89; Target->Fixed[1].ReadOnly = 97;
		FOptions Options; Options.bCompactValues = true; Options.bEmitEditorNotifications = false;
		FPreparedApply Prepared; FFailure Failure; FApplyResult Result;
		if (!PrepareObject(Target, {Descriptor(Name.ToString(), Ordinary)}, Options, Prepared, Failure)) { AddError(Name.ToString() + TEXT(":") + Failure.Diagnostic.Code + TEXT(":") + Failure.Diagnostic.PropertyPath); return false; }
		if (!TestTrue(TEXT("ordinary heterogeneous containers apply"), ApplyPrepared(Prepared, Result))) return false;
		TArray<const FUeAgentPairedContainers*> Restored;
		if (Name == TEXT("Fixed")) { Restored.Add(&Target->Fixed[0]); Restored.Add(&Target->Fixed[1]); }
		else if (Name == TEXT("Array")) { if (!TestEqual(TEXT("array restored"), Target->Array.Num(), 1)) return false; Restored.Add(&Target->Array[0]); }
		else if (Name == TEXT("Optional")) { if (!TestTrue(TEXT("optional restored"), Target->Optional.IsSet())) return false; Restored.Add(&Target->Optional.GetValue()); }
		else Restored.Add(&Target->Nested);
		for (const auto* Container : Restored) {
			const int32 ExpectedReadOnly = Container == &Target->Nested ? 91 : Container == &Target->Fixed[0] ? 89 : Container == &Target->Fixed[1] ? 97 : 73;
			TestEqual(TEXT("sparse struct completion preserves each native base"), Container->ReadOnly, ExpectedReadOnly);
			TestEqual(TEXT("map inventory restored"), Container->Map.Num(), 2); TestEqual(TEXT("set inventory restored"), Container->Set.Num(), 2);
			auto CheckPayload = [&](FName Key, const FInstancedStruct& Payload) {
				if (Key == TEXT("A")) { if (!TestTrue(TEXT("A keeps its native type"), Payload.GetScriptStruct() == FUeAgentAlternateNotificationSettings::StaticStruct())) return false;
					return TestEqual(TEXT("A keeps its native value"), Payload.Get<FUeAgentAlternateNotificationSettings>().Value, 31); }
				if (!TestTrue(TEXT("Z keeps its native curve type"), Key == TEXT("Z") && Payload.GetScriptStruct() == FUeAgentDynamicCurveSettings::StaticStruct())) return false;
				const auto& Actual = Payload.Get<FUeAgentDynamicCurveSettings>(); const auto& Expected = Curved.Get<FUeAgentDynamicCurveSettings>();
				return TestTrue(TEXT("Z keeps both complete curves"), Actual.Low.EditorCurveData == Expected.Low.EditorCurveData && Actual.High.EditorCurveData == Expected.High.EditorCurveData);
			};
			for (const auto& Pair : Container->Map) if (!CheckPayload(Pair.Key, Pair.Value)) return false;
			for (const auto& Item : Container->Set) if (!CheckPayload(Item.Key, Item.Payload)) return false;
		}
		if (!TestTrue(TEXT("prepared container rollback succeeds"), RollbackPrepared(Prepared, Result))) return false;
		TestTrue(TEXT("rollback restores original empty container state"), Target->Nested.Map.IsEmpty() && Target->Nested.Set.IsEmpty()
			&& Target->Fixed[0].Map.IsEmpty() && Target->Fixed[0].Set.IsEmpty() && Target->Fixed[1].Map.IsEmpty() && Target->Fixed[1].Set.IsEmpty()
			&& Target->Array.IsEmpty() && !Target->Optional.IsSet());
		TestTrue(TEXT("rollback retains original readonly bases"), Target->Nested.ReadOnly == 91 && Target->Fixed[0].ReadOnly == 89 && Target->Fixed[1].ReadOnly == 97);
	}
	return !HasAnyErrors();
}

bool FUeAgentContainerValueDefinitionTest::RunTest(const FString&)
{
	using namespace UeAgentAuthoringObjectApply;
	auto* Object=NewObject<UUeAgentObjectApplyNotificationTarget>();
	FRuntimeFloatCurve Curve; Curve.EditorCurveData.AddKey(0.0f,0.3f); Curve.EditorCurveData.AddKey(1.0f,0.9f); Curve.EditorCurveData.SetDefaultValue(0.6f); Curve.EditorCurveData.PostInfinityExtrap=RCCE_Linear;
	Object->FixedCurves[0]=Curve; Object->FixedCurves[1]=Curve; Object->FixedCurves[1].EditorCurveData.SetDefaultValue(0.8f);
	FUeAgentKeyedCurve Entry; Entry.Key=TEXT("One"); Entry.Curve=Curve; Object->CurveSet.Add(Entry);
	Object->OptionalCurve=Curve;
	if (!TestTrue(TEXT("native bag property added"),Object->CurveBag.AddProperty(TEXT("Curve"),EPropertyBagPropertyType::Struct,FRuntimeFloatCurve::StaticStruct())==EPropertyBagAlterationResult::Success)
		|| !TestTrue(TEXT("native bag value set"),Object->CurveBag.SetValueStruct(TEXT("Curve"),Curve)==EPropertyBagResult::Success)) return false;
	TSharedPtr<FJsonValue> Ordinary,Definition;
	FOptions Options; Options.bCompactValues=true; Options.bEmitEditorNotifications=false;
	auto* Target=NewObject<UUeAgentObjectApplyNotificationTarget>();
	auto RoundTrip=[&](FName Name,int32 Leaves) {
		if(!ProjectOrdinary(*this,Object,Name,Leaves,Ordinary,Definition)) return false;
		FPreparedApply Prepared; FFailure Failure; FApplyResult Result;
		if(!PrepareObject(Target,{Descriptor(Name.ToString(),Ordinary)},Options,Prepared,Failure)) { AddError(Name.ToString()+TEXT(":")+Failure.Diagnostic.Code); return false; }
		return TestTrue(TEXT("ordinary container native apply"),ApplyPrepared(Prepared,Result));
	};
	if(!RoundTrip(TEXT("FixedCurves"),1)) return false;
	TestTrue(TEXT("both fixed native curves restored"),Target->FixedCurves[0].EditorCurveData==Object->FixedCurves[0].EditorCurveData && Target->FixedCurves[1].EditorCurveData==Object->FixedCurves[1].EditorCurveData);
	if(!RoundTrip(TEXT("CurveSet"),1)) return false;
	const auto* Actual=Target->CurveSet.Find(Entry);
	TestTrue(TEXT("set identity and full payload restored"),Target->CurveSet.Num()==1 && Actual && Actual->Curve.EditorCurveData==Curve.EditorCurveData && !Actual->Curve.ExternalCurve);
	if(!RoundTrip(TEXT("OptionalCurve"),1)) return false;
	TestTrue(TEXT("set optional full curve restored"),Target->OptionalCurve.IsSet() && Target->OptionalCurve->EditorCurveData==Curve.EditorCurveData);
	if(!RoundTrip(TEXT("CurveBag"),1)) return false;
	const auto BagCurve=Target->CurveBag.GetValueStruct<FRuntimeFloatCurve>(TEXT("Curve"));
	TestTrue(TEXT("bag full curve restored"),BagCurve.IsValid() && BagCurve.GetValue()->EditorCurveData==Curve.EditorCurveData);
	Object->CurveSet.Empty(); Object->OptionalCurve.Reset(); Object->CurveBag.Reset();
	for(FName Name:{FName(TEXT("CurveSet")),FName(TEXT("OptionalCurve")),FName(TEXT("CurveBag"))}) {
		if(!RoundTrip(Name,0)) return false;
		if (Name == TEXT("CurveBag"))
			TestTrue(TEXT("empty bag keeps its schema merge contract"), Definition.IsValid()
				&& Definition->AsObject()->GetStringField(TEXT("builtin")) == TEXT("property_bag"));
		else TestFalse(TEXT("empty or unset container has no value definition"),Definition.IsValid());
	}
	TestTrue(TEXT("empty set unset optional empty bag restore natively"),Target->CurveSet.IsEmpty() && !Target->OptionalCurve.IsSet() && !Target->CurveBag.IsValid());
	return !HasAnyErrors();
}

bool FUeAgentObjectApplyCompactTest::RunTest(const FString&)
{
	using namespace UeAgentAuthoringObjectApply;
	auto* Object = NewObject<UUeAgentObjectApplyNotificationTarget>(GetTransientPackage());
	auto MakeRecord = [](bool bCount, int32 Count, bool bValues)
	{
		auto Fields = MakeShared<FJsonObject>();
		if (bCount) Fields->SetNumberField(TEXT("Count"), Count);
		if (bValues) Fields->SetArrayField(TEXT("Values"), {MakeShared<FJsonValueNumber>(8), MakeShared<FJsonValueNumber>(9)});
		auto Record = MakeShared<FJsonObject>(); Record->SetObjectField(TEXT("fields"), Fields);
		return MakeShared<FJsonValueObject>(Record);
	};
	FOptions Options;
	FFailure Failure;
	FApplyResult Result;
	FPreparedApply Strict;
	auto EmptyToOne = MakeShared<FJsonValueArray>(TArray<TSharedPtr<FJsonValue>>{MakeRecord(false, 0, true)});
	TestFalse(TEXT("typed default rejects omitted array envelope"), PrepareObject(Object,
		{Descriptor(TEXT("Records"), EmptyToOne)}, Options, Strict, Failure));
	TestEqual(TEXT("typed rejection remains precise"), Failure.Diagnostic.Code, FString(TEXT("array_value_invalid")));
	TestTrue(TEXT("rejected prepare does not mutate"), Object->Records.IsEmpty());
	Options.bCompactValues = true;
	FPreparedApply Fixed;
	Object->FixedSizeValues = {4, 6};
	for (int32 Count : {0, 1, 3})
	{
		TArray<TSharedPtr<FJsonValue>> Values;
		for (int32 Index = 0; Index < Count; ++Index) Values.Add(MakeShared<FJsonValueNumber>(10 + Index));
		FPreparedApply Rejected;
		TestFalse(TEXT("EditFixedSize rejects membership changes during prepare"), PrepareObject(Object,
			{Descriptor(TEXT("FixedSizeValues"), MakeShared<FJsonValueArray>(Values))}, Options, Rejected, Failure));
		TestEqual(TEXT("EditFixedSize length failure is precise"), Failure.Diagnostic.Code,
			FString(TEXT("edit_fixed_size_array_length_mismatch")));
		TestTrue(TEXT("rejected count preserves native values"), Object->FixedSizeValues == TArray<int32>({4, 6}));
	}
	FPreparedApply SameCount;
	if (!TestTrue(TEXT("EditFixedSize permits existing element edits"), PrepareObject(Object,
		{Descriptor(TEXT("FixedSizeValues"), MakeShared<FJsonValueArray>(TArray<TSharedPtr<FJsonValue>>{
			MakeShared<FJsonValueNumber>(10), MakeShared<FJsonValueNumber>(12)}))}, Options, SameCount, Failure))) return false;
	if (!TestTrue(TEXT("EditFixedSize existing element apply"), ApplyPrepared(SameCount, Result))) return false;
	TestTrue(TEXT("EditFixedSize preserves count and writes requested values"), Object->FixedSizeValues == TArray<int32>({10, 12}));
	FOptions ResizeOptions = Options;
	ResizeOptions.AuthorizeArrayResize = [](const FProperty*) { return false; };
	auto ResizedValues = MakeShared<FJsonValueArray>(TArray<TSharedPtr<FJsonValue>>{MakeShared<FJsonValueNumber>(21)});
	FPreparedApply DeniedResize;
	TestFalse(TEXT("native resize denial retains fixed size protection"), PrepareObject(Object,
		{Descriptor(TEXT("FixedSizeValues"), ResizedValues)}, ResizeOptions, DeniedResize, Failure));
	ResizeOptions.AuthorizeArrayResize = [](const FProperty* Property) {
		return Property && Property->GetFName() == TEXT("FixedSizeValues");
	};
	FPreparedApply AllowedResize;
	if (!TestTrue(TEXT("explicit native resize grant prepares"), PrepareObject(Object,
		{Descriptor(TEXT("FixedSizeValues"), ResizedValues)}, ResizeOptions, AllowedResize, Failure))) return false;
	TestTrue(TEXT("resize preparation preserves native values"), Object->FixedSizeValues == TArray<int32>({10, 12}));
	if (!TestTrue(TEXT("authorized resize applies"), ApplyPrepared(AllowedResize, Result))) return false;
	TestTrue(TEXT("authorized resize has requested native membership"), Object->FixedSizeValues == TArray<int32>({21}));
	if (!TestTrue(TEXT("fixed array compact prepare"), PrepareObject(Object,
		{Descriptor(TEXT("Fixed"), MakeShared<FJsonValueArray>(TArray<TSharedPtr<FJsonValue>>{
			MakeShared<FJsonValueNumber>(10), MakeShared<FJsonValueNumber>(12)}))}, Options, Fixed, Failure)))
	{ AddError(Failure.Diagnostic.Code); return false; }
	if (!TestTrue(TEXT("fixed array compact apply"), ApplyPrepared(Fixed, Result))) return false;
	TestTrue(TEXT("fixed scalar elements restore individual wrappers"), Object->Fixed[0] == 10 && Object->Fixed[1] == 12);
	FPreparedApply First;
	if (!TestTrue(TEXT("empty array compact prepare"), PrepareObject(Object,
		{Descriptor(TEXT("Records"), EmptyToOne)}, Options, First, Failure))) { AddError(Failure.Diagnostic.Code); return false; }
	if (!TestTrue(TEXT("empty array apply"), ApplyPrepared(First, Result))) return false;
	if (!TestEqual(TEXT("one native record"), Object->Records.Num(), 1)) return false;
	TestEqual(TEXT("omitted scalar uses native initialized default"), Object->Records[0].Count, 3);
	TestTrue(TEXT("nested array scalar envelopes restored"), Object->Records[0].Values == TArray<int32>({8,9}));
	Object->Records[0].Count = 17;
	Object->Records[0].ReadOnly = 91;
	Object->Settings.ReadOnly = 73;
	TArray<UeAgentAuthoringState::FReadRequest> ReadRequests;
	UeAgentAuthoringState::FEditablePropertyReadOptions ReadOptions;
	ReadOptions.IncludeProperty = [](const FProperty* Property, const void*, const UObject*)
		{ return Property->GetFName() == TEXT("Settings") || Property->GetFName() == TEXT("Records"); };
	FString ReadError;
	if (!TestTrue(TEXT("mixed struct and array remain authoring eligible"), UeAgentAuthoringState::AppendEditableObjectPropertyReadRequests(Object, ReadOptions, ReadRequests, ReadError))) return false;
	if (!TestEqual(TEXT("both mixed properties are captured"), ReadRequests.Num(), 2)) return false;
	UeAgentAuthoringState::FCaptureResult Capture;
	if (!TestTrue(TEXT("editable capture succeeds"), UeAgentAuthoringState::CaptureSnapshot(ReadRequests, Capture))) { AddError(Capture.Diagnostic.Code); return false; }
	for (const auto& Entry : Capture.Snapshot.CopyEntries())
	{
		const bool bRecords = Entry.TargetIdentity.GetSelector(TEXT("property_path")) == TEXT("Records");
		const auto Fields = bRecords
			? Entry.Value->AsObject()->GetArrayField(TEXT("items"))[0]->AsObject()->GetObjectField(TEXT("fields"))
			: Entry.Value->AsObject()->GetObjectField(TEXT("fields"));
		TestTrue(TEXT("ordinary capture retains editable member"), Fields->HasField(TEXT("Count")));
		TestFalse(TEXT("ordinary capture excludes readonly member"), Fields->HasField(TEXT("ReadOnly")));
		if (bRecords)
		{
			const auto* Native = static_cast<const TArray<FUeAgentNotificationSettings>*>(Entry.NativeValue->GetValuePtr());
			TestEqual(TEXT("native snapshot retains full readonly array state"), (*Native)[0].ReadOnly, 91);
		}
		else TestEqual(TEXT("native snapshot retains full readonly struct state"), static_cast<const FUeAgentNotificationSettings*>(Entry.NativeValue->GetValuePtr())->ReadOnly, 73);
	}
	auto OneToTwo = MakeShared<FJsonValueArray>(TArray<TSharedPtr<FJsonValue>>{
		MakeRecord(false, 0, true), MakeRecord(true, 23, false)});
	FPreparedApply Second;
	if (!TestTrue(TEXT("existing array append prepare"), PrepareObject(Object,
		{Descriptor(TEXT("Records"), OneToTwo)}, Options, Second, Failure))) { AddError(Failure.Diagnostic.Code); return false; }
	if (!TestTrue(TEXT("existing array append apply"), ApplyPrepared(Second, Result))) return false;
	if (!TestEqual(TEXT("two native records"), Object->Records.Num(), 2)) return false;
	TestEqual(TEXT("existing omitted member preserves current"), Object->Records[0].Count, 17);
	TestEqual(TEXT("existing readonly member preserves current"), Object->Records[0].ReadOnly, 91);
	TestEqual(TEXT("new readonly member uses native default"), Object->Records[1].ReadOnly, 41);
	TestEqual(TEXT("new scalar decoded"), Object->Records[1].Count, 23);
	TestTrue(TEXT("new omitted array uses native default, not old element"), Object->Records[1].Values == TArray<int32>({1}));
	if (!TestTrue(TEXT("compact rollback"), RollbackPrepared(Second, Result))) return false;
	TestEqual(TEXT("rollback restores membership"), Object->Records.Num(), 1);
	TestEqual(TEXT("rollback restores current native value"), Object->Records[0].Count, 17);
	TestEqual(TEXT("rollback restores readonly native value"), Object->Records[0].ReadOnly, 91);
	auto ForbiddenFields = MakeShared<FJsonObject>(); ForbiddenFields->SetNumberField(TEXT("ReadOnly"), 5);
	auto Forbidden = MakeShared<FJsonObject>(); Forbidden->SetObjectField(TEXT("fields"), ForbiddenFields);
	FPreparedApply Rejected;
	TestFalse(TEXT("explicit readonly struct write remains rejected"), PrepareObject(Object,
		{Descriptor(TEXT("Settings"), MakeShared<FJsonValueObject>(Forbidden))}, Options, Rejected, Failure));
	TestEqual(TEXT("readonly rejection keeps native state"), Object->Settings.ReadOnly, 73);
	TestFalse(TEXT("explicit readonly array member write remains rejected"), PrepareObject(Object,
		{Descriptor(TEXT("Records"), MakeShared<FJsonValueArray>(TArray<TSharedPtr<FJsonValue>>{MakeShared<FJsonValueObject>(Forbidden)}))}, Options, Rejected, Failure));
	TestEqual(TEXT("array rejection keeps readonly state"), Object->Records[0].ReadOnly, 91);
	Object->Dynamic.InitializeAs<FUeAgentNotificationSettings>();
	Object->Dynamic.GetMutable<FUeAgentNotificationSettings>().ReadOnly = 99;
	ReadRequests.Reset();
	ReadOptions.IncludeProperty = [](const FProperty* Property, const void*, const UObject*) { return Property->GetFName() == TEXT("Dynamic"); };
	if (!UeAgentAuthoringState::AppendEditableObjectPropertyReadRequests(Object, ReadOptions, ReadRequests, ReadError)
		|| !TestEqual(TEXT("dynamic mixed property authoring eligible"), ReadRequests.Num(), 1)
		|| !UeAgentAuthoringState::CaptureSnapshot(ReadRequests, Capture)) { AddError(Capture.Diagnostic.Code); return false; }
	const auto DynamicEntries = Capture.Snapshot.CopyEntries();
	const auto DynamicFields = DynamicEntries[0].Value->AsObject()->GetObjectField(TEXT("fields"));
	TestFalse(TEXT("dynamic authoring value filters readonly"), DynamicFields->HasField(TEXT("ReadOnly")));
	TestTrue(TEXT("dynamic authoring value keeps editable"), DynamicFields->HasField(TEXT("Count")));
	TestEqual(TEXT("dynamic native snapshot keeps readonly"), static_cast<const FInstancedStruct*>(DynamicEntries[0].NativeValue->GetValuePtr())->Get<FUeAgentNotificationSettings>().ReadOnly, 99);
	auto DynamicInput = [](UScriptStruct* Type, const TCHAR* Field, int32 Value)
	{
		auto Fields = MakeShared<FJsonObject>(); Fields->SetNumberField(Field, Value);
		auto Input = MakeShared<FJsonObject>(); Input->SetStringField(TEXT("struct_path"), Type->GetPathName()); Input->SetObjectField(TEXT("fields"), Fields);
		return MakeShared<FJsonValueObject>(Input);
	};
	FPreparedApply SameType;
	if (!TestTrue(TEXT("same dynamic type prepare"), PrepareObject(Object, {Descriptor(TEXT("Dynamic"), DynamicInput(FUeAgentNotificationSettings::StaticStruct(), TEXT("Count"), 20))}, Options, SameType, Failure))
		|| !ApplyPrepared(SameType, Result)) { AddError(Failure.Diagnostic.Code); return false; }
	TestTrue(TEXT("same dynamic type preserves complete omitted state"), Object->Dynamic.Get<FUeAgentNotificationSettings>().Count == 20
		&& Object->Dynamic.Get<FUeAgentNotificationSettings>().ReadOnly == 99 && Object->Dynamic.Get<FUeAgentNotificationSettings>().Values == TArray<int32>({1}));
	FPreparedApply ChangedType;
	if (!TestTrue(TEXT("different dynamic type prepare"), PrepareObject(Object, {Descriptor(TEXT("Dynamic"), DynamicInput(FUeAgentAlternateNotificationSettings::StaticStruct(), TEXT("Value"), 25))}, Options, ChangedType, Failure))
		|| !ApplyPrepared(ChangedType, Result)) { AddError(Failure.Diagnostic.Code); return false; }
	TestTrue(TEXT("different dynamic type uses native readonly default"), Object->Dynamic.Get<FUeAgentAlternateNotificationSettings>().Value == 25 && Object->Dynamic.Get<FUeAgentAlternateNotificationSettings>().ReadOnly == 62);
	TestFalse(TEXT("explicit dynamic readonly write rejected"), PrepareObject(Object,
		{Descriptor(TEXT("Dynamic"), DynamicInput(FUeAgentAlternateNotificationSettings::StaticStruct(), TEXT("ReadOnly"), 4))}, Options, Rejected, Failure));
	TestEqual(TEXT("dynamic rejected write preserves readonly"), Object->Dynamic.Get<FUeAgentAlternateNotificationSettings>().ReadOnly, 62);
	if (!RollbackPrepared(ChangedType, Result)) return false;
	TestTrue(TEXT("dynamic rollback restores type and full old state"), Object->Dynamic.GetScriptStruct() == FUeAgentNotificationSettings::StaticStruct()
		&& Object->Dynamic.Get<FUeAgentNotificationSettings>().ReadOnly == 99 && Object->Dynamic.Get<FUeAgentNotificationSettings>().Count == 20);
	Object->Dynamic.InitializeAs<FUeAgentNestedDynamicSettings>();
	Object->Dynamic.GetMutable<FUeAgentNestedDynamicSettings>().Nested.InitializeAs<FUeAgentUnsupportedDynamicSettings>();
	const FProperty* DynamicProperty = FindFProperty<FProperty>(Object->GetClass(), TEXT("Dynamic"));
	TestFalse(TEXT("readonly nested dynamic unsupported codec member rejects capability"), UeAgentPropertyValueCodec::IsAuthoringSupported(DynamicProperty, &Object->Dynamic, ReadError));
	TestEqual(TEXT("unsupported nested dynamic cause"), ReadError, FString(TEXT("unsupported_property_type")));
	ReadRequests.Reset();
	if (!UeAgentAuthoringState::AppendEditableObjectPropertyReadRequests(Object, ReadOptions, ReadRequests, ReadError)) return false;
	TestTrue(TEXT("unsupported full dynamic state is not offered for authoring"), ReadRequests.IsEmpty());
	return !HasAnyErrors();
}

bool FUeAgentObjectApplyNotificationTest::RunTest(const FString&)
{
	using namespace UeAgentAuthoringObjectApply;
	auto* Object = NewObject<UUeAgentObjectApplyNotificationTarget>(GetTransientPackage());
	FOptions Options;
	FFailure Failure;
	FApplyResult Result;
	auto Prepare = [&](const FString& Path, TSharedPtr<FJsonValue> Value, FPreparedApply& Prepared)
	{
		const bool bResult = PrepareObject(Object, {Descriptor(Path, Value)}, Options, Prepared, Failure);
		if (!bResult) AddError(Failure.Diagnostic.Code + TEXT(":") + Failure.Diagnostic.PublicMessage);
		return bResult;
	};
	auto Check = [&](const FUeAgentObservedPropertyEvent& Event, const TCHAR* Member, const TCHAR* Leaf)
	{
		TestEqual(TEXT("actual UObject event member"), Event.Member, FName(Member));
		TestEqual(TEXT("actual UObject event leaf"), Event.Leaf, FName(Leaf));
		TestEqual(TEXT("whole value notification"), static_cast<int32>(Event.Change), static_cast<int32>(EPropertyChangeType::ValueSet));
		TestEqual(TEXT("no fabricated element index"), Event.ArrayIndex, INDEX_NONE);
	};
	FPreparedApply Top;
	if (!Prepare(TEXT("Value"), Number(9), Top) || !TestTrue(TEXT("top apply"), ApplyPrepared(Top, Result))) return false;
	if (!TestEqual(TEXT("one top event"), Object->Events.Num(), 1)) return false;
	Check(Object->Events[0], TEXT("Value"), TEXT("Value"));
	TestEqual(TEXT("event sees new native value"), Object->Events[0].ValueAtEvent, 9);
	if (!TestTrue(TEXT("top rollback"), RollbackPrepared(Top, Result))) return false;
	Check(Object->Events.Last(), TEXT("Value"), TEXT("Value"));
	TestEqual(TEXT("rollback event sees restored native value"), Object->Events.Last().ValueAtEvent, 2);

	Object->Events.Reset(); Object->PreLeaves.Reset();
	auto Fields = MakeShared<FJsonObject>();
	Fields->SetField(TEXT("Count"), Number(7));
	auto Array = MakeShared<FJsonObject>();
	Array->SetArrayField(TEXT("items"), {Number(4), Number(5)});
	Fields->SetObjectField(TEXT("Values"), Array);
	auto Settings = MakeShared<FJsonObject>(); Settings->SetObjectField(TEXT("fields"), Fields);
	FPreparedApply Whole;
	if (!Prepare(TEXT("Settings"), MakeShared<FJsonValueObject>(Settings), Whole)
		|| !TestTrue(TEXT("whole nested struct apply"), ApplyPrepared(Whole, Result))) return false;
	if (!TestEqual(TEXT("only two changed leaves notify, not the wrapper"), Object->Events.Num(), 2)) return false;
	TestTrue(TEXT("both scalar and array leaf are reported"),
		Object->Events.ContainsByPredicate([](const auto& Event) { return Event.Leaf == TEXT("Count"); })
		&& Object->Events.ContainsByPredicate([](const auto& Event) { return Event.Leaf == TEXT("Values"); }));
	TestEqual(TEXT("pre notification count matches post"), Object->PreLeaves.Num(), 2);
	for (const auto& Event : Object->Events)
	{
		Check(Event, TEXT("Settings"), Event.Leaf == TEXT("Count") ? TEXT("Count") : TEXT("Values"));
		TestEqual(TEXT("all fields copied before leaf notifications"), Event.CountAtEvent, 7);
		TestTrue(TEXT("whole array visible during notification"), Event.ArrayAtEvent == TArray<int32>({4,5}));
	}
	if (!TestTrue(TEXT("whole struct rollback"), RollbackPrepared(Whole, Result))) return false;
	TestEqual(TEXT("rollback emits same two leaf contracts"), Object->Events.Num(), 4);
	TestEqual(TEXT("struct restored"), Object->Settings.Count, 3);
	TestTrue(TEXT("array restored"), Object->Settings.Values == TArray<int32>({1}));
	for (int32 Index = 2; Index < Object->Events.Num(); ++Index)
	{
		const auto& Event = Object->Events[Index];
		Check(Event, TEXT("Settings"), Event.Leaf == TEXT("Count") ? TEXT("Count") : TEXT("Values"));
		TestEqual(TEXT("rollback event count"), Event.CountAtEvent, 3);
		TestTrue(TEXT("rollback event array"), Event.ArrayAtEvent == TArray<int32>({1}));
	}

	Object->Events.Reset();
	FPreparedApply Explicit;
	if (!Prepare(TEXT("Settings.Count"), Number(8), Explicit) || !ApplyPrepared(Explicit, Result)) return false;
	if (!TestEqual(TEXT("explicit child emits once"), Object->Events.Num(), 1)) return false;
	Check(Object->Events[0], TEXT("Settings"), TEXT("Count"));
	TestEqual(TEXT("explicit value changed"), Object->Settings.Count, 8);
	Object->Events.Reset();
	FPreparedApply NoChange;
	if (!Prepare(TEXT("Settings.Count"), Number(8), NoChange) || !ApplyPrepared(NoChange, Result)) return false;
	TestTrue(TEXT("unchanged value does not notify"), Object->Events.IsEmpty());
	Options.bEmitEditorNotifications = false;
	FPreparedApply Silent;
	if (!Prepare(TEXT("Value"), Number(12), Silent) || !ApplyPrepared(Silent, Result) || !RollbackPrepared(Silent, Result)) return false;
	TestTrue(TEXT("disabled apply and rollback do not notify"), Object->Events.IsEmpty());
	Options.bEmitEditorNotifications = true;
	Object->Child = NewObject<UUeAgentObjectApplyNotificationTarget>(Object);
	FPreparedApply Child;
	if (!Prepare(TEXT("Child.Value"), Number(11), Child) || !ApplyPrepared(Child, Result)) return false;
	TestTrue(TEXT("owned child does not notify parent"), Object->Events.IsEmpty());
	if (!TestEqual(TEXT("owned child receives event"), Object->Child->Events.Num(), 1)) return false;
	Check(Object->Child->Events[0], TEXT("Value"), TEXT("Value"));

	FUeAgentNotificationSettings Independent;
	FPreparedApply IndependentApply;
	if (!PrepareStruct(FUeAgentNotificationSettings::StaticStruct(), &Independent, Object,
		{Descriptor(TEXT("Count"), Number(10))}, Options, IndependentApply, Failure)
		|| !ApplyPrepared(IndependentApply, Result)) return false;
	TestEqual(TEXT("independent struct changed"), Independent.Count, 10);
	TestTrue(TEXT("independent struct cannot fabricate member chain"), Object->Events.IsEmpty());
	return !HasAnyErrors();
}
