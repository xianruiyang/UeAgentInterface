#include "Core/ArtifactPublisher/UeAgentFileArtifactPublisher.h"
#include "Utilities/UeAgentFileOperationDiagnostic.h"

#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "HAL/FileManager.h"
#include "Misc/FileHelper.h"
#include "Misc/Guid.h"
#include "Misc/Paths.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"
#include "Serialization/JsonWriter.h"

#define UI OpenSslUi
#include <openssl/sha.h>
#undef UI

namespace UE::UAI::ArtifactPublisher
{
	using namespace ContextResource;
	using namespace StateSemantic;

	namespace
	{
		const TCHAR* FileArtifactFormat = TEXT("uai.c8f.file_artifact.v1");

		FString NormalizeText(FString Value)
		{
			Value.TrimStartAndEndInline();
			return Value;
		}

		FString NormalizePath(const FString& Value)
		{
			FString Result = FPaths::ConvertRelativePathToFull(Value);
			FPaths::NormalizeFilename(Result);
			FPaths::CollapseRelativeDirectories(Result);
			return Result;
		}

		FString NormalizeLower(FString Value)
		{
			Value = NormalizeText(MoveTemp(Value));
			Value.ToLowerInline();
			return Value;
		}

		bool IsToken(const FString& Value)
		{
			if (Value.IsEmpty())
			{
				return false;
			}
			for (const TCHAR Character : Value)
			{
				if (!(FChar::IsAlnum(Character)
					|| Character == TEXT('_')
					|| Character == TEXT('-')
					|| Character == TEXT('.')
					|| Character == TEXT('+')))
				{
					return false;
				}
			}
			return true;
		}

		FFileArtifactPublishError Failure(
			const TCHAR* Code, const FString& Reason,
			const FString& PropertyPath = FString(), const FString& InternalPath = FString())
		{
			auto Error = FFileArtifactPublishError::Failure(Code, Reason, TEXT("artifact_publish"), InternalPath);
			Error.PropertyPath = PropertyPath;
			return Error;
		}

		FFileArtifactPublishError FileFailure(
			const TCHAR* Code, const FString& Reason, const FString& InternalPath)
		{
			return FileDiagnostics::MakeFileOperationFailure(Code, Reason, TEXT("artifact_publish"), InternalPath);
		}

