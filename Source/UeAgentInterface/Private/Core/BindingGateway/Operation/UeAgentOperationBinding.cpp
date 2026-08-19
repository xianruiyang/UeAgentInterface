#include "Core/BindingGateway/Operation/UeAgentOperationBinding.h"

#include "Core/Contract/UeAgentContractSchema.h"
#include "Core/Verification/UeAgentVerificationOutcomeView.h"

namespace UE::UAI::BindingGateway::Operation
{
	namespace
	{
		using UE::UAI::BindingGateway::Ticket::
			FUeAgentOperationTicketError;
		using UE::UAI::ContextResource::
			FUeAgentCancellationRequested;
		using UE::UAI::ContextResource::
			FUeAgentOperationDeadlineExceeded;
		using UE::UAI::ContextResource::
			FUeAgentResourceBudgetExceeded;
		using UE::UAI::ContextResource::FUeAgentTextUnits;
		using UE::UAI::ContextResource::MeasureText;
		using UE::UAI::Discovery::
			EUeAgentCandidateOperation;
		using UE::UAI::Discovery::FUeAgentCandidateSummary;
		using UE::UAI::Discovery::FUeAgentCatalogRevision;
		using UE::UAI::ExecutionMutation::EExecutionPhase;
		using UE::UAI::ExecutionMutation::EExecutionStatus;
		using UE::UAI::PlanningPreflight::EChangeIntent;
		using UE::UAI::PlanningPreflight::FChangeDraft;
		using UE::UAI::Projection::
			EUeAgentProjectionResultKind;
		using UE::UAI::Projection::
			FUeAgentProjectionError;
		using UE::UAI::Projection::
			FUeAgentProjectionOperationResult;
		using UE::UAI::Projection::
			FUeAgentProjectionSourceValue;
		using UE::UAI::StateSemantic::
			FUeAgentCanonicalValuePtr;
		using UE::UAI::Verification::FVerificationError;
		using UE::UAI::Verification::EEffectOutcome;
		using UE::UAI::Verification::EVerificationStatus;
		using UE::UAI::Verification::LexToString;

		void SetError(
			FUeAgentOperationBindingError& OutError,
			const FString& Code,
			const FString& SubjectId,
			const FString& Detail)
		{
			OutError = {};
			OutError.Code = Code;
			OutError.SubjectId = SubjectId;
			OutError.Detail = Detail;
		}

		void CopyError(
			const FUeAgentOperationTicketError& Source,
			FUeAgentOperationBindingError& OutError)
		{
			SetError(
				OutError,
				Source.Code,
				Source.SubjectId,
				Source.Detail);
		}

		void CopyError(
			const FUeAgentProjectionError& Source,
			FUeAgentOperationBindingError& OutError)
		{
			OutError = {};
			OutError.Code = Source.Code;
			OutError.SubjectId = Source.SubjectId;
			OutError.Detail = Source.Detail;
			OutError.ResourceDimension =
				Source.ResourceDimension;
			OutError.ResourceLimit = Source.ResourceLimit;
			OutError.ResourceObserved =
				Source.ResourceObserved;
		}

		void CopyError(
			const FVerificationError& Source,
			FUeAgentOperationBindingError& OutError)
		{
			SetError(
				OutError,
				Source.GetCode(),
				FString(),
				Source.GetDetail());
		}

		bool SameCaller(
			const FUeAgentCallerContext& Left,
			const FUeAgentCallerContext& Right)
		{
			return Left.GetCallerId() == Right.GetCallerId()
				&& Left.GetAuthorityScopes()
					== Right.GetAuthorityScopes()
				&& Left.GetCorrelationId()
					== Right.GetCorrelationId();
		}

		EUeAgentCandidateOperation CandidateOperation(
			EChangeIntent Intent)
		{
			switch (Intent)
			{
			case EChangeIntent::Create:
				return EUeAgentCandidateOperation::Create;
			case EChangeIntent::Update:
				return EUeAgentCandidateOperation::Update;
			case EChangeIntent::Replace:
				return EUeAgentCandidateOperation::Replace;
			case EChangeIntent::Delete:
				return EUeAgentCandidateOperation::Delete;
			default:
				return EUeAgentCandidateOperation::Read;
			}
		}
	}

	FUeAgentOperationTargetBinding::
		FUeAgentOperationTargetBinding(
			FUeAgentOperationTicket InTicket,
			FUeAgentCandidateBinding InBinding)
		: Ticket(MoveTemp(InTicket))
		, Binding(MoveTemp(InBinding))
	{
		bValid = Ticket.IsValid() && Binding.IsValid();
	}

