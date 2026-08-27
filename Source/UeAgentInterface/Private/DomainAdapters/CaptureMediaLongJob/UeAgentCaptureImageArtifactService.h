#pragma once

#include "CoreMinimal.h"
#include "Core/ArtifactPublisher/UeAgentFileArtifactPublisher.h"
#include "Core/ContextResource/UeAgentContextResource.h"
#include "Core/ExecutionMutation/UeAgentLongRunningOperation.h"

namespace UE::UAI::DomainAdapters::CaptureMediaLongJob
{
	class UEAGENTINTERFACE_API FUeAgentStagedCaptureImage final
	{
	public:
		FUeAgentStagedCaptureImage() = default;
		FUeAgentStagedCaptureImage(
			FString InSubjectRef,
			FString InNativePath,
			FString InFinalPath,
			FString InFormat,
			int32 InWidth,
			int32 InHeight,
			FString InPixelHash);
		const FString& GetSubjectRef() const { return SubjectRef; }
		const FString& GetNativePath() const { return NativePath; }
		const FString& GetFinalPath() const { return FinalPath; }
		const FString& GetFormat() const { return Format; }
		int32 GetWidth() const { return Width; }
		int32 GetHeight() const { return Height; }
		const FString& GetPixelHash() const { return PixelHash; }
		bool IsValid() const;

	private:
		friend class FUeAgentCaptureImageArtifactService;
		FString SubjectRef;
		FString NativePath;
		FString FinalPath;
		FString Format;
		FString PixelHash;
		int32 Width = 0;
		int32 Height = 0;
	};

	class UEAGENTINTERFACE_API IUeAgentCaptureImageArtifactService
	{
	public:
		virtual ~IUeAgentCaptureImageArtifactService() = default;
		virtual bool TryStagePngPixels(
			const FString& OperationId,
			const FString& SubjectRef,
			const TArray<FColor>& Pixels,
			const FIntPoint& Size,
			ContextResource::FUeAgentResourceTracker& Resources,
			FUeAgentStagedCaptureImage& OutImage,
			ExecutionMutation::FExecutionMutationError& OutError) const = 0;

		virtual bool TryAdoptStagedImage(
			const FString& OperationId,
			const FString& SubjectRef,
			const FString& NativePath,
			const FString& RequestedFinalPath,
			const FString& Format,
			FUeAgentStagedCaptureImage& OutImage,
			ExecutionMutation::FExecutionMutationError& OutError) const = 0;

		virtual bool TryPublishVerified(
			const FString& OperationId,
			const FString& PublisherOwnerId,
			const FUeAgentStagedCaptureImage& Image,
			ContextResource::FUeAgentResourceTracker& Resources,
			ArtifactPublisher::FFileArtifactReference& OutArtifact,
			FString& OutEvidenceId,
			ExecutionMutation::FExecutionMutationError& OutError) const = 0;

		virtual void DiscardStaged(const FUeAgentStagedCaptureImage& Image) const = 0;
	};

	class UEAGENTINTERFACE_API FUeAgentCaptureImageArtifactService final
		: public IUeAgentCaptureImageArtifactService
	{
	public:
		bool TryStagePngPixels(const FString& OperationId, const FString& SubjectRef,
			const TArray<FColor>& Pixels, const FIntPoint& Size,
			ContextResource::FUeAgentResourceTracker& Resources,
			FUeAgentStagedCaptureImage& OutImage,
			ExecutionMutation::FExecutionMutationError& OutError) const override;
		bool TryAdoptStagedImage(const FString& OperationId, const FString& SubjectRef,
			const FString& NativePath, const FString& RequestedFinalPath,
			const FString& Format, FUeAgentStagedCaptureImage& OutImage,
			ExecutionMutation::FExecutionMutationError& OutError) const override;
		bool TryPublishVerified(const FString& OperationId, const FString& PublisherOwnerId,
			const FUeAgentStagedCaptureImage& Image,
			ContextResource::FUeAgentResourceTracker& Resources,
			ArtifactPublisher::FFileArtifactReference& OutArtifact,
			FString& OutEvidenceId,
			ExecutionMutation::FExecutionMutationError& OutError) const override;
		void DiscardStaged(const FUeAgentStagedCaptureImage& Image) const override;
	};
}
