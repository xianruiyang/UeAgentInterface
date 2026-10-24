#include "Misc/AutomationTest.h"

#include "Core/ContextResource/UeAgentContextResource.h"
#include "Core/StateSemantic/UeAgentStateSemantic.h"
#include "Editor.h"
#include "Engine/TextureRenderTarget2D.h"
#include "HAL/FileManager.h"
#include "ImageCore.h"
#include "ImageUtils.h"
#include "Kismet/KismetRenderingLibrary.h"
#include "Misc/App.h"
#include "Misc/FileHelper.h"
#include "Misc/Guid.h"
#include "Misc/Paths.h"
#include "RenderingThread.h"

#define UI OpenSslUi
#include <openssl/sha.h>
#undef UI

#include "Core/ArtifactPublisher/UeAgentFileArtifactPublisher.h"
#include "Core/Verification/UeAgentIndependentReadback.h"
#include "DomainAdapters/CaptureMediaLongJob/UeAgentCaptureMediaReadback.h"

namespace
{
	using namespace UE::UAI::ArtifactPublisher;
	using namespace UE::UAI::ContextResource;
	using namespace UE::UAI::DomainAdapters::CaptureMediaLongJob;
	using namespace UE::UAI::StateSemantic;
	using namespace UE::UAI::Verification;

	FUeAgentResourceTracker MakeResources()
	{
		return FUeAgentResourceTracker();
	}

	class FScopedReadbackRoot final
	{
	public:
		FScopedReadbackRoot()
			: Root(FPaths::ConvertRelativePathToFull(FPaths::Combine(
				FPaths::ProjectIntermediateDir(),
				TEXT("UeAgentInterfaceTests"),
				TEXT("LS012Readback"),
				FGuid::NewGuid().ToString(EGuidFormats::Digits))))
		{
			IFileManager::Get().MakeDirectory(*Root, true);
		}

		~FScopedReadbackRoot()
		{
			IFileManager::Get().DeleteDirectory(*Root, false, true);
		}

		FString Path(const FString& Leaf) const
		{
			return Root / Leaf;
		}

	private:
		FString Root;
	};

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

	FUeAgentCanonicalValueRef StringValue(
		const FString& Value,
		FUeAgentResourceTracker& Resources)
	{
		FUeAgentCanonicalValuePtr Result;
		FUeAgentStateSemanticError Error;
		check(FUeAgentCanonicalValue::TryString(Value, Resources, Result, Error));
		return Result.ToSharedRef();
	}

	FUeAgentCanonicalValueRef IntegerValue(
		const int64 Value,
		FUeAgentResourceTracker& Resources)
	{
		FUeAgentCanonicalValuePtr Result;
		FUeAgentStateSemanticError Error;
		check(FUeAgentCanonicalValue::TryInteger(
			FString::Printf(TEXT("%lld"), Value), Resources, Result, Error));
		return Result.ToSharedRef();
	}

	FUeAgentCanonicalValueRef ObjectValue(
		TArray<TPair<FString, FUeAgentCanonicalValueRef>> Items,
		FUeAgentResourceTracker& Resources)
	{
		FUeAgentCanonicalValuePtr Result;
		FUeAgentStateSemanticError Error;
		check(FUeAgentCanonicalValue::TryObject(
			Items, Resources, Result, Error));
		return Result.ToSharedRef();
	}

	FUeAgentCanonicalValueRef PixelExpected(
		const FString& SubjectRef,
		const int32 Width,
		const int32 Height,
		const FString& PixelHash,
		FUeAgentResourceTracker& Resources)
	{
		return ObjectValue({
			{TEXT("subject_ref"), StringValue(SubjectRef, Resources)},
			{TEXT("width"), IntegerValue(Width, Resources)},
			{TEXT("height"), IntegerValue(Height, Resources)},
			{TEXT("pixel_count"),
				IntegerValue(static_cast<int64>(Width) * Height, Resources)},
			{TEXT("pixel_sha256"), StringValue(PixelHash, Resources)},
		}, Resources);
	}