	const FUeAgentOperationTicket&
	FUeAgentOperationTargetBinding::GetTicket() const
	{
		return Ticket;
	}

	const FUeAgentCandidateBinding&
	FUeAgentOperationTargetBinding::GetBinding() const
	{
		return Binding;
	}

	bool FUeAgentOperationTargetBinding::IsValid() const
	{
		return bValid;
	}

	FUeAgentOperationBindingPayload::
		FUeAgentOperationBindingPayload(
			FUeAgentOperationSessionIdentity InSession,
			TArray<FUeAgentOperationTargetBinding> InTargets,
			FUeAgentExecutionContextInput InExecutionContext,
			FPlanningRequest InPlanning,
			FUeAgentProjectionRequest InProjection)
		: Session(MoveTemp(InSession))
		, Targets(MoveTemp(InTargets))
		, ExecutionContext(MoveTemp(InExecutionContext))
		, Planning(MoveTemp(InPlanning))
		, Projection(MoveTemp(InProjection))
	{
		bValid = Session.IsValid()
			&& !Targets.IsEmpty()
			&& !ExecutionContext.ProjectInstanceId.IsEmpty()
			&& !ExecutionContext.EditorInstanceId.IsEmpty()
			&& !ExecutionContext.ContextRevision.IsEmpty()
			&& Planning.IsSet()
			&& Planning->IsValid()
			&& Projection.IsValid();
		for (const FUeAgentOperationTargetBinding& Target :
			Targets)
		{
			bValid = bValid && Target.IsValid();
		}
	}

	FUeAgentOperationBindingPayload::
		FUeAgentOperationBindingPayload(
			FUeAgentOperationSessionIdentity InSession,
			TArray<FUeAgentOperationTargetBinding> InTargets,
			FUeAgentExecutionContextInput InExecutionContext,
			FUeAgentContractValue InInput,
			TSharedRef<IUeAgentOperationRequestPreparer>
				InPreparer,
			FUeAgentProjectionRequest InProjection)
		: Session(MoveTemp(InSession))
		, Targets(MoveTemp(InTargets))
		, ExecutionContext(MoveTemp(InExecutionContext))
		, Input(MoveTemp(InInput))
		, Preparer(MoveTemp(InPreparer))
		, Projection(MoveTemp(InProjection))
	{
		bValid = Session.IsValid()
			&& !ExecutionContext.ProjectInstanceId.IsEmpty()
			&& !ExecutionContext.EditorInstanceId.IsEmpty()
			&& !ExecutionContext.ContextRevision.IsEmpty()
			&& Input.IsSet()
			&& Input->IsValid()
			&& Preparer.IsValid()
			&& Projection.IsValid();
		for (const FUeAgentOperationTargetBinding& Target :
			Targets)
		{
			bValid = bValid && Target.IsValid();
		}
	}

	const FUeAgentOperationSessionIdentity&
	FUeAgentOperationBindingPayload::GetSession() const
	{
		return Session;
	}

	const TArray<FUeAgentOperationTargetBinding>&
	FUeAgentOperationBindingPayload::GetTargets() const
	{
		return Targets;
	}

	const FUeAgentExecutionContextInput&
	FUeAgentOperationBindingPayload::GetExecutionContext() const
	{
		return ExecutionContext;
	}

	const FPlanningRequest&
	FUeAgentOperationBindingPayload::GetPlanning() const
	{
		return Planning.GetValue();
	}

	bool FUeAgentOperationBindingPayload::RequiresPreparation() const
	{
		return Input.IsSet() && Preparer.IsValid();
	}

	const FUeAgentContractValue&
	FUeAgentOperationBindingPayload::GetInput() const
	{
		return Input.GetValue();
	}

	const TSharedPtr<IUeAgentOperationRequestPreparer>&
	FUeAgentOperationBindingPayload::GetPreparer() const
	{
		return Preparer;
	}

	const FUeAgentProjectionRequest&
	FUeAgentOperationBindingPayload::GetProjection() const
	{
		return Projection;
	}

	bool FUeAgentOperationBindingPayload::IsValid() const
	{
		return bValid;
	}

	FUeAgentOperationBindingResponse::
		FUeAgentOperationBindingResponse(
			FString InOperationId,
			FString InPlanFingerprint,
			TSharedRef<const FUeAgentProjectedResponse>
				InProjection)
		: OperationId(MoveTemp(InOperationId))
		, PlanFingerprint(MoveTemp(InPlanFingerprint))
		, Projection(MoveTemp(InProjection))
	{
		OperationId.TrimStartAndEndInline();
		PlanFingerprint.TrimStartAndEndInline();
		bValid = !OperationId.IsEmpty()
			&& !PlanFingerprint.IsEmpty()
			&& Projection.IsValid()
			&& Projection->IsValid();
	}

