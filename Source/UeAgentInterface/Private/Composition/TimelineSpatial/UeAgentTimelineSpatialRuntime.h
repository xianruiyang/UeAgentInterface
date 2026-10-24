#pragma once

#include "CommandEdge/UeAgentCommandDispatch.h"
#include "Core/BindingGateway/Operation/UeAgentOperationBinding.h"
#include "Core/BindingGateway/Ticket/UeAgentBindingTicket.h"
#include "Core/Contract/UeAgentCommandContract.h"
#include "Core/Contract/UeAgentContract.h"
#include "Core/Discovery/UeAgentDiscovery.h"
#include "Core/Projection/UeAgentProjection.h"
#include "DomainAdapters/RigSimulationSpatial/UeAgentRigSimulationSpatialProductionRuntime.h"
#include "DomainAdapters/TimelineAnimation/UeAgentTimelineAnimationProductionRuntime.h"
#include "Templates/SharedPointer.h"

#define UAI_TIMELINE_SPATIAL_UNIFIED_OPERATION_BINDING_V1 1

class UWorld;

namespace UE::UAI::Composition::TimelineSpatial
{
	struct UEAGENTINTERFACE_API FUeAgentTimelineSpatialRuntimeConfig
	{
		FString ProjectInstanceId;
		FString EditorInstanceId;
		FString ServerSessionId;
		int64 RegistryEpoch = -1;
		TArray<DomainAdapters::TimelineAnimation::
			FUeAgentTimelineAnimationSubjectBinding> TimelineSubjectBindings;
		TArray<DomainAdapters::TimelineAnimation::
			FUeAgentTimelineAnimationRigCandidateBinding> TimelineRigCandidateBindings;
		TArray<DomainAdapters::RigSimulationSpatial::
			FUeAgentRigSimulationSpatialObjectBinding> RigObjectBindings;
		TArray<DomainAdapters::RigSimulationSpatial::
			FUeAgentRigSimulationSpatialClockBinding> RigClockBindings;
		TArray<DomainAdapters::RigSimulationSpatial::
			FUeAgentRigSimulationSpatialFrameBinding> RigFrameBindings;
		TArray<DomainAdapters::RigSimulationSpatial::
			FUeAgentRigSimulationSpatialSkeletonBinding> RigSkeletonBindings;
		TArray<DomainAdapters::RigSimulationSpatial::
			FUeAgentRigSimulationSpatialSocketBinding> RigSocketBindings;
		TArray<DomainAdapters::RigSimulationSpatial::
			FUeAgentRigSimulationSpatialRigBakeBinding> RigBakeBindings;
		TArray<DomainAdapters::RigSimulationSpatial::
			FUeAgentRigSimulationSpatialIkRigBinding> RigIkBindings;
		TArray<DomainAdapters::RigSimulationSpatial::
			FUeAgentRigSimulationSpatialRetargeterBinding> RigRetargeterBindings;
		TSharedPtr<DomainAdapters::RigSimulationSpatial::
			IUeAgentRigSimulationSpatialAssetEffectPort> RigAssetEffectPort;
		TSharedPtr<DomainAdapters::RigSimulationSpatial::
			IUeAgentRigSimulationSpatialPhysicsSessionPort> RigPhysicsSessionPort;

		bool IsValid() const;
	};

	using FUeAgentTimelineSpatialRuntimeError = Contract::FDiagnostic;

	/**
	 * Production owner for the registered Timeline/Spatial public capabilities.
	 *
	 * The runtime composes the three exact domain runtimes once, then exposes
	 * one C3Q application port and one C3O application port to the command
	 * edge. Public handlers never select a domain implementation themselves.
	 */
	class UEAGENTINTERFACE_API FUeAgentTimelineSpatialRuntime final
	{
	public:
		class FState;

		static bool TryCreate(
			const FUeAgentTimelineSpatialRuntimeConfig& Config,
			UWorld& ExplicitWorld,
			TSharedRef<const BindingGateway::Ticket::
				FUeAgentOperationTicketService> TicketService,
			TSharedRef<const Discovery::FUeAgentDiscoveryService> Discovery,
			TSharedRef<const Projection::FUeAgentResponseProjector> Projector,
			TSharedPtr<FUeAgentTimelineSpatialRuntime>& OutRuntime,
			FUeAgentTimelineSpatialRuntimeError& OutError);

		const TSharedRef<Contract::IUeAgentQueryApplicationPort>&
			GetQueryPort() const;
		const TSharedRef<Contract::IUeAgentOperationApplicationPort>&
			GetOperationPort() const;
		const TArray<TSharedRef<CommandEdge::IUeAgentCommandHandler>>&
			GetCommandHandlers() const;
		const TArray<FString>& GetAuthorityScopes() const;
		int32 GetQueryBindingCount() const;
		int32 GetOperationBindingCount() const;

	private:
		FUeAgentTimelineSpatialRuntime(
			TSharedRef<FState, ESPMode::ThreadSafe> InState,
			TSharedRef<Contract::IUeAgentQueryApplicationPort> InQueryPort,
			TSharedRef<Contract::IUeAgentOperationApplicationPort>
				InOperationPort,
			TArray<TSharedRef<CommandEdge::IUeAgentCommandHandler>>
				InCommandHandlers);

		TSharedRef<FState, ESPMode::ThreadSafe> State;
		TSharedRef<Contract::IUeAgentQueryApplicationPort> QueryPort;
		TSharedRef<Contract::IUeAgentOperationApplicationPort> OperationPort;
		TArray<TSharedRef<CommandEdge::IUeAgentCommandHandler>> CommandHandlers;
		TArray<FString> AuthorityScopes;
	};
}
