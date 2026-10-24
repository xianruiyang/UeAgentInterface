#include "Misc/AutomationTest.h"

#include "AssetRegistry/AssetRegistryModule.h"
#include "Curves/CurveFloat.h"
#include "DomainAdapters/AssetDestructiveImport/UeAgentAssetDeletePreflight.h"
#include "Editor.h"
#include "Engine/World.h"
#include "Misc/Guid.h"
#include "Misc/PackageName.h"
#include "UObject/Package.h"

using namespace UE::UAI::DomainAdapters::AssetDestructiveImport;

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUeAgentAssetDeletePreflightSelectionTest,
	"UeAgentInterface.DomainAdapters.AssetDestructiveImport.DeletePreflight.Selection",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FUeAgentAssetDeletePreflightSelectionTest::RunTest(
	const FString& Parameters)
{
	(void)Parameters;
	const FString AssetName = TEXT("PreflightAsset");
	const FString PackageName = TEXT("/Game/UAI_Automation/T164_")
		+ FGuid::NewGuid().ToString(EGuidFormats::Digits);
	UPackage* const Package = CreatePackage(*PackageName);
	UCurveFloat* const Asset = Package
		? NewObject<UCurveFloat>(
			Package, *AssetName, RF_Public | RF_Standalone)
		: nullptr;
	if (!TestNotNull(TEXT("a fresh registry fixture is created"), Asset))
	{
		return false;
	}
	FAssetRegistryModule::AssetCreated(Asset);

	FUeAgentAssetDeleteSelectionRequest Request;
	Request.Targets = {Asset->GetPathName()};
	const FUeAgentAssetDeletePreflightResult Preflight =
		PrepareAssetDeleteSelection(Request);
	TestTrue(TEXT("a valid registry selection reaches target preflight"),
		Preflight.IsSuccess());
	TestEqual(TEXT("the selection identity is preserved"),
		Preflight.Targets.Num(), 1);
	if (Preflight.Targets.Num() == 1)
	{
		TestEqual(TEXT("the object path is preserved"),
			Preflight.Targets[0].ObjectPath, Asset->GetPathName());
		TestEqual(TEXT("a transient package has a stable missing-file blocker"),
			Preflight.Targets[0].BlockerCode,
			FString(TEXT("asset_delete_target_missing")));
	}

	const UWorld* const CurrentEditorWorld = GEditor
		? GEditor->GetEditorWorldContext().World()
		: nullptr;
	FString CurrentWorldFilename;
	if (CurrentEditorWorld
		&& FPackageName::DoesPackageExist(
			CurrentEditorWorld->GetOutermost()->GetName(),
			&CurrentWorldFilename))
	{
		FUeAgentAssetDeleteSelectionRequest CurrentWorldRequest;
		CurrentWorldRequest.Targets = {CurrentEditorWorld->GetPathName()};
		const FUeAgentAssetDeletePreflightResult CurrentWorldPreflight =
			PrepareAssetDeleteSelection(CurrentWorldRequest);
		TestTrue(TEXT("the current persisted Editor world reaches preflight"),
			CurrentWorldPreflight.IsSuccess());
		TestEqual(TEXT("the current world is represented once"),
			CurrentWorldPreflight.Targets.Num(), 1);
		if (CurrentWorldPreflight.Targets.Num() == 1)
		{
			TestEqual(TEXT("the current world has a typed open-world blocker"),
				CurrentWorldPreflight.Targets[0].BlockerCode,
				FString(TEXT("asset_delete_current_world_open")));
		}
	}

	FAssetRegistryModule::AssetDeleted(Asset);
	Asset->ClearFlags(RF_Public | RF_Standalone);
	Asset->MarkAsGarbage();
	return !HasAnyErrors();
}
