#if WITH_DEV_AUTOMATION_TESTS

#include "Utilities/UeAgentLevelFolderValidate.h"

#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "HAL/FileManager.h"
#include "Misc/AutomationTest.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "Server/UeAgentCommandRegistry.h"

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUeAgentLevelFolderValidateSmokeTest,
	"GptProjectTest.UeAgentInterface.Smoke.LevelFolderValidate.Basic",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

namespace
{
	bool WriteUtf8TextFileForValidateTest(const FString& FilePath, const FString& Text)
	{
		return FFileHelper::SaveStringToFile(Text, *FilePath, FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM);
	}

	bool BuildValidateDetailsPackage(
		const FString& DetailsRoot,
		const FString& OwnerKind,
		const FString& OwnerId,
		const FString& GroupFile,
		const FString& FieldJson)
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
			TEXT("{\"schema\":\"uai.details_group.v1\",\"owner_ref\":{\"owner_kind\":\"%s\",\"owner_id\":\"%s\"},\"group_key\":\"Smoke\",\"display_name\":\"Smoke\",\"fields\":[%s]}\n"),
			*OwnerKind,
			*OwnerId,
			*FieldJson);

		return
			WriteUtf8TextFileForValidateTest(DetailsRoot / TEXT("index.json"), IndexJson) &&
			WriteUtf8TextFileForValidateTest(DetailsRoot / GroupFile, GroupJson);
	}

	bool BuildValidatePackage(const FString& Root, bool bValid)
	{
		IFileManager& FileManager = IFileManager::Get();
		if (!FileManager.MakeDirectory(*(Root / TEXT("actors") / TEXT("items") / TEXT("seed_actor") / TEXT("components") / TEXT("seed_light_component")), true) ||
			!FileManager.MakeDirectory(*(Root / TEXT("references")), true))
		{
			return false;
		}

		const FString ActorClass = bValid ? TEXT("/Script/Engine.PointLight") : TEXT("/Script/Engine.NotARealActor");
		const FString ComponentProperty = bValid ?
			TEXT("    {\"selector\":{\"property_name\":\"Intensity\"},\"display_name\":\"Intensity\",\"availability\":{\"enabled\":true},\"value_text\":\"1234.0\"}\n") :
			TEXT("    {\"property_path\":\"MissingProperty\",\"value_text\":\"1\"},\n")
			TEXT("    {\"property_path\":\"Intensity\"}\n");
		const FString ReferenceEntry = bValid ?
			TEXT("  \"references\": []\n") :
			TEXT("  \"references\": [\n")
			TEXT("    {\"target_path\":\"/Game/UAI/Missing/MissingAsset.MissingAsset\"}\n")
			TEXT("  ]\n");

		const FString ActorJson = FString::Printf(
			TEXT("{\"schema\":\"uai.level_folder.v1\",\"kind\":\"actor\",\"actor_id\":\"seed_actor\",\"label\":\"ValidateSeed\",\"class_path\":\"%s\",\"folder_path\":\"UAI/Validate\"}\n"),
			*ActorClass);
		const FString ComponentJson =
			TEXT("{\"schema\":\"uai.level_folder.v1\",\"kind\":\"component\",\"component_id\":\"seed_light_component\",\"name\":\"PointLightComponent0\",\"class_path\":\"/Script/Engine.PointLightComponent\"}\n");
		const FString ReferencesJson =
			TEXT("{\n")
			TEXT("  \"schema\":\"uai.level_folder.v1\",\n")
			TEXT("  \"kind\":\"asset_references\",\n") +
			ReferenceEntry +
			TEXT("}\n");

		return
			WriteUtf8TextFileForValidateTest(Root / TEXT("manifest.json"), TEXT("{\"schema\":\"uai.level_folder.v1\"}\n")) &&
			WriteUtf8TextFileForValidateTest(Root / TEXT("actors") / TEXT("items") / TEXT("seed_actor") / TEXT("actor.json"), ActorJson) &&
			WriteUtf8TextFileForValidateTest(Root / TEXT("actors") / TEXT("items") / TEXT("seed_actor") / TEXT("components") / TEXT("seed_light_component") / TEXT("component.json"), ComponentJson) &&
			WriteUtf8TextFileForValidateTest(Root / TEXT("references") / TEXT("assets.json"), ReferencesJson) &&
			BuildValidateDetailsPackage(
				Root / TEXT("actors") / TEXT("items") / TEXT("seed_actor") / TEXT("details"),
				TEXT("actor"),
				TEXT("seed_actor"),
				TEXT("Actor.json"),
				TEXT("")) &&
			BuildValidateDetailsPackage(
				Root / TEXT("actors") / TEXT("items") / TEXT("seed_actor") / TEXT("components") / TEXT("seed_light_component") / TEXT("details"),
				TEXT("component"),
				TEXT("seed_light_component"),
				TEXT("Component.json"),
				ComponentProperty);
	}

	bool BuildLegacyValidatePackage(const FString& Root)
	{
		IFileManager& FileManager = IFileManager::Get();
		if (!FileManager.MakeDirectory(*(Root / TEXT("actors") / TEXT("items") / TEXT("seed_actor")), true))
		{
			return false;
		}

		return
			WriteUtf8TextFileForValidateTest(Root / TEXT("manifest.json"), TEXT("{\"schema\":\"uai.level_folder.v1\"}\n")) &&
			WriteUtf8TextFileForValidateTest(
				Root / TEXT("actors") / TEXT("items") / TEXT("seed_actor") / TEXT("actor.json"),
				TEXT("{\"schema\":\"uai.level_folder.v1\",\"kind\":\"actor\",\"actor_id\":\"seed_actor\",\"class_path\":\"/Script/Engine.PointLight\",\"properties\":[]}\n")) &&
			WriteUtf8TextFileForValidateTest(
				Root / TEXT("actors") / TEXT("items") / TEXT("seed_actor") / TEXT("properties.json"),
				TEXT("{\"schema\":\"uai.uobject_properties.v1\",\"properties\":[]}\n"));
	}

	bool HasIssueCode(const TSharedPtr<FJsonObject>& Summary, const FString& Code)
	{
		const TArray<TSharedPtr<FJsonValue>>* Issues = nullptr;
		if (!Summary.IsValid() || !Summary->TryGetArrayField(TEXT("issues"), Issues) || !Issues)
		{
			return false;
		}
		for (const TSharedPtr<FJsonValue>& IssueValue : *Issues)
		{
			const TSharedPtr<FJsonObject> Issue = IssueValue.IsValid() ? IssueValue->AsObject() : nullptr;
			if (Issue.IsValid() && Issue->GetStringField(TEXT("code")) == Code)
			{
				return true;
			}
		}
		return false;
	}
}

