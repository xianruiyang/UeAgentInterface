#include "Core/BindingGateway/Operation/UeAgentOperationBinding.h"
#include "Utilities/AuthoringCore/UeAgentAuthoringDiagnostics.h"

#include "Core/Contract/UeAgentContractSchema.h"
#include "Core/StateSemantic/UeAgentCanonicalValueJson.h"
#include "Core/StateSemantic/UeAgentStateRevisionJson.h"
#include "Core/Verification/UeAgentVerificationOutcomeView.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Misc/ScopeExit.h"

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
			FUeAgentResourceChargeError;
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

		void SetFailure(FUeAgentOperationBindingError& OutError, const FString& Code,
			const FString& SubjectId, const FString& PublicReason)
		{
			OutError = Contract::FDiagnostic::Failure(Code, PublicReason, TEXT("operation_binding"));
			OutError.SubjectId = SubjectId;
		}

		FUeAgentOperationBindingError InvalidBinding(const TCHAR* Code, const TCHAR* Reason, const FString& Path)
		{
			auto Diagnostic = Contract::FDiagnostic::Failure(Code, Reason, TEXT("operation_binding"));
			Diagnostic.PropertyPath = Path;
			Diagnostic.SourceKind = TEXT("native_result");
			return Diagnostic;
		}

		FUeAgentOperationBindingError AtBindingPath(FUeAgentOperationBindingError Diagnostic, const FString& Path)
		{
			if (Diagnostic.PropertyPath.IsEmpty()) Diagnostic.PropertyPath = Path;
			else Diagnostic.PropertyPath = Path + (Diagnostic.PropertyPath.StartsWith(TEXT("/")) ? TEXT("") : TEXT(".")) + Diagnostic.PropertyPath;
			return Diagnostic;
		}

		void SetPrivateMismatch(FUeAgentOperationBindingError& Error, const FString& Code,
			const TCHAR* Reason, const FString& Path, const FString& Actual, const FString& Expected)
		{
			SetFailure(Error, Code, FString(), Reason);
			Error.PropertyPath = Path;
			Error.ActualEvidence = MakeShared<FJsonValueString>(Actual);
			Error.ExpectedEvidence = MakeShared<FJsonValueString>(Expected);
		}

		void AppendProjectionDiagnostics(const TSharedPtr<const FUeAgentProjectedResponse>& Projected, FUeAgentOperationBindingError& Error)
		{
			if (Projected.IsValid())
				for (const auto& Diagnostic : Projected->GetDiagnostics()) Error.Diagnostics.Add(Diagnostic.GetDiagnostic());
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
		if (!Ticket.IsValid()) ValidationDiagnostic = AtBindingPath(Ticket.GetValidationDiagnostic(), TEXT("ticket"));
		else if (!Binding.IsValid()) ValidationDiagnostic = AtBindingPath(*Binding.GetValidationDiagnostic(), TEXT("binding"));
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

	bool FUeAgentOperationTargetBinding::IsValid() const { return ValidationDiagnostic.IsEmpty(); }
	const FUeAgentOperationBindingError& FUeAgentOperationTargetBinding::GetValidationDiagnostic() const { return ValidationDiagnostic; }

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
		Validate(true);
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
		Validate(false);
	}

	void FUeAgentOperationBindingPayload::Validate(bool bPlanningVariant)
	{
		ValidationDiagnostic.Reset();
		if (!Session.IsValid()) ValidationDiagnostic = AtBindingPath(Session.GetValidationDiagnostic(), TEXT("session"));
		else if (bPlanningVariant && Targets.IsEmpty()) ValidationDiagnostic = InvalidBinding(TEXT("operation_targets_missing"),
			TEXT("A prepared operation requires at least one target binding."), TEXT("targets"));
		else if (ExecutionContext.ProjectInstanceId.IsEmpty()) ValidationDiagnostic = InvalidBinding(TEXT("operation_context_project_missing"),
			TEXT("The operation context requires a project instance identifier."), TEXT("execution_context.project_instance_id"));
		else if (ExecutionContext.EditorInstanceId.IsEmpty()) ValidationDiagnostic = InvalidBinding(TEXT("operation_context_editor_missing"),
			TEXT("The operation context requires an editor instance identifier."), TEXT("execution_context.editor_instance_id"));
		else if (ExecutionContext.ContextRevision.IsEmpty()) ValidationDiagnostic = InvalidBinding(TEXT("operation_context_revision_missing"),
			TEXT("The operation context requires a revision."), TEXT("execution_context.context_revision"));
		else if (bPlanningVariant && !Planning.IsSet()) ValidationDiagnostic = InvalidBinding(TEXT("operation_planning_missing"),
			TEXT("The prepared operation has no planning request."), TEXT("planning"));
		else if (bPlanningVariant && !Planning->IsValid()) ValidationDiagnostic = AtBindingPath(Planning->GetValidationDiagnostic(), TEXT("planning"));
		else if (!bPlanningVariant && (!Input.IsSet() || !Input->IsValid())) ValidationDiagnostic = InvalidBinding(TEXT("operation_input_missing"),
			TEXT("The operation has no input value for preparation."), TEXT("input"));
		else if (!bPlanningVariant && !Preparer.IsValid()) ValidationDiagnostic = InvalidBinding(TEXT("operation_preparer_missing"),
			TEXT("The operation has no request preparer."), TEXT("preparer"));
		else if (!Projection.IsValid()) ValidationDiagnostic = AtBindingPath(Projection.GetValidationDiagnostic(), TEXT("projection"));
		if (!ValidationDiagnostic.IsEmpty()) return;
		for (int32 Index = 0; Index < Targets.Num(); ++Index)
		{
			if (!Targets[Index].IsValid())
			{
				ValidationDiagnostic = AtBindingPath(Targets[Index].GetValidationDiagnostic(), FString::Printf(TEXT("targets[%d]"), Index));
				return;
			}
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

	bool FUeAgentOperationBindingPayload::IsValid() const { return ValidationDiagnostic.IsEmpty(); }
	const FUeAgentOperationBindingError& FUeAgentOperationBindingPayload::GetValidationDiagnostic() const { return ValidationDiagnostic; }

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
		if (OperationId.IsEmpty()) ValidationDiagnostic = InvalidBinding(TEXT("operation_result_id_missing"),
			TEXT("The operation result has no operation identifier."), TEXT("operation_id"));
		else if (PlanFingerprint.IsEmpty()) ValidationDiagnostic = InvalidBinding(TEXT("operation_result_plan_missing"),
			TEXT("The operation result has no plan fingerprint."), TEXT("plan_fingerprint"));
		else if (!Projection.IsValid()) ValidationDiagnostic = InvalidBinding(TEXT("operation_result_projection_missing"),
			TEXT("The operation result has no projected response."), TEXT("projection"));
		else if (!Projection->IsValid()) ValidationDiagnostic = AtBindingPath(Projection->GetValidationDiagnostic(), TEXT("projection"));
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

	bool FUeAgentOperationBindingResponse::IsValid() const { return ValidationDiagnostic.IsEmpty(); }
	const FUeAgentOperationBindingError& FUeAgentOperationBindingResponse::GetValidationDiagnostic() const { return ValidationDiagnostic; }

	TArray<Contract::FDiagnostic> FUeAgentOperationBindingResponse::GetDiagnostics() const
	{
		TArray<Contract::FDiagnostic> Diagnostics;
		if (Projection.IsValid())
			for (const auto& Diagnostic : Projection->GetDiagnostics()) Diagnostics.Add(Diagnostic.GetDiagnostic());
		return Diagnostics;
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
		if (!DescriptorIdentity.IsValid())
		{
			OutError = DescriptorIdentity.GetValidationDiagnostic();
			return false;
		}
		if (!Descriptor)
		{
			OutError = InvalidBinding(TEXT("operation_binding_invalid"), TEXT("The exact operation descriptor is not registered."), TEXT("descriptor_identity"));
			OutError.SubjectId = DescriptorIdentity.GetDescriptorId();
			return false;
		}
		if (Descriptor->GetOperationKind() != Contract::EUeAgentDescriptorOperationKind::Operation)
		{
			OutError = InvalidBinding(TEXT("operation_binding_invalid"), TEXT("The registered descriptor is not an operation."), TEXT("descriptor.operation_kind"));
			return false;
		}
		if (!DescriptorRegistry->GetRegistryHash().StartsWith(TEXT("sha256:")))
		{
			OutError = InvalidBinding(TEXT("operation_binding_invalid"), TEXT("The operation registry identity must use a sha256 digest."), TEXT("registry_hash"));
			OutError.ActualEvidence = MakeShared<FJsonValueString>(DescriptorRegistry->GetRegistryHash());
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
			OutError = CancellationError;
			OutError.PublicCause = MakeShared<UE::UAI::Contract::FDiagnostic>(CancellationError);
			OutError.Code = TEXT("operation_cancelled");
			OutError.SubjectId = Request.GetRequestId();
			return false;
		}
		if (!Request.GetDescriptorIdentity().IsValid()) OutError = Request.GetDescriptorIdentity().GetValidationDiagnostic();
		else if (Request.GetRequestId().IsEmpty()) OutError = InvalidBinding(TEXT("operation_request_invalid"),
			TEXT("The operation request requires a non-empty request identifier."), TEXT("request_id"));
		else if (Request.GetIdempotencyKey().IsEmpty()) OutError = InvalidBinding(TEXT("operation_request_invalid"),
			TEXT("The operation request requires a non-empty idempotency key."), TEXT("idempotency_key"));
		else if (!Request.GetPayload().IsValid()) OutError = Request.GetPayload().GetValidationDiagnostic();
		else if (!CallerContext.IsValid()) OutError = CallerContext.GetValidationDiagnostic();
		if (!OutError.IsEmpty()) return false;
		using UE::UAI::Contract::
			EUeAgentDescriptorCompatibility;
		const EUeAgentDescriptorCompatibility Compatibility =
			DescriptorRegistry->CheckCompatibility(
				Request.GetDescriptorIdentity());
		if (Compatibility != EUeAgentDescriptorCompatibility::Exact || !(Request.GetDescriptorIdentity() == DescriptorIdentity))
		{
			const auto& Actual = Request.GetDescriptorIdentity();
			if (Compatibility == EUeAgentDescriptorCompatibility::NotFound)
			{
				OutError = InvalidBinding(TEXT("descriptor_not_found"), TEXT("The requested operation descriptor is not registered."), TEXT("descriptor_identity"));
				OutError.SubjectId = Actual.GetDescriptorId();
			}
			else if (Compatibility == EUeAgentDescriptorCompatibility::VersionIncompatible || Actual.GetVersion() != DescriptorIdentity.GetVersion())
				SetPrivateMismatch(OutError, TEXT("descriptor_version_mismatch"), TEXT("The request descriptor version is incompatible with the operation binding."),
					TEXT("descriptor_identity.version"), FString::FromInt(Actual.GetVersion()), FString::FromInt(DescriptorIdentity.GetVersion()));
			else if (Actual.GetDescriptorId() != DescriptorIdentity.GetDescriptorId())
				SetPrivateMismatch(OutError, TEXT("descriptor_hash_mismatch"), TEXT("The request selects a different operation descriptor from this binding."),
					TEXT("descriptor_identity.descriptor_id"), Actual.GetDescriptorId(), DescriptorIdentity.GetDescriptorId());
			else SetPrivateMismatch(OutError, TEXT("descriptor_hash_mismatch"), TEXT("The request descriptor digest differs from the registered operation binding."),
				TEXT("descriptor_identity.descriptor_hash"), Actual.GetDescriptorHash(), DescriptorIdentity.GetDescriptorHash());
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
			SetFailure(
				OutError,
				TEXT("descriptor_operation_required"),
				Request.GetDescriptorIdentity()
					.GetDescriptorId(),
				Descriptor ? TEXT("The exact descriptor does not declare an operation.")
					: TEXT("The registry did not return the exact operation descriptor."));
			OutError.PropertyPath = Descriptor ? TEXT("descriptor.operation_kind") : TEXT("descriptor_identity");
			return false;
		}

		const FUeAgentOperationBindingPayload& Payload =
			Request.GetPayload();
		const FUeAgentOperationSessionIdentity CurrentSession =
			SessionSource->GetCurrent();
		if (!CurrentSession.IsValid())
		{
			OutError = AtBindingPath(CurrentSession.GetValidationDiagnostic(), TEXT("current_session"));
			return false;
		}
		if (CurrentSession.GetRegistryHash() != DescriptorRegistry->GetRegistryHash())
		{
			SetPrivateMismatch(OutError, TEXT("registry_hash_mismatch"), TEXT("The current session and operation binding use different registry identities."),
				TEXT("current_session.registry_hash"), CurrentSession.GetRegistryHash(), DescriptorRegistry->GetRegistryHash());
			return false;
		}
		if (Payload.GetSession().GetServerSessionId() != CurrentSession.GetServerSessionId())
		{
			SetPrivateMismatch(OutError, TEXT("server_session_stale"), TEXT("The operation request belongs to a different server session."),
				TEXT("session.server_session_id"), Payload.GetSession().GetServerSessionId(), CurrentSession.GetServerSessionId());
			return false;
		}
		if (Payload.GetSession().GetRegistryEpoch() != CurrentSession.GetRegistryEpoch())
		{
			SetPrivateMismatch(OutError, TEXT("registry_epoch_stale"), TEXT("The request registry epoch is stale."), TEXT("session.registry_epoch"),
				FString::Printf(TEXT("%lld"), Payload.GetSession().GetRegistryEpoch()), FString::Printf(TEXT("%lld"), CurrentSession.GetRegistryEpoch()));
			return false;
		}
		if (Payload.GetSession().GetRegistryHash() != CurrentSession.GetRegistryHash())
		{
			SetPrivateMismatch(OutError, TEXT("registry_hash_mismatch"), TEXT("The request registry identity is stale."),
				TEXT("session.registry_hash"), Payload.GetSession().GetRegistryHash(), CurrentSession.GetRegistryHash());
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
			OutError = ExecutionContextError;
			if (OutError.SubjectId.IsEmpty()) OutError.SubjectId = Request.GetRequestId();
			return false;
		}
		if (!ContextValidator->TryValidate(
				*ExecutionContext,
				ExecutionContextError))
		{
			OutError = !ExecutionContextError.IsEmpty() ? ExecutionContextError : UE::UAI::Contract::FDiagnostic::Failure(TEXT("execution_context_validation_failed"), TEXT(
					"Explicit execution context identity or "
					"revision is stale."), TEXT("context"));
			if (OutError.SubjectId.IsEmpty()) OutError.SubjectId = Request.GetRequestId();
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
			OutError = OperationContextError;
			if (OutError.IsEmpty()) OutError = InvalidBinding(TEXT("operation_context_creation_failed"), TEXT("The operation context could not be created and returned no diagnostic."), TEXT("context"));
			if (OutError.SubjectId.IsEmpty()) OutError.SubjectId = Request.GetRequestId();
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
				SetFailure(
					OutError,
					TEXT("operation_input_schema_unavailable"),
					DescriptorIdentity.GetDescriptorId(),
					TEXT(
						"The exact operation descriptor has no "
						"compiled input schema."));
				return false;
			}
			FUeAgentResourceChargeError InputChargeError;
			const FUeAgentTextUnits InputUnits =
				MeasureText(Payload.GetInput().GetJsonValue());
			if (!Resources.TryCharge(
					EUeAgentResourceDimension::InputBytes,
					InputUnits.Utf8Bytes,
					TEXT("c3o.operation_binding.input"),
					InputChargeError))
			{
				OutError = InputChargeError;
				OutError.SubjectId = Request.GetRequestId();
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
							Error = UE::UAI::Contract::FDiagnostic::WrapFailure(
								TEXT("operation_cancelled"), Cancelled);
							if (Error.Source.JsonPath.IsEmpty()) Error.Source.JsonPath = InstancePath;
							return false;
						}
						FUeAgentResourceChargeError ChargeError;
						if (!OperationContext->GetResourceTracker()
								.TryCharge(
									EUeAgentResourceDimension::WorkUnits,
									WorkUnits,
									TEXT("c3o.operation_binding.schema"),
									ChargeError))
						{
							Error = ChargeError;
							if (Error.Source.JsonPath.IsEmpty()) Error.Source.JsonPath = InstancePath;
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
				OutError = !SchemaError.IsEmpty() ? SchemaError : UE::UAI::Contract::FDiagnostic::Failure(TEXT("operation_input_invalid"), TEXT("The input validator returned no canonical input or failure diagnostic."), TEXT("operation_binding"));
				if (OutError.SubjectId.IsEmpty()) OutError.SubjectId = SchemaError.Source.JsonPath;
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
				if (OutError.IsEmpty() && PreparedPlanning.IsSet() && !PreparedPlanning->IsValid())
					OutError = PreparedPlanning->GetValidationDiagnostic();
				if (OutError.IsEmpty())
				{
					SetFailure(
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
		if (!(Draft.GetDescriptorIdentity() == Request.GetDescriptorIdentity()))
		{
			OutError = InvalidBinding(TEXT("operation_request_binding_mismatch"),
				TEXT("The prepared plan descriptor differs from the submitted operation."), TEXT("planning.descriptor_identity"));
			OutError.ActualEvidence = MakeShared<FJsonValueString>(Draft.GetDescriptorIdentity().GetDescriptorId() + TEXT(":")
				+ FString::FromInt(Draft.GetDescriptorIdentity().GetVersion()) + TEXT(":") + Draft.GetDescriptorIdentity().GetDescriptorHash());
			OutError.ExpectedEvidence = MakeShared<FJsonValueString>(Request.GetDescriptorIdentity().GetDescriptorId() + TEXT(":")
				+ FString::FromInt(Request.GetDescriptorIdentity().GetVersion()) + TEXT(":") + Request.GetDescriptorIdentity().GetDescriptorHash());
		}
		else if (Draft.GetRequestId() != Request.GetRequestId())
			SetPrivateMismatch(OutError, TEXT("operation_request_binding_mismatch"), TEXT("The prepared plan belongs to another request."),
				TEXT("planning.request_id"), Draft.GetRequestId(), Request.GetRequestId());
		else if (Draft.GetIdempotencyKey() != Request.GetIdempotencyKey())
			SetPrivateMismatch(OutError, TEXT("operation_request_binding_mismatch"), TEXT("The prepared plan and request have different idempotency keys."),
				TEXT("planning.idempotency_key"), Draft.GetIdempotencyKey(), Request.GetIdempotencyKey());
		else if (!SameCaller(PlanningRequest.GetCallerContext(), CallerContext))
		{
			const auto& Actual = PlanningRequest.GetCallerContext();
			if (Actual.GetCallerId() != CallerContext.GetCallerId())
				SetPrivateMismatch(OutError, TEXT("operation_request_binding_mismatch"), TEXT("The prepared plan belongs to a different caller."),
					TEXT("planning.caller.caller_id"), Actual.GetCallerId(), CallerContext.GetCallerId());
			else if (Actual.GetAuthorityScopes() != CallerContext.GetAuthorityScopes())
				SetPrivateMismatch(OutError, TEXT("operation_request_binding_mismatch"), TEXT("The prepared plan and caller have different authority scopes."),
					TEXT("planning.caller.authority_scopes"), FString::Join(Actual.GetAuthorityScopes(), TEXT(",")), FString::Join(CallerContext.GetAuthorityScopes(), TEXT(",")));
			else SetPrivateMismatch(OutError, TEXT("operation_request_binding_mismatch"), TEXT("The prepared plan has a different caller correlation identifier."),
				TEXT("planning.caller.correlation_id"), Actual.GetCorrelationId(), CallerContext.GetCorrelationId());
		}
		if (!OutError.IsEmpty()) return false;

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
					OutError = AtBindingPath(*Candidate.GetValidationDiagnostic(), FString::Printf(TEXT("prepared_targets[%d]"), PreparedTargets.Num()));
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
					OutError = TicketError;
					if (OutError.IsEmpty())
						OutError = InvalidBinding(TEXT("operation_ticket_missing"), TEXT("The ticket issuer did not return an operation ticket."), FString::Printf(TEXT("prepared_targets[%d].ticket"), PreparedTargets.Num()));
					return false;
				}
				PreparedTargets.Emplace(
					Ticket.GetValue(),
					Candidate);
			}
			if (ConsumedRequestedTargets.Num()
				!= Payload.GetTargets().Num())
			{
				SetFailure(
					OutError,
					TEXT("operation_requested_target_unbound"),
					Request.GetRequestId(),
					TEXT(
						"A requested target was not selected by "
						"the prepared ChangeSet."));
				OutError.PropertyPath = TEXT("targets");
				TArray<TSharedPtr<FJsonValue>> UnboundTargets;
				for (int32 Index = 0; Index < Payload.GetTargets().Num(); ++Index)
				{
					if (!ConsumedRequestedTargets.Contains(Index))
					{
						const TSharedRef<FJsonObject> Item = MakeShared<FJsonObject>();
						Item->SetNumberField(TEXT("index"), Index);
						Item->SetStringField(TEXT("target"), Payload.GetTargets()[Index].GetBinding().GetPublicRef());
						UnboundTargets.Add(MakeShared<FJsonValueObject>(Item));
					}
				}
				OutError.ActualValue = MakeShared<FJsonValueArray>(MoveTemp(UnboundTargets));
				return false;
			}
			EffectiveTargets = MoveTemp(PreparedTargets);
		}

		TMap<FString, const FUeAgentOperationTargetBinding*>
			TargetsByIdentity;
		for (int32 TargetIndex = 0; TargetIndex < EffectiveTargets.Num(); ++TargetIndex)
		{
			const FUeAgentOperationTargetBinding& Target = EffectiveTargets[TargetIndex];
			const FString TargetPath = FString::Printf(TEXT("targets[%d]"), TargetIndex);
			const FString IdentityKey =
				Target.GetBinding()
					.GetSummary()
					.GetIdentity()
					.GetCanonicalKey();
			if (TargetsByIdentity.Contains(IdentityKey))
			{
				SetFailure(
					OutError,
					TEXT(
						"operation_target_binding_duplicate"),
					IdentityKey,
					TEXT(
						"Duplicate canonical target "
						"binding."));
				OutError.PropertyPath = TargetPath;
				OutError.TargetPath = Target.GetBinding().GetPublicRef();
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
				OutError = TicketError.IsEmpty()
					? InvalidBinding(TEXT("operation_ticket_validation_failed"), TEXT("The target ticket validator failed without a diagnostic."), TargetPath + TEXT(".ticket"))
					: AtBindingPath(TicketError, TargetPath + TEXT(".ticket"));
				if (OutError.TargetPath.IsEmpty()) OutError.TargetPath = Target.GetBinding().GetPublicRef();
				return false;
			}
			if (!Target.GetTicket()
				.GetTargetRevision()
				.IsSet())
			{
				SetFailure(
					OutError,
					TEXT("target_revision_missing"),
					Target.GetTicket().GetTicketId(),
					TEXT(
						"Effect ticket has no target "
						"revision."));
				OutError.PropertyPath = TargetPath + TEXT(".ticket.target_revision");
				OutError.TargetPath = Target.GetBinding().GetPublicRef();
				return false;
			}
			if (!(Target.GetTicket()
					.GetTargetRevision()
					.GetValue()
				== Draft.GetSnapshot()->GetRevision()))
			{
				SetFailure(
					OutError,
					TEXT("target_revision_stale"),
					Target.GetTicket().GetTicketId(),
					TEXT(
						"Effect ticket target revision "
						"differs from planning snapshot."));
				OutError.PropertyPath = TargetPath + TEXT(".ticket.target_revision");
				OutError.TargetPath = Target.GetBinding().GetPublicRef();
				OutError.ActualEvidence = MakeShared<FJsonValueObject>(StateSemantic::FUeAgentStateRevisionJson::ToJson(Target.GetTicket().GetTargetRevision().GetValue()));
				OutError.ExpectedEvidence = MakeShared<FJsonValueObject>(StateSemantic::FUeAgentStateRevisionJson::ToJson(Draft.GetSnapshot()->GetRevision()));
				return false;
			}
			TargetsByIdentity.Add(IdentityKey, &Target);
		}

		TSet<FString> ChangeKeys;
		for (int32 ChangeIndex = 0; ChangeIndex < Draft.GetChanges().Num(); ++ChangeIndex)
		{
			const auto& Change = Draft.GetChanges()[ChangeIndex];
			const FString IdentityKey =
				Change.GetIdentity().GetCanonicalKey();
			if (ChangeKeys.Contains(IdentityKey)
				|| !TargetsByIdentity.Contains(IdentityKey))
			{
				SetFailure(
					OutError,
					TEXT(
						"operation_target_binding_mismatch"),
					IdentityKey,
					ChangeKeys.Contains(IdentityKey)
						? TEXT("A second change refers to the same canonical target.")
						: TEXT("The planned change has no matching target binding."));
				OutError.PropertyPath = FString::Printf(TEXT("changes[%d].target"), ChangeIndex);
				OutError.TargetPath = Change.GetIdentity().GetSelector();
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
				SetFailure(
					OutError,
					TEXT(
						"candidate_operation_unsupported"),
					Target->GetBinding().GetBindingId(),
					TEXT(
						"Candidate does not support "
						"planned change intent."));
				OutError.PropertyPath = FString::Printf(TEXT("changes[%d].intent"), ChangeIndex);
				OutError.TargetPath = Target->GetBinding().GetPublicRef();
				OutError.ActualValue = MakeShared<FJsonValueString>(PlanningPreflight::LexToString(Change.GetIntent()));
				return false;
			}
		}
		if (ChangeKeys.Num() != TargetsByIdentity.Num())
		{
			SetFailure(
				OutError,
				TEXT("operation_target_binding_mismatch"),
				Request.GetRequestId(),
				TEXT(
					"Targets without a planned change are "
					"not allowed."));
			OutError.PropertyPath = TEXT("targets");
			TArray<TSharedPtr<FJsonValue>> UnusedTargets;
			for (int32 Index = 0; Index < EffectiveTargets.Num(); ++Index)
			{
				const auto& Binding = EffectiveTargets[Index].GetBinding();
				if (!ChangeKeys.Contains(Binding.GetSummary().GetIdentity().GetCanonicalKey()))
				{
					const TSharedRef<FJsonObject> Item = MakeShared<FJsonObject>();
					Item->SetNumberField(TEXT("index"), Index);
					Item->SetStringField(TEXT("target"), Binding.GetPublicRef());
					UnusedTargets.Add(MakeShared<FJsonValueObject>(Item));
				}
			}
			OutError.ActualValue = MakeShared<FJsonValueArray>(MoveTemp(UnusedTargets));
			return false;
		}

		FPlanningResult Planning;
		if (!Planner->TryPlan(
				PlanningRequest,
				*OperationContext,
				Planning,
				OutError))
		{
			if (OutError.IsEmpty()) OutError = InvalidBinding(TEXT("operation_planning_failed"), TEXT("The planner failed without returning a diagnostic."), TEXT("planning"));
			return false;
		}
		if (!Planning.IsReady())
		{
			SetFailure(
				OutError,
				TEXT("operation_planning_result_invalid"),
				Request.GetRequestId(),
				TEXT(
					"Planner did not return a ready "
					"PlanningResult."));
			OutError.PropertyPath = Planning.GetPlan().IsValid() ? TEXT("planning.status") : TEXT("planning.plan");
			OutError.PublicMessage = Planning.GetPlan().IsValid()
				? TEXT("The planner returned a status that cannot be executed.") : TEXT("The planner returned no executable plan.");
			if (Planning.GetPlan().IsValid())
			{
				OutError.ActualValue = MakeShared<FJsonValueString>(Planning.GetStatus());
				OutError.ExpectedValue = MakeShared<FJsonValueString>(TEXT("ready or no_change"));
			}
			return false;
		}

		FExecutionReceipt Receipt;
		FVerificationOutcome Outcome;
		FString FailureStage = TEXT("execution");
		ON_SCOPE_EXIT
		{
			if (!OutError.IsEmpty())
			{
				OutError.Schema = TEXT("uai.operation.failure/1");
				if (OutError.Stage.IsEmpty()) OutError.Stage = FailureStage;
				const bool bReceiptKnown = Receipt.IsValid();
				OutError.MutationMayHaveStarted = !bReceiptKnown || Receipt.DidMutationStart();
				OutError.bRetrySafe = bReceiptKnown && !Receipt.DidMutationStart() && Receipt.IsRetrySafe();
				if (bReceiptKnown)
				{
					OutError.MutationStarted = Receipt.DidMutationStart();
					OutError.ExecutionStatus = LexToString(Receipt.GetStatus());
				}
				if (FailureStage != TEXT("execution"))
				{
					OutError.VerificationStatus = LexToString(Outcome.GetStatus());
					OutError.EffectOutcome = LexToString(Outcome.GetEffectOutcome());
					FUeAgentCanonicalValuePtr VerificationView;
					FVerificationError ViewError;
					TSharedPtr<FJsonValue> VerificationJson;
					UE::UAI::Contract::FDiagnostic JsonError;
					if (UE::UAI::Verification::TryCanonicalizeVerificationOutcome(
						Outcome, OperationContext->GetResourceTracker(), VerificationView, ViewError)
						&& VerificationView.IsValid()
						&& UE::UAI::StateSemantic::CanonicalJson::TryToJsonValue(*VerificationView, VerificationJson, JsonError))
					{
						if (VerificationJson && VerificationJson->Type == EJson::Object)
							OutError.Verification = VerificationJson->AsObject();
					}
					else
					{
						const auto& ProjectionError = ViewError.IsEmpty() ? JsonError : ViewError;
						OutError.Diagnostics.Add(ProjectionError.IsEmpty()
							? UE::UAI::Contract::FDiagnostic::Failure(TEXT("verification_failure_view_unavailable"), TEXT("The verification failure could not be represented as a canonical value."), TEXT("verification"))
							: ProjectionError);
					}
				}
			}
		};
		if (!Executor->TryExecute(
				Planning,
				EffectiveTargets,
				*OperationContext,
				Receipt,
				OutError))
		{
			if (OutError.IsEmpty())
			{
				OutError = Receipt.GetDiagnostic();
				if (OutError.IsEmpty()) OutError = InvalidBinding(TEXT("operation_execution_failed"), TEXT("The executor failed without returning a diagnostic."), TEXT("execution"));
			}
			return false;
		}
		if (!Receipt.IsValid())
		{
			OutError = AtBindingPath(Receipt.GetValidationDiagnostic(), TEXT("execution_receipt"));
			if (!Receipt.GetDiagnostic().IsEmpty()) OutError.Diagnostics.Add(Receipt.GetDiagnostic());
			return false;
		}
		if (Receipt.GetStatus() != EExecutionStatus::AppliedInMemory
			&& Receipt.GetStatus()
				!= EExecutionStatus::PersistedUnverified)
		{
			OutError = Receipt.GetDiagnostic().IsEmpty()
				? UE::UAI::Contract::FDiagnostic::Failure(TEXT("operation_execution_failed"), TEXT("The operation did not reach a successful execution state."), TEXT("execution"))
				: Receipt.GetDiagnostic();
			if (OutError.SubjectId.IsEmpty()) OutError.SubjectId = Receipt.GetOperationId();
			return false;
		}

		FailureStage = TEXT("verification");
		if (!Verifier->TryVerify(
				Receipt,
				EffectiveTargets,
				*OperationContext,
				Outcome,
				OutError))
		{
			if (OutError.IsEmpty())
			{
				OutError = Outcome.GetDiagnostic();
				if (OutError.IsEmpty()) OutError = InvalidBinding(TEXT("operation_verification_failed"), TEXT("The verifier failed without returning a diagnostic."), TEXT("verification"));
			}
			return false;
		}
		if (Outcome.GetStatus() != EVerificationStatus::Verified
			|| Outcome.GetEffectOutcome() != EEffectOutcome::Complete)
		{
			UeAgentAuthoringDiagnostics::ReportConsistencyWarning(
				Outcome.GetDiagnosticCode().IsEmpty() ? TEXT("operation_verification_failed") : Outcome.GetDiagnosticCode(),
				Outcome.GetDiagnostic(), TEXT("operation_observation"));
		}
		FUeAgentCanonicalValuePtr View;
		FailureStage = TEXT("projection");
		FVerificationError VerificationError;
		if (!UE::UAI::Verification::
				TryCanonicalizeVerificationOutcome(
					Outcome,
					OperationContext->GetResourceTracker(),
					View,
					VerificationError))
		{
			OutError = VerificationError;
			if (OutError.IsEmpty()) OutError = InvalidBinding(TEXT("operation_verification_view_failed"), TEXT("The verification canonicalizer failed without returning a diagnostic."), TEXT("verification"));
			return false;
		}
		if (!View.IsValid())
		{
			SetFailure(
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
			OutError = ProjectionError;
			if (OutError.IsEmpty()) OutError = InvalidBinding(TEXT("operation_projection_failed"), TEXT("The projector failed without returning a diagnostic."), TEXT("projection"));
			AppendProjectionDiagnostics(Projected, OutError);
			return false;
		}
		if (!Projected.IsValid())
		{
			OutError = InvalidBinding(TEXT("operation_projection_identity_mismatch"), TEXT("The projector returned no operation response."), TEXT("projection"));
			return false;
		}
		if (!Projected->IsValid()) OutError = Projected->GetValidationDiagnostic();
		else if (Projected->GetDescriptorIdentity() != DescriptorIdentity)
		{
			OutError = InvalidBinding(TEXT("operation_projection_identity_mismatch"), TEXT("The projected response belongs to a different operation descriptor."), TEXT("projection.descriptor_identity"));
			OutError.ActualEvidence = MakeShared<FJsonValueString>(Projected->GetDescriptorIdentity().GetDescriptorId() + TEXT(":")
				+ FString::FromInt(Projected->GetDescriptorIdentity().GetVersion()) + TEXT(":") + Projected->GetDescriptorIdentity().GetDescriptorHash());
			OutError.ExpectedEvidence = MakeShared<FJsonValueString>(DescriptorIdentity.GetDescriptorId() + TEXT(":")
				+ FString::FromInt(DescriptorIdentity.GetVersion()) + TEXT(":") + DescriptorIdentity.GetDescriptorHash());
		}
		else if (Projected->GetRequestId() != Request.GetRequestId())
			SetPrivateMismatch(OutError, TEXT("operation_projection_identity_mismatch"), TEXT("The projected response belongs to a different request."),
				TEXT("projection.request_id"), Projected->GetRequestId(), Request.GetRequestId());
		else if (Projected->GetResultKind() != EUeAgentProjectionResultKind::Operation)
		{
			OutError = InvalidBinding(TEXT("operation_projection_identity_mismatch"), TEXT("The projected response is not an operation result."), TEXT("projection.result_kind"));
			OutError.ActualValue = MakeShared<FJsonValueString>(TEXT("query"));
			OutError.ExpectedValue = MakeShared<FJsonValueString>(TEXT("operation"));
		}
		if (!OutError.IsEmpty())
		{
			AppendProjectionDiagnostics(Projected, OutError);
			return false;
		}

		FUeAgentOperationBindingResponse Response(
			Receipt.GetOperationId(),
			Planning.GetPlan()->GetFingerprint(),
			Projected.ToSharedRef());
		if (!Response.IsValid())
		{
			OutError = Response.GetValidationDiagnostic();
			AppendProjectionDiagnostics(Projected, OutError);
			return false;
		}
		OutResult = MakeShared<FUeAgentOperationBindingResult>(
			DescriptorIdentity,
			Request.GetRequestId(),
			MoveTemp(Response));
		return true;
	}
}
