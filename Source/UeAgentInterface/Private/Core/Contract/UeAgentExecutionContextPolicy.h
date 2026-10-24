#pragma once

#include "CoreMinimal.h"
#include "Core/Contract/UeAgentDiagnostic.h"

namespace UE::UAI::Contract
{
	enum class EUeAgentStatePlane : uint8
	{
		Invalid,
		AuthoringMemory,
		Persisted,
		CompiledGenerated,
		RuntimeInstance,
		RenderedObservation,
		RegistryIndex,
	};

	enum class EUeAgentExecutionContextFacet : uint8
	{
		Invalid,
		World,
		Level,
		DataLayer,
		WorldPartition,
		PieInstance,
		NetMode,
		NetRole,
		AssetEditor,
		Viewport,
		Selection,
	};

	enum class EUeAgentContextResolutionPolicy : uint8
	{
		Invalid,
		ExplicitOnly,
		ResolveCurrentThenFreeze,
	};

	enum class EUeAgentTransientContextPolicy : uint8
	{
		Invalid,
		ReadOnly,
		TemporaryMutationAllowed,
	};

	UEAGENTINTERFACE_API const TCHAR* LexToString(
		EUeAgentStatePlane Value);
	UEAGENTINTERFACE_API const TCHAR* LexToString(
		EUeAgentExecutionContextFacet Value);
	UEAGENTINTERFACE_API const TCHAR* LexToString(
		EUeAgentContextResolutionPolicy Value);
	UEAGENTINTERFACE_API const TCHAR* LexToString(
		EUeAgentTransientContextPolicy Value);

	UEAGENTINTERFACE_API bool TryParseStatePlane(
		const FString& Value,
		EUeAgentStatePlane& OutStatePlane);

	class UEAGENTINTERFACE_API FUeAgentExecutionContextPolicy final
	{
	public:
		FUeAgentExecutionContextPolicy(
			TArray<EUeAgentStatePlane> InAllowedStatePlanes,
			TArray<EUeAgentExecutionContextFacet> InRequiredFacets,
			EUeAgentContextResolutionPolicy InResolutionPolicy,
			EUeAgentTransientContextPolicy
				InTransientContextPolicy);

		const TArray<EUeAgentStatePlane>&
			GetAllowedStatePlanes() const;
		const TArray<EUeAgentExecutionContextFacet>&
			GetRequiredFacets() const;
		EUeAgentContextResolutionPolicy
			GetResolutionPolicy() const;
		EUeAgentTransientContextPolicy
			GetTransientContextPolicy() const;
		bool IsValid() const;
		const FDiagnostic& GetValidationDiagnostic() const;

	private:
		TArray<EUeAgentStatePlane> AllowedStatePlanes;
		TArray<EUeAgentExecutionContextFacet> RequiredFacets;
		EUeAgentContextResolutionPolicy ResolutionPolicy =
			EUeAgentContextResolutionPolicy::Invalid;
		EUeAgentTransientContextPolicy TransientContextPolicy =
			EUeAgentTransientContextPolicy::Invalid;
		FDiagnostic ValidationDiagnostic;
	};
}
