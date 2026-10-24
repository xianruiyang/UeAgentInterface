#include "Core/BindingGateway/Operation/UeAgentOperationLifecycleService.h"
#include "Utilities/AuthoringCore/UeAgentAuthoringDiagnostics.h"
#include "Dom/JsonValue.h"

namespace UE::UAI::BindingGateway::Operation
{
	namespace
	{
		FString Normalize(FString Value)
		{
			Value.TrimStartAndEndInline();
			return Value;
		}

		FOperationLifecycleError InvalidInput(const TCHAR* Code, const FString& Reason, const FString& Path)
		{
			auto Error = FOperationLifecycleError::Failure(Code, Reason, TEXT("operation_lifecycle"));
			Error.PropertyPath = Path;
			return Error;
		}

		bool NormalizeSet(TArray<FString>& Values, FOperationLifecycleError* OutError = nullptr,
			const TCHAR* Code = TEXT("operation_value_set_invalid"), const FString& Path = FString())
		{
			TMap<FString, int32> FirstIndices;
			TMap<FString, int32> DuplicateIndices;
			for (int32 Index = 0; Index < Values.Num(); ++Index)
			{
				FString& Value = Values[Index];
				Value = Normalize(MoveTemp(Value));
				if (Value.IsEmpty())
				{
					if (OutError)
					{
						*OutError = InvalidInput(Code, TEXT("The set contains an empty value after trimming whitespace."), FString::Printf(TEXT("%s[%d]"), *Path, Index));
						OutError->ActualEvidence = MakeShared<FJsonValueString>(Value);
					}
					return false;
				}
				if (OutError)
				{
					if (FirstIndices.Contains(Value))
					{
						if (!DuplicateIndices.Contains(Value)) DuplicateIndices.Add(Value, Index);
					}
					else FirstIndices.Add(Value, Index);
				}
			}
			Values.Sort();
			for (int32 Index = 1; Index < Values.Num(); ++Index)
			{
				if (Values[Index - 1] == Values[Index])
				{
					if (OutError)
					{
						*OutError = InvalidInput(Code, FString::Printf(TEXT("The value duplicates the entry at index %d after trimming whitespace."), FirstIndices.FindChecked(Values[Index])),
							FString::Printf(TEXT("%s[%d]"), *Path, DuplicateIndices.FindChecked(Values[Index])));
						OutError->ActualEvidence = MakeShared<FJsonValueString>(Values[Index]);
					}
					return false;
				}
			}
			return true;
		}

		bool IsSha256(const FString& Value)
		{
			if (Value.Len() != 71 || !Value.StartsWith(TEXT("sha256:")))
			{
				return false;
			}
			for (int32 Index = 7; Index < Value.Len(); ++Index)
			{
				if (!FChar::IsHexDigit(Value[Index]))
				{
					return false;
				}
			}
			return true;
		}

		FOperationLifecycleError PhaseFailure(
			const FOperationLifecycleError& Error, const TCHAR* Code, const TCHAR* Reason,
			const EOperationLifecycleStage Stage)
		{
			if (!Error.IsEmpty()) return Error;
			auto Diagnostic = FOperationLifecycleError::Failure(Code, Reason, LexToString(Stage));
			Diagnostic.SourceKind = TEXT("native_result");
			return Diagnostic;
		}

		void AppendStage(
			TArray<FString>& History,
			const EOperationLifecycleStage Stage)
		{
			History.Add(LexToString(Stage));
		}
	}

	FString LexToString(const EOperationLifecycleStage Stage)
	{
		switch (Stage)
		{
		case EOperationLifecycleStage::Planning:
			return TEXT("planning");
		case EOperationLifecycleStage::Authorization:
			return TEXT("authorization");
		case EOperationLifecycleStage::PreExecuteFence:
			return TEXT("pre_execute_fence");
		case EOperationLifecycleStage::DomainExecute:
			return TEXT("domain_execute");
		case EOperationLifecycleStage::Settle:
			return TEXT("settle");
		case EOperationLifecycleStage::PrePersistFence:
			return TEXT("pre_persist_fence");
		case EOperationLifecycleStage::Persist:
			return TEXT("persist");
		case EOperationLifecycleStage::IndependentVerify:
			return TEXT("independent_verify");
		case EOperationLifecycleStage::Rollback:
			return TEXT("rollback");
		case EOperationLifecycleStage::Terminal:
			return TEXT("terminal");
		default:
			return FString();
		}
	}

