#include "DomainAdapters/CaptureMediaLongJob/UeAgentCaptureMediaReadback.h"

#include "Engine/TextureRenderTarget2D.h"
#include "HAL/FileManager.h"
#include "ImageCore.h"
#include "ImageUtils.h"
#include "Misc/App.h"
#include "Misc/FileHelper.h"
#include "RHI.h"
#include "RHIGlobals.h"
#include "RenderingThread.h"

#define UI OpenSslUi
#include <openssl/sha.h>
#undef UI

namespace UE::UAI::DomainAdapters::CaptureMediaLongJob
{
	using namespace ArtifactPublisher;
	using namespace ContextResource;
	using namespace StateSemantic;
	using namespace Verification;

	namespace
	{
		FString NormalizeText(FString Value)
		{
			Value.TrimStartAndEndInline();
			return Value;
		}

		bool IsOracleId(const FString& Value)
		{
			return !Value.IsEmpty()
				&& Value == NormalizeText(Value)
				&& Value.StartsWith(TEXT("uai.readback.capture."));
		}

		FString Sha256(const void* Data, const int64 NumBytes)
		{
			uint8 Digest[SHA256_DIGEST_LENGTH] = {};
			const uint8 Empty = 0;
			SHA256(
				NumBytes > 0 ? static_cast<const uint8*>(Data) : &Empty,
				static_cast<size_t>(FMath::Max<int64>(0, NumBytes)),
				Digest);
			FString Hex;
			Hex.Reserve(SHA256_DIGEST_LENGTH * 2);
			for (const uint8 Byte : Digest)
			{
				Hex += FString::Printf(TEXT("%02x"), Byte);
			}
			return TEXT("sha256:") + Hex;
		}

		FString EvidenceId(
			const FIndependentReadbackRequest& Request,
			const FString& ObservationHash)
		{
			const FString Text = Request.GetOperationId()
				+ TEXT("\n") + Request.GetOracleId()
				+ TEXT("\n") + Request.GetSubjectRef()
				+ TEXT("\n") + ObservationHash;
			const FTCHARToUTF8 Utf8(*Text);
			return TEXT("readback:") + Sha256(Utf8.Get(), Utf8.Length());
		}

		bool ChargeReadback(
			const int64 Bytes,
			const int64 Files,
			const FString& Path,
			FUeAgentResourceTracker& Resources,
			FVerificationError& OutError)
		{
			FUeAgentResourceBudgetExceeded BudgetError;
			if (!Resources.TryChargeMany(
				{
					{EUeAgentResourceDimension::Files, Files},
					{EUeAgentResourceDimension::InputBytes, Bytes},
					{EUeAgentResourceDimension::WorkUnits,
						FMath::Max<int64>(1, Bytes)},
					{EUeAgentResourceDimension::EstimatedMemoryBytes, Bytes},
				},
				Path,
				BudgetError))
			{
				OutError = FVerificationError(
					BudgetError.Code,
					BudgetError.Path);
				return false;
			}
			return true;
		}

		bool StringValue(
			const FString& Value,
			FUeAgentResourceTracker& Resources,
			FUeAgentCanonicalValuePtr& OutValue,
			FVerificationError& OutError)
		{
			FUeAgentStateSemanticError Error;
			if (!FUeAgentCanonicalValue::TryString(
				Value, Resources, OutValue, Error))
			{
				OutError = FVerificationError(Error.Code, Error.Path);
				return false;
			}
			return true;
		}

		bool IntegerValue(
			const int64 Value,
			FUeAgentResourceTracker& Resources,
			FUeAgentCanonicalValuePtr& OutValue,
			FVerificationError& OutError)
		{
			FUeAgentStateSemanticError Error;
			if (!FUeAgentCanonicalValue::TryInteger(
				FString::Printf(TEXT("%lld"), Value),
				Resources,
				OutValue,
				Error))
			{
				OutError = FVerificationError(Error.Code, Error.Path);
				return false;
			}
			return true;
		}

