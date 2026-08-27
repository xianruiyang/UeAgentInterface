#include "TestSupport/UeAgentFixtureDirectoryLifecycle.h"

#include "HAL/PlatformFile.h"
#include "HAL/PlatformFileManager.h"
#include "Misc/PackageName.h"
#include "Misc/Paths.h"

namespace UE::UAI::TestSupport
{
	namespace
	{
		bool TryNormalizePackageDirectory(
			const FString& Input,
			const TCHAR* ErrorCode,
			FString& OutPackage,
			FString& OutError)
		{
			OutPackage = Input;
			OutPackage.ReplaceInline(TEXT("\\"), TEXT("/"));
			FPaths::RemoveDuplicateSlashes(OutPackage);
			while (OutPackage.Len() > 1 && OutPackage.EndsWith(TEXT("/")))
			{
				OutPackage.LeftChopInline(1, EAllowShrinking::No);
			}
			if (!FPaths::CollapseRelativeDirectories(OutPackage)
				|| !FPackageName::IsValidLongPackageName(
					OutPackage, true))
			{
				OutError = FString::Printf(
					TEXT("%s:%s"), ErrorCode, *Input);
				return false;
			}
			return true;
		}

		bool TryVerifyDirectorySegment(
			IPlatformFile& Files,
			const FString& Directory,
			FString& OutError)
		{
			if (!Files.DirectoryExists(*Directory))
			{
				const TCHAR* const ErrorCode = Files.FileExists(*Directory)
					? TEXT("fixture_directory_segment_not_directory")
					: TEXT("fixture_directory_segment_missing");
				OutError = FString::Printf(
					TEXT("%s:%s"),
					ErrorCode,
					*Directory);
				return false;
			}

			const FFileJournalData Stat = Files.FileJournalGetFileData(
				*Directory);
			if (!Stat.bIsValid)
			{
				OutError = FString::Printf(
					TEXT("fixture_directory_segment_identity_unavailable:%s"),
					*Directory);
				return false;
			}
			if (!Stat.bIsDirectory)
			{
				OutError = FString::Printf(
					TEXT("fixture_directory_segment_not_directory:%s"),
					*Directory);
				return false;
			}
			if (Stat.bIsReparsePoint)
			{
				OutError = FString::Printf(
					TEXT("fixture_directory_segment_reparse_point:%s"),
					*Directory);
				return false;
			}
			return true;
		}
	}

	bool TryRemoveEmptyFixturePackageDirectory(
		const FString& AllowedOwnerPackage,
		const FString& ExactFixturePackageDirectory,
		FString& OutError)
	{
		OutError.Reset();
		FString OwnerPackage;
		if (!TryNormalizePackageDirectory(
			AllowedOwnerPackage,
			TEXT("fixture_directory_owner_package_invalid"),
			OwnerPackage,
			OutError))
		{
			return false;
		}
		FString TargetPackage;
		if (!TryNormalizePackageDirectory(
			ExactFixturePackageDirectory,
			TEXT("fixture_directory_target_package_invalid"),
			TargetPackage,
			OutError))
		{
			return false;
		}
		const FString OwnerPrefix = OwnerPackage + TEXT("/");
		if (!TargetPackage.StartsWith(
			OwnerPrefix, ESearchCase::CaseSensitive))
		{
			OutError = FString::Printf(
				TEXT("fixture_directory_target_not_strict_descendant:%s:%s"),
				*OwnerPackage,
				*TargetPackage);
			return false;
		}

		IPlatformFile& Files =
			FPlatformFileManager::Get().GetPlatformPhysical();
		FString TargetDirectory = FPackageName::LongPackageNameToFilename(
			TargetPackage);
		FPaths::NormalizeDirectoryName(TargetDirectory);
		if (!Files.DirectoryExists(*TargetDirectory)
			&& !Files.FileExists(*TargetDirectory))
		{
			return true;
		}

		FString CurrentDirectory = FPackageName::LongPackageNameToFilename(
			OwnerPackage);
		FPaths::NormalizeDirectoryName(CurrentDirectory);
		if (!TryVerifyDirectorySegment(Files, CurrentDirectory, OutError))
		{
			return false;
		}
		FString Relative = TargetPackage.RightChop(OwnerPrefix.Len());
		TArray<FString> Segments;
		Relative.ParseIntoArray(Segments, TEXT("/"), true);
		for (const FString& Segment : Segments)
		{
			CurrentDirectory /= Segment;
			if (!TryVerifyDirectorySegment(
				Files, CurrentDirectory, OutError))
			{
				return false;
			}
		}

		FString FirstEntry;
		const bool bEnumeratedEmpty = Files.IterateDirectory(
			*TargetDirectory,
			[&FirstEntry](const TCHAR* Path, const bool)
			{
				FirstEntry = Path;
				return false;
			});
		if (!FirstEntry.IsEmpty())
		{
			OutError = FString::Printf(
				TEXT("fixture_directory_not_empty:%s:%s"),
				*TargetDirectory,
				*FirstEntry);
			return false;
		}
		if (!bEnumeratedEmpty)
		{
			OutError = FString::Printf(
				TEXT("fixture_directory_enumeration_failed:%s"),
				*TargetDirectory);
			return false;
		}
		if (!Files.DeleteDirectory(*TargetDirectory))
		{
			OutError = FString::Printf(
				TEXT("fixture_directory_delete_failed:%s"),
				*TargetDirectory);
			return false;
		}
		if (Files.DirectoryExists(*TargetDirectory)
			|| Files.FileExists(*TargetDirectory))
		{
			OutError = FString::Printf(
				TEXT("fixture_directory_delete_readback_failed:%s"),
				*TargetDirectory);
			return false;
		}
		return true;
	}
}