		void AppendFailure(FFileArtifactPublishError& OutError, FFileArtifactPublishError Error)
		{
			if (OutError.Code.IsEmpty()) OutError = MoveTemp(Error);
			else OutError.Append(MoveTemp(Error));
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

		FString Sha256Text(const TArray<FString>& Values)
		{
			TArray<uint8> Bytes;
			for (const FString& Value : Values)
			{
				const FTCHARToUTF8 Utf8(*Value);
				Bytes.Append(
					reinterpret_cast<const uint8*>(Utf8.Get()), Utf8.Length());
				Bytes.Add(0);
			}
			return Sha256(Bytes);
		}

		bool SerializeJson(
			const TSharedRef<FJsonObject>& Object,
			FString& OutText)
		{
			const TSharedRef<TJsonWriter<>> Writer =
				TJsonWriterFactory<>::Create(&OutText);
			return FJsonSerializer::Serialize(Object, Writer);
		}

		bool SaveOwnerRecord(
			const FString& Path,
			const FFileArtifactReference& Reference,
			FFileArtifactPublishError& OutError)
		{
			const TSharedRef<FJsonObject> Object = MakeShared<FJsonObject>();
			Object->SetStringField(TEXT("format"), FileArtifactFormat);
			Object->SetStringField(
				TEXT("artifact_id"), Reference.GetArtifactId());
			Object->SetStringField(
				TEXT("publisher_owner_id"), Reference.GetPublisherOwnerId());
			Object->SetStringField(TEXT("locator"), Reference.GetLocator());
			Object->SetStringField(
				TEXT("content_hash"), Reference.GetContentHash());
			Object->SetNumberField(
				TEXT("byte_count"), static_cast<double>(Reference.GetByteCount()));
			FString Text;
			if (!SerializeJson(Object, Text))
			{
				OutError = Failure(TEXT("file_artifact_staging_write_failed"),
					TEXT("The artifact ownership record could not be encoded as JSON."), TEXT("ownership_marker"), Path);
				return false;
			}
			if (!FFileHelper::SaveStringToFile(Text + TEXT("\n"), *Path, FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM))
			{
				OutError = FileFailure(TEXT("file_artifact_staging_write_failed"),
					TEXT("The staged artifact ownership record could not be written."), Path);
				return false;
			}
			return true;
		}

		bool ExistingFinalIsOwned(
			const FString& Final,
			const FString& Marker,
			const FString& OwnerId,
			FFileArtifactPublishError& OutError)
		{
			IFileManager& Files = IFileManager::Get();
			const bool bFinalExists = Files.FileExists(*Final);
			const bool bMarkerExists = Files.FileExists(*Marker);
			if (!bFinalExists && !bMarkerExists)
			{
				return true;
			}
			if (!bFinalExists || !bMarkerExists)
			{
				OutError = Failure(TEXT("file_artifact_final_ownership_required"),
					bFinalExists ? TEXT("An existing artifact has no ownership record; it cannot be replaced.")
						: TEXT("An artifact ownership record exists but its artifact file is missing."), TEXT("ownership_marker"), Final);
				auto Presence = MakeShared<FJsonObject>();
				Presence->SetBoolField(TEXT("artifact_exists"), bFinalExists);
				Presence->SetBoolField(TEXT("ownership_record_exists"), bMarkerExists);
				OutError.ActualValue = MakeShared<FJsonValueObject>(Presence);
				return false;
			}
			FString Text;
			TSharedPtr<FJsonObject> Object;
			if (!FFileHelper::LoadFileToString(Text, *Marker))
			{
				OutError = FileFailure(TEXT("file_artifact_final_ownership_required"),
					TEXT("The existing artifact ownership record could not be read."), Marker);
				return false;
			}
			const auto Reader = TJsonReaderFactory<>::Create(Text);
			if (!FJsonSerializer::Deserialize(Reader, Object) || !Object.IsValid())
			{
				OutError = Failure(TEXT("file_artifact_final_ownership_required"),
					TEXT("The existing artifact ownership record is not a valid JSON object."), TEXT("ownership_marker"), Marker);
				OutError.ActualEvidence = MakeShared<FJsonValueString>(Reader->GetErrorMessage());
				return false;
			}
			FString Format;
			FString MarkerOwner;
			FString MarkerFinal;
			auto ReadField = [&](const TCHAR* Field, FString& Value)
			{
				if (Object->TryGetStringField(Field, Value)) return true;
				const auto* Actual = Object->Values.Find(Field);
				const FString Reason = Actual
					? FString::Printf(TEXT("The artifact ownership field '%s' is not a string."), Field)
					: FString::Printf(TEXT("The artifact ownership record is missing the required field '%s'."), Field);
				OutError = Failure(TEXT("file_artifact_final_ownership_required"),
					Reason,
					FString(TEXT("ownership_marker.")) + Field, Marker);
				if (Actual) OutError.ActualEvidence = *Actual;
				OutError.ExpectedValue = MakeShared<FJsonValueString>(TEXT("string"));
				return false;
			};
			auto Matches = [&](const TCHAR* Field, const FString& Actual, const FString& Expected, const TCHAR* Reason)
			{
				if (Actual == Expected) return true;
				OutError = Failure(TEXT("file_artifact_final_ownership_required"), Reason,
					FString(TEXT("ownership_marker.")) + Field, Marker);
				OutError.ActualEvidence = MakeShared<FJsonValueString>(Actual);
				OutError.ExpectedEvidence = MakeShared<FJsonValueString>(Expected);
				return false;
			};
			if (!ReadField(TEXT("format"), Format)
				|| !Matches(TEXT("format"), Format, FileArtifactFormat, TEXT("The existing artifact ownership record uses an unsupported format."))
				|| !ReadField(TEXT("publisher_owner_id"), MarkerOwner)
				|| !Matches(TEXT("publisher_owner_id"), MarkerOwner, OwnerId, TEXT("The existing artifact belongs to a different publisher."))
				|| !ReadField(TEXT("locator"), MarkerFinal)
				|| !Matches(TEXT("locator"), NormalizePath(MarkerFinal), Final, TEXT("The ownership record refers to a different artifact location."))) return false;
			return true;
		}

		bool MoveFile(const FString& Destination, const FString& Source)
		{
			return IFileManager::Get().Move(
				*Destination, *Source, false, false, false, true);
		}

		bool DeleteFile(const FString& Path)
		{
			return !IFileManager::Get().FileExists(*Path)
				|| IFileManager::Get().Delete(*Path, false, true, true);
		}

		bool CleanupFile(const FString& Path, const TCHAR* Reason, FFileArtifactPublishError& OutError)
		{
			if (DeleteFile(Path)) return true;
			AppendFailure(OutError, FileFailure(TEXT("file_artifact_cleanup_failed"), Reason, Path));
			return false;
		}

		bool RestoreFile(const FString& Destination, const FString& Backup, const TCHAR* Reason, FFileArtifactPublishError& OutError)
		{
			if (MoveFile(Destination, Backup)) return true;
			auto Error = FileFailure(TEXT("file_artifact_restore_failed"), Reason, Backup);
			Error.ExpectedEvidence = MakeShared<FJsonValueString>(Destination);
			AppendFailure(OutError, MoveTemp(Error));
			return false;
		}

		bool VerifyBytes(const TArray<uint8>& Actual, const TArray<uint8>& Expected, const FString& ExpectedHash,
			const TCHAR* Code, const TCHAR* Reason, const FString& Path, FFileArtifactPublishError& OutError)
		{
			const FString ActualHash = Sha256(Actual);
			if (Actual == Expected && ActualHash == ExpectedHash) return true;
			OutError = Failure(Code, Reason, TEXT("content"), Path);
			auto ActualInfo = MakeShared<FJsonObject>();
			auto ExpectedInfo = MakeShared<FJsonObject>();
			ActualInfo->SetNumberField(TEXT("byte_count"), Actual.Num());
			ExpectedInfo->SetNumberField(TEXT("byte_count"), Expected.Num());
			OutError.ActualValue = MakeShared<FJsonValueObject>(ActualInfo);
			OutError.ExpectedValue = MakeShared<FJsonValueObject>(ExpectedInfo);
			OutError.ActualEvidence = MakeShared<FJsonValueString>(ActualHash);
			OutError.ExpectedEvidence = MakeShared<FJsonValueString>(ExpectedHash);
			for (int32 Index = 0; Index < FMath::Min(Actual.Num(), Expected.Num()); ++Index)
			{
				if (Actual[Index] != Expected[Index])
				{
					OutError.PropertyPath = FString::Printf(TEXT("content[%d]"), Index);
					break;
				}
			}
			return false;
		}

		bool MakeStringValue(
			const FString& Value,
			FUeAgentResourceTracker& Resources,
			FUeAgentCanonicalValuePtr& OutValue,
			FFileArtifactPublishError& OutError)
		{
			FUeAgentStateSemanticError Error;
			if (!FUeAgentCanonicalValue::TryString(
				Value, Resources, OutValue, Error))
			{
				OutError = MoveTemp(Error);
				return false;
			}
			return true;
		}

		bool MakeIntegerValue(
			const int64 Value,
			FUeAgentResourceTracker& Resources,
			FUeAgentCanonicalValuePtr& OutValue,
			FFileArtifactPublishError& OutError)
		{
			FUeAgentStateSemanticError Error;
			if (!FUeAgentCanonicalValue::TryInteger(
				FString::Printf(TEXT("%lld"), Value),
				Resources,
				OutValue,
				Error))
			{
				OutError = MoveTemp(Error);
				return false;
			}
			return true;
		}
	}

