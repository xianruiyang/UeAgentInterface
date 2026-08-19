#pragma once

#include "CoreMinimal.h"
#include "Core/ContextResource/UeAgentContextResource.h"
#include "Core/StateSemantic/UeAgentStateSemantic.h"

#define UAI_FILE_ARTIFACT_PUBLISHER_V1 1

namespace UE::UAI::ArtifactPublisher
{
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

	private:
		FString SourcePath;
		FString FinalPath;
		FString PublisherOwnerId;
		FString ArtifactKind;
		FString Format;
		FString MimeType;
		bool bValid = false;
	};

	class UEAGENTINTERFACE_API FFileArtifactReference final
	{
	public:
		FFileArtifactReference() = default;
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

	private:
		FString ArtifactId;
		FString PublisherOwnerId;
		FString ArtifactKind;
		FString Format;
		FString MimeType;
		FString Locator;
		FString ContentHash;
		int64 ByteCount = -1;
		bool bValid = false;
	};

	class UEAGENTINTERFACE_API FFileArtifactPublishError final
	{
	public:
		const FString& GetCode() const;
		const FString& GetPath() const;
		const FString& GetDetail() const;
		void Set(
			FString InCode,
			FString InPath = FString(),
			FString InDetail = FString());

	private:
		FString Code;
		FString Path;
		FString Detail;
	};

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

