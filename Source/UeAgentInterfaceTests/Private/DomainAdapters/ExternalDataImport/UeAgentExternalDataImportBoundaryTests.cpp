#include "Misc/AutomationTest.h"

#include "Composition/DestructiveImport/UeAgentDestructiveImportActivation.h"

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUeAgentExternalDataImportBoundaryTest,
	"UeAgentInterface.DomainAdapters.ExternalDataImport.Boundary",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FUeAgentExternalDataImportBoundaryTest::RunTest(const FString& Parameters)
{
	(void)Parameters;
	TSharedPtr<const UE::UAI::CommandContract::
		FUeAgentCommandContractRegistry> Registry;
	FString Error;
	TestTrue(TEXT("formal destructive/import registry is created"),
		UE::UAI::Composition::DestructiveImport::
			TryCreateDestructiveImportProductionCommandContractRegistry(
				Registry, Error));
	const TArray<FString> CanonicalIds{
		TEXT("external.fbx.animation.import"),
		TEXT("external.fbx.skeletal_mesh.import"),
		TEXT("external.geometry_cache.import"),
		TEXT("external.texture.import"),
		TEXT("external.light_profile.import"),
		TEXT("external.sparse_volume_texture.import"),
		TEXT("external.sound_wave.import"),
		TEXT("external.localization.po.import"),
		TEXT("external.skin_weight_profile.import"),
	};
	for (const FString& CommandId : CanonicalIds)
	{
		TestNotNull(TEXT("canonical external command contract: ") + CommandId,
			Registry.IsValid() ? Registry->FindExact(CommandId) : nullptr);
	}
	return !HasAnyErrors();
}
