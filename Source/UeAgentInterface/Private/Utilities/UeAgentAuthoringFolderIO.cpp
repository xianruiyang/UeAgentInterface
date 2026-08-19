#include "Utilities/UeAgentAuthoringFolderIO.h"

#include "HAL/FileManager.h"
#include "Misc/Paths.h"
#include "Utilities/AuthoringCore/UeAgentAuthoringWorkspacePaths.h"
#include "Utilities/AuthoringExport/UeAgentExportPublisher.h"

namespace UeAgentAuthoringFolderIO
{
	bool ResetExportFolder(
		const FString& FolderPathInput,
		FString& OutResolvedFolderPath,
		FString& OutError)
	{
		return UeAgentExportPublisher::ResetOwnedWorkspace(
			FolderPathInput,
			OutResolvedFolderPath,
			OutError);
	}

	bool ResetExportFolder(const FString& FolderPathInput, FString& OutError)
	{
		FString ResolvedFolderPath;
		return ResetExportFolder(FolderPathInput, ResolvedFolderPath, OutError);
	}

	bool CopyAuthoringTreeIfExists(
		const FString& SourceFolderPath,
		const FString& DestinationFolderPath,
		FString& OutError)
	{
		OutError.Reset();

		UeAgentAuthoringWorkspacePaths::FResolveOptions ResolveOptions;
		UeAgentAuthoringWorkspacePaths::FResolvedPath ResolvedSourceRoot;
		if (!UeAgentAuthoringWorkspacePaths::ResolveProjectPath(
			SourceFolderPath,
			ResolveOptions,
			ResolvedSourceRoot,
			OutError))
		{
			return false;
		}
		UeAgentAuthoringWorkspacePaths::FResolvedPath ResolvedDestinationRoot;
		if (!UeAgentAuthoringWorkspacePaths::ResolveProjectPath(
			DestinationFolderPath,
			ResolveOptions,
			ResolvedDestinationRoot,
			OutError))
		{
			return false;
		}
		const FString& SourceRoot = ResolvedSourceRoot.AbsolutePath;
		const FString& DestinationRoot = ResolvedDestinationRoot.AbsolutePath;
		if (!IFileManager::Get().DirectoryExists(*SourceRoot))
		{
			return true;
		}

		FString SourceRootWithSlash = SourceRoot;
		SourceRootWithSlash.Append(TEXT("/"));

		TArray<FString> SourceFiles;
		IFileManager::Get().FindFilesRecursive(SourceFiles, *SourceRoot, TEXT("*.hjson"), true, false);
		if (!UeAgentAuthoringWorkspacePaths::DetectCaseCollisions(SourceRoot, SourceFiles, OutError))
		{
			return false;
		}
		for (const FString& SourceFile : SourceFiles)
		{
			FString RelativePath = SourceFile;
			if (!FPaths::MakePathRelativeTo(RelativePath, *SourceRootWithSlash))
			{
				OutError = FString::Printf(TEXT("authoring_tree_relative_path_failed:%s"), *SourceFile);
				return false;
			}

			UeAgentAuthoringWorkspacePaths::FResolveOptions DestinationOptions;
			DestinationOptions.bRequireHjson = true;
			UeAgentAuthoringWorkspacePaths::FResolvedPath DestinationFilePath;
			if (!UeAgentAuthoringWorkspacePaths::ResolveUnderRoot(
				DestinationRoot,
				RelativePath,
				DestinationOptions,
				DestinationFilePath,
				OutError))
			{
				return false;
			}
			const FString& DestinationFile = DestinationFilePath.AbsolutePath;
			if (!IFileManager::Get().MakeDirectory(*FPaths::GetPath(DestinationFile), true)
				|| IFileManager::Get().Copy(*DestinationFile, *SourceFile, true, true) != COPY_OK)
			{
				OutError = FString::Printf(TEXT("authoring_tree_copy_failed:%s"), *RelativePath);
				return false;
			}
		}
		return true;
	}
}
