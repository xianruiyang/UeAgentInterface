#include "DomainAdapters/NiagaraCompound/UeAgentNiagaraCompoundAdapter.h"

#include "Core/PlanningPreflight/UeAgentPlanningPreflight.h"
#include "Containers/Set.h"

namespace UE::UAI::DomainAdapters::NiagaraCompound
{
	namespace
	{
		using namespace ContextResource;
		using namespace Discovery;
		using namespace ExecutionMutation;
		using namespace PlanningPreflight;
		using namespace StateSemantic;

		bool Fail(
			FExecutionMutationError& OutError,
			FString Code,
			FString Detail = FString())
		{
			OutError = FExecutionMutationError(
				MoveTemp(Code),
				MoveTemp(Detail));
			return false;
		}

		bool FailRead(
			GenericRead::FUeAgentGenericReadError& OutError,
			FString Code,
			const FString& SubjectId,
			FString Detail = FString())
		{
			OutError = {
				MoveTemp(Code),
				SubjectId,
				MoveTemp(Detail),
			};
			return false;
		}

		const FUeAgentCanonicalValueRef* FindField(
			const FUeAgentCanonicalValue& Value,
			const FString& Name)
		{
			if (Value.GetKind() != EUeAgentValueKind::Object)
			{
				return nullptr;
			}
			const TPair<FString, FUeAgentCanonicalValueRef>* Pair =
				Value.GetObjectItems().FindByPredicate(
					[&Name](
						const TPair<
							FString,
							FUeAgentCanonicalValueRef>& Candidate)
					{
						return Candidate.Key == Name;
					});
			return Pair ? &Pair->Value : nullptr;
		}

		bool TryRequiredString(
			const FUeAgentCanonicalValue& Value,
			const FString& Name,
			const FString& Path,
			FString& OutValue,
			FExecutionMutationError& OutError)
		{
			const FUeAgentCanonicalValueRef* Field =
				FindField(Value, Name);
			if (!Field
				|| (*Field)->GetKind() != EUeAgentValueKind::String)
			{
				return Fail(
					OutError,
					TEXT("niagara_compound_field_invalid"),
					Path + TEXT("/") + Name);
			}
			OutValue = (*Field)->GetText();
			OutValue.TrimStartAndEndInline();
			if (OutValue.IsEmpty())
			{
				return Fail(
					OutError,
					TEXT("niagara_compound_field_invalid"),
					Path + TEXT("/") + Name);
			}
			return true;
		}

		bool ChargeValue(
			FUeAgentResourceTracker& Resources,
			const int32 FieldCount,
			const FString& Path,
			FExecutionMutationError& OutError)
		{
			FUeAgentResourceBudgetExceeded BudgetError;
			if (Resources.TryChargeMany(
					{
						{EUeAgentResourceDimension::Nodes, 1},
						{
							EUeAgentResourceDimension::Items,
							FMath::Max(1, FieldCount),
						},
						{EUeAgentResourceDimension::WorkUnits, 1},
						{
							EUeAgentResourceDimension::
								EstimatedMemoryBytes,
							static_cast<int64>(
								sizeof(FUeAgentCanonicalValueRef)
								+ FMath::Max(1, FieldCount)
									* sizeof(
										TPair<
											FString,
											FUeAgentCanonicalValueRef>)),
						},
					},
					Path,
					BudgetError))
			{
				return true;
			}
			return Fail(
				OutError,
				BudgetError.Code,
				FString::Printf(
					TEXT("%s limit=%lld observed=%lld"),
					*BudgetError.Path,
					BudgetError.Limit,
					BudgetError.Observed));
		}

		bool CheckContext(
			const FUeAgentOperationDeadline& Deadline,
			const FUeAgentCancellationToken& Cancellation,
			const FString& Stage,
			FExecutionMutationError& OutError)
		{
			FUeAgentCancellationRequested Cancelled;
			if (!Cancellation.TryCheckpoint(Cancelled))
			{
				return Fail(
					OutError,
					Cancelled.Code,
					Cancelled.Reason);
			}
			FUeAgentOperationDeadlineExceeded Expired;
			if (!Deadline.TryCheckpoint(Stage, Expired))
			{
				return Fail(
					OutError,
					Expired.Code,
					Expired.Stage);
			}
			return true;
		}

