#include "Misc/AutomationTest.h"

#include "DomainAdapters/CaptureMediaLongJob/UeAgentCaptureImageArtifactService.h"
#include "DomainAdapters/CaptureMediaLongJob/UeAgentCaptureImageProcessing.h"
#include "Dom/JsonObject.h"
#include "HAL/FileManager.h"
#include "Misc/FileHelper.h"
#include "Misc/Guid.h"
#include "Misc/Paths.h"

namespace
{
	UE::UAI::ContextResource::FUeAgentResourceTracker MakeResources()
	{
		using namespace UE::UAI::ContextResource;
		return FUeAgentResourceTracker();
	}

	void Cleanup(const FString& Path)
	{
		IFileManager::Get().Delete(*Path, false, true, true);
		IFileManager::Get().Delete(*(Path + TEXT(".uai-artifact-owner.json")), false, true, true);
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUeAgentCaptureImageArtifactServiceTest,
	"UeAgentInterface.DomainAdapters.CaptureMediaLongJob.ImageArtifactService",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FUeAgentCaptureImageArtifactServiceTest::RunTest(const FString& Parameters)
{
	(void)Parameters;
	using namespace UE::UAI::ArtifactPublisher;
	using namespace UE::UAI::DomainAdapters::CaptureMediaLongJob;
	using namespace UE::UAI::ExecutionMutation;

	FUeAgentCaptureImageArtifactService Service;
	{
		auto Resources = MakeResources();
		FUeAgentStagedCaptureImage Invalid;
		FExecutionMutationError Error;
		TestFalse(TEXT("zero-size pixels fail closed"),
			Service.TryStagePngPixels(TEXT("operation:test:invalid"),
				TEXT("pie-output-surface:test"), {}, FIntPoint::ZeroValue,
				Resources, Invalid, Error));
		TestEqual(TEXT("zero-size diagnostic"), Error.GetCode(),
			FString(TEXT("capture_image_stage_request_invalid")));
	}

	{
		auto Resources = MakeResources();
		const TArray<FColor> Pixels = {
			FColor::Red, FColor::Green, FColor::Blue, FColor::White};
		FUeAgentStagedCaptureImage Image;
		FExecutionMutationError Error;
		TestTrue(TEXT("valid pixels stage through capture owner"),
			Service.TryStagePngPixels(TEXT("operation:test:verified"),
				TEXT("pie-output-surface:test"), Pixels, FIntPoint(2, 2),
				Resources, Image, Error));
		FFileArtifactReference Artifact;
		FString EvidenceId;
		TestTrue(TEXT("publish requires independent image readback"),
			Service.TryPublishVerified(TEXT("operation:test:verified"),
				TEXT("test-capture-owner"), Image, Resources,
				Artifact, EvidenceId, Error));
		TestTrue(TEXT("verified artifact is valid"), Artifact.IsValid());
		TestTrue(TEXT("verified artifact has evidence"),
			EvidenceId.StartsWith(TEXT("readback:")));
		Cleanup(Artifact.GetLocator());
	}

	{
		auto Resources = MakeResources();
		const TArray<FColor> Pixels = {FColor::Black};
		FUeAgentStagedCaptureImage Image;
		FExecutionMutationError Error;
		TestTrue(TEXT("failure fixture stages"),
			Service.TryStagePngPixels(TEXT("operation:test:corrupt"),
				TEXT("pie-output-surface:corrupt"), Pixels, FIntPoint(1, 1),
				Resources, Image, Error));
		TArray<uint8> Corrupt = {0, 1, 2, 3};
		FFileHelper::SaveArrayToFile(Corrupt, *Image.GetNativePath());
		FFileArtifactReference Artifact;
		FString EvidenceId;
		TestFalse(TEXT("decode/readback failure cannot succeed"),
			Service.TryPublishVerified(TEXT("operation:test:corrupt"),
				TEXT("test-capture-owner"), Image, Resources,
				Artifact, EvidenceId, Error));
		Cleanup(Image.GetFinalPath());
		Cleanup(Image.GetNativePath());
	}
	return !HasAnyErrors();
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUeAgentCaptureImageProcessingTest,
	"UeAgentInterface.DomainAdapters.CaptureMediaLongJob.ImageProcessing",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FUeAgentCaptureImageProcessingTest::RunTest(const FString& Parameters)
{
	(void)Parameters;
	using namespace UE::UAI::DomainAdapters::CaptureMediaLongJob;

	FString Error;
	TArray<FColor> Resized;
	FIntPoint ResizedSize = FIntPoint::ZeroValue;
	TestFalse(
		TEXT("pixel count mismatch is rejected"),
		FCaptureImageProcessing::ResizePixelsMaxSize(
			{FColor::Red}, FIntPoint(2, 2), 64, Resized, ResizedSize, Error));
	TestEqual(TEXT("pixel mismatch error is stable"), Error, FString(TEXT("invalid_pixels")));

	TArray<FColor> Source;
	Source.Init(FColor(40, 80, 120, 64), 128 * 64);
	Error.Reset();
	TestTrue(
		TEXT("shared image owner resizes a valid frame"),
		FCaptureImageProcessing::ResizePixelsMaxSize(
			Source, FIntPoint(128, 64), 64, Resized, ResizedSize, Error));
	TestEqual(TEXT("resize width honors max size"), ResizedSize.X, 64);
	TestEqual(TEXT("resize preserves aspect ratio"), ResizedSize.Y, 32);
	TestEqual(TEXT("resize returns the exact pixel count"), Resized.Num(), 64 * 32);
	const FString OwnedRoot = FPaths::Combine(
		FPaths::ProjectSavedDir(),
		TEXT("UeAgentInterface/Tests/CaptureImageProcessing"),
		FGuid::NewGuid().ToString(EGuidFormats::Digits));
	const FString ImagePath = FPaths::Combine(OwnedRoot, TEXT("single.png"));
	Error.Reset();
	TestTrue(
		TEXT("shared image owner writes a compressed image"),
		FCaptureImageProcessing::WriteCompressedImage(
			Resized, ResizedSize, TEXT("png"), 90, ImagePath, Error, false));
	TestTrue(TEXT("compressed image exists"), IFileManager::Get().FileSize(*ImagePath) > 0);

	const FString SheetPath = FPaths::Combine(OwnedRoot, TEXT("sheet.png"));
	const TSharedPtr<FJsonObject> SheetParams = MakeShared<FJsonObject>();
	SheetParams->SetStringField(TEXT("frame_sheet_output_file"), SheetPath);
	SheetParams->SetNumberField(TEXT("frame_sheet_padding"), 2);
	SheetParams->SetNumberField(TEXT("frame_sheet_label_scale"), 1);
	TArray<TArray<FColor>> Frames;
	Frames.Add(TArray<FColor>());
	Frames.Last().Init(FColor::Red, 16 * 16);
	Frames.Add(TArray<FColor>());
	Frames.Last().Init(FColor::Blue, 16 * 16);
	FString ActualSheetPath;
	TSharedPtr<FJsonObject> SheetData;
	Error.Reset();
	TestTrue(
		TEXT("shared image owner creates one frame sheet"),
		FCaptureImageProcessing::WriteFrameSequenceSheetImage(
			Frames,
			{TEXT("F0"), TEXT("F1")},
			FIntPoint(16, 16),
			SheetParams,
			TEXT("png"),
			90,
			ActualSheetPath,
			SheetData,
			Error));
	TestEqual(TEXT("frame sheet uses the requested path"), ActualSheetPath, SheetPath);
	TestTrue(TEXT("frame sheet metadata is returned"), SheetData.IsValid());
	if (SheetData.IsValid())
	{
		TestEqual(TEXT("frame sheet reports both frames"),
			static_cast<int32>(SheetData->GetNumberField(TEXT("frame_count"))), 2);
	}
	TestTrue(TEXT("frame sheet exists"), IFileManager::Get().FileSize(*SheetPath) > 0);
	Frames.SetNum(1);
	Frames[0].Init(FColor::Green, 32769);
	SheetParams->SetNumberField(TEXT("frame_sheet_padding"), 0);
	SheetParams->SetNumberField(TEXT("frame_sheet_max_size"), 32769);
	TestTrue(TEXT("sheet width beyond former 32768 admission cap is written"),
		FCaptureImageProcessing::WriteFrameSequenceSheetImage(Frames, {TEXT("F0")}, FIntPoint(32769, 1),
			SheetParams, TEXT("png"), 90, ActualSheetPath, SheetData, Error));
	if (SheetData) TestEqual(TEXT("sheet retains requested width"), int32(SheetData->GetNumberField(TEXT("width"))), 32769);

	IFileManager::Get().DeleteDirectory(*OwnedRoot, false, true);
	return !HasAnyErrors();
}
