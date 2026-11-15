#include "Core/ArtifactPublisher/UeAgentArtifactPublisher.h"
#include "Core/ArtifactPublisher/UeAgentArtifactPlanBytes.h"
#include "Async/ParallelFor.h"
#include <atomic>

#include "Core/ArtifactPublisher/UeAgentArtifactPublisherTestHooks.h"
#include "Core/ArtifactPublisher/UeAgentArtifactGenerationFiles.h"
#include "Core/HjsonWorkspace/UeAgentHjsonValidatedHeader.h"
#include "Core/HjsonWorkspace/UeAgentHjsonSyntaxCache.h"
#include "Core/HjsonWorkspace/UeAgentSemanticWorkspaceResolver.h"
#include "Core/HjsonWorkspace/UeAgentWorkspacePublication.h"
#include "Core/HjsonWorkspace/UeAgentWorkspaceFileProof.h"
#include "CoreGlobals.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "HAL/CriticalSection.h"
#include "HAL/FileManager.h"
#include "HAL/PlatformMisc.h"
#include "HAL/PlatformFileManager.h"
#include "HAL/PlatformProcess.h"
#include "HAL/PlatformTime.h"
#include "Misc/FileHelper.h"
#include "Misc/Guid.h"
#include "Misc/Paths.h"
#include "Misc/ScopeLock.h"
#include "Misc/ScopeExit.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"
#include "Serialization/JsonWriter.h"
#if PLATFORM_WINDOWS
#include "Windows/WindowsHWrapper.h"
#endif

#define UI OpenSslUi
#include <openssl/sha.h>
#undef UI

namespace UE::UAI::ArtifactPublisher
{
	using namespace UE::UAI::ContextResource;
	using namespace UE::UAI::HjsonWorkspace;

	namespace
	{
		const TCHAR* OwnerMarkerName = TEXT(".uai-export-owner.json");
		const TCHAR* FormatId = TEXT("uai.c8f.artifact_publish.v1");

		struct FPublishPaths
		{
			FString Final;
			FString Parent;
			FString Staging;
			FString Backup;
			FString Journal;
			FString StagingOwner;
			FString BackupOwner;
			FString FinalOwner;
			FString TransactionId;
			bool bSingleFile = false;
		};

		FCriticalSection FailureMutex;
		int32 AfterFinalMovedToBackupFailures = 0;
		int32 FirstMoveAttemptFailures = 0;

		// Encoding facts from the exact staged bytes are handed to the next source
		// reader. They are not syntax or semantic admission proofs.
		struct FArtifactTextEncodings
		{
			TMap<FString, TSharedPtr<const FWorkspaceTextEncodingProof>> TextEncodings;
		};
		thread_local TSharedPtr<const FArtifactTextEncodings> PreviousArtifactEncodings;

		void SetError(
			FArtifactPublishError& OutError,
			FString Code,
			FString Reason,
			FString Target = FString(),
			FString Detail = FString())
		{
			OutError = UE::UAI::Contract::FDiagnostic::Failure(MoveTemp(Code), MoveTemp(Reason), TEXT("artifact_publish"), MoveTemp(Detail));
			OutError.SourceKind = TEXT("native_result");
			OutError.TargetPath = MoveTemp(Target);
		}

		void SetPointer(FArtifactPublishError& Error, const FString& Pointer)
		{
			Error.PropertyPath = Pointer;
			Error.SemanticPointer = Pointer;
			Error.Source.JsonPath = Pointer;
		}

		void SetPrivateComparison(FArtifactPublishError& Error, const FString& Actual, const FString& Expected)
		{
			Error.ActualEvidence = MakeShared<FJsonValueString>(Actual);
			Error.ExpectedEvidence = MakeShared<FJsonValueString>(Expected);
		}

		FString Sha256(const TArray<uint8>& Bytes)
		{
			uint8 Digest[SHA256_DIGEST_LENGTH] = {};
			const uint8 Empty = 0;
			SHA256(
				Bytes.IsEmpty() ? &Empty : Bytes.GetData(),
				static_cast<size_t>(Bytes.Num()),
				Digest);
			FString Hex;
			Hex.Reserve(SHA256_DIGEST_LENGTH * 2);
			for (const uint8 Byte : Digest)
			{
				Hex += FString::Printf(TEXT("%02x"), Byte);
			}
			return TEXT("sha256:") + Hex;
		}

		void AppendUtf8(TArray<uint8>& Target, const FString& Value)
		{
			const FTCHARToUTF8 Converted(*Value);
			Target.Append(
				reinterpret_cast<const uint8*>(Converted.Get()),
				Converted.Length());
		}

		FString InventoryHash(
			const TArray<FString>& Paths,
			const TMap<FString, FString>& Hashes)
		{
			TArray<uint8> Bytes;
			for (const FString& Path : Paths)
			{
				AppendUtf8(Bytes, Path);
				Bytes.Add(0);
				AppendUtf8(Bytes, Hashes.FindChecked(Path));
				Bytes.Add(0);
			}
			return Sha256(Bytes);
		}

		FString PublishedArtifactId(
			const FArtifactPublishRequest& Request,
			const FString& FinalPath,
			const FString& Inventory)
		{
			const FExportWorkspacePlanRef& Plan = Request.GetPlan();
			TArray<uint8> Bytes;
			for (const FString& Value : {
				Request.GetPublisherOwnerId(),
				FinalPath,
				Plan->GetPlanFingerprint(),
				Plan->GetSnapshotId(),
				Plan->GetSnapshotEvidenceId(),
				Inventory})
			{
				AppendUtf8(Bytes, Value);
				Bytes.Add(0);
			}
			return Sha256(Bytes);
		}

		bool ValidateRelativePath(const FString& Input, FString& OutNormalized, FArtifactPublishError& OutError)
		{
			OutNormalized = Input;
			OutNormalized.ReplaceInline(TEXT("\\"), TEXT("/"));
			if (Input.Contains(TEXT("\\"))
				|| OutNormalized.IsEmpty()
				|| FPaths::IsRelative(OutNormalized) == false
				|| OutNormalized.StartsWith(TEXT("/"))
				|| OutNormalized.EndsWith(TEXT("/")))
			{
				SetError(OutError, TEXT("artifact_path_invalid"),
					Input.Contains(TEXT("\\")) ? TEXT("Export file paths must use forward slashes.")
						: OutNormalized.IsEmpty() ? TEXT("An export file path cannot be empty.")
						: !FPaths::IsRelative(OutNormalized) || OutNormalized.StartsWith(TEXT("/"))
							? TEXT("Export file paths must be relative to the workspace.")
							: TEXT("An export file path cannot end with a directory separator."), Input);
				OutError.ActualValue = MakeShared<FJsonValueString>(Input);
				OutError.ExpectedValue = MakeShared<FJsonValueString>(TEXT("a nonempty relative file path using / separators"));
				return false;
			}
			TArray<FString> Parts;
			OutNormalized.ParseIntoArray(Parts, TEXT("/"), false);
			if (Parts.IsEmpty())
			{
				SetError(OutError, TEXT("artifact_path_invalid"), TEXT("The export path contains no file name."), Input);
				return false;
			}
			for (int32 Index = 0; Index < Parts.Num(); ++Index)
			{
				const FString& Part = Parts[Index];
				if (Part.IsEmpty() || Part == TEXT(".") || Part == TEXT(".."))
				{
					SetError(OutError, TEXT("artifact_path_invalid"), TEXT("Export paths cannot contain empty, '.' or '..' segments."), Input);
					SetPointer(OutError, FString::Printf(TEXT("/path/segments/%d"), Index));
					OutError.ActualValue = MakeShared<FJsonValueString>(Part);
					OutError.ExpectedValue = MakeShared<FJsonValueString>(TEXT("a nonempty file or directory name"));
					return false;
				}
			}
			return true;
		}

		FString NormalizeFinalPath(const FString& Input)
		{
			FString Result = FPaths::ConvertRelativePathToFull(Input);
			FPaths::NormalizeFilename(Result);
			FPaths::CollapseRelativeDirectories(Result);
			return Result;
		}

		bool BuildPaths(
			const FArtifactPublishRequest& Request,
			FPublishPaths& OutPaths,
			FArtifactPublishError& OutError)
		{
			OutPaths.Final = NormalizeFinalPath(Request.GetFinalPath());
			OutPaths.Parent = FPaths::GetPath(OutPaths.Final);
			const FString Leaf = FPaths::GetCleanFilename(OutPaths.Final);
			if (Leaf.IsEmpty() || OutPaths.Parent.IsEmpty())
			{
				SetError(
					OutError,
					TEXT("artifact_final_path_invalid"),
					Leaf.IsEmpty() ? TEXT("The export destination has no file or directory name.")
						: TEXT("The export destination has no parent directory."), Request.GetFinalPath());
				SetPointer(OutError, TEXT("/final_path"));
				return false;
			}
			OutPaths.TransactionId =
				FGuid::NewGuid().ToString(EGuidFormats::Digits);
			const FString Prefix = OutPaths.Parent / (
				TEXT(".") + Leaf + TEXT(".uai-") + OutPaths.TransactionId);
			OutPaths.Staging = Prefix + TEXT(".staging");
			OutPaths.Backup = Prefix + TEXT(".backup");
			OutPaths.Journal = Prefix + TEXT(".journal.json");
			OutPaths.bSingleFile =
				Request.GetPlan()->GetWorkspaceKind()
				== EWorkspaceKind::SingleFile;
			if (OutPaths.bSingleFile)
			{
				OutPaths.StagingOwner =
					Prefix + TEXT(".staging-owner.json");
				OutPaths.BackupOwner =
					Prefix + TEXT(".backup-owner.json");
				OutPaths.FinalOwner =
					OutPaths.Final + TEXT(".uai-export-owner.json");
			}
			else
			{
				OutPaths.StagingOwner =
					OutPaths.Staging / OwnerMarkerName;
				OutPaths.BackupOwner =
					OutPaths.Backup / OwnerMarkerName;
				OutPaths.FinalOwner =
					OutPaths.Final / OwnerMarkerName;
			}
			return true;
		}

		bool SerializeJson(
			const TSharedRef<FJsonObject>& Object,
			FString& OutText)
		{
			const TSharedRef<TJsonWriter<>> Writer =
				TJsonWriterFactory<>::Create(&OutText);
			return FJsonSerializer::Serialize(Object, Writer);
		}

		bool SaveJson(
			const FString& Path,
			const TSharedRef<FJsonObject>& Object,
			const FString& Target,
			const FString& Purpose,
			FArtifactPublishError& OutError)
		{
			FString Text;
			if (!SerializeJson(Object, Text))
			{
				SetError(OutError, TEXT("artifact_metadata_serialize_failed"),
					FString::Printf(TEXT("Could not serialize the export %s."), *Purpose), Target, Path);
				return false;
			}
			if (!FFileHelper::SaveStringToFile(
					Text + TEXT("\n"),
					*Path,
					FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM))
			{
				SetError(OutError, TEXT("artifact_metadata_write_failed"),
					FString::Printf(TEXT("Could not write the export %s."), *Purpose), Target, Path);
				return false;
			}
			return true;
		}

		bool ParseJson(const FString& Text,
			const FString& Path,
			const FString& Target,
			TSharedPtr<FJsonObject>& OutObject,
			FArtifactPublishError& OutError)
		{
			const TSharedRef<TJsonReader<>> Reader =
				TJsonReaderFactory<>::Create(Text);
			const bool bParsed = FJsonSerializer::Deserialize(Reader, OutObject);
			if (!bParsed || !OutObject.IsValid())
			{
				SetError(OutError, TEXT("artifact_owner_marker_parse_failed"),
					!Reader->GetErrorMessage().IsEmpty() ? Reader->GetErrorMessage()
						: TEXT("The export ownership record does not contain a JSON object."), Target, Path);
				OutError.Source.SourceUnit = FPaths::GetCleanFilename(Path);
				OutError.Source.Line = static_cast<int32>(Reader->GetLineNumber());
				OutError.Source.Column = static_cast<int32>(Reader->GetCharacterNumber());
				return false;
			}
			return true;
		}

		bool LoadJson(const FString& Path, const FString& Target,
			TSharedPtr<FJsonObject>& OutObject, FArtifactPublishError& OutError)
		{
			FString Text;
			if (!FFileHelper::LoadFileToString(Text, *Path)) {
				SetError(OutError, TEXT("artifact_owner_marker_read_failed"),
					TEXT("The export ownership record could not be read."), Target, Path);
				return false;
			}
			return ParseJson(Text, Path, Target, OutObject, OutError);
		}

		void SetMarkerFieldError(const TSharedPtr<FJsonObject>& Marker, const FString& Key,
			const FString& ExpectedType, const FString& Target, FArtifactPublishError& OutError,
			const FString& ParentPointer = FString())
		{
			SetError(OutError, TEXT("artifact_owner_marker_field_invalid"),
				Marker->Values.Contains(Key)
					? FString::Printf(TEXT("An export ownership record field must be %s."), *ExpectedType)
					: TEXT("The export ownership record is missing a required field."), Target);
			SetPointer(OutError, ParentPointer + TEXT("/") + EscapeJsonPointerSegment(Key));
			OutError.ActualValuePresent = Marker->Values.Contains(Key);
			OutError.ActualEvidence = Marker->Values.FindRef(Key);
			OutError.ExpectedValue = MakeShared<FJsonValueString>(ExpectedType);
		}

		bool ReadMarkerString(const TSharedPtr<FJsonObject>& Marker, const FString& Key,
			FString& OutValue, const FString& Target, FArtifactPublishError& OutError,
			const FString& ParentPointer = FString())
		{
			if (Marker->TryGetStringField(Key, OutValue)) return true;
			SetMarkerFieldError(Marker, Key, TEXT("a string"), Target, OutError, ParentPointer);
			return false;
		}

		bool ValidateMatchingOwnerRecord(const FString& MarkerPath, const FString& FinalPath,
			const FString& OwnerId, TSharedPtr<FJsonObject>& Marker, FArtifactPublishError& OutError)
		{
			FString Format, MarkerOwner, MarkerFinal;
			if (!ReadMarkerString(Marker, TEXT("format"), Format, FinalPath, OutError)) return false;
			if (Format != FormatId)
			{
				SetError(OutError, TEXT("artifact_owner_marker_format_mismatch"), TEXT("The export ownership record uses an unsupported format."), FinalPath, MarkerPath);
				SetPointer(OutError, TEXT("/format"));
				SetPrivateComparison(OutError, Format, FormatId);
				return false;
			}
			if (!ReadMarkerString(Marker, TEXT("publisher_owner_id"), MarkerOwner, FinalPath, OutError)) return false;
			if (MarkerOwner != OwnerId)
			{
				SetError(OutError, TEXT("artifact_owner_marker_owner_mismatch"), TEXT("The existing export belongs to a different publisher."), FinalPath, MarkerPath);
				SetPointer(OutError, TEXT("/publisher_owner_id"));
				SetPrivateComparison(OutError, MarkerOwner, OwnerId);
				return false;
			}
			if (!ReadMarkerString(Marker, TEXT("final_path"), MarkerFinal, FinalPath, OutError)) return false;
			if (!NormalizeFinalPath(MarkerFinal).Equals(FinalPath, ESearchCase::IgnoreCase))
			{
				SetError(OutError, TEXT("artifact_owner_marker_path_mismatch"), TEXT("The ownership record identifies a different export destination."), FinalPath, MarkerPath);
				SetPointer(OutError, TEXT("/final_path"));
				SetPrivateComparison(OutError, MarkerFinal, FinalPath);
				return false;
			}
			return true;
		}

		bool LoadMatchingOwnerRecord(const FString& MarkerPath, const FString& FinalPath,
			const FString& OwnerId, TSharedPtr<FJsonObject>& Marker, FArtifactPublishError& OutError)
		{
			return LoadJson(MarkerPath, FinalPath, Marker, OutError)
				&& ValidateMatchingOwnerRecord(MarkerPath, FinalPath, OwnerId, Marker, OutError);
		}

		bool ExistingFinalIsOwned(
			const FPublishPaths& Paths,
			const FString& OwnerId,
			FArtifactPublishError& OutError,
			bool bDeferDirectoryReadOnly = false)
		{
			IFileManager& Files = IFileManager::Get();
			const bool bFinalExists = Paths.bSingleFile
				? Files.FileExists(*Paths.Final)
				: Files.DirectoryExists(*Paths.Final);
			if (!bFinalExists)
			{
				const bool bWrongKindExists = Paths.bSingleFile
					? Files.DirectoryExists(*Paths.Final)
					: Files.FileExists(*Paths.Final);
				if (bWrongKindExists || Files.FileExists(*Paths.FinalOwner))
				{
					SetError(
						OutError,
						TEXT("artifact_final_ownership_required"),
						bWrongKindExists ? TEXT("The export destination exists with the wrong filesystem kind.")
							: TEXT("An ownership record exists without its export destination."), Paths.Final);
					OutError.ActualValue = MakeShared<FJsonValueString>(bWrongKindExists
						? (Paths.bSingleFile ? TEXT("directory") : TEXT("file")) : TEXT("ownership record without destination"));
					OutError.ExpectedValue = MakeShared<FJsonValueString>(Paths.bSingleFile ? TEXT("file or unused destination") : TEXT("directory or unused destination"));
					return false;
				}
				return true;
			}
			if (!Paths.bSingleFile)
			{
				TArray<FString> Existing;
				Files.FindFiles(
					Existing,
					*(Paths.Final / TEXT("*")),
					true,
					true);
				if (Existing.IsEmpty())
				{
					return true;
				}
			}
			TSharedPtr<FJsonObject> Marker;
			FArtifactPublishError MarkerError;
			if (!LoadMatchingOwnerRecord(Paths.FinalOwner, Paths.Final, OwnerId, Marker, MarkerError))
			{
				SetError(
					OutError,
					TEXT("artifact_final_ownership_required"),
					TEXT("The existing export destination has no matching ownership record."), Paths.Final);
				OutError.PublicCause = MakeShared<FArtifactPublishError>(MarkerError);
				return false;
			}
			IPlatformFile& PlatformFile =
				FPlatformFileManager::Get().GetPlatformFile();
			if (Paths.bSingleFile)
			{
				const bool bFinalReadOnly = PlatformFile.IsReadOnly(*Paths.Final);
				if (bFinalReadOnly || PlatformFile.IsReadOnly(*Paths.FinalOwner))
				{
					SetError(
						OutError,
						TEXT("artifact_final_read_only"),
						bFinalReadOnly ? TEXT("The existing export file is read-only.")
							: TEXT("The existing export ownership record is read-only."), Paths.Final);
					return false;
				}
			}
			else if (!bDeferDirectoryReadOnly)
			{
				TArray<FString> ExistingFiles;
				Files.FindFilesRecursive(
					ExistingFiles,
					*Paths.Final,
					TEXT("*"),
					true,
					false);
				for (const FString& ExistingFile : ExistingFiles)
				{
					if (PlatformFile.IsReadOnly(*ExistingFile))
					{
						SetError(
							OutError,
							TEXT("artifact_final_read_only"),
							TEXT("The existing export workspace contains a read-only file."), ExistingFile);
						return false;
					}
				}
			}
			return true;
		}