		bool BuildObject(
			const TArray<TPair<FString, FString>>& StringFields,
			const TArray<TPair<FString, int64>>& IntegerFields,
			FUeAgentResourceTracker& Resources,
			FUeAgentCanonicalValuePtr& OutValue,
			FVerificationError& OutError)
		{
			TArray<TPair<FString, FUeAgentCanonicalValueRef>> Items;
			for (const TPair<FString, FString>& Field : StringFields)
			{
				FUeAgentCanonicalValuePtr Value;
				if (!StringValue(Field.Value, Resources, Value, OutError))
				{
					return false;
				}
				Items.Emplace(Field.Key, Value.ToSharedRef());
			}
			for (const TPair<FString, int64>& Field : IntegerFields)
			{
				FUeAgentCanonicalValuePtr Value;
				if (!IntegerValue(Field.Value, Resources, Value, OutError))
				{
					return false;
				}
				Items.Emplace(Field.Key, Value.ToSharedRef());
			}
			FUeAgentCanonicalValuePtr Object;
			FUeAgentStateSemanticError Error;
			if (!FUeAgentCanonicalValue::TryObject(
				Items, Resources, Object, Error))
			{
				OutError = FVerificationError(Error.Code, Error.Path);
				return false;
			}
			OutValue = MoveTemp(Object);
			return true;
		}

		class FCaptureReadbackOracleBase
			: public IIndependentReadbackOracle
		{
		public:
			FCaptureReadbackOracleBase(FString InOracleId, FString InSubjectRef)
				: OracleId(NormalizeText(MoveTemp(InOracleId)))
				, SubjectRef(NormalizeText(MoveTemp(InSubjectRef)))
			{
			}

			const FString& GetOracleId() const override
			{
				return OracleId;
			}

			bool ValidateRequest(
				const FIndependentReadbackRequest& Request,
				FVerificationError& OutError) const
			{
				if (Request.GetOracleId() != OracleId
					|| Request.GetSubjectRef() != SubjectRef)
				{
					OutError = FVerificationError(
						TEXT("capture_readback_binding_mismatch"));
					return false;
				}
				return true;
			}

		protected:
			FString OracleId;
			FString SubjectRef;
		};

		class FImageArtifactReadbackOracle final
			: public FCaptureReadbackOracleBase
		{
		public:
			using FCaptureReadbackOracleBase::FCaptureReadbackOracleBase;

			bool TryRead(
				const FIndependentReadbackRequest& Request,
				FUeAgentResourceTracker& Resources,
				FIndependentReadbackObservation& OutObservation,
				FVerificationError& OutError) override
			{
				if (!ValidateRequest(Request, OutError)
					|| !Request.GetArtifact().IsSet()
					|| Request.GetArtifact()->GetArtifactKind() != TEXT("image"))
				{
					if (!OutError.IsPresent())
					{
						OutError = FVerificationError(
							TEXT("capture_image_artifact_required"));
					}
					return false;
				}
				const FFileArtifactReference& Artifact =
					Request.GetArtifact().GetValue();
				const int64 FileSize = IFileManager::Get().FileSize(
					*Artifact.GetLocator());
				if (FileSize < 0
					|| !ChargeReadback(
						FileSize,
						1,
						TEXT("capture_image_readback"),
						Resources,
						OutError))
				{
					if (!OutError.IsPresent())
					{
						OutError = FVerificationError(
							TEXT("capture_image_artifact_unavailable"));
					}
					return false;
				}
				FImage Image;
				if (!FImageUtils::LoadImage(*Artifact.GetLocator(), Image))
				{
					OutObservation = FIndependentReadbackObservation::Unavailable(
						TEXT("capture_image_decode_unavailable"));
					return true;
				}
				Image.ChangeFormat(ERawImageFormat::BGRA8, EGammaSpace::sRGB);
				const FString PixelHash = Sha256(
					Image.RawData.GetData(), Image.RawData.Num());
				FUeAgentCanonicalValuePtr Actual;
				if (!BuildObject(
					{
						{TEXT("subject_ref"), SubjectRef},
						{TEXT("pixel_sha256"), PixelHash},
					},
					{
						{TEXT("width"), Image.SizeX},
						{TEXT("height"), Image.SizeY},
						{TEXT("pixel_count"), Image.GetNumPixels()},
					},
					Resources,
					Actual,
					OutError))
				{
					return false;
				}
				OutObservation = FIndependentReadbackObservation::Available(
					Actual.ToSharedRef(), EvidenceId(Request, PixelHash));
				return true;
			}
		};

