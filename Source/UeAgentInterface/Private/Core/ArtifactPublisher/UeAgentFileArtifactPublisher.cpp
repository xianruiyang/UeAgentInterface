#include "Core/ArtifactPublisher/UeAgentFileArtifactPublisher.h"

#include "Dom/JsonObject.h"
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

		void SetError(
			FFileArtifactPublishError& OutError,
			FString Code,
			FString Path = FString(),
			FString Detail = FString())
		{
			OutError.Set(MoveTemp(Code), MoveTemp(Path), MoveTemp(Detail));
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
			const FFileArtifactReference& Reference)
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
			return SerializeJson(Object, Text)
				&& FFileHelper::SaveStringToFile(
					Text + TEXT("\n"),
					*Path,
					FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM);
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
				SetError(
					OutError,
					TEXT("file_artifact_final_ownership_required"),
					Final,
					TEXT("final and ownership marker are inconsistent"));
				return false;
			}
			FString Text;
			TSharedPtr<FJsonObject> Object;
			if (!FFileHelper::LoadFileToString(Text, *Marker)
				|| !FJsonSerializer::Deserialize(
					TJsonReaderFactory<>::Create(Text), Object)
				|| !Object.IsValid())
			{
				SetError(
					OutError,
					TEXT("file_artifact_final_ownership_required"), Marker);
				return false;
			}
			FString Format;
			FString MarkerOwner;
			FString MarkerFinal;
			if (!Object->TryGetStringField(TEXT("format"), Format)
				|| Format != FileArtifactFormat
				|| !Object->TryGetStringField(
					TEXT("publisher_owner_id"), MarkerOwner)
				|| MarkerOwner != OwnerId
				|| !Object->TryGetStringField(TEXT("locator"), MarkerFinal)
				|| NormalizePath(MarkerFinal) != Final)
			{
				SetError(
					OutError,
					TEXT("file_artifact_final_ownership_required"), Marker);
				return false;
			}
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
				SetError(OutError, Error.Code, Error.Path);
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
				SetError(OutError, Error.Code, Error.Path);
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
		bValid = !SourcePath.IsEmpty()
			&& !FinalPath.IsEmpty()
			&& !SourcePath.Equals(FinalPath, ESearchCase::IgnoreCase)
			&& !PublisherOwnerId.IsEmpty()
			&& IsToken(ArtifactKind)
			&& IsToken(Format)
			&& MimeType.Contains(TEXT("/"))
			&& !FPaths::GetPath(FinalPath).IsEmpty();
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
		return bValid;
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
		bValid = ArtifactId.StartsWith(TEXT("sha256:"))
			|| ArtifactId.StartsWith(TEXT("artifact:"));
		bValid = bValid
			&& !PublisherOwnerId.IsEmpty()
			&& IsToken(ArtifactKind)
			&& IsToken(Format)
			&& MimeType.Contains(TEXT("/"))
			&& !Locator.IsEmpty()
			&& ContentHash.StartsWith(TEXT("sha256:"))
			&& ByteCount >= 0;
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
		return bValid;
	}

	const FString& FFileArtifactPublishError::GetCode() const
	{
		return Code;
	}

	const FString& FFileArtifactPublishError::GetPath() const
	{
		return Path;
	}

	const FString& FFileArtifactPublishError::GetDetail() const
	{
		return Detail;
	}

	void FFileArtifactPublishError::Set(
		FString InCode,
		FString InPath,
		FString InDetail)
	{
		Code = NormalizeText(MoveTemp(InCode));
		Path = NormalizeText(MoveTemp(InPath));
		Detail = NormalizeText(MoveTemp(InDetail));
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
			SetError(OutError, TEXT("file_artifact_request_invalid"));
			return false;
		}

		TArray<uint8> SourceBytes;
		if (!FFileHelper::LoadFileToArray(
			SourceBytes, *Request.GetSourcePath()))
		{
			SetError(
				OutError,
				TEXT("file_artifact_source_read_failed"),
				Request.GetSourcePath());
			return false;
		}
		FUeAgentResourceBudgetExceeded BudgetError;
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
			BudgetError))
		{
			SetError(
				OutError,
				BudgetError.Code,
				BudgetError.Path,
				LexToString(BudgetError.Dimension));
			return false;
		}

		const FString Final = Request.GetFinalPath();
		const FString Parent = FPaths::GetPath(Final);
		const FString Marker = Final + TEXT(".uai-artifact-owner.json");
		if (!IFileManager::Get().MakeDirectory(*Parent, true)
			|| !ExistingFinalIsOwned(
				Final, Marker, Request.GetPublisherOwnerId(), OutError))
		{
			if (OutError.GetCode().IsEmpty())
			{
				SetError(
					OutError,
					TEXT("file_artifact_final_parent_create_failed"), Parent);
			}
			return false;
		}

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
		if (Files.FileExists(*Staging)
			|| Files.FileExists(*StagingMarker)
			|| Files.FileExists(*Backup)
			|| Files.FileExists(*BackupMarker))
		{
			SetError(
				OutError, TEXT("file_artifact_transaction_collision"), Prefix);
			return false;
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
		if (!Reference.IsValid()
			|| !FFileHelper::SaveArrayToFile(SourceBytes, *Staging)
			|| !SaveOwnerRecord(StagingMarker, Reference))
		{
			DeleteFile(Staging);
			DeleteFile(StagingMarker);
			SetError(
				OutError, TEXT("file_artifact_staging_write_failed"), Staging);
			return false;
		}

		TArray<uint8> StagingBytes;
		if (!FFileHelper::LoadFileToArray(StagingBytes, *Staging)
			|| StagingBytes != SourceBytes
			|| Sha256(StagingBytes) != ContentHash)
		{
			DeleteFile(Staging);
			DeleteFile(StagingMarker);
			SetError(
				OutError, TEXT("file_artifact_staging_verify_failed"), Staging);
			return false;
		}

		const bool bHadFinal = Files.FileExists(*Final);
		if (bHadFinal
			&& (!MoveFile(Backup, Final)
				|| !MoveFile(BackupMarker, Marker)))
		{
			if (Files.FileExists(*Backup) && !Files.FileExists(*Final))
			{
				MoveFile(Final, Backup);
			}
			if (Files.FileExists(*BackupMarker)
				&& !Files.FileExists(*Marker))
			{
				MoveFile(Marker, BackupMarker);
			}
			DeleteFile(Staging);
			DeleteFile(StagingMarker);
			SetError(
				OutError, TEXT("file_artifact_backup_failed"), Final);
			return false;
		}

		const bool bPublished = MoveFile(Final, Staging)
			&& MoveFile(Marker, StagingMarker);
		TArray<uint8> FinalBytes;
		const bool bVerified = bPublished
			&& FFileHelper::LoadFileToArray(FinalBytes, *Final)
			&& FinalBytes == SourceBytes
			&& Sha256(FinalBytes) == ContentHash;
		if (!bVerified)
		{
			DeleteFile(Final);
			DeleteFile(Marker);
			bool bRestored = !bHadFinal;
			if (bHadFinal)
			{
				bRestored = MoveFile(Final, Backup)
					&& MoveFile(Marker, BackupMarker);
			}
			DeleteFile(Staging);
			DeleteFile(StagingMarker);
			SetError(
				OutError,
				bRestored
					? TEXT("file_artifact_publish_failed_restored")
					: TEXT("file_artifact_publish_state_unknown"),
				Final);
			return false;
		}

		DeleteFile(Backup);
		DeleteFile(BackupMarker);
		OutReference = Reference;
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
			SetError(OutError, TEXT("file_artifact_reference_invalid"));
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
				return false;
			}
			Items.Emplace(Item.Key, Value.ToSharedRef());
		}
		FUeAgentCanonicalValuePtr ByteCount;
		if (!MakeIntegerValue(
			Reference.GetByteCount(), Resources, ByteCount, OutError))
		{
			return false;
		}
		Items.Emplace(TEXT("byte_count"), ByteCount.ToSharedRef());
		FUeAgentStateSemanticError SemanticError;
		if (!FUeAgentCanonicalValue::TryObject(
			Items, Resources, OutValue, SemanticError))
		{
			SetError(
				OutError, SemanticError.Code, SemanticError.Path);
			return false;
		}
		return true;
	}
}
