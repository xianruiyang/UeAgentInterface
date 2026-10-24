#pragma once

#include "CoreMinimal.h"
#include "Core/HjsonWorkspace/UeAgentEditBundleSelection.h"
#include "Core/Projection/UeAgentProjection.h"

namespace UE::UAI::Projection
{
	class UEAGENTINTERFACE_API FEditBundleProjection final
	{
	public:
		FEditBundleProjection(
			FString InBundleFingerprint,
			FString InClosureFingerprint,
			TArray<FString> InInlineUnitIds,
			TArray<FString> InInlineFieldIds,
			TOptional<FUeAgentArtifactDescriptor> InArtifact,
			FUeAgentPageInfo InPage);

		const FString& GetBundleFingerprint() const;
		const FString& GetClosureFingerprint() const;
		const TArray<FString>& GetInlineUnitIds() const;
		const TArray<FString>& GetInlineFieldIds() const;
		const TOptional<FUeAgentArtifactDescriptor>& GetArtifact() const;
		const FUeAgentPageInfo& GetPage() const;
		bool IsValid() const;

	private:
		FString BundleFingerprint;
		FString ClosureFingerprint;
		TArray<FString> InlineUnitIds;
		TArray<FString> InlineFieldIds;
		TOptional<FUeAgentArtifactDescriptor> Artifact;
		FUeAgentPageInfo Page;
		bool bValid = false;
	};
}
