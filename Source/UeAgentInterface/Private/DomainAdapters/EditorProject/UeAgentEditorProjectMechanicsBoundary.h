#pragma once

#include "CoreMinimal.h"

class FJsonObject;

namespace UE::UAI::DomainAdapters::EditorProject
{
	struct FEditorProjectQueryRequest
	{
		FString RequestId;
		TSharedPtr<FJsonObject> Params;
	};

	class IEditorProjectQueryMechanicsPort
	{
	public:
		virtual ~IEditorProjectQueryMechanicsPort() = default;

		virtual bool Execute(
			const FString& Action,
			const FEditorProjectQueryRequest& Request,
			TSharedPtr<FJsonObject>& OutObservation,
			FString& OutError) const = 0;
	};

	UEAGENTINTERFACE_API const TArray<FString>& GetCommandIds();
	UEAGENTINTERFACE_API TArray<FString> GetRequiredAuthorityScopes(
		const FString& CommandId);
	UEAGENTINTERFACE_API bool IsEditorProjectQueryRoute(
		const FString& ProviderBinding,
		const FString& Action);
	UEAGENTINTERFACE_API TSharedRef<const IEditorProjectQueryMechanicsPort>
		CreateEditorProjectQueryMechanicsPort();
}
