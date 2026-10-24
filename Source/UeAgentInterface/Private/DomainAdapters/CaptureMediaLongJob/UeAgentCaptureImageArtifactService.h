#pragma once

#include "CoreMinimal.h"
#include "Async/Future.h"
#include "Core/ArtifactPublisher/UeAgentFileArtifactPublisher.h"
#include "Core/ContextResource/UeAgentContextResource.h"
#include "Core/ExecutionMutation/UeAgentLongRunningOperation.h"

class FJsonValue;

namespace UE::UAI::DomainAdapters::CaptureMediaLongJob
{
	// Only for files produced and owned by the current capture, including failed adoption.
	UEAGENTINTERFACE_API bool DiscardProducedCaptureFile(const FString& Path);

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
		bool TryValidate(UE::UAI::Contract::FDiagnostic& OutError) const;

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
		// Stage an opaque scene screenshot; render-target alpha is not PNG opacity.
		// Transparent captures retain their encoded alpha through TryAdoptStagedImage.
		virtual bool TryStagePngPixels(
			const FString& OperationId,
			const FString& SubjectRef,
			const TArray<FColor>& Pixels,
			const FIntPoint& Size,
			ContextResource::FUeAgentResourceTracker& Resources,
			FUeAgentStagedCaptureImage& OutImage,
			ExecutionMutation::FExecutionMutationError& OutError) const = 0;

		// The caller owns NativePath until success. Failure leaves OutImage empty
		// and does not delete the input; the caller must clean up its produced files.
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

		virtual bool DiscardStaged(const FUeAgentStagedCaptureImage& Image) const = 0;
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
		bool DiscardStaged(const FUeAgentStagedCaptureImage& Image) const override;
	};

	struct FPublishedCaptureImage
	{
		TSharedPtr<FJsonValue> Artifact;
		TArray<FColor> Preview;
		FIntPoint PreviewSize = FIntPoint::ZeroValue;
		UE::UAI::Contract::FDiagnostic Error;
		UE::UAI::Contract::FDiagnostic CleanupError;
		UE::UAI::Contract::FDiagnostic PreviewError;
		double ProcessingSeconds = 0.0;
		TMap<ContextResource::EUeAgentResourceDimension, int64> Charges;
	};

	/** Owns its pixels and accounting; no game-thread object or caller tracker is borrowed. */
	TFuture<FPublishedCaptureImage> PublishCaptureImageAsync(
		TSharedRef<IUeAgentCaptureImageArtifactService, ESPMode::ThreadSafe> Service,
		FString OperationId, FString SubjectRef, FString PublisherOwnerId,
		TArray<FColor> Pixels, FIntPoint Size, bool bPreview);
}
