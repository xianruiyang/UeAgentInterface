#pragma once

#include "CoreMinimal.h"
#include "Core/Contract/UeAgentDiagnostic.h"
#include "Core/HjsonWorkspace/UeAgentSemanticWorkspaceIO.h"
#include "Core/PlanningPreflight/UeAgentPlanningPreflight.h"

class FJsonObject;

namespace UE::UAI::DomainAdapters::WorldSpatial
{
	UEAGENTINTERFACE_API bool TryBuildLevelSemanticWorkspaceBundle(
		const HjsonWorkspace::FResolvedSemanticWorkspaceRef& Workspace,
		const TArray<PlanningPreflight::FChangeDraft>& Changes,
		TSharedRef<FJsonObject>& OutBundle,
		HjsonWorkspace::FWorkspaceError& OutError,
		TFunction<Contract::FSourceLocation(const FString&, const FString&)>* OutSourceResolver = nullptr);
}
