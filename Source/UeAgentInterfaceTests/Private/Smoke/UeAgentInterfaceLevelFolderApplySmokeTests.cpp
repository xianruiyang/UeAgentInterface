#if WITH_DEV_AUTOMATION_TESTS

#include "Utilities/UeAgentLevelFolderApply.h"
#include "Utilities/UeAgentLevelFolderExport.h"

#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Components/PointLightComponent.h"
#include "Engine/World.h"
#include "GameFramework/Actor.h"
#include "GameFramework/WorldSettings.h"
#include "HAL/FileManager.h"
#include "Misc/AutomationTest.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "Server/UeAgentCommandRegistry.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUeAgentLevelFolderApplyMinimalTest,
	"GptProjectTest.UeAgentInterface.Smoke.LevelFolderApply.Minimal",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

namespace
{
	bool WriteUtf8TextFile(const FString& FilePath, const FString& Text)
	{
		return FFileHelper::SaveStringToFile(Text, *FilePath, FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM);
	}

	TSharedPtr<FJsonObject> LoadJsonObjectForTest(const FString& FilePath)
	{
		FString Text;
		if (!FFileHelper::LoadFileToString(Text, *FilePath))
		{
			return nullptr;
		}
		TSharedPtr<FJsonObject> Object;
		const TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(Text);
		if (!FJsonSerializer::Deserialize(Reader, Object))
		{
			return nullptr;
		}
		return Object;
	}

	bool BuildSingleFieldDetailsPackage(
		const FString& DetailsRoot,
		const FString& OwnerKind,
		const FString& OwnerId,
		const FString& GroupFile,
		const FString& PropertyPath,
		const FString& ValueText)
	{
		if (!IFileManager::Get().MakeDirectory(*DetailsRoot, true))
		{
			return false;
		}

		const FString IndexJson = FString::Printf(
			TEXT("{\"schema\":\"uai.details_index.v1\",\"profile\":\"authoring\",\"owner_ref\":{\"owner_kind\":\"%s\",\"owner_id\":\"%s\"},\"groups\":[{\"file\":\"%s\"}]}\n"),
			*OwnerKind,
			*OwnerId,
			*GroupFile);
		const FString GroupJson = FString::Printf(
			TEXT("{\"schema\":\"uai.details_group.v1\",\"owner_ref\":{\"owner_kind\":\"%s\",\"owner_id\":\"%s\"},\"group_key\":\"Smoke\",\"display_name\":\"Smoke\",\"fields\":[{\"property_path\":\"%s\",\"value_text\":\"%s\",\"operations\":[\"set\"]}]}\n"),
			*OwnerKind,
			*OwnerId,
			*PropertyPath,
			*ValueText);

		return
			WriteUtf8TextFile(DetailsRoot / TEXT("index.json"), IndexJson) &&
			WriteUtf8TextFile(DetailsRoot / GroupFile, GroupJson);
	}

