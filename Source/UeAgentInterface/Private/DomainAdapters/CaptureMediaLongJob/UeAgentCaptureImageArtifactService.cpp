#include "DomainAdapters/CaptureMediaLongJob/UeAgentCaptureImageArtifactService.h"
#include "Core/Contract/UeAgentDiagnostic.h"
#include "DomainAdapters/CaptureMediaLongJob/UeAgentCaptureMediaDiagnostics.h"

#include "Core/Verification/UeAgentIndependentReadback.h"
#include "Core/StateSemantic/UeAgentCanonicalValueJson.h"
#include "Async/Async.h"
#include "DomainAdapters/CaptureMediaLongJob/UeAgentCaptureImageProcessing.h"
#include "DomainAdapters/CaptureMediaLongJob/UeAgentCaptureMediaReadback.h"
#include "HAL/FileManager.h"
#include "ImageCore.h"
#include "ImageUtils.h"
#include "Misc/Guid.h"
#include "Misc/Paths.h"
#include "Modules/ModuleManager.h"
#include "IImageWrapperModule.h"

THIRD_PARTY_INCLUDES_START
#include <openssl/sha.h>
THIRD_PARTY_INCLUDES_END

namespace UE::UAI::DomainAdapters::CaptureMediaLongJob
{
	using namespace CaptureDiagnostics;
	using namespace ArtifactPublisher;
	using namespace ContextResource;
	using namespace ExecutionMutation;
	using namespace StateSemantic;
	using namespace Verification;

	namespace
	{
		bool Fail(UE::UAI::Contract::FDiagnostic& OutError, UE::UAI::Contract::FDiagnostic Diagnostic, const FString& Subject = FString())
        {
            OutError = MoveTemp(Diagnostic);
            if (OutError.SubjectId.IsEmpty()) OutError.SubjectId = Subject;
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
				auto Error = Native(TEXT("capture_image_decode_unavailable"), TEXT("The staged capture file could not be decoded as an image."),
					TEXT("execution"), TEXT("image_decoded"), MakeShared<FJsonValueBoolean>(false), MakeShared<FJsonValueBoolean>(true));
				Error.InternalContext = Path;
				return Fail(OutError, MoveTemp(Error));
			}
			Image.ChangeFormat(ERawImageFormat::BGRA8, EGammaSpace::sRGB);
			if (Image.SizeX <= 0 || Image.SizeY <= 0 || Image.SizeX > MAX_int32 || Image.SizeY > MAX_int32)
			{
				auto Error = Native(TEXT("capture_image_dimensions_invalid"), TEXT("The decoded capture image dimensions are outside the supported range."),
					TEXT("execution"), TEXT("dimensions"), Facts({ { TEXT("width"), Number(Image.SizeX) }, { TEXT("height"), Number(Image.SizeY) } }),
					MakeShared<FJsonValueString>(TEXT("width and height in [1, MAX_int32]")));
				Error.InternalContext = Path;
				return Fail(OutError, MoveTemp(Error));
			}
			OutWidth = static_cast<int32>(Image.SizeX);
			OutHeight = static_cast<int32>(Image.SizeY);
			OutPixelHash = Sha256(Image.RawData.GetData(), Image.RawData.Num());
			return true;
		}

		bool MakeString(const FString& Value, FUeAgentResourceTracker& Resources, FUeAgentCanonicalValuePtr& Out, FUeAgentStateSemanticError& Error)
		{
			return FUeAgentCanonicalValue::TryString(Value, Resources, Out, Error) && Out.IsValid();
		}

		bool MakeInteger(const int64 Value, FUeAgentResourceTracker& Resources, FUeAgentCanonicalValuePtr& Out, FUeAgentStateSemanticError& Error)
		{
			return FUeAgentCanonicalValue::TryInteger(
				FString::Printf(TEXT("%lld"), Value), Resources, Out, Error)
				&& Out.IsValid();
		}

