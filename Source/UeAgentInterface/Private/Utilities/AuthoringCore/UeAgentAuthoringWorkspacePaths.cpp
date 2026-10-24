#include "Utilities/AuthoringCore/UeAgentAuthoringWorkspacePaths.h"

#include "HAL/FileManager.h"
#include "Misc/PackageName.h"
#include "Misc/Paths.h"

#if PLATFORM_WINDOWS
#include "Microsoft/WindowsHWrapper.h"
#endif

namespace
{
	FString CollapseAbsoluteParentSegments(const FString& Input)
	{
		FString Prefix;
		int32 ContentStart = 0;
		if (Input.Len() >= 3 && FChar::IsAlpha(Input[0]) && Input[1] == TEXT(':') && Input[2] == TEXT('/'))
		{
			Prefix = Input.Left(3);
			ContentStart = 3;
		}
		else if (Input.StartsWith(TEXT("/")) && !Input.StartsWith(TEXT("//")))
		{
			Prefix = TEXT("/");
			ContentStart = 1;
		}
		else
		{
			return Input;
		}

		TArray<FString> Segments;
		Input.Mid(ContentStart).ParseIntoArray(Segments, TEXT("/"), true);
		TArray<FString> Canonical;
		for (const FString& Segment : Segments)
		{
			if (Segment.IsEmpty() || Segment == TEXT(".")) continue;
			if (Segment == TEXT(".."))
			{
				if (!Canonical.IsEmpty()) Canonical.Pop(EAllowShrinking::No);
				continue;
			}
			Canonical.Add(Segment);
		}
		return Prefix + FString::Join(Canonical, TEXT("/"));
	}

	bool IsPathUnderRoot(const FString& Path, const FString& Root)
	{
		if (Path.Equals(Root, ESearchCase::IgnoreCase))
		{
			return true;
		}
		FString RootWithSlash = Root;
		RootWithSlash.Append(TEXT("/"));
		return Path.StartsWith(RootWithSlash, ESearchCase::IgnoreCase);
	}

	FString MakeRelativePath(const FString& Root, const FString& Path)
	{
		if (Path.Equals(Root, ESearchCase::IgnoreCase))
		{
			return FString();
		}
		FString Relative = Path;
		FString RootWithSlash = Root;
		RootWithSlash.Append(TEXT("/"));
		if (!FPaths::MakePathRelativeTo(Relative, *RootWithSlash))
		{
			return FString();
		}
		FPaths::NormalizeFilename(Relative);
		return Relative;
	}

#if PLATFORM_WINDOWS
	FString RemoveWindowsExtendedPathPrefix(const FString& Path)
	{
		if (Path.StartsWith(TEXT("\\\\?\\UNC\\"), ESearchCase::IgnoreCase))
		{
			return TEXT("\\\\") + Path.Mid(8);
		}
		if (Path.StartsWith(TEXT("\\\\?\\"), ESearchCase::IgnoreCase))
		{
			return Path.Mid(4);
		}
		return Path;
	}

	bool ResolveExistingWindowsFinalPath(
		const FString& ExistingPath,
		FString& OutFinalPath)
	{
		OutFinalPath.Reset();
		const HANDLE Handle = ::CreateFileW(
			*ExistingPath,
			0,
			FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
			nullptr,
			OPEN_EXISTING,
			FILE_FLAG_BACKUP_SEMANTICS,
			nullptr);
		if (Handle == INVALID_HANDLE_VALUE)
		{
			return false;
		}

		const DWORD RequiredLength = ::GetFinalPathNameByHandleW(
			Handle,
			nullptr,
			0,
			FILE_NAME_NORMALIZED | VOLUME_NAME_DOS);
		if (RequiredLength == 0)
		{
			::CloseHandle(Handle);
			return false;
		}

		TArray<WCHAR> Buffer;
		Buffer.SetNumZeroed(static_cast<int32>(RequiredLength) + 1);
		const DWORD WrittenLength = ::GetFinalPathNameByHandleW(
			Handle,
			Buffer.GetData(),
			static_cast<DWORD>(Buffer.Num()),
			FILE_NAME_NORMALIZED | VOLUME_NAME_DOS);
		::CloseHandle(Handle);
		if (WrittenLength == 0 || WrittenLength >= static_cast<DWORD>(Buffer.Num()))
		{
			return false;
		}

		OutFinalPath = UeAgentAuthoringWorkspacePaths::NormalizeAbsolutePath(
			RemoveWindowsExtendedPathPrefix(FString(Buffer.GetData(), static_cast<int32>(WrittenLength))));
		return !OutFinalPath.IsEmpty();
	}
#endif

