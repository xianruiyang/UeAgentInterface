#include "Misc/AutomationTest.h"

#include "DomainAdapters/AssetDestructiveImport/UeAgentAssetDeleteOwner.h"

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUeAgentAssetDestructiveImportBoundaryTest,
	"UeAgentInterface.DomainAdapters.AssetDestructiveImport.ModuleBoundary",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FUeAgentAssetDestructiveImportBoundaryTest::RunTest(
	const FString& Parameters)
{
	(void)Parameters;
	TSharedPtr<UE::UAI::DomainAdapters::AssetDestructiveImport::
		IUeAgentAssetDeleteOwner> Owner;
	FString Error;
	TestFalse(
		TEXT("domain owner rejects an empty durability/authority configuration"),
		UE::UAI::DomainAdapters::AssetDestructiveImport::TryCreateAssetDeleteOwner(
			FString(), FString(), Owner, Error));
	TestFalse(TEXT("invalid configuration creates no owner"), Owner.IsValid());
	TestEqual(
		TEXT("invalid configuration has a typed failure"),
		Error,
		FString(TEXT("asset_delete_owner_config_invalid")));
	return !HasAnyErrors();
}
