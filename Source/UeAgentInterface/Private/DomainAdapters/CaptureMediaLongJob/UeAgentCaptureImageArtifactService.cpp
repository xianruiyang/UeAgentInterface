#include "DomainAdapters/CaptureMediaLongJob/UeAgentCaptureImageArtifactService.h"

#include "Core/Verification/UeAgentIndependentReadback.h"
#include "DomainAdapters/CaptureMediaLongJob/UeAgentCaptureMediaReadback.h"
#include "HAL/FileManager.h"
#include "ImageCore.h"
#include "ImageUtils.h"
#include "Misc/FileHelper.h"
#include "Misc/Guid.h"
#include "Misc/Paths.h"

THIRD_PARTY_INCLUDES_START
#include <openssl/sha.h>
THIRD_PARTY_INCLUDES_END

namespace UE::UAI::DomainAdapters::CaptureMediaLongJob
{
	using namespace ArtifactPublisher;
	using namespace ContextResource;
	using namespace ExecutionMutation;
	using namespace StateSemantic;
	using namespace Verification;

	namespace
	{
		bool Fail(FExecutionMutationError& OutError, FString Code, FString Detail = {})
		{
			OutError = FExecutionMutationError(MoveTemp(Code), MoveTemp(Detail));
			return false;
		}

		FString Sha256(const void* Data, const int64 NumBytes)
		{
			uint8 Digest[SHA256_DIGEST_LENGTH] = {};
			const uint8 Empty = 0;
			SHA256(NumBytes > 0 ? static_cast<const uint8*>(Data) : &Empty,
				static_cast<size_t>(FMath::Max<int64>(0, NumBytes)), Digest);
			FString Result(TEXT("sha256:"));
			for (const uint8 Byte : Digest) Result += FString::Printf(TEXT("%02x"), Byte);
			return Result;
		}

		FString SafeSegment(FString Value)
		{
			for (TCHAR& Character : Value)
			{
				if (!(FChar::IsAlnum(Character) || Character == TEXT('-') || Character == TEXT('_')))
				{
					Character = TEXT('_');
				}
			}
			return Value.IsEmpty() ? TEXT("unknown") : Value;
		}

		bool TryInspectImage(
			const FString& Path,
			int32& OutWidth,
			int32& OutHeight,
			FString& OutPixelHash,
			FExecutionMutationError& OutError)
		{
			FImage Image;
			if (!FImageUtils::LoadImage(*Path, Image))
			{
				return Fail(OutError, TEXT("capture_image_decode_unavailable"), Path);
			}
			Image.ChangeFormat(ERawImageFormat::BGRA8, EGammaSpace::sRGB);
			if (Image.SizeX <= 0 || Image.SizeY <= 0 || Image.SizeX > MAX_int32 || Image.SizeY > MAX_int32)
			{
				return Fail(OutError, TEXT("capture_image_dimensions_invalid"), Path);
			}
			OutWidth = static_cast<int32>(Image.SizeX);
			OutHeight = static_cast<int32>(Image.SizeY);
			OutPixelHash = Sha256(Image.RawData.GetData(), Image.RawData.Num());
			return true;
		}

		bool MakeString(const FString& Value, FUeAgentResourceTracker& Resources, FUeAgentCanonicalValuePtr& Out)
		{
			FUeAgentStateSemanticError Error;
			return FUeAgentCanonicalValue::TryString(Value, Resources, Out, Error) && Out.IsValid();
		}

		bool MakeInteger(const int64 Value, FUeAgentResourceTracker& Resources, FUeAgentCanonicalValuePtr& Out)
		{
			FUeAgentStateSemanticError Error;
			return FUeAgentCanonicalValue::TryInteger(
				FString::Printf(TEXT("%lld"), Value), Resources, Out, Error)
				&& Out.IsValid();
		}

		bool TryExpected(const FUeAgentStagedCaptureImage& Image, FUeAgentResourceTracker& Resources, FUeAgentCanonicalValuePtr& Out)
		{
			FUeAgentCanonicalValuePtr Subject, Width, Height, Count, Hash;
			if (!MakeString(Image.GetSubjectRef(), Resources, Subject)
				|| !MakeInteger(Image.GetWidth(), Resources, Width)
				|| !MakeInteger(Image.GetHeight(), Resources, Height)
				|| !MakeInteger(static_cast<int64>(Image.GetWidth()) * Image.GetHeight(), Resources, Count)
				|| !MakeString(Image.GetPixelHash(), Resources, Hash)) return false;
			FUeAgentStateSemanticError Error;
			return FUeAgentCanonicalValue::TryObject({
				{TEXT("subject_ref"), Subject.ToSharedRef()}, {TEXT("width"), Width.ToSharedRef()},
				{TEXT("height"), Height.ToSharedRef()}, {TEXT("pixel_count"), Count.ToSharedRef()},
				{TEXT("pixel_sha256"), Hash.ToSharedRef()}}, Resources, Out, Error) && Out.IsValid();
		}
	}