		bool CheckReadContext(
			const FUeAgentOperationDeadline& Deadline,
			const FUeAgentCancellationToken& Cancellation,
			const FString& SubjectId,
			const FString& Stage,
			GenericRead::FUeAgentGenericReadError& OutError)
		{
			FUeAgentCancellationRequested Cancelled;
			if (!Cancellation.TryCheckpoint(Cancelled))
			{
				return FailRead(
					OutError,
					Cancelled.Code,
					SubjectId,
					Cancelled.Reason);
			}
			FUeAgentOperationDeadlineExceeded Expired;
			if (!Deadline.TryCheckpoint(Stage, Expired))
			{
				return FailRead(
					OutError,
					Expired.Code,
					SubjectId,
					Expired.Stage);
			}
			return true;
		}

		bool ContainsAll(
			const TArray<FString>& Values,
			const TArray<FString>& Required)
		{
			for (const FString& Value : Required)
			{
				if (!Values.Contains(Value))
				{
					return false;
				}
			}
			return true;
		}
	}

	bool TryValidateNiagaraCompoundInput(
		const FUeAgentCanonicalValueRef& Value,
		FUeAgentResourceTracker& Resources,
		FExecutionMutationError& OutError)
	{
		OutError = FExecutionMutationError();
		struct FFrame
		{
			FUeAgentCanonicalValueRef Value;
			FString Path;
			bool bExit = false;
		};

		TArray<FFrame> Stack;
		Stack.Add({Value, TEXT("/niagara_input"), false});
		TSet<const FUeAgentCanonicalValue*> Active;

		while (!Stack.IsEmpty())
		{
			FFrame Frame = Stack.Pop(EAllowShrinking::No);
			const FUeAgentCanonicalValue* Raw = &Frame.Value.Get();
			if (Frame.bExit)
			{
				Active.Remove(Raw);
				continue;
			}
			if (Active.Contains(Raw))
			{
				return Fail(
					OutError,
					TEXT("niagara_compound_cycle_detected"),
					Frame.Path);
			}
			if (Frame.Value->GetKind() != EUeAgentValueKind::Object)
			{
				return Fail(
					OutError,
					TEXT("niagara_compound_value_not_object"),
					Frame.Path);
			}
			const TArray<
				TPair<FString, FUeAgentCanonicalValueRef>>& Fields =
					Frame.Value->GetObjectItems();
			if (!ChargeValue(
					Resources,
					Fields.Num(),
					Frame.Path,
					OutError))
			{
				return false;
			}

			FString Kind;
			FString InputName;
			FString TypeId;
			FString Mode;
			if (!TryRequiredString(
					*Frame.Value,
					TEXT("kind"),
					Frame.Path,
					Kind,
					OutError)
				|| !TryRequiredString(
					*Frame.Value,
					TEXT("input_name"),
					Frame.Path,
					InputName,
					OutError)
				|| !TryRequiredString(
					*Frame.Value,
					TEXT("type_id"),
					Frame.Path,
					TypeId,
					OutError)
				|| !TryRequiredString(
					*Frame.Value,
					TEXT("mode"),
					Frame.Path,
					Mode,
					OutError))
			{
				return false;
			}
			if (Kind != TEXT("niagara_input"))
			{
				return Fail(
					OutError,
					TEXT("niagara_compound_kind_invalid"),
					Frame.Path + TEXT("/kind"));
			}

			const FUeAgentCanonicalValueRef* Literal =
				FindField(*Frame.Value, TEXT("literal"));
			const FUeAgentCanonicalValueRef* Linked =
				FindField(*Frame.Value, TEXT("linked_parameter"));
			const FUeAgentCanonicalValueRef* DataInterface =
				FindField(*Frame.Value, TEXT("data_interface"));
			const FUeAgentCanonicalValueRef* DynamicScript =
				FindField(
					*Frame.Value,
					TEXT("dynamic_script_identity"));
			const FUeAgentCanonicalValueRef* Inputs =
				FindField(*Frame.Value, TEXT("inputs"));
			const bool bHasLiteral = Literal != nullptr;
			const bool bHasLinked = Linked != nullptr;
			const bool bHasDataInterface = DataInterface != nullptr;
			const bool bHasDynamic =
				DynamicScript != nullptr || Inputs != nullptr;
			const int32 BranchCount =
				static_cast<int32>(bHasLiteral)
				+ static_cast<int32>(bHasLinked)
				+ static_cast<int32>(bHasDataInterface)
				+ static_cast<int32>(bHasDynamic);
			if (BranchCount > 1)
			{
				return Fail(
					OutError,
					TEXT("niagara_compound_branch_conflict"),
					Frame.Path);
			}
			if (BranchCount == 0)
			{
				return Fail(
					OutError,
					TEXT("niagara_compound_branch_missing"),
					Frame.Path);
			}

			if (Mode == TEXT("literal"))
			{
				if (!bHasLiteral)
				{
					return Fail(
						OutError,
						TEXT("niagara_compound_mode_branch_mismatch"),
						Frame.Path);
				}
			}
			else if (Mode == TEXT("linked_parameter"))
			{
				if (!bHasLinked
					|| (*Linked)->GetKind()
						!= EUeAgentValueKind::String
					|| (*Linked)->GetText().TrimStartAndEnd().IsEmpty())
				{
					return Fail(
						OutError,
						TEXT("niagara_compound_mode_branch_mismatch"),
						Frame.Path);
				}
			}
			else if (Mode == TEXT("data_interface"))
			{
				if (!bHasDataInterface
					|| (*DataInterface)->GetKind()
						!= EUeAgentValueKind::Object)
				{
					return Fail(
						OutError,
						TEXT("niagara_compound_mode_branch_mismatch"),
						Frame.Path);
				}
			}
			else if (Mode == TEXT("dynamic_input"))
			{
				if (!DynamicScript
					|| (*DynamicScript)->GetKind()
						!= EUeAgentValueKind::String
					|| (*DynamicScript)
						->GetText()
						.TrimStartAndEnd()
						.IsEmpty()
					|| !Inputs
					|| (*Inputs)->GetKind()
						!= EUeAgentValueKind::Array)
				{
					return Fail(
						OutError,
						TEXT("niagara_compound_mode_branch_mismatch"),
						Frame.Path);
				}
				Active.Add(Raw);
				Stack.Add({Frame.Value, Frame.Path, true});
				const TArray<FUeAgentCanonicalValueRef>& Children =
					(*Inputs)->GetArrayItems();
				for (
					int32 Index = Children.Num() - 1;
					Index >= 0;
					--Index)
				{
					Stack.Add(
						{
							Children[Index],
							FString::Printf(
								TEXT("%s/inputs/%d"),
								*Frame.Path,
								Index),
							false,
						});
				}
			}
			else
			{
				return Fail(
					OutError,
					TEXT("niagara_compound_mode_invalid"),
					Frame.Path + TEXT("/mode"));
			}
		}
		return true;
	}

