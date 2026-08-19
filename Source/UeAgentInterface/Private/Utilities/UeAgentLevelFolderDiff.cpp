#include "Utilities/UeAgentLevelFolderDiff.h"

#include "Domains/Level/Authoring/UeAgentLevelAuthoringApplyProvider.h"

namespace UeAgentLevelFolderDiff
{
	FDiffOptions::FDiffOptions()
		: MaxChanges(256)
	{
	}

	bool DiffLevelFolders(
		const FDiffOptions& Options,
		TSharedPtr<FJsonObject>& OutSummary,
		FString& OutError)
	{
		return UeAgentLevelAuthoring::DiffWorkspaceDirectories(
			Options.BaseFolder,
			Options.TargetFolder,
			Options.MaxChanges,
			OutSummary,
			OutError);
	}
}

