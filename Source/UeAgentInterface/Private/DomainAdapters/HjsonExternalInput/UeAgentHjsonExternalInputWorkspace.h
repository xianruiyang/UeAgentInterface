#pragma once

#include "CoreMinimal.h"
#include "Core/HjsonWorkspace/UeAgentSemanticLayout.h"
#include "Core/HjsonWorkspace/DomainAuthoring/UeAgentDomainHjsonAuthoringKit.h"

class FJsonObject;

namespace UE::UAI::DomainAdapters::HjsonExternalInput
{
	// Prepared values and their original parser/workspace provenance travel together.
	struct FResolvedExternalInputDocument final
	{
		HjsonWorkspace::DomainAuthoring::FResolvedDomainHjsonDocument Source;
		TSharedPtr<FJsonObject> Root;
		void LocateFailure(HjsonWorkspace::FWorkspaceError& Error, bool bDelegated = false) const;
	};

	UEAGENTINTERFACE_API const FString& GetExternalInputWorkspaceContract();
	UEAGENTINTERFACE_API bool TryReadExternalInputTargetRevision(
		const FString& Target, FString& OutRevision, HjsonWorkspace::FWorkspaceError& OutError,
		const FString& RootType, const FString& Source);
	UEAGENTINTERFACE_API bool TryReadExternalInputSource(
		const FString& Source, FString& OutPath, FString& OutDigest,
		HjsonWorkspace::FWorkspaceError& OutError);
	UEAGENTINTERFACE_API bool TryBuildExternalInputWorkspaceModel(
		const FString& Target, const TSharedPtr<FJsonObject>& Root,
		const FString& BaseRevision, const FString& ExpectedSourceDigest,
		HjsonWorkspace::FAuthoringModelPtr& OutModel,
		HjsonWorkspace::FWorkspaceError& OutError);
	UEAGENTINTERFACE_API bool TryResolveExternalInputDocument(
		const FString& CanonicalParamsJson, const FString& RootType,
		FResolvedExternalInputDocument& OutDocument,
		FString& OutTarget, HjsonWorkspace::FWorkspaceError& OutError);
}