	FUeAgentNiagaraCompileSettleReceipt::
	FUeAgentNiagaraCompileSettleReceipt(
		const int64 InSubjectGeneration,
		const bool bInCompilationComplete,
		const int32 InCompileErrorCount,
		const int32 InStackIssueCount,
		const int32 InCompileWarningCount,
		const int32 InStackWarningCount,
		TArray<FString> InCompiledTargets,
		TArray<FString> InObservedSignals,
		FString InPostBarrierRevision,
		FString InEvidenceId)
		: SubjectGeneration(InSubjectGeneration)
		, bCompilationComplete(bInCompilationComplete)
		, CompileErrorCount(InCompileErrorCount)
		, StackIssueCount(InStackIssueCount)
		, CompileWarningCount(InCompileWarningCount)
		, StackWarningCount(InStackWarningCount)
		, CompiledTargets(MoveTemp(InCompiledTargets))
		, ObservedSignals(MoveTemp(InObservedSignals))
		, PostBarrierRevision(MoveTemp(InPostBarrierRevision))
		, EvidenceId(MoveTemp(InEvidenceId))
	{
		bValid = SubjectGeneration >= 0
			&& CompileErrorCount >= 0
			&& StackIssueCount >= 0
			&& CompileWarningCount >= 0
			&& StackWarningCount >= 0
			&& !EvidenceId.IsEmpty();
	}

	int64 FUeAgentNiagaraCompileSettleReceipt::
	GetSubjectGeneration() const
	{
		return SubjectGeneration;
	}

	bool FUeAgentNiagaraCompileSettleReceipt::
	IsCompilationComplete() const
	{
		return bCompilationComplete;
	}

	int32 FUeAgentNiagaraCompileSettleReceipt::
	GetCompileErrorCount() const
	{
		return CompileErrorCount;
	}

	int32 FUeAgentNiagaraCompileSettleReceipt::
	GetStackIssueCount() const
	{
		return StackIssueCount;
	}

	int32 FUeAgentNiagaraCompileSettleReceipt::
	GetCompileWarningCount() const
	{
		return CompileWarningCount;
	}

