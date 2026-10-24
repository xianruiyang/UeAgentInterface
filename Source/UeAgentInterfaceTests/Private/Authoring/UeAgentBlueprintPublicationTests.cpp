#include "Misc/AutomationTest.h"

#include "Blueprint/BlueprintExtension.h"
#include "Blueprint/UserWidget.h"
#include "Blueprint/WidgetTree.h"
#include "Blueprint/WidgetBlueprintGeneratedClass.h"
#include "Domains/Blueprint/Authoring/UeAgentBlueprintAssetLifecycle.h"
#include "Engine/Blueprint.h"
#include "Engine/BlueprintGeneratedClass.h"
#include "Kismet2/KismetEditorUtilities.h"
#include "Misc/ScopeExit.h"
#include "UObject/Package.h"
#include "UObject/StrongObjectPtr.h"
#include "UObject/UObjectHash.h"
#include "UObject/UObjectGlobals.h"
#include "WidgetBlueprint.h"

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FUaiBlueprintPublicationFlagsTest,
	"UAI.Authoring.BlueprintPublication.FlagsAndDiagnostics",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FUaiBlueprintPublicationFlagsTest::RunTest(const FString&)
{
	using namespace UeAgentBlueprintAssetLifecycle;
	const FString Prefix = TEXT("/Temp/UAIBlueprintPublication/") + FGuid::NewGuid().ToString(EGuidFormats::Digits);
	TArray<TStrongObjectPtr<UPackage>> Packages;
	auto MakePackage = [&](const TCHAR* Suffix)
	{
		UPackage* Package = CreatePackage(*(Prefix + Suffix));
		Packages.Emplace(Package);
		return Package;
	};
	ON_SCOPE_EXIT
	{
		// These packages have never been saved. Keep their lifetime through native
		// compile/GC, then release only this fixture's objects for collection.
		for (const auto& Package : Packages)
		{
			Package->SetDirtyFlag(false);
			ForEachObjectWithPackage(Package.Get(), [](UObject* Object)
			{
				Object->ClearFlags(RF_Standalone);
				return true;
			}, true);
		}
		Packages.Reset();
		CollectGarbage(RF_NoFlags, true);
	};

	auto* Seed = Cast<UWidgetBlueprint>(FKismetEditorUtilities::CreateBlueprint(
		UUserWidget::StaticClass(), MakePackage(TEXT("_Seed")), TEXT("WBP_Seed"), BPTYPE_Normal,
		UWidgetBlueprint::StaticClass(), UWidgetBlueprintGeneratedClass::StaticClass()));
	if (!TestNotNull(TEXT("native widget seed"), Seed)
		|| !TestNotNull(TEXT("native source WidgetTree"), Seed->WidgetTree.Get())) return false;
	const FString Target = Prefix + TEXT("_Published.WBP_Published");
	SetAuthoringContext(*Seed, Target, nullptr);
	FDiagnostic Error;
	auto* Working = Cast<UWidgetBlueprint>(DuplicateBlueprint(
		Seed, MakePackage(TEXT("_Working")), TEXT("WBP_Working"), false, &Error));
	if (!TestNotNull(TEXT("working copy"), Working)) return false;
	SetAuthoringContext(*Working, Target, nullptr);
	TestTrue(TEXT("seed root transient restored"), Seed->HasAnyFlags(RF_Transient));
	TestFalse(TEXT("working tree did not inherit root transient"), Working->WidgetTree->HasAnyFlags(RF_Transient));
	auto* Published = Cast<UWidgetBlueprint>(PublishBlueprint(
		Working, MakePackage(TEXT("_Published")), TEXT("WBP_Published"), Error));
	if (!TestNotNull(TEXT("published widget"), Published)) return false;
	TestTrue(TEXT("working root transient restored"), Working->HasAnyFlags(RF_Transient));
	TestFalse(TEXT("published root persistent"), Published->HasAnyFlags(RF_Transient));
	TestFalse(TEXT("published tree persistent"), Published->WidgetTree->HasAnyFlags(RF_Transient));
	TestTrue(TEXT("published tree owned by source blueprint"), Published->WidgetTree->GetOuter() == Published);
	TestTrue(TEXT("empty widget tree is valid"), ValidateBlueprintPersistence(*Published, Error));

	UWidgetTree* Tree = Working->WidgetTree;
	Working->WidgetTree = nullptr;
	TestFalse(TEXT("missing required tree returns diagnostic"), ValidateBlueprintStructure(*Working, Error));
	TestEqual(TEXT("specific missing object"), Error.PropertyPath, FString(TEXT("WidgetTree")));
	TestEqual(TEXT("source target retained"), Error.TargetPath, Target);
	TestEqual(TEXT("missing tree code"), Error.Code, FString(TEXT("blueprint_required_object_missing")));
	Working->WidgetTree = Tree;
	Tree->SetFlags(RF_Transient);
	TestNull(TEXT("intentional transient required tree rejected before duplication"),
		PublishBlueprint(Working, MakePackage(TEXT("_Rejected")), TEXT("WBP_Rejected"), Error));
	TestEqual(TEXT("unsaveable tree code"), Error.Code, FString(TEXT("blueprint_required_object_transient")));
	TestTrue(TEXT("rejection does not silently repair child flags"), Tree->HasAnyFlags(RF_Transient));
	TestTrue(TEXT("rejected source root stays transient"), Working->HasAnyFlags(RF_Transient));
	Tree->ClearFlags(RF_Transient);

	UBlueprint* Plain = FKismetEditorUtilities::CreateBlueprint(
		UObject::StaticClass(), MakePackage(TEXT("_Plain")), TEXT("BP_Plain"), BPTYPE_Normal,
		UBlueprint::StaticClass(), UBlueprintGeneratedClass::StaticClass());
	if (!TestNotNull(TEXT("plain blueprint"), Plain)) return false;
	UBlueprintExtension* Extension = NewObject<UBlueprintExtension>(Plain, TEXT("OptionalTransient"), RF_Transient);
	Plain->AddExtension(Extension);
	SetAuthoringContext(*Plain, Prefix + TEXT("_PlainPublished.BP_PlainPublished"), nullptr);
	UBlueprint* PlainPublished = PublishBlueprint(
		Plain, MakePackage(TEXT("_PlainPublished")), TEXT("BP_PlainPublished"), Error);
	if (!TestNotNull(TEXT("plain publication"), PlainPublished)) return false;
	UBlueprintExtension* CopiedExtension = FindObject<UBlueprintExtension>(PlainPublished, TEXT("OptionalTransient"));
	if (TestNotNull(TEXT("native optional extension copied"), CopiedExtension))
		TestTrue(TEXT("optional transient intent preserved"), CopiedExtension->HasAnyFlags(RF_Transient));
	TestTrue(TEXT("optional transient is not a persistence error"), ValidateBlueprintPersistence(*PlainPublished, Error));
	return true;
}
