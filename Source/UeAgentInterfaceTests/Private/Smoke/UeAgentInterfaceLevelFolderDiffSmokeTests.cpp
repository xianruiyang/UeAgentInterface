#if WITH_DEV_AUTOMATION_TESTS

#include "Utilities/UeAgentLevelFolderDiff.h"

#include "Dom/JsonObject.h"
#include "HAL/FileManager.h"
#include "Misc/AutomationTest.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "Server/UeAgentCommandRegistry.h"

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUeAgentLevelFolderDiffSmokeTest,
	"GptProjectTest.UeAgentInterface.Smoke.LevelFolderDiff.Basic",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

namespace
{
	bool WriteUtf8TextFileForDiffTest(const FString& FilePath, const FString& Text)
	{
		return FFileHelper::SaveStringToFile(Text, *FilePath, FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM);
	}

	bool BuildDiffDetailsPackage(
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
			WriteUtf8TextFileForDiffTest(DetailsRoot / TEXT("index.json"), IndexJson) &&
			WriteUtf8TextFileForDiffTest(DetailsRoot / GroupFile, GroupJson);
	}

	bool BuildDiffPackage(const FString& Root, bool bTarget)
	{
		IFileManager& FileManager = IFileManager::Get();
		if (!FileManager.MakeDirectory(*(Root / TEXT("actors") / TEXT("items") / TEXT("seed_actor") / TEXT("components") / TEXT("seed_light_component")), true) ||
			!FileManager.MakeDirectory(*(Root / TEXT("references")), true))
		{
			return false;
		}

		const FString Label = bTarget ? TEXT("DiffTargetActor") : TEXT("DiffBaseActor");
		const FString ActorPropertyValue = bTarget ? TEXT("20") : TEXT("10");
		const FString ComponentName = bTarget ? TEXT("PointLightComponent0") : TEXT("PointLightComponentBase");
		const FString ReferenceTarget = bTarget ? TEXT("/Game/UAI/TargetAsset.TargetAsset") : TEXT("/Game/UAI/BaseAsset.BaseAsset");

		const FString ActorJson = FString::Printf(
			TEXT("{\"schema\":\"uai.level_folder.v1\",\"kind\":\"actor\",\"actor_id\":\"seed_actor\",\"label\":\"%s\",\"class_path\":\"/Script/Engine.PointLight\"}\n"),
			*Label);
		const FString ComponentJson = FString::Printf(
			TEXT("{\"schema\":\"uai.level_folder.v1\",\"kind\":\"component\",\"component_id\":\"seed_light_component\",\"name\":\"%s\",\"class_path\":\"/Script/Engine.PointLightComponent\"}\n"),
			*ComponentName);
		const FString ComponentFields = bTarget ?
			TEXT("{\"selector\":{\"property_name\":\"Intensity\"},\"value_text\":\"777\",\"operations\":[\"set\"]},{\"property_path\":\"LightColor\",\"value_text\":\"(R=1,G=1,B=1,A=1)\",\"operations\":[\"set\"]}") :
			TEXT("{\"property_path\":\"Intensity\",\"value_text\":\"111\",\"operations\":[\"set\"]}");
		const FString ReferencesJson = FString::Printf(
			TEXT("{\"schema\":\"uai.level_folder.v1\",\"kind\":\"asset_references\",\"references\":[{\"target_path\":\"%s\"}]}\n"),
			*ReferenceTarget);
		const FString LevelJson = bTarget ?
			TEXT("{\"schema\":\"uai.level_folder.v1\",\"kind\":\"level\",\"name\":\"Target\"}\n") :
			TEXT("{\"schema\":\"uai.level_folder.v1\",\"kind\":\"level\",\"name\":\"Base\"}\n");

		return
			WriteUtf8TextFileForDiffTest(Root / TEXT("manifest.json"), TEXT("{\"schema\":\"uai.level_folder.v1\"}\n")) &&
			WriteUtf8TextFileForDiffTest(Root / TEXT("level.json"), LevelJson) &&
			WriteUtf8TextFileForDiffTest(Root / TEXT("actors") / TEXT("items") / TEXT("seed_actor") / TEXT("actor.json"), ActorJson) &&
			WriteUtf8TextFileForDiffTest(Root / TEXT("actors") / TEXT("items") / TEXT("seed_actor") / TEXT("components") / TEXT("seed_light_component") / TEXT("component.json"), ComponentJson) &&
			WriteUtf8TextFileForDiffTest(Root / TEXT("references") / TEXT("assets.json"), ReferencesJson) &&
			BuildDiffDetailsPackage(
				Root / TEXT("actors") / TEXT("items") / TEXT("seed_actor") / TEXT("details"),
				TEXT("actor"),
				TEXT("seed_actor"),
				TEXT("Actor.json"),
				FString::Printf(TEXT("{\"property_path\":\"Tags\",\"value_text\":\"%s\",\"operations\":[\"set\"]}"), *ActorPropertyValue)) &&
			BuildDiffDetailsPackage(
				Root / TEXT("actors") / TEXT("items") / TEXT("seed_actor") / TEXT("components") / TEXT("seed_light_component") / TEXT("details"),
				TEXT("component"),
				TEXT("seed_light_component"),
				TEXT("Component.json"),
				ComponentFields);
	}