	FFileArtifactPublishRequest::FFileArtifactPublishRequest(
		FString InSourcePath,
		FString InFinalPath,
		FString InPublisherOwnerId,
		FString InArtifactKind,
		FString InFormat,
		FString InMimeType)
		: SourcePath(NormalizePath(InSourcePath))
		, FinalPath(NormalizePath(InFinalPath))
		, PublisherOwnerId(NormalizeText(MoveTemp(InPublisherOwnerId)))
		, ArtifactKind(NormalizeLower(MoveTemp(InArtifactKind)))
		, Format(NormalizeLower(MoveTemp(InFormat)))
		, MimeType(NormalizeLower(MoveTemp(InMimeType)))
	{
		auto Reject = [this](const TCHAR* Reason, const TCHAR* Path, const FString& Actual, const bool bPublic)
		{
			auto Error = Failure(TEXT("file_artifact_request_invalid"), Reason, Path);
			if (bPublic) Error.ActualValue = MakeShared<FJsonValueString>(Actual);
			else Error.ActualEvidence = MakeShared<FJsonValueString>(Actual);
			ValidationDiagnostic = MakeShared<FFileArtifactPublishError>(MoveTemp(Error));
		};
		if (SourcePath.IsEmpty()) Reject(TEXT("The artifact publication request has no source file path."), TEXT("source_path"), SourcePath, false);
		else if (FinalPath.IsEmpty()) Reject(TEXT("The artifact publication request has no destination file path."), TEXT("final_path"), FinalPath, false);
		else if (SourcePath.Equals(FinalPath, ESearchCase::IgnoreCase)) Reject(TEXT("The artifact source and destination must be different files."), TEXT("final_path"), FinalPath, false);
		else if (PublisherOwnerId.IsEmpty()) Reject(TEXT("The artifact publication request has no publisher owner."), TEXT("publisher_owner_id"), PublisherOwnerId, false);
		else if (!IsToken(ArtifactKind)) Reject(TEXT("The artifact kind must be a non-empty token containing only letters, digits, '_', '-', '.' or '+'."), TEXT("kind"), ArtifactKind, true);
		else if (!IsToken(Format)) Reject(TEXT("The artifact format must be a non-empty token containing only letters, digits, '_', '-', '.' or '+'."), TEXT("format"), Format, true);
		else if (!MimeType.Contains(TEXT("/"))) Reject(TEXT("The artifact MIME type must contain '/'."), TEXT("mime_type"), MimeType, true);
		else if (FPaths::GetPath(FinalPath).IsEmpty()) Reject(TEXT("The artifact destination must have a parent directory."), TEXT("final_path"), FinalPath, false);
	}

