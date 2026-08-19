#pragma once

#include "CoreMinimal.h"

class FJsonObject;

namespace UE::UAI::DomainAdapters::ModelingGeometry
{
	struct FModelingSessionNativeRequest
	{
		FString RequestId;
		FString CommandId;
		TSharedPtr<FJsonObject> Params;
	};

	class IUeAgentModelingSessionNativeMechanicsPort
	{
	public:
		virtual ~IUeAgentModelingSessionNativeMechanicsPort() = default;

		virtual bool Execute(
			const FString& Action,
			const FModelingSessionNativeRequest& Request,
			TSharedPtr<FJsonObject>& OutObservation,
			FString& OutError) const = 0;
	};

	UEAGENTINTERFACE_API const TArray<FString>& GetCommandIds();
	UEAGENTINTERFACE_API const TArray<FString>& GetRequiredAuthorityScopes();
	UEAGENTINTERFACE_API bool IsModelingSessionAction(const FString& Action);
	UEAGENTINTERFACE_API TSharedRef<const IUeAgentModelingSessionNativeMechanicsPort>
		CreateModelingSessionNativeMechanicsPort();
}