	void AppendU16(TArray<uint8>& Bytes, const uint16 Value)
	{
		Bytes.Add(static_cast<uint8>(Value & 0xff));
		Bytes.Add(static_cast<uint8>((Value >> 8) & 0xff));
	}

	void AppendU32(TArray<uint8>& Bytes, const uint32 Value)
	{
		Bytes.Add(static_cast<uint8>(Value & 0xff));
		Bytes.Add(static_cast<uint8>((Value >> 8) & 0xff));
		Bytes.Add(static_cast<uint8>((Value >> 16) & 0xff));
		Bytes.Add(static_cast<uint8>((Value >> 24) & 0xff));
	}

	void AppendAscii(TArray<uint8>& Bytes, const ANSICHAR* Text, const int32 Count)
	{
		for (int32 Index = 0; Index < Count; ++Index)
		{
			Bytes.Add(static_cast<uint8>(Text[Index]));
		}
	}

	TArray<uint8> MakePcmWave()
	{
		constexpr uint16 Channels = 1;
		constexpr uint32 SampleRate = 8000;
		constexpr uint16 BitsPerSample = 16;
		const TArray<int16> Samples = {0, 1024, -1024, 2048};
		const uint32 DataBytes = Samples.Num() * sizeof(int16);
		TArray<uint8> Bytes;
		AppendAscii(Bytes, "RIFF", 4);
		AppendU32(Bytes, 36 + DataBytes);
		AppendAscii(Bytes, "WAVE", 4);
		AppendAscii(Bytes, "fmt ", 4);
		AppendU32(Bytes, 16);
		AppendU16(Bytes, 1);
		AppendU16(Bytes, Channels);
		AppendU32(Bytes, SampleRate);
		AppendU32(Bytes, SampleRate * Channels * BitsPerSample / 8);
		AppendU16(Bytes, Channels * BitsPerSample / 8);
		AppendU16(Bytes, BitsPerSample);
		AppendAscii(Bytes, "data", 4);
		AppendU32(Bytes, DataBytes);
		for (const int16 Sample : Samples)
		{
			AppendU16(Bytes, static_cast<uint16>(Sample));
		}
		return Bytes;
	}

	bool Publish(
		const FString& Source,
		const FString& Final,
		const FString& Kind,
		const FString& Format,
		const FString& Mime,
		FUeAgentResourceTracker& Resources,
		FFileArtifactReference& OutArtifact)
	{
		FFileArtifactPublishError Error;
		return TryPublishFileArtifact(
			FFileArtifactPublishRequest(
				Source,
				Final,
				TEXT("operation:ls012:readback"),
				Kind,
				Format,
				Mime),
			Resources,
			OutArtifact,
			Error);
	}