	const FString& FFileArtifactPublishRequest::GetSourcePath() const
	{
		return SourcePath;
	}

	const FString& FFileArtifactPublishRequest::GetFinalPath() const
	{
		return FinalPath;
	}

	const FString& FFileArtifactPublishRequest::GetPublisherOwnerId() const
	{
		return PublisherOwnerId;
	}

	const FString& FFileArtifactPublishRequest::GetArtifactKind() const
	{
		return ArtifactKind;
	}

	const FString& FFileArtifactPublishRequest::GetFormat() const
	{
		return Format;
	}

	const FString& FFileArtifactPublishRequest::GetMimeType() const
	{
		return MimeType;
	}

	bool FFileArtifactPublishRequest::IsValid() const
	{
		return !ValidationDiagnostic.IsValid();
	}

	const FFileArtifactPublishError* FFileArtifactPublishRequest::GetValidationDiagnostic() const
	{
		return ValidationDiagnostic.Get();
	}

	FFileArtifactReference::FFileArtifactReference()
	{
		auto Error = FFileArtifactPublishError::Failure(TEXT("file_artifact_reference_invalid"),
			TEXT("The file artifact reference has no artifact identifier."), TEXT("artifact_publish"));
		Error.PropertyPath = TEXT("artifact_id");
		ValidationDiagnostic = MakeShared<FFileArtifactPublishError>(MoveTemp(Error));
	}

