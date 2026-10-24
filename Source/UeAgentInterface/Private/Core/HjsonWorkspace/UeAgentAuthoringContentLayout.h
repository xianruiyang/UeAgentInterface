#pragma once

#include "CoreMinimal.h"
#include "Core/StateSemantic/UeAgentStateSemantic.h"

namespace UE::UAI::HjsonWorkspace
{
	class FWorkspaceError;
	// Physical organization only. Providers continue to consume their original values.
	using FPublishAuthoringContent = TFunctionRef<bool(
		const StateSemantic::FUeAgentCanonicalValuePtr& Value, const FString& RelativeStem,
		StateSemantic::FUeAgentCanonicalValuePtr& OutReference)>;

	FString MakeAuthoringContentPathSegment(const FString& Name);

	bool TryOrganizeAuthoringContent(const FString& SemanticType, const FString& Role,
		StateSemantic::FUeAgentCanonicalValuePtr& VisibleRow, FPublishAuthoringContent Publish,
		ContextResource::FUeAgentResourceTracker& Resources, FWorkspaceError& OutError);
}