		bool ReadU16(
			const TArray<uint8>& Bytes,
			const int64 Offset,
			uint16& OutValue)
		{
			if (Offset < 0 || Offset + 2 > Bytes.Num())
			{
				return false;
			}
			OutValue = static_cast<uint16>(Bytes[Offset])
				| (static_cast<uint16>(Bytes[Offset + 1]) << 8);
			return true;
		}

		bool ReadU32(
			const TArray<uint8>& Bytes,
			const int64 Offset,
			uint32& OutValue)
		{
			if (Offset < 0 || Offset + 4 > Bytes.Num())
			{
				return false;
			}
			OutValue = static_cast<uint32>(Bytes[Offset])
				| (static_cast<uint32>(Bytes[Offset + 1]) << 8)
				| (static_cast<uint32>(Bytes[Offset + 2]) << 16)
				| (static_cast<uint32>(Bytes[Offset + 3]) << 24);
			return true;
		}

		bool HasFourCc(
			const TArray<uint8>& Bytes,
			const int64 Offset,
			const ANSICHAR* Value)
		{
			return Offset >= 0 && Offset + 4 <= Bytes.Num()
				&& Bytes[Offset] == static_cast<uint8>(Value[0])
				&& Bytes[Offset + 1] == static_cast<uint8>(Value[1])
				&& Bytes[Offset + 2] == static_cast<uint8>(Value[2])
				&& Bytes[Offset + 3] == static_cast<uint8>(Value[3]);
		}

