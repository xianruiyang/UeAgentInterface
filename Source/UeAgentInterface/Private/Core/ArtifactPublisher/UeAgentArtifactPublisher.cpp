#include "Core/ArtifactPublisher/UeAgentArtifactPublisher.h"

#include "Core/ArtifactPublisher/UeAgentArtifactPublisherTestHooks.h"
#include "Core/HjsonWorkspace/UeAgentHjsonValidatedHeader.h"
#include "Core/HjsonWorkspace/UeAgentSemanticWorkspaceResolver.h"
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
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"
#include "Serialization/JsonWriter.h"

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

		// This proves syntax only. Each candidate came from a successful parse of
		// real staged-file readback with this parser. Publication still verifies
		// every current byte/hash, the complete workspace and target version.
		struct FValidatedArtifactSyntax
		{
			TMap<FString, FString> ContentHashes;
			TMap<FString, TMap<EUeAgentResourceDimension, int64>> ParseCharges;
		};
		thread_local TSharedPtr<const FValidatedArtifactSyntax> PreviousArtifactSyntax;

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

		bool LoadJson(
			const FString& Path,
			const FString& Target,
			TSharedPtr<FJsonObject>& OutObject,
			FArtifactPublishError& OutError)
		{
			FString Text;
			if (!FFileHelper::LoadFileToString(Text, *Path))
			{
				SetError(OutError, TEXT("artifact_owner_marker_read_failed"),
					TEXT("The export ownership record could not be read."), Target, Path);
				return false;
			}
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

		bool LoadMatchingOwnerRecord(const FString& MarkerPath, const FString& FinalPath,
			const FString& OwnerId, TSharedPtr<FJsonObject>& Marker, FArtifactPublishError& OutError)
		{
			FString Format, MarkerOwner, MarkerFinal;
			if (!LoadJson(MarkerPath, FinalPath, Marker, OutError)
				|| !ReadMarkerString(Marker, TEXT("format"), Format, FinalPath, OutError)) return false;
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

		bool ExistingFinalIsOwned(
			const FPublishPaths& Paths,
			const FString& OwnerId,
			FArtifactPublishError& OutError)
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
			else
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
				const FString ActualHash = Sha256(File.GetCanonicalUtf8());
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
				OutPaths.Add(Path);
				OutHashes.Add(Path, File.GetContentHash());
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

		bool WriteAndVerifyStagedFile(const FString& Destination, const FString& Relative,
			const TArray<uint8>& Expected, const FString* ExpectedHash,
			const FString& WriteCode, const FString& VerifyCode,
			TArray<uint8>& Observed, FArtifactPublishError& OutError)
		{
			if (!IFileManager::Get().MakeDirectory(*FPaths::GetPath(Destination), true))
			{
				SetError(OutError, WriteCode, TEXT("Could not create the staging file's parent directory."), Relative, Destination);
				return false;
			}
			if (!FFileHelper::SaveArrayToFile(Expected, *Destination))
			{
				SetError(OutError, WriteCode, TEXT("Could not write the staged export file."), Relative, Destination);
				return false;
			}
			if (!FFileHelper::LoadFileToArray(Observed, *Destination))
			{
				SetError(OutError, VerifyCode, TEXT("Could not read back the staged export file."), Relative, Destination);
				return false;
			}
			if (Observed != Expected)
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
			if (ExpectedHash)
			{
				const FString ActualHash = Sha256(Observed);
				if (ActualHash != *ExpectedHash)
				{
					SetError(OutError, VerifyCode, TEXT("The staged export file does not match its expected content hash."), Relative, Destination);
					SetPrivateComparison(OutError, ActualHash, *ExpectedHash);
					return false;
				}
			}
			return true;
		}

		bool StageAndReparse(
			const FArtifactPublishRequest& Request,
			const FPublishPaths& Paths,
			FUeAgentResourceTracker& Resources,
			const TMap<FString, FString>& Hashes,
			FArtifactPublishError& OutError)
		{
			const double StageStarted = FPlatformTime::Seconds();
			double WriteReadbackSeconds = 0.0;
			double ParseSeconds = 0.0;
			int32 ReusedSyntaxFiles = 0;
			const auto SyntaxBaseline = PreviousArtifactSyntax;
			TMap<FString, TMap<EUeAgentResourceDimension, int64>> ParseCharges;
			IFileManager& Files = IFileManager::Get();
			if (!Files.MakeDirectory(
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
			for (const FExportFilePlan& File : Request.GetPlan()->GetFiles())
			{
				FString Relative;
				if (!ValidateRelativePath(File.GetRelativePath(), Relative, OutError)) return false;
				const FString Destination = Paths.bSingleFile
					? Paths.Staging
					: Paths.Staging / Relative;
				TArray<uint8> Observed;
				const double WriteStarted = FPlatformTime::Seconds();
				if (!WriteAndVerifyStagedFile(Destination, Relative, File.GetCanonicalUtf8(), &Hashes.FindChecked(Relative),
					TEXT("artifact_staging_write_failed"), TEXT("artifact_staging_verification_failed"), Observed, OutError)) return false;
				const double ParseStarted = FPlatformTime::Seconds();
				WriteReadbackSeconds += ParseStarted - WriteStarted;
				const FString* PreviousHash = SyntaxBaseline.IsValid()
					? SyntaxBaseline->ContentHashes.Find(Relative) : nullptr;
				const auto* PreviousCharges = SyntaxBaseline.IsValid() ? SyntaxBaseline->ParseCharges.Find(Relative) : nullptr;
				if (PreviousHash && PreviousCharges)
				{
					// WriteAndVerifyStagedFile has just hashed the actual readback;
					// use that same content proof instead of retaining giant files.
					if (PreviousHash->Equals(Hashes.FindChecked(Relative), ESearchCase::CaseSensitive))
					{
						FUeAgentResourceChargeError ResourceError;
						if (!Resources.TryChargeMany(*PreviousCharges,
							TEXT("artifact_publish.validated_syntax"), ResourceError))
						{
							OutError = ResourceError;
							return false;
						}
						ParseCharges.Add(Relative, *PreviousCharges);
						++ReusedSyntaxFiles;
						ParseSeconds += FPlatformTime::Seconds() - ParseStarted;
						continue;
					}
				}
				Observed.Add(0);
				const FString Text = UTF8_TO_TCHAR(
					reinterpret_cast<const char*>(Observed.GetData()));
				Observed.Pop(EAllowShrinking::No);
				FHjsonDocumentPtr Document;
				FWorkspaceError ParseError;
				const auto UsageBefore = Resources.Snapshot();
				if (!TryParseHjsonDocument(
					Text,
					Relative,
					Resources,
					Document,
					ParseError))
				{
					OutError = ParseError.GetDiagnostic();
					if (OutError.IsEmpty())
						SetError(OutError, TEXT("artifact_reparse_failed"), TEXT("The staged export file could not be parsed as HJSON."), Relative, Destination);
					if (OutError.TargetPath.IsEmpty()) OutError.TargetPath = Relative;
					return false;
				}
				const auto UsageAfter = Resources.Snapshot();
				TMap<EUeAgentResourceDimension, int64> Charges;
				bool bExactCharges = true;
				for (uint8 D = 0; D <= static_cast<uint8>(EUeAgentResourceDimension::EstimatedMemoryBytes); ++D)
				{
					const auto Dimension = static_cast<EUeAgentResourceDimension>(D);
					const int64 Before = UsageBefore.Consumed(Dimension), After = UsageAfter.Consumed(Dimension);
					if (After == MAX_int64 || After < Before) { bExactCharges = false; break; }
					if (After > Before) Charges.Add(Dimension, After - Before);
				}
				if (bExactCharges)
				{
					if (Relative.Equals(Request.GetPlan()->GetEntryFile(), ESearchCase::CaseSensitive))
					{
						RememberParsedHjsonRootHeader(Hashes.FindChecked(Relative), *Document, Charges);
						RememberParsedSemanticManifest(Paths.Final, Hashes.FindChecked(Relative), *Document, Observed, Charges);
					}
					else if (Relative.StartsWith(TEXT(".uai/inventory/"), ESearchCase::CaseSensitive))
						RememberParsedSemanticManifest(Paths.Final, Hashes.FindChecked(Relative), *Document, Observed, Charges);
					ParseCharges.Add(Relative, MoveTemp(Charges));
				}
				ParseSeconds += FPlatformTime::Seconds() - ParseStarted;
			}
			const double GeneratedFinished = FPlatformTime::Seconds();
			for (const auto& File : Request.GetPreservedFiles())
			{
				const FString Destination = Paths.Staging / File.Key;
				TArray<uint8> Observed;
				if (!WriteAndVerifyStagedFile(Destination, File.Key, File.Value, nullptr,
					TEXT("artifact_preserved_file_write_failed"), TEXT("artifact_preserved_file_write_failed"), Observed, OutError)) return false;
			}
			const double PreservedFinished = FPlatformTime::Seconds();
			if (Request.RequiresVerification()
				&& !Request.GetVerification()->TryVerifyStagedWorkspace(
					Paths.Staging,
					Request.GetPlan()->GetWorkspaceKind(),
					Request.GetPlan()->GetEntryFile(),
					Resources,
					OutError))
			{
				if (OutError.IsEmpty())
				{
					SetError(
						OutError,
						TEXT("artifact_semantic_verification_failed"),
						TEXT("The staged export workspace failed semantic verification."), Paths.Final, Paths.Staging);
				}
				return false;
			}
			const double StageFinished = FPlatformTime::Seconds();
			PreviousArtifactSyntax.Reset();
			if (Request.GetPlan()->GetFiles().Num() <= 200000)
			{
				auto Next = MakeShared<FValidatedArtifactSyntax>();
				Next->ContentHashes = Hashes;
				Next->ParseCharges = MoveTemp(ParseCharges);
				PreviousArtifactSyntax = MoveTemp(Next);
			}
			UE_LOG(LogTemp, Display, TEXT("UAI export timing phase=incremental_syntax reused_files=%d parsed_files=%d"),
				ReusedSyntaxFiles, Request.GetPlan()->GetFiles().Num() - ReusedSyntaxFiles);
			UE_LOG(LogTemp, Display, TEXT("UAI export timing phase=publish_stage files=%d write_readback_s=%.6f parse_s=%.6f generated_s=%.6f preserved_s=%.6f semantic_verify_s=%.6f total_s=%.6f success=true"),
				Request.GetPlan()->GetFiles().Num(), WriteReadbackSeconds, ParseSeconds,
				GeneratedFinished - StageStarted, PreservedFinished - GeneratedFinished,
				StageFinished - PreservedFinished, StageFinished - StageStarted);
			return true;
		}

		bool VerifyInventory(
			const FString& Root,
			const bool bSingleFile,
			const TArray<FString>& ExpectedPaths,
			const TMap<FString, FString>& ExpectedHashes,
			const FString& Target,
			FArtifactPublishError& OutError)
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
			TArray<FString> ObservedFiles;
			IFileManager::Get().FindFilesRecursive(
				ObservedFiles,
				*Root,
				TEXT("*"),
				true,
				false);
			TArray<FString> ObservedPaths;
			TMap<FString, FString> ObservedHashes;
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
				TArray<uint8> Bytes;
				if (!FFileHelper::LoadFileToArray(Bytes, *File))
				{
					SetError(OutError, TEXT("artifact_inventory_read_failed"), TEXT("Could not read an export file while verifying the workspace inventory."), Relative, File);
					return false;
				}
				ObservedPaths.Add(Relative);
				ObservedHashes.Add(Relative, Sha256(Bytes));
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
				ExpectedHashes, Paths.Final, OutError))
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
				const bool bBackupExists = Paths.bSingleFile
					? IFileManager::Get().FileExists(*Paths.Backup)
					: IFileManager::Get().DirectoryExists(*Paths.Backup);
				if (!bBackupExists)
				{
					SetError(OutError, TEXT("artifact_rollback_backup_missing"), TEXT("The previous export backup is missing, so it cannot be restored."), Paths.Final, Paths.Backup);
					OutError.ActualValuePresent = false;
					OutError.ExpectedValue = MakeShared<FJsonValueString>(TEXT("the previous export backup"));
					return false;
				}
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
			CleanupStaging(Paths, true, OutError);
		}
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
		bSemanticWorkspaceVerified = Request.RequiresVerification();
		bTargetVersionRechecked = Request.RequiresVerification();
		SemanticVerificationFields = Request.RequiresVerification()
			? Request.GetVerification()->GetSemanticVerificationFields()
			: TMap<FString, FString>();
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
		return TryPublishExportPlan(FArtifactPublishRequest(Plan, SourceRoot, Owner, MoveTemp(Preserved)),
			Resources, OutReceipt, OutError);
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
		if (!BuildPaths(Request, Transaction, OutError)
			|| !ExistingFinalIsOwned(
				Transaction,
				Request.GetPublisherOwnerId(),
				OutError))
		{
			return false;
		}
		IFileManager& Files = IFileManager::Get();
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
		bool bOldMoved = false;
		bool bNewPublished = false;
		TArray<FArtifactPublishError> PendingWarnings;
		const double StageStarted = FPlatformTime::Seconds();
		if (!StageAndReparse(
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

		const bool bFinalExists = Transaction.bSingleFile
			? Files.FileExists(*Transaction.Final)
			: Files.DirectoryExists(*Transaction.Final);
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
			FArtifactPublishError JournalWarning;
			if (!SaveJournal(
				Transaction,
				Request,
				TEXT("old_final_backed_up"), JournalWarning))
			{
				JournalWarning.Severity = UE::UAI::Contract::ESeverity::Warning;
				PendingWarnings.Add(MoveTemp(JournalWarning));
			}
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
		if (!VerifyFinal(
			Transaction,
			Request,
			Paths,
			Hashes,
			Inventory, OutError))
		{
			FinishFailedPublish(Transaction, bOldMoved, bNewPublished, TEXT("artifact_publish_verification_failed"), OutError, PendingWarnings, OutError);
			return false;
		}

		const double FinalVerified = FPlatformTime::Seconds();
		OutReceipt.SetPublished(
			Request, Transaction.Final, Transaction.TransactionId,
			MoveTemp(Paths), MoveTemp(Hashes), Inventory);
		OutReceipt.CleanupWarnings.Append(PendingWarnings);
		FArtifactPublishError CleanupWarning;
		if (!SaveJournal(Transaction, Request, TEXT("published_verified"), CleanupWarning))
		{
			CleanupWarning.Severity = UE::UAI::Contract::ESeverity::Warning;
			OutReceipt.CleanupWarnings.Add(CleanupWarning);
		}
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
		// Keep the journal while backup cleanup is pending, so recovery retains ownership.
		if (bCleanupComplete && !DeleteFile(Transaction.Journal, Transaction.Final, TEXT("transaction journal"), CleanupWarning))
		{
			CleanupWarning.Severity = UE::UAI::Contract::ESeverity::Warning;
			OutReceipt.CleanupWarnings.Add(CleanupWarning);
		}
		const double PublishFinished = FPlatformTime::Seconds();
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