		bool DeleteDirectory(const FString& Path, const FString& Target, const FString& Purpose, FArtifactPublishError& OutError)
		{
			if (!IFileManager::Get().DirectoryExists(*Path) || IFileManager::Get().DeleteDirectory(*Path, false, true)) return true;
			SetError(OutError, TEXT("artifact_directory_delete_failed"),
				FString::Printf(TEXT("Could not delete the export %s directory."), *Purpose), Target, Path);
			return false;
		}

		bool DeleteFile(const FString& Path, const FString& Target, const FString& Purpose, FArtifactPublishError& OutError)
		{
			if (!IFileManager::Get().FileExists(*Path) || IFileManager::Get().Delete(*Path, false, true, true)) return true;
			SetError(OutError, TEXT("artifact_file_delete_failed"),
				FString::Printf(TEXT("Could not delete the export %s file."), *Purpose), Target, Path);
			return false;
		}

		bool DeleteDataPath(const FPublishPaths& Paths, const FString& Path, const FString& Purpose, FArtifactPublishError& OutError)
		{
			return Paths.bSingleFile
				? DeleteFile(Path, Paths.Final, Purpose, OutError)
				: DeleteDirectory(Path, Paths.Final, Purpose, OutError);
		}

		bool MovePath(
			const FString& Destination,
			const FString& Source,
			const FString& Target,
			FArtifactPublishError& OutError)
		{
			// A publish commit is a same-parent rename. Keep every attempt atomic;
			// do not fall back to copying a partially visible directory tree.
			if (!FPaths::GetPath(Destination).Equals(
					FPaths::GetPath(Source), ESearchCase::IgnoreCase))
			{
				SetError(OutError, TEXT("artifact_move_parent_mismatch"),
					TEXT("An atomic export rename requires source and destination in the same parent directory."), Target,
					FString::Printf(TEXT("source=%s destination=%s"), *Source, *Destination));
				return false;
			}
			IPlatformFile& Files =
				FPlatformFileManager::Get().GetPlatformFile();
			constexpr int32 MaxAttempts = 3;
			bool bInjectedFirstFailure = false;
			uint32 LastPlatformError = 0;
			for (int32 Attempt = 0; Attempt < MaxAttempts; ++Attempt)
			{
				bool bInjectFailure = false;
				{
					FScopeLock Lock(&FailureMutex);
					bInjectFailure = FirstMoveAttemptFailures > 0
						&& !bInjectedFirstFailure;
				}
				bInjectedFirstFailure |= bInjectFailure;
				if (!bInjectFailure && Files.MoveFile(*Destination, *Source))
				{
					return true;
				}
				LastPlatformError = bInjectFailure
					? 0
					: FPlatformMisc::GetLastError();

				// A missing source or an occupied destination is a permanent state
				// error. Only retry a transient platform rename failure.
				const bool bSourceExists = Files.FileExists(*Source)
					|| Files.DirectoryExists(*Source);
				const bool bDestinationExists = Files.FileExists(*Destination)
					|| Files.DirectoryExists(*Destination);
				if (!bSourceExists || bDestinationExists
					|| Attempt + 1 == MaxAttempts)
				{
					FString Reason = !bSourceExists ? TEXT("The export rename failed; its source is no longer present.")
						: bDestinationExists ? TEXT("The export rename failed; its destination is occupied.")
							: TEXT("The filesystem could not complete the atomic export rename after three attempts.");
					if (LastPlatformError != 0)
					{
						TCHAR NativeMessage[1024] = {};
						FPlatformMisc::GetSystemErrorMessage(NativeMessage, UE_ARRAY_COUNT(NativeMessage), static_cast<int32>(LastPlatformError));
						const FString NativeReason = FString(NativeMessage).TrimStartAndEnd();
						if (!NativeReason.IsEmpty()) Reason += TEXT(" System error: ") + NativeReason;
					}
					SetError(OutError, TEXT("artifact_move_failed"), Reason, Target,
						FString::Printf(TEXT("source=%s destination=%s platform_error=%u"), *Source, *Destination, LastPlatformError));
					TSharedRef<FJsonObject> Facts = MakeShared<FJsonObject>();
					Facts->SetBoolField(TEXT("source_exists"), bSourceExists);
					Facts->SetBoolField(TEXT("destination_exists"), bDestinationExists);
					if (LastPlatformError != 0) Facts->SetNumberField(TEXT("platform_error"), LastPlatformError);
					OutError.ActualValue = MakeShared<FJsonValueObject>(Facts);
					return false;
				}
				FPlatformProcess::SleepNoStats(
					Attempt == 0 ? 0.01f : 0.05f);
			}
			SetError(OutError, TEXT("artifact_move_failed"), TEXT("The filesystem did not complete the atomic export rename."), Target,
				FString::Printf(TEXT("source=%s destination=%s platform_error=%u"), *Source, *Destination, LastPlatformError));
			return false;
		}

		bool ShouldFailAfterFinalMovedToBackup()
		{
			FScopeLock Lock(&FailureMutex);
			return AfterFinalMovedToBackupFailures > 0;
		}

		bool ValidatePlan(
			const FArtifactPublishRequest& Request,
			FUeAgentResourceTracker& Resources,
			TArray<FString>& OutPaths,
			TMap<FString, FString>& OutHashes,
			FArtifactPublishError& OutError)
		{
			const FExportWorkspacePlanRef& Plan = Request.GetPlan();
			if (Plan->GetWorkspaceKind() == EWorkspaceKind::Inline)
			{
				SetError(
					OutError,
					TEXT("artifact_workspace_kind_unsupported"),
					TEXT("Inline workspaces cannot be published to a filesystem destination."), Request.GetFinalPath());
				OutError.ActualValue = MakeShared<FJsonValueString>(TEXT("inline"));
				OutError.ExpectedValue = MakeShared<FJsonValueString>(TEXT("directory or single_file"));
				return false;
			}
			if (Plan->GetFiles().IsEmpty())
			{
				SetError(OutError, TEXT("artifact_plan_invalid"), TEXT("The export plan contains no files."), Request.GetFinalPath());
				SetPointer(OutError, TEXT("/files"));
				OutError.ActualValue = MakeShared<FJsonValueNumber>(0);
				OutError.ExpectedValue = MakeShared<FJsonValueString>(TEXT("at least one file"));
				return false;
			}
			if (Plan->GetWorkspaceKind() == EWorkspaceKind::SingleFile
				&& Plan->GetFiles().Num() != 1)
			{
				SetError(
					OutError,
					TEXT("artifact_plan_invalid"),
					TEXT("A single-file export plan must contain exactly one file."), Plan->GetEntryFile());
				SetPointer(OutError, TEXT("/files"));
				OutError.ActualValue = MakeShared<FJsonValueNumber>(Plan->GetFiles().Num());
				OutError.ExpectedValue = MakeShared<FJsonValueNumber>(1);
				return false;
			}

			int64 OutputBytes = 0;
			const double ValidationStarted = FPlatformTime::Seconds();
			int64 HashedBytes = 0, ProofBytes = 0, ProvenFiles = 0;
			TSet<FString> Folded;
			for (const FExportFilePlan& File : Plan->GetFiles())
			{
				FString Path;
				if (!ValidateRelativePath(File.GetRelativePath(), Path, OutError)) return false;
				if (Path.Equals(OwnerMarkerName, ESearchCase::IgnoreCase))
				{
					SetError(
						OutError,
						TEXT("artifact_path_invalid"),
						TEXT("An exported file cannot replace the reserved ownership record."), File.GetRelativePath());
					return false;
				}
				const FString Key = Path.ToLower();
				if (Folded.Contains(Key))
				{
					SetError(
						OutError,
						TEXT("artifact_path_collision"),
						TEXT("The export plan contains file paths that collide when compared case-insensitively."), Path);
					return false;
				}
				Folded.Add(Key);
				const bool bProven = File.HasVerifiedCanonicalUtf8();
				const FString ActualHash = bProven ? File.GetContentHash() : Sha256(File.GetCanonicalUtf8());
				(bProven ? ProofBytes : HashedBytes) += File.GetCanonicalUtf8().Num();
				if (ActualHash != File.GetContentHash())
				{
					SetError(
						OutError,
						TEXT("artifact_content_hash_mismatch"),
						TEXT("The planned file bytes do not match the plan's declared content hash."), Path);
					SetPrivateComparison(OutError, ActualHash, File.GetContentHash());
					return false;
				}
				OutputBytes += File.GetCanonicalUtf8().Num();
				RememberVerifiedPlanBytes(File);
				ProvenFiles += bProven ? 1 : 0;
				OutPaths.Add(Path);
				OutHashes.Add(Path, File.GetContentHash());
			}
			for (const auto& File : Plan->GetRetainedFileHashes()) {
				FString Path;
				if (!ValidateRelativePath(File.Key, Path, OutError)) return false;
				if (Path.Equals(OwnerMarkerName, ESearchCase::IgnoreCase) || Folded.Contains(Path.ToLower())) {
					SetError(OutError, TEXT("artifact_path_collision"), TEXT("A retained generated path collides with a planned output or owner record."), Path);
					return false;
				}
				Folded.Add(Path.ToLower()); OutPaths.Add(Path); OutHashes.Add(Path, File.Value);
			}
			FString Entry;
			if (!ValidateRelativePath(Plan->GetEntryFile(), Entry, OutError)) return false;
			if (!Folded.Contains(Entry.ToLower()))
			{
				SetError(
					OutError,
					TEXT("artifact_entry_missing"),
					TEXT("The export entry file is absent from the planned file inventory."), Plan->GetEntryFile());
				OutError.ActualValuePresent = false;
				OutError.ExpectedValue = MakeShared<FJsonValueString>(TEXT("an entry included in the export plan"));
				return false;
			}
			for (const auto& File : Request.GetPreservedFiles())
			{
				FString Path;
				if (Plan->GetWorkspaceKind() != EWorkspaceKind::Directory)
				{
					SetError(OutError, TEXT("artifact_path_invalid"), TEXT("Preserved files require a directory export workspace."), File.Key);
					return false;
				}
				if (!ValidateRelativePath(File.Key, Path, OutError)) return false;
				if (Path.Equals(OwnerMarkerName, ESearchCase::IgnoreCase))
				{
					SetError(OutError, TEXT("artifact_path_invalid"), TEXT("A preserved file cannot replace the reserved ownership record."), File.Key);
					return false;
				}
				if (Folded.Contains(Path.ToLower()))
				{
					SetError(OutError, TEXT("artifact_preserved_file_collision"), TEXT("A preserved file collides with another planned output path."), Path);
					return false;
				}
				Folded.Add(Path.ToLower());
				OutPaths.Add(Path);
				OutHashes.Add(Path, Sha256(File.Value));
				OutputBytes += File.Value.Num();
			}
			OutPaths.Sort();

			FUeAgentResourceChargeError ChargeError;
			if (!Resources.TryChargeMany(
				{
					{EUeAgentResourceDimension::Files, OutPaths.Num()},
					{EUeAgentResourceDimension::OutputBytes, OutputBytes},
					{EUeAgentResourceDimension::WorkUnits,
						FMath::Max<int64>(1, OutPaths.Num())},
					{EUeAgentResourceDimension::EstimatedMemoryBytes,
						OutputBytes},
				},
				TEXT("artifact_publish:preflight"),
				ChargeError))
			{
				OutError = ChargeError;
				if (OutError.TargetPath.IsEmpty()) OutError.TargetPath = Request.GetFinalPath();
				return false;
			}
			UE_LOG(LogTemp, Display, TEXT("UAI export timing phase=artifact_validate_plan elapsed=%.6f hashed_bytes=%lld proven_bytes=%lld proof_entries=%lld"),
				FPlatformTime::Seconds() - ValidationStarted, HashedBytes, ProofBytes, ProvenFiles);
			return true;
		}

		TSharedRef<FJsonObject> BuildOwnerRecord(
			const FArtifactPublishRequest& Request,
			const FString& FinalPath,
			const TArray<FString>& Paths,
			const TMap<FString, FString>& Hashes,
			const FString& Inventory)
		{
			const FExportWorkspacePlanRef& Plan = Request.GetPlan();
			const FUeAgentStateRevision& Revision =
				Plan->GetSnapshotRevision();
			const TSharedRef<FJsonObject> Record = MakeShared<FJsonObject>();
			Record->SetStringField(TEXT("format"), FormatId);
			Record->SetStringField(
				TEXT("publisher_owner_id"),
				Request.GetPublisherOwnerId());
			Record->SetStringField(TEXT("final_path"), FinalPath);
			if (!Request.GetConsumerState().IsEmpty())
				Record->SetStringField(TEXT("consumer_state"), Request.GetConsumerState());
			Record->SetStringField(
				TEXT("workspace_kind"),
				Plan->GetWorkspaceKind() == EWorkspaceKind::SingleFile
					? TEXT("single_file")
					: TEXT("directory"));
			Record->SetStringField(
				TEXT("entry_file"),
				Plan->GetEntryFile());
			Record->SetStringField(
				TEXT("published_artifact_id"),
				PublishedArtifactId(Request, FinalPath, Inventory));
			Record->SetStringField(
				TEXT("plan_fingerprint"),
				Plan->GetPlanFingerprint());
			Record->SetStringField(TEXT("snapshot_id"), Plan->GetSnapshotId());
			const TSharedRef<FJsonObject> RevisionObject =
				MakeShared<FJsonObject>();
			RevisionObject->SetStringField(
				TEXT("source_id"),
				Revision.GetSourceId());
			RevisionObject->SetStringField(
				TEXT("generation"),
				Revision.GetGeneration());
			RevisionObject->SetNumberField(
				TEXT("sequence"),
				static_cast<double>(Revision.GetSequence()));
			Record->SetObjectField(TEXT("snapshot_revision"), RevisionObject);
			Record->SetStringField(
				TEXT("snapshot_evidence_id"),
				Plan->GetSnapshotEvidenceId());
			Record->SetStringField(TEXT("inventory_hash"), Inventory);
			TSet<FString> GeneratedPaths;
			for (const FExportFilePlan& File : Plan->GetFiles())
				GeneratedPaths.Add(File.GetRelativePath().ToLower());
			for (const auto& File : Plan->GetRetainedFileHashes()) GeneratedPaths.Add(File.Key.ToLower());
			TArray<TSharedPtr<FJsonValue>> FileRows;
			for (const FString& Path : Paths)
			{
				const TSharedRef<FJsonObject> Row = MakeShared<FJsonObject>();
				Row->SetStringField(TEXT("relative_path"), Path);
				Row->SetStringField(TEXT("content_hash"), Hashes.FindChecked(Path));
				Row->SetBoolField(TEXT("generated"), GeneratedPaths.Contains(Path.ToLower()));
				FileRows.Add(MakeShared<FJsonValueObject>(Row));
			}
			Record->SetArrayField(TEXT("files"), FileRows);
			return Record;
		}

		bool SaveJournal(
			const FPublishPaths& Paths,
			const FArtifactPublishRequest& Request,
			const FString& State,
			FArtifactPublishError& OutError)
		{
			const TSharedRef<FJsonObject> Record = MakeShared<FJsonObject>();
			Record->SetStringField(TEXT("format"), FormatId);
			Record->SetStringField(
				TEXT("transaction_id"),
				Paths.TransactionId);
			Record->SetStringField(
				TEXT("publisher_owner_id"),
				Request.GetPublisherOwnerId());
			Record->SetStringField(TEXT("final_path"), Paths.Final);
			Record->SetStringField(TEXT("staging_path"), Paths.Staging);
			Record->SetStringField(TEXT("backup_path"), Paths.Backup);
			Record->SetStringField(TEXT("state"), State);
			if (SaveJson(Paths.Journal, Record, Paths.Final, TEXT("transaction journal"), OutError)) return true;
			OutError.InternalContext += TEXT(" state=") + State;
			return false;
		}

		struct FStagedIoTimings { double Write = 0, Read = 0, Compare = 0; };
		bool WriteAndVerifyStagedFile(const FString& Destination, const FString& Relative,
			const TArray<uint8>& Expected,
			const FString& WriteCode, const FString& VerifyCode,
			TArray<uint8>& Observed, FArtifactPublishError& OutError, bool bGeneration = false, bool bParentReady = false,
			FStagedIoTimings* Timings = nullptr)
		{
			if (!bGeneration && !bParentReady && !IFileManager::Get().MakeDirectory(*FPaths::GetPath(Destination), true))
			{
				SetError(OutError, WriteCode, TEXT("Could not create the staging file's parent directory."), Relative, Destination);
				return false;
			}
			const double WriteStarted = FPlatformTime::Seconds();
			const bool bWritten = bGeneration ? WriteGenerationFile(Destination, Expected, OutError) : FFileHelper::SaveArrayToFile(Expected, *Destination);
			if (Timings) Timings->Write += FPlatformTime::Seconds() - WriteStarted;
			if (!bWritten)
			{
				if (OutError.IsEmpty()) SetError(OutError, WriteCode, TEXT("Could not write the staged export file."), Relative, Destination);
				return false;
			}
			FGenerationFileGuard StagedRead;
			const double ReadStarted = FPlatformTime::Seconds();
			const bool bRead = bGeneration ? (StagedRead.Open(Destination, false, false, OutError) && StagedRead.Read(Observed, OutError))
				: FFileHelper::LoadFileToArray(Observed, *Destination);
			if (Timings) Timings->Read += FPlatformTime::Seconds() - ReadStarted;
			if (!bRead)
			{
				if (OutError.IsEmpty()) SetError(OutError, VerifyCode, TEXT("Could not read back the staged export file."), Relative, Destination);
				return false;
			}
			const double CompareStarted = FPlatformTime::Seconds();
			const bool bSameBytes = Observed == Expected;
			if (Timings) Timings->Compare += FPlatformTime::Seconds() - CompareStarted;
			if (!bSameBytes)
			{
				int32 Offset = 0;
				while (Offset < Observed.Num() && Offset < Expected.Num() && Observed[Offset] == Expected[Offset]) ++Offset;
				SetError(OutError, VerifyCode, TEXT("The staged export file bytes differ from the bytes written."), Relative, Destination);
				SetPointer(OutError, FString::Printf(TEXT("/bytes/%d"), Offset));
				OutError.ActualValuePresent = Observed.IsValidIndex(Offset);
				OutError.ExpectedValuePresent = Expected.IsValidIndex(Offset);
				if (Observed.IsValidIndex(Offset)) OutError.ActualValue = MakeShared<FJsonValueNumber>(Observed[Offset]);
				if (Expected.IsValidIndex(Offset)) OutError.ExpectedValue = MakeShared<FJsonValueNumber>(Expected[Offset]);
				return false;
			}
			// ValidatePlan already bound Expected to the declared hash before
			// staging. Exact readback equality proves the same hash for Observed;
			// hashing the identical bytes again adds no independent observation.
			return true;
		}

