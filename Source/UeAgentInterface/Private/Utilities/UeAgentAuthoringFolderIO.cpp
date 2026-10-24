#include "Utilities/UeAgentAuthoringFolderIO.h"

#include "Core/Contract/UeAgentDiagnostic.h"

#include "HAL/FileManager.h"
#include "Misc/Paths.h"
#include "Utilities/AuthoringCore/UeAgentAuthoringWorkspacePaths.h"
#include "Utilities/AuthoringExport/UeAgentExportPublisher.h"

namespace UeAgentAuthoringFolderIO
{
	bool ResetExportFolder(
		const FString& FolderPathInput,
		FString& OutResolvedFolderPath,
		UE::UAI::Contract::FDiagnostic& OutError)
	{
		return UeAgentExportPublisher::ResetOwnedWorkspace(
			FolderPathInput,
			OutResolvedFolderPath,
			OutError);
	}

	bool ResetExportFolder(const FString& FolderPathInput, UE::UAI::Contract::FDiagnostic& OutError)
	{
		FString ResolvedFolderPath;
		return ResetExportFolder(FolderPathInput, ResolvedFolderPath, OutError);
	}

	bool CopyAuthoringTreeIfExists(
		const FString& SourceFolderPath,
		const FString& DestinationFolderPath,
		UE::UAI::Contract::FDiagnostic& OutError)
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
				OutError = UE::UAI::Contract::FDiagnostic::Failure(TEXT("authoring_tree_relative_path_failed"),
					TEXT("The source file could not be expressed relative to the authoring folder."), TEXT("authoring_io"));
				OutError.Source.SourceFile = SourceFile;
				OutError.TargetPath = SourceFolderPath;
				OutError.SourceKind = TEXT("native_result");
				OutError.InternalContext = SourceRoot;
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
			if (!IFileManager::Get().MakeDirectory(*FPaths::GetPath(DestinationFile), true))
			{
				OutError = UE::UAI::Contract::FDiagnostic::Failure(TEXT("authoring_tree_copy_failed"),
					TEXT("The destination directory for the authoring file could not be created."), TEXT("authoring_io"));
				OutError.Source.SourceFile = SourceFile;
				OutError.TargetPath = DestinationFile;
				OutError.SourceKind = TEXT("native_result");
				return false;
			}
			const uint32 CopyResult = IFileManager::Get().Copy(*DestinationFile, *SourceFile, true, true);
			if (CopyResult != COPY_OK)
			{
				OutError = UE::UAI::Contract::FDiagnostic::Failure(TEXT("authoring_tree_copy_failed"),
					TEXT("The authoring file could not be copied to its destination."), TEXT("authoring_io"));
				OutError.Source.SourceFile = SourceFile;
				OutError.TargetPath = DestinationFile;
				OutError.SourceKind = TEXT("native_result");
				OutError.InternalContext = FString::Printf(TEXT("copy_result=%u"), CopyResult);
				return false;
			}
		}
		return true;
	}
}
