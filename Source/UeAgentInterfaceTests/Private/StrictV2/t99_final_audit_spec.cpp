#include "Dom/JsonObject.h"
#include "HAL/FileManager.h"
#include "Misc/AutomationTest.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"

namespace
{
	static FString ProjectPath(const FString& RelativePath)
	{
		return FPaths::ConvertRelativePathToFull(FPaths::Combine(
			FPaths::ProjectDir(),
			RelativePath));
	}

	static bool LoadJsonObject(
		FAutomationTestBase& Test,
		const FString& RelativePath,
		TSharedPtr<FJsonObject>& OutObject)
	{
		FString Source;
		if (!Test.TestTrue(
			*FString::Printf(TEXT("load %s"), *RelativePath),
			FFileHelper::LoadFileToString(Source, *ProjectPath(RelativePath))))
		{
			return false;
		}

		const TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(Source);
		return Test.TestTrue(
			*FString::Printf(TEXT("parse %s"), *RelativePath),
			FJsonSerializer::Deserialize(Reader, OutObject) && OutObject.IsValid());
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUaiStrictV2T99FinalAuditTest,
	"UAI.StrictV2.T99.FINAL.AUDIT",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FUaiStrictV2T99FinalAuditTest::RunTest(const FString& Parameters)
{
	const FString ManifestPath =
		TEXT("Plugins/UeAgentInterface/Resources/UaiCommandManifest.json");
	TSharedPtr<FJsonObject> Manifest;
	if (!LoadJsonObject(*this, ManifestPath, Manifest))
	{
		return false;
	}

	TestEqual(
		TEXT("manifest schema"),
		Manifest->GetStringField(TEXT("schema")),
		FString(TEXT("uai.command_manifest.v1")));
	TestEqual(
		TEXT("manifest source"),
		Manifest->GetObjectField(TEXT("source"))->GetStringField(TEXT("kind")),
		FString(TEXT("cpp_immutable_command_registry_export")));

	const TArray<TSharedPtr<FJsonValue>>& Commands =
		Manifest->GetArrayField(TEXT("commands"));
	TestEqual(TEXT("active command count"), Commands.Num(), 342);
	TSet<FString> CommandIds;
	for (const TSharedPtr<FJsonValue>& Value : Commands)
	{
		const TSharedPtr<FJsonObject> Command = Value->AsObject();
		if (!TestTrue(TEXT("command row is an object"), Command.IsValid()))
		{
			continue;
		}
		const FString CommandId = Command->GetStringField(TEXT("command_id"));
		TestFalse(*FString::Printf(TEXT("duplicate command %s"), *CommandId), CommandIds.Contains(CommandId));
		CommandIds.Add(CommandId);
		TestEqual(
			*FString::Printf(TEXT("active disposition %s"), *CommandId),
			Command->GetStringField(TEXT("status")),
			FString(TEXT("active")));
	}

	const FString FinalAuditPath =
		TEXT("Plugins/UeAgentInterface/docs/active/t99_final_audit.md");
	FString FinalAudit;
	if (TestTrue(
		TEXT("final audit projection exists"),
		FFileHelper::LoadFileToString(FinalAudit, *ProjectPath(FinalAuditPath))))
	{
		TestTrue(TEXT("final audit identifies T99"), FinalAudit.Contains(TEXT("T99-FINAL-AUDIT")));
		TestTrue(TEXT("final audit records strict_v2"), FinalAudit.Contains(TEXT("strict_v2")));
		TestTrue(TEXT("final audit records taskctl audit"), FinalAudit.Contains(TEXT("taskctl audit --all")));
		TestTrue(TEXT("final audit forbids publish"), FinalAudit.Contains(TEXT("no publish")));
	}

	TestTrue(
		TEXT("legacy retirement manifest exists"),
		IFileManager::Get().FileExists(*ProjectPath(
			TEXT("Plugins/UeAgentInterface/docs/active/legacy_asset_retirement_manifest.json"))));
	TestFalse(
		TEXT("old generated command specs left the active path"),
		IFileManager::Get().DirectoryExists(*ProjectPath(
			TEXT("Plugins/UeAgentInterface/docs/command-specs"))));
	TestFalse(
		TEXT("old generated command pages left the active path"),
		IFileManager::Get().DirectoryExists(*ProjectPath(
			TEXT("Plugins/UeAgentInterface/docs/commands"))));
	TestTrue(
		TEXT("retired plugin audit root exists"),
		IFileManager::Get().DirectoryExists(*ProjectPath(
			TEXT("Plugins/UeAgentInterface/retired/strict_v2"))));
	TestTrue(
		TEXT("generated public SDK signature exists"),
		IFileManager::Get().FileExists(*ProjectPath(
			TEXT("UeAgentInterfaceCMD/cli/uai/commands/generated.pyi"))));

	return !HasAnyErrors();
}
