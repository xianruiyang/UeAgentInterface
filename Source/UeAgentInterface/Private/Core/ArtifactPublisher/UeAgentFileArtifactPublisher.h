#pragma once

#include "CoreMinimal.h"
#include "Core/Contract/UeAgentDiagnostic.h"
#include "Core/ContextResource/UeAgentContextResource.h"
#include "Core/StateSemantic/UeAgentStateSemantic.h"

#define UAI_FILE_ARTIFACT_PUBLISHER_V1 1

namespace UE::UAI::ArtifactPublisher
{
	using FFileArtifactPublishError = UE::UAI::Contract::FDiagnostic;
	class UEAGENTINTERFACE_API FFileArtifactPublishRequest final
	{
	public:
		FFileArtifactPublishRequest(
			FString InSourcePath,
			FString InFinalPath,
			FString InPublisherOwnerId,
			FString InArtifactKind,
			FString InFormat,
			FString InMimeType);

		const FString& GetSourcePath() const;
		const FString& GetFinalPath() const;
		const FString& GetPublisherOwnerId() const;
		const FString& GetArtifactKind() const;
		const FString& GetFormat() const;
		const FString& GetMimeType() const;
		bool IsValid() const;
		const FFileArtifactPublishError* GetValidationDiagnostic() const;

	private:
		FString SourcePath;
		FString FinalPath;
		FString PublisherOwnerId;
		FString ArtifactKind;
		FString Format;
		FString MimeType;
		TSharedPtr<const FFileArtifactPublishError> ValidationDiagnostic;
	};

	class UEAGENTINTERFACE_API FFileArtifactReference final
	{
	public:
		FFileArtifactReference();
		FFileArtifactReference(
			FString InArtifactId,
			FString InPublisherOwnerId,
			FString InArtifactKind,
			FString InFormat,
			FString InMimeType,
			FString InLocator,
			FString InContentHash,
			int64 InByteCount);

		const FString& GetArtifactId() const;
		const FString& GetPublisherOwnerId() const;
		const FString& GetArtifactKind() const;
		const FString& GetFormat() const;
		const FString& GetMimeType() const;
		const FString& GetLocator() const;
		const FString& GetContentHash() const;
		int64 GetByteCount() const;
		bool IsValid() const;
		const FFileArtifactPublishError* GetValidationDiagnostic() const;

	private:
		FString ArtifactId;
		FString PublisherOwnerId;
		FString ArtifactKind;
		FString Format;
		FString MimeType;
		FString Locator;
		FString ContentHash;
		int64 ByteCount = -1;
		TSharedPtr<const FFileArtifactPublishError> ValidationDiagnostic;
	};

	

	// A failure after verified publication retains OutReference and reports artifact_published=true;
	// callers must forward the diagnostic so cleanup failure is not mistaken for an unchanged destination.
	UEAGENTINTERFACE_API bool TryPublishFileArtifact(
		const FFileArtifactPublishRequest& Request,
		ContextResource::FUeAgentResourceTracker& Resources,
		FFileArtifactReference& OutReference,
		FFileArtifactPublishError& OutError);

	UEAGENTINTERFACE_API bool TryProjectFileArtifactReference(
		const FFileArtifactReference& Reference,
		ContextResource::FUeAgentResourceTracker& Resources,
		StateSemantic::FUeAgentCanonicalValuePtr& OutValue,
		FFileArtifactPublishError& OutError);
}