	const FString&
	FUeAgentOperationBindingResponse::GetOperationId() const
	{
		return OperationId;
	}

	const FString&
	FUeAgentOperationBindingResponse::GetPlanFingerprint() const
	{
		return PlanFingerprint;
	}

	const TSharedPtr<const FUeAgentProjectedResponse>&
	FUeAgentOperationBindingResponse::GetProjection() const
	{
		return Projection;
	}

	bool FUeAgentOperationBindingResponse::IsValid() const
	{
		return bValid;
	}

	FUeAgentOperationBinding::FUeAgentOperationBinding(
		TSharedRef<
			const UE::UAI::Contract::
				FUeAgentDescriptorRegistry>
			InDescriptorRegistry,
		FUeAgentDescriptorIdentity InDescriptorIdentity,
		TSharedRef<
			const IUeAgentExecutionContextValidator>
			InContextValidator,
		TSharedRef<IUeAgentOperationPlanner> InPlanner,
		TSharedRef<IUeAgentOperationExecutor> InExecutor,
		TSharedRef<IUeAgentOperationVerifier> InVerifier,
		TSharedRef<
			const UE::UAI::Projection::
				FUeAgentResponseProjector> InProjector,
		TSharedRef<IUeAgentOperationSessionSource>
			InSessionSource,
		TSharedRef<const FUeAgentOperationTicketService>
			InTicketService)
		: DescriptorRegistry(MoveTemp(InDescriptorRegistry))
		, DescriptorIdentity(MoveTemp(InDescriptorIdentity))
		, ContextValidator(MoveTemp(InContextValidator))
		, Planner(MoveTemp(InPlanner))
		, Executor(MoveTemp(InExecutor))
		, Verifier(MoveTemp(InVerifier))
		, Projector(MoveTemp(InProjector))
		, SessionSource(MoveTemp(InSessionSource))
		, TicketService(MoveTemp(InTicketService))
	{
	}

	bool FUeAgentOperationBinding::TryCreate(
		TSharedRef<
			const UE::UAI::Contract::
				FUeAgentDescriptorRegistry>
			DescriptorRegistry,
		FUeAgentDescriptorIdentity DescriptorIdentity,
		TSharedRef<
			const IUeAgentExecutionContextValidator>
			ContextValidator,
		TSharedRef<IUeAgentOperationPlanner> Planner,
		TSharedRef<IUeAgentOperationExecutor> Executor,
		TSharedRef<IUeAgentOperationVerifier> Verifier,
		TSharedRef<
			const UE::UAI::Projection::
				FUeAgentResponseProjector> Projector,
		TSharedRef<IUeAgentOperationSessionSource> SessionSource,
		TSharedRef<const FUeAgentOperationTicketService>
			TicketService,
		TSharedPtr<const FUeAgentOperationBinding>& OutBinding,
		FUeAgentOperationBindingError& OutError)
	{
		OutBinding.Reset();
		OutError = {};
		const UE::UAI::Contract::FUeAgentOperationDescriptor*
			Descriptor =
				DescriptorRegistry->FindExact(DescriptorIdentity);
		if (!DescriptorIdentity.IsValid()
			|| !Descriptor
			|| Descriptor->GetOperationKind()
				!= UE::UAI::Contract::
					EUeAgentDescriptorOperationKind::Operation
			|| !DescriptorRegistry->GetRegistryHash().StartsWith(
				TEXT("sha256:")))
		{
			SetError(
				OutError,
				TEXT("operation_binding_invalid"),
				DescriptorIdentity.GetDescriptorId(),
				TEXT(
					"An exact operation descriptor from the "
					"immutable registry is required."));
			return false;
		}
		OutBinding = MakeShareable(
			new FUeAgentOperationBinding(
				MoveTemp(DescriptorRegistry),
				MoveTemp(DescriptorIdentity),
				MoveTemp(ContextValidator),
				MoveTemp(Planner),
				MoveTemp(Executor),
				MoveTemp(Verifier),
				MoveTemp(Projector),
				MoveTemp(SessionSource),
				MoveTemp(TicketService)));
		return true;
	}