	bool FUeAgentStagedCaptureImage::IsValid() const
	{
		return !SubjectRef.IsEmpty() && !NativePath.IsEmpty() && !FinalPath.IsEmpty()
			&& (Format == TEXT("png") || Format == TEXT("jpg")
				|| Format == TEXT("jpeg") || Format == TEXT("webp"))
			&& Width > 0 && Height > 0 && PixelHash.StartsWith(TEXT("sha256:"));
	}

	FUeAgentStagedCaptureImage::FUeAgentStagedCaptureImage(
		FString InSubjectRef,
		FString InNativePath,
		FString InFinalPath,
		FString InFormat,
		const int32 InWidth,
		const int32 InHeight,
		FString InPixelHash)
		: SubjectRef(MoveTemp(InSubjectRef))
		, NativePath(MoveTemp(InNativePath))
		, FinalPath(MoveTemp(InFinalPath))
		, Format(MoveTemp(InFormat))
		, PixelHash(MoveTemp(InPixelHash))
		, Width(InWidth)
		, Height(InHeight)
	{
	}

	bool FUeAgentCaptureImageArtifactService::TryStagePngPixels(
		const FString& OperationId, const FString& SubjectRef, const TArray<FColor>& Pixels,
		const FIntPoint& Size, FUeAgentResourceTracker& Resources,
		FUeAgentStagedCaptureImage& OutImage, FExecutionMutationError& OutError) const
	{
		OutImage = {};
		if (OperationId.IsEmpty() || SubjectRef.IsEmpty() || Size.X <= 0 || Size.Y <= 0
			|| Pixels.Num() != Size.X * Size.Y)
		{
			return Fail(OutError, TEXT("capture_image_stage_request_invalid"));
		}
		const int64 PixelBytes = static_cast<int64>(Pixels.Num()) * sizeof(FColor);
		FUeAgentResourceBudgetExceeded BudgetError;
		if (!Resources.TryChargeMany({
			{EUeAgentResourceDimension::WorkUnits, 1},
			{EUeAgentResourceDimension::EstimatedMemoryBytes, PixelBytes}},
			TEXT("capture_image_stage_pixels"), BudgetError))
		{
			return Fail(OutError, BudgetError.Code, BudgetError.Path);
		}
		const FString Root = FPaths::ConvertRelativePathToFull(FPaths::ProjectSavedDir()
			/ TEXT("UeAgentInterface/LongRunningSession/Native") / SafeSegment(OperationId));
		const FString Native = Root / TEXT("capture.png");
		TArray64<uint8> Compressed;
		const FImageView View(const_cast<FColor*>(Pixels.GetData()), Size.X, Size.Y, ERawImageFormat::BGRA8);
		if (!FImageUtils::CompressImage(Compressed, TEXT("png"), View, 100)
			|| Compressed.IsEmpty() || Compressed.Num() > MAX_int32)
		{
			return Fail(OutError, TEXT("capture_image_compress_failed"));
		}
		TArray<uint8> Bytes;
		Bytes.Append(Compressed.GetData(), static_cast<int32>(Compressed.Num()));
		if (!IFileManager::Get().MakeDirectory(*Root, true) || !FFileHelper::SaveArrayToFile(Bytes, *Native))
		{
			return Fail(OutError, TEXT("capture_image_write_failed"), Native);
		}
		return TryAdoptStagedImage(OperationId, SubjectRef, Native, FString(), TEXT("png"), OutImage, OutError);
	}

