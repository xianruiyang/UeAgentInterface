#include "Utilities/UeAgentLevelFolderApply.h"

#include "Domains/Level/Authoring/UeAgentLevelAuthoringApplyProvider.h"
#include "Dom/JsonObject.h"
#include "Engine/World.h"

namespace UeAgentLevelFolderApply
{
	FApplyOptions::FApplyOptions() = default;

	bool ApplyLevelFolder(
		UWorld* World,
		const FApplyOptions& Options,
		TSharedPtr<FJsonObject>& OutSummary,
		FString& OutError,
		const FString& RequestId)
	{
		OutSummary = MakeShared<FJsonObject>();
		OutError.Reset();
		if (!World)
		{
			OutError = TEXT("editor_world_unavailable");
			return false;
		}
		UeAgentLevelAuthoring::FWorkspaceApplyOptions ApplyOptions;
		UeAgentLevelAuthoring::FWorkspaceApplyResult Result;
		const bool bApplied = UeAgentLevelAuthoring::ApplyWorkspaceDirectory(
			*World, Options.InputFolder, ApplyOptions, Result, RequestId);
		OutSummary = Result.ToJsonObject();
		if (!bApplied) OutError = Result.ErrorCode + TEXT(":") + Result.ErrorDetail;
		return bApplied;
	}
}