	bool ResolveFinalOrProjectedPath(
		const FString& LogicalPath,
		FString& OutFinalPath,
		bool& bOutTraversedReparse)
	{
		OutFinalPath.Reset();
		bOutTraversedReparse = false;
		FString ExistingPath = LogicalPath;
		while (!IFileManager::Get().FileExists(*ExistingPath)
			&& !IFileManager::Get().DirectoryExists(*ExistingPath))
		{
			const FString Parent = UeAgentAuthoringWorkspacePaths::NormalizeAbsolutePath(
				FPaths::GetPath(ExistingPath));
			if (Parent.IsEmpty() || Parent.Equals(ExistingPath, ESearchCase::IgnoreCase))
			{
				return false;
			}
			ExistingPath = Parent;
		}

		FString FinalExistingPath = ExistingPath;
#if PLATFORM_WINDOWS
		if (!ResolveExistingWindowsFinalPath(ExistingPath, FinalExistingPath))
		{
			return false;
		}
#endif
		bOutTraversedReparse = !FinalExistingPath.Equals(ExistingPath, ESearchCase::IgnoreCase);
		if (LogicalPath.Equals(ExistingPath, ESearchCase::IgnoreCase))
		{
			OutFinalPath = FinalExistingPath;
			return true;
		}

		FString RemainingPath = LogicalPath.Mid(ExistingPath.Len());
		while (RemainingPath.StartsWith(TEXT("/")) || RemainingPath.StartsWith(TEXT("\\")))
		{
			RemainingPath.RightChopInline(1, EAllowShrinking::No);
		}
		OutFinalPath = UeAgentAuthoringWorkspacePaths::NormalizeAbsolutePath(
			FPaths::Combine(FinalExistingPath, RemainingPath));
		return !OutFinalPath.IsEmpty();
	}
}

namespace UeAgentAuthoringWorkspacePaths
{
	FString NormalizeAbsolutePath(const FString& PathInput)
	{
		FString Path = PathInput;
		Path.TrimStartAndEndInline();
		if (Path.IsEmpty())
		{
			return Path;
		}
		Path = FPaths::ConvertRelativePathToFull(Path);
		FPaths::NormalizeFilename(Path);
		FPaths::CollapseRelativeDirectories(Path);
		Path = CollapseAbsoluteParentSegments(Path);
		while (Path.EndsWith(TEXT("/")) && !FPaths::IsDrive(Path))
		{
			Path.LeftChopInline(1, EAllowShrinking::No);
		}
		return Path;
	}