		class FWaveArtifactReadbackOracle final
			: public FCaptureReadbackOracleBase
		{
		public:
			using FCaptureReadbackOracleBase::FCaptureReadbackOracleBase;

			bool TryRead(
				const FIndependentReadbackRequest& Request,
				FUeAgentResourceTracker& Resources,
				FIndependentReadbackObservation& OutObservation,
				FVerificationError& OutError) override
			{
				if (!ValidateRequest(Request, OutError)
					|| !Request.GetArtifact().IsSet()
					|| Request.GetArtifact()->GetArtifactKind() != TEXT("media"))
				{
					if (!OutError.IsPresent())
					{
						OutError = FVerificationError(
							TEXT("capture_media_artifact_required"));
					}
					return false;
				}
				const FFileArtifactReference& Artifact =
					Request.GetArtifact().GetValue();
				TArray<uint8> Bytes;
				if (!FFileHelper::LoadFileToArray(Bytes, *Artifact.GetLocator())
					|| !ChargeReadback(
						Bytes.Num(),
						1,
						TEXT("capture_media_readback"),
						Resources,
						OutError))
				{
					if (!OutError.IsPresent())
					{
						OutError = FVerificationError(
							TEXT("capture_media_artifact_unavailable"));
					}
					return false;
				}
				uint16 AudioFormat = 0;
				uint16 Channels = 0;
				uint16 BlockAlign = 0;
				uint32 SampleRate = 0;
				uint32 DataSize = 0;
				bool bFmt = false;
				bool bData = false;
				if (!HasFourCc(Bytes, 0, "RIFF")
					|| !HasFourCc(Bytes, 8, "WAVE"))
				{
					OutObservation = FIndependentReadbackObservation::Unavailable(
						TEXT("capture_media_container_invalid"));
					return true;
				}
				for (int64 Offset = 12; Offset + 8 <= Bytes.Num();)
				{
					uint32 ChunkSize = 0;
					if (!ReadU32(Bytes, Offset + 4, ChunkSize)
						|| Offset + 8 + ChunkSize > Bytes.Num())
					{
						break;
					}
					if (HasFourCc(Bytes, Offset, "fmt ") && ChunkSize >= 16)
					{
						bFmt = ReadU16(Bytes, Offset + 8, AudioFormat)
							&& ReadU16(Bytes, Offset + 10, Channels)
							&& ReadU32(Bytes, Offset + 12, SampleRate)
							&& ReadU16(Bytes, Offset + 20, BlockAlign);
					}
					else if (HasFourCc(Bytes, Offset, "data"))
					{
						DataSize = ChunkSize;
						bData = true;
					}
					Offset += 8 + ChunkSize + (ChunkSize & 1u);
				}
				if (!bFmt || !bData || AudioFormat != 1
					|| Channels == 0 || SampleRate == 0 || BlockAlign == 0
					|| DataSize % BlockAlign != 0)
				{
					OutObservation = FIndependentReadbackObservation::Unavailable(
						TEXT("capture_media_stream_invalid"));
					return true;
				}
				const FString ActualArtifactHash = Sha256(
					Bytes.GetData(), Bytes.Num());
				FUeAgentCanonicalValuePtr Actual;
				if (!BuildObject(
					{
						{TEXT("subject_ref"), SubjectRef},
						{TEXT("container"), TEXT("wav")},
						{TEXT("artifact_sha256"),
							ActualArtifactHash},
					},
					{
						{TEXT("channels"), Channels},
						{TEXT("sample_rate"), SampleRate},
						{TEXT("frame_count"), DataSize / BlockAlign},
					},
					Resources,
					Actual,
					OutError))
				{
					return false;
				}
				OutObservation = FIndependentReadbackObservation::Available(
					Actual.ToSharedRef(),
					EvidenceId(Request, ActualArtifactHash));
				return true;
			}
		};

		class FRenderTargetReadbackOracle final
			: public FCaptureReadbackOracleBase
		{
		public:
			FRenderTargetReadbackOracle(
				FString InOracleId,
				FString InSubjectRef,
				UTextureRenderTarget2D* InRenderTarget)
				: FCaptureReadbackOracleBase(
					MoveTemp(InOracleId), MoveTemp(InSubjectRef))
				, RenderTarget(InRenderTarget)
			{
			}

			bool TryRead(
				const FIndependentReadbackRequest& Request,
				FUeAgentResourceTracker& Resources,
				FIndependentReadbackObservation& OutObservation,
				FVerificationError& OutError) override
			{
				if (!ValidateRequest(Request, OutError))
				{
					return false;
				}
				if (!FApp::CanEverRender() || GUsingNullRHI || GDynamicRHI == nullptr)
				{
					OutObservation = FIndependentReadbackObservation::Unavailable(
						TEXT("capture_render_readback_rhi_unavailable"));
					return true;
				}
				if (!IsInGameThread())
				{
					OutObservation = FIndependentReadbackObservation::Unavailable(
						TEXT("capture_render_readback_game_thread_required"));
					return true;
				}
				UTextureRenderTarget2D* Target = RenderTarget.Get();
				if (Target == nullptr)
				{
					OutObservation = FIndependentReadbackObservation::Unavailable(
						TEXT("capture_render_readback_target_unavailable"));
					return true;
				}
				FTextureRenderTargetResource* Resource =
					Target->GameThread_GetRenderTargetResource();
				if (Resource == nullptr)
				{
					OutObservation = FIndependentReadbackObservation::Unavailable(
						TEXT("capture_render_readback_resource_unavailable"));
					return true;
				}
				FlushRenderingCommands();
				TArray<FColor> Pixels;
				FReadSurfaceDataFlags Flags(RCM_UNorm);
				if (!Resource->ReadPixels(Pixels, Flags)
					|| Pixels.Num() <= 0)
				{
					OutObservation = FIndependentReadbackObservation::Unavailable(
						TEXT("capture_render_readback_pixels_unavailable"));
					return true;
				}
				const int64 PixelBytes = Pixels.Num() * sizeof(FColor);
				if (!ChargeReadback(
					PixelBytes,
					0,
					TEXT("capture_render_readback"),
					Resources,
					OutError))
				{
					return false;
				}
				const FString PixelHash = Sha256(Pixels.GetData(), PixelBytes);
				FUeAgentCanonicalValuePtr Actual;
				if (!BuildObject(
					{
						{TEXT("subject_ref"), SubjectRef},
						{TEXT("pixel_sha256"), PixelHash},
					},
					{
						{TEXT("width"), Target->SizeX},
						{TEXT("height"), Target->SizeY},
						{TEXT("pixel_count"), Pixels.Num()},
					},
					Resources,
					Actual,
					OutError))
				{
					return false;
				}
				OutObservation = FIndependentReadbackObservation::Available(
					Actual.ToSharedRef(), EvidenceId(Request, PixelHash));
				return true;
			}

		private:
			TWeakObjectPtr<UTextureRenderTarget2D> RenderTarget;
		};
	}

