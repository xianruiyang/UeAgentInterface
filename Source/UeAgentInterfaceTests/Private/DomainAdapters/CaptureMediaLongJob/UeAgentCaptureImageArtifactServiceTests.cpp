#include "Misc/AutomationTest.h"

#include "DomainAdapters/CaptureMediaLongJob/UeAgentCaptureImageArtifactService.h"
#include "HAL/FileManager.h"
#include "Misc/FileHelper.h"

namespace
{
	UE::UAI::ContextResource::FUeAgentResourceTracker MakeResources()
	{
		using namespace UE::UAI::ContextResource;
		return FUeAgentResourceTracker(FUeAgentResourceBudget({
			{EUeAgentResourceDimension::InputBytes, 4 * 1024 * 1024},
			{EUeAgentResourceDimension::OutputBytes, 4 * 1024 * 1024},
			{EUeAgentResourceDimension::StringBytes, 4 * 1024 * 1024},
			{EUeAgentResourceDimension::Nodes, 4096},
			{EUeAgentResourceDimension::Items, 4096},
			{EUeAgentResourceDimension::Files, 32},
			{EUeAgentResourceDimension::ReferenceEdges, 4096},
			{EUeAgentResourceDimension::WorkUnits, 8 * 1024 * 1024},
			{EUeAgentResourceDimension::EstimatedMemoryBytes, 16 * 1024 * 1024},
		}));
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