	bool BuildLegacyDiffPackage(const FString& Root)
	{
		IFileManager& FileManager = IFileManager::Get();
		if (!FileManager.MakeDirectory(*(Root / TEXT("actors") / TEXT("items") / TEXT("seed_actor")), true))
		{
			return false;
		}

		return
			WriteUtf8TextFileForDiffTest(Root / TEXT("manifest.json"), TEXT("{\"schema\":\"uai.level_folder.v1\"}\n")) &&
			WriteUtf8TextFileForDiffTest(
				Root / TEXT("actors") / TEXT("items") / TEXT("seed_actor") / TEXT("actor.json"),
				TEXT("{\"schema\":\"uai.level_folder.v1\",\"kind\":\"actor\",\"actor_id\":\"seed_actor\",\"class_path\":\"/Script/Engine.PointLight\"}\n")) &&
			WriteUtf8TextFileForDiffTest(
				Root / TEXT("actors") / TEXT("items") / TEXT("seed_actor") / TEXT("properties.json"),
				TEXT("{\"schema\":\"uai.uobject_properties.v1\",\"properties\":[]}\n"));
	}

	int32 GetGroupCount(const TSharedPtr<FJsonObject>& Summary, const FString& Group)
	{
		const TSharedPtr<FJsonObject>* Counts = nullptr;
		if (!Summary.IsValid() || !Summary->TryGetObjectField(TEXT("counts_by_group"), Counts) || !Counts || !Counts->IsValid())
		{
			return 0;
		}
		return static_cast<int32>((*Counts)->GetNumberField(Group));
	}

	bool HasChange(const TSharedPtr<FJsonObject>& Summary, const FString& Group, const FString& ChangeType, const FString& Detail)
	{
		const TArray<TSharedPtr<FJsonValue>>* Changes = nullptr;
		if (!Summary.IsValid() || !Summary->TryGetArrayField(TEXT("changes"), Changes) || !Changes)
		{
			return false;
		}
		for (const TSharedPtr<FJsonValue>& ChangeValue : *Changes)
		{
			const TSharedPtr<FJsonObject> Change = ChangeValue.IsValid() ? ChangeValue->AsObject() : nullptr;
			if (Change.IsValid()
				&& Change->GetStringField(TEXT("group")) == Group
				&& Change->GetStringField(TEXT("change")) == ChangeType
				&& Change->GetStringField(TEXT("detail")) == Detail)
			{
				return true;
			}
		}
		return false;
	}
}

