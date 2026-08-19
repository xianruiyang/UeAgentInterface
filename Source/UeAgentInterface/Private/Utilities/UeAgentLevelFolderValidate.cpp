#include "Utilities/UeAgentLevelFolderValidate.h"

#include "Domains/Level/Authoring/UeAgentLevelAuthoringApplyProvider.h"
#include "Dom/JsonObject.h"
#include "Editor.h"
#include "Engine/World.h"

namespace UeAgentLevelFolderValidate
{
	FValidateOptions::FValidateOptions() = default;

	bool ValidateLevelFolder(
		const FValidateOptions& Options,
		TSharedPtr<FJsonObject>& OutSummary,
		FString& OutError)
	{
		OutSummary = MakeShared<FJsonObject>();
		OutError.Reset();
		UWorld* World = GEditor ? GEditor->GetEditorWorldContext().World() : nullptr;
		if (!World)
		{
			OutError = TEXT("editor_world_unavailable");
			return false;
		}
		UeAgentLevelAuthoring::FWorkspaceApplyResult Result;
		const bool bValid = UeAgentLevelAuthoring::ValidateWorkspaceDirectory(
			*World, Options.InputFolder, Result);
		OutSummary = Result.ToJsonObject();
		OutSummary->SetBoolField(TEXT("ok"), bValid);
		if (!bValid) OutError = Result.ErrorCode + TEXT(":") + Result.ErrorDetail;
		return bValid;
	}
}