	FFileArtifactReference::FFileArtifactReference(
		FString InArtifactId,
		FString InPublisherOwnerId,
		FString InArtifactKind,
		FString InFormat,
		FString InMimeType,
		FString InLocator,
		FString InContentHash,
		const int64 InByteCount)
		: ArtifactId(NormalizeText(MoveTemp(InArtifactId)))
		, PublisherOwnerId(NormalizeText(MoveTemp(InPublisherOwnerId)))
		, ArtifactKind(NormalizeLower(MoveTemp(InArtifactKind)))
		, Format(NormalizeLower(MoveTemp(InFormat)))
		, MimeType(NormalizeLower(MoveTemp(InMimeType)))
		, Locator(NormalizePath(InLocator))
		, ContentHash(NormalizeLower(MoveTemp(InContentHash)))
		, ByteCount(InByteCount)
	{
		auto Reject = [this](const FString& Reason, const FString& Path, const FString& Actual, const bool bPublic)
		{
			auto Error = FFileArtifactPublishError::Failure(TEXT("file_artifact_reference_invalid"), Reason, TEXT("artifact_publish"));
			Error.PropertyPath = Path;
			if (bPublic) Error.ActualValue = MakeShared<FJsonValueString>(Actual);
			else Error.ActualEvidence = MakeShared<FJsonValueString>(Actual);
			ValidationDiagnostic = MakeShared<FFileArtifactPublishError>(MoveTemp(Error));
		};
		if (!ArtifactId.StartsWith(TEXT("sha256:")) && !ArtifactId.StartsWith(TEXT("artifact:")))
			Reject(TEXT("The file artifact identifier must start with 'sha256:' or 'artifact:'."), TEXT("artifact_id"), ArtifactId, false);
		else if (PublisherOwnerId.IsEmpty()) Reject(TEXT("The file artifact reference has no publisher owner."), TEXT("publisher_owner_id"), PublisherOwnerId, false);
		else if (!IsToken(ArtifactKind)) Reject(TEXT("The file artifact kind must be a non-empty token containing only letters, digits, '_', '-', '.' or '+'."), TEXT("kind"), ArtifactKind, true);
		else if (!IsToken(Format)) Reject(TEXT("The file artifact format must be a non-empty token containing only letters, digits, '_', '-', '.' or '+'."), TEXT("format"), Format, true);
		else if (!MimeType.Contains(TEXT("/"))) Reject(TEXT("The file artifact MIME type must contain '/'."), TEXT("mime_type"), MimeType, true);
		else if (Locator.IsEmpty()) Reject(TEXT("The file artifact reference has no file locator."), TEXT("locator"), Locator, false);
		else if (!ContentHash.StartsWith(TEXT("sha256:"))) Reject(TEXT("The file artifact content hash must start with 'sha256:'."), TEXT("content_hash"), ContentHash, false);
		else if (ByteCount < 0)
		{
			auto Error = FFileArtifactPublishError::Failure(TEXT("file_artifact_reference_invalid"),
				TEXT("The file artifact byte count must be non-negative."), TEXT("artifact_publish"));
			Error.PropertyPath = TEXT("byte_count");
			Error.ActualValue = MakeShared<FJsonValueNumber>(static_cast<double>(ByteCount));
			ValidationDiagnostic = MakeShared<FFileArtifactPublishError>(MoveTemp(Error));
		}
	}

	const FString& FFileArtifactReference::GetArtifactId() const
	{
		return ArtifactId;
	}

	const FString& FFileArtifactReference::GetPublisherOwnerId() const
	{
		return PublisherOwnerId;
	}

	const FString& FFileArtifactReference::GetArtifactKind() const
	{
		return ArtifactKind;
	}

	const FString& FFileArtifactReference::GetFormat() const
	{
		return Format;
	}