		bool StageFiles(
			const FArtifactPublishRequest& Request,
			const FPublishPaths& Paths,
			FUeAgentResourceTracker& Resources,
			const TMap<FString, FString>& Hashes,
			FArtifactPublishError& OutError, const TSet<FString>* ChangedPaths = nullptr)
		{
			const double StageStarted = FPlatformTime::Seconds();
			double WriteReadbackSeconds = 0.0;
			double ParseSeconds = 0.0;
			double EncodingSeconds = 0.0;
			FStagedIoTimings IoTimings;
			int32 ReusedEncodings = 0;
			int64 EncodingBytes = 0;
			const auto EncodingBaseline = PreviousArtifactEncodings;
			TMap<FString, TSharedPtr<const FWorkspaceTextEncodingProof>> TextEncodings;
			IFileManager& Files = IFileManager::Get();
			if (ChangedPaths ? !MakeGenerationDirectory(Paths.Staging, OutError) : !Files.MakeDirectory(
				*(Paths.bSingleFile
					? Paths.Parent
					: Paths.Staging),
				true))
			{
				SetError(
					OutError,
					TEXT("artifact_staging_create_failed"),
					Paths.bSingleFile ? TEXT("Could not create the export destination's parent directory.")
						: TEXT("Could not create the export staging directory."), Paths.Final, Paths.bSingleFile ? Paths.Parent : Paths.Staging);
				return false;
			}
			struct FStagedReadback {
				FString Relative, Destination;
				TArray<uint8> Observed;
				FArtifactPublishError Error;
				FStagedIoTimings IoTimings;
				bool bSucceeded = false;
			};
			const auto& PlannedFiles = Request.GetPlan()->GetFiles();
			const bool bParallelPrivateFiles = !ChangedPaths && !Paths.bSingleFile && PlannedFiles.Num() > 1;
			TArray<FStagedReadback> Readbacks;
			TSet<FString> PreparedParents;
			int32 BatchFirst = 0, BatchEnd = 0;
			for (int32 FileIndex = 0; FileIndex < PlannedFiles.Num(); ++FileIndex)
			{
				const auto& File = PlannedFiles[FileIndex];
				const double WriteStarted = FPlatformTime::Seconds();
				if (bParallelPrivateFiles && FileIndex == BatchEnd) {
					Readbacks.Reset(); BatchFirst = FileIndex;
					int64 BatchBytes = 0;
					int32 PreparationFailure = MAX_int32;
					// At most two independent private-file writers, with bounded
					// readback storage. A single larger file keeps the original bound.
					while (BatchEnd < PlannedFiles.Num() && Readbacks.Num() < 64) {
						const auto& Next = PlannedFiles[BatchEnd];
						const int64 Bytes = Next.GetCanonicalUtf8().Num();
						if (!Readbacks.IsEmpty() && BatchBytes + Bytes > 8LL * 1024 * 1024) break;
						auto& Readback = Readbacks.AddDefaulted_GetRef();
						if (!ValidateRelativePath(Next.GetRelativePath(), Readback.Relative, Readback.Error)) {
							PreparationFailure = Readbacks.Num() - 1; ++BatchEnd; break;
						}
						Readback.Destination = Paths.Staging / Readback.Relative;
						const FString Parent = FPaths::GetPath(Readback.Destination);
						if (!PreparedParents.Contains(Parent)) {
							if (!Files.MakeDirectory(*Parent, true)) {
								SetError(Readback.Error, TEXT("artifact_staging_write_failed"), TEXT("Could not create the staging file's parent directory."), Readback.Relative, Readback.Destination);
								PreparationFailure = Readbacks.Num() - 1; ++BatchEnd; break;
							}
							PreparedParents.Add(Parent);
						}
						BatchBytes += Bytes; ++BatchEnd;
					}
					std::atomic<int32> FirstFailure{PreparationFailure};
					ParallelFor(2, [&](int32 Worker) {
						for (int32 Offset = Worker; Offset < Readbacks.Num(); Offset += 2) {
							if (Offset >= FirstFailure.load()) break;
							auto& Readback = Readbacks[Offset];
							Readback.bSucceeded = WriteAndVerifyStagedFile(Readback.Destination, Readback.Relative,
								PlannedFiles[BatchFirst + Offset].GetCanonicalUtf8(), TEXT("artifact_staging_write_failed"),
								TEXT("artifact_staging_verification_failed"), Readback.Observed, Readback.Error, false, true, &Readback.IoTimings);
							if (!Readback.bSucceeded) {
								int32 Previous = FirstFailure.load();
								while (Offset < Previous && !FirstFailure.compare_exchange_weak(Previous, Offset)) {}
								break;
							}
						}
					});
					// ParallelFor has joined every started writer. Error projection,
					// parser/TLS proofs, resources and cleanup stay on the owner thread.
				}
				// Consume failures in the original file/parse order as well.
				if (bParallelPrivateFiles && !Readbacks[FileIndex - BatchFirst].bSucceeded) {
					OutError = MoveTemp(Readbacks[FileIndex - BatchFirst].Error); return false;
				}
				FString Relative;
				if (bParallelPrivateFiles) Relative = Readbacks[FileIndex - BatchFirst].Relative;
				else if (!ValidateRelativePath(File.GetRelativePath(), Relative, OutError)) return false;
				const FString Destination = Paths.bSingleFile
					? Paths.Staging
					: Paths.Staging / Relative;
				TArray<uint8> Observed;
				const bool bWrite = !ChangedPaths || ChangedPaths->Contains(Relative);
				if (bParallelPrivateFiles) {
					auto& Readback = Readbacks[FileIndex - BatchFirst]; Observed = MoveTemp(Readback.Observed);
					IoTimings.Write += Readback.IoTimings.Write; IoTimings.Read += Readback.IoTimings.Read; IoTimings.Compare += Readback.IoTimings.Compare;
				}
				else if (bWrite && !WriteAndVerifyStagedFile(Destination, Relative, File.GetCanonicalUtf8(),
					TEXT("artifact_staging_write_failed"), TEXT("artifact_staging_verification_failed"), Observed, OutError, ChangedPaths != nullptr, false, &IoTimings)) return false;
				const double ParseStarted = FPlatformTime::Seconds();
				WriteReadbackSeconds += ParseStarted - WriteStarted;
				// The exact readback (or generation owner's observed unchanged
				// bytes) binds this pure encoding fact. It never replaces a later
				// current-path read/hash or kernel content observation.
				if (Request.ShouldRememberSource(Relative) && TextEncodings.Num() < 16384 && EncodingBytes < 8LL * 1024 * 1024) {
					const double EncodingStarted = FPlatformTime::Seconds();
					const auto& EncodingInput = bWrite ? Observed : File.GetCanonicalUtf8();
					TSharedPtr<const FWorkspaceTextEncodingProof> Encoding;
					if (EncodingBaseline) if (const auto* Prior = EncodingBaseline->TextEncodings.Find(Relative))
						if (*Prior && (*Prior)->Matches(Hashes.FindChecked(Relative), EncodingInput.Num())) { Encoding = *Prior; ++ReusedEncodings; }
					// Generated plan files are the common formatter's canonical UTF-8.
					// Staging already compared their actual readback above; only the
					// text length is missing, not another encoding validation pass.
					if (!Encoding) Encoding = MeasureCanonicalWorkspaceTextEncoding(EncodingInput, Hashes.FindChecked(Relative));
					const int64 Bytes = Encoding->GetAllocatedSize() + Relative.GetAllocatedSize() + 128;
					if (Bytes <= 8LL * 1024 * 1024 - EncodingBytes) { TextEncodings.Add(Relative, MoveTemp(Encoding)); EncodingBytes += Bytes; }
					EncodingSeconds += FPlatformTime::Seconds() - EncodingStarted;
				}
				// Formatter-owned canonical bytes are the artifact. Input parsing remains
				// at source admission; publication verifies actual written bytes once.
				ParseSeconds += FPlatformTime::Seconds() - ParseStarted;
			}
			const double GeneratedFinished = FPlatformTime::Seconds();
			for (const auto& File : Request.GetPreservedFiles())
			{
				if (ChangedPaths && !ChangedPaths->Contains(File.Key)) continue;
				const FString Destination = Paths.Staging / File.Key;
				TArray<uint8> Observed;
				if (!WriteAndVerifyStagedFile(Destination, File.Key, File.Value,
					TEXT("artifact_preserved_file_write_failed"), TEXT("artifact_preserved_file_write_failed"), Observed, OutError, ChangedPaths != nullptr, false, &IoTimings)) return false;
			}
			const double PreservedFinished = FPlatformTime::Seconds();
			const double StageFinished = FPlatformTime::Seconds();
			PreviousArtifactEncodings.Reset();
			if (Request.GetPlan()->GetFiles().Num() <= 200000)
			{
				auto Next = MakeShared<FArtifactTextEncodings>();
				Next->TextEncodings = MoveTemp(TextEncodings);
				PreviousArtifactEncodings = MoveTemp(Next);
			}
			UE_LOG(LogTemp, Display, TEXT("UAI export timing phase=artifact_generated_syntax parser_calls=0 syntax_cache_store=0 builder=canonical_formatter encoding_s=%.6f"), EncodingSeconds);
			UE_LOG(LogTemp, Display, TEXT("UAI export timing phase=artifact_io_functions write_s_sum=%.6f read_s_sum=%.6f compare_s_sum=%.6f workers=%d scope=stage_nested_function_wall_sums"),
				IoTimings.Write, IoTimings.Read, IoTimings.Compare, bParallelPrivateFiles ? 2 : 1);
			UE_LOG(LogTemp, Display, TEXT("UAI export timing phase=publish_stage files=%d write_readback_s=%.6f parse_s=%.6f generated_s=%.6f preserved_s=%.6f semantic_verify_s=%.6f total_s=%.6f success=true io_workers=%d"),
				Request.GetPlan()->GetFiles().Num(), WriteReadbackSeconds, ParseSeconds,
				GeneratedFinished - StageStarted, PreservedFinished - GeneratedFinished,
				StageFinished - PreservedFinished, StageFinished - StageStarted, bParallelPrivateFiles ? 2 : 1);
			return true;
		}

		struct FGenerationObservedFile
		{
			FString Hash;
			FWorkspaceFileProofPtr Proof;
		};
		thread_local FString GenerationObservationRoot;
		thread_local TMap<FString, FGenerationObservedFile> GenerationObservations;

		bool ObserveGuardedGenerationFile(const FString& Path, const FGenerationFileGuard& Guard,
            FString& Hash, FWorkspaceFileProofPtr& Proof, FArtifactPublishError& Error,
            bool bRetainObservation = true)
		{
			void* CurrentHandle = Guard.GetCurrentReadHandle(Path);
            if (const auto* Previous = GenerationObservations.Find(Path); bRetainObservation && CurrentHandle && Previous && Previous->Proof && Previous->Proof->IsCurrentHandle(CurrentHandle)) {
				Hash = Previous->Hash; Proof = Previous->Proof; return true;
			}
            bool bBudgetExhausted = false;
            Proof = TryBeginWorkspaceFileObservation(Path, &bBudgetExhausted);
            if (bBudgetExhausted) {
                const auto Usage = GetWorkspaceFileProofUsage(true);
                UE_LOG(LogTemp, Warning, TEXT("UAI export proof budget pending=%d path_characters=%lld generation_observations=%d lease_pending=%d retiring=%d"),
                    Usage.PendingOperations, Usage.PathCharacters, GenerationObservations.Num(),
                    Usage.LeasePendingOperations, Usage.RetiringOperations);
                SetError(Error, TEXT("workspace_publication_guard_budget"),
                    TEXT("The bounded kernel file-proof capacity is exhausted; the generation was not committed."), Path);
                return false;
            }
			TArray<uint8> Bytes;
            if (!CurrentHandle || !Proof) {
				SetError(Error, TEXT("workspace_publication_read_lease_unavailable"),
					TEXT("The file cannot supply a current kernel read lease under the commit guard. A writable mapping or unsupported filesystem cannot use this generation commit."), Path);
				return false;
			}
            if (!Guard.Read(Bytes, Error)) return false;
            if (!Proof->IsCurrentHandle(CurrentHandle)) {
                SetError(Error, TEXT("workspace_publication_read_lease_unavailable"),
                    TEXT("The file lost its kernel read lease or current identity during guarded observation."), Path);
                return false;
            }
			Hash = Sha256(Bytes);
            if (bRetainObservation && (GenerationObservations.Num() < 16384 || GenerationObservations.Contains(Path)))
				GenerationObservations.Add(Path, {Hash, Proof});
			return true;
		}

		FString GenerationJournalPath(const FString& Root) { return Root + TEXT(".uai-publication.json"); }
		FString GenerationWorkPath(const FString& Root, const FString& Id)
		{ return FPaths::GetPath(Root) / (TEXT(".uai-generation-") + Id); }

		bool IsLeafCaseChange(const FString& Old, const FString& New)
		{
			return Old.Equals(New, ESearchCase::IgnoreCase) && !Old.Equals(New, ESearchCase::CaseSensitive)
				&& FPaths::GetPath(Old).Equals(FPaths::GetPath(New), ESearchCase::CaseSensitive);
		}

		bool RecoverGenerationJournal(const FString& Root, FArtifactPublishError& Error)
		{
			FGenerationDirectoryScope DirectoryScope;
			const FString JournalPath = GenerationJournalPath(Root);
			if (!IFileManager::Get().FileExists(*JournalPath)) {
				const FString Next = JournalPath + TEXT(".next");
				if (!IFileManager::Get().FileExists(*Next)) return true;
				// No journal was committed, so the protocol cannot have touched
				// public files or created its stage directory. /3 owns this reserved
				// sidecar even when the interrupted first write is incomplete.
				FWorkspacePublication Publication; FWorkspaceError PublicationError;
				TSharedPtr<FJsonObject> Owner; FString Format, Final, OwnerId;
				TArray<TUniquePtr<FGenerationFileGuard>> Parents;
				if (!GuardGenerationDirectories(Root, {}, Parents, Error)
					|| !TryReadWorkspacePublication(Root, Publication, PublicationError) || !Publication.bGenerationPublication
					|| !LoadJson(Root / OwnerMarkerName, Root, Owner, Error)
					|| !Owner->TryGetStringField(TEXT("format"), Format) || Format != FormatId
					|| !Owner->TryGetStringField(TEXT("final_path"), Final) || Final != Root
					|| !Owner->TryGetStringField(TEXT("publisher_owner_id"), OwnerId) || OwnerId.IsEmpty()) {
					if (Error.IsEmpty()) SetError(Error, TEXT("workspace_orphan_journal_unowned"), TEXT("The interrupted journal sidecar cannot be attributed to a committed owned /3 workspace."), Next);
					return false;
				}
				FGenerationFileGuard Guard;
				return Guard.Open(Next, true, false, Error) && Guard.Remove(Error);
			}
			TSharedPtr<FJsonObject> Journal;
			if (!LoadJson(JournalPath, Root, Journal, Error)) return false;
			FString Format, RecordedRoot, Id, Generation, State, BusyHash;
			const TArray<TSharedPtr<FJsonValue>>* Rows = nullptr; FGuid ParsedId;
			if (!Journal->TryGetStringField(TEXT("format"), Format) || Format != TEXT("uai.artifact.generation/1")
				|| !Journal->TryGetStringField(TEXT("root"), RecordedRoot) || RecordedRoot != Root
				|| !Journal->TryGetStringField(TEXT("transaction"), Id) || !FGuid::ParseExact(Id, EGuidFormats::Digits, ParsedId)
				|| !Journal->TryGetStringField(TEXT("generation"), Generation) || Generation.IsEmpty()
				|| !Journal->TryGetStringField(TEXT("state"), State) || !Journal->TryGetArrayField(TEXT("files"), Rows)
				|| Rows->Num() > 32768 || !Journal->TryGetStringField(TEXT("busy_hash"), BusyHash)) {
				SetError(Error, TEXT("workspace_publication_journal_invalid"), TEXT("The generation journal is not bound to this workspace."), Root); return false;
			}
			const FString Work = GenerationWorkPath(Root, Id);
			// This recognized journal owns its interrupted next-record file.
			if (IFileManager::Get().FileExists(*(JournalPath + TEXT(".next")))) {
				FGenerationFileGuard Next;
				if (!Next.Open(JournalPath + TEXT(".next"), true, false, Error) || !Next.Remove(Error)) return false;
			}
			auto Cleanup = [&]() {
				Journal->SetStringField(TEXT("state"), TEXT("cleanup"));
				if (!SaveGenerationJournal(JournalPath, Journal.ToSharedRef(), Error)) return false;
				if (!RemoveGenerationPrivateDirectory(Work, Error)) return false;
				FGenerationFileGuard CompletedJournal;
				return CompletedJournal.Open(JournalPath, true, false, Error) && CompletedJournal.Remove(Error);
			};
			FWorkspacePublication Current; FWorkspaceError PublicationError;
			const bool bNewCommitted = TryReadWorkspacePublication(Root, Current, PublicationError)
				&& Current.bGenerationPublication && Current.Generation == Generation;
			if (State == TEXT("cleanup") || State == TEXT("committed") || bNewCommitted
				|| State == TEXT("prepared") || State == TEXT("staged")) return Cleanup();
			if (State != TEXT("publishing")) {
				SetError(Error, TEXT("workspace_publication_journal_invalid"), TEXT("Unknown generation journal state."), Root); return false;
			}
			struct FRecoveryFile {
				FString Relative, OldRelative, OldHash, NewHash, CurrentHash;
				TUniquePtr<FGenerationFileGuard> Current, Backup;
			};
			TArray<FRecoveryFile> Files;
			TArray<FString> Paths; TSet<FString> Seen;
			for (const auto& Value : *Rows) {
				const TSharedPtr<FJsonObject>* Row = nullptr; FRecoveryFile File; FString Path;
				if (!Value->TryGetObject(Row) || !(*Row)->TryGetStringField(TEXT("path"), Path)
					|| !ValidateRelativePath(Path, File.Relative, Error)
					|| !(*Row)->TryGetStringField(TEXT("old_hash"), File.OldHash)
					|| !(*Row)->TryGetStringField(TEXT("new_hash"), File.NewHash)
					|| Seen.Contains(File.Relative.ToLower())) {
					SetError(Error, TEXT("workspace_publication_journal_invalid"), TEXT("Invalid or duplicate generation recovery row."), Root); return false;
				}
				File.OldRelative = File.Relative;
				if ((*Row)->HasField(TEXT("old_path"))) {
					FString OldPath;
					if (!(*Row)->TryGetStringField(TEXT("old_path"), OldPath) || !ValidateRelativePath(OldPath, File.OldRelative, Error)
						|| !IsLeafCaseChange(File.OldRelative, File.Relative) || File.OldHash.IsEmpty() || File.NewHash.IsEmpty()) {
						SetError(Error, TEXT("workspace_publication_journal_invalid"), TEXT("Invalid original filename for a generation rename."), Root);
						return false;
					}
				}
				Seen.Add(File.Relative.ToLower()); Paths.Add(File.Relative); Files.Add(MoveTemp(File));
			}
			TArray<TUniquePtr<FGenerationFileGuard>> Directories;
			if (!GuardGenerationDirectories(Root, Paths, Directories, Error)) return false;
			TArray<FString> PrivatePaths;
			for (const FString& Path : Paths) {
				PrivatePaths.Add(TEXT("old/") + Path); PrivatePaths.Add(TEXT("new/") + Path); PrivatePaths.Add(TEXT("undone/") + Path);
					if (!MakeGenerationDirectory(FPaths::GetPath(Work / TEXT("undone") / Path), Error)) return false;
			}
			if (!GuardGenerationDirectories(Work, PrivatePaths, Directories, Error)) return false;
			// Preflight every recovery input before moving any file. Unknown bytes
			// are user changes, not permission to overwrite or delete them.
			for (auto& File : Files) {
				const FString CurrentPath = Root / File.Relative, BackupPath = Work / TEXT("old") / File.Relative;
				for (int32 Which = 0; Which != 2; ++Which) {
					const FString Path = Which ? BackupPath : CurrentPath;
					if (!IFileManager::Get().FileExists(*Path)) continue;
					auto Guard = MakeUnique<FGenerationFileGuard>(); FString Hash; FWorkspaceFileProofPtr Proof;
					if (!Guard->Open(Path, true, false, Error) || !ObserveGuardedGenerationFile(Path, *Guard, Hash, Proof, Error)) return false;
					if ((Which && (File.OldHash.IsEmpty() || Hash != File.OldHash))
						|| (!Which && Hash != File.OldHash && Hash != File.NewHash
							&& !(File.Relative == TEXT(".uai/workspace.hjson") && Hash == BusyHash))) {
						SetError(Error, TEXT("workspace_publication_recovery_conflict"), TEXT("A recovery file contains an external change; the journal and all files are preserved."), Path); return false;
					}
					if (Which) File.Backup = MoveTemp(Guard);
					else { File.Current = MoveTemp(Guard); File.CurrentHash = Hash; }
				}
				if (!File.OldHash.IsEmpty() && !File.Backup && (!File.Current || File.CurrentHash != File.OldHash)) {
					SetError(Error, TEXT("workspace_publication_recovery_missing_backup"), TEXT("The original generation file is unavailable; automatic recovery cannot discard current data."), CurrentPath); return false;
				}
			}
			Files.Sort([](const FRecoveryFile& A, const FRecoveryFile& B) {
				if (A.Relative == TEXT(".uai/workspace.hjson")) return false;
				if (B.Relative == TEXT(".uai/workspace.hjson")) return true;
				return A.Relative < B.Relative;
			});
			for (auto& File : Files) {
				if (File.Current && File.CurrentHash == File.OldHash && !File.Backup) continue;
				if (File.Current) {
					const FString Undone = Work / TEXT("undone") / File.Relative;
					if (!MakeGenerationDirectory(FPaths::GetPath(Undone), Error) || !File.Current->Rename(Undone, Error)) return false;
				}
				if (File.Backup && !File.Backup->Rename(Root / File.OldRelative, Error)) return false;
			}
			Files.Reset(); Directories.Reset();
			return Cleanup();
		}

