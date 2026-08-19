#pragma once

#include "CoreMinimal.h"
#include "Core/ContextResource/UeAgentContextResource.h"
#include "Core/StateSemantic/UeAgentStateSemantic.h"

#define UAI_WORLD_SPATIAL_TYPED_PORT_V1 1

namespace UE::UAI::DomainAdapters::WorldSpatial
{
	enum class EUeAgentWorldSpatialPortFamily : uint8
	{
		GraphView,
		GraphLayout,
		ObjectTransform,
		ViewportProjection,
		ViewportSessionState,
	};

	class UEAGENTINTERFACE_API FUeAgentWorldSpatialPortRequest final
	{
	public:
		FUeAgentWorldSpatialPortRequest(
			EUeAgentWorldSpatialPortFamily InFamily,
			TArray<StateSemantic::FUeAgentSpatialValuePtr> InSpatialInputs,
			TSet<FString> InFrozenContextFacets);

		EUeAgentWorldSpatialPortFamily GetFamily() const;
		const TArray<StateSemantic::FUeAgentSpatialValuePtr>&
		GetSpatialInputs() const;
		const TSet<FString>& GetFrozenContextFacets() const;

	private:
		EUeAgentWorldSpatialPortFamily Family;
		TArray<StateSemantic::FUeAgentSpatialValuePtr> SpatialInputs;
		TSet<FString> FrozenContextFacets;
	};

	class UEAGENTINTERFACE_API FUeAgentWorldSpatialPortObservation final
	{
	public:
		FUeAgentWorldSpatialPortObservation() = default;
		FUeAgentWorldSpatialPortObservation(
			TArray<StateSemantic::FUeAgentSpatialValuePtr>
				InSpatialObservations,
			FString InDimensionBridgeEvidence);

		const TArray<StateSemantic::FUeAgentSpatialValuePtr>&
		GetSpatialObservations() const;
		const FString& GetDimensionBridgeEvidence() const;

	private:
		TArray<StateSemantic::FUeAgentSpatialValuePtr> SpatialObservations;
		FString DimensionBridgeEvidence;
	};

	struct UEAGENTINTERFACE_API FUeAgentWorldSpatialPortError
	{
		FString Code;
		FString Path;
		FString Detail;
	};

	class UEAGENTINTERFACE_API IUeAgentWorldSpatialNativePort
	{
	public:
		virtual ~IUeAgentWorldSpatialNativePort() = default;

		virtual bool TryExecute(
			const FUeAgentWorldSpatialPortRequest& Request,
			ContextResource::FUeAgentResourceTracker& Resources,
			FUeAgentWorldSpatialPortObservation& OutObservation,
			FUeAgentWorldSpatialPortError& OutError) = 0;
	};

	UEAGENTINTERFACE_API bool TryExecuteWorldSpatialPort(
		const FUeAgentWorldSpatialPortRequest& Request,
		IUeAgentWorldSpatialNativePort& NativePort,
		ContextResource::FUeAgentResourceTracker& Resources,
		FUeAgentWorldSpatialPortObservation& OutObservation,
		FUeAgentWorldSpatialPortError& OutError);
}