	const FString& FFileArtifactReference::GetMimeType() const
	{
		return MimeType;
	}

	const FString& FFileArtifactReference::GetLocator() const
	{
		return Locator;
	}

	const FString& FFileArtifactReference::GetContentHash() const
	{
		return ContentHash;
	}

	int64 FFileArtifactReference::GetByteCount() const
	{
		return ByteCount;
	}

	bool FFileArtifactReference::IsValid() const
	{
		return !ValidationDiagnostic.IsValid();
	}

	const FFileArtifactPublishError* FFileArtifactReference::GetValidationDiagnostic() const
	{
		return ValidationDiagnostic.Get();
	}

	

	

	

	

	bool TryPublishFileArtifact(
		const FFileArtifactPublishRequest& Request,
		FUeAgentResourceTracker& Resources,
		FFileArtifactReference& OutReference,
		FFileArtifactPublishError& OutError)
	{
		OutReference = FFileArtifactReference();
		OutError = FFileArtifactPublishError();
		if (!Request.IsValid())
		{
			OutError = *Request.GetValidationDiagnostic();
			return false;
		}

		TArray<uint8> SourceBytes;
		if (!FFileHelper::LoadFileToArray(
			SourceBytes, *Request.GetSourcePath()))
		{
			OutError = FileFailure(TEXT("file_artifact_source_read_failed"),
				TEXT("The source file for artifact publication could not be read."), Request.GetSourcePath());
			return false;
		}
		FUeAgentResourceChargeError ChargeError;
		if (!Resources.TryChargeMany(
			{
				{EUeAgentResourceDimension::Files, 2},
				{EUeAgentResourceDimension::InputBytes, SourceBytes.Num()},
				{EUeAgentResourceDimension::OutputBytes, SourceBytes.Num()},
				{EUeAgentResourceDimension::WorkUnits,
					FMath::Max<int64>(1, SourceBytes.Num())},
				{EUeAgentResourceDimension::EstimatedMemoryBytes,
					SourceBytes.Num() * 2LL},
			},
			TEXT("file_artifact_publish"),
			ChargeError))
		{
			OutError = MoveTemp(ChargeError);
			return false;
		}

		const FString Final = Request.GetFinalPath();
		const FString Parent = FPaths::GetPath(Final);
		const FString Marker = Final + TEXT(".uai-artifact-owner.json");
		if (!IFileManager::Get().MakeDirectory(*Parent, true))
		{
			OutError = FileFailure(TEXT("file_artifact_final_parent_create_failed"),
				TEXT("The artifact destination directory could not be created."), Parent);
			return false;
		}
		if (!ExistingFinalIsOwned(Final, Marker, Request.GetPublisherOwnerId(), OutError)) return false;

		const FString TransactionId =
			FGuid::NewGuid().ToString(EGuidFormats::Digits);
		const FString Prefix = Parent / (
			TEXT(".") + FPaths::GetCleanFilename(Final)
			+ TEXT(".uai-") + TransactionId);
		const FString Staging = Prefix + TEXT(".staging");
		const FString StagingMarker = Prefix + TEXT(".staging-owner.json");
		const FString Backup = Prefix + TEXT(".backup");
		const FString BackupMarker = Prefix + TEXT(".backup-owner.json");
		IFileManager& Files = IFileManager::Get();
		const TArray<TPair<FString, FString>> TransactionFiles = {
			{TEXT("staging"), Staging}, {TEXT("staging_owner"), StagingMarker},
			{TEXT("backup"), Backup}, {TEXT("backup_owner"), BackupMarker}};
		for (const auto& File : TransactionFiles)
		{
			if (Files.FileExists(*File.Value))
			{
				OutError = Failure(TEXT("file_artifact_transaction_collision"),
					TEXT("A temporary file required for artifact publication already exists."), File.Key, File.Value);
				return false;
			}
		}

		const FString ContentHash = Sha256(SourceBytes);
		const FString ArtifactId = Sha256Text({
			Request.GetPublisherOwnerId(),
			Final,
			Request.GetArtifactKind(),
			Request.GetFormat(),
			Request.GetMimeType(),
			ContentHash,
		});
		const FFileArtifactReference Reference(
			ArtifactId,
			Request.GetPublisherOwnerId(),
			Request.GetArtifactKind(),
			Request.GetFormat(),
			Request.GetMimeType(),
			Final,
			ContentHash,
			SourceBytes.Num());
		if (!Reference.IsValid())
		{
			OutError = *Reference.GetValidationDiagnostic();
			return false;
		}
		auto CleanupStaging = [&]()
		{
			CleanupFile(Staging, TEXT("The temporary artifact file could not be removed."), OutError);
			CleanupFile(StagingMarker, TEXT("The temporary artifact ownership record could not be removed."), OutError);
		};
		if (!FFileHelper::SaveArrayToFile(SourceBytes, *Staging))
		{
			OutError = FileFailure(TEXT("file_artifact_staging_write_failed"),
				TEXT("The staged artifact content could not be written."), Staging);
			CleanupStaging();
			return false;
		}
		if (!SaveOwnerRecord(StagingMarker, Reference, OutError))
		{
			CleanupStaging();
			return false;
		}

		TArray<uint8> StagingBytes;
		if (!FFileHelper::LoadFileToArray(StagingBytes, *Staging))
		{
			OutError = FileFailure(TEXT("file_artifact_staging_verify_failed"),
				TEXT("The staged artifact could not be read back for verification."), Staging);
			CleanupStaging();
			return false;
		}
		if (!VerifyBytes(StagingBytes, SourceBytes, ContentHash, TEXT("file_artifact_staging_verify_failed"),
			TEXT("The staged artifact content differs from the source file."), Staging, OutError))
		{
			CleanupStaging();
			return false;
		}

		const bool bHadFinal = Files.FileExists(*Final);
		if (bHadFinal)
		{
			if (!MoveFile(Backup, Final))
			{
				OutError = FileFailure(TEXT("file_artifact_backup_failed"),
					TEXT("The existing artifact could not be moved to its transaction backup."), Final);
			}
			else if (!MoveFile(BackupMarker, Marker))
			{
				OutError = FileFailure(TEXT("file_artifact_backup_failed"),
					TEXT("The existing artifact ownership record could not be moved to its transaction backup."), Marker);
			}
			if (!OutError.Code.IsEmpty())
			{
				if (Files.FileExists(*Backup) && !Files.FileExists(*Final))
					RestoreFile(Final, Backup, TEXT("The previous artifact could not be restored after the backup step failed."), OutError);
				if (Files.FileExists(*BackupMarker) && !Files.FileExists(*Marker))
					RestoreFile(Marker, BackupMarker, TEXT("The previous ownership record could not be restored after the backup step failed."), OutError);
				CleanupStaging();
				return false;
			}
		}

		TArray<uint8> FinalBytes;
		bool bVerified = false;
		if (!MoveFile(Final, Staging))
			OutError = FileFailure(TEXT("file_artifact_publish_move_failed"), TEXT("The staged artifact could not be moved to its destination."), Staging);
		else if (!MoveFile(Marker, StagingMarker))
			OutError = FileFailure(TEXT("file_artifact_publish_move_failed"), TEXT("The staged ownership record could not be moved to its destination."), StagingMarker);
		else if (!FFileHelper::LoadFileToArray(FinalBytes, *Final))
			OutError = FileFailure(TEXT("file_artifact_publish_verify_failed"), TEXT("The published artifact could not be read back for verification."), Final);
		else bVerified = VerifyBytes(FinalBytes, SourceBytes, ContentHash, TEXT("file_artifact_publish_verify_failed"),
			TEXT("The published artifact content differs from the source file."), Final, OutError);
		if (!bVerified)
		{
			const bool bFinalRemoved = CleanupFile(Final, TEXT("The failed publication's artifact file could not be removed."), OutError);
			const bool bMarkerRemoved = CleanupFile(Marker, TEXT("The failed publication's ownership record could not be removed."), OutError);
			bool bRestored = bFinalRemoved && bMarkerRemoved;
			if (bHadFinal)
			{
				const bool bFileRestored = RestoreFile(Final, Backup, TEXT("The previous artifact could not be restored after publication failed."), OutError);
				const bool bMarkerRestored = RestoreFile(Marker, BackupMarker, TEXT("The previous ownership record could not be restored after publication failed."), OutError);
				bRestored = bRestored && bFileRestored && bMarkerRestored;
			}
			CleanupStaging();
			auto Result = Failure(bRestored ? TEXT("file_artifact_publish_failed_restored") : TEXT("file_artifact_publish_state_unknown"),
				bRestored ? TEXT("Artifact publication failed; the previous destination state was restored.")
					: TEXT("Artifact publication failed and the previous destination state could not be fully restored."), FString(), Final);
			Result.PublicCause = MakeShared<FFileArtifactPublishError>(MoveTemp(OutError));
			OutError = MoveTemp(Result);
			return false;
		}

		OutReference = Reference;
		const bool bBackupRemoved = CleanupFile(Backup, TEXT("The artifact was published and verified, but its previous backup could not be removed."), OutError);
		const bool bBackupMarkerRemoved = CleanupFile(BackupMarker, TEXT("The artifact was published and verified, but its previous ownership backup could not be removed."), OutError);
		if (!bBackupRemoved || !bBackupMarkerRemoved)
		{
			auto State = MakeShared<FJsonObject>();
			State->SetBoolField(TEXT("artifact_published"), true);
			OutError.ActualValue = MakeShared<FJsonValueObject>(State);
			return false;
		}
		return true;
	}