	FString LexToString(const EOperationLifecycleStatus Status)
	{
		switch (Status)
		{
		case EOperationLifecycleStatus::Succeeded:
			return TEXT("succeeded");
		case EOperationLifecycleStatus::Rejected:
			return TEXT("rejected");
		case EOperationLifecycleStatus::RolledBack:
			return TEXT("rolled_back");
		case EOperationLifecycleStatus::StateUnknown:
			return TEXT("state_unknown");
		default:
			return FString();
		}
	}


	FOperationLifecyclePlan::FOperationLifecyclePlan(
		FString InPlanId,
		FString InPlanFingerprint,
		TArray<FString> InImpactScope,
		TMap<FString, FString> InExpectedRevisions,
		TArray<FString> InRequiredSettleSignals)
		: PlanId(Normalize(MoveTemp(InPlanId)))
		, PlanFingerprint(Normalize(MoveTemp(InPlanFingerprint)))
		, ImpactScope(MoveTemp(InImpactScope))
		, ExpectedRevisions(MoveTemp(InExpectedRevisions))
		, RequiredSettleSignals(MoveTemp(InRequiredSettleSignals))
	{
		const TCHAR* Code = TEXT("operation_plan_invalid");
		if (PlanId.IsEmpty())
			ValidationDiagnostic = InvalidInput(Code, TEXT("The operation plan identifier is empty after trimming whitespace."), TEXT("plan_id"));
		else if (!IsSha256(PlanFingerprint))
		{
			ValidationDiagnostic = InvalidInput(Code, TEXT("The operation plan fingerprint must contain a sha256 prefix and 64 hexadecimal digits."), TEXT("plan_fingerprint"));
			ValidationDiagnostic.ActualEvidence = MakeShared<FJsonValueString>(PlanFingerprint);
		}
		else if (ImpactScope.IsEmpty())
			ValidationDiagnostic = InvalidInput(Code, TEXT("The operation plan must declare at least one affected resource."), TEXT("impact_scope"));
		else if (NormalizeSet(ImpactScope, &ValidationDiagnostic, Code, TEXT("impact_scope"))
			&& NormalizeSet(RequiredSettleSignals, &ValidationDiagnostic, Code, TEXT("required_settle_signals"))
			&& ExpectedRevisions.Num() != ImpactScope.Num())
		{
			ValidationDiagnostic = InvalidInput(Code, TEXT("The expected revision count must equal the number of affected resources."), TEXT("expected_revisions"));
			ValidationDiagnostic.ActualValue = MakeShared<FJsonValueNumber>(ExpectedRevisions.Num());
			ValidationDiagnostic.ExpectedValue = MakeShared<FJsonValueNumber>(ImpactScope.Num());
		}
		TMap<FString, FString> NormalizedRevisions;
		for (TPair<FString, FString>& Pair : ExpectedRevisions)
		{
			FString Key = Normalize(Pair.Key);
			FString Value = Normalize(MoveTemp(Pair.Value));
			if (Key.IsEmpty() || Value.IsEmpty()
				|| !ImpactScope.Contains(Key)
				|| NormalizedRevisions.Contains(Key))
			{
				if (ValidationDiagnostic.IsEmpty())
				{
					const bool bValueMissing = !Key.IsEmpty() && Value.IsEmpty();
					ValidationDiagnostic = InvalidInput(Code,
						Key.IsEmpty() ? TEXT("The expected revision has an empty resource identity.")
						: bValueMissing ? TEXT("The expected revision value is empty.")
						: !ImpactScope.Contains(Key) ? TEXT("The expected revision refers to a resource outside the plan impact scope.")
						: TEXT("More than one expected revision refers to the same normalized resource identity."),
						bValueMissing ? TEXT("expected_revisions.value") : TEXT("expected_revisions.key"));
					ValidationDiagnostic.SubjectId = Key;
					ValidationDiagnostic.InternalContext = Pair.Key;
					ValidationDiagnostic.ActualEvidence = MakeShared<FJsonValueString>(bValueMissing ? Value : Key);
				}
				break;
			}
			NormalizedRevisions.Add(MoveTemp(Key), MoveTemp(Value));
		}
		ExpectedRevisions = MoveTemp(NormalizedRevisions);
	}