	int32 FUeAgentNiagaraCompileSettleReceipt::
	GetStackWarningCount() const
	{
		return StackWarningCount;
	}

	const TArray<FString>& FUeAgentNiagaraCompileSettleReceipt::
	GetCompiledTargets() const
	{
		return CompiledTargets;
	}

	const TArray<FString>& FUeAgentNiagaraCompileSettleReceipt::
	GetObservedSignals() const
	{
		return ObservedSignals;
	}

	const FString& FUeAgentNiagaraCompileSettleReceipt::
	GetPostBarrierRevision() const
	{
		return PostBarrierRevision;
	}

	const FString& FUeAgentNiagaraCompileSettleReceipt::
	GetEvidenceId() const
	{
		return EvidenceId;
	}

	bool FUeAgentNiagaraCompileSettleReceipt::IsValid() const
	{
		return bValid;
	}

	FUeAgentNiagaraCompoundReadHook::
	FUeAgentNiagaraCompoundReadHook(
		TSharedRef<
			IUeAgentNiagaraCompoundUeHook,
			ESPMode::ThreadSafe> InUeHook)
		: UeHook(MoveTemp(InUeHook))
	{
	}

	bool FUeAgentNiagaraCompoundReadHook::TryCapture(
		const FUeAgentStateReadRequest& Request,
		FUeAgentResourceTracker& Resources,
		const FUeAgentOperationDeadline& Deadline,
		const FUeAgentCancellationToken& Cancellation,
		TSharedPtr<const
			GenericRead::FUeAgentUeReadCapture>& OutCapture,
		GenericRead::FUeAgentGenericReadError& OutError)
	{
		OutCapture.Reset();
		OutError = GenericRead::FUeAgentGenericReadError();
		const FString SubjectId = Request.IsValid()
			? Request.GetBinding().GetSummary()
				.GetIdentity().GetCanonicalKey()
			: FString();
		if (!Request.IsValid())
		{
			return FailRead(
				OutError,
				TEXT("niagara_compound_read_request_invalid"),
				SubjectId);
		}
		if (!CheckReadContext(
				Deadline,
				Cancellation,
				SubjectId,
				TEXT("domain_adapter.niagara_compound.before_capture"),
				OutError))
		{
			return false;
		}
		if (!UeHook->TryCapture(
				Request,
				Resources,
				Deadline,
				Cancellation,
				OutCapture,
				OutError))
		{
			return false;
		}
		if (!OutCapture.IsValid()
			|| !OutCapture->IsValid()
			|| OutCapture->GetEntries().Num() != 1)
		{
			return FailRead(
				OutError,
				TEXT("niagara_compound_capture_invalid"),
				SubjectId);
		}
		if (!OutCapture->IsSettled())
		{
			return FailRead(
				OutError,
				TEXT("niagara_compound_capture_unsettled"),
				SubjectId);
		}
		if (OutCapture->GetStatePlane() != Request.GetStatePlane())
		{
			return FailRead(
				OutError,
				TEXT("niagara_compound_state_plane_mismatch"),
				SubjectId);
		}
		const GenericRead::FUeAgentUeReadEntry& Entry =
			OutCapture->GetEntries()[0];
		if (Entry.GetIdentity()
				!= Request.GetBinding().GetSummary().GetIdentity())
		{
			return FailRead(
				OutError,
				TEXT("niagara_compound_identity_mismatch"),
				SubjectId);
		}
		FExecutionMutationError ValidationError;
		if (!TryValidateNiagaraCompoundInput(
				Entry.GetValue(),
				Resources,
				ValidationError))
		{
			return FailRead(
				OutError,
				ValidationError.GetCode(),
				SubjectId,
				ValidationError.GetDetail());
		}
		if (!CheckReadContext(
				Deadline,
				Cancellation,
				SubjectId,
				TEXT("domain_adapter.niagara_compound.after_capture"),
				OutError))
		{
			return false;
		}
		return true;
	}

	FUeAgentNiagaraCompoundEffectHook::
	FUeAgentNiagaraCompoundEffectHook(
		TSharedRef<
			IUeAgentNiagaraCompoundUeHook,
			ESPMode::ThreadSafe> InUeHook)
		: UeHook(MoveTemp(InUeHook))
	{
	}

