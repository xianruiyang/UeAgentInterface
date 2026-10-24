#include "Utilities/AuthoringWorkspaceApply/UeAgentAuthoringDurableDirectory.h"

#include "Core/Contract/UeAgentDiagnostic.h"
#include "Utilities/UeAgentFileOperationDiagnostic.h"
#include "Dom/JsonValue.h"

#include "GenericPlatform/GenericPlatformFile.h"
#include "Misc/Paths.h"
#include "Misc/SecureHash.h"

namespace UeAgentAuthoringWorkspaceTransaction::DurableDirectory
{
	namespace
	{
		bool SafePath(IPlatformFile& Files, const FString& Path, UE::UAI::Contract::FDiagnostic& Error)
		{
			FString Current = Path;
			while (!Current.IsEmpty())
			{
				const auto Link = Files.IsSymlink(*Current);
				if (Link != ESymlinkResult::NonSymlink)
				{ Error = UE::UAI::Contract::FDiagnostic::Failure(TEXT("durable_directory_link_unsupported"), TEXT("The durable directory path contains a symbolic link or its link status cannot be determined."), TEXT("durable_directory_snapshot"), Current); return false; }
				const FString Parent = FPaths::GetPath(Current);
				if (Parent == Current) break;
				Current = Parent;
			}
			return true;
		}
		bool FileDigest(IPlatformFile& Files, const FString& Path, FString& Digest, UE::UAI::Contract::FDiagnostic& Error)
		{
			TUniquePtr<IFileHandle> Handle(Files.OpenRead(*Path));
			if (!Handle) { Error = UE::UAI::FileDiagnostics::MakeFileOperationFailure(TEXT("durable_directory_file_read_failed"), TEXT("A durable directory file could not be opened for snapshot reading."), TEXT("durable_directory_snapshot"), Path); return false; }
			FMD5 Hash;
			uint8 Buffer[65536];
			int64 Remaining = Handle->Size();
			if (Remaining < 0) { Error = UE::UAI::Contract::FDiagnostic::Failure(TEXT("durable_directory_file_size_failed"), TEXT("The durable file handle did not report a valid byte length."), TEXT("durable_directory_snapshot"), Path); Error.ExpectedValue = MakeShared<FJsonValueString>(TEXT("nonnegative byte length")); Error.ActualValue = MakeShared<FJsonValueNumber>(Remaining); return false; }
			while (Remaining > 0)
			{
				const int32 Count = static_cast<int32>(FMath::Min<int64>(Remaining, sizeof(Buffer)));
				if (!Handle->Read(Buffer, Count)) { Error = UE::UAI::FileDiagnostics::MakeFileOperationFailure(TEXT("durable_directory_file_read_failed"), TEXT("Reading a durable directory file failed before its declared size was consumed."), TEXT("durable_directory_snapshot"), Path); Error.ResourceDimension = TEXT("unread_bytes"); Error.ResourceObserved = Remaining; return false; }
				Hash.Update(Buffer, Count);
				Remaining -= Count;
			}
			uint8 Bytes[16];
			Hash.Final(Bytes);
			Digest = BytesToHex(Bytes, 16).ToLower();
			return true;
		}
	}