	const FString& FOperationLifecyclePlan::GetPlanId() const
	{
		return PlanId;
	}

	const FString& FOperationLifecyclePlan::GetPlanFingerprint() const
	{
		return PlanFingerprint;
	}

	const TArray<FString>& FOperationLifecyclePlan::GetImpactScope() const
	{
		return ImpactScope;
	}

	const TMap<FString, FString>&
	FOperationLifecyclePlan::GetExpectedRevisions() const
	{
		return ExpectedRevisions;
	}

	const TArray<FString>&
	FOperationLifecyclePlan::GetRequiredSettleSignals() const
	{
		return RequiredSettleSignals;
	}

	bool FOperationLifecyclePlan::IsValid() const
	{
		return ValidationDiagnostic.IsEmpty();
	}

	FOperationImpactAuthorization::FOperationImpactAuthorization(
		FString InPlanFingerprint,
		TArray<FString> InAuthorizedImpactScope)
		: PlanFingerprint(Normalize(MoveTemp(InPlanFingerprint)))
		, AuthorizedImpactScope(MoveTemp(InAuthorizedImpactScope))
	{
		const TCHAR* Code = TEXT("operation_authorization_invalid");
		if (!IsSha256(PlanFingerprint))
		{
			ValidationDiagnostic = InvalidInput(Code, TEXT("The authorization fingerprint must contain a sha256 prefix and 64 hexadecimal digits."), TEXT("plan_fingerprint"));
			ValidationDiagnostic.ActualEvidence = MakeShared<FJsonValueString>(PlanFingerprint);
		}
		else if (AuthorizedImpactScope.IsEmpty())
			ValidationDiagnostic = InvalidInput(Code, TEXT("The authorization must cover at least one affected resource."), TEXT("authorized_impact_scope"));
		else NormalizeSet(AuthorizedImpactScope, &ValidationDiagnostic, Code, TEXT("authorized_impact_scope"));
	}

	const FString&
	FOperationImpactAuthorization::GetPlanFingerprint() const
	{
		return PlanFingerprint;
	}

	const TArray<FString>&
	FOperationImpactAuthorization::GetAuthorizedImpactScope() const
	{
		return AuthorizedImpactScope;
	}

	bool FOperationImpactAuthorization::Authorizes(
		const FOperationLifecyclePlan& Plan) const
	{
		FOperationLifecycleError Error;
		return TryAuthorize(Plan, Error);
	}

	bool FOperationImpactAuthorization::TryAuthorize(
		const FOperationLifecyclePlan& Plan, FOperationLifecycleError& OutError) const
	{
		OutError = ValidationDiagnostic;
		if (!OutError.IsEmpty()) return false;
		if (!Plan.IsValid())
		{
			OutError = Plan.GetValidationDiagnostic();
			return false;
		}
		if (PlanFingerprint != Plan.GetPlanFingerprint())
		{
			OutError = InvalidInput(TEXT("operation_impact_not_authorized"), TEXT("The authorization was issued for a different operation plan."), TEXT("plan_fingerprint"));
			OutError.ActualEvidence = MakeShared<FJsonValueString>(PlanFingerprint);
			OutError.ExpectedEvidence = MakeShared<FJsonValueString>(Plan.GetPlanFingerprint());
			return false;
		}
		if (AuthorizedImpactScope != Plan.GetImpactScope())
		{
			OutError = InvalidInput(TEXT("operation_impact_not_authorized"), TEXT("The authorized resources differ from the operation plan impact scope."), TEXT("authorized_impact_scope"));
			TArray<TSharedPtr<FJsonValue>> Actual, Expected;
			for (const auto& Value : AuthorizedImpactScope) Actual.Add(MakeShared<FJsonValueString>(Value));
			for (const auto& Value : Plan.GetImpactScope()) Expected.Add(MakeShared<FJsonValueString>(Value));
			OutError.ActualEvidence = MakeShared<FJsonValueArray>(MoveTemp(Actual));
			OutError.ExpectedEvidence = MakeShared<FJsonValueArray>(MoveTemp(Expected));
			return false;
		}
		return true;
	}

