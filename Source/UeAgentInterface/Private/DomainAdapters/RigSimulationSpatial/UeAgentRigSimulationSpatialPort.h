#pragma once

#include "Core/Contract/UeAgentDiagnostic.h"

#include "CoreMinimal.h"
#include "Core/ContextResource/UeAgentContextResource.h"
#include "Core/StateSemantic/UeAgentStateSemantic.h"
#include "Core/StateSemantic/UeAgentTemporalContract.h"

#define UAI_RIG_SIMULATION_SPATIAL_TYPED_PORT_V1 1

namespace UE::UAI::DomainAdapters::RigSimulationSpatial
{
	enum class EUeAgentRigSimulationSpatialPortFamily : uint8
	{
		SkeletalAttachment,
		RigBake,
		IkRetarget,
		SimulationDefinition,
		SimulationObservation,
	};

	class UEAGENTINTERFACE_API
	FUeAgentRigSimulationSpatialPortRequest final
	{
	public:
		FUeAgentRigSimulationSpatialPortRequest(
			EUeAgentRigSimulationSpatialPortFamily InFamily,
			TArray<StateSemantic::FUeAgentTemporalPoint> InTemporalPoints,
			TArray<StateSemantic::FUeAgentTemporalRange> InTemporalRanges,
			TArray<StateSemantic::FUeAgentSpatialValuePtr> InSpatialValues,
			TArray<StateSemantic::FUeAgentSpatialConversion>
				InSpatialConversions,
			TSet<FString> InFrozenContextFacets,
			TArray<StateSemantic::FUeAgentTemporalConversion>
				InTemporalConversions = {});

		EUeAgentRigSimulationSpatialPortFamily GetFamily() const;
		const TArray<StateSemantic::FUeAgentTemporalPoint>&
		GetTemporalPoints() const;
		const TArray<StateSemantic::FUeAgentTemporalRange>&
		GetTemporalRanges() const;
		const TArray<StateSemantic::FUeAgentSpatialValuePtr>&
		GetSpatialValues() const;
		const TArray<StateSemantic::FUeAgentSpatialConversion>&
		GetSpatialConversions() const;
		const TSet<FString>& GetFrozenContextFacets() const;
		const TArray<StateSemantic::FUeAgentTemporalConversion>&
		GetTemporalConversions() const;

	private:
		EUeAgentRigSimulationSpatialPortFamily Family;
		TArray<StateSemantic::FUeAgentTemporalPoint> TemporalPoints;
		TArray<StateSemantic::FUeAgentTemporalRange> TemporalRanges;
		TArray<StateSemantic::FUeAgentSpatialValuePtr> SpatialValues;
		TArray<StateSemantic::FUeAgentSpatialConversion> SpatialConversions;
		TSet<FString> FrozenContextFacets;
		TArray<StateSemantic::FUeAgentTemporalConversion>
			TemporalConversions;
	};

	class UEAGENTINTERFACE_API
	FUeAgentRigSimulationSpatialPortObservation final
	{
	public:
		FUeAgentRigSimulationSpatialPortObservation() = default;
		FUeAgentRigSimulationSpatialPortObservation(
			TArray<StateSemantic::FUeAgentTemporalPoint>
				InTemporalObservations,
			TArray<StateSemantic::FUeAgentSpatialValuePtr>
				InSpatialObservations,
			TArray<FString> InReadbackEvidence,
			TArray<StateSemantic::FUeAgentTemporalRange>
				InTemporalRangeObservations = {});

		const TArray<StateSemantic::FUeAgentTemporalPoint>&
		GetTemporalObservations() const;
		const TArray<StateSemantic::FUeAgentTemporalRange>&
		GetTemporalRangeObservations() const;
		const TArray<StateSemantic::FUeAgentSpatialValuePtr>&
		GetSpatialObservations() const;
		const TArray<FString>& GetReadbackEvidence() const;

	private:
		TArray<StateSemantic::FUeAgentTemporalPoint> TemporalObservations;
		TArray<StateSemantic::FUeAgentTemporalRange>
			TemporalRangeObservations;
		TArray<StateSemantic::FUeAgentSpatialValuePtr> SpatialObservations;
		TArray<FString> ReadbackEvidence;
	};

	using FUeAgentRigSimulationSpatialPortError = UE::UAI::Contract::FDiagnostic;

	class UEAGENTINTERFACE_API IUeAgentRigSimulationSpatialNativePort
	{
	public:
		virtual ~IUeAgentRigSimulationSpatialNativePort() = default;

		virtual bool TryExecute(
			const FUeAgentRigSimulationSpatialPortRequest& Request,
			ContextResource::FUeAgentResourceTracker& Resources,
			FUeAgentRigSimulationSpatialPortObservation& OutObservation,
			FUeAgentRigSimulationSpatialPortError& OutError) = 0;
	};

	UEAGENTINTERFACE_API bool TryExecuteRigSimulationSpatialPort(
		const FUeAgentRigSimulationSpatialPortRequest& Request,
		IUeAgentRigSimulationSpatialNativePort& NativePort,
		ContextResource::FUeAgentResourceTracker& Resources,
		FUeAgentRigSimulationSpatialPortObservation& OutObservation,
		FUeAgentRigSimulationSpatialPortError& OutError);
}