	bool FUeAgentCaptureImageArtifactService::TryAdoptStagedImage(
		const FString& OperationId, const FString& SubjectRef, const FString& NativePath,
		const FString& RequestedFinalPath, const FString& Format,
		FUeAgentStagedCaptureImage& OutImage, FExecutionMutationError& OutError) const
	{
		OutImage = {};
		FString NormalizedFormat = Format;
		NormalizedFormat.TrimStartAndEndInline();
		NormalizedFormat.ToLowerInline();
		if (OperationId.IsEmpty() || SubjectRef.IsEmpty() || NativePath.IsEmpty()
			|| (NormalizedFormat != TEXT("png") && NormalizedFormat != TEXT("jpg")
				&& NormalizedFormat != TEXT("jpeg") && NormalizedFormat != TEXT("webp")))
		{
			return Fail(OutError, TEXT("capture_image_staged_identity_invalid"));
		}
		OutImage.SubjectRef = SubjectRef;
		OutImage.NativePath = FPaths::ConvertRelativePathToFull(NativePath);
		OutImage.FinalPath = RequestedFinalPath.IsEmpty()
			? FPaths::ConvertRelativePathToFull(FPaths::ProjectSavedDir()
				/ TEXT("UeAgentInterface/LongRunningSession/Artifacts")
				/ (FGuid::NewGuid().ToString(EGuidFormats::Digits) + TEXT(".") + NormalizedFormat))
			: FPaths::ConvertRelativePathToFull(RequestedFinalPath);
		OutImage.Format = MoveTemp(NormalizedFormat);
		if (!TryInspectImage(OutImage.NativePath, OutImage.Width, OutImage.Height, OutImage.PixelHash, OutError))
		{
			OutImage = {};
			return false;
		}
		return true;
	}

	bool FUeAgentCaptureImageArtifactService::TryPublishVerified(
		const FString& OperationId, const FString& PublisherOwnerId,
		const FUeAgentStagedCaptureImage& Image, FUeAgentResourceTracker& Resources,
		FFileArtifactReference& OutArtifact, FString& OutEvidenceId,
		FExecutionMutationError& OutError) const
	{
		OutArtifact = {};
		OutEvidenceId.Reset();
		if (OperationId.IsEmpty() || PublisherOwnerId.IsEmpty() || !Image.IsValid())
		{
			return Fail(OutError, TEXT("capture_image_publish_request_invalid"));
		}
		FFileArtifactPublishError PublishError;
		const FString MimeType = Image.GetFormat() == TEXT("png")
			? FString(TEXT("image/png"))
			: (Image.GetFormat() == TEXT("webp")
				? FString(TEXT("image/webp")) : FString(TEXT("image/jpeg")));
		if (!TryPublishFileArtifact(FFileArtifactPublishRequest(Image.GetNativePath(), Image.GetFinalPath(),
			PublisherOwnerId, TEXT("image"), Image.GetFormat(), MimeType),
			Resources, OutArtifact, PublishError))
		{
			return Fail(OutError, PublishError.GetCode(), PublishError.GetDetail());
		}
		TSharedPtr<IIndependentReadbackOracle, ESPMode::ThreadSafe> Oracle;
		FVerificationError VerifyError;
		FUeAgentCanonicalValuePtr Expected;
		TSharedPtr<FIndependentReadbackService> Readback;
		FIndependentReadbackOutcome Outcome;
		const FString OracleId = TEXT("uai.readback.capture.image/v1");
		if (!TryExpected(Image, Resources, Expected)
			|| !TryCreateCaptureImageArtifactReadbackOracle(OracleId, Image.GetSubjectRef(), Oracle, VerifyError)
			|| !Oracle.IsValid()
			|| !FIndependentReadbackService::TryCreate({Oracle.ToSharedRef()}, Readback, VerifyError)
			|| !Readback.IsValid()
			|| !Readback->TryVerify(FIndependentReadbackRequest(OperationId, OracleId,
				Image.GetSubjectRef(), Expected.ToSharedRef(), OutArtifact), Resources, Outcome, VerifyError)
			|| Outcome.GetStatus() != EVerificationStatus::Verified
			|| Outcome.GetEffectOutcome() != EEffectOutcome::Complete)
		{
			IFileManager::Get().Delete(*OutArtifact.GetLocator(), false, true, true);
			IFileManager::Get().Delete(
				*(OutArtifact.GetLocator() + TEXT(".uai-artifact-owner.json")),
				false, true, true);
			OutArtifact = {};
			return Fail(OutError,
				VerifyError.GetCode().IsEmpty() ? TEXT("capture_image_readback_failed") : VerifyError.GetCode(),
				Outcome.GetDiagnosticCode());
		}
		OutEvidenceId = Outcome.GetEvidenceId();
		if (OutEvidenceId.IsEmpty())
		{
			IFileManager::Get().Delete(*OutArtifact.GetLocator(), false, true, true);
			IFileManager::Get().Delete(
				*(OutArtifact.GetLocator() + TEXT(".uai-artifact-owner.json")),
				false, true, true);
			OutArtifact = {};
			return Fail(OutError, TEXT("capture_image_readback_evidence_missing"));
		}
		return true;
	}

	void FUeAgentCaptureImageArtifactService::DiscardStaged(
		const FUeAgentStagedCaptureImage& Image) const
	{
		if (!Image.GetNativePath().IsEmpty())
		{
			IFileManager::Get().Delete(*Image.GetNativePath(), false, true, true);
		}
	}
}