	bool FOperationImpactAuthorization::IsValid() const
	{
		return ValidationDiagnostic.IsEmpty();
	}

	FOperationMutationFacts::FOperationMutationFacts()
	{
		ValidationDiagnostic = InvalidInput(TEXT("operation_mutation_facts_missing"),
			TEXT("The operation did not provide mutation facts."), TEXT("mutation_facts"));
		ValidationDiagnostic.SourceKind = TEXT("native_result");
		ValidationDiagnostic.Stage = TEXT("domain_execute");
		ValidationDiagnostic.ActualValuePresent = false;
		ValidationDiagnostic.ExpectedValue = MakeShared<FJsonValueString>(TEXT("mutation facts"));
	}

	FOperationMutationFacts::FOperationMutationFacts(
		FString InMutationId,
		TArray<FString> InMutatedIdentityKeys,
		const bool bInRollbackSupported)
		: MutationId(Normalize(MoveTemp(InMutationId)))
		, MutatedIdentityKeys(MoveTemp(InMutatedIdentityKeys))
		, bRollbackSupported(bInRollbackSupported)
		, bProvided(true)
	{
		if (MutationId.IsEmpty())
		{
			ValidationDiagnostic = InvalidInput(TEXT("operation_mutation_id_missing"),
				TEXT("The mutation facts contain an empty mutation identifier."), TEXT("mutation_facts.mutation_id"));
			ValidationDiagnostic.ActualValue = MakeShared<FJsonValueString>(MutationId);
			ValidationDiagnostic.ExpectedValue = MakeShared<FJsonValueString>(TEXT("nonempty mutation identifier"));
		}
		else if (MutatedIdentityKeys.IsEmpty())
		{
			ValidationDiagnostic = InvalidInput(TEXT("operation_mutation_targets_missing"),
				TEXT("The mutation facts contain no mutated target identities."), TEXT("mutation_facts.mutated_targets"));
			ValidationDiagnostic.ActualValue = MakeShared<FJsonValueNumber>(0);
			ValidationDiagnostic.ExpectedValue = MakeShared<FJsonValueString>(TEXT("at least one mutated target"));
		}
		else
		{
			if (!NormalizeSet(MutatedIdentityKeys, &ValidationDiagnostic,
				TEXT("operation_mutation_targets_invalid"), TEXT("mutation_facts.mutated_targets")))
			{
				ValidationDiagnostic.ActualValuePresent = true;
				ValidationDiagnostic.ExpectedValue = MakeShared<FJsonValueString>(TEXT("unique nonempty target identities"));
			}
		}
		if (!ValidationDiagnostic.IsEmpty())
		{
			ValidationDiagnostic.SourceKind = TEXT("native_result");
			ValidationDiagnostic.Stage = TEXT("domain_execute");
		}
	}

	const FString& FOperationMutationFacts::GetMutationId() const
	{
		return MutationId;
	}

	const TArray<FString>&
	FOperationMutationFacts::GetMutatedIdentityKeys() const
	{
		return MutatedIdentityKeys;
	}

	bool FOperationMutationFacts::IsRollbackSupported() const
	{
		return bRollbackSupported;
	}

	bool FOperationMutationFacts::IsValid() const
	{
		return ValidationDiagnostic.IsEmpty();
	}

	bool FOperationMutationFacts::TryValidate(FOperationLifecycleError& OutError) const
	{
		OutError = ValidationDiagnostic;
		return IsValid();
	}

	FOperationLifecycleReceipt::FOperationLifecycleReceipt(
		FString InPlanId,
		const EOperationLifecycleStatus InStatus,
		UE::UAI::Contract::FDiagnostic InDiagnostic,
		const bool bInMutationStarted,
		const bool bInRetrySafe,
		FString InPersistenceReceiptId,
		TArray<FString> InPhaseHistory)
		: PlanId(Normalize(MoveTemp(InPlanId)))
		, Status(InStatus)
		, Diagnostic(MoveTemp(InDiagnostic))
		, bMutationStarted(bInMutationStarted)
		, bRetrySafe(bInRetrySafe)
		, PersistenceReceiptId(
			Normalize(MoveTemp(InPersistenceReceiptId)))
		, PhaseHistory(MoveTemp(InPhaseHistory))
		, bValid(!PlanId.IsEmpty() && !PhaseHistory.IsEmpty())
	{
		if (!Diagnostic.IsEmpty())
		{
			Diagnostic.Schema = TEXT("uai.operation.failure/1");
			Diagnostic.TerminalStatus = LexToString(Status);
			Diagnostic.MutationStarted = bMutationStarted;
			Diagnostic.MutationMayHaveStarted = bMutationStarted || Status == EOperationLifecycleStatus::StateUnknown;
			Diagnostic.bRetrySafe = bRetrySafe;
		}
	}

