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
		FString& OutError)
	{
		OutPath = FResolvedPath();
		OutError.Reset();

		const FString WorkspaceRoot = NormalizeAbsolutePath(WorkspaceRootInput);
		FString Candidate = PathInput;
		Candidate.TrimStartAndEndInline();
		if (WorkspaceRoot.IsEmpty() || Candidate.IsEmpty())
		{
			OutError = TEXT("workspace_source_invalid");
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
			OutError = FString::Printf(TEXT("workspace_path_escape:%s"), *Candidate);
			return false;
		}
		if (!Options.bAllowRoot && AbsolutePath.Equals(WorkspaceRoot, ESearchCase::IgnoreCase))
		{
			OutError = FString::Printf(TEXT("workspace_root_not_allowed:%s"), *AbsolutePath);
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
			OutError = FString::Printf(TEXT("workspace_reparse_resolution_failed:%s"), *AbsolutePath);
			return false;
		}
		if (bWorkspaceRootTraversedReparse)
		{
			OutError = FString::Printf(TEXT("workspace_reparse_not_allowed:%s"), *WorkspaceRoot);
			return false;
		}
		if (!IsPathUnderRoot(FinalAbsolutePath, FinalWorkspaceRoot))
		{
			OutError = FString::Printf(TEXT("workspace_reparse_escape:%s"), *AbsolutePath);
			return false;
		}
		if (bCandidateTraversedReparse)
		{
			OutError = FString::Printf(TEXT("workspace_reparse_not_allowed:%s"), *AbsolutePath);
			return false;
		}
		if (Options.bRequireHjson && !FPaths::GetExtension(AbsolutePath, false).Equals(TEXT("hjson"), ESearchCase::IgnoreCase))
		{
			OutError = FString::Printf(TEXT("hjson_file_required:%s"), *AbsolutePath);
			return false;
		}

		const bool bIsFile = IFileManager::Get().FileExists(*AbsolutePath);
		const bool bIsDirectory = IFileManager::Get().DirectoryExists(*AbsolutePath);
		if (Options.bRequireExists && !bIsFile && !bIsDirectory)
		{
			OutError = FString::Printf(TEXT("workspace_entry_missing:%s"), *AbsolutePath);
			return false;
		}
		if (Options.bRequireDirectory && Options.bRequireExists && !bIsDirectory)
		{
			OutError = FString::Printf(TEXT("workspace_source_invalid:%s"), *AbsolutePath);
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
		FString& OutError)
	{
		return ResolveUnderRoot(FPaths::ProjectDir(), PathInput, Options, OutPath, OutError);
	}

	bool ResolveReference(
		const FString& WorkspaceRootInput,
		const FString& SourceFileInput,
		const FString& ReferenceInput,
		const FResolveOptions& Options,
		FResolvedPath& OutPath,
		FString& OutError)
	{
		FResolveOptions SourceOptions;
		SourceOptions.bAllowRoot = false;
		FResolvedPath SourcePath;
		if (!ResolveUnderRoot(WorkspaceRootInput, SourceFileInput, SourceOptions, SourcePath, OutError))
		{
			OutError = FString::Printf(TEXT("workspace_source_invalid:%s"), *OutError);
			return false;
		}

		FString Reference = ReferenceInput;
		Reference.TrimStartAndEndInline();
		if (Reference.IsEmpty())
		{
			OutError = TEXT("workspace_reference_unresolved:empty");
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
			if (!OutError.StartsWith(TEXT("workspace_path_escape:"))
				&& !OutError.StartsWith(TEXT("workspace_reparse_escape:"))
				&& !OutError.StartsWith(TEXT("workspace_reparse_not_allowed:"))
				&& !OutError.StartsWith(TEXT("workspace_reparse_resolution_failed:")))
			{
				OutError = FString::Printf(TEXT("workspace_reference_unresolved:%s:%s"), *ReferenceInput, *OutError);
			}
			return false;
		}
		return true;
	}

	bool DetectCaseCollisions(
		const FString& WorkspaceRootInput,
		const TArray<FString>& PathInputs,
		FString& OutError)
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
					OutError = FString::Printf(TEXT("workspace_path_case_collision:%s:%s"), **Existing, *Resolved.RelativePath);
					return false;
				}
				OutError = FString::Printf(TEXT("workspace_duplicate_target:%s"), *Resolved.RelativePath);
				return false;
			}
			OriginalByCanonicalKey.Add(Resolved.CanonicalKey, Resolved.RelativePath);
		}
		return true;
	}

	bool MakeAssetRelativePath(
		const FString& AssetPathInput,
		FString& OutRelativePath,
		FString& OutError)
	{
		OutRelativePath.Reset();
		OutError.Reset();
		FString AssetPath = AssetPathInput;
		AssetPath.TrimStartAndEndInline();
		AssetPath = FPackageName::ObjectPathToPackageName(AssetPath);
		if (!FPackageName::IsValidLongPackageName(AssetPath))
		{
			OutError = TEXT("invalid_asset_path");
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
		FString& OutError)
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
