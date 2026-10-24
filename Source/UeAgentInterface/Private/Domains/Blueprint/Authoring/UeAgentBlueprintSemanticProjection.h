#pragma once

#include "CoreMinimal.h"
#include "Core/HjsonWorkspace/UeAgentSemanticLayout.h"
#include "Core/HjsonWorkspace/UeAgentSemanticWorkspaceIO.h"

class FJsonObject;
class FJsonValue;

namespace UeAgentBlueprintAuthoring
{
	/**
	 * Projects the common semantic Blueprint graph model into the existing
	 * Blueprint authoring provider_extension contract. This function is pure:
	 * it does not resolve or mutate UObjects.
	 */
	UEAGENTINTERFACE_API bool TryProjectSemanticProviderExtension(
		const UE::UAI::HjsonWorkspace::FAuthoringModelRef& ProposedModel,
		const TArray<FString>& ChangedSemanticUnitIds,
		const TMap<FString, TSharedPtr<FJsonValue>>& PropertyChangesByUnitId,
		const TArray<UE::UAI::HjsonWorkspace::FStructuralEdit>& StructuralEdits,
		TSharedPtr<FJsonObject>& OutProviderExtension,
		UE::UAI::HjsonWorkspace::FWorkspaceError& OutError,
		const UE::UAI::HjsonWorkspace::FResolvedSemanticWorkspace* Workspace = nullptr,
		const UE::UAI::HjsonWorkspace::FAuthoringModel* CurrentModel = nullptr);
}