	bool FUeAgentNiagaraCompoundEffectHook::TryApply(
		const FExecutionMutationRequest& Request,
		const FLockFenceLease& Lease,
		const TArray<
			GenericEffect::FUeAgentGenericEffectBinding>& Bindings,
		FUeAgentResourceTracker& Resources,
		GenericEffect::FUeAgentGenericEffectApplyResult& OutResult,
		FExecutionMutationError& OutError)
	{
		OutResult =
			GenericEffect::FUeAgentGenericEffectApplyResult();
		OutError = FExecutionMutationError();
		if (!Request.IsValid() || !Lease.IsValid())
		{
			return Fail(
				OutError,
				TEXT("niagara_compound_apply_request_invalid"));
		}
		if (!CheckContext(
				Request.GetDeadline(),
				Request.GetCancellation(),
				TEXT("domain_adapter.niagara_compound.before_validation"),
				OutError))
		{
			return false;
		}
		for (const FPlannedChange& Change :
			Request.GetPlan()->GetChangeSet().GetChanges())
		{
			if (Change.GetBefore().IsValid()
				&& !TryValidateNiagaraCompoundInput(
					Change.GetBefore().ToSharedRef(),
					Resources,
					OutError))
			{
				return false;
			}
			if (Change.GetAfter().IsValid()
				&& !TryValidateNiagaraCompoundInput(
					Change.GetAfter().ToSharedRef(),
					Resources,
					OutError))
			{
				return false;
			}
		}
		return UeHook->TryApply(
			Request,
			Lease,
			Bindings,
			Resources,
			OutResult,
			OutError);
	}

	bool FUeAgentNiagaraCompoundEffectHook::TryRollback(
		const FExecutionMutationRequest& Request,
		const GenericEffect::FUeAgentGenericEffectApplyResult& Applied,
		FUeAgentResourceTracker& Resources,
		bool& bOutConfirmed,
		FExecutionMutationError& OutError)
	{
		bOutConfirmed = false;
		OutError = FExecutionMutationError();
		if (!Request.IsValid() || !Applied.IsValid())
		{
			return Fail(
				OutError,
				TEXT("niagara_compound_rollback_request_invalid"));
		}
		return UeHook->TryRollback(
			Request,
			Applied,
			Resources,
			bOutConfirmed,
			OutError);
	}

	void FUeAgentNiagaraCompoundEffectHook::
		ReleaseVerifiedOperation(const FString& OperationId)
	{
		UeHook->ReleaseVerifiedOperation(OperationId);
	}

	bool FUeAgentNiagaraCompoundEffectHook::TryWaitForSettle(
		const FBarrierRequest& Request,
		FUeAgentResourceTracker& Resources,
		FBarrierResult& OutResult,
		FExecutionMutationError& OutError)
	{
		OutResult = FBarrierResult();
		OutError = FExecutionMutationError();
		if (!Request.IsValid())
		{
			return Fail(
				OutError,
				TEXT("niagara_settle_request_invalid"));
		}
		FUeAgentNiagaraCompileSettleReceipt Receipt;
		if (!UeHook->TryCompileAndSettle(
				Request,
				Resources,
				Receipt,
				OutError))
		{
			return false;
		}
		if (!Receipt.IsValid())
		{
			return Fail(
				OutError,
				TEXT("niagara_settle_receipt_invalid"));
		}
		if (Receipt.GetSubjectGeneration()
			!= Request.GetSubjectGeneration())
		{
			return Fail(
				OutError,
				TEXT("niagara_settle_generation_mismatch"));
		}
		if (!Receipt.IsCompilationComplete())
		{
			return Fail(
				OutError,
				TEXT("niagara_compile_incomplete"));
		}
		if (Receipt.GetCompileErrorCount() > 0)
		{
			return Fail(
				OutError,
				TEXT("niagara_compile_error"));
		}
		if (Receipt.GetStackIssueCount() > 0)
		{
			return Fail(
				OutError,
				TEXT("niagara_stack_issue"));
		}
		if (Receipt.GetCompiledTargets().IsEmpty())
		{
			return Fail(
				OutError,
				TEXT("niagara_compile_target_missing"));
		}
		if (!ContainsAll(
				Receipt.GetObservedSignals(),
				Request.GetRequiredSignals()))
		{
			return Fail(
				OutError,
				TEXT("niagara_settle_signal_missing"));
		}
		if (Receipt.GetPostBarrierRevision().IsEmpty())
		{
			return Fail(
				OutError,
				TEXT("niagara_post_barrier_revision_missing"));
		}
		OutResult = FBarrierResult(
			Receipt.GetSubjectGeneration(),
			Receipt.GetObservedSignals(),
			Receipt.GetPostBarrierRevision(),
			true);
		return true;
	}
}
