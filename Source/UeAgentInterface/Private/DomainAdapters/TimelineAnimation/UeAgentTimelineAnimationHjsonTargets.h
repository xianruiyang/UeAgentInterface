#pragma once

#include "Composition/HjsonImport/UeAgentHjsonImportTargetRegistry.h"
#include "Core/BindingGateway/Operation/UeAgentOperationBinding.h"

namespace UE::UAI::DomainAdapters::TimelineAnimation
{
	/**
	 * Request-scoped native owner for one resolved HJSON import.
	 *
	 * It never parses HJSON. The immutable resolved workspace is converted to
	 * a typed ChangeSet during prepare, then the same OperationContext is
	 * required by planning, mutation and independent persisted verification.
	 */
	class UEAGENTINTERFACE_API FUeAgentTimelineAnimationHjsonOperation final
		: public Composition::HjsonImport::IUeAgentHjsonImportOperation
	{
	public:
		class FState;

		static bool TryCreate(
			Composition::HjsonImport::FUeAgentHjsonTargetImportRequest Request,
			TSharedPtr<FUeAgentTimelineAnimationHjsonOperation>& OutOperation,
			Composition::HjsonImport::FUeAgentHjsonImportTargetError& OutError);

		bool TryPrepare(
			const Contract::FUeAgentContractValue& CanonicalInput,
			const TArray<BindingGateway::Operation::
				FUeAgentOperationTargetBinding>& RequestedTargets,
			const ContextResource::FUeAgentOperationContext& Context,
			TOptional<PlanningPreflight::FPlanningRequest>& OutPlanning,
			BindingGateway::Operation::FUeAgentOperationBindingError& OutError)
			override;

		bool TryPlan(
			const PlanningPreflight::FPlanningRequest& Request,
			const ContextResource::FUeAgentOperationContext& Context,
			PlanningPreflight::FPlanningResult& OutResult,
			BindingGateway::Operation::FUeAgentOperationBindingError& OutError)
			override;

		bool TryExecute(
			const PlanningPreflight::FPlanningResult& Planning,
			const TArray<BindingGateway::Operation::
				FUeAgentOperationTargetBinding>& Targets,
			const ContextResource::FUeAgentOperationContext& Context,
			ExecutionMutation::FExecutionReceipt& OutReceipt,
			BindingGateway::Operation::FUeAgentOperationBindingError& OutError)
			override;

		bool TryVerify(
			const ExecutionMutation::FExecutionReceipt& Receipt,
			const TArray<BindingGateway::Operation::
				FUeAgentOperationTargetBinding>& Targets,
			const ContextResource::FUeAgentOperationContext& Context,
			Verification::FVerificationOutcome& OutOutcome,
			BindingGateway::Operation::FUeAgentOperationBindingError& OutError)
			override;

		const Composition::HjsonImport::FUeAgentHjsonTargetImportReceipt&
			GetImportReceipt() const override;
		bool UsedOneOperationContext() const override;

	private:
		explicit FUeAgentTimelineAnimationHjsonOperation(
			TSharedRef<FState, ESPMode::ThreadSafe> InState);

		TSharedRef<FState, ESPMode::ThreadSafe> State;
	};

	UEAGENTINTERFACE_API bool TryExportTimelineAnimationHjsonTarget(
		const Composition::HjsonImport::FUeAgentHjsonTargetExportRequest& Request,
		Composition::HjsonImport::FUeAgentHjsonTargetExportReceipt& OutReceipt,
		Composition::HjsonImport::FUeAgentHjsonImportTargetError& OutError);

}