	bool TryCreateCaptureImageArtifactReadbackOracle(
		FString OracleId,
		FString SubjectRef,
		TSharedPtr<IIndependentReadbackOracle, ESPMode::ThreadSafe>& OutOracle,
		FVerificationError& OutError)
	{
		OutOracle.Reset();
		OracleId = NormalizeText(MoveTemp(OracleId));
		SubjectRef = NormalizeText(MoveTemp(SubjectRef));
		if (!IsOracleId(OracleId) || SubjectRef.IsEmpty())
		{
			OutError = FVerificationError(
				TEXT("capture_image_readback_oracle_invalid"));
			return false;
		}
		OutOracle = MakeShared<FImageArtifactReadbackOracle,
			ESPMode::ThreadSafe>(MoveTemp(OracleId), MoveTemp(SubjectRef));
		return true;
	}

	bool TryCreateCaptureWaveArtifactReadbackOracle(
		FString OracleId,
		FString SubjectRef,
		TSharedPtr<IIndependentReadbackOracle, ESPMode::ThreadSafe>& OutOracle,
		FVerificationError& OutError)
	{
		OutOracle.Reset();
		OracleId = NormalizeText(MoveTemp(OracleId));
		SubjectRef = NormalizeText(MoveTemp(SubjectRef));
		if (!IsOracleId(OracleId) || SubjectRef.IsEmpty())
		{
			OutError = FVerificationError(
				TEXT("capture_media_readback_oracle_invalid"));
			return false;
		}
		OutOracle = MakeShared<FWaveArtifactReadbackOracle,
			ESPMode::ThreadSafe>(MoveTemp(OracleId), MoveTemp(SubjectRef));
		return true;
	}

	bool TryCreateCaptureRenderTargetReadbackOracle(
		FString OracleId,
		FString SubjectRef,
		UTextureRenderTarget2D* RenderTarget,
		TSharedPtr<IIndependentReadbackOracle, ESPMode::ThreadSafe>& OutOracle,
		FVerificationError& OutError)
	{
		OutOracle.Reset();
		OracleId = NormalizeText(MoveTemp(OracleId));
		SubjectRef = NormalizeText(MoveTemp(SubjectRef));
		if (!IsOracleId(OracleId) || SubjectRef.IsEmpty()
			|| RenderTarget == nullptr)
		{
			OutError = FVerificationError(
				TEXT("capture_render_readback_oracle_invalid"));
			return false;
		}
		OutOracle = MakeShared<FRenderTargetReadbackOracle,
			ESPMode::ThreadSafe>(
			MoveTemp(OracleId), MoveTemp(SubjectRef), RenderTarget);
		return true;
	}
}