	const FString& FOperationLifecycleReceipt::GetPlanId() const
	{
		return PlanId;
	}

	EOperationLifecycleStatus
	FOperationLifecycleReceipt::GetStatus() const
	{
		return Status;
	}

	const UE::UAI::Contract::FDiagnostic& FOperationLifecycleReceipt::GetDiagnostic() const
	{
		return Diagnostic;
	}

	bool FOperationLifecycleReceipt::DidMutationStart() const
	{
		return bMutationStarted;
	}

	bool FOperationLifecycleReceipt::IsRetrySafe() const
	{
		return bRetrySafe;
	}

	const FString&
	FOperationLifecycleReceipt::GetPersistenceReceiptId() const
	{
		return PersistenceReceiptId;
	}

	const TArray<FString>&
	FOperationLifecycleReceipt::GetPhaseHistory() const
	{
		return PhaseHistory;
	}

	bool FOperationLifecycleReceipt::IsValid() const
	{
		return bValid;
	}

	FOperationLifecycleService::FOperationLifecycleService(
		TSharedRef<IOperationVersionFencePort> InVersionFence,
		TSharedRef<IOperationDomainMutationPort> InDomainMutation,
		TSharedRef<IOperationStabilityPort> InStability,
		TSharedRef<IOperationPersistencePort> InPersistence,
		TSharedRef<IOperationIndependentVerificationPort>
			InIndependentVerification)
		: VersionFence(MoveTemp(InVersionFence))
		, DomainMutation(MoveTemp(InDomainMutation))
		, Stability(MoveTemp(InStability))
		, Persistence(MoveTemp(InPersistence))
		, IndependentVerification(MoveTemp(InIndependentVerification))
	{
	}

	bool FOperationLifecycleService::Rollback(
		const FOperationLifecyclePlan& Plan,
		const FOperationMutationFacts& Facts,
		UE::UAI::Contract::FDiagnostic OriginalDiagnostic,
		TArray<FString> PhaseHistory,
		FOperationLifecycleReceipt& OutReceipt) const
	{
		AppendStage(PhaseHistory, EOperationLifecycleStage::Rollback);
		if (!Facts.IsValid() || !Facts.IsRollbackSupported())
		{
			if (!OriginalDiagnostic.RollbackAttempted.IsSet()) OriginalDiagnostic.RollbackAttempted = false;
			if (!OriginalDiagnostic.RollbackSucceeded.IsSet()) OriginalDiagnostic.RollbackSucceeded = false;
			if (!Facts.IsValid()) OriginalDiagnostic.Diagnostics.Add(Facts.GetValidationDiagnostic());
			else OriginalDiagnostic.Diagnostics.Add(PhaseFailure({}, TEXT("operation_rollback_not_available"),
				TEXT("The operation's mutation facts declare that rollback is unsupported."), EOperationLifecycleStage::Rollback));
			AppendStage(PhaseHistory, EOperationLifecycleStage::Terminal);
			OutReceipt = FOperationLifecycleReceipt(Plan.GetPlanId(), EOperationLifecycleStatus::StateUnknown,
				MoveTemp(OriginalDiagnostic), true, false, FString(), MoveTemp(PhaseHistory));
			return false;
		}
		bool bConfirmed = false;
		FOperationLifecycleError RollbackError;
		OriginalDiagnostic.RollbackAttempted = true;
		const bool bSucceeded = DomainMutation->Rollback(Plan, Facts, bConfirmed, RollbackError);
		const bool bRolledBack = bSucceeded && bConfirmed;
		OriginalDiagnostic.RollbackSucceeded = bRolledBack;
		if (!bRolledBack)
			OriginalDiagnostic.Diagnostics.Add(PhaseFailure(RollbackError, TEXT("operation_rollback_unconfirmed"),
				!bSucceeded ? TEXT("The rollback provider failed without reporting a diagnostic.") : TEXT("The rollback provider returned success without confirming restoration."), EOperationLifecycleStage::Rollback));
		AppendStage(PhaseHistory, EOperationLifecycleStage::Terminal);
		OutReceipt = FOperationLifecycleReceipt(Plan.GetPlanId(),
			bRolledBack ? EOperationLifecycleStatus::RolledBack : EOperationLifecycleStatus::StateUnknown,
			MoveTemp(OriginalDiagnostic), true, bRolledBack, FString(), MoveTemp(PhaseHistory));
		return false;
	}