		bool TryPublishGeneration(const FArtifactPublishRequest& Request, const FPublishPaths& Transaction,
			const TArray<FString>& Paths, const TMap<FString, FString>& Hashes, const FString& Inventory,
			FUeAgentResourceTracker& Resources, const FString& WorkspaceSchema, const FString& Generation, FArtifactPublishError& Error,
			TArray<FArtifactPublishError>& Warnings)
		{
			const double Started = FPlatformTime::Seconds();
			const FString& Root = Transaction.Final;
			FGenerationDirectoryScope DirectoryScope;
			FGenerationPublishLock Lock;
			if (!Lock.Acquire(Root, Error) || !RecoverGenerationJournal(Root, Error)) return false;
			const double RecoveryFinished = FPlatformTime::Seconds();
			if (GenerationObservationRoot != Root) { GenerationObservations.Reset(); GenerationObservationRoot = Root; }
			TArray<FString> ActualFiles;
			IFileManager::Get().FindFilesRecursive(ActualFiles, *Root, TEXT("*"), true, false);
			TArray<FString> ActualPaths;
			for (FString File : ActualFiles) {
				if (!FPaths::MakePathRelativeTo(File, *(Root / TEXT("")))) return false;
				FPaths::NormalizeFilename(File); FString Relative;
				if (!ValidateRelativePath(File, Relative, Error)) return false;
				ActualPaths.Add(Relative);
			}
			ActualPaths.Sort();
			TArray<TUniquePtr<FGenerationFileGuard>> Directories;
			if (!GuardGenerationDirectories(Root, {OwnerMarkerName}, Directories, Error)) return false;
			const double NamespaceFinished = FPlatformTime::Seconds();
			TMap<FString, TUniquePtr<FGenerationFileGuard>> CurrentGuards;
			TMap<FString, FString> CurrentHashes;
			TMap<FString, FWorkspaceFileProofPtr> CurrentProofs;
			// The owner is an actual changing file and the authority for removals.
			// Read it under its retained guard before classifying other files.
			{
				auto Guard = MakeUnique<FGenerationFileGuard>(); FString Hash; FWorkspaceFileProofPtr Proof;
				if (!Guard->Open(Root / OwnerMarkerName, false, false, Error)
					|| !ObserveGuardedGenerationFile(Root / OwnerMarkerName, *Guard, Hash, Proof, Error)) return false;
				if (Guard->WasReadOnlyAtOpen()) {
					SetError(Error, TEXT("artifact_final_read_only"), TEXT("The export owner record is read-only."), Root / OwnerMarkerName); return false;
				}
				CurrentGuards.Add(OwnerMarkerName, MoveTemp(Guard));
				CurrentHashes.Add(OwnerMarkerName, MoveTemp(Hash)); CurrentProofs.Add(OwnerMarkerName, MoveTemp(Proof));
			}
			TSharedPtr<FJsonObject> OldOwner;
			if (!LoadMatchingOwnerRecord(Root / OwnerMarkerName, Root, Request.GetPublisherOwnerId(), OldOwner, Error)) return false;
			TSet<FString> Managed;
			struct FLeafRename { FString OldRelative, AcceptedHash; };
			TMap<FString, FLeafRename> LeafRenames;
			const TArray<TSharedPtr<FJsonValue>>* Owned = nullptr;
			if (!OldOwner->TryGetArrayField(TEXT("files"), Owned)) return false;
			for (const auto& Value : *Owned) {
				const auto Row = Value->AsObject(); FString Path, Relative; bool bGenerated = false;
				if (!Row || !Row->TryGetStringField(TEXT("relative_path"), Path) || !ValidateRelativePath(Path, Relative, Error)) return false;
				if (Row->TryGetBoolField(TEXT("generated"), bGenerated) && bGenerated) {
					Managed.Add(Relative);
					const auto IntendedId = Hashes.FindId(Relative);
					if (IntendedId.IsValidId() && IsLeafCaseChange(Relative, Hashes.Get(IntendedId).Key)) {
						FString AcceptedHash;
						if (!Row->TryGetStringField(TEXT("content_hash"), AcceptedHash) || AcceptedHash.IsEmpty()) {
							SetError(Error, TEXT("workspace_publication_source_changed"), TEXT("The prior generated file lacks its accepted digest for renaming."), Root / Relative);
							return false;
						}
						LeafRenames.Add(Hashes.Get(IntendedId).Key, { Relative, MoveTemp(AcceptedHash) });
					}
				}
			}
			Managed.Add(OwnerMarkerName);
			for (const FString& SourcePath : Request.GetManagedSourcePaths()) {
				FString Relative; if (!ValidateRelativePath(SourcePath, Relative, Error)) return false;
				Managed.Add(Relative);
			}
			TMap<FString, FString> NewHashes = Hashes;
			FString OwnerText; SerializeJson(BuildOwnerRecord(Request, Root, Paths, Hashes, Inventory), OwnerText);
			FTCHARToUTF8 OwnerUtf8(*(OwnerText + TEXT("\n"))); TArray<uint8> OwnerBytes;
			OwnerBytes.Append(reinterpret_cast<const uint8*>(OwnerUtf8.Get()), OwnerUtf8.Length());
			NewHashes.Add(OwnerMarkerName, Sha256(OwnerBytes));
			// An unchanged retained file is neither read nor renamed by this
			// publication. Its directory has no mutation consumer here. Keep the
			// complete ancestor chain only for actual observation/write candidates;
			// added logical paths obtain their own chain before commit below.
			TArray<FString> ObservedPaths;
			for (const FString& Relative : ActualPaths) {
				if (Relative == OwnerMarkerName) continue;
				const auto* Intended = NewHashes.Find(Relative);
				const auto* Retained = Request.GetPlan()->GetRetainedFileHashes().Find(Relative);
				const auto RenameId = LeafRenames.FindId(Relative);
				if (RenameId.IsValidId()) {
					const auto& Rename = LeafRenames.Get(RenameId);
					if (Relative.Equals(Rename.Key, ESearchCase::CaseSensitive)) LeafRenames.Remove(Relative);
					else if (!Relative.Equals(Rename.Value.OldRelative, ESearchCase::CaseSensitive)) {
						SetError(Error, TEXT("workspace_publication_source_changed"), TEXT("The physical filename no longer matches its accepted spelling."), Root / Relative);
						return false;
					}
				}
				if (!Retained || !Intended || *Retained != *Intended || LeafRenames.Contains(Relative)) ObservedPaths.Add(Relative);
			}
			if (!GuardGenerationDirectories(Root, ObservedPaths, Directories, Error)) return false;
			UE_LOG(LogTemp, Display, TEXT("UAI export timing phase=generation_directory_scope namespace_files=%d observed_paths=%d retained_paths_not_guarded=%d"),
				ActualPaths.Num(), ObservedPaths.Num() + 1, ActualPaths.Num() - ObservedPaths.Num() - 1);
			int32 UnchangedFiles = 0, AcceptedByteObservations = 0, ShortReadObservations = 0;
			for (const FString& Relative : ActualPaths) {
				if (Relative == OwnerMarkerName) continue;
				const FString* Intended = NewHashes.Find(Relative);
				const auto* Rename = LeafRenames.Find(Relative);
				if (const auto* Retained = Request.GetPlan()->GetRetainedFileHashes().Find(Relative);
					!Rename && Retained && Intended && *Retained == *Intended) {
					// No replacement/deletion is planned for this path. Leave any
					// new author bytes in place; the OS candidate remains independent
					// of this publication's actual R/W checkpoint and consumption.
					CurrentHashes.Add(Relative, *Intended); ++UnchangedFiles; continue;
				}
				// This digest belongs to the actual accepted read and current data
				// lease. The existing checkpoint still binds the current logical
				// path before staging and again before committing any replacement.
				if (!Rename && Intended && Request.HasSourceCheckpoint()
					&& HasAcceptedSemanticSourceByteObservation(Root, Relative, *Intended)) {
					CurrentHashes.Add(Relative, *Intended); ++UnchangedFiles; ++AcceptedByteObservations; continue;
				}
				auto Guard = MakeUnique<FGenerationFileGuard>(); FString Hash; FWorkspaceFileProofPtr Proof;
				if (!Guard->Open(Root / Relative, false, false, Error)) return false;
				TArray<uint8> ActualBytes;
				if (!Guard->Read(ActualBytes, Error)) return false;
				Hash = Sha256(ActualBytes); ++ShortReadObservations;
				if (Rename && Hash != Rename->AcceptedHash
					&& !(Request.HasSourceCheckpoint() && HasAcceptedSemanticSourceByteObservation(Root, Relative, Hash))) {
					SetError(Error, TEXT("workspace_publication_source_changed"), TEXT("The original file has unaccepted edits and cannot be renamed."), Root / Relative);
					return false;
				}
				// Untouched bytes have no later lease consumer. Changing/deleting
				// files still obtain the original stable observation and reread
				// before entering the recovery write set.
				if ((Rename || !Intended || *Intended != Hash)
					&& !ObserveGuardedGenerationFile(Root / Relative, *Guard, Hash, Proof, Error, false)) return false;
				if (!Managed.Contains(Relative) && (!Intended || *Intended != Hash)) {
					SetError(Error, TEXT("workspace_publication_unmanaged_conflict"), TEXT("An unmanaged file changed or was added after preparation; it has been preserved."), Root / Relative); return false;
				}
				CurrentHashes.Add(Relative, Hash);
				if (!Rename && Intended && *Intended == Hash) {
					// This file is not staged, replaced, deleted or consumed by the
					// recovery journal. Leave its actual bytes alone. Source inputs
					// still have the original checkpoint before and after staging.
					++UnchangedFiles; continue;
				}
				if (Guard->WasReadOnlyAtOpen()) {
					SetError(Error, TEXT("artifact_final_read_only"), TEXT("An export file that must be replaced or deleted is read-only."), Root / Relative); return false;
				}
				if (CurrentGuards.Num() >= 16384) {
					SetError(Error, TEXT("workspace_publication_guard_budget"), TEXT("The changing files exceed the bounded generation-commit handle budget."), Root); return false;
				}
				CurrentGuards.Add(Relative, MoveTemp(Guard)); CurrentProofs.Add(Relative, MoveTemp(Proof));
			}
			// Only changing old files need long-lived write/delete exclusion.
			// The original source checkpoint covers its actual inputs, including
			// unchanged files; repeat it after staging before DELETE upgrades.
			const double CurrentGuardsFinished = FPlatformTime::Seconds();
			const auto GuardUsage = GetWorkspaceFileProofUsage();
			UE_LOG(LogTemp, Display, TEXT("UAI export timing phase=generation_guard_budget current_guards=%d unchanged_files=%d generation_observations=%d pending_file_proofs=%d proof_path_characters=%lld accepted_byte_reuses=%d short_reads=%d"),
				CurrentGuards.Num(), UnchangedFiles, GenerationObservations.Num(), GuardUsage.PendingOperations, GuardUsage.PathCharacters, AcceptedByteObservations, ShortReadObservations);
			// Consume exactly these current strong guards in source checkpoint
			// and final lease validation. No scope escapes this transaction and a
			// closed, upgraded or renamed path cannot yield the old handle.
			FWorkspaceCurrentFileScope CurrentFileScope([&](const FString& Path) -> void* {
				if (!Path.StartsWith(Root + TEXT("/"), ESearchCase::CaseSensitive)) return nullptr;
				const auto* Guard = CurrentGuards.Find(Path.Mid(Root.Len() + 1));
				return Guard && *Guard ? (*Guard)->GetCurrentObservationHandle(Path) : nullptr;
			});
			if (!Request.RecheckSource(Error)) return false;
			// Retained files have no replacement body. A changed/missing one is
			// an actual source conflict, never permission to write an empty file
			// or reconstruct unrelated semantic state.
			for (const auto& File : Request.GetPlan()->GetRetainedFileHashes()) {
				const auto* Current = CurrentHashes.Find(File.Key);
				if (!Current || *Current != File.Value) {
					SetError(Error, TEXT("workspace_source_changed"), TEXT("A generated file selected to remain unchanged differs from its accepted bytes."), Root / File.Key);
					return false;
				}
			}
			const double SourceCheckpointFinished = FPlatformTime::Seconds();
			const FString Work = GenerationWorkPath(Root, Transaction.TransactionId);
			const FString JournalPath = GenerationJournalPath(Root);
			if (IFileManager::Get().DirectoryExists(*Work) || IFileManager::Get().FileExists(*JournalPath)) {
				SetError(Error, TEXT("workspace_publication_transaction_conflict"), TEXT("The private generation transaction path is occupied."), Work); return false;
			}
			TSet<FString> Changed;
			for (const auto& New : NewHashes) if (CurrentHashes.FindRef(New.Key) != New.Value || LeafRenames.Contains(New.Key)) Changed.Add(New.Key);
			for (const auto& Old : CurrentHashes) if (Managed.Contains(Old.Key) && !NewHashes.Contains(Old.Key)) Changed.Add(Old.Key);
			FPublishPaths Stage = Transaction; Stage.Staging = Work / TEXT("new");
			const double ChangePlanningFinished = FPlatformTime::Seconds();
			Stage.StagingOwner = Stage.Staging / OwnerMarkerName;
			bool bCommitted = false;
			TMap<FString, TUniquePtr<FGenerationFileGuard>> NewGuards;
			TUniquePtr<FGenerationFileGuard> BusyGuard;
			ON_SCOPE_EXIT {
				NewGuards.Reset(); BusyGuard.Reset(); CurrentGuards.Reset(); CurrentProofs.Reset(); Directories.Reset();
				if (!bCommitted) {
					FArtifactPublishError Recovery;
					if (IFileManager::Get().FileExists(*JournalPath)) {
						if (!RecoverGenerationJournal(Root, Recovery)) Error.Append(Recovery);
					} else if (!RemoveGenerationPrivateDirectory(Work, Recovery)) Error.Append(Recovery);
				}
			};
			const auto Journal = MakeShared<FJsonObject>();
			Journal->SetStringField(TEXT("format"), TEXT("uai.artifact.generation/1"));
			Journal->SetStringField(TEXT("root"), Root); Journal->SetStringField(TEXT("transaction"), Transaction.TransactionId);
			Journal->SetStringField(TEXT("generation"), Generation); Journal->SetStringField(TEXT("state"), TEXT("prepared"));
			FString BusyText = FString::Printf(TEXT("{\"workspace_schema\":\"%s\",\"publication_state\":\"publishing\",\"publication_generation\":\"%s\"}"), *WorkspaceSchema, *Generation);
			FTCHARToUTF8 BusyUtf8(*BusyText); TArray<uint8> BusyBytes;
			BusyBytes.Append(reinterpret_cast<const uint8*>(BusyUtf8.Get()), BusyUtf8.Length());
			Journal->SetStringField(TEXT("busy_hash"), Sha256(BusyBytes));
			TArray<TSharedPtr<FJsonValue>> Rows;
			TArray<FString> ChangedOrder = Changed.Array(); ChangedOrder.Sort();
			for (const FString& Relative : ChangedOrder) {
				auto Row = MakeShared<FJsonObject>(); Row->SetStringField(TEXT("path"), Relative);
				if (const auto* Rename = LeafRenames.Find(Relative); Rename && CurrentGuards.Contains(Relative))
					Row->SetStringField(TEXT("old_path"), Rename->OldRelative);
				Row->SetStringField(TEXT("old_hash"), CurrentHashes.FindRef(Relative)); Row->SetStringField(TEXT("new_hash"), NewHashes.FindRef(Relative));
				Rows.Add(MakeShared<FJsonValueObject>(Row));
			}
			Journal->SetArrayField(TEXT("files"), MoveTemp(Rows));
			if (!SaveGenerationJournal(JournalPath, Journal, Error)
				|| !StageFiles(Request, Stage, Resources, Hashes, Error, &Changed)) return false;
			// Retained files carry a hash only. A real rename is the one case in
			// which their verified current bytes need a staged destination.
			for (const auto& Rename : LeafRenames) {
				if (!Request.GetPlan()->GetRetainedFileHashes().Contains(Rename.Key)) continue;
				TArray<uint8> Expected, Readback;
				if (!CurrentGuards.FindChecked(Rename.Key)->Read(Expected, Error)
					|| !WriteAndVerifyStagedFile(Stage.Staging / Rename.Key, Rename.Key, Expected,
						TEXT("artifact_staging_write_failed"), TEXT("artifact_staging_verification_failed"), Readback, Error, true)) return false;
			}
			TArray<uint8> Observed;
			if (!WriteAndVerifyStagedFile(Stage.StagingOwner, OwnerMarkerName, OwnerBytes,
				TEXT("workspace_owner_stage_failed"), TEXT("workspace_owner_verify_failed"), Observed, Error, true)
				|| !WriteAndVerifyStagedFile(Work / TEXT("publishing.hjson"), TEXT("publication_marker"), BusyBytes,
				TEXT("workspace_marker_stage_failed"), TEXT("workspace_marker_verify_failed"), Observed, Error, true)) return false;
			TArray<FString> PrivatePaths;
			const double StagingFinished = FPlatformTime::Seconds();
			TArray<FString> AddedPaths;
			for (const FString& Relative : ChangedOrder) {
				PrivatePaths.Add(TEXT("new/") + Relative); PrivatePaths.Add(TEXT("old/") + Relative);
				if (NewHashes.Contains(Relative) && !CurrentGuards.Contains(Relative)) AddedPaths.Add(Relative);
				if (!MakeGenerationDirectory(FPaths::GetPath(Work / TEXT("old") / Relative), Error)
					|| (NewHashes.Contains(Relative) && !MakeGenerationDirectory(FPaths::GetPath(Root / Relative), Error))) return false;
			}
			if (!GuardGenerationDirectories(Work, PrivatePaths, Directories, Error)
				// CurrentGuards were accepted only after their complete parent
				// chains were retained in Directories for this request. Replaced
				// files keep those capabilities; only new logical paths need more.
				|| !GuardGenerationDirectories(Root, AddedPaths, Directories, Error)) return false;
			for (const FString& Relative : ChangedOrder) if (NewHashes.Contains(Relative)) {
				auto Guard = MakeUnique<FGenerationFileGuard>(); FString Hash; FWorkspaceFileProofPtr Proof;
				if (!Guard->Open(Stage.Staging / Relative, true, false, Error)
                    || !ObserveGuardedGenerationFile(Stage.Staging / Relative, *Guard, Hash, Proof, Error, false)) return false;
				if (Hash != NewHashes.FindChecked(Relative)) { SetError(Error, TEXT("workspace_generation_staged_changed"), TEXT("A staged file changed before publication."), Relative); return false; }
                // The successful kernel lease excludes an already-writable mapping;
                // the same retained guard then prevents a new writer or replacement.
                // A private staging path is never a reusable public observation. Its
                // lease can retire now rather than retaining old+new workspace proofs
                // together until commit and exhausting the shared bounded backend.
				NewGuards.Add(Relative, MoveTemp(Guard));
			}
			BusyGuard = MakeUnique<FGenerationFileGuard>();
			if (!BusyGuard->Open(Work / TEXT("publishing.hjson"), true, false, Error)) return false;
			const double NewGuardsFinished = FPlatformTime::Seconds();
			Journal->SetStringField(TEXT("state"), TEXT("staged"));
			if (!SaveGenerationJournal(JournalPath, Journal, Error)) return false;
			// Ordinary source readers need not share DELETE access. Validate
			// their exact bytes/watermark at the last point before those upgrades.
			// A later edit of an untouched file is preserved, never overwritten
			// or silently included in the generated baseline.
			if (!Request.RecheckSource(Error)) return false;
			// Upgrade only changing files to DELETE access. Observe again after
			// the upgrade; a writer in the handover gap must not go unnoticed.
			for (const FString& Relative : Changed) if (auto* Guard = CurrentGuards.Find(Relative)) {
				Guard->Reset(); CurrentProofs.Remove(Relative);
				auto Replacement = MakeUnique<FGenerationFileGuard>(); FString Hash; FWorkspaceFileProofPtr Proof;
				const auto* Rename = LeafRenames.Find(Relative);
				const FString CurrentPath = Root / (Rename ? Rename->OldRelative : Relative);
				if (!Replacement->Open(CurrentPath, true, false, Error)
					|| !ObserveGuardedGenerationFile(CurrentPath, *Replacement, Hash, Proof, Error)) return false;
				if (Hash != CurrentHashes.FindChecked(Relative)) {
					SetError(Error, TEXT("workspace_publication_source_changed"), TEXT("A source file changed while its commit guard was acquired."), Root / Relative); return false;
				}
				*Guard = MoveTemp(Replacement); CurrentProofs.Add(Relative, MoveTemp(Proof));
			}
			for (const auto& Proof : CurrentProofs) if (!Proof.Value || !Proof.Value->IsCurrent()) {
				SetError(Error, TEXT("workspace_generation_source_invalidated"), TEXT("A source file lost its read lease before commit."), Root / Proof.Key); return false;
			}
			Journal->SetStringField(TEXT("state"), TEXT("publishing"));
			if (!SaveGenerationJournal(JournalPath, Journal, Error)) return false;
			const FString Manifest(TEXT(".uai/workspace.hjson"));
			if (!Changed.Contains(Manifest) || !CurrentGuards.Contains(Manifest) || !NewGuards.Contains(Manifest)) {
				SetError(Error, TEXT("workspace_generation_manifest_missing"), TEXT("A generation commit must replace its existing manifest."), Root); return false;
			}
			const double CommitPrepared = FPlatformTime::Seconds();
			auto MoveOld = [&](const FString& Relative) {
				if (auto* Guard = CurrentGuards.Find(Relative)) {
					const FString Backup = Work / TEXT("old") / Relative;
					return MakeGenerationDirectory(FPaths::GetPath(Backup), Error) && (*Guard)->Rename(Backup, Error);
				} return true;
			};
			if (!MoveOld(Manifest) || !BusyGuard->Rename(Root / Manifest, Error)) return false;
			for (const FString& Relative : ChangedOrder) if (Relative != Manifest) {
				if (!MoveOld(Relative)) return false;
				if (auto* New = NewGuards.Find(Relative)) {
					if (!MakeGenerationDirectory(FPaths::GetPath(Root / Relative), Error) || !(*New)->Rename(Root / Relative, Error)) return false;
				}
			}
			// The committed manifest is the visibility point. Every replaced or
			// deleted file remains guarded; unchanged files are never written.
			// Changed bytes have real staged readback and the actual source fence.
			if (!BusyGuard->Rename(Work / TEXT("publishing-retired.hjson"), Error)
				|| !NewGuards.FindChecked(Manifest)->Rename(Root / Manifest, Error)) return false;
			bCommitted = true;
			const double CommitFinished = FPlatformTime::Seconds();
			Journal->SetStringField(TEXT("state"), TEXT("committed"));
			FArtifactPublishError CleanupError;
			if (!SaveGenerationJournal(JournalPath, Journal, CleanupError)) Warnings.Add(CleanupError);
			const double CommitJournalFinished = FPlatformTime::Seconds();
			int32 RememberedSourceFiles = 0;
			{
				// Staging observations do not prove the committed logical path. Bind
				// only the changed final files while their existing strong guards are
				// still held. Failure to retain this optional cache never changes the
				// already committed generation or weakens its publication read set.
				FWorkspaceCurrentFileScope FinalFileScope([&](const FString& Path) -> void* {
					if (!Path.StartsWith(Root + TEXT("/"), ESearchCase::CaseSensitive)) return nullptr;
					const FString Relative = Path.Mid(Root.Len() + 1);
					const auto* Guard = NewGuards.Find(Relative);
					if (!Guard) Guard = CurrentGuards.Find(Relative);
					return Guard && *Guard ? (*Guard)->GetCurrentObservationHandle(Path) : nullptr;
				});
				for (const FString& Relative : ChangedOrder) {
					if (!Request.ShouldRememberSource(Relative)) continue;
					const auto* Guard = NewGuards.Find(Relative);
					if (!Guard || !Relative.EndsWith(TEXT(".hjson"), ESearchCase::IgnoreCase)) continue;
					const FString FinalPath = Root / Relative;
					void* Handle = (*Guard)->GetCurrentReadHandle(FinalPath);
					auto Proof = TryBeginWorkspaceFileObservation(FinalPath);
					if (!Handle || !Proof) continue;
					TArray<uint8> Bytes; FArtifactPublishError ObservationError;
					if (!(*Guard)->Read(Bytes, ObservationError) || !Proof->IsCurrentHandle(Handle)) continue;
					const FString Hash = Sha256(Bytes);
					if (!Hash.Equals(NewHashes.FindChecked(Relative), ESearchCase::CaseSensitive)) continue;
					const auto* Encoding = PreviousArtifactEncodings ? PreviousArtifactEncodings->TextEncodings.Find(Relative) : nullptr;
					if (RememberObservedSemanticSourceFile(Root, FinalPath, Bytes, Hash, MoveTemp(Proof),
						Encoding ? *Encoding : TSharedPtr<const FWorkspaceTextEncodingProof>(), true)) ++RememberedSourceFiles;
				}
			}
			const double SourceRememberFinished = FPlatformTime::Seconds();
			NewGuards.Reset(); BusyGuard.Reset(); CurrentGuards.Reset(); CurrentProofs.Reset(); Directories.Reset();
			if (!RecoverGenerationJournal(Root, CleanupError)) Warnings.Add(CleanupError);
			UE_LOG(LogTemp, Display, TEXT("UAI export timing phase=generation_publish_parts recovery_s=%.6f namespace_s=%.6f current_guards_s=%.6f source_checkpoint_s=%.6f change_plan_s=%.6f staging_s=%.6f new_guards_s=%.6f final_fence_s=%.6f commit_s=%.6f commit_journal_s=%.6f source_handoff_s=%.6f source_handoff_files=%d cleanup_s=%.6f"),
				RecoveryFinished - Started, NamespaceFinished - RecoveryFinished, CurrentGuardsFinished - NamespaceFinished,
				SourceCheckpointFinished - CurrentGuardsFinished, ChangePlanningFinished - SourceCheckpointFinished,
				StagingFinished - ChangePlanningFinished, NewGuardsFinished - StagingFinished, CommitPrepared - NewGuardsFinished,
				CommitFinished - CommitPrepared, CommitJournalFinished - CommitFinished,
				SourceRememberFinished - CommitJournalFinished, RememberedSourceFiles, FPlatformTime::Seconds() - SourceRememberFinished);
			UE_LOG(LogTemp, Display, TEXT("UAI export timing phase=generation_publish files=%d changed_files=%d total_s=%.6f success=true"),
				Paths.Num(), Changed.Num(), FPlatformTime::Seconds() - Started);
			return true;
		}

