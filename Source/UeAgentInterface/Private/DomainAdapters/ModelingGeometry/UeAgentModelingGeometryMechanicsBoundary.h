#pragma once

#include "Core/Contract/UeAgentDiagnostic.h"

#include "CoreMinimal.h"
#include "DomainAdapters/ModelingGeometry/UeAgentModelingSession.h"

class FJsonObject;

namespace UE::UAI::DomainAdapters::ModelingGeometry
{
	using FModelingSessionNativeRequest = FModelingSessionRequest;

	class IUeAgentModelingSessionNativeMechanicsPort
	{
	public:
		virtual ~IUeAgentModelingSessionNativeMechanicsPort() = default;

		virtual bool Execute(
			const FString& Action,
			const FModelingSessionNativeRequest& Request,
			TSharedPtr<FJsonObject>& OutObservation,
			UE::UAI::Contract::FDiagnostic& OutError) const = 0;
	};

	UEAGENTINTERFACE_API const TArray<FString>& GetCommandIds();
	UEAGENTINTERFACE_API const TArray<FString>& GetRequiredAuthorityScopes();
	UEAGENTINTERFACE_API bool IsModelingSessionAction(const FString& Action);
	UEAGENTINTERFACE_API TSharedRef<const IUeAgentModelingSessionNativeMechanicsPort>
		CreateModelingSessionNativeMechanicsPort();
}
