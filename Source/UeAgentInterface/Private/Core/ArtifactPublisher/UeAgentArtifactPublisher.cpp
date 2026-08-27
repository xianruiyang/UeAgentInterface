#include "Core/ArtifactPublisher/UeAgentArtifactPublisher.h"

#include "Core/ArtifactPublisher/UeAgentArtifactPublisherTestHooks.h"
#include "Dom/JsonObject.h"
#include "HAL/CriticalSection.h"
#include "HAL/FileManager.h"
#include "HAL/PlatformMisc.h"
#include "HAL/PlatformFileManager.h"
#include "HAL/PlatformProcess.h"
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

		void SetError(
			FArtifactPublishError& OutError,
			FString Code,
			FString Path = FString(),
			FString Detail = FString(),
			int64 Limit = 0,
			int64 Observed = 0)
		{
			OutError.Set(
				MoveTemp(Code),
				MoveTemp(Path),
				MoveTemp(Detail),
				Limit,
				Observed);
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

		bool IsSafeRelativePath(const FString& Input, FString& OutNormalized)
		{
			OutNormalized = Input;
			OutNormalized.ReplaceInline(TEXT("\\"), TEXT("/"));
			if (Input.Contains(TEXT("\\"))
				|| OutNormalized.IsEmpty()
				|| FPaths::IsRelative(OutNormalized) == false
				|| OutNormalized.StartsWith(TEXT("/"))
				|| OutNormalized.EndsWith(TEXT("/")))
			{
				return false;
			}
			TArray<FString> Parts;
			OutNormalized.ParseIntoArray(Parts, TEXT("/"), false);
			if (Parts.IsEmpty())
			{
				return false;
			}
			for (const FString& Part : Parts)
			{
				if (Part.IsEmpty() || Part == TEXT(".") || Part == TEXT(".."))
				{
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
					OutPaths.Final);
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
			const TSharedRef<FJsonObject>& Object)
		{
			FString Text;
			return SerializeJson(Object, Text)
				&& FFileHelper::SaveStringToFile(
					Text + TEXT("\n"),
					*Path,
					FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM);
		}

		bool LoadJson(
			const FString& Path,
			TSharedPtr<FJsonObject>& OutObject)
		{
			FString Text;
			if (!FFileHelper::LoadFileToString(Text, *Path))
			{
				return false;
			}
			const TSharedRef<TJsonReader<>> Reader =
				TJsonReaderFactory<>::Create(Text);
			return FJsonSerializer::Deserialize(Reader, OutObject)
				&& OutObject.IsValid();
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
						Paths.Final,
						TEXT("final kind or adjacent ownership is inconsistent"));
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
			FString Format;
			FString MarkerOwner;
			FString MarkerFinal;
			if (!LoadJson(Paths.FinalOwner, Marker)
				|| !Marker->TryGetStringField(TEXT("format"), Format)
				|| Format != FormatId
				|| !Marker->TryGetStringField(
					TEXT("publisher_owner_id"), MarkerOwner)
				|| MarkerOwner != OwnerId
				|| !Marker->TryGetStringField(
					TEXT("final_path"), MarkerFinal)
				|| !NormalizeFinalPath(MarkerFinal).Equals(
					Paths.Final,
					ESearchCase::IgnoreCase))
			{
				SetError(
					OutError,
					TEXT("artifact_final_ownership_required"),
					Paths.Final,
					TEXT("existing final lacks matching C8F ownership"));
				return false;
			}
			IPlatformFile& PlatformFile =
				FPlatformFileManager::Get().GetPlatformFile();
			if (Paths.bSingleFile)
			{
				if (PlatformFile.IsReadOnly(*Paths.Final)
					|| PlatformFile.IsReadOnly(*Paths.FinalOwner))
				{
					SetError(
						OutError,
						TEXT("artifact_final_read_only"),
						Paths.Final,
						TEXT("owned final or ownership marker is read-only"));
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
							ExistingFile,
							TEXT("owned workspace contains a read-only file"));
						return false;
					}
				}
			}
			return true;
		}

		bool DeleteDirectory(const FString& Path)
		{
			return !IFileManager::Get().DirectoryExists(*Path)
				|| IFileManager::Get().DeleteDirectory(*Path, false, true);
		}

		bool DeleteFile(const FString& Path)
		{
			return !IFileManager::Get().FileExists(*Path)
				|| IFileManager::Get().Delete(*Path, false, true, true);
		}

		bool DeleteDataPath(const FPublishPaths& Paths, const FString& Path)
		{
			return Paths.bSingleFile
				? DeleteFile(Path)
				: DeleteDirectory(Path);
		}

		bool MovePath(
			const FString& Destination,
			const FString& Source,
			uint32* OutLastPlatformError = nullptr)
		{
			// A publish commit is a same-parent rename. Keep every attempt atomic;
			// do not fall back to copying a partially visible directory tree.
			if (!FPaths::GetPath(Destination).Equals(
					FPaths::GetPath(Source), ESearchCase::IgnoreCase))
			{
				if (OutLastPlatformError)
				{
					*OutLastPlatformError = 0;
				}
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
					if (OutLastPlatformError)
					{
						*OutLastPlatformError = 0;
					}
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
					if (OutLastPlatformError)
					{
						*OutLastPlatformError = LastPlatformError;
					}
					return false;
				}
				FPlatformProcess::SleepNoStats(
					Attempt == 0 ? 0.01f : 0.05f);
			}
			if (OutLastPlatformError)
			{
				*OutLastPlatformError = LastPlatformError;
			}
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
					Request.GetFinalPath(),
					TEXT("inline workspaces do not have a filesystem final"));
				return false;
			}
			if (Plan->GetFiles().IsEmpty())
			{
				SetError(OutError, TEXT("artifact_plan_invalid"));
				return false;
			}
			if (Plan->GetWorkspaceKind() == EWorkspaceKind::SingleFile
				&& Plan->GetFiles().Num() != 1)
			{
				SetError(
					OutError,
					TEXT("artifact_plan_invalid"),
					Plan->GetEntryFile(),
					TEXT("single-file plan must contain exactly one file"));
				return false;
			}

			int64 OutputBytes = 0;
			TSet<FString> Folded;
			for (const FExportFilePlan& File : Plan->GetFiles())
			{
				FString Path;
				if (!IsSafeRelativePath(File.GetRelativePath(), Path)
					|| Path.Equals(OwnerMarkerName, ESearchCase::IgnoreCase))
				{
					SetError(
						OutError,
						TEXT("artifact_path_invalid"),
						File.GetRelativePath());
					return false;
				}
				const FString Key = Path.ToLower();
				if (Folded.Contains(Key))
				{
					SetError(
						OutError,
						TEXT("artifact_path_collision"),
						Path);
					return false;
				}
				Folded.Add(Key);
				if (Sha256(File.GetCanonicalUtf8()) != File.GetContentHash())
				{
					SetError(
						OutError,
						TEXT("artifact_content_hash_mismatch"),
						Path);
					return false;
				}
				OutputBytes += File.GetCanonicalUtf8().Num();
				OutPaths.Add(Path);
				OutHashes.Add(Path, File.GetContentHash());
			}
			FString Entry;
			if (!IsSafeRelativePath(Plan->GetEntryFile(), Entry)
				|| !Folded.Contains(Entry.ToLower()))
			{
				SetError(
					OutError,
					TEXT("artifact_entry_missing"),
					Plan->GetEntryFile());
				return false;
			}
			OutPaths.Sort();

			FUeAgentResourceBudgetExceeded BudgetError;
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
				BudgetError))
			{
				SetError(
					OutError,
					BudgetError.Code,
					BudgetError.Path,
					LexToString(BudgetError.Dimension),
					BudgetError.Limit,
					BudgetError.Observed);
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
			TArray<TSharedPtr<FJsonValue>> FileRows;
			for (const FString& Path : Paths)
			{
				const TSharedRef<FJsonObject> Row = MakeShared<FJsonObject>();
				Row->SetStringField(TEXT("relative_path"), Path);
				Row->SetStringField(TEXT("content_hash"), Hashes.FindChecked(Path));
				FileRows.Add(MakeShared<FJsonValueObject>(Row));
			}
			Record->SetArrayField(TEXT("files"), FileRows);
			return Record;
		}

		bool SaveJournal(
			const FPublishPaths& Paths,
			const FArtifactPublishRequest& Request,
			const FString& State)
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
			return SaveJson(Paths.Journal, Record);
		}

		bool StageAndReparse(
			const FArtifactPublishRequest& Request,
			const FPublishPaths& Paths,
			FUeAgentResourceTracker& Resources,
			const TMap<FString, FString>& Hashes,
			FArtifactPublishError& OutError)
		{
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
					Paths.Staging);
				return false;
			}
			for (const FExportFilePlan& File : Request.GetPlan()->GetFiles())
			{
				FString Relative;
				IsSafeRelativePath(File.GetRelativePath(), Relative);
				const FString Destination = Paths.bSingleFile
					? Paths.Staging
					: Paths.Staging / Relative;
				if (!Files.MakeDirectory(*FPaths::GetPath(Destination), true)
					|| !FFileHelper::SaveArrayToFile(
						File.GetCanonicalUtf8(),
						*Destination))
				{
					SetError(
						OutError,
						TEXT("artifact_staging_write_failed"),
						Relative);
					return false;
				}
				TArray<uint8> Observed;
				if (!FFileHelper::LoadFileToArray(Observed, *Destination)
					|| Observed != File.GetCanonicalUtf8()
					|| Sha256(Observed) != Hashes.FindChecked(Relative))
				{
					SetError(
						OutError,
						TEXT("artifact_staging_verification_failed"),
						Relative);
					return false;
				}
				Observed.Add(0);
				const FString Text = UTF8_TO_TCHAR(
					reinterpret_cast<const char*>(Observed.GetData()));
				Observed.Pop(EAllowShrinking::No);
				FHjsonDocumentPtr Document;
				FWorkspaceError ParseError;
				if (!TryParseHjsonDocument(
					Text,
					Relative,
					Resources,
					Document,
					ParseError))
				{
					SetError(
						OutError,
						TEXT("artifact_reparse_failed"),
						Relative,
						ParseError.GetCode());
					return false;
				}
			}
			if (Request.RequiresVerification()
				&& !Request.GetVerification()->TryVerifyStagedWorkspace(
					Paths.Staging,
					Request.GetPlan()->GetWorkspaceKind(),
					Request.GetPlan()->GetEntryFile(),
					Resources,
					OutError))
			{
				if (OutError.GetCode().IsEmpty())
				{
					SetError(
						OutError,
						TEXT("artifact_semantic_verification_failed"),
						Paths.Staging);
				}
				return false;
			}
			return true;
		}

		bool VerifyInventory(
			const FString& Root,
			const bool bSingleFile,
			const TArray<FString>& ExpectedPaths,
			const TMap<FString, FString>& ExpectedHashes)
		{
			if (bSingleFile)
			{
				if (ExpectedPaths.Num() != 1)
				{
					return false;
				}
				TArray<uint8> Bytes;
				return FFileHelper::LoadFileToArray(Bytes, *Root)
					&& Sha256(Bytes)
						== ExpectedHashes.FindChecked(ExpectedPaths[0]);
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
					return false;
				}
				ObservedPaths.Add(Relative);
				ObservedHashes.Add(Relative, Sha256(Bytes));
			}
			ObservedPaths.Sort();
			return ObservedPaths == ExpectedPaths
				&& ObservedHashes.OrderIndependentCompareEqual(ExpectedHashes);
		}

		bool VerifyFinal(
			const FPublishPaths& Paths,
			const FArtifactPublishRequest& Request,
			const TArray<FString>& ExpectedPaths,
			const TMap<FString, FString>& ExpectedHashes,
			const FString& Inventory)
		{
			if (!VerifyInventory(
				Paths.Final,
				Paths.bSingleFile,
				ExpectedPaths,
				ExpectedHashes))
			{
				return false;
			}
			TSharedPtr<FJsonObject> Marker;
			FString Owner;
			FString MarkerInventory;
			return LoadJson(Paths.FinalOwner, Marker)
				&& Marker->TryGetStringField(
					TEXT("publisher_owner_id"), Owner)
				&& Owner == Request.GetPublisherOwnerId()
				&& Marker->TryGetStringField(
					TEXT("inventory_hash"), MarkerInventory)
				&& MarkerInventory == Inventory;
		}

		bool RestoreOldFinal(
			const FPublishPaths& Paths,
			const bool bOldMoved,
			const bool bNewPublished)
		{
			const bool bNewExists = Paths.bSingleFile
				? IFileManager::Get().FileExists(*Paths.Final)
				: IFileManager::Get().DirectoryExists(*Paths.Final);
			if (bNewPublished && bNewExists
				&& !(Paths.bSingleFile
					? DeleteFile(Paths.Final)
					: DeleteDirectory(Paths.Final)))
			{
				return false;
			}
			if (bNewPublished
				&& Paths.bSingleFile
				&& !DeleteFile(Paths.FinalOwner))
			{
				return false;
			}
			if (bOldMoved)
			{
				const bool bBackupExists = Paths.bSingleFile
					? IFileManager::Get().FileExists(*Paths.Backup)
					: IFileManager::Get().DirectoryExists(*Paths.Backup);
				if (!bBackupExists
					|| !MovePath(Paths.Final, Paths.Backup))
				{
					return false;
				}
				if (Paths.bSingleFile
					&& IFileManager::Get().FileExists(*Paths.BackupOwner)
					&& !MovePath(Paths.FinalOwner, Paths.BackupOwner))
				{
					return false;
				}
			}
			return !bOldMoved
				|| (Paths.bSingleFile
					? IFileManager::Get().FileExists(*Paths.Final)
					: IFileManager::Get().DirectoryExists(*Paths.Final));
		}
	}

	FArtifactPublishRequest::FArtifactPublishRequest(
		FExportWorkspacePlanRef InPlan,
		FString InFinalPath,
		FString InPublisherOwnerId)
		: Plan(MoveTemp(InPlan))
		, FinalPath(MoveTemp(InFinalPath))
		, PublisherOwnerId(MoveTemp(InPublisherOwnerId))
	{
		FinalPath.TrimStartAndEndInline();
		PublisherOwnerId.TrimStartAndEndInline();
		bValid = !FinalPath.IsEmpty() && !PublisherOwnerId.IsEmpty();
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
		bValid = bValid && Verification.IsValid();
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
		return bValid;
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

	const FString& FArtifactPublishError::GetCode() const
	{
		return Code;
	}

	const FString& FArtifactPublishError::GetPath() const
	{
		return Path;
	}

	const FString& FArtifactPublishError::GetDetail() const
	{
		return Detail;
	}

	int64 FArtifactPublishError::GetResourceLimit() const
	{
		return ResourceLimit;
	}

	int64 FArtifactPublishError::GetResourceObserved() const
	{
		return ResourceObserved;
	}

	void FArtifactPublishError::Set(
		FString InCode,
		FString InPath,
		FString InDetail,
		int64 InResourceLimit,
		int64 InResourceObserved)
	{
		Code = MoveTemp(InCode);
		Path = MoveTemp(InPath);
		Detail = MoveTemp(InDetail);
		ResourceLimit = InResourceLimit;
		ResourceObserved = InResourceObserved;
	}

	bool TryPublishExportPlan(
		const FArtifactPublishRequest& Request,
		FUeAgentResourceTracker& Resources,
		FArtifactPublishReceipt& OutReceipt,
		FArtifactPublishError& OutError)
	{
		OutReceipt = {};
		OutError = {};
		if (!Request.IsValid())
		{
			SetError(
				OutError,
				TEXT("artifact_request_invalid"),
				Request.GetFinalPath());
			return false;
		}

		TArray<FString> Paths;
		TMap<FString, FString> Hashes;
		if (!ValidatePlan(Request, Resources, Paths, Hashes, OutError))
		{
			return false;
		}

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
		if (bStagingExists
			|| bBackupExists
			|| Files.FileExists(*Transaction.Journal)
			|| Files.FileExists(*Transaction.StagingOwner)
			|| Files.FileExists(*Transaction.BackupOwner))
		{
			SetError(
				OutError,
				TEXT("artifact_transaction_path_conflict"),
				Transaction.Parent);
			return false;
		}
		if (!Files.MakeDirectory(*Transaction.Parent, true))
		{
			SetError(
				OutError,
				TEXT("artifact_parent_create_failed"),
				Transaction.Parent);
			return false;
		}

		const FString Inventory = InventoryHash(Paths, Hashes);
		bool bOldMoved = false;
		bool bNewPublished = false;
		if (!StageAndReparse(
			Request,
			Transaction,
			Resources,
			Hashes,
			OutError))
		{
			DeleteDataPath(Transaction, Transaction.Staging);
			DeleteFile(Transaction.StagingOwner);
			return false;
		}
		if (!SaveJson(
				Transaction.StagingOwner,
				BuildOwnerRecord(
					Request,
					Transaction.Final,
					Paths,
					Hashes,
					Inventory))
			|| !VerifyInventory(
				Transaction.Staging,
				Transaction.bSingleFile,
				Paths,
				Hashes)
			|| !SaveJournal(
				Transaction,
				Request,
				TEXT("staged")))
		{
			DeleteDataPath(Transaction, Transaction.Staging);
			DeleteFile(Transaction.StagingOwner);
			DeleteFile(Transaction.Journal);
			SetError(
				OutError,
				TEXT("artifact_staging_verification_failed"),
				Transaction.Staging);
			return false;
		}

		if (Request.RequiresVerification()
			&& !Request.GetVerification()->TryRecheckTargetVersion(OutError))
		{
			DeleteDataPath(Transaction, Transaction.Staging);
			DeleteFile(Transaction.StagingOwner);
			DeleteFile(Transaction.Journal);
			if (OutError.GetCode().IsEmpty())
			{
				SetError(
					OutError,
					TEXT("artifact_target_version_recheck_failed"),
					Transaction.Final);
			}
			return false;
		}

		const bool bFinalExists = Transaction.bSingleFile
			? Files.FileExists(*Transaction.Final)
			: Files.DirectoryExists(*Transaction.Final);
		if (bFinalExists)
		{
			if (!MovePath(Transaction.Backup, Transaction.Final))
			{
				DeleteDataPath(Transaction, Transaction.Staging);
				DeleteFile(Transaction.StagingOwner);
				DeleteFile(Transaction.Journal);
				SetError(
					OutError,
					TEXT("artifact_publish_failed"),
					Transaction.Final,
					TEXT("old final could not be moved to backup"));
				return false;
			}
			bOldMoved = true;
			if (Transaction.bSingleFile
				&& !MovePath(
					Transaction.BackupOwner,
					Transaction.FinalOwner))
			{
				const bool bRestored = RestoreOldFinal(
					Transaction,
					bOldMoved,
					bNewPublished);
				DeleteDataPath(Transaction, Transaction.Staging);
				DeleteFile(Transaction.StagingOwner);
				DeleteFile(Transaction.Journal);
				SetError(
					OutError,
					bRestored
						? TEXT("artifact_publish_failed")
						: TEXT("artifact_publish_state_unknown"),
					Transaction.Final,
					TEXT("old owner metadata could not be moved to backup"));
				return false;
			}
			SaveJournal(
				Transaction,
				Request,
				TEXT("old_final_backed_up"));
			if (ShouldFailAfterFinalMovedToBackup())
			{
				const bool bRestored = RestoreOldFinal(
					Transaction,
					bOldMoved,
					bNewPublished);
				DeleteDataPath(Transaction, Transaction.Staging);
				DeleteFile(Transaction.StagingOwner);
				DeleteFile(Transaction.Journal);
				SetError(
					OutError,
					bRestored
						? TEXT("artifact_publish_failed")
						: TEXT("artifact_publish_state_unknown"),
					Transaction.Final,
					TEXT("injected failure after final backup"));
				return false;
			}
		}

		uint32 CommitPlatformError = 0;
		if (!MovePath(
			Transaction.Final,
			Transaction.Staging,
			&CommitPlatformError))
		{
			const bool bRestored = RestoreOldFinal(
				Transaction,
				bOldMoved,
				bNewPublished);
			DeleteDataPath(Transaction, Transaction.Staging);
			DeleteFile(Transaction.StagingOwner);
			DeleteFile(Transaction.Journal);
			SetError(
				OutError,
				bRestored
					? TEXT("artifact_publish_failed")
					: TEXT("artifact_publish_state_unknown"),
				Transaction.Final,
				FString::Printf(
					TEXT("staging could not become final; platform_error=%u"),
					CommitPlatformError));
			return false;
		}
		bNewPublished = true;
		if (Transaction.bSingleFile
			&& !MovePath(
				Transaction.FinalOwner,
				Transaction.StagingOwner))
		{
			const bool bRestored = RestoreOldFinal(
				Transaction,
				bOldMoved,
				bNewPublished);
			DeleteDataPath(Transaction, Transaction.Staging);
			DeleteFile(Transaction.StagingOwner);
			DeleteFile(Transaction.Journal);
			SetError(
				OutError,
				bRestored
					? TEXT("artifact_publish_failed")
					: TEXT("artifact_publish_state_unknown"),
				Transaction.Final,
				TEXT("staged owner metadata could not become final"));
			return false;
		}

		if (!VerifyFinal(
			Transaction,
			Request,
			Paths,
			Hashes,
			Inventory))
		{
			const bool bRestored = RestoreOldFinal(
				Transaction,
				bOldMoved,
				bNewPublished);
			DeleteDataPath(Transaction, Transaction.Staging);
			DeleteFile(Transaction.StagingOwner);
			DeleteFile(Transaction.Journal);
			SetError(
				OutError,
				bRestored
					? TEXT("artifact_publish_verification_failed")
					: TEXT("artifact_publish_state_unknown"),
				Transaction.Final);
			return false;
		}

		if (bOldMoved
			&& (!DeleteDataPath(Transaction, Transaction.Backup)
				|| (Transaction.bSingleFile
					&& !DeleteFile(Transaction.BackupOwner))))
		{
			SetError(
				OutError,
				TEXT("artifact_publish_cleanup_failed"),
				Transaction.Backup);
			return false;
		}
		if (!DeleteFile(Transaction.Journal))
		{
			SetError(
				OutError,
				TEXT("artifact_publish_cleanup_failed"),
				Transaction.Journal);
			return false;
		}

		OutReceipt.SetPublished(
			Request,
			Transaction.Final,
			Transaction.TransactionId,
			MoveTemp(Paths),
			MoveTemp(Hashes),
			Inventory);
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
