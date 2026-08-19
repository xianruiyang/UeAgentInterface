#include "Utilities/UeAgentLevelFolderExport.h"

#include "Domains/Level/Authoring/UeAgentLevelAuthoringExport.h"
#include "Dom/JsonObject.h"
#include "Engine/World.h"
#include "Misc/Paths.h"

namespace UeAgentLevelFolderExport
{
	FExportOptions::FExportOptions()
		: Profile(TEXT("llm_authoring"))
	{
	}

	bool ExportLevelFolder(
		UWorld* World,
		const FExportOptions& Options,
		TSharedPtr<FJsonObject>& OutSummary,
		FString& OutError)
	{
		OutSummary = MakeShared<FJsonObject>();
		OutError.Reset();
		if (!World)
		{
			OutError = TEXT("editor_world_unavailable");
			return false;
		}
		FString OutputFolder = Options.OutputFolder;
		if (OutputFolder.IsEmpty())
		{
			OutputFolder = FPaths::Combine(
				FPaths::ProjectSavedDir(),
				TEXT("UeAgentInterface/LevelExports"),
				FPaths::GetBaseFilename(World->GetOutermost()->GetName()));
		}
		UeAgentLevelAuthoring::FLevelExportResult Result;
		const bool bExported = UeAgentLevelAuthoring::ExportLevelWorkspace(
			*World, OutputFolder, Options.Profile, Result, OutError);
		OutSummary = Result.ToJsonObject();
		return bExported;
	}
}