	bool ResolveUnderRoot(
		const FString& WorkspaceRootInput,
		const FString& PathInput,
		const FResolveOptions& Options,
		FResolvedPath& OutPath,
		FDiagnostic& OutError)
	{
		OutPath = FResolvedPath();
		OutError.Reset();

		const FString WorkspaceRoot = NormalizeAbsolutePath(WorkspaceRootInput);
		FString Candidate = PathInput;
		Candidate.TrimStartAndEndInline();
		if (WorkspaceRoot.IsEmpty() || Candidate.IsEmpty())
		{
			OutError = FDiagnostic::Failure(TEXT("workspace_source_invalid"), TEXT("The workspace source is missing or is not the required directory."), TEXT("workspace_path"));
			return false;
		}

		FString AbsolutePath;
		if (FPaths::IsRelative(Candidate))
		{
			const FString ProcessRelativePath = NormalizeAbsolutePath(Candidate);
			AbsolutePath = IsPathUnderRoot(ProcessRelativePath, WorkspaceRoot)
				? ProcessRelativePath
				: NormalizeAbsolutePath(FPaths::Combine(WorkspaceRoot, Candidate));
		}
		else
		{
			AbsolutePath = NormalizeAbsolutePath(Candidate);
		}
		if (!IsPathUnderRoot(AbsolutePath, WorkspaceRoot))
		{
			OutError = FDiagnostic::Failure(TEXT("workspace_path_escape"), TEXT("The requested path is outside the workspace root."), TEXT("workspace_path"));
			OutError.InternalContext = FString::Printf(TEXT("%s"), *Candidate);
			return false;
		}
		if (!Options.bAllowRoot && AbsolutePath.Equals(WorkspaceRoot, ESearchCase::IgnoreCase))
		{
			OutError = FDiagnostic::Failure(TEXT("workspace_root_not_allowed"), TEXT("This operation requires a workspace entry, not the root directory."), TEXT("workspace_path"));
			OutError.InternalContext = FString::Printf(TEXT("%s"), *AbsolutePath);
			return false;
		}

		FString FinalWorkspaceRoot;
		FString FinalAbsolutePath;
		bool bWorkspaceRootTraversedReparse = false;
		bool bCandidateTraversedReparse = false;
		if (!ResolveFinalOrProjectedPath(
				WorkspaceRoot,
				FinalWorkspaceRoot,
				bWorkspaceRootTraversedReparse)
			|| !ResolveFinalOrProjectedPath(
				AbsolutePath,
				FinalAbsolutePath,
				bCandidateTraversedReparse))
		{
			OutError = FDiagnostic::Failure(TEXT("workspace_reparse_resolution_failed"), TEXT("The filesystem could not resolve the final workspace path."), TEXT("workspace_path"));
			OutError.InternalContext = FString::Printf(TEXT("%s"), *AbsolutePath);
			return false;
		}
		if (bWorkspaceRootTraversedReparse)
		{
			OutError = FDiagnostic::Failure(TEXT("workspace_reparse_not_allowed"), TEXT("The workspace path passes through a reparse point that this operation does not allow."), TEXT("workspace_path"));
			OutError.InternalContext = FString::Printf(TEXT("%s"), *WorkspaceRoot);
			return false;
		}
		if (!IsPathUnderRoot(FinalAbsolutePath, FinalWorkspaceRoot))
		{
			OutError = FDiagnostic::Failure(TEXT("workspace_reparse_escape"), TEXT("The resolved filesystem path escapes the workspace root."), TEXT("workspace_path"));
			OutError.InternalContext = FString::Printf(TEXT("%s"), *AbsolutePath);
			return false;
		}
		if (bCandidateTraversedReparse)
		{
			OutError = FDiagnostic::Failure(TEXT("workspace_reparse_not_allowed"), TEXT("The workspace path passes through a reparse point that this operation does not allow."), TEXT("workspace_path"));
			OutError.InternalContext = FString::Printf(TEXT("%s"), *AbsolutePath);
			return false;
		}
		if (Options.bRequireHjson && !FPaths::GetExtension(AbsolutePath, false).Equals(TEXT("hjson"), ESearchCase::IgnoreCase))
		{
			OutError = FDiagnostic::Failure(TEXT("hjson_file_required"), TEXT("The workspace entry must be an HJSON file."), TEXT("workspace_path"));
			OutError.InternalContext = FString::Printf(TEXT("%s"), *AbsolutePath);
			return false;
		}

		const bool bIsFile = IFileManager::Get().FileExists(*AbsolutePath);
		const bool bIsDirectory = IFileManager::Get().DirectoryExists(*AbsolutePath);
		if (Options.bRequireExists && !bIsFile && !bIsDirectory)
		{
			OutError = FDiagnostic::Failure(TEXT("workspace_entry_missing"), TEXT("The requested workspace entry does not exist."), TEXT("workspace_path"));
			OutError.InternalContext = FString::Printf(TEXT("%s"), *AbsolutePath);
			return false;
		}
		if (Options.bRequireDirectory && Options.bRequireExists && !bIsDirectory)
		{
			OutError = FDiagnostic::Failure(TEXT("workspace_source_invalid"), TEXT("The workspace source is missing or is not the required directory."), TEXT("workspace_path"));
			OutError.InternalContext = FString::Printf(TEXT("%s"), *AbsolutePath);
			return false;
		}

		OutPath.WorkspaceRoot = WorkspaceRoot;
		OutPath.AbsolutePath = AbsolutePath;
		OutPath.RelativePath = MakeRelativePath(WorkspaceRoot, AbsolutePath);
		OutPath.CanonicalKey = OutPath.RelativePath.ToLower();
		OutPath.bExists = bIsFile || bIsDirectory;
		return true;
	}

	bool ResolveProjectPath(
		const FString& PathInput,
		const FResolveOptions& Options,
		FResolvedPath& OutPath,
		FDiagnostic& OutError)
	{
		return ResolveUnderRoot(FPaths::ProjectDir(), PathInput, Options, OutPath, OutError);
	}