	bool BuildMinimalFolderPackage(const FString& Root)
	{
		IFileManager& FileManager = IFileManager::Get();
		if (!FileManager.MakeDirectory(*(Root / TEXT("actors") / TEXT("items") / TEXT("seed_actor")), true))
		{
			return false;
		}
		if (!FileManager.MakeDirectory(*(Root / TEXT("actors") / TEXT("items") / TEXT("seed_actor") / TEXT("components") / TEXT("seed_light_component")), true))
		{
			return false;
		}
		if (!FileManager.MakeDirectory(*(Root / TEXT("world_settings")), true))
		{
			return false;
		}

		const FString ActorJson =
			TEXT("{\n")
			TEXT("  \"schema\": \"uai.level_folder.v1\",\n")
			TEXT("  \"kind\": \"actor\",\n")
			TEXT("  \"actor_id\": \"seed_actor\",\n")
			TEXT("  \"label\": \"UAI_ApplyFolderSeedActor\",\n")
			TEXT("  \"class_path\": \"/Script/Engine.PointLight\",\n")
			TEXT("  \"object_path\": \"\",\n")
			TEXT("  \"folder_path\": \"UAI/ApplyFolderSmoke\"\n")
			TEXT("}\n");
		const FString ComponentJson =
			TEXT("{\n")
			TEXT("  \"schema\": \"uai.level_folder.v1\",\n")
			TEXT("  \"kind\": \"component\",\n")
			TEXT("  \"component_id\": \"seed_light_component\",\n")
			TEXT("  \"name\": \"PointLightComponent0\",\n")
			TEXT("  \"class_path\": \"/Script/Engine.PointLightComponent\",\n")
			TEXT("  \"object_path\": \"\"\n")
			TEXT("}\n");

		return
			WriteUtf8TextFile(Root / TEXT("manifest.json"), TEXT("{\"schema\":\"uai.level_folder.v1\"}\n")) &&
			WriteUtf8TextFile(Root / TEXT("actors") / TEXT("items") / TEXT("seed_actor") / TEXT("actor.json"), ActorJson) &&
			WriteUtf8TextFile(Root / TEXT("actors") / TEXT("items") / TEXT("seed_actor") / TEXT("components") / TEXT("seed_light_component") / TEXT("component.json"), ComponentJson) &&
			BuildSingleFieldDetailsPackage(
				Root / TEXT("world_settings") / TEXT("details"),
				TEXT("world_settings"),
				TEXT("world_settings"),
				TEXT("General.json"),
				TEXT("KillZ"),
				TEXT("-12345.0")) &&
			BuildSingleFieldDetailsPackage(
				Root / TEXT("actors") / TEXT("items") / TEXT("seed_actor") / TEXT("details"),
				TEXT("actor"),
				TEXT("seed_actor"),
				TEXT("General.json"),
				TEXT("bHidden"),
				TEXT("True")) &&
			BuildSingleFieldDetailsPackage(
				Root / TEXT("actors") / TEXT("items") / TEXT("seed_actor") / TEXT("components") / TEXT("seed_light_component") / TEXT("details"),
				TEXT("component"),
				TEXT("seed_light_component"),
				TEXT("Light.json"),
				TEXT("Intensity"),
				TEXT("7777.0"));
	}

	bool BuildLegacyPropertiesFolderPackage(const FString& Root, const FString& LegacyOwner)
	{
		if (!BuildMinimalFolderPackage(Root))
		{
			return false;
		}

		const FString LegacyProperties =
			TEXT("{\"schema\":\"uai.uobject_properties.v1\",\"properties\":[{\"property_path\":\"Intensity\",\"value_text\":\"123.0\"}]}\n");
		if (LegacyOwner == TEXT("actor"))
		{
			return WriteUtf8TextFile(Root / TEXT("actors") / TEXT("items") / TEXT("seed_actor") / TEXT("properties.json"), LegacyProperties);
		}
		if (LegacyOwner == TEXT("component"))
		{
			return WriteUtf8TextFile(Root / TEXT("actors") / TEXT("items") / TEXT("seed_actor") / TEXT("components") / TEXT("seed_light_component") / TEXT("properties.json"), LegacyProperties);
		}
		if (LegacyOwner == TEXT("world_settings"))
		{
			return WriteUtf8TextFile(Root / TEXT("world_settings") / TEXT("properties.json"), LegacyProperties);
		}
		return false;
	}
}