		bool VerifyInventory(
			const FString& Root,
			const bool bSingleFile,
			const TArray<FString>& ExpectedPaths,
			const TMap<FString, FString>& ExpectedHashes,
			const FString& Target,
			FArtifactPublishError& OutError,
			const bool bRememberFinalSource = false,
			const FArtifactPublishRequest* SourceRequest = nullptr)
		{
			if (bSingleFile)
			{
				if (ExpectedPaths.Num() != 1)
				{
					SetError(OutError, TEXT("artifact_inventory_invalid"), TEXT("A single-file inventory must contain exactly one path."), Target, Root);
					OutError.ActualValue = MakeShared<FJsonValueNumber>(ExpectedPaths.Num());
					OutError.ExpectedValue = MakeShared<FJsonValueNumber>(1);
					return false;
				}
				TArray<uint8> Bytes;
				if (!FFileHelper::LoadFileToArray(Bytes, *Root))
				{
					SetError(OutError, TEXT("artifact_inventory_read_failed"), TEXT("Could not read the export file while verifying its inventory."), Target, Root);
					return false;
				}
				const FString ActualHash = Sha256(Bytes);
				if (ActualHash != ExpectedHashes.FindChecked(ExpectedPaths[0]))
				{
					SetError(OutError, TEXT("artifact_inventory_content_mismatch"), TEXT("The exported file content does not match its planned inventory."), Target, Root);
					SetPrivateComparison(OutError, ActualHash, ExpectedHashes.FindChecked(ExpectedPaths[0]));
					return false;
				}
				return true;
			}
			const double InventoryStarted = FPlatformTime::Seconds();
			double LeaseSeconds = 0, ReadSeconds = 0, HashSeconds = 0, SourceSeconds = 0;
			TArray<FString> ObservedFiles;
			IFileManager::Get().FindFilesRecursive(
				ObservedFiles,
				*Root,
				TEXT("*"),
				true,
				false);
			const double EnumerationSeconds = FPlatformTime::Seconds() - InventoryStarted;
			TArray<FString> ObservedPaths;
			TMap<FString, FString> ObservedHashes;
			int32 RememberedFiles = 0;
			for (const FString& File : ObservedFiles)
			{
				FString Relative = File;
				FPaths::MakePathRelativeTo(Relative, *(Root / TEXT("")));
				FPaths::NormalizeFilename(Relative);
				if (Relative.Equals(
					OwnerMarkerName,
					ESearchCase::IgnoreCase))
				{
					continue;
				}
				const double LeaseStarted = FPlatformTime::Seconds();
				const bool bSourceObservation = bRememberFinalSource && (!SourceRequest || SourceRequest->ShouldRememberSource(Relative));
				auto Observation = bSourceObservation && File.EndsWith(TEXT(".hjson"), ESearchCase::IgnoreCase)
					? TryBeginWorkspaceFileObservation(File) : FWorkspaceFileProofPtr();
				const double ReadStarted = FPlatformTime::Seconds();
				LeaseSeconds += ReadStarted - LeaseStarted;
				TArray<uint8> Bytes;
				if (!FFileHelper::LoadFileToArray(Bytes, *File))
				{
					SetError(OutError, TEXT("artifact_inventory_read_failed"), TEXT("Could not read an export file while verifying the workspace inventory."), Relative, File);
					return false;
				}
				const double HashStarted = FPlatformTime::Seconds();
				ReadSeconds += HashStarted - ReadStarted;
				ObservedPaths.Add(Relative);
				const FString Hash = Sha256(Bytes);
				ObservedHashes.Add(Relative, Hash);
				const double SourceStarted = FPlatformTime::Seconds();
				HashSeconds += SourceStarted - HashStarted;
				const auto* Encoding = PreviousArtifactEncodings ? PreviousArtifactEncodings->TextEncodings.Find(Relative) : nullptr;
				if (bSourceObservation && RememberObservedSemanticSourceFile(Root, File, Bytes, Hash, MoveTemp(Observation),
					Encoding ? *Encoding : TSharedPtr<const FWorkspaceTextEncodingProof>())) ++RememberedFiles;
				SourceSeconds += FPlatformTime::Seconds() - SourceStarted;
			}
			ObservedPaths.Sort();
			if (ObservedPaths != ExpectedPaths)
			{
				int32 Index = 0;
				while (Index < ObservedPaths.Num() && Index < ExpectedPaths.Num() && ObservedPaths[Index] == ExpectedPaths[Index]) ++Index;
				SetError(OutError, TEXT("artifact_inventory_paths_mismatch"), TEXT("The exported file list differs from the planned inventory."), Target, Root);
				SetPointer(OutError, FString::Printf(TEXT("/files/%d"), Index));
				OutError.ActualValuePresent = ObservedPaths.IsValidIndex(Index);
				OutError.ExpectedValuePresent = ExpectedPaths.IsValidIndex(Index);
				if (ObservedPaths.IsValidIndex(Index)) OutError.ActualValue = MakeShared<FJsonValueString>(ObservedPaths[Index]);
				if (ExpectedPaths.IsValidIndex(Index)) OutError.ExpectedValue = MakeShared<FJsonValueString>(ExpectedPaths[Index]);
				return false;
			}
			if (!ObservedHashes.OrderIndependentCompareEqual(ExpectedHashes))
			{
				for (const auto& Expected : ExpectedHashes)
				{
					const FString* Actual = ObservedHashes.Find(Expected.Key);
					if (!Actual || *Actual != Expected.Value)
					{
						SetError(OutError, TEXT("artifact_inventory_content_mismatch"),
							Actual ? TEXT("The exported file content differs from its planned inventory.") : TEXT("The exported file has no observed inventory hash."), Expected.Key, Root);
						SetPrivateComparison(OutError, Actual ? *Actual : FString(), Expected.Value);
						return false;
					}
				}
				for (const auto& Actual : ObservedHashes)
				{
					if (!ExpectedHashes.Contains(Actual.Key))
					{
						SetError(OutError, TEXT("artifact_inventory_content_mismatch"), TEXT("The exported file has no expected content hash in the inventory."), Actual.Key, Root);
						OutError.ActualEvidence = MakeShared<FJsonValueString>(Actual.Value);
						return false;
					}
				}
			}
			if (bRememberFinalSource) UE_LOG(LogTemp, Display, TEXT("UAI export timing phase=final_source_observation files=%d retained_content_proofs=%d"), ObservedPaths.Num(), RememberedFiles);
			const double InventorySeconds = FPlatformTime::Seconds() - InventoryStarted;
			UE_LOG(LogTemp, Display, TEXT("UAI export timing phase=artifact_inventory files=%d remember_source=%s enumerate_s=%.6f lease_s=%.6f read_s=%.6f hash_and_index_s=%.6f source_record_s=%.6f remaining_s=%.6f total_s=%.6f"),
				ObservedPaths.Num(), bRememberFinalSource ? TEXT("true") : TEXT("false"), EnumerationSeconds,
				LeaseSeconds, ReadSeconds, HashSeconds, SourceSeconds,
				InventorySeconds - EnumerationSeconds - LeaseSeconds - ReadSeconds - HashSeconds - SourceSeconds, InventorySeconds);
			return true;
		}

