#include "Misc/AutomationTest.h"

#include "Composition/DestructiveImport/UeAgentDestructiveImportActivation.h"
#include "Server/UeAgentCommandRegistry.h"

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
	const TArray<FString> LegacyIds{
		TEXT("asset_import_fbx_animation"),
		TEXT("asset_import_fbx_skeletal_mesh"),
		TEXT("asset_import_geometry_cache"),
		TEXT("asset_import_texture"),
		TEXT("light_profile_import"),
		TEXT("sparse_volume_texture_import"),
		TEXT("sound_wave_import"),
		TEXT("localization_import_po"),
		TEXT("skeletal_mesh_import_skin_weight_profile"),
	};
	for (const FString& LegacyId : LegacyIds)
	{
		TestNull(TEXT("legacy exact external owner is absent: ") + LegacyId,
			UeAgentCommandRegistry::FindExactCommandInvoker(LegacyId));
		TestNull(TEXT("legacy prefix external owner is absent: ") + LegacyId,
			UeAgentCommandRegistry::FindPrefixedCommandInvoker(LegacyId));
		TestTrue(TEXT("legacy external route fails closed: ") + LegacyId,
			UeAgentCommandRegistry::IsExplicitlyUnsupportedCommand(LegacyId));
	}
	return !HasAnyErrors();
}
