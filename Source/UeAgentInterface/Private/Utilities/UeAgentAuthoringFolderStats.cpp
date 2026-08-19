#include "Utilities/UeAgentAuthoringFolderStats.h"

#include "Dom/JsonObject.h"
#include "HAL/FileManager.h"
#include "Misc/Paths.h"

namespace
{
	int32 CountFilesByPattern(const FString& FolderPath, const FString& Pattern)
	{
		if (FolderPath.IsEmpty() || !IFileManager::Get().DirectoryExists(*FolderPath))
		{
			return 0;
		}

		TArray<FString> Files;
		IFileManager::Get().FindFilesRecursive(Files, *FolderPath, *Pattern, true, false, false);
		return Files.Num();
	}
}

namespace UeAgentAuthoringFolderStats
{
	FString NormalizeFolderPath(const FString& FolderPathInput)
	{
		FString FolderPath = FolderPathInput;
		FolderPath.TrimStartAndEndInline();
		if (FolderPath.IsEmpty())
		{
			return FString();
		}
		return FPaths::ConvertRelativePathToFull(FolderPath);
	}

	FStats CollectStats(const FString& FolderPathInput, const FCollectOptions& Options)
	{
		const FString FolderPath = NormalizeFolderPath(FolderPathInput);

		FStats Stats;
		Stats.bFolderExists = !FolderPath.IsEmpty() && IFileManager::Get().DirectoryExists(*FolderPath);
		if (!Stats.bFolderExists)
		{
			return Stats;
		}

		Stats.HjsonFileCount = CountFilesByPattern(FolderPath, TEXT("*.hjson"));
		if (Options.bIncludeJsonFileCount)
		{
			Stats.JsonFileCount = CountFilesByPattern(FolderPath, TEXT("*.json"));
		}
		return Stats;
	}

	int32 CountHjsonFiles(const FString& FolderPathInput)
	{
		return CollectStats(FolderPathInput).HjsonFileCount;
	}

	void SetExportStatsFields(
		const TSharedPtr<FJsonObject>& TargetObject,
		const FStats& Stats,
		const FCollectOptions& Options)
	{
		if (!TargetObject.IsValid())
		{
			return;
		}

		TargetObject->SetNumberField(TEXT("file_count"), Stats.HjsonFileCount);
		if (Options.bIncludeHjsonFileCount)
		{
			TargetObject->SetNumberField(TEXT("hjson_file_count"), Stats.HjsonFileCount);
		}
		if (Options.bIncludeJsonFileCount)
		{
			TargetObject->SetNumberField(TEXT("json_file_count"), Stats.JsonFileCount);
		}
	}

	void SetExportStatsFields(
		const TSharedPtr<FJsonObject>& TargetObject,
		const FString& FolderPathInput,
		const FCollectOptions& Options)
	{
		SetExportStatsFields(TargetObject, CollectStats(FolderPathInput, Options), Options);
	}
}