		bool VerifyFinal(
			const FPublishPaths& Paths,
			const FArtifactPublishRequest& Request,
			const TArray<FString>& ExpectedPaths,
			const TMap<FString, FString>& ExpectedHashes,
			const FString& Inventory,
			FArtifactPublishError& OutError)
		{
			if (!VerifyInventory(
				Paths.Final,
				Paths.bSingleFile,
				ExpectedPaths,
				ExpectedHashes, Paths.Final, OutError, true, &Request))
			{
				return false;
			}
			TSharedPtr<FJsonObject> Marker;
			FString Owner;
			FString MarkerInventory;
			if (!LoadJson(Paths.FinalOwner, Paths.Final, Marker, OutError)
				|| !ReadMarkerString(Marker, TEXT("publisher_owner_id"), Owner, Paths.Final, OutError)) return false;
			if (Owner != Request.GetPublisherOwnerId())
			{
				SetError(OutError, TEXT("artifact_owner_marker_owner_mismatch"), TEXT("The published ownership record identifies a different publisher."), Paths.Final, Paths.FinalOwner);
				SetPointer(OutError, TEXT("/publisher_owner_id"));
				SetPrivateComparison(OutError, Owner, Request.GetPublisherOwnerId());
				return false;
			}
			if (!ReadMarkerString(Marker, TEXT("inventory_hash"), MarkerInventory, Paths.Final, OutError)) return false;
			if (MarkerInventory != Inventory)
			{
				SetError(OutError, TEXT("artifact_owner_marker_inventory_mismatch"), TEXT("The published ownership record does not match the verified file inventory."), Paths.Final, Paths.FinalOwner);
				SetPointer(OutError, TEXT("/inventory_hash"));
				SetPrivateComparison(OutError, MarkerInventory, Inventory);
				return false;
			}
			return true;
		}

		bool RestoreOldFinal(
			const FPublishPaths& Paths,
			const bool bOldMoved,
			const bool bNewPublished,
			FArtifactPublishError& OutError)
		{
			// Check the only copy of the old destination before removing the
			// published replacement. If it is missing, retain both the current
			// output and the journal for recovery instead of losing both versions.
			if (bOldMoved) {
				const bool bBackupExists = Paths.bSingleFile
					? IFileManager::Get().FileExists(*Paths.Backup)
					: IFileManager::Get().DirectoryExists(*Paths.Backup);
				if (!bBackupExists) {
					SetError(OutError, TEXT("artifact_rollback_backup_missing"), TEXT("The previous export backup is missing, so it cannot be restored."), Paths.Final, Paths.Backup);
					OutError.ActualValuePresent = false;
					OutError.ExpectedValue = MakeShared<FJsonValueString>(TEXT("the previous export backup"));
					return false;
				}
			}
			const bool bNewExists = Paths.bSingleFile
				? IFileManager::Get().FileExists(*Paths.Final)
				: IFileManager::Get().DirectoryExists(*Paths.Final);
			if (bNewPublished && bNewExists
				&& !DeleteDataPath(Paths, Paths.Final, TEXT("new output during rollback"), OutError))
			{
				return false;
			}
			if (bNewPublished
				&& Paths.bSingleFile
				&& !DeleteFile(Paths.FinalOwner, Paths.Final, TEXT("new ownership record during rollback"), OutError))
			{
				return false;
			}
			if (bOldMoved)
			{
				if (!MovePath(Paths.Final, Paths.Backup, Paths.Final, OutError)) return false;
				if (Paths.bSingleFile
					&& IFileManager::Get().FileExists(*Paths.BackupOwner)
					&& !MovePath(Paths.FinalOwner, Paths.BackupOwner, Paths.Final, OutError))
				{
					return false;
				}
			}
			const bool bRestored = !bOldMoved
				|| (Paths.bSingleFile
					? IFileManager::Get().FileExists(*Paths.Final)
					: IFileManager::Get().DirectoryExists(*Paths.Final));
			if (!bRestored)
			{
				SetError(OutError, TEXT("artifact_rollback_final_missing"), TEXT("The previous export is still absent after its backup was restored."), Paths.Final);
				OutError.ActualValuePresent = false;
			}
			return bRestored;
		}

		void CleanupStaging(const FPublishPaths& Paths, const bool bDeleteJournal, FArtifactPublishError& OutError)
		{
			FArtifactPublishError CleanupError;
			if (!DeleteDataPath(Paths, Paths.Staging, TEXT("staging"), CleanupError)) OutError.Append(CleanupError);
			if (!DeleteFile(Paths.StagingOwner, Paths.Final, TEXT("staged ownership record"), CleanupError)) OutError.Append(CleanupError);
			if (bDeleteJournal && !DeleteFile(Paths.Journal, Paths.Final, TEXT("transaction journal"), CleanupError)) OutError.Append(CleanupError);
		}

		void FinishFailedPublish(const FPublishPaths& Paths, const bool bOldMoved, const bool bNewPublished,
			const FString& FailureCode, FArtifactPublishError Primary,
			const TArray<FArtifactPublishError>& Warnings, FArtifactPublishError& OutError)
		{
			FArtifactPublishError RestoreError;
			const bool bRestored = RestoreOldFinal(Paths, bOldMoved, bNewPublished, RestoreError);
			SetError(OutError, bRestored ? FailureCode : TEXT("artifact_publish_state_unknown"),
				bRestored ? (bOldMoved || bNewPublished ? TEXT("Export publication failed; the previous destination state was restored.")
					: TEXT("Export publication failed before a destination replacement completed."))
					: TEXT("Export publication failed and the previous destination state could not be restored."), Paths.Final);
			OutError.PublicCause = MakeShared<FArtifactPublishError>(Primary);
			OutError.RollbackAttempted = bOldMoved || bNewPublished;
			if (bOldMoved || bNewPublished) OutError.RollbackSucceeded = bRestored;
			if (!RestoreError.IsEmpty()) OutError.Append(RestoreError);
			for (const FArtifactPublishError& Warning : Warnings) OutError.Append(Warning);
			// An unconfirmed restore still needs its original staging/backup and
			// journal. Cleanup belongs to a confirmed restore/commit, not an
			// error path that has just reported an unknown publication state.
			if (bRestored) CleanupStaging(Paths, true, OutError);
		}
	}

	void ReleaseCachedArtifactFileObservations(const FString& WorkspaceRoot)
	{
		FString Root = FPaths::ConvertRelativePathToFull(WorkspaceRoot);
		FPaths::NormalizeDirectoryName(Root);
		if (!GenerationObservationRoot.Equals(Root, ESearchCase::CaseSensitive)) return;
		const int32 Released = GenerationObservations.Num();
		GenerationObservations.Reset();
		UE_LOG(LogTemp, Display, TEXT("UAI import timing phase=generation_observation_release observations=%d consumer=source_and_history existing_budget=true"), Released);
	}

	void RememberVerifiedPlanBytes(const FExportFilePlan& File)
	{
		// Same existing producer handoff, attached to the immutable file itself.
		// It expires with that file; it is not a second budgeted proof directory.
		FPlatformAtomics::InterlockedExchange(&File.bVerifiedCanonicalUtf8, 1);
	}

	FArtifactPublishRequest::FArtifactPublishRequest(
		FExportWorkspacePlanRef InPlan,
		FString InFinalPath,
		FString InPublisherOwnerId,
		TMap<FString, TArray<uint8>> InPreservedFiles)
		: Plan(MoveTemp(InPlan))
		, FinalPath(MoveTemp(InFinalPath))
		, PublisherOwnerId(MoveTemp(InPublisherOwnerId))
		, PreservedFiles(MoveTemp(InPreservedFiles))
	{
		FinalPath.TrimStartAndEndInline();
		PublisherOwnerId.TrimStartAndEndInline();
		for (const auto& File : Plan->GetFiles())
			if (!File.NeedsSourceObservation()) NonSourceFiles.Add({&File.GetRelativePath()});
		if (FinalPath.IsEmpty() || PublisherOwnerId.IsEmpty())
		{
			SetError(ValidationDiagnostic, TEXT("artifact_request_invalid"),
				FinalPath.IsEmpty() ? TEXT("The export publish request has no destination path.")
					: TEXT("The export publish request has no publisher identity."), FinalPath);
			SetPointer(ValidationDiagnostic, FinalPath.IsEmpty() ? TEXT("/final_path") : TEXT("/publisher_owner_id"));
			ValidationDiagnostic.ActualValue = MakeShared<FJsonValueString>(FString());
			ValidationDiagnostic.ExpectedValue = MakeShared<FJsonValueString>(TEXT("a nonempty value"));
		}
	}

	FArtifactPublishRequest::FArtifactPublishRequest(
		FExportWorkspacePlanRef InPlan,
		FString InFinalPath,
		FString InPublisherOwnerId,
		TSharedRef<IArtifactPublishVerification> InVerification)
		: FArtifactPublishRequest(
			MoveTemp(InPlan),
			MoveTemp(InFinalPath),
			MoveTemp(InPublisherOwnerId))
	{
		Verification = MoveTemp(InVerification);
		if (ValidationDiagnostic.IsEmpty() && !Verification.IsValid())
		{
			SetError(ValidationDiagnostic, TEXT("artifact_request_invalid"), TEXT("The export publish request has no verification service."), FinalPath);
			SetPointer(ValidationDiagnostic, TEXT("/verification"));
			ValidationDiagnostic.ActualValuePresent = false;
		}
	}

	const FExportWorkspacePlanRef& FArtifactPublishRequest::GetPlan() const
	{
		return Plan;
	}

	const FString& FArtifactPublishRequest::GetFinalPath() const
	{
		return FinalPath;
	}

	const FString& FArtifactPublishRequest::GetPublisherOwnerId() const
	{
		return PublisherOwnerId;
	}

	const TMap<FString, TArray<uint8>>& FArtifactPublishRequest::GetPreservedFiles() const
	{
		return PreservedFiles;
	}

	const TSharedPtr<IArtifactPublishVerification>&
	FArtifactPublishRequest::GetVerification() const
	{
		return Verification;
	}

	bool FArtifactPublishRequest::RequiresVerification() const
	{
		return Verification.IsValid();
	}

	void FArtifactPublishRequest::SetSourceCheckpoint(TFunction<bool(FArtifactPublishError&)> InCheckpoint)
	{
		SourceCheckpoint = MoveTemp(InCheckpoint);
	}

	bool FArtifactPublishRequest::RecheckSource(FArtifactPublishError& OutError) const
	{
		return !SourceCheckpoint || SourceCheckpoint(OutError);
	}

	bool FArtifactPublishRequest::IsValid() const
	{
		return ValidationDiagnostic.IsEmpty();
	}

	const FArtifactPublishError& FArtifactPublishRequest::GetValidationDiagnostic() const
	{
		return ValidationDiagnostic;
	}

	EArtifactPublishStatus FArtifactPublishReceipt::GetStatus() const
	{
		return Status;
	}

	const FString& FArtifactPublishReceipt::GetFinalPath() const
	{
		return FinalPath;
	}

	const FString& FArtifactPublishReceipt::GetPublisherOwnerId() const
	{
		return PublisherOwnerId;
	}

	const FString& FArtifactPublishReceipt::GetTransactionId() const
	{
		return TransactionId;
	}

	EWorkspaceKind FArtifactPublishReceipt::GetWorkspaceKind() const
	{
		return WorkspaceKind;
	}

	const FString& FArtifactPublishReceipt::GetEntryFile() const
	{
		return EntryFile;
	}

	const FString& FArtifactPublishReceipt::GetPlanFingerprint() const
	{
		return PlanFingerprint;
	}

	const FString& FArtifactPublishReceipt::GetSnapshotId() const
	{
		return SnapshotId;
	}

	const FString& FArtifactPublishReceipt::GetSnapshotRevisionSourceId() const
	{
		return SnapshotRevisionSourceId;
	}

	const FString& FArtifactPublishReceipt::GetSnapshotRevisionGeneration() const
	{
		return SnapshotRevisionGeneration;
	}

	int64 FArtifactPublishReceipt::GetSnapshotRevisionSequence() const
	{
		return SnapshotRevisionSequence;
	}

	const FString& FArtifactPublishReceipt::GetSnapshotEvidenceId() const
	{
		return SnapshotEvidenceId;
	}

	const TArray<FString>& FArtifactPublishReceipt::GetIncludedFiles() const
	{
		return IncludedFiles;
	}

	const TMap<FString, FString>& FArtifactPublishReceipt::GetFileHashes() const
	{
		return FileHashes;
	}

	const FString& FArtifactPublishReceipt::GetInventoryHash() const
	{
		return InventoryHash;
	}

	bool FArtifactPublishReceipt::WasSemanticWorkspaceVerified() const
	{
		return bSemanticWorkspaceVerified;
	}

	bool FArtifactPublishReceipt::WasTargetVersionRechecked() const
	{
		return bTargetVersionRechecked;
	}

	const TMap<FString, FString>&
	FArtifactPublishReceipt::GetSemanticVerificationFields() const
	{
		return SemanticVerificationFields;
	}

	FArtifactPublishError FArtifactPublishReceipt::MakeResultEncodingFailure(
		const FArtifactPublishError& Cause) const
	{
		FArtifactPublishError Error = FArtifactPublishError::WrapFailure(
			TEXT("artifact_published_result_invalid"), Cause, TEXT("result_encode"),
			TEXT("The workspace was published, but its command result could not be encoded."));
		Error.SourceKind = TEXT("native_result");
		Error.TargetPath = FinalPath;
		Error.TransactionStatus = TEXT("committed");
		Error.bRetrySafe = false;
		Error.RecoveryAction = TEXT("Read the already-published workspace before deciding whether to export again.");
		for (const FArtifactPublishError& Warning : CleanupWarnings) Error.Append(Warning);
		return Error;
	}

	void FArtifactPublishReceipt::SetPublished(
		const FArtifactPublishRequest& Request,
		FString InFinalPath,
		FString InTransactionId,
		TArray<FString> InIncludedFiles,
		TMap<FString, FString> InFileHashes,
		FString InInventoryHash)
	{
		const FExportWorkspacePlanRef& Plan = Request.GetPlan();
		const FUeAgentStateRevision& Revision = Plan->GetSnapshotRevision();
		Status = EArtifactPublishStatus::Published;
		FinalPath = MoveTemp(InFinalPath);
		PublisherOwnerId = Request.GetPublisherOwnerId();
		TransactionId = MoveTemp(InTransactionId);
		WorkspaceKind = Plan->GetWorkspaceKind();
		EntryFile = Plan->GetEntryFile();
		PlanFingerprint = Plan->GetPlanFingerprint();
		SnapshotId = Plan->GetSnapshotId();
		SnapshotRevisionSourceId = Revision.GetSourceId();
		SnapshotRevisionGeneration = Revision.GetGeneration();
		SnapshotRevisionSequence = Revision.GetSequence();
		SnapshotEvidenceId = Plan->GetSnapshotEvidenceId();
		IncludedFiles = MoveTemp(InIncludedFiles);
		FileHashes = MoveTemp(InFileHashes);
		InventoryHash = MoveTemp(InInventoryHash);
		bSemanticWorkspaceVerified = false;
		bTargetVersionRechecked = Request.RequiresVerification();
		SemanticVerificationFields.Reset(); // Compatibility receipt: no independent rebuild was performed.
	}

	

	

	

	

	static bool ReadOwnerFileHashes(const TSharedPtr<FJsonObject>& Marker, const FString& Root,
		TMap<FString, FString>& OutHashes, FArtifactPublishError& OutError, bool bGeneratedOnly)
	{
		OutHashes.Reset();
		const TArray<TSharedPtr<FJsonValue>>* Rows = nullptr;
		if (!Marker->TryGetArrayField(TEXT("files"), Rows)) {
			SetMarkerFieldError(Marker, TEXT("files"), TEXT("an array"), Root, OutError); return false;
		}
		for (const auto& Value : *Rows) {
			const TSharedPtr<FJsonObject>* Row = nullptr;
			FString Path, Relative, Hash;
			if (!Value || !Value->TryGetObject(Row) || !Row->IsValid()
				|| !(*Row)->TryGetStringField(TEXT("relative_path"), Path)
				|| !ValidateRelativePath(Path, Relative, OutError)
				|| !(*Row)->TryGetStringField(TEXT("content_hash"), Hash)
				|| OutHashes.Contains(Relative)) { OutHashes.Reset(); return false; }
			bool bGenerated = false;
			if (bGeneratedOnly && (!(*Row)->TryGetBoolField(TEXT("generated"), bGenerated) || !bGenerated)) continue;
			OutHashes.Add(MoveTemp(Relative), MoveTemp(Hash));
		}
		return true;
	}

	bool TryReadPublishedWorkspaceFileHashes(const FString& WorkspaceRoot, const FString& PublisherOwnerId,
		TMap<FString, FString>& OutHashes, FArtifactPublishError& OutError, bool bGeneratedOnly)
	{
		OutHashes.Reset();
		const FString Root = NormalizeFinalPath(WorkspaceRoot);
		TSharedPtr<FJsonObject> Marker;
		return LoadMatchingOwnerRecord(Root / OwnerMarkerName, Root, PublisherOwnerId, Marker, OutError)
			&& ReadOwnerFileHashes(Marker, Root, OutHashes, OutError, bGeneratedOnly);
	}

