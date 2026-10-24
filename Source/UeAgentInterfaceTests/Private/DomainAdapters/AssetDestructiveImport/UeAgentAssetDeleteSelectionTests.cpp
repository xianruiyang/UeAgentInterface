#include "Misc/AutomationTest.h"

#include "AssetRegistry/AssetRegistryModule.h"
#include "Curves/CurveFloat.h"
#include "DomainAdapters/AssetDestructiveImport/UeAgentAssetDeleteSelection.h"
#include "Misc/Guid.h"
#include "UObject/Package.h"

using namespace UE::UAI::DomainAdapters::AssetDestructiveImport;

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUeAgentAssetDeleteSelectionValidationTest,
	"UeAgentInterface.DomainAdapters.AssetDestructiveImport.DeleteSelection.Validation",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FUeAgentAssetDeleteSelectionValidationTest::RunTest(
	const FString& Parameters)
{
	(void)Parameters;
	const FUeAgentAssetDeleteSelectionResult Empty =
		ResolveAssetDeleteSelection({});
	TestEqual(
		TEXT("an empty target list has a stable error code"),
		Empty.Diagnostic.Code,
		FString(TEXT("asset_delete_selection_targets_empty")));

	FUeAgentAssetDeleteSelectionRequest InvalidRequest;
	InvalidRequest.Targets = {TEXT("Game/NotALongPackageName")};
	const FUeAgentAssetDeleteSelectionResult Invalid =
		ResolveAssetDeleteSelection(InvalidRequest);
	TestEqual(
		TEXT("an invalid reference has a stable error code"),
		Invalid.Diagnostic.Code,
		FString(TEXT("asset_delete_selection_ref_invalid")));
	return !HasAnyErrors();
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUeAgentAssetDeleteSelectionRegistryTest,
	"UeAgentInterface.DomainAdapters.AssetDestructiveImport.DeleteSelection.Registry",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FUeAgentAssetDeleteSelectionRegistryTest::RunTest(
	const FString& Parameters)
{
	(void)Parameters;
	const FString Root = TEXT("/Game/UAI_Automation/T164_")
		+ FGuid::NewGuid().ToString(EGuidFormats::Digits);
	const FString SiblingRoot = Root + TEXT("_Sibling");
	TArray<UCurveFloat*> Fixtures;
	auto AddFixture = [this, &Fixtures](
		const FString& PackageName,
		const TCHAR* AssetName)
	{
		UPackage* const Package = CreatePackage(*PackageName);
		UCurveFloat* const Asset = Package
			? NewObject<UCurveFloat>(
				Package, AssetName, RF_Public | RF_Standalone)
			: nullptr;
		if (TestNotNull(TEXT("a fresh registry fixture is created"), Asset))
		{
			FAssetRegistryModule::AssetCreated(Asset);
			Fixtures.Add(Asset);
		}
		return Asset;
	};

	UCurveFloat* const DirectA = AddFixture(Root + TEXT("/DirectA"), TEXT("DirectA"));
	UCurveFloat* const DirectB = AddFixture(Root + TEXT("/DirectB"), TEXT("DirectB"));
	UCurveFloat* const Nested = AddFixture(
		Root + TEXT("/Nested/NestedAsset"), TEXT("NestedAsset"));
	UCurveFloat* const Deeper = AddFixture(
		Root + TEXT("/Nested/Deeper/DeepAsset"), TEXT("DeepAsset"));
	UCurveFloat* const Sibling = AddFixture(
		SiblingRoot + TEXT("/SiblingAsset"), TEXT("SiblingAsset"));

	if (DirectA && DirectB && Nested && Deeper && Sibling)
	{
		FUeAgentAssetDeleteSelectionRequest MultipleRequest;
		MultipleRequest.Targets = {
			DirectB->GetPathName(), DirectA->GetOutermost()->GetName()};
		const FUeAgentAssetDeleteSelectionResult Multiple =
			ResolveAssetDeleteSelection(MultipleRequest);
		TestTrue(TEXT("multiple exact assets resolve"), Multiple.IsSuccess());
		TestEqual(TEXT("multiple exact assets remain distinct"),
			Multiple.Items.Num(), 2);

		FUeAgentAssetDeleteSelectionRequest DirectFolderRequest;
		DirectFolderRequest.Targets = {Root + TEXT("/")};
		const FUeAgentAssetDeleteSelectionResult DirectFolder =
			ResolveAssetDeleteSelection(DirectFolderRequest);
		TestTrue(TEXT("a direct folder resolves"), DirectFolder.IsSuccess());
		TestEqual(TEXT("non-recursive selection includes only direct assets"),
			DirectFolder.Items.Num(), 2);

		FUeAgentAssetDeleteSelectionRequest RecursiveRequest;
		RecursiveRequest.Targets = {Root};
		RecursiveRequest.bRecursive = true;
		const FUeAgentAssetDeleteSelectionResult Recursive =
			ResolveAssetDeleteSelection(RecursiveRequest);
		TestTrue(TEXT("a recursive folder resolves"), Recursive.IsSuccess());
		TestEqual(TEXT("recursive selection includes all descendants"),
			Recursive.Items.Num(), 4);
		TestFalse(TEXT("a similarly prefixed sibling stays outside the scope"),
			Recursive.Items.ContainsByPredicate([Sibling](
				const FUeAgentAssetDeleteSelectionItem& Item)
			{
				return Item.ObjectPath == Sibling->GetPathName();
			}));

		FUeAgentAssetDeleteSelectionRequest OverlapRequest;
		OverlapRequest.Targets = {DirectA->GetPathName(), Root};
		OverlapRequest.bRecursive = true;
		const FUeAgentAssetDeleteSelectionResult Overlap =
			ResolveAssetDeleteSelection(OverlapRequest);
		TestTrue(TEXT("an explicit and folder overlap resolves"),
			Overlap.IsSuccess());
		TestEqual(TEXT("overlapping selection is deduplicated"),
			Overlap.Items.Num(), 4);
		const FUeAgentAssetDeleteSelectionItem* const OverlappedItem =
			Overlap.Items.FindByPredicate([DirectA](
				const FUeAgentAssetDeleteSelectionItem& Item)
			{
				return Item.ObjectPath == DirectA->GetPathName();
			});
		TestTrue(TEXT("overlap preserves both input sources"),
			OverlappedItem && OverlappedItem->Sources.Num() == 2);
	}

	for (UCurveFloat* const Fixture : Fixtures)
	{
		FAssetRegistryModule::AssetDeleted(Fixture);
		Fixture->ClearFlags(RF_Public | RF_Standalone);
		Fixture->MarkAsGarbage();
	}
	return !HasAnyErrors();
}