bool FUeAgentLevelFolderApplyMinimalTest::RunTest(const FString& Parameters)
{
	UeAgentCommandRegistry::FExactCommandInvoker Invoker = UeAgentCommandRegistry::FindExactCommandInvoker(TEXT("level_apply_folder"));
	TestTrue(TEXT("level_apply_folder registry entry exists"), Invoker != nullptr);

	const FString InputFolder = FPaths::ConvertRelativePathToFull(FPaths::ProjectSavedDir() / TEXT("Automation") / TEXT("UAI_LevelFolderApplySmoke_Input"));
	const FString OutputFolder = FPaths::ConvertRelativePathToFull(FPaths::ProjectSavedDir() / TEXT("Automation") / TEXT("UAI_LevelFolderApplySmoke_Output"));
	IFileManager::Get().DeleteDirectory(*InputFolder, false, true);
	IFileManager::Get().DeleteDirectory(*OutputFolder, false, true);
	TestTrue(TEXT("minimal folder package built"), BuildMinimalFolderPackage(InputFolder));

	UWorld* World = UWorld::CreateWorld(EWorldType::Editor, false, FName(TEXT("UAI_LevelFolderApplySmokeWorld")));
	TestNotNull(TEXT("world created"), World);
	if (!World)
	{
		return false;
	}

	UeAgentLevelFolderApply::FApplyOptions ApplyOptions;
	ApplyOptions.InputFolder = InputFolder;
	ApplyOptions.bCreateMissingActors = true;

	TSharedPtr<FJsonObject> ApplySummary;
	FString ApplyError;
	const bool bApplied = UeAgentLevelFolderApply::ApplyLevelFolder(World, ApplyOptions, ApplySummary, ApplyError);
	TestTrue(FString::Printf(TEXT("apply succeeds: %s"), *ApplyError), bApplied);
	TestTrue(TEXT("apply summary valid"), ApplySummary.IsValid());
	if (ApplySummary.IsValid())
	{
		TestEqual(TEXT("apply schema"), ApplySummary->GetStringField(TEXT("schema")), FString(TEXT("uai.level_folder_apply.v1")));
		TestEqual(TEXT("actor created count"), static_cast<int32>(ApplySummary->GetNumberField(TEXT("actors_created"))), 1);
		TestEqual(TEXT("actor skipped count"), static_cast<int32>(ApplySummary->GetNumberField(TEXT("actors_skipped"))), 0);
		TestEqual(TEXT("actor details applied count"), static_cast<int32>(ApplySummary->GetNumberField(TEXT("actor_properties_applied"))), 1);
		TestEqual(TEXT("component properties applied count"), static_cast<int32>(ApplySummary->GetNumberField(TEXT("component_properties_applied"))), 1);
		TestEqual(TEXT("world settings details applied count"), static_cast<int32>(ApplySummary->GetNumberField(TEXT("world_settings_properties_applied"))), 1);
	}

	AActor* AppliedActor = nullptr;
	for (ULevel* Level : World->GetLevels())
	{
		if (!Level)
		{
			continue;
		}
		for (AActor* Actor : Level->Actors)
		{
			if (IsValid(Actor) && Actor->GetActorLabel() == TEXT("UAI_ApplyFolderSeedActor"))
			{
				AppliedActor = Actor;
				break;
			}
		}
	}
	TestNotNull(TEXT("seed actor exists after apply"), AppliedActor);
	if (AppliedActor)
	{
		TestEqual(TEXT("folder path restored"), AppliedActor->GetFolderPath().ToString(), FString(TEXT("UAI/ApplyFolderSmoke")));
		TestTrue(TEXT("actor hidden patched from grouped details"), AppliedActor->IsHidden());
		UPointLightComponent* PointLightComponent = AppliedActor->FindComponentByClass<UPointLightComponent>();
		TestNotNull(TEXT("point light component exists"), PointLightComponent);
		if (PointLightComponent)
		{
			TestEqual(TEXT("point light intensity patched"), PointLightComponent->Intensity, 7777.0f);
		}
	}
	AWorldSettings* WorldSettings = World->GetWorldSettings(false, false);
	TestNotNull(TEXT("world settings exists"), WorldSettings);
	if (WorldSettings)
	{
		TestEqual(TEXT("world settings KillZ patched from grouped details"), WorldSettings->KillZ, -12345.0f);
	}

	UeAgentLevelFolderExport::FExportOptions ExportOptions;
	ExportOptions.OutputFolder = OutputFolder;
	ExportOptions.bOverwrite = true;

	TSharedPtr<FJsonObject> ExportSummary;
	FString ExportError;
	const bool bExported = UeAgentLevelFolderExport::ExportLevelFolder(World, ExportOptions, ExportSummary, ExportError);
	TestTrue(FString::Printf(TEXT("export succeeds after apply: %s"), *ExportError), bExported);

	TSharedPtr<FJsonObject> ActorIndex = LoadJsonObjectForTest(OutputFolder / TEXT("actors") / TEXT("index.json"));
	TestTrue(TEXT("exported actor index readable"), ActorIndex.IsValid());
	if (ActorIndex.IsValid())
	{
		const TArray<TSharedPtr<FJsonValue>>* Actors = nullptr;
		TestTrue(TEXT("actor index has actors array"), ActorIndex->TryGetArrayField(TEXT("actors"), Actors));
		bool bFoundAppliedActor = false;
		if (Actors)
		{
			for (const TSharedPtr<FJsonValue>& ActorValue : *Actors)
			{
				const TSharedPtr<FJsonObject> ActorObject = ActorValue.IsValid() ? ActorValue->AsObject() : nullptr;
				if (ActorObject.IsValid() && ActorObject->GetStringField(TEXT("label")) == TEXT("UAI_ApplyFolderSeedActor"))
				{
					bFoundAppliedActor = true;
					break;
				}
			}
		}
		TestTrue(TEXT("applied actor appears in exported actor index"), bFoundAppliedActor);
	}

	World->DestroyWorld(false);

	auto RunLegacyRejectionCase = [this](const FString& CaseName, const FString& LegacyOwner) -> bool
	{
		const FString LegacyInputFolder = FPaths::ConvertRelativePathToFull(
			FPaths::ProjectSavedDir() / TEXT("Automation") / FString::Printf(TEXT("UAI_LevelFolderApplySmoke_Legacy_%s"), *LegacyOwner));
		IFileManager::Get().DeleteDirectory(*LegacyInputFolder, false, true);
		if (!TestTrue(CaseName + TEXT(" legacy package built"), BuildLegacyPropertiesFolderPackage(LegacyInputFolder, LegacyOwner)))
		{
			return false;
		}

		UWorld* LegacyWorld = UWorld::CreateWorld(EWorldType::Editor, false, FName(*FString::Printf(TEXT("UAI_LevelFolderApplyLegacy_%s"), *LegacyOwner)));
		TestNotNull(CaseName + TEXT(" world created"), LegacyWorld);
		if (!LegacyWorld)
		{
			return false;
		}

		UeAgentLevelFolderApply::FApplyOptions LegacyApplyOptions;
		LegacyApplyOptions.InputFolder = LegacyInputFolder;
		LegacyApplyOptions.bCreateMissingActors = true;

		TSharedPtr<FJsonObject> LegacySummary;
		FString LegacyError;
		const bool bLegacyApplied = UeAgentLevelFolderApply::ApplyLevelFolder(LegacyWorld, LegacyApplyOptions, LegacySummary, LegacyError);
		TestFalse(CaseName + TEXT(" legacy properties rejected"), bLegacyApplied);
		TestEqual(CaseName + TEXT(" legacy error code"), LegacyError, FString(TEXT("legacy_properties_file_not_supported")));
		TestTrue(CaseName + TEXT(" legacy summary valid"), LegacySummary.IsValid());
		if (LegacySummary.IsValid())
		{
			TestEqual(CaseName + TEXT(" legacy schema"), LegacySummary->GetStringField(TEXT("schema")), FString(TEXT("uai.legacy_rejection.v1")));
			TestFalse(CaseName + TEXT(" legacy ok=false"), LegacySummary->GetBoolField(TEXT("ok")));
			TestEqual(CaseName + TEXT(" legacy surface"), LegacySummary->GetStringField(TEXT("legacy_surface")), FString(TEXT("properties.json")));
			TestEqual(CaseName + TEXT(" legacy owner kind"), LegacySummary->GetStringField(TEXT("owner_kind")), LegacyOwner);
			TestFalse(CaseName + TEXT(" legacy not dropped"), LegacySummary->GetBoolField(TEXT("dropped")));
		}

		LegacyWorld->DestroyWorld(false);
		return true;
	};

	RunLegacyRejectionCase(TEXT("actor"), TEXT("actor"));
	RunLegacyRejectionCase(TEXT("component"), TEXT("component"));
	RunLegacyRejectionCase(TEXT("world settings"), TEXT("world_settings"));
	return true;
}

#endif