	TSharedPtr<FIndependentReadbackService> MakeService(
		const TArray<TSharedRef<IIndependentReadbackOracle, ESPMode::ThreadSafe>>&
			Oracles,
		FVerificationError& OutError)
	{
		TSharedPtr<FIndependentReadbackService> Service;
		FIndependentReadbackService::TryCreate(Oracles, Service, OutError);
		return Service;
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUeAgentCaptureImageMediaReadbackTest,
	"UeAgentInterface.DomainAdapters.CaptureMediaLongJob.Readback.E2."
		"ImageAndMediaIndependentReadback",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FUeAgentCaptureImageMediaReadbackTest::RunTest(
	const FString& Parameters)
{
	FScopedReadbackRoot Files;
	FUeAgentResourceTracker Resources = MakeResources();

	FImage Image;
	Image.Init(2, 2, 1, ERawImageFormat::BGRA8, EGammaSpace::sRGB);
	TArrayView64<FColor> Colors = Image.AsBGRA8();
	Colors[0] = FColor::Red;
	Colors[1] = FColor::Green;
	Colors[2] = FColor::Blue;
	Colors[3] = FColor::White;
	const FString ImageSource = Files.Path(TEXT("native/image.png"));
	IFileManager::Get().MakeDirectory(*FPaths::GetPath(ImageSource), true);
	TestTrue(TEXT("image native source is encoded"),
		FImageUtils::SaveImageByExtension(*ImageSource, Image));
	FFileArtifactReference ImageArtifact;
	TestTrue(TEXT("image source is published through C8F"),
		Publish(
			ImageSource,
			Files.Path(TEXT("final/image.png")),
			TEXT("image"),
			TEXT("png"),
			TEXT("image/png"),
			Resources,
			ImageArtifact));

	const TArray<uint8> WaveBytes = MakePcmWave();
	const FString WaveSource = Files.Path(TEXT("native/audio.wav"));
	TestTrue(TEXT("media native source is written"),
		FFileHelper::SaveArrayToFile(WaveBytes, *WaveSource));
	FFileArtifactReference WaveArtifact;
	TestTrue(TEXT("media source is published through C8F"),
		Publish(
			WaveSource,
			Files.Path(TEXT("final/audio.wav")),
			TEXT("media"),
			TEXT("wav"),
			TEXT("audio/wav"),
			Resources,
			WaveArtifact));

	TSharedPtr<IIndependentReadbackOracle, ESPMode::ThreadSafe> ImageOracle;
	TSharedPtr<IIndependentReadbackOracle, ESPMode::ThreadSafe> MediaOracle;
	FVerificationError Error;
	TestTrue(TEXT("image pixel readback oracle constructs"),
		TryCreateCaptureImageArtifactReadbackOracle(
			TEXT("uai.readback.capture.image/v1"),
			TEXT("capture-subject:image:ls012"),
			ImageOracle,
			Error));
	TestTrue(TEXT("media container readback oracle constructs"),
		TryCreateCaptureWaveArtifactReadbackOracle(
			TEXT("uai.readback.capture.media/v1"),
			TEXT("media-source:submix:ls012"),
			MediaOracle,
			Error));
	const TSharedPtr<FIndependentReadbackService> Service = MakeService(
		{ImageOracle.ToSharedRef(), MediaOracle.ToSharedRef()}, Error);
	TestTrue(TEXT("C9 exact oracle catalog constructs"), Service.IsValid());
	if (!Service.IsValid())
	{
		return false;
	}

	const FString ImagePixelHash = Sha256(
		Image.RawData.GetData(), Image.RawData.Num());
	FIndependentReadbackOutcome Outcome;
	const bool bImageVerified = Service->TryVerify(
			FIndependentReadbackRequest(
				TEXT("operation:ls012:image-readback"),
				TEXT("uai.readback.capture.image/v1"),
				TEXT("capture-subject:image:ls012"),
				PixelExpected(
					TEXT("capture-subject:image:ls012"),
					2,
					2,
					ImagePixelHash,
					Resources),
				ImageArtifact),
			Resources,
			Outcome,
			Error);
	if (!bImageVerified)
	{
		AddError(FString::Printf(
			TEXT("image_readback_error:%s:%s"),
			*Error.GetCode(),
			*Error.GetDetail()));
	}
	TestTrue(TEXT("C9 reopens and decodes published image"), bImageVerified);
	TestEqual(TEXT("image pixels verify complete"), Outcome.GetStatus(),
		EVerificationStatus::Verified);
	TestEqual(TEXT("image readback produces business complete"),
		Outcome.GetEffectOutcome(), EEffectOutcome::Complete);

	const FUeAgentCanonicalValueRef MediaExpected = ObjectValue({
		{TEXT("subject_ref"),
			StringValue(TEXT("media-source:submix:ls012"), Resources)},
		{TEXT("container"), StringValue(TEXT("wav"), Resources)},
		{TEXT("channels"), IntegerValue(1, Resources)},
		{TEXT("sample_rate"), IntegerValue(8000, Resources)},
		{TEXT("frame_count"), IntegerValue(4, Resources)},
		{TEXT("artifact_sha256"),
			StringValue(WaveArtifact.GetContentHash(), Resources)},
	}, Resources);
	const bool bMediaVerified = Service->TryVerify(
			FIndependentReadbackRequest(
				TEXT("operation:ls012:media-readback"),
				TEXT("uai.readback.capture.media/v1"),
				TEXT("media-source:submix:ls012"),
				MediaExpected,
				WaveArtifact),
			Resources,
			Outcome,
			Error);
	if (!bMediaVerified)
	{
		AddError(FString::Printf(
			TEXT("media_readback_error:%s:%s"),
			*Error.GetCode(),
			*Error.GetDetail()));
	}
	TestTrue(TEXT("C9 reopens and parses published WAV"), bMediaVerified);
	TestEqual(TEXT("media container verifies complete"), Outcome.GetStatus(),
		EVerificationStatus::Verified);

	TArray<uint8> TamperedWave = WaveBytes;
	TamperedWave.Last() ^= 0x01;
	TestTrue(TEXT("published WAV can be externally tampered for oracle proof"),
		FFileHelper::SaveArrayToFile(
			TamperedWave, *WaveArtifact.GetLocator()));
	TestTrue(TEXT("C9 independently re-hashes published WAV"),
		Service->TryVerify(
			FIndependentReadbackRequest(
				TEXT("operation:ls012:media-readback-tampered"),
				TEXT("uai.readback.capture.media/v1"),
				TEXT("media-source:submix:ls012"),
				MediaExpected,
				WaveArtifact),
			Resources,
			Outcome,
			Error));
	TestEqual(TEXT("tampered media is a semantic mismatch"),
		Outcome.GetStatus(), EVerificationStatus::Mismatch);
	TestEqual(TEXT("tampered media cannot remain complete"),
		Outcome.GetEffectOutcome(), EEffectOutcome::Partial);

	TestFalse(TEXT("unknown oracle suffix fails exact lookup"),
		Service->TryVerify(
			FIndependentReadbackRequest(
				TEXT("operation:ls012:unknown-oracle"),
				TEXT("uai.readback.capture.media/v2"),
				TEXT("media-source:submix:ls012"),
				MediaExpected,
				WaveArtifact),
			Resources,
			Outcome,
			Error));
	TestEqual(TEXT("unknown oracle has stable error"), Error.GetCode(),
		FString(TEXT("independent_readback_oracle_not_registered")));
	return !HasAnyErrors();
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUeAgentCaptureRenderNullRhiTest,
	"UeAgentInterface.DomainAdapters.CaptureMediaLongJob.Readback.E2."
		"RenderTargetNullRhiFailsClosed",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FUeAgentCaptureRenderNullRhiTest::RunTest(
	const FString& Parameters)
{
	if (FApp::CanEverRender())
	{
		AddError(TEXT("null_rhi_fixture_requires_non_rendering_worker"));
		return false;
	}
	UTextureRenderTarget2D* RenderTarget =
		NewObject<UTextureRenderTarget2D>(GetTransientPackage());
	TSharedPtr<IIndependentReadbackOracle, ESPMode::ThreadSafe> Oracle;
	FVerificationError Error;
	TestTrue(TEXT("render readback oracle binds exact target"),
		TryCreateCaptureRenderTargetReadbackOracle(
			TEXT("uai.readback.capture.render_target/v1"),
			TEXT("render-resource:transient:ls012-nullrhi"),
			RenderTarget,
			Oracle,
			Error));
	const TSharedPtr<FIndependentReadbackService> Service =
		MakeService({Oracle.ToSharedRef()}, Error);
	FUeAgentResourceTracker Resources = MakeResources();
	FIndependentReadbackOutcome Outcome;
	TestTrue(TEXT("C9 returns an explicit unavailable outcome"),
		Service->TryVerify(
			FIndependentReadbackRequest(
				TEXT("operation:ls012:render-nullrhi"),
				TEXT("uai.readback.capture.render_target/v1"),
				TEXT("render-resource:transient:ls012-nullrhi"),
				PixelExpected(
					TEXT("render-resource:transient:ls012-nullrhi"),
					8,
					8,
					TEXT("sha256:00000000000000000000000000000000"
						"00000000000000000000000000000000"),
					Resources)),
			Resources,
			Outcome,
			Error));
	TestEqual(TEXT("NullRHI readback is unavailable"), Outcome.GetStatus(),
		EVerificationStatus::Unavailable);
	TestEqual(TEXT("NullRHI cannot claim business completion"),
		Outcome.GetEffectOutcome(), EEffectOutcome::StateUnknown);
	TestEqual(TEXT("NullRHI has stable diagnostic"),
		Outcome.GetDiagnosticCode(),
		FString(TEXT("capture_render_readback_rhi_unavailable")));
	return !HasAnyErrors();
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUeAgentCaptureRenderGpuReadbackTest,
	"UeAgentInterface.DomainAdapters.CaptureMediaLongJob.Readback.E2Rhi."
		"RenderTargetGpuReadback",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FUeAgentCaptureRenderGpuReadbackTest::RunTest(
	const FString& Parameters)
{
	if (!FApp::CanEverRender())
	{
		AddError(TEXT("render_target_gpu_readback_requires_rhi_worker"));
		return false;
	}
	UWorld* World = GEditor ? GEditor->GetEditorWorldContext().World() : nullptr;
	if (!World)
	{
		AddError(TEXT("render_target_gpu_readback_world_missing"));
		return false;
	}
	UTextureRenderTarget2D* RenderTarget =
		NewObject<UTextureRenderTarget2D>(GetTransientPackage());
	RenderTarget->AddToRoot();
	RenderTarget->ClearColor = FLinearColor::Red;
	RenderTarget->InitCustomFormat(8, 8, PF_B8G8R8A8, false);
	RenderTarget->UpdateResourceImmediate(true);
	UKismetRenderingLibrary::ClearRenderTarget2D(
		World, RenderTarget, FLinearColor::Red);
	FlushRenderingCommands();

	TArray<FColor> ExpectedPixels;
	ExpectedPixels.Init(FColor::Red, 64);
	const FString ExpectedHash = Sha256(
		ExpectedPixels.GetData(), ExpectedPixels.Num() * sizeof(FColor));
	TSharedPtr<IIndependentReadbackOracle, ESPMode::ThreadSafe> Oracle;
	FVerificationError Error;
	TestTrue(TEXT("render GPU readback oracle binds exact target"),
		TryCreateCaptureRenderTargetReadbackOracle(
			TEXT("uai.readback.capture.render_target/v1"),
			TEXT("render-resource:transient:ls012-rhi"),
			RenderTarget,
			Oracle,
			Error));
	const TSharedPtr<FIndependentReadbackService> Service =
		MakeService({Oracle.ToSharedRef()}, Error);
	FUeAgentResourceTracker Resources = MakeResources();
	FIndependentReadbackOutcome Outcome;
	TestTrue(TEXT("C9 performs independent GPU surface readback"),
		Service->TryVerify(
			FIndependentReadbackRequest(
				TEXT("operation:ls012:render-rhi"),
				TEXT("uai.readback.capture.render_target/v1"),
				TEXT("render-resource:transient:ls012-rhi"),
				PixelExpected(
					TEXT("render-resource:transient:ls012-rhi"),
					8,
					8,
					ExpectedHash,
					Resources)),
			Resources,
			Outcome,
			Error));
	TestEqual(TEXT("GPU pixels verify complete"), Outcome.GetStatus(),
		EVerificationStatus::Verified);
	TestEqual(TEXT("GPU readback produces business complete"),
		Outcome.GetEffectOutcome(), EEffectOutcome::Complete);

	RenderTarget->ReleaseResource();
	RenderTarget->RemoveFromRoot();
	return !HasAnyErrors();
}
