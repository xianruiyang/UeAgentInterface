#include "Misc/AutomationTest.h"

#include "Core/ContextResource/UeAgentContextResource.h"
#include "HAL/FileManager.h"
#include "Misc/FileHelper.h"
#include "Misc/Guid.h"
#include "Misc/Paths.h"

#include "Core/ArtifactPublisher/UeAgentFileArtifactPublisher.h"

namespace
{
	using namespace UE::UAI::ArtifactPublisher;
	using namespace UE::UAI::ContextResource;

	FUeAgentResourceTracker MakeResources()
	{
		return FUeAgentResourceTracker(FUeAgentResourceBudget({
			{EUeAgentResourceDimension::InputBytes, 4 * 1024 * 1024},
			{EUeAgentResourceDimension::OutputBytes, 4 * 1024 * 1024},
			{EUeAgentResourceDimension::StringBytes, 1024 * 1024},
			{EUeAgentResourceDimension::Nodes, 4096},
			{EUeAgentResourceDimension::Items, 4096},
			{EUeAgentResourceDimension::Files, 64},
			{EUeAgentResourceDimension::ReferenceEdges, 4096},
			{EUeAgentResourceDimension::WorkUnits, 65536},
			{EUeAgentResourceDimension::EstimatedMemoryBytes, 16 * 1024 * 1024},
		}));
	}

	class FScopedArtifactRoot final
	{
	public:
		FScopedArtifactRoot()
			: Root(FPaths::ConvertRelativePathToFull(FPaths::Combine(
				FPaths::ProjectIntermediateDir(),
				TEXT("UeAgentInterfaceTests"),
				TEXT("LS012FileArtifact"),
				FGuid::NewGuid().ToString(EGuidFormats::Digits))))
		{
			IFileManager::Get().MakeDirectory(*Root, true);
		}

		~FScopedArtifactRoot()
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
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUeAgentFileArtifactPublishOwnershipTest,
	"UeAgentInterface.Core.ArtifactPublisher.FileArtifact.E1."
		"PublishHashOwnershipAndSourceRetention",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FUeAgentFileArtifactPublishOwnershipTest::RunTest(
	const FString& Parameters)
{
	FScopedArtifactRoot Files;
	const FString Source = Files.Path(TEXT("native-owned-source.bin"));
	const FString Final = Files.Path(TEXT("published/artifact.bin"));
	const TArray<uint8> Bytes = {0x55, 0x41, 0x49, 0x00, 0x7f, 0xff};
	TestTrue(TEXT("native source fixture is written"),
		FFileHelper::SaveArrayToFile(Bytes, *Source));

	FUeAgentResourceTracker Resources = MakeResources();
	FFileArtifactReference Artifact;
	FFileArtifactPublishError Error;
	const FFileArtifactPublishRequest Request(
		Source,
		Final,
		TEXT("operation:ls012:file-publish"),
		TEXT("image"),
		TEXT("bin"),
		TEXT("application/octet-stream"));
	TestTrue(TEXT("C8F publishes exact source into owned final"),
		TryPublishFileArtifact(Request, Resources, Artifact, Error));
	TestTrue(TEXT("artifact reference is valid"), Artifact.IsValid());
	TestEqual(TEXT("artifact final locator is exact"),
		Artifact.GetLocator(), FPaths::ConvertRelativePathToFull(Final));
	TestEqual(TEXT("artifact byte count is exact"),
		Artifact.GetByteCount(), static_cast<int64>(Bytes.Num()));
	TestTrue(TEXT("artifact content hash is SHA-256"),
		Artifact.GetContentHash().StartsWith(TEXT("sha256:")));
	TestEqual(TEXT("artifact owner is exact"),
		Artifact.GetPublisherOwnerId(),
		FString(TEXT("operation:ls012:file-publish")));
	TestTrue(TEXT("C8F does not delete the C11-owned source"),
		IFileManager::Get().FileExists(*Source));
	TestTrue(TEXT("published final exists"),
		IFileManager::Get().FileExists(*Final));

	TArray<uint8> FinalBytes;
	TestTrue(TEXT("published final is independently readable"),
		FFileHelper::LoadFileToArray(FinalBytes, *Final));
	TestTrue(TEXT("published bytes exactly match native source"),
		FinalBytes == Bytes);
	return !HasAnyErrors();
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUeAgentFileArtifactUnownedFinalTest,
	"UeAgentInterface.Core.ArtifactPublisher.FileArtifact.E1."
		"UnownedFinalFailsClosed",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FUeAgentFileArtifactUnownedFinalTest::RunTest(
	const FString& Parameters)
{
	FScopedArtifactRoot Files;
	const FString Source = Files.Path(TEXT("source.bin"));
	const FString Final = Files.Path(TEXT("unowned.bin"));
	const TArray<uint8> SourceBytes = {1, 2, 3, 4};
	const TArray<uint8> ExistingBytes = {9, 8, 7};
	TestTrue(TEXT("source fixture is written"),
		FFileHelper::SaveArrayToFile(SourceBytes, *Source));
	TestTrue(TEXT("unowned final fixture is written"),
		FFileHelper::SaveArrayToFile(ExistingBytes, *Final));

	FUeAgentResourceTracker Resources = MakeResources();
	FFileArtifactReference Artifact;
	FFileArtifactPublishError Error;
	TestFalse(TEXT("non-empty unowned final fails closed"),
		TryPublishFileArtifact(
			FFileArtifactPublishRequest(
				Source,
				Final,
				TEXT("operation:ls012:other-owner"),
				TEXT("media"),
				TEXT("wav"),
				TEXT("audio/wav")),
			Resources,
			Artifact,
			Error));
	TestEqual(TEXT("unowned final has stable failure"), Error.GetCode(),
		FString(TEXT("file_artifact_final_ownership_required")));
	TArray<uint8> AfterBytes;
	TestTrue(TEXT("unowned final remains readable"),
		FFileHelper::LoadFileToArray(AfterBytes, *Final));
	TestTrue(TEXT("unowned final is unchanged"), AfterBytes == ExistingBytes);
	return !HasAnyErrors();
}