	bool ResolveReference(
		const FString& WorkspaceRootInput,
		const FString& SourceFileInput,
		const FString& ReferenceInput,
		const FResolveOptions& Options,
		FResolvedPath& OutPath,
		FDiagnostic& OutError)
	{
		FResolveOptions SourceOptions;
		SourceOptions.bAllowRoot = false;
		FResolvedPath SourcePath;
		if (!ResolveUnderRoot(WorkspaceRootInput, SourceFileInput, SourceOptions, SourcePath, OutError))
		{
			const FDiagnostic Cause = OutError;
			OutError = FDiagnostic::Failure(TEXT("workspace_source_invalid"), TEXT("The source file cannot be resolved inside the workspace."), TEXT("workspace_path"));
			OutError.PublicCause = MakeShared<FDiagnostic>(Cause);
			return false;
		}

		FString Reference = ReferenceInput;
		Reference.TrimStartAndEndInline();
		if (Reference.IsEmpty())
		{
			OutError = FDiagnostic::Failure(TEXT("workspace_reference_unresolved"), TEXT("The workspace reference is empty."), TEXT("workspace_path"));
			return false;
		}

		FString Candidate;
		if (Reference.StartsWith(TEXT("/")) && !FPaths::IsDrive(Reference))
		{
			while (Reference.StartsWith(TEXT("/")))
			{
				Reference.RightChopInline(1, EAllowShrinking::No);
			}
			Candidate = Reference;
		}
		else if (FPaths::IsRelative(Reference))
		{
			Candidate = FPaths::Combine(FPaths::GetPath(SourcePath.AbsolutePath), Reference);
		}
		else
		{
			Candidate = Reference;
		}

		if (!ResolveUnderRoot(SourcePath.WorkspaceRoot, Candidate, Options, OutPath, OutError))
		{
			if (!(OutError.Code == TEXT("workspace_path_escape"))
				&& !(OutError.Code == TEXT("workspace_reparse_escape"))
				&& !(OutError.Code == TEXT("workspace_reparse_not_allowed"))
				&& !(OutError.Code == TEXT("workspace_reparse_resolution_failed")))
			{
				const FDiagnostic Cause = OutError;
				OutError = FDiagnostic::Failure(TEXT("workspace_reference_unresolved"), TEXT("The workspace reference could not be resolved."), TEXT("workspace_path"));
				OutError.InternalContext = ReferenceInput;
				OutError.PublicCause = MakeShared<FDiagnostic>(Cause);
			}
			return false;
		}
		return true;
	}

	bool DetectCaseCollisions(
		const FString& WorkspaceRootInput,
		const TArray<FString>& PathInputs,
		FDiagnostic& OutError)
	{
		OutError.Reset();
		TMap<FString, FString> OriginalByCanonicalKey;
		FResolveOptions Options;
		Options.bAllowRoot = false;
		for (const FString& PathInput : PathInputs)
		{
			FResolvedPath Resolved;
			if (!ResolveUnderRoot(WorkspaceRootInput, PathInput, Options, Resolved, OutError))
			{
				return false;
			}
			if (const FString* Existing = OriginalByCanonicalKey.Find(Resolved.CanonicalKey))
			{
				if (!Existing->Equals(Resolved.RelativePath, ESearchCase::CaseSensitive))
				{
					OutError = FDiagnostic::Failure(TEXT("workspace_path_case_collision"), TEXT("Workspace paths collide when compared without case."), TEXT("workspace_path"));
			OutError.InternalContext = FString::Printf(TEXT("%s:%s"), **Existing, *Resolved.RelativePath);
					return false;
				}
				OutError = FDiagnostic::Failure(TEXT("workspace_duplicate_target"), TEXT("The workspace contains the same target path more than once."), TEXT("workspace_path"));
			OutError.InternalContext = FString::Printf(TEXT("%s"), *Resolved.RelativePath);
				return false;
			}
			OriginalByCanonicalKey.Add(Resolved.CanonicalKey, Resolved.RelativePath);
		}
		return true;
	}

	bool MakeAssetRelativePath(
		const FString& AssetPathInput,
		FString& OutRelativePath,
		FDiagnostic& OutError)
	{
		OutRelativePath.Reset();
		OutError.Reset();
		FString AssetPath = AssetPathInput;
		AssetPath.TrimStartAndEndInline();
		AssetPath = FPackageName::ObjectPathToPackageName(AssetPath);
		if (!FPackageName::IsValidLongPackageName(AssetPath))
		{
			OutError = FDiagnostic::Failure(TEXT("invalid_asset_path"), TEXT("The asset path is invalid."), TEXT("workspace_path"));
			return false;
		}
		while (AssetPath.StartsWith(TEXT("/")))
		{
			AssetPath.RightChopInline(1, EAllowShrinking::No);
		}
		OutRelativePath = AssetPath;
		return true;
	}

	bool ResolveAssetWorkspaceFolder(
		const FString& WorkspaceRootInput,
		const FString& AssetPathInput,
		FString& OutFolderPath,
		FDiagnostic& OutError)
	{
		OutFolderPath.Reset();
		FString RelativePath;
		if (!MakeAssetRelativePath(AssetPathInput, RelativePath, OutError))
		{
			return false;
		}
		FResolveOptions Options;
		Options.bAllowRoot = false;
		FResolvedPath Resolved;
		if (!ResolveUnderRoot(WorkspaceRootInput, RelativePath, Options, Resolved, OutError))
		{
			return false;
		}
		OutFolderPath = Resolved.AbsolutePath;
		return true;
	}
}