	bool Snapshot(const FString& RootInput, bool& bExists, FString& Hash, UE::UAI::Contract::FDiagnostic& Error)
	{
		bExists = false;
		Hash.Reset();
		IPlatformFile& Files = IPlatformFile::GetPlatformPhysical();
		FString Root = FPaths::ConvertRelativePathToFull(RootInput);
		FPaths::NormalizeDirectoryName(Root);
		if (!SafePath(Files, Root, Error)) return false;
		if (Files.FileExists(*Root)) { Error = UE::UAI::Contract::FDiagnostic::Failure(TEXT("durable_artifact_type_conflict"), TEXT("A file occupies the path reserved for a durable directory tree."), TEXT("durable_directory_snapshot"), Root); Error.ExpectedValue = MakeShared<FJsonValueString>(TEXT("directory")); Error.ActualValue = MakeShared<FJsonValueString>(TEXT("file")); return false; }
		bExists = Files.DirectoryExists(*Root);
		if (!bExists) return true;
		TArray<FString> Pending{Root};
		TArray<FString> Records{TEXT("D0:")};
		while (!Pending.IsEmpty())
		{
			const FString Directory = Pending.Pop(EAllowShrinking::No);
			const bool bVisited = Files.IterateDirectory(*Directory, [&](const TCHAR* Entry, bool bDirectory)
			{
				FString Path(Entry);
				FPaths::NormalizeFilename(Path);
				if (!Path.StartsWith(Root + TEXT("/"), ESearchCase::IgnoreCase) || !SafePath(Files, Path, Error))
				{ if (Error.IsEmpty()) Error = UE::UAI::Contract::FDiagnostic::Failure(TEXT("durable_directory_path_escape"), TEXT("Directory enumeration produced an entry outside the declared durable root."), TEXT("durable_directory_snapshot"), Path); return false; }
				const FString Relative = Path.Mid(Root.Len() + 1);
				FString Record = FString::Printf(TEXT("%c%d:%s"), bDirectory ? TEXT('D') : TEXT('F'), Relative.Len(), *Relative);
				if (bDirectory) Pending.Add(Path);
				else
				{
					FString Digest;
					if (!FileDigest(Files, Path, Digest, Error)) return false;
					Record += TEXT(":") + Digest;
				}
				Records.Add(MoveTemp(Record));
				return true;
			});
			if (!bVisited) { if (Error.IsEmpty()) Error = UE::UAI::FileDiagnostics::MakeFileOperationFailure(TEXT("durable_directory_enumeration_failed"), TEXT("Enumerating the durable directory failed."), TEXT("durable_directory_snapshot"), Directory); return false; }
		}
		Records.Sort([](const auto& A, const auto& B) { return A.Compare(B, ESearchCase::CaseSensitive) < 0; });
		FMD5 Digest;
		for (const auto& Record : Records)
		{
			const FTCHARToUTF8 Text(*Record);
			const FString Length = FString::FromInt(Text.Length()) + TEXT(":");
			const FTCHARToUTF8 Prefix(*Length);
			Digest.Update(reinterpret_cast<const uint8*>(Prefix.Get()), Prefix.Length());
			Digest.Update(reinterpret_cast<const uint8*>(Text.Get()), Text.Length());
		}
		uint8 Bytes[16];
		Digest.Final(Bytes);
		Hash = BytesToHex(Bytes, 16).ToLower();
		return true;
	}

	bool Remove(const FString& Root, UE::UAI::Contract::FDiagnostic& Error)
	{
		bool bExists;
		FString Hash;
		if (!Snapshot(Root, bExists, Hash, Error)) return false;
		if (!bExists) return true;
		auto& Files = IPlatformFile::GetPlatformPhysical();
		if (!Files.DeleteDirectoryRecursively(*Root))
		{ Error = UE::UAI::FileDiagnostics::MakeFileOperationFailure(TEXT("durable_directory_delete_failed"), TEXT("Deleting the durable directory tree failed."), TEXT("durable_directory_remove"), Root); return false; }
		if (Files.DirectoryExists(*Root))
		{ Error = UE::UAI::Contract::FDiagnostic::Failure(TEXT("durable_directory_delete_failed"), TEXT("The durable directory still exists after deletion."), TEXT("durable_directory_remove"), Root); Error.ExpectedValuePresent = false; Error.ActualValuePresent = true; return false; }
		return true;
	}

	bool Copy(const FString& Destination, const FString& Source, UE::UAI::Contract::FDiagnostic& Error)
	{
		bool bSourceExists, bDestinationExists;
		FString Before, Existing;
		if (!Snapshot(Source, bSourceExists, Before, Error)) return false;
		if (!bSourceExists) {
			Error = UE::UAI::Contract::FDiagnostic::Failure(TEXT("durable_directory_source_missing"), TEXT("The source durable directory does not exist."), TEXT("durable_directory_copy"), Source);
			Error.ExpectedValuePresent = true; Error.ActualValuePresent = false;
			return false;
		}
		if (!Snapshot(Destination, bDestinationExists, Existing, Error)) return false;
		if (bDestinationExists && !Remove(Destination, Error)) return false;
		auto& Files = IPlatformFile::GetPlatformPhysical();
		if (!Files.CopyDirectoryTree(*Destination, *Source, true)) { Error = UE::UAI::FileDiagnostics::MakeFileOperationFailure(TEXT("durable_directory_copy_failed"), TEXT("Copying the durable directory tree failed."), TEXT("durable_directory_copy"), Destination); Error.SubjectId = Source; return false; }
		FString After;
		if (!Snapshot(Destination, bDestinationExists, After, Error)) return false;
		if (!bDestinationExists || Before != After) {
			Error = UE::UAI::Contract::FDiagnostic::Failure(TEXT("durable_directory_copy_verification_failed"),
				!bDestinationExists ? TEXT("The destination directory is absent after copying.") : TEXT("The copied directory tree differs from the source snapshot."), TEXT("durable_directory_copy"), Destination, Source);
			Error.ExpectedValuePresent = true; Error.ActualValuePresent = bDestinationExists;
			Error.ExpectedEvidence = MakeShared<FJsonValueString>(Before); Error.ActualEvidence = MakeShared<FJsonValueString>(After);
			return false;
		}
		return true;
	}

}
