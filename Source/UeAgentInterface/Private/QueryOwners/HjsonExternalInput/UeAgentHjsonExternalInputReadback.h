#pragma once

#include "CoreMinimal.h"
#include "Core/HjsonWorkspace/UeAgentHjsonWorkspace.h"

namespace UE::UAI::HjsonWorkspace { struct FPreparedWorkspaceExport; }

namespace UE::UAI::QueryOwners::HjsonExternalInput
{
	UEAGENTINTERFACE_API const TArray<FString>&
	GetHjsonExternalInputReadbackFields();

	UEAGENTINTERFACE_API bool HasHjsonExternalInputProvenance(
		const FString& TargetIdentity);

	UEAGENTINTERFACE_API bool TryExportHjsonExternalInputWorkspace(
		const FString& TargetIdentity,
		const FString& CanonicalParamsJson,
		FString& OutPayloadJson,
		HjsonWorkspace::FWorkspaceError& OutError,
		HjsonWorkspace::FPreparedWorkspaceExport* PreparedExport = nullptr);
}