		bool TryExpected(const FUeAgentStagedCaptureImage& Image, FUeAgentResourceTracker& Resources, FUeAgentCanonicalValuePtr& Out, FUeAgentStateSemanticError& Error)
		{
			FUeAgentCanonicalValuePtr Subject, Width, Height, Count, Hash;
			if (!MakeString(Image.GetSubjectRef(), Resources, Subject, Error)
				|| !MakeInteger(Image.GetWidth(), Resources, Width, Error)
				|| !MakeInteger(Image.GetHeight(), Resources, Height, Error)
				|| !MakeInteger(static_cast<int64>(Image.GetWidth()) * Image.GetHeight(), Resources, Count, Error)
				|| !MakeString(Image.GetPixelHash(), Resources, Hash, Error)) return false;
			return FUeAgentCanonicalValue::TryObject({
				{TEXT("subject_ref"), Subject.ToSharedRef()}, {TEXT("width"), Width.ToSharedRef()},
				{TEXT("height"), Height.ToSharedRef()}, {TEXT("pixel_count"), Count.ToSharedRef()},
				{TEXT("pixel_sha256"), Hash.ToSharedRef()}}, Resources, Out, Error) && Out.IsValid();
		}
	}

	bool FUeAgentStagedCaptureImage::IsValid() const
	{
		UE::UAI::Contract::FDiagnostic Error;
		return TryValidate(Error);
	}

	bool FUeAgentStagedCaptureImage::TryValidate(UE::UAI::Contract::FDiagnostic& OutError) const
	{
		OutError.Reset();
		for (const auto& Field : { TPair<const TCHAR*, const FString*>(TEXT("subject_ref"), &SubjectRef),
			TPair<const TCHAR*, const FString*>(TEXT("native_path"), &NativePath),
			TPair<const TCHAR*, const FString*>(TEXT("final_path"), &FinalPath) })
		{
			if (Field.Value->IsEmpty())
				return Fail(OutError, Native(TEXT("capture_image_publish_request_invalid"),
					TEXT("The staged capture image is missing a required identity or path."), TEXT("execution"), Field.Key,
					MakeShared<FJsonValueString>(FString()), MakeShared<FJsonValueString>(TEXT("nonempty string"))));
		}
		if (Format != TEXT("png") && Format != TEXT("jpg") && Format != TEXT("jpeg") && Format != TEXT("webp"))
			return Fail(OutError, Native(TEXT("capture_image_publish_request_invalid"), TEXT("The staged capture image has an unsupported format."),
				TEXT("execution"), TEXT("format"), MakeShared<FJsonValueString>(Format), MakeShared<FJsonValueString>(TEXT("png, jpg/jpeg, or webp"))));
		if (Width <= 0 || Height <= 0)
			return Fail(OutError, Native(TEXT("capture_image_publish_request_invalid"), TEXT("The staged capture image dimensions must be positive."),
				TEXT("execution"), Width <= 0 ? TEXT("width") : TEXT("height"), Number(Width <= 0 ? Width : Height), MakeShared<FJsonValueString>(TEXT("greater than zero"))));
		if (!PixelHash.StartsWith(TEXT("sha256:")))
		{
			OutError = Native(TEXT("capture_image_publish_request_invalid"), TEXT("The staged capture image has no SHA-256 pixel hash."),
				TEXT("execution"), TEXT("pixel_hash_format_valid"), MakeShared<FJsonValueBoolean>(false), MakeShared<FJsonValueBoolean>(true));
			OutError.ActualEvidence = MakeShared<FJsonValueString>(PixelHash);
			OutError.ExpectedEvidence = MakeShared<FJsonValueString>(TEXT("sha256:<digest>"));
			return false;
		}
		return true;
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
		if (OperationId.IsEmpty())
			return Fail(OutError, Native(TEXT("capture_image_stage_request_invalid"), TEXT("Image staging requires an operation identity."),
				TEXT("execution"), TEXT("operation_identity_available"), MakeShared<FJsonValueBoolean>(false), MakeShared<FJsonValueBoolean>(true)));
		if (SubjectRef.IsEmpty())
			return Fail(OutError, Native(TEXT("capture_image_stage_request_invalid"), TEXT("Image staging requires a subject reference."),
				TEXT("execution"), TEXT("subject_ref"), MakeShared<FJsonValueString>(SubjectRef), MakeShared<FJsonValueString>(TEXT("nonempty string"))));
		if (Size.X <= 0 || Size.Y <= 0)
			return Fail(OutError, Native(TEXT("capture_image_stage_request_invalid"), TEXT("Image staging requires positive image dimensions."),
				TEXT("execution"), Size.X <= 0 ? TEXT("width") : TEXT("height"), Number(Size.X <= 0 ? Size.X : Size.Y), MakeShared<FJsonValueString>(TEXT("greater than zero"))));
		if (Pixels.Num() != Size.X * Size.Y)
			return Fail(OutError, Native(TEXT("capture_image_stage_request_invalid"), TEXT("The supplied pixel count does not match the image dimensions."),
				TEXT("execution"), TEXT("pixel_count"), Number(Pixels.Num()), Number(static_cast<int64>(Size.X) * Size.Y)));
		const int64 PixelBytes = static_cast<int64>(Pixels.Num()) * sizeof(FColor);
		FUeAgentResourceChargeError ChargeError;
		if (!Resources.TryChargeMany({
			{EUeAgentResourceDimension::WorkUnits, 1},
			{EUeAgentResourceDimension::EstimatedMemoryBytes, PixelBytes}},
			TEXT("capture_image_stage_pixels"), ChargeError))
		{
			return Fail(OutError, ChargeError);
		}
		const FString Root = FPaths::ConvertRelativePathToFull(FPaths::ProjectSavedDir()
			/ TEXT("UeAgentInterface/LongRunningSession/Native") / SafeSegment(OperationId));
		const FString Native = Root / (FGuid::NewGuid().ToString(EGuidFormats::Digits) + TEXT(".png"));
		// Runtime scene pixels can carry inverse opacity, not image transparency.
		// Normalize once, and use these lossless PNG input pixels as the expected
		// image. The published file is still decoded independently by readback.
		TArray<FColor> OpaquePixels = Pixels;
		for (FColor& Pixel : OpaquePixels) Pixel.A = 255;
		UE::UAI::Contract::FDiagnostic WriteError;
		// PNG -1 selects zlib level 1 in UE; it changes size/cost, not pixel quality.
		if (!FCaptureImageProcessing::WriteCompressedImage(
			OpaquePixels, Size, TEXT("png"), -1, Native, WriteError))
		{
			IFileManager::Get().Delete(*Native, false, true, true);
			return Fail(OutError, WriteError);
		}
		const FString Final = FPaths::ConvertRelativePathToFull(FPaths::ProjectSavedDir()
			/ TEXT("UeAgentInterface/LongRunningSession/Artifacts")
			/ (FGuid::NewGuid().ToString(EGuidFormats::Digits) + TEXT(".png")));
		OutImage = FUeAgentStagedCaptureImage(SubjectRef, Native, Final, TEXT("png"),
			Size.X, Size.Y, Sha256(OpaquePixels.GetData(), PixelBytes));
		return true;
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
		if (OperationId.IsEmpty())
			return Fail(OutError, Native(TEXT("capture_image_staged_identity_invalid"), TEXT("Image adoption requires an operation identity."),
				TEXT("execution"), TEXT("operation_identity_available"), MakeShared<FJsonValueBoolean>(false), MakeShared<FJsonValueBoolean>(true)));
		if (SubjectRef.IsEmpty())
			return Fail(OutError, Native(TEXT("capture_image_staged_identity_invalid"), TEXT("Image adoption requires a subject reference."),
				TEXT("execution"), TEXT("subject_ref"), MakeShared<FJsonValueString>(SubjectRef), MakeShared<FJsonValueString>(TEXT("nonempty string"))));
		if (NativePath.IsEmpty())
			return Fail(OutError, Native(TEXT("capture_image_staged_identity_invalid"), TEXT("Image adoption requires the captured native file path."),
				TEXT("execution"), TEXT("native_path_available"), MakeShared<FJsonValueBoolean>(false), MakeShared<FJsonValueBoolean>(true)));
		if (NormalizedFormat != TEXT("png") && NormalizedFormat != TEXT("jpg") && NormalizedFormat != TEXT("jpeg") && NormalizedFormat != TEXT("webp"))
			return Fail(OutError, Native(TEXT("capture_image_staged_identity_invalid"), TEXT("Image adoption received an unsupported format."),
				TEXT("execution"), TEXT("format"), MakeShared<FJsonValueString>(Format), MakeShared<FJsonValueString>(TEXT("png, jpg/jpeg, or webp"))));
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
		if (OperationId.IsEmpty() || PublisherOwnerId.IsEmpty())
			return Fail(OutError, Native(TEXT("capture_image_publish_request_invalid"),
				OperationId.IsEmpty() ? TEXT("Image publication requires an operation identity.") : TEXT("Image publication requires an owner identity."),
				TEXT("execution"), OperationId.IsEmpty() ? TEXT("operation_identity_available") : TEXT("publisher_identity_available"),
				MakeShared<FJsonValueBoolean>(false), MakeShared<FJsonValueBoolean>(true)));
		if (!Image.TryValidate(OutError)) return false;
		FFileArtifactPublishError PublishError;
		const FString MimeType = Image.GetFormat() == TEXT("png")
			? FString(TEXT("image/png"))
			: (Image.GetFormat() == TEXT("webp")
				? FString(TEXT("image/webp")) : FString(TEXT("image/jpeg")));
		if (!TryPublishFileArtifact(FFileArtifactPublishRequest(Image.GetNativePath(), Image.GetFinalPath(),
			PublisherOwnerId, TEXT("image"), Image.GetFormat(), MimeType),
			Resources, OutArtifact, PublishError))
		{
			return Fail(OutError, PublishError);
		}
		TSharedPtr<IIndependentReadbackOracle, ESPMode::ThreadSafe> Oracle;
		FVerificationError VerifyError;
		FUeAgentCanonicalValuePtr Expected;
		TSharedPtr<FIndependentReadbackService> Readback;
		FIndependentReadbackOutcome Outcome;
		const FString OracleId = TEXT("uai.readback.capture.image/v1");
		if (!TryExpected(Image, Resources, Expected, VerifyError)
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
			if (VerifyError.IsEmpty() && (!Oracle.IsValid() || !Readback.IsValid()))
				VerifyError = Native(TEXT("capture_image_readback_failed"), TEXT("Image verification returned no required readback component."),
					TEXT("execution"), !Oracle.IsValid() ? TEXT("readback_oracle_available") : TEXT("readback_service_available"),
					MakeShared<FJsonValueBoolean>(false), MakeShared<FJsonValueBoolean>(true));
			return Fail(OutError, Outcome.GetFailureDiagnostic(VerifyError));
		}
		OutEvidenceId = Outcome.GetEvidenceId();
		if (OutEvidenceId.IsEmpty())
		{
			IFileManager::Get().Delete(*OutArtifact.GetLocator(), false, true, true);
			IFileManager::Get().Delete(
				*(OutArtifact.GetLocator() + TEXT(".uai-artifact-owner.json")),
				false, true, true);
			OutArtifact = {};
			return Fail(OutError, Native(TEXT("capture_image_readback_evidence_missing"), TEXT("Successful image verification returned no evidence identifier."),
				TEXT("execution"), TEXT("verification_evidence_available"), MakeShared<FJsonValueBoolean>(false), MakeShared<FJsonValueBoolean>(true)));
		}
		return true;
	}

	bool DiscardProducedCaptureFile(const FString& Path)
	{
		return Path.IsEmpty() || !IFileManager::Get().FileExists(*Path)
			|| IFileManager::Get().Delete(*Path, false, true, true);
	}

	bool FUeAgentCaptureImageArtifactService::DiscardStaged(
		const FUeAgentStagedCaptureImage& Image) const
	{
		return DiscardProducedCaptureFile(Image.GetNativePath());
	}

	TFuture<FPublishedCaptureImage> PublishCaptureImageAsync(
		TSharedRef<IUeAgentCaptureImageArtifactService, ESPMode::ThreadSafe> Service,
		FString OperationId, FString SubjectRef, FString PublisherOwnerId,
		TArray<FColor> Pixels, FIntPoint Size, bool bPreview)
	{
		// Module initialization belongs to the caller, before the worker uses the codec.
		FModuleManager::LoadModuleChecked<IImageWrapperModule>(TEXT("ImageWrapper"));
		return Async(EAsyncExecution::ThreadPool,
			[Service, OperationId = MoveTemp(OperationId), SubjectRef = MoveTemp(SubjectRef),
			 PublisherOwnerId = MoveTemp(PublisherOwnerId), Pixels = MoveTemp(Pixels), Size, bPreview]() mutable
		{
			const double Start = FPlatformTime::Seconds();
			FPublishedCaptureImage Result;
			FUeAgentResourceTracker Resources;
			FUeAgentStagedCaptureImage Image;
			FExecutionMutationError Error;
			FFileArtifactReference Published;
			FString Evidence;
			FUeAgentCanonicalValuePtr Value;
			FFileArtifactPublishError ProjectionError;
			if (!Service->TryStagePngPixels(OperationId, SubjectRef, Pixels, Size, Resources, Image, Error)
				|| !Service->TryPublishVerified(OperationId, PublisherOwnerId, Image, Resources, Published, Evidence, Error))
				Result.Error = Error;
			else if (!TryProjectFileArtifactReference(Published, Resources, Value, ProjectionError))
                Result.Error = ProjectionError;
            else CanonicalJson::TryToJsonValue(*Value, Result.Artifact, Result.Error);
			if (!Service->DiscardStaged(Image))
				Result.CleanupError = UE::UAI::Contract::FDiagnostic::Failure(TEXT("capture_staged_cleanup_failed"), TEXT("The staged capture file could not be removed."), TEXT("cleanup"), Image.GetNativePath());
			if (bPreview && Result.Error.IsEmpty())
				FCaptureImageProcessing::ResizePixelsMaxSize(Pixels, Size, 320,
					Result.Preview, Result.PreviewSize, Result.PreviewError);
			const auto Usage = Resources.Snapshot();
			for (uint8 Index = 0; Index <= static_cast<uint8>(EUeAgentResourceDimension::EstimatedMemoryBytes); ++Index)
			{
				const auto Dimension = static_cast<EUeAgentResourceDimension>(Index);
				Result.Charges.Add(Dimension, Usage.Consumed(Dimension));
			}
			Result.ProcessingSeconds = FPlatformTime::Seconds() - Start;
			return Result;
		});
	}
}