	bool FOperationLifecycleService::Run(
		const FOperationLifecyclePlan& Plan,
		const FOperationImpactAuthorization& Authorization,
		FOperationLifecycleReceipt& OutReceipt) const
	{
		TArray<FString> History;
		AppendStage(History, EOperationLifecycleStage::Planning);
		if (!Plan.IsValid())
		{
			AppendStage(History, EOperationLifecycleStage::Terminal);
			OutReceipt = FOperationLifecycleReceipt(Plan.GetPlanId().IsEmpty() ? TEXT("invalid-plan") : Plan.GetPlanId(),
		EOperationLifecycleStatus::Rejected,
		Plan.GetValidationDiagnostic(),
		false,
		true,
		FString(),
		MoveTemp(History));
			return false;
		}

		AppendStage(History, EOperationLifecycleStage::Authorization);
		FOperationLifecycleError AuthorizationError;
		if (!Authorization.TryAuthorize(Plan, AuthorizationError))
		{
			AppendStage(History, EOperationLifecycleStage::Terminal);
			OutReceipt = FOperationLifecycleReceipt(Plan.GetPlanId(),
		EOperationLifecycleStatus::Rejected,
		MoveTemp(AuthorizationError),
		false,
		true,
		FString(),
		MoveTemp(History));
			return false;
		}

		AppendStage(History, EOperationLifecycleStage::PreExecuteFence);
		bool bCurrent = false;
		FOperationLifecycleError Error;
		const bool bPreExecuteChecked = VersionFence->Check(
				Plan,
				EOperationLifecycleStage::PreExecuteFence,
				bCurrent,
				Error);
		if (!bPreExecuteChecked || !bCurrent)
		{
			AppendStage(History, EOperationLifecycleStage::Terminal);
			OutReceipt = FOperationLifecycleReceipt(
				Plan.GetPlanId(),
				EOperationLifecycleStatus::Rejected,
				PhaseFailure(Error, TEXT("operation_version_conflict"),
					!bPreExecuteChecked ? TEXT("The version check failed before execution without reporting a diagnostic.")
						: TEXT("The operation's expected revisions are no longer current before execution."), EOperationLifecycleStage::PreExecuteFence),
				false,
				true,
				FString(),
				MoveTemp(History));
			return false;
		}

		AppendStage(History, EOperationLifecycleStage::DomainExecute);
		FOperationMutationFacts Facts;
		Error = FOperationLifecycleError();
		if (!DomainMutation->Execute(Plan, Facts, Error))
		{
			if (!Facts.IsValid())
			{
				if (Facts.IsProvided())
				{
					const auto& Validation = Facts.GetValidationDiagnostic();
					if (Error.IsEmpty()) Error = Validation;
					else if (Error.Code != Validation.Code) Error.Append(Validation);
					// An invalid supplied receipt cannot establish that execution made no changes.
					AppendStage(History, EOperationLifecycleStage::Terminal);
					OutReceipt = FOperationLifecycleReceipt(Plan.GetPlanId(), EOperationLifecycleStatus::StateUnknown,
						MoveTemp(Error), false, false, FString(), MoveTemp(History));
					return false;
				}
				AppendStage(History, EOperationLifecycleStage::Terminal);
				OutReceipt = FOperationLifecycleReceipt(
					Plan.GetPlanId(),
					EOperationLifecycleStatus::Rejected,
					PhaseFailure(Error, TEXT("operation_execute_failed_no_change"), TEXT("The domain execution failed without reporting a diagnostic or mutation facts."), EOperationLifecycleStage::DomainExecute),
					false,
					true,
					FString(),
					MoveTemp(History));
				return false;
			}
			return Rollback(
				Plan,
				Facts,
				PhaseFailure(Error, TEXT("operation_execute_failed"), TEXT("The domain execution failed after reporting mutation facts, without providing a diagnostic."), EOperationLifecycleStage::DomainExecute),
				MoveTemp(History),
				OutReceipt);
		}
		if (!Facts.IsValid())
		{
			AppendStage(History, EOperationLifecycleStage::Terminal);
			OutReceipt = FOperationLifecycleReceipt(Plan.GetPlanId(),
		EOperationLifecycleStatus::StateUnknown,
		Facts.GetValidationDiagnostic(),
		true,
		false,
		FString(),
		MoveTemp(History));
			return false;
		}

		AppendStage(History, EOperationLifecycleStage::Settle);
		Error = FOperationLifecycleError();
		if (!Stability->Settle(Plan, Facts, Error))
		{
			return Rollback(
				Plan,
				Facts,
				PhaseFailure(Error, TEXT("operation_settle_failed"), TEXT("The native stability stage failed without reporting a diagnostic."), EOperationLifecycleStage::Settle),
				MoveTemp(History),
				OutReceipt);
		}

		AppendStage(History, EOperationLifecycleStage::PrePersistFence);
		bCurrent = false;
		Error = FOperationLifecycleError();
		const bool bPrePersistChecked = VersionFence->Check(
				Plan,
				EOperationLifecycleStage::PrePersistFence,
				bCurrent,
				Error);
		if (!bPrePersistChecked || !bCurrent)
		{
			return Rollback(
				Plan,
				Facts,
				PhaseFailure(Error, TEXT("operation_version_conflict"),
					!bPrePersistChecked ? TEXT("The version check failed before persistence without reporting a diagnostic.")
						: TEXT("The operation's expected revisions are no longer current before persistence."), EOperationLifecycleStage::PrePersistFence),
				MoveTemp(History),
				OutReceipt);
		}

		AppendStage(History, EOperationLifecycleStage::Persist);
		FString PersistenceReceiptId;
		Error = FOperationLifecycleError();
		const bool bPersisted = Persistence->Persist(
				Plan,
				Facts,
				PersistenceReceiptId,
				Error);
		if (!bPersisted || Normalize(PersistenceReceiptId).IsEmpty())
		{
			return Rollback(
				Plan,
				Facts,
				PhaseFailure(Error, TEXT("operation_persist_failed"),
					!bPersisted ? TEXT("The persistence provider failed without reporting a diagnostic.")
						: TEXT("The persistence provider returned success without a nonempty receipt identifier."), EOperationLifecycleStage::Persist),
				MoveTemp(History),
				OutReceipt);
		}

		AppendStage(History, EOperationLifecycleStage::IndependentVerify);
		bool bEquivalent = false;
		Error = FOperationLifecycleError();
		if (!IndependentVerification->Verify(
				Plan,
				Facts,
				PersistenceReceiptId,
				bEquivalent,
				Error))
		{
			return Rollback(
				Plan,
				Facts,
				PhaseFailure(Error, TEXT("operation_readback_mismatch"), TEXT("Independent readback failed without reporting a diagnostic."), EOperationLifecycleStage::IndependentVerify),
				MoveTemp(History),
				OutReceipt);
		}

		if (!bEquivalent)
		{
			auto Diagnostic = PhaseFailure(Error, TEXT("operation_readback_mismatch"),
				TEXT("Independent readback completed but reported a non-equivalent result."), EOperationLifecycleStage::IndependentVerify);
			if (Error.IsEmpty())
			{
				Diagnostic.PropertyPath = TEXT("equivalent");
				Diagnostic.ActualValue = MakeShared<FJsonValueBoolean>(false);
				Diagnostic.ExpectedValue = MakeShared<FJsonValueBoolean>(true);
			}
			UeAgentAuthoringDiagnostics::ReportConsistencyWarning(Diagnostic);
		}
		AppendStage(History, EOperationLifecycleStage::Terminal);
		OutReceipt = FOperationLifecycleReceipt(Plan.GetPlanId(),
		EOperationLifecycleStatus::Succeeded,
		UE::UAI::Contract::FDiagnostic(),
		true,
		false,
		MoveTemp(PersistenceReceiptId),
		MoveTemp(History));
		return true;
	}
}
