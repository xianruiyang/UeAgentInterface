#include "Core/BindingGateway/Operation/UeAgentOperationLifecycleService.h"

namespace UE::UAI::BindingGateway::Operation
{
	namespace
	{
		FString Normalize(FString Value)
		{
			Value.TrimStartAndEndInline();
			return Value;
		}

		bool NormalizeSet(TArray<FString>& Values)
		{
			for (FString& Value : Values)
			{
				Value = Normalize(MoveTemp(Value));
				if (Value.IsEmpty())
				{
					return false;
				}
			}
			Values.Sort();
			for (int32 Index = 1; Index < Values.Num(); ++Index)
			{
				if (Values[Index - 1] == Values[Index])
				{
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

		FString ErrorCodeOr(
			const FOperationLifecycleError& Error,
			const TCHAR* Fallback)
		{
			return Error.IsPresent() ? Error.GetCode() : FString(Fallback);
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

	FOperationLifecycleError::FOperationLifecycleError(
		FString InCode,
		FString InDetail)
		: Code(Normalize(MoveTemp(InCode)))
		, Detail(Normalize(MoveTemp(InDetail)))
	{
	}

	const FString& FOperationLifecycleError::GetCode() const
	{
		return Code;
	}

	const FString& FOperationLifecycleError::GetDetail() const
	{
		return Detail;
	}

	bool FOperationLifecycleError::IsPresent() const
	{
		return !Code.IsEmpty();
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
		bValid = !PlanId.IsEmpty()
			&& IsSha256(PlanFingerprint)
			&& !ImpactScope.IsEmpty()
			&& NormalizeSet(ImpactScope)
			&& NormalizeSet(RequiredSettleSignals)
			&& ExpectedRevisions.Num() == ImpactScope.Num();
		TMap<FString, FString> NormalizedRevisions;
		for (TPair<FString, FString>& Pair : ExpectedRevisions)
		{
			FString Key = Normalize(Pair.Key);
			FString Value = Normalize(MoveTemp(Pair.Value));
			if (Key.IsEmpty() || Value.IsEmpty()
				|| !ImpactScope.Contains(Key)
				|| NormalizedRevisions.Contains(Key))
			{
				bValid = false;
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
		return bValid;
	}

	FOperationImpactAuthorization::FOperationImpactAuthorization(
		FString InPlanFingerprint,
		TArray<FString> InAuthorizedImpactScope)
		: PlanFingerprint(Normalize(MoveTemp(InPlanFingerprint)))
		, AuthorizedImpactScope(MoveTemp(InAuthorizedImpactScope))
		, bValid(
			IsSha256(PlanFingerprint)
			&& !AuthorizedImpactScope.IsEmpty()
			&& NormalizeSet(AuthorizedImpactScope))
	{
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
		return bValid
			&& Plan.IsValid()
			&& PlanFingerprint == Plan.GetPlanFingerprint()
			&& AuthorizedImpactScope == Plan.GetImpactScope();
	}

	bool FOperationImpactAuthorization::IsValid() const
	{
		return bValid;
	}

	FOperationMutationFacts::FOperationMutationFacts(
		FString InMutationId,
		TArray<FString> InMutatedIdentityKeys,
		const bool bInRollbackSupported)
		: MutationId(Normalize(MoveTemp(InMutationId)))
		, MutatedIdentityKeys(MoveTemp(InMutatedIdentityKeys))
		, bRollbackSupported(bInRollbackSupported)
		, bValid(
			!MutationId.IsEmpty()
			&& !MutatedIdentityKeys.IsEmpty()
			&& NormalizeSet(MutatedIdentityKeys))
	{
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
		return bValid;
	}

	FOperationLifecycleReceipt::FOperationLifecycleReceipt(
		FString InPlanId,
		const EOperationLifecycleStatus InStatus,
		FString InErrorCode,
		const bool bInMutationStarted,
		const bool bInRetrySafe,
		FString InPersistenceReceiptId,
		TArray<FString> InPhaseHistory)
		: PlanId(Normalize(MoveTemp(InPlanId)))
		, Status(InStatus)
		, ErrorCode(Normalize(MoveTemp(InErrorCode)))
		, bMutationStarted(bInMutationStarted)
		, bRetrySafe(bInRetrySafe)
		, PersistenceReceiptId(
			Normalize(MoveTemp(InPersistenceReceiptId)))
		, PhaseHistory(MoveTemp(InPhaseHistory))
		, bValid(!PlanId.IsEmpty() && !PhaseHistory.IsEmpty())
	{
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

	const FString& FOperationLifecycleReceipt::GetErrorCode() const
	{
		return ErrorCode;
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
		const FString& OriginalErrorCode,
		TArray<FString> PhaseHistory,
		FOperationLifecycleReceipt& OutReceipt) const
	{
		AppendStage(PhaseHistory, EOperationLifecycleStage::Rollback);
		if (!Facts.IsValid() || !Facts.IsRollbackSupported())
		{
			AppendStage(PhaseHistory, EOperationLifecycleStage::Terminal);
			OutReceipt = FOperationLifecycleReceipt(
				Plan.GetPlanId(),
				EOperationLifecycleStatus::StateUnknown,
				TEXT("operation_rollback_not_available"),
				true,
				false,
				FString(),
				MoveTemp(PhaseHistory));
			return false;
		}
		bool bConfirmed = false;
		FOperationLifecycleError RollbackError;
		const bool bRollbackCallSucceeded = DomainMutation->Rollback(
			Plan,
			Facts,
			bConfirmed,
			RollbackError);
		AppendStage(PhaseHistory, EOperationLifecycleStage::Terminal);
		if (bRollbackCallSucceeded && bConfirmed)
		{
			OutReceipt = FOperationLifecycleReceipt(
				Plan.GetPlanId(),
				EOperationLifecycleStatus::RolledBack,
				OriginalErrorCode,
				true,
				true,
				FString(),
				MoveTemp(PhaseHistory));
			return false;
		}
		OutReceipt = FOperationLifecycleReceipt(
			Plan.GetPlanId(),
			EOperationLifecycleStatus::StateUnknown,
			ErrorCodeOr(
				RollbackError,
				TEXT("operation_rollback_unconfirmed")),
			true,
			false,
			FString(),
			MoveTemp(PhaseHistory));
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
			OutReceipt = FOperationLifecycleReceipt(
				Plan.GetPlanId().IsEmpty() ? TEXT("invalid-plan") : Plan.GetPlanId(),
				EOperationLifecycleStatus::Rejected,
				TEXT("operation_plan_invalid"),
				false,
				true,
				FString(),
				MoveTemp(History));
			return false;
		}

		AppendStage(History, EOperationLifecycleStage::Authorization);
		if (!Authorization.Authorizes(Plan))
		{
			AppendStage(History, EOperationLifecycleStage::Terminal);
			OutReceipt = FOperationLifecycleReceipt(
				Plan.GetPlanId(),
				EOperationLifecycleStatus::Rejected,
				TEXT("operation_impact_not_authorized"),
				false,
				true,
				FString(),
				MoveTemp(History));
			return false;
		}

		AppendStage(History, EOperationLifecycleStage::PreExecuteFence);
		bool bCurrent = false;
		FOperationLifecycleError Error;
		if (!VersionFence->Check(
				Plan,
				EOperationLifecycleStage::PreExecuteFence,
				bCurrent,
				Error)
			|| !bCurrent)
		{
			AppendStage(History, EOperationLifecycleStage::Terminal);
			OutReceipt = FOperationLifecycleReceipt(
				Plan.GetPlanId(),
				EOperationLifecycleStatus::Rejected,
				ErrorCodeOr(Error, TEXT("operation_version_conflict")),
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
				AppendStage(History, EOperationLifecycleStage::Terminal);
				OutReceipt = FOperationLifecycleReceipt(
					Plan.GetPlanId(),
					EOperationLifecycleStatus::Rejected,
					ErrorCodeOr(Error, TEXT("operation_execute_failed_no_change")),
					false,
					true,
					FString(),
					MoveTemp(History));
				return false;
			}
			return Rollback(
				Plan,
				Facts,
				ErrorCodeOr(Error, TEXT("operation_execute_failed")),
				MoveTemp(History),
				OutReceipt);
		}
		if (!Facts.IsValid())
		{
			AppendStage(History, EOperationLifecycleStage::Terminal);
			OutReceipt = FOperationLifecycleReceipt(
				Plan.GetPlanId(),
				EOperationLifecycleStatus::StateUnknown,
				TEXT("operation_mutation_facts_missing"),
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
				ErrorCodeOr(Error, TEXT("operation_settle_failed")),
				MoveTemp(History),
				OutReceipt);
		}

		AppendStage(History, EOperationLifecycleStage::PrePersistFence);
		bCurrent = false;
		Error = FOperationLifecycleError();
		if (!VersionFence->Check(
				Plan,
				EOperationLifecycleStage::PrePersistFence,
				bCurrent,
				Error)
			|| !bCurrent)
		{
			return Rollback(
				Plan,
				Facts,
				ErrorCodeOr(Error, TEXT("operation_version_conflict")),
				MoveTemp(History),
				OutReceipt);
		}

		AppendStage(History, EOperationLifecycleStage::Persist);
		FString PersistenceReceiptId;
		Error = FOperationLifecycleError();
		if (!Persistence->Persist(
				Plan,
				Facts,
				PersistenceReceiptId,
				Error)
			|| Normalize(PersistenceReceiptId).IsEmpty())
		{
			return Rollback(
				Plan,
				Facts,
				ErrorCodeOr(Error, TEXT("operation_persist_failed")),
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
				Error)
			|| !bEquivalent)
		{
			return Rollback(
				Plan,
				Facts,
				ErrorCodeOr(Error, TEXT("operation_readback_mismatch")),
				MoveTemp(History),
				OutReceipt);
		}

		AppendStage(History, EOperationLifecycleStage::Terminal);
		OutReceipt = FOperationLifecycleReceipt(
			Plan.GetPlanId(),
			EOperationLifecycleStatus::Succeeded,
			FString(),
			true,
			false,
			MoveTemp(PersistenceReceiptId),
			MoveTemp(History));
		return true;
	}
}