	bool TryRefreshPublishedWorkspace(
		const FString& CanonicalWorkspace, const FString& SourceWorkspace,
		const TArray<FString>& ManagedSourcePaths, FUeAgentResourceTracker& Resources,
		FArtifactPublishReceipt& OutReceipt, FArtifactPublishError& OutError)
	{
		const FString CanonicalRoot = NormalizeFinalPath(CanonicalWorkspace);
		TSharedPtr<FJsonObject> Marker;
		const TArray<TSharedPtr<FJsonValue>>* Rows = nullptr;
		const TSharedPtr<FJsonObject>* Revision = nullptr;
		FString Owner, Entry, Snapshot, Evidence, Fingerprint, Kind;
		FString RevisionSource, Generation;
		double Sequence = 0;
		if (!LoadJson(CanonicalRoot / OwnerMarkerName, SourceWorkspace, Marker, OutError)
			|| !ReadMarkerString(Marker, TEXT("publisher_owner_id"), Owner, SourceWorkspace, OutError)
			|| !ReadMarkerString(Marker, TEXT("entry_file"), Entry, SourceWorkspace, OutError)
			|| !ReadMarkerString(Marker, TEXT("workspace_kind"), Kind, SourceWorkspace, OutError)) return false;
		if (Kind != TEXT("directory"))
		{
			SetError(OutError, TEXT("artifact_refresh_source_invalid"), TEXT("Refreshing a published workspace requires a directory export."), SourceWorkspace, CanonicalRoot);
			SetPointer(OutError, TEXT("/workspace_kind"));
			OutError.ActualValue = MakeShared<FJsonValueString>(Kind);
			OutError.ExpectedValue = MakeShared<FJsonValueString>(TEXT("directory"));
			return false;
		}
		if (!ReadMarkerString(Marker, TEXT("snapshot_id"), Snapshot, SourceWorkspace, OutError)
			|| !ReadMarkerString(Marker, TEXT("snapshot_evidence_id"), Evidence, SourceWorkspace, OutError)
			|| !ReadMarkerString(Marker, TEXT("plan_fingerprint"), Fingerprint, SourceWorkspace, OutError)) return false;
		if (!Marker->TryGetArrayField(TEXT("files"), Rows))
		{
			SetMarkerFieldError(Marker, TEXT("files"), TEXT("an array"), SourceWorkspace, OutError);
			return false;
		}
		if (!Marker->TryGetObjectField(TEXT("snapshot_revision"), Revision) || !Revision->IsValid())
		{
			SetMarkerFieldError(Marker, TEXT("snapshot_revision"), TEXT("an object"), SourceWorkspace, OutError);
			return false;
		}
		if (!ReadMarkerString(*Revision, TEXT("source_id"), RevisionSource, SourceWorkspace, OutError, TEXT("/snapshot_revision"))
			|| !ReadMarkerString(*Revision, TEXT("generation"), Generation, SourceWorkspace, OutError, TEXT("/snapshot_revision"))) return false;
		if (!(*Revision)->TryGetNumberField(TEXT("sequence"), Sequence))
		{
			SetMarkerFieldError(*Revision, TEXT("sequence"), TEXT("a number"), SourceWorkspace, OutError, TEXT("/snapshot_revision"));
			return false;
		}
		TArray<FExportFilePlan> CanonicalFiles;
		for (int32 RowIndex = 0; RowIndex < Rows->Num(); ++RowIndex)
		{
			const auto& Value = (*Rows)[RowIndex];
			const FString RowPointer = FString::Printf(TEXT("/files/%d"), RowIndex);
			const TSharedPtr<FJsonObject>* Row = nullptr;
			FString Path, Relative, Hash;
			TArray<uint8> Bytes;
			if (!Value.IsValid() || !Value->TryGetObject(Row) || !Row->IsValid())
			{
				SetError(OutError, TEXT("artifact_refresh_source_invalid"), TEXT("An ownership inventory entry must be an object."), SourceWorkspace, CanonicalRoot);
				SetPointer(OutError, RowPointer);
				OutError.ActualValuePresent = Value.IsValid();
				OutError.ActualEvidence = Value;
				OutError.ExpectedValue = MakeShared<FJsonValueString>(TEXT("an object"));
				return false;
			}
			if (!ReadMarkerString(*Row, TEXT("relative_path"), Path, SourceWorkspace, OutError, RowPointer)
				|| !ValidateRelativePath(Path, Relative, OutError)
				|| !ReadMarkerString(*Row, TEXT("content_hash"), Hash, SourceWorkspace, OutError, RowPointer)) return false;
			if (!FFileHelper::LoadFileToArray(Bytes, *(CanonicalRoot / Relative)))
			{
				SetError(OutError, TEXT("artifact_refresh_source_invalid"), TEXT("Could not read a canonical export file required for workspace refresh."), Relative, CanonicalRoot / Relative);
				return false;
			}
			const FString ActualHash = Sha256(Bytes);
			if (ActualHash != Hash)
			{
				SetError(OutError, TEXT("artifact_refresh_source_invalid"), TEXT("The canonical export file content does not match its ownership inventory."), Relative, CanonicalRoot / Relative);
				SetPrivateComparison(OutError, ActualHash, Hash);
				return false;
			}
			CanonicalFiles.Emplace(Relative, MoveTemp(Bytes), Hash);
		}
		const FExportWorkspacePlanRef Plan = MakeShared<FExportWorkspacePlan>(
			EWorkspaceKind::Directory, Entry, Snapshot,
			FUeAgentStateRevision(RevisionSource, Generation, static_cast<int64>(Sequence)),
			Evidence, MoveTemp(CanonicalFiles), Fingerprint, 0);
		return TryRefreshPlannedWorkspace(Plan, Owner, SourceWorkspace, ManagedSourcePaths,
			Resources, OutReceipt, OutError);
	}

	bool TryRefreshPlannedWorkspace(
		FExportWorkspacePlanRef Plan, const FString& Owner, const FString& SourceWorkspace,
		const TArray<FString>& ManagedSourcePaths, FUeAgentResourceTracker& Resources,
		FArtifactPublishReceipt& OutReceipt, FArtifactPublishError& OutError)
	{
		return TryRefreshPlannedWorkspace(Plan, Owner, SourceWorkspace, ManagedSourcePaths, Resources, OutReceipt, OutError, {});
	}

	bool TryRefreshPlannedWorkspace(
		FExportWorkspacePlanRef Plan, const FString& Owner, const FString& SourceWorkspace,
		const TArray<FString>& ManagedSourcePaths, FUeAgentResourceTracker& Resources,
		FArtifactPublishReceipt& OutReceipt, FArtifactPublishError& OutError,
		TFunction<bool(FArtifactPublishError&)> SourceCheckpoint, FString ConsumerState,
		TSharedPtr<IArtifactPublishVerification> Verification)
	{
		const double RefreshStarted = FPlatformTime::Seconds();
		const FString SourceRoot = NormalizeFinalPath(SourceWorkspace);
		if (Plan->GetWorkspaceKind() != EWorkspaceKind::Directory)
		{
			SetError(OutError, TEXT("artifact_refresh_source_invalid"), TEXT("Refreshing a published workspace requires a directory export."), SourceWorkspace, SourceRoot);
			SetPointer(OutError, TEXT("/workspace_kind"));
			OutError.ActualValue = MakeShared<FJsonValueString>(TEXT("single_file"));
			OutError.ExpectedValue = MakeShared<FJsonValueString>(TEXT("directory"));
			return false;
		}
		TSet<FString> Managed;
		TSet<FString> LegacyInventory;
		TSharedPtr<FJsonObject> SourceMarker;
		const TArray<TSharedPtr<FJsonValue>>* SourceRows = nullptr;
		FArtifactPublishError OptionalMarkerError;
		// This source inventory is optional compatibility metadata; a nonmatching
		// record keeps the legacy inventory path rather than rejecting refresh here.
		if (LoadMatchingOwnerRecord(SourceRoot / OwnerMarkerName, SourceRoot, Owner, SourceMarker, OptionalMarkerError)
			&& SourceMarker->TryGetArrayField(TEXT("files"), SourceRows))
		{
			for (const auto& Value : *SourceRows)
			{
				const TSharedPtr<FJsonObject>* Row = nullptr;
				FString Path, Relative;
				FArtifactPublishError OptionalPathError;
				if (!Value.IsValid() || !Value->TryGetObject(Row)
					|| !(*Row)->TryGetStringField(TEXT("relative_path"), Path)
					|| !ValidateRelativePath(Path, Relative, OptionalPathError)) continue;
				bool bGenerated = false;
				if ((*Row)->TryGetBoolField(TEXT("generated"), bGenerated))
				{
					if (bGenerated) Managed.Add(Relative.ToLower());
				}
				else LegacyInventory.Add(Relative.ToLower());
			}
		}
		for (const FString& Path : ManagedSourcePaths)
		{
			FString Relative;
			if (!ValidateRelativePath(Path, Relative, OutError)) return false;
			Managed.Add(Relative.ToLower());
		}
		Managed.Add(FString(OwnerMarkerName).ToLower());
		TMap<FString, TArray<uint8>> Preserved;
		TArray<FString> ExistingFiles;
		IFileManager::Get().FindFilesRecursive(ExistingFiles, *SourceRoot, TEXT("*"), true, false);
		for (FString Path : ExistingFiles)
		{
			FString Relative = Path;
			if (!FPaths::MakePathRelativeTo(Relative, *(SourceRoot / TEXT(""))))
			{
				SetError(OutError, TEXT("artifact_path_invalid"), TEXT("Could not express an existing file path relative to the source workspace."), SourceWorkspace, Path);
				return false;
			}
			FPaths::NormalizeFilename(Relative);
			if (Managed.Contains(Relative.ToLower())) continue;
			if (Plan->GetRetainedFileHashes().Contains(Relative)) continue;
			TArray<uint8> Bytes;
			if (!FFileHelper::LoadFileToArray(Bytes, *Path))
			{
				SetError(OutError, TEXT("artifact_preserved_file_read_failed"), TEXT("Could not read an existing source file that must be preserved during refresh."), Relative, Path);
				return false;
			}
			// Older receipts did not distinguish generated files from preserved user files.
			// Coalesce only byte-identical collisions; never infer ownership from a suffix.
			if (LegacyInventory.Contains(Relative.ToLower())
				&& Plan->GetFiles().ContainsByPredicate([&](const FExportFilePlan& File)
				{
					return File.GetRelativePath().Equals(Relative, ESearchCase::IgnoreCase)
						&& File.GetCanonicalUtf8() == Bytes;
				})) continue;
			Preserved.Add(Relative, MoveTemp(Bytes));
		}
		FArtifactPublishRequest Request(Plan, SourceRoot, Owner, MoveTemp(Preserved));
		Request.SetVerification(MoveTemp(Verification));
		Request.SetConsumerState(MoveTemp(ConsumerState));
		Request.SetSourceCheckpoint(MoveTemp(SourceCheckpoint));
		Request.SetManagedSourcePaths(ManagedSourcePaths);
		const double PreserveFinished = FPlatformTime::Seconds();
		const bool bPublished = TryPublishExportPlan(Request, Resources, OutReceipt, OutError);
		UE_LOG(LogTemp, Display, TEXT("UAI export timing phase=artifact_refresh_outer preserve_prepare_s=%.6f publish_s=%.6f total_s=%.6f"),
			PreserveFinished - RefreshStarted, FPlatformTime::Seconds() - PreserveFinished, FPlatformTime::Seconds() - RefreshStarted);
		return bPublished;
	}

	bool TryReadWorkspaceConsumerState(const FString& WorkspaceRoot, const FString& PublisherOwnerId,
		FString& OutState, FArtifactPublishError& OutError)
	{
		FString Owner;
		return TryReadWorkspaceConsumerState(WorkspaceRoot, PublisherOwnerId, OutState, Owner, OutError);
	}

	bool TryReadWorkspaceConsumerState(const FString& WorkspaceRoot, const FString& PublisherOwnerId,
		FString& OutState, FString& OutPublisherOwnerId, FArtifactPublishError& OutError)
	{
		return TryReadWorkspaceConsumerState(WorkspaceRoot, PublisherOwnerId, OutState, OutPublisherOwnerId, OutError, nullptr);
	}

	static bool ReadWorkspaceConsumerStateSource(const FString& WorkspaceRoot, const FString& PublisherOwnerId,
		FString& OutState, FString& OutPublisherOwnerId, FArtifactPublishError& OutError,
		TMap<FString, FString>* OutFileHashes, const FString* ObservedText)
	{
		OutState.Reset();
		OutPublisherOwnerId.Reset();
		const FString Root = NormalizeFinalPath(WorkspaceRoot);
		TSharedPtr<FJsonObject> Marker;
		FString Owner = PublisherOwnerId;
		if (OutFileHashes) OutFileHashes->Reset();
		if (!(ObservedText ? ParseJson(*ObservedText, Root / OwnerMarkerName, Root, Marker, OutError)
			: LoadJson(Root / OwnerMarkerName, Root, Marker, OutError))) return false;
		if (Owner.IsEmpty()) {
			if (!ReadMarkerString(Marker, TEXT("publisher_owner_id"), Owner, Root, OutError)) return false;
		}
		if (!ValidateMatchingOwnerRecord(Root / OwnerMarkerName, Root, Owner, Marker, OutError)) return false;
		OutPublisherOwnerId = Owner;
		if (Marker->HasField(TEXT("consumer_state"))
			&& !ReadMarkerString(Marker, TEXT("consumer_state"), OutState, Root, OutError)) return false;
		return !OutFileHashes || ReadOwnerFileHashes(Marker, Root, *OutFileHashes, OutError, false);
	}

	bool TryReadWorkspaceConsumerState(const FString& WorkspaceRoot, const FString& PublisherOwnerId,
		FString& OutState, FString& OutPublisherOwnerId, FArtifactPublishError& OutError,
		TMap<FString, FString>* OutFileHashes)
	{
		return ReadWorkspaceConsumerStateSource(WorkspaceRoot, PublisherOwnerId,
			OutState, OutPublisherOwnerId, OutError, OutFileHashes, nullptr);
	}

	bool TryReadObservedWorkspaceConsumerState(const FString& WorkspaceRoot, const FString& PublisherOwnerId,
		const FString& ObservedText, FString& OutState, FString& OutPublisherOwnerId,
		FArtifactPublishError& OutError, TMap<FString, FString>* OutFileHashes)
	{
		return ReadWorkspaceConsumerStateSource(WorkspaceRoot, PublisherOwnerId,
			OutState, OutPublisherOwnerId, OutError, OutFileHashes, &ObservedText);
	}

	bool TryRecoverWorkspacePublication(const FString& WorkspaceRoot, FArtifactPublishError& OutError)
	{
		const FString Root = NormalizeFinalPath(WorkspaceRoot);
		const FString Parent = FPaths::GetPath(Root), Leaf = FPaths::GetCleanFilename(Root);
		TArray<FString> LegacyJournals;
		IFileManager::Get().FindFiles(LegacyJournals, *(Parent / (TEXT(".") + Leaf + TEXT(".uai-*.journal.json"))), true, false);
		const bool bGeneration = IFileManager::Get().FileExists(*GenerationJournalPath(Root))
			|| IFileManager::Get().FileExists(*(GenerationJournalPath(Root) + TEXT(".next")));
		if (!bGeneration && LegacyJournals.IsEmpty()) return true;
		FGenerationPublishLock Lock;
		if (!Lock.Acquire(Root, OutError)) return false;
		LegacyJournals.Sort();
		for (const FString& Name : LegacyJournals) {
			const FString Path = Parent / Name;
			TSharedPtr<FJsonObject> Journal;
			FString Format, Transaction, Final, Staging, Backup, State, Publisher;
			FArtifactPublishError ReadError;
			const bool bRead = LoadJson(Path, Root, Journal, ReadError)
				&& ReadMarkerString(Journal, TEXT("format"), Format, Root, ReadError)
				&& ReadMarkerString(Journal, TEXT("transaction_id"), Transaction, Root, ReadError)
				&& ReadMarkerString(Journal, TEXT("publisher_owner_id"), Publisher, Root, ReadError)
				&& ReadMarkerString(Journal, TEXT("final_path"), Final, Root, ReadError)
				&& ReadMarkerString(Journal, TEXT("staging_path"), Staging, Root, ReadError)
				&& ReadMarkerString(Journal, TEXT("backup_path"), Backup, Root, ReadError)
				&& ReadMarkerString(Journal, TEXT("state"), State, Root, ReadError);
			bool bTransaction = Transaction.Len() == 32;
			for (TCHAR Character : Transaction) bTransaction &= FChar::IsHexDigit(Character);
			const FString Prefix = Parent / (TEXT(".") + Leaf + TEXT(".uai-") + Transaction);
			const bool bBound = bRead && Format == FormatId && bTransaction && !Publisher.IsEmpty()
				&& NormalizeFinalPath(Final) == Root && NormalizeFinalPath(Staging) == Prefix + TEXT(".staging")
				&& NormalizeFinalPath(Backup) == Prefix + TEXT(".backup") && Path == Prefix + TEXT(".journal.json");
			if (bBound && State == TEXT("published_verified")) {
				// This old producer recorded completed verification. Its leftover
				// backup is cleanup material, not permission to restore over later
				// author edits or later publications. Leave it for explicit cleanup.
				continue;
			}
			if (bBound) {
				TSharedPtr<FJsonObject> CurrentOwner;
				FString CurrentTransaction;
				FArtifactPublishError CurrentError;
				if (LoadMatchingOwnerRecord(Root / OwnerMarkerName, Root, Publisher, CurrentOwner, CurrentError)
					&& ReadMarkerString(CurrentOwner, TEXT("transaction_id"), CurrentTransaction, Root, CurrentError)
					&& CurrentTransaction != Transaction) {
					// A different complete directory publication is current. The old
					// transaction cannot be used to replace it. Retain its unresolved
					// materials without making them a gate for the current generation.
					UE_LOG(LogTemp, Warning, TEXT("UAI legacy publication retained path=%s current_generation=other action=inspect_without_restore"), *Path);
					continue;
				}
			}
			SetError(OutError, TEXT("artifact_publish_state_unknown"),
				TEXT("A retained legacy publication has no confirmed final state. Its original journal, staging and backup were preserved; inspect the recorded destination before continuing this workspace."), Root, Path);
			OutError.RollbackAttempted = false;
			OutError.RecoveryAction = TEXT("inspect_original_legacy_publication_without_native_replay");
			if (!ReadError.IsEmpty()) OutError.Append(ReadError);
			return false;
		}
		return !bGeneration || RecoverGenerationJournal(Root, OutError);
	}