	bool TryProjectFileArtifactReference(
		const FFileArtifactReference& Reference,
		FUeAgentResourceTracker& Resources,
		FUeAgentCanonicalValuePtr& OutValue,
		FFileArtifactPublishError& OutError)
	{
		OutValue.Reset();
		OutError = FFileArtifactPublishError();
		if (!Reference.IsValid())
		{
			OutError = *Reference.GetValidationDiagnostic();
			return false;
		}
		TArray<TPair<FString, FUeAgentCanonicalValueRef>> Items;
		const TArray<TPair<FString, FString>> Strings = {
			{TEXT("artifact_id"), Reference.GetArtifactId()},
			{TEXT("publisher_owner_id"), Reference.GetPublisherOwnerId()},
			{TEXT("kind"), Reference.GetArtifactKind()},
			{TEXT("format"), Reference.GetFormat()},
			{TEXT("mime_type"), Reference.GetMimeType()},
			{TEXT("locator"), Reference.GetLocator()},
			{TEXT("content_hash"), Reference.GetContentHash()},
			{TEXT("ownership"), TEXT("server_retained")},
		};
		for (const TPair<FString, FString>& Item : Strings)
		{
			FUeAgentCanonicalValuePtr Value;
			if (!MakeStringValue(Item.Value, Resources, Value, OutError))
			{
				if (OutError.PropertyPath.IsEmpty()) OutError.PropertyPath = Item.Key;
				return false;
			}
			Items.Emplace(Item.Key, Value.ToSharedRef());
		}
		FUeAgentCanonicalValuePtr ByteCount;
		if (!MakeIntegerValue(
			Reference.GetByteCount(), Resources, ByteCount, OutError))
		{
			if (OutError.PropertyPath.IsEmpty()) OutError.PropertyPath = TEXT("byte_count");
			return false;
		}
		Items.Emplace(TEXT("byte_count"), ByteCount.ToSharedRef());
		FUeAgentStateSemanticError SemanticError;
		if (!FUeAgentCanonicalValue::TryObject(
			Items, Resources, OutValue, SemanticError))
		{
			OutError = MoveTemp(SemanticError);
			return false;
		}
		return true;
	}
}