bool FUeAgentLevelFolderDiffSmokeTest::RunTest(const FString& Parameters)
{
	UeAgentCommandRegistry::FExactCommandInvoker Invoker = UeAgentCommandRegistry::FindExactCommandInvoker(TEXT("level_diff_folder"));
	TestTrue(TEXT("level_diff_folder registry entry exists"), Invoker != nullptr);

	const FString BaseFolder = FPaths::ConvertRelativePathToFull(FPaths::ProjectSavedDir() / TEXT("Automation") / TEXT("UAI_LevelFolderDiffSmoke_Base"));
	const FString TargetFolder = FPaths::ConvertRelativePathToFull(FPaths::ProjectSavedDir() / TEXT("Automation") / TEXT("UAI_LevelFolderDiffSmoke_Target"));
	const FString LegacyFolder = FPaths::ConvertRelativePathToFull(FPaths::ProjectSavedDir() / TEXT("Automation") / TEXT("UAI_LevelFolderDiffSmoke_Legacy"));
	IFileManager::Get().DeleteDirectory(*BaseFolder, false, true);
	IFileManager::Get().DeleteDirectory(*TargetFolder, false, true);
	IFileManager::Get().DeleteDirectory(*LegacyFolder, false, true);
	TestTrue(TEXT("base package built"), BuildDiffPackage(BaseFolder, false));
	TestTrue(TEXT("target package built"), BuildDiffPackage(TargetFolder, true));
	TestTrue(TEXT("legacy package built"), BuildLegacyDiffPackage(LegacyFolder));

	UeAgentLevelFolderDiff::FDiffOptions Options;
	Options.BaseFolder = BaseFolder;
	Options.TargetFolder = TargetFolder;
	Options.MaxChanges = 128;
	TSharedPtr<FJsonObject> Summary;
	FString Error;
	TestTrue(TEXT("diff succeeds"), UeAgentLevelFolderDiff::DiffLevelFolders(Options, Summary, Error));
	TestTrue(TEXT("summary valid"), Summary.IsValid());
	TestFalse(TEXT("packages differ"), Summary->GetBoolField(TEXT("same")));
	TestTrue(TEXT("level diff counted"), GetGroupCount(Summary, TEXT("Level")) > 0);
	TestTrue(TEXT("actor diff counted"), GetGroupCount(Summary, TEXT("Actor")) > 0);
	TestTrue(TEXT("component diff counted"), GetGroupCount(Summary, TEXT("Component")) > 0);
	TestTrue(TEXT("property diff counted"), GetGroupCount(Summary, TEXT("Property")) > 0);
	TestTrue(TEXT("property selector alias diffs by canonical property path"), HasChange(Summary, TEXT("Property"), TEXT("changed"), TEXT("Intensity")));
	TestTrue(TEXT("reference diff counted"), GetGroupCount(Summary, TEXT("Reference")) > 0);

	UeAgentLevelFolderDiff::FDiffOptions SameOptions;
	SameOptions.BaseFolder = BaseFolder;
	SameOptions.TargetFolder = BaseFolder;
	TSharedPtr<FJsonObject> SameSummary;
	FString SameError;
	TestTrue(TEXT("same diff succeeds"), UeAgentLevelFolderDiff::DiffLevelFolders(SameOptions, SameSummary, SameError));
	TestTrue(TEXT("same package detected"), SameSummary.IsValid() && SameSummary->GetBoolField(TEXT("same")));

	UeAgentLevelFolderDiff::FDiffOptions LegacyOptions;
	LegacyOptions.BaseFolder = BaseFolder;
	LegacyOptions.TargetFolder = LegacyFolder;
	TSharedPtr<FJsonObject> LegacySummary;
	FString LegacyError;
	TestFalse(TEXT("legacy properties diff rejected"), UeAgentLevelFolderDiff::DiffLevelFolders(LegacyOptions, LegacySummary, LegacyError));
	TestTrue(TEXT("legacy diff error names old surface"), LegacyError.Contains(TEXT("legacy_properties_file_not_supported")));

	return true;
}

#endif