	bool TryPublishExportPlan(
		const FArtifactPublishRequest& Request,
		FUeAgentResourceTracker& Resources,
		FArtifactPublishReceipt& OutReceipt,
		FArtifactPublishError& OutError)
	{
		const double PublishStarted = FPlatformTime::Seconds();
		OutReceipt = {};
		OutError = {};
		if (!Request.IsValid())
		{
			OutError = Request.GetValidationDiagnostic();
			return false;
		}

		TArray<FString> Paths;
		TMap<FString, FString> Hashes;
		if (!ValidatePlan(Request, Resources, Paths, Hashes, OutError))
		{
			return false;
		}
		const double PreflightFinished = FPlatformTime::Seconds();

		FPublishPaths Transaction;
		if (!BuildPaths(Request, Transaction, OutError)) return false;
		IFileManager& Files = IFileManager::Get();
		const FExportFilePlan* SemanticManifest = Request.GetPlan()->GetFiles().FindByPredicate(
			[](const FExportFilePlan& File) { return File.GetRelativePath() == TEXT(".uai/workspace.hjson"); });
		FString PlannedSchema, PlannedGeneration, PlannedState;
		bool bGenerationIsString = false;
		if (SemanticManifest) {
			const FString& Hash = SemanticManifest->GetContentHash();
			const auto HeaderFacts = SemanticManifest->GetRootHeaderFacts();
			const bool bSchemaProven = TryReadHjsonRootString(HeaderFacts, Hash, TEXT("workspace_schema"), PlannedSchema);
			const bool bHeaderProven = bSchemaProven && (!IsGenerationWorkspaceSchema(PlannedSchema)
				|| (TryReadHjsonRootString(HeaderFacts, Hash, TEXT("publication_generation"), PlannedGeneration)
					&& TryReadHjsonRootString(HeaderFacts, Hash, TEXT("publication_state"), PlannedState)));
			if (bHeaderProven) bGenerationIsString = true;
			else {
				if (!Request.RequiresVerification()) {
					SetError(OutError, TEXT("workspace_publication_header_handoff_missing"),
						TEXT("The generated publication plan has no matching formatter header fields. Publication stopped before writing; no alternate parser was selected."), SemanticManifest->GetRelativePath());
					return false;
				}
				// Explicit unverified external input has its original parser contract.
				const auto& Bytes = SemanticManifest->GetCanonicalUtf8();
				FUTF8ToTCHAR Converted(reinterpret_cast<const ANSICHAR*>(Bytes.GetData()), Bytes.Num());
				FWorkspaceError ParseError; FHjsonDocumentPtr Document;
				if (!TryParseHjsonDocument(FString(Converted.Length(), Converted.Get()), SemanticManifest->GetRelativePath(), Resources, Document, ParseError)) {
					OutError = ParseError.GetDiagnostic(); return false;
				}
				const auto* Schema = Document->GetRoot()->Get(TEXT("workspace_schema"));
				const auto* Generation = Document->GetRoot()->Get(TEXT("publication_generation"));
				const auto* State = Document->GetRoot()->Get(TEXT("publication_state"));
				PlannedSchema = Schema ? Schema->GetText() : FString();
				PlannedGeneration = Generation ? Generation->GetText() : FString();
				PlannedState = State ? State->GetText() : FString();
				bGenerationIsString = Generation && Generation->GetKind() == EHjsonNodeKind::String;
			}
			UE_LOG(LogTemp, Display, TEXT("UAI export timing phase=publication_header producer_handoff=%s full_header_parser=%s"), bHeaderProven ? TEXT("true") : TEXT("false"), bHeaderProven ? TEXT("false") : TEXT("true"));
		}
		const bool bGenerationReadSet = IsGenerationWorkspaceSchema(PlannedSchema)
			&& !Request.RequiresVerification() && !Transaction.bSingleFile
			&& Files.FileExists(*(Transaction.Final / TEXT(".uai/workspace.hjson")));
		if (IsGenerationWorkspaceSchema(PlannedSchema)) {
			FWorkspaceError SourceInputs;
			if (!ValidateWorkspaceFileVersionInputs(Transaction.Final, *Request.GetPlan(), SourceInputs)) {
				OutError = SourceInputs.GetDiagnostic(); return false;
				}
			}
		// /3 checks the same read-only policy on the later complete, strongly
		// guarded read set. Do not enumerate/stat that set once here and again
		// in TryPublishGeneration. Initial and /2 publication retain the old path.
		if (!ExistingFinalIsOwned(
				Transaction,
				Request.GetPublisherOwnerId(),
				OutError, bGenerationReadSet))
		{
			return false;
		}
		const bool bStagingExists = Transaction.bSingleFile
			? Files.FileExists(*Transaction.Staging)
			: Files.DirectoryExists(*Transaction.Staging);
		const bool bBackupExists = Transaction.bSingleFile
			? Files.FileExists(*Transaction.Backup)
			: Files.DirectoryExists(*Transaction.Backup);
		FString ConflictKind;
		FString ConflictPath;
		if (bStagingExists) { ConflictKind = TEXT("staging output"); ConflictPath = Transaction.Staging; }
		else if (bBackupExists) { ConflictKind = TEXT("backup output"); ConflictPath = Transaction.Backup; }
		else if (Files.FileExists(*Transaction.Journal)) { ConflictKind = TEXT("journal"); ConflictPath = Transaction.Journal; }
		else if (Files.FileExists(*Transaction.StagingOwner)) { ConflictKind = TEXT("staged ownership record"); ConflictPath = Transaction.StagingOwner; }
		else if (Files.FileExists(*Transaction.BackupOwner)) { ConflictKind = TEXT("backup ownership record"); ConflictPath = Transaction.BackupOwner; }
		if (!ConflictKind.IsEmpty())
		{
			SetError(
				OutError,
				TEXT("artifact_transaction_path_conflict"),
				FString::Printf(TEXT("The export transaction %s path is already occupied."), *ConflictKind), Transaction.Final, ConflictPath);
			OutError.ActualValue = MakeShared<FJsonValueBoolean>(true);
			OutError.ExpectedValue = MakeShared<FJsonValueBoolean>(false);
			return false;
		}
		if (!Files.MakeDirectory(*Transaction.Parent, true))
		{
			SetError(
				OutError,
				TEXT("artifact_parent_create_failed"),
				TEXT("Could not create the export destination's parent directory."), Transaction.Final, Transaction.Parent);
			return false;
		}

		const FString Inventory = InventoryHash(Paths, Hashes);
		if (SemanticManifest) {
			if (IsGenerationWorkspaceSchema(PlannedSchema)) {
				FGuid GenerationId;
				if (!bGenerationIsString || !FGuid::ParseExact(PlannedGeneration, EGuidFormats::Digits, GenerationId)
					|| PlannedState != TEXT("committed")) {
					SetError(OutError, TEXT("workspace_generation_plan_invalid"), TEXT("Schema /3 export requires a fresh committed publication generation."), Transaction.Final); return false;
				}
				if (Files.FileExists(*(Transaction.Final / TEXT(".uai/workspace.hjson")))) {
					FWorkspacePublication Previous; FWorkspaceError PublicationError;
					if (!TryReadWorkspacePublication(Transaction.Final, Previous, PublicationError)) { OutError = PublicationError.GetDiagnostic(); return false; }
					if (Previous.bGenerationPublication && Previous.Generation == PlannedGeneration) {
						SetError(OutError, TEXT("workspace_generation_baseline_incompatible"), TEXT("The new publication must use a different generation from the accepted baseline."), Transaction.Final); return false;
					}
#if PLATFORM_WINDOWS
					if (!Request.RequiresVerification()) {
						TArray<FArtifactPublishError> Warnings;
						const double GenerationStarted = FPlatformTime::Seconds();
						if (!TryPublishGeneration(Request, Transaction, Paths, Hashes, Inventory, Resources, PlannedSchema, PlannedGeneration, OutError, Warnings)) return false;
						FWorkspaceError SourceHandoff;
						if (!RememberPublishedWorkspaceFileVersions(Transaction.Final, *Request.GetPlan(), PlannedGeneration, Request.GetConsumerState(), SourceHandoff)) {
							OutError = SourceHandoff.GetDiagnostic(); return false;
						}
						const double GenerationReturned = FPlatformTime::Seconds();
						OutReceipt.SetPublished(Request, Transaction.Final, Transaction.TransactionId, MoveTemp(Paths), MoveTemp(Hashes), Inventory);
						OutReceipt.CleanupWarnings = MoveTemp(Warnings);
						UE_LOG(LogTemp, Display, TEXT("UAI export timing phase=artifact_generation_outer preflight_s=%.6f preparation_s=%.6f generation_s=%.6f receipt_s=%.6f total_s=%.6f"),
							PreflightFinished - PublishStarted, GenerationStarted - PreflightFinished,
							GenerationReturned - GenerationStarted, FPlatformTime::Seconds() - GenerationReturned,
							FPlatformTime::Seconds() - PublishStarted);
						return true;
					}
					SetError(OutError, TEXT("workspace_generation_verification_unsupported"),
						TEXT("This staged verification contract requires a complete staging directory. Use a new schema /2 workspace; schema /3 cannot silently bypass its protected commit."), Transaction.Final);
					return false;
#else
					SetError(OutError, TEXT("workspace_generation_backend_unavailable"),
						TEXT("Schema /3 publication requires the Windows file-proof backend. Use a new schema /2 workspace on this platform."), Transaction.Final);
					return false;
#endif
				}
			}
		}
		if (!Request.GetPlan()->GetRetainedFileHashes().IsEmpty()) {
			SetError(OutError, TEXT("artifact_plan_requires_existing_generation"),
				TEXT("Retained generated files require publication into their existing generation workspace; no replacement bodies were supplied."), Transaction.Final);
			return false;
		}
		bool bOldMoved = false;
		bool bNewPublished = false;
		TArray<FArtifactPublishError> PendingWarnings;
		const double StageStarted = FPlatformTime::Seconds();
		if (!StageFiles(
			Request,
			Transaction,
			Resources,
			Hashes,
			OutError))
		{
			CleanupStaging(Transaction, false, OutError);
			return false;
		}
		const double StageFinished = FPlatformTime::Seconds();
		if (!SaveJson(
				Transaction.StagingOwner,
				BuildOwnerRecord(
					Request,
					Transaction.Final,
					Paths,
					Hashes,
					Inventory), Transaction.Final, TEXT("staged ownership record"), OutError)
			|| !VerifyInventory(
				Transaction.Staging,
				Transaction.bSingleFile,
				Paths,
				Hashes, Transaction.Final, OutError)
			|| !SaveJournal(
				Transaction,
				Request,
				TEXT("staged"), OutError))
		{
			CleanupStaging(Transaction, true, OutError);
			return false;
		}
		const double StageVerified = FPlatformTime::Seconds();

		if (Request.RequiresVerification()
			&& !Request.GetVerification()->TryRecheckTargetVersion(OutError))
		{
			if (OutError.IsEmpty())
			{
				SetError(
					OutError,
					TEXT("artifact_target_version_recheck_failed"),
					TEXT("The export target version could not be revalidated before publication."), Transaction.Final);
			}
			CleanupStaging(Transaction, true, OutError);
			return false;
		}

		const double TargetRechecked = FPlatformTime::Seconds();
		if (!Request.RecheckSource(OutError)) {
			CleanupStaging(Transaction, true, OutError);
			return false;
		}
		const double SourceRechecked = FPlatformTime::Seconds();
		double BackupMoved = SourceRechecked, BackupJournaled = SourceRechecked;
		const bool bFinalExists = Transaction.bSingleFile
			? Files.FileExists(*Transaction.Final)
			: Files.DirectoryExists(*Transaction.Final);
		// v2 consumes its source checkpoint once, above. These RH observations
		// are invalidation caches, not a commit lock, and have no later consumer
		// in this directory-swap transaction. v3 retains its separate W/R guards.
		const int32 RetiredSourceLeases = bFinalExists
			? RetireWorkspaceFileObservationsForRename(Transaction.Final, !Transaction.bSingleFile) : 0;
		const double SourceLeasesRetired = FPlatformTime::Seconds();
		BackupMoved = SourceLeasesRetired; BackupJournaled = SourceLeasesRetired;
		FArtifactPublishError MoveError;
		if (bFinalExists)
		{
			if (!MovePath(Transaction.Backup, Transaction.Final, Transaction.Final, MoveError))
			{
				SetError(
					OutError,
					TEXT("artifact_publish_failed"),
					TEXT("The previous export could not be moved to its transaction backup."), Transaction.Final);
				OutError.PublicCause = MakeShared<FArtifactPublishError>(MoveError);
				CleanupStaging(Transaction, true, OutError);
				return false;
			}
			bOldMoved = true;
			if (Transaction.bSingleFile
				&& !MovePath(
					Transaction.BackupOwner,
					Transaction.FinalOwner, Transaction.Final, MoveError))
			{
				FinishFailedPublish(Transaction, bOldMoved, bNewPublished, TEXT("artifact_publish_failed"), MoveError, PendingWarnings, OutError);
				return false;
			}
			BackupMoved = FPlatformTime::Seconds();
			FArtifactPublishError JournalWarning;
			if (!SaveJournal(
				Transaction,
				Request,
				TEXT("old_final_backed_up"), JournalWarning))
			{
				JournalWarning.Severity = UE::UAI::Contract::ESeverity::Warning;
				PendingWarnings.Add(MoveTemp(JournalWarning));
			}
			BackupJournaled = FPlatformTime::Seconds();
			if (ShouldFailAfterFinalMovedToBackup())
			{
				FArtifactPublishError InjectedError;
				SetError(InjectedError, TEXT("artifact_publish_injected_failure"), TEXT("The publish failure hook rejected the transaction after the previous export was backed up."), Transaction.Final);
				FinishFailedPublish(Transaction, bOldMoved, bNewPublished, TEXT("artifact_publish_failed"), InjectedError, PendingWarnings, OutError);
				return false;
			}
		}

		if (!MovePath(
			Transaction.Final,
			Transaction.Staging,
			Transaction.Final, MoveError))
		{
			FinishFailedPublish(Transaction, bOldMoved, bNewPublished, TEXT("artifact_publish_failed"), MoveError, PendingWarnings, OutError);
			return false;
		}
		bNewPublished = true;
		if (Transaction.bSingleFile
			&& !MovePath(
				Transaction.FinalOwner,
				Transaction.StagingOwner, Transaction.Final, MoveError))
		{
			FinishFailedPublish(Transaction, bOldMoved, bNewPublished, TEXT("artifact_publish_failed"), MoveError, PendingWarnings, OutError);
			return false;
		}

		const double CommitFinished = FPlatformTime::Seconds();
		if (SemanticManifest && IsGenerationWorkspaceSchema(PlannedSchema)) {
			FWorkspaceError SourceHandoff;
			if (!BeginExplicitWorkspaceFileObservation(Transaction.Final, SourceHandoff, true)) {
				OutError = SourceHandoff.GetDiagnostic(); return false;
			}
		}
		UE_LOG(LogTemp, Display, TEXT("UAI publish timing phase=directory_commit target_recheck_s=%.6f source_checkpoint_s=%.6f source_lease_retire_s=%.6f retired_leases=%d backup_move_s=%.6f backup_journal_s=%.6f final_move_s=%.6f total_s=%.6f"),
			TargetRechecked - StageVerified, SourceRechecked - TargetRechecked, SourceLeasesRetired - SourceRechecked,
			RetiredSourceLeases, BackupMoved - SourceLeasesRetired,
			BackupJournaled - BackupMoved, CommitFinished - BackupJournaled, CommitFinished - StageVerified);
		if (!VerifyFinal(
			Transaction,
			Request,
			Paths,
			Hashes,
			Inventory, OutError))
		{
			// These are optional observations of the new final, not commit guards.
			// A failed verification must retire them before rollback removes it.
			RetireWorkspaceFileObservationsForRename(Transaction.Final, !Transaction.bSingleFile);
			FinishFailedPublish(Transaction, bOldMoved, bNewPublished, TEXT("artifact_publish_verification_failed"), OutError, PendingWarnings, OutError);
			return false;
		}

		const double FinalVerified = FPlatformTime::Seconds();
		if (SemanticManifest && IsGenerationWorkspaceSchema(PlannedSchema)) {
			FWorkspaceError SourceHandoff;
			if (!RememberPublishedWorkspaceFileVersions(Transaction.Final, *Request.GetPlan(), PlannedGeneration, Request.GetConsumerState(), SourceHandoff)) {
				OutError = SourceHandoff.GetDiagnostic(); return false;
			}
		}
		OutReceipt.SetPublished(
			Request, Transaction.Final, Transaction.TransactionId,
			MoveTemp(Paths), MoveTemp(Hashes), Inventory);
		OutReceipt.CleanupWarnings.Append(PendingWarnings);
		FArtifactPublishError CleanupWarning;
		const double CleanupJournalStarted = FPlatformTime::Seconds();
		if (!SaveJournal(Transaction, Request, TEXT("published_verified"), CleanupWarning))
		{
			CleanupWarning.Severity = UE::UAI::Contract::ESeverity::Warning;
			OutReceipt.CleanupWarnings.Add(CleanupWarning);
		}
		const double BackupCleanupStarted = FPlatformTime::Seconds();
		bool bCleanupComplete = true;
		if (bOldMoved
			&& (!DeleteDataPath(Transaction, Transaction.Backup, TEXT("previous-output backup"), CleanupWarning)
				|| (Transaction.bSingleFile
					&& !DeleteFile(Transaction.BackupOwner, Transaction.Final, TEXT("backup ownership record"), CleanupWarning))))
		{
			CleanupWarning.Severity = UE::UAI::Contract::ESeverity::Warning;
			OutReceipt.CleanupWarnings.Add(CleanupWarning);
			bCleanupComplete = false;
		}
		const double JournalDeleteStarted = FPlatformTime::Seconds();
		// Keep the journal while backup cleanup is pending, so recovery retains ownership.
		if (bCleanupComplete && !DeleteFile(Transaction.Journal, Transaction.Final, TEXT("transaction journal"), CleanupWarning))
		{
			CleanupWarning.Severity = UE::UAI::Contract::ESeverity::Warning;
			OutReceipt.CleanupWarnings.Add(CleanupWarning);
		}
		const double PublishFinished = FPlatformTime::Seconds();
		UE_LOG(LogTemp, Display, TEXT("UAI export timing phase=directory_cleanup receipt_s=%.6f journal_save_s=%.6f backup_delete_s=%.6f journal_delete_s=%.6f"),
			CleanupJournalStarted - FinalVerified, BackupCleanupStarted - CleanupJournalStarted,
			JournalDeleteStarted - BackupCleanupStarted, PublishFinished - JournalDeleteStarted);
		UE_LOG(LogTemp, Display, TEXT("UAI export timing phase=publish files=%d preflight_s=%.6f setup_s=%.6f stage_s=%.6f staged_inventory_s=%.6f commit_s=%.6f final_verify_s=%.6f cleanup_s=%.6f total_s=%.6f success=true"),
			Request.GetPlan()->GetFiles().Num(), PreflightFinished - PublishStarted,
			StageStarted - PreflightFinished, StageFinished - StageStarted,
			StageVerified - StageFinished, CommitFinished - StageVerified,
			FinalVerified - CommitFinished, PublishFinished - FinalVerified,
			PublishFinished - PublishStarted);
		return true;
	}

	namespace TestHooks
	{
		FScopedPublishFailure::FScopedPublishFailure(
			const EFailurePoint InFailurePoint)
			: FailurePoint(InFailurePoint)
		{
			FScopeLock Lock(&FailureMutex);
			if (FailurePoint == EFailurePoint::AfterFinalMovedToBackup)
			{
				++AfterFinalMovedToBackupFailures;
			}
			else if (FailurePoint == EFailurePoint::FirstMoveAttempt)
			{
				++FirstMoveAttemptFailures;
			}
		}

		FScopedPublishFailure::~FScopedPublishFailure()
		{
			FScopeLock Lock(&FailureMutex);
			if (FailurePoint == EFailurePoint::AfterFinalMovedToBackup)
			{
				AfterFinalMovedToBackupFailures =
					FMath::Max(0, AfterFinalMovedToBackupFailures - 1);
			}
			else if (FailurePoint == EFailurePoint::FirstMoveAttempt)
			{
				FirstMoveAttemptFailures =
					FMath::Max(0, FirstMoveAttemptFailures - 1);
			}
		}
	}
}