bool FUeAgentLevelFolderValidateSmokeTest::RunTest(const FString& Parameters)
{
	UeAgentCommandRegistry::FExactCommandInvoker Invoker = UeAgentCommandRegistry::FindExactCommandInvoker(TEXT("level_validate_folder"));
	TestTrue(TEXT("level_validate_folder registry entry exists"), Invoker != nullptr);

	const FString ValidFolder = FPaths::ConvertRelativePathToFull(FPaths::ProjectSavedDir() / TEXT("Automation") / TEXT("UAI_LevelFolderValidateSmoke_Valid"));
	const FString InvalidFolder = FPaths::ConvertRelativePathToFull(FPaths::ProjectSavedDir() / TEXT("Automation") / TEXT("UAI_LevelFolderValidateSmoke_Invalid"));
	const FString LegacyFolder = FPaths::ConvertRelativePathToFull(FPaths::ProjectSavedDir() / TEXT("Automation") / TEXT("UAI_LevelFolderValidateSmoke_Legacy"));
	IFileManager::Get().DeleteDirectory(*ValidFolder, false, true);
	IFileManager::Get().DeleteDirectory(*InvalidFolder, false, true);
	IFileManager::Get().DeleteDirectory(*LegacyFolder, false, true);
	TestTrue(TEXT("valid package built"), BuildValidatePackage(ValidFolder, true));
	TestTrue(TEXT("invalid package built"), BuildValidatePackage(InvalidFolder, false));
	TestTrue(TEXT("legacy package built"), BuildLegacyValidatePackage(LegacyFolder));

	UeAgentLevelFolderValidate::FValidateOptions ValidOptions;
	ValidOptions.InputFolder = ValidFolder;
	ValidOptions.bCheckReferences = true;
	TSharedPtr<FJsonObject> ValidSummary;
	FString ValidError;
	TestTrue(TEXT("valid package validates"), UeAgentLevelFolderValidate::ValidateLevelFolder(ValidOptions, ValidSummary, ValidError));
	TestTrue(TEXT("valid summary ok"), ValidSummary.IsValid() && ValidSummary->GetBoolField(TEXT("ok")));
	TestEqual(TEXT("valid package has no errors"), static_cast<int32>(ValidSummary->GetNumberField(TEXT("errors"))), 0);

	UeAgentLevelFolderValidate::FValidateOptions InvalidOptions;
	InvalidOptions.InputFolder = InvalidFolder;
	InvalidOptions.bCheckReferences = true;
	TSharedPtr<FJsonObject> InvalidSummary;
	FString InvalidError;
	TestTrue(TEXT("invalid package returns validation summary"), UeAgentLevelFolderValidate::ValidateLevelFolder(InvalidOptions, InvalidSummary, InvalidError));
	TestTrue(TEXT("invalid summary not ok"), InvalidSummary.IsValid() && !InvalidSummary->GetBoolField(TEXT("ok")));
	TestTrue(TEXT("invalid actor class detected"), HasIssueCode(InvalidSummary, TEXT("invalid_actor_class")));
	TestTrue(TEXT("missing property detected"), HasIssueCode(InvalidSummary, TEXT("property_not_found")));
	TestTrue(TEXT("missing patch value detected"), HasIssueCode(InvalidSummary, TEXT("missing_patch_value")));
	TestTrue(TEXT("missing reference detected"), HasIssueCode(InvalidSummary, TEXT("reference_target_not_loaded_or_missing")));

	UeAgentLevelFolderValidate::FValidateOptions LegacyOptions;
	LegacyOptions.InputFolder = LegacyFolder;
	TSharedPtr<FJsonObject> LegacySummary;
	FString LegacyError;
	TestTrue(TEXT("legacy package returns validation summary"), UeAgentLevelFolderValidate::ValidateLevelFolder(LegacyOptions, LegacySummary, LegacyError));
	TestTrue(TEXT("legacy summary not ok"), LegacySummary.IsValid() && !LegacySummary->GetBoolField(TEXT("ok")));
	TestTrue(TEXT("legacy inline properties rejected"), HasIssueCode(LegacySummary, TEXT("legacy_inline_properties_not_supported")));
	TestTrue(TEXT("legacy properties file rejected"), HasIssueCode(LegacySummary, TEXT("legacy_properties_file_not_supported")));

	return true;
}

#endif