	bool FUeAgentOperationBinding::TrySubmit(
		const FUeAgentOperationBindingRequest& Request,
		const FUeAgentCallerContext& CallerContext,
		TSharedRef<
			FUeAgentResourceTracker,
			ESPMode::ThreadSafe> ResourceOwner,
		const FUeAgentOperationDeadline& Deadline,
		const FUeAgentCancellationToken& Cancellation,
		TSharedPtr<const FUeAgentOperationBindingResult>&
			OutResult,
		FUeAgentOperationBindingError& OutError) const
	{
		FUeAgentResourceTracker& Resources = ResourceOwner.Get();
		OutResult.Reset();
		OutError = {};

		FUeAgentCancellationRequested CancellationError;
		if (!Cancellation.TryCheckpoint(CancellationError))
		{
			SetError(
				OutError,
				TEXT("operation_cancelled"),
				Request.GetRequestId(),
				CancellationError.Reason);
			return false;
		}
		FUeAgentOperationDeadlineExceeded DeadlineError;
		if (!Deadline.TryCheckpoint(
				TEXT("c3o.operation_binding.start"),
				DeadlineError))
		{
			SetError(
				OutError,
				TEXT("operation_deadline_exceeded"),
				Request.GetRequestId(),
				DeadlineError.Stage);
			return false;
		}
		if (!Request.IsValid() || !CallerContext.IsValid())
		{
			SetError(
				OutError,
				TEXT("operation_request_invalid"),
				Request.GetRequestId(),
				TEXT(
					"Typed request or caller context is "
					"invalid."));
			return false;
		}
		using UE::UAI::Contract::
			EUeAgentDescriptorCompatibility;
		const EUeAgentDescriptorCompatibility Compatibility =
			DescriptorRegistry->CheckCompatibility(
				Request.GetDescriptorIdentity());
		if (Compatibility
			!= EUeAgentDescriptorCompatibility::Exact
			|| !(Request.GetDescriptorIdentity()
				== DescriptorIdentity))
		{
			const FString Code =
				Compatibility
						== EUeAgentDescriptorCompatibility::
							VersionIncompatible
					? TEXT("descriptor_version_mismatch")
					: Compatibility
							== EUeAgentDescriptorCompatibility::
								NotFound
						? TEXT("descriptor_not_found")
						: TEXT("descriptor_hash_mismatch");
			SetError(
				OutError,
				Code,
				Request.GetDescriptorIdentity()
					.GetDescriptorId(),
				TEXT(
					"Request descriptor identity differs "
					"from OperationBinding."));
			return false;
		}
		const UE::UAI::Contract::FUeAgentOperationDescriptor*
			Descriptor = DescriptorRegistry->FindExact(
				Request.GetDescriptorIdentity());
		if (!Descriptor
			|| Descriptor->GetOperationKind()
				!= UE::UAI::Contract::
					EUeAgentDescriptorOperationKind::Operation)
		{
			SetError(
				OutError,
				TEXT("descriptor_operation_required"),
				Request.GetDescriptorIdentity()
					.GetDescriptorId(),
				TEXT(
					"Exact descriptor is not an operation."));
			return false;
		}

		const FUeAgentOperationBindingPayload& Payload =
			Request.GetPayload();
		const FUeAgentOperationSessionIdentity CurrentSession =
			SessionSource->GetCurrent();
		if (!CurrentSession.IsValid())
		{
			SetError(
				OutError,
				TEXT("operation_session_unavailable"),
				Request.GetRequestId(),
				TEXT("Current server session is invalid."));
			return false;
		}
		if (CurrentSession.GetRegistryHash()
			!= DescriptorRegistry->GetRegistryHash())
		{
			SetError(
				OutError,
				TEXT("registry_hash_mismatch"),
				Request.GetRequestId(),
				TEXT(
					"Current registry hash differs from "
					"OperationBinding composition."));
			return false;
		}
		if (Payload.GetSession().GetServerSessionId()
			!= CurrentSession.GetServerSessionId())
		{
			SetError(
				OutError,
				TEXT("server_session_stale"),
				Request.GetRequestId(),
				TEXT(
					"Request belongs to another server "
					"session."));
			return false;
		}
		if (Payload.GetSession().GetRegistryEpoch()
			!= CurrentSession.GetRegistryEpoch())
		{
			SetError(
				OutError,
				TEXT("registry_epoch_stale"),
				Request.GetRequestId(),
				TEXT("Request registry epoch is stale."));
			return false;
		}
		if (Payload.GetSession().GetRegistryHash()
			!= CurrentSession.GetRegistryHash())
		{
			SetError(
				OutError,
				TEXT("registry_hash_mismatch"),
				Request.GetRequestId(),
				TEXT("Request registry hash is stale."));
			return false;
		}

		FUeAgentExecutionContextRefPtr ExecutionContext;
		FUeAgentExecutionContextError ExecutionContextError;
		if (!FUeAgentExecutionContextRef::TryCreate(
				Payload.GetExecutionContext(),
				Descriptor->GetIdentity(),
				EUeAgentRequestKind::Operation,
				Descriptor->GetExecutionContextPolicy(),
				ExecutionContext,
				ExecutionContextError))
		{
			SetError(
				OutError,
				ExecutionContextError.Code,
				Request.GetRequestId(),
				TEXT(
					"Explicit execution context violates "
					"the exact operation descriptor policy."));
			return false;
		}
		if (!ContextValidator->TryValidate(
				*ExecutionContext,
				ExecutionContextError))
		{
			SetError(
				OutError,
				ExecutionContextError.Code.IsEmpty()
					? TEXT(
						"execution_context_validation_failed")
					: ExecutionContextError.Code,
				Request.GetRequestId(),
				TEXT(
					"Explicit execution context identity or "
					"revision is stale."));
			return false;
		}
		TSharedPtr<FUeAgentOperationContext> OperationContext;
		FUeAgentOperationContextError OperationContextError;
		if (!FUeAgentOperationContext::TryCreate(
				Request,
				CallerContext,
				ExecutionContext.ToSharedRef(),
				ResourceOwner,
				Deadline,
				Cancellation,
				OperationContext,
				OperationContextError))
		{
			SetError(
				OutError,
				OperationContextError.Code,
				Request.GetRequestId(),
				TEXT(
					"OperationContext composition failed."));
			return false;
		}

		TOptional<FPlanningRequest> PreparedPlanning;
		TArray<FUeAgentOperationTargetBinding> EffectiveTargets =
			Payload.GetTargets();
		const FPlanningRequest* PlanningRequestPtr = nullptr;
		if (Payload.RequiresPreparation())
		{
			const UE::UAI::Contract::FUeAgentContractSchema* Schema =
				DescriptorRegistry
					->FindCompiledInputSchemaExact(
						DescriptorIdentity);
			if (!Schema)
			{
				SetError(
					OutError,
					TEXT("operation_input_schema_unavailable"),
					DescriptorIdentity.GetDescriptorId(),
					TEXT(
						"The exact operation descriptor has no "
						"compiled input schema."));
				return false;
			}
			FUeAgentResourceBudgetExceeded InputBudgetError;
			const FUeAgentTextUnits InputUnits =
				MeasureText(Payload.GetInput().GetJsonValue());
			if (!Resources.TryCharge(
					EUeAgentResourceDimension::InputBytes,
					InputUnits.Utf8Bytes,
					TEXT("c3o.operation_binding.input"),
					InputBudgetError))
			{
				SetError(
					OutError,
					InputBudgetError.Code,
					Request.GetRequestId(),
					TEXT("Operation input byte budget exceeded."));
				OutError.ResourceDimension =
					InputBudgetError.Dimension;
				OutError.ResourceLimit = InputBudgetError.Limit;
				OutError.ResourceObserved =
					InputBudgetError.Observed;
				return false;
			}
			const UE::UAI::Contract::FUeAgentContractSchemaCheckpoint
				Checkpoint =
					[OperationContext](
						const int64 WorkUnits,
						const FString& InstancePath,
						UE::UAI::Contract::
							FUeAgentContractSchemaError& Error)
					{
						FUeAgentCancellationRequested Cancelled;
						if (!OperationContext->GetCancellationToken()
								.TryCheckpoint(Cancelled))
						{
							Error.Code = TEXT("operation_cancelled");
							Error.InstancePath = InstancePath;
							Error.Detail = Cancelled.Reason;
							return false;
						}
						FUeAgentOperationDeadlineExceeded Expired;
						if (!OperationContext->GetDeadline()
								.TryCheckpoint(
									TEXT("c3o.operation_binding.schema"),
									Expired))
						{
							Error.Code =
								TEXT("operation_deadline_exceeded");
							Error.InstancePath = InstancePath;
							Error.Detail = Expired.Stage;
							return false;
						}
						FUeAgentResourceBudgetExceeded BudgetError;
						if (!OperationContext->GetResourceTracker()
								.TryCharge(
									EUeAgentResourceDimension::WorkUnits,
									WorkUnits,
									TEXT("c3o.operation_binding.schema"),
									BudgetError))
						{
							Error.Code = BudgetError.Code;
							Error.InstancePath = InstancePath;
							Error.Detail =
								TEXT("Operation schema work budget exceeded.");
							return false;
						}
						return true;
					};
			TOptional<FUeAgentContractValue> CanonicalInput;
			UE::UAI::Contract::FUeAgentContractSchemaError
				SchemaError;
			if (!Schema->TryValidateAndCanonicalize(
					Payload.GetInput(),
					Checkpoint,
					CanonicalInput,
					SchemaError)
				|| !CanonicalInput.IsSet())
			{
				SetError(
					OutError,
					SchemaError.Code.IsEmpty()
						? TEXT("operation_input_invalid")
						: SchemaError.Code,
					SchemaError.InstancePath,
					SchemaError.Detail);
				return false;
			}
			if (!Payload.GetPreparer()->TryPrepare(
					CanonicalInput.GetValue(),
					Payload.GetTargets(),
					*OperationContext,
					PreparedPlanning,
					OutError)
				|| !PreparedPlanning.IsSet()
				|| !PreparedPlanning->IsValid())
			{
				if (!OutError.IsValid())
				{
					SetError(
						OutError,
						TEXT("operation_preparation_failed"),
						Request.GetRequestId(),
						TEXT(
							"Operation preparer returned no valid "
							"PlanningRequest."));
				}
				return false;
			}
			PlanningRequestPtr = &PreparedPlanning.GetValue();
		}
		else
		{
			PlanningRequestPtr = &Payload.GetPlanning();
		}

		const FPlanningRequest& PlanningRequest =
			*PlanningRequestPtr;
		const auto& Draft = PlanningRequest.GetDraft();
		if (!(Draft.GetDescriptorIdentity()
				== Request.GetDescriptorIdentity())
			|| Draft.GetRequestId() != Request.GetRequestId()
			|| Draft.GetIdempotencyKey()
				!= Request.GetIdempotencyKey()
			|| !SameCaller(
				PlanningRequest.GetCallerContext(),
				CallerContext))
		{
			SetError(
				OutError,
				TEXT("operation_request_binding_mismatch"),
				Request.GetRequestId(),
				TEXT(
					"Outer operation request differs from "
					"planning request."));
			return false;
		}

		if (Payload.RequiresPreparation())
		{
			TArray<FUeAgentOperationTargetBinding>
				PreparedTargets;
			PreparedTargets.Reserve(Draft.GetChanges().Num());
			TSet<int32> ConsumedRequestedTargets;
			for (const FChangeDraft& Change : Draft.GetChanges())
			{
				const FUeAgentOperationTargetBinding* Requested =
					nullptr;
				int32 RequestedIndex = INDEX_NONE;
				for (int32 Index = 0;
					Index < Payload.GetTargets().Num();
					++Index)
				{
					if (ConsumedRequestedTargets.Contains(Index))
					{
						continue;
					}
					const FUeAgentCandidateBinding& Candidate =
						Payload.GetTargets()[Index].GetBinding();
					if (Candidate.GetSummary()
							.GetIdentity()
							.GetCanonicalKey()
							== Change.GetIdentity().GetCanonicalKey()
						|| Candidate.GetPublicRef()
							== Change.GetIdentity().GetSelector())
					{
						Requested = &Payload.GetTargets()[Index];
						RequestedIndex = Index;
						break;
					}
				}
				if (!Requested
					&& Payload.GetTargets().Num()
						== Draft.GetChanges().Num())
				{
					const int32 Index = PreparedTargets.Num();
					if (!ConsumedRequestedTargets.Contains(Index))
					{
						Requested = &Payload.GetTargets()[Index];
						RequestedIndex = Index;
					}
				}
				if (RequestedIndex != INDEX_NONE)
				{
					ConsumedRequestedTargets.Add(RequestedIndex);
				}

				const FUeAgentCandidateBinding* RequestedBinding =
					Requested ? &Requested->GetBinding() : nullptr;
				const FString PublicRef = RequestedBinding
					? RequestedBinding->GetPublicRef()
					: Change.GetIdentity().GetSelector();
				const FUeAgentCandidateSummary Summary(
					Change.GetIdentity(),
					DescriptorIdentity,
					RequestedBinding
						? RequestedBinding->GetSummary()
							.GetCatalogKind()
						: Change.GetIdentity().GetTargetKind(),
					RequestedBinding
						? RequestedBinding->GetSummary()
							.GetDisplayName()
						: PublicRef,
					RequestedBinding
						? RequestedBinding->GetSummary().GetSummary()
						: TEXT("C3O prepared operation target"),
					RequestedBinding
						? RequestedBinding->GetSummary()
							.GetCategoryPath()
						: TArray<FString>({
							TEXT("OperationBinding"),
							Descriptor->GetCapabilityId(),
						}));
				const auto& Revision =
					Draft.GetSnapshot()->GetRevision();
				const FUeAgentCandidateBinding Candidate(
					TEXT("c3o-prepared:")
						+ Request.GetRequestId()
						+ TEXT(":")
						+ FString::FromInt(PreparedTargets.Num()),
					Summary,
					FUeAgentCatalogRevision(
						Revision.GetSourceId(),
						Revision.GetGeneration(),
						Revision.GetSequence()),
					PublicRef,
					RequestedBinding
						? RequestedBinding->GetReadPortId()
						: TEXT("c3o.operation_preparation"),
					{
						EUeAgentCandidateOperation::Read,
						CandidateOperation(Change.GetIntent()),
					},
					RequestedBinding
						? RequestedBinding->GetBindingEvidence()
							+ TEXT(":c3o-prepared")
						: TEXT("c3o-operation-preparation"));
				if (!Candidate.IsValid())
				{
					SetError(
						OutError,
						TEXT("operation_prepared_target_invalid"),
						Change.GetIdentity().GetCanonicalKey(),
						TEXT(
							"Preparation did not produce a valid "
							"canonical target binding."));
					return false;
				}
				TOptional<FUeAgentOperationTicket> Ticket;
				FUeAgentOperationTicketError TicketError;
				if (!TicketService->TryIssue(
						Candidate,
						DescriptorIdentity,
						CurrentSession,
						CallerContext,
						Revision,
						Ticket,
						TicketError)
					|| !Ticket.IsSet())
				{
					CopyError(TicketError, OutError);
					return false;
				}
				PreparedTargets.Emplace(
					Ticket.GetValue(),
					Candidate);
			}
			if (ConsumedRequestedTargets.Num()
				!= Payload.GetTargets().Num())
			{
				SetError(
					OutError,
					TEXT("operation_requested_target_unbound"),
					Request.GetRequestId(),
					TEXT(
						"A requested target was not selected by "
						"the prepared ChangeSet."));
				return false;
			}
			EffectiveTargets = MoveTemp(PreparedTargets);
		}

		TMap<FString, const FUeAgentOperationTargetBinding*>
			TargetsByIdentity;
		for (const FUeAgentOperationTargetBinding& Target :
			EffectiveTargets)
		{
			const FString IdentityKey =
				Target.GetBinding()
					.GetSummary()
					.GetIdentity()
					.GetCanonicalKey();
			if (TargetsByIdentity.Contains(IdentityKey))
			{
				SetError(
					OutError,
					TEXT(
						"operation_target_binding_duplicate"),
					IdentityKey,
					TEXT(
						"Duplicate canonical target "
						"binding."));
				return false;
			}
			FUeAgentOperationTicketError TicketError;
			if (!TicketService->TryValidate(
					Target.GetTicket(),
					Target.GetBinding(),
					DescriptorIdentity,
					CurrentSession,
					CallerContext,
					TicketError))
			{
				CopyError(TicketError, OutError);
				return false;
			}
			if (!Target.GetTicket()
				.GetTargetRevision()
				.IsSet())
			{
				SetError(
					OutError,
					TEXT("target_revision_missing"),
					Target.GetTicket().GetTicketId(),
					TEXT(
						"Effect ticket has no target "
						"revision."));
				return false;
			}
			if (!(Target.GetTicket()
					.GetTargetRevision()
					.GetValue()
				== Draft.GetSnapshot()->GetRevision()))
			{
				SetError(
					OutError,
					TEXT("target_revision_stale"),
					Target.GetTicket().GetTicketId(),
					TEXT(
						"Effect ticket target revision "
						"differs from planning snapshot."));
				return false;
			}
			TargetsByIdentity.Add(IdentityKey, &Target);
		}

		TSet<FString> ChangeKeys;
		for (const auto& Change : Draft.GetChanges())
		{
			const FString IdentityKey =
				Change.GetIdentity().GetCanonicalKey();
			if (ChangeKeys.Contains(IdentityKey)
				|| !TargetsByIdentity.Contains(IdentityKey))
			{
				SetError(
					OutError,
					TEXT(
						"operation_target_binding_mismatch"),
					IdentityKey,
					TEXT(
						"Each change must have exactly one "
						"canonical target binding."));
				return false;
			}
			ChangeKeys.Add(IdentityKey);
			const auto* Target =
				TargetsByIdentity.FindChecked(IdentityKey);
			if (!Target->GetBinding()
					.GetSupportedOperations()
					.Contains(
						CandidateOperation(
							Change.GetIntent())))
			{
				SetError(
					OutError,
					TEXT(
						"candidate_operation_unsupported"),
					Target->GetBinding().GetBindingId(),
					TEXT(
						"Candidate does not support "
						"planned change intent."));
				return false;
			}
		}
		if (ChangeKeys.Num() != TargetsByIdentity.Num())
		{
			SetError(
				OutError,
				TEXT("operation_target_binding_mismatch"),
				Request.GetRequestId(),
				TEXT(
					"Targets without a planned change are "
					"not allowed."));
			return false;
		}

		FPlanningResult Planning;
		if (!Planner->TryPlan(
				PlanningRequest,
				*OperationContext,
				Planning,
				OutError))
		{
			return false;
		}
		if (!Planning.IsReady())
		{
			SetError(
				OutError,
				TEXT("operation_planning_result_invalid"),
				Request.GetRequestId(),
				TEXT(
					"Planner did not return a ready "
					"PlanningResult."));
			return false;
		}

		FExecutionReceipt Receipt;
		if (!Executor->TryExecute(
				Planning,
				EffectiveTargets,
				*OperationContext,
				Receipt,
				OutError))
		{
			return false;
		}
		if (!Receipt.IsValid()
			|| Receipt.GetPhase() != EExecutionPhase::Terminal)
		{
			SetError(
				OutError,
				TEXT(
					"operation_execution_receipt_invalid"),
				Request.GetRequestId(),
				TEXT(
					"Executor did not return a terminal "
					"ExecutionReceipt."));
			return false;
		}
		if (Receipt.GetStatus() != EExecutionStatus::AppliedInMemory
			&& Receipt.GetStatus()
				!= EExecutionStatus::PersistedUnverified)
		{
			SetError(
				OutError,
				Receipt.GetErrorCode().IsEmpty()
					? TEXT("operation_execution_failed")
					: Receipt.GetErrorCode(),
				Receipt.GetOperationId(),
				LexToString(Receipt.GetStatus()));
			return false;
		}

		FVerificationOutcome Outcome;
		if (!Verifier->TryVerify(
				Receipt,
				EffectiveTargets,
				*OperationContext,
				Outcome,
				OutError))
		{
			return false;
		}
		if (Outcome.GetStatus() != EVerificationStatus::Verified
			|| Outcome.GetEffectOutcome() != EEffectOutcome::Complete)
		{
			SetError(
				OutError,
				Outcome.GetDiagnosticCode().IsEmpty()
					? TEXT("operation_verification_failed")
					: Outcome.GetDiagnosticCode(),
				Receipt.GetOperationId(),
				FString::Printf(
					TEXT("status=%s; effect_outcome=%s"),
					*LexToString(Outcome.GetStatus()),
					*LexToString(Outcome.GetEffectOutcome())));
			return false;
		}
		FUeAgentCanonicalValuePtr View;
		FVerificationError VerificationError;
		if (!UE::UAI::Verification::
				TryCanonicalizeVerificationOutcome(
					Outcome,
					OperationContext->GetResourceTracker(),
					View,
					VerificationError))
		{
			CopyError(VerificationError, OutError);
			return false;
		}
		if (!View.IsValid())
		{
			SetError(
				OutError,
				TEXT(
					"operation_verification_view_missing"),
				Receipt.GetOperationId(),
				TEXT(
					"Verification canonicalizer returned "
					"no C4 value."));
			return false;
		}

		const FUeAgentProjectionOperationResult ProjectionResult(
			DescriptorIdentity,
			Request.GetRequestId(),
			FUeAgentProjectionSourceValue(
				View.ToSharedRef()));
		TSharedPtr<const FUeAgentProjectedResponse> Projected;
		FUeAgentProjectionError ProjectionError;
		if (!Projector->TryProject(
				ProjectionResult,
				Payload.GetProjection(),
				OperationContext->GetResourceTracker(),
				OperationContext->GetDeadline(),
				OperationContext->GetCancellationToken(),
				Projected,
				ProjectionError))
		{
			CopyError(ProjectionError, OutError);
			return false;
		}
		if (!Projected.IsValid()
			|| !Projected->IsValid()
			|| Projected->GetDescriptorIdentity()
				!= DescriptorIdentity
			|| Projected->GetRequestId()
				!= Request.GetRequestId()
			|| Projected->GetResultKind()
				!= EUeAgentProjectionResultKind::Operation)
		{
			SetError(
				OutError,
				TEXT(
					"operation_projection_identity_mismatch"),
				Request.GetRequestId(),
				TEXT(
					"Projected response does not match "
					"operation identity."));
			return false;
		}

		FUeAgentOperationBindingResponse Response(
			Receipt.GetOperationId(),
			Planning.GetPlan()->GetFingerprint(),
			Projected.ToSharedRef());
		OutResult = MakeShared<FUeAgentOperationBindingResult>(
			DescriptorIdentity,
			Request.GetRequestId(),
			MoveTemp(Response));
		return OutResult->IsValid();
	}
}
