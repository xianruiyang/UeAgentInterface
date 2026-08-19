// Copyright Epic Games, Inc. All Rights Reserved.

#include "Utilities/AuthoringWorkspaceApply/UeAgentAuthoringWorkspaceTransaction.h"

#include "Utilities/AuthoringWorkspaceApply/UeAgentAuthoringDurableTransaction.h"

#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"

namespace
{
	using namespace UeAgentAuthoringWorkspaceRouter;
	using namespace UeAgentAuthoringWorkspaceTransaction;

	FString FoldTargetKey(const FString& Value)
	{
		FString Result = Value;
		Result.TrimStartAndEndInline();
		return Result.ToLower();
	}

	UeAgentAuthoringDiagnostics::FSourceLocation UnitSource(const FWorkspaceUnit* Unit)
	{
		return Unit ? Unit->Provenance.ToDiagnosticSource() : UeAgentAuthoringDiagnostics::FSourceLocation();
	}

	void CompleteDiagnostic(
		FOperationFailure& Failure,
		const FString& DefaultCode,
		const FString& Stage,
		const FWorkspaceUnit* Unit)
	{
		if (Failure.ErrorCode.IsEmpty()) Failure.ErrorCode = DefaultCode;
		if (Failure.ErrorDetail.IsEmpty()) Failure.ErrorDetail = Failure.ErrorCode;
		if (Failure.Diagnostic.Code.IsEmpty()) Failure.Diagnostic.Code = Failure.ErrorCode;
		if (Failure.Diagnostic.Stage.IsEmpty()) Failure.Diagnostic.Stage = Stage;
		Failure.Diagnostic.Severity = UeAgentAuthoringDiagnostics::ESeverity::Error;
		if (Failure.Diagnostic.Message.IsEmpty()) Failure.Diagnostic.Message = Failure.ErrorDetail;
		if (Failure.Diagnostic.Source.SourceFile.IsEmpty()) Failure.Diagnostic.Source = UnitSource(Unit);
	}

	void FailPreflight(
		FPreflightResult& OutResult,
		const FString& CauseCode,
		const FString& ErrorDetail,
		const FWorkspaceUnit* Unit,
		const FOperationFailure* ProviderFailure = nullptr)
	{
		OutResult.Status = TEXT("failed");
		OutResult.ErrorCode = TEXT("workspace_preflight_failed");
		OutResult.CauseCode = CauseCode;
		OutResult.ErrorDetail = ErrorDetail;
		if (ProviderFailure) OutResult.Diagnostic = ProviderFailure->Diagnostic;
		if (OutResult.Diagnostic.Code.IsEmpty()) OutResult.Diagnostic.Code = CauseCode;
		if (OutResult.Diagnostic.Stage.IsEmpty()) OutResult.Diagnostic.Stage = TEXT("workspace_preflight");
		OutResult.Diagnostic.Severity = UeAgentAuthoringDiagnostics::ESeverity::Error;
		OutResult.Diagnostic.bRetrySafe = true;
		if (OutResult.Diagnostic.Message.IsEmpty()) OutResult.Diagnostic.Message = ErrorDetail;
		if (OutResult.Diagnostic.Source.SourceFile.IsEmpty()) OutResult.Diagnostic.Source = UnitSource(Unit);
		OutResult.Units.Reset();
	}

	void FailExecution(
		FExecutionResult& OutResult,
		const FString& Phase,
		const FString& CauseCode,
		const FOperationFailure& Failure)
	{
		OutResult.Status = TEXT("failed");
		OutResult.Phase = Phase;
		OutResult.ErrorCode = TEXT("workspace_transaction_failed");
		OutResult.CauseCode = CauseCode;
		OutResult.ErrorDetail = Failure.ErrorDetail.IsEmpty() ? Failure.ErrorCode : Failure.ErrorDetail;
		OutResult.Diagnostic = Failure.Diagnostic;
		if (OutResult.Diagnostic.Code.IsEmpty()) OutResult.Diagnostic.Code = Failure.ErrorCode;
		if (OutResult.Diagnostic.Stage.IsEmpty()) OutResult.Diagnostic.Stage = Phase;
		OutResult.Diagnostic.Severity = UeAgentAuthoringDiagnostics::ESeverity::Error;
		if (OutResult.Diagnostic.Message.IsEmpty()) OutResult.Diagnostic.Message = OutResult.ErrorDetail;
	}

	void AppendRollbackFailure(FExecutionResult& OutResult, const FOperationFailure& Failure)
	{
		const FString Detail = Failure.ErrorDetail.IsEmpty() ? Failure.ErrorCode : Failure.ErrorDetail;
		if (Detail.IsEmpty()) return;
		if (!OutResult.ErrorDetail.IsEmpty()) OutResult.ErrorDetail += TEXT("; ");
		OutResult.ErrorDetail += TEXT("rollback:") + Detail;
	}
}

namespace UeAgentAuthoringWorkspaceTransaction
{
	bool FDurabilityResult::IsPresent() const
	{
		return !JournalSchema.IsEmpty()
			|| !JournalPhase.IsEmpty()
			|| !RecoveryStatus.IsEmpty()
			|| bReplayed
			|| bRestoreAttempted;
	}

	TSharedRef<FJsonObject> FDurabilityResult::ToJsonObject() const
	{
		TSharedRef<FJsonObject> Object = MakeShared<FJsonObject>();
		Object->SetStringField(
			TEXT("journal_schema"),
			JournalSchema.IsEmpty() ? TEXT("uai.apply_durable_journal.v1") : JournalSchema);
		Object->SetStringField(TEXT("journal_phase"), JournalPhase);
		Object->SetStringField(TEXT("recovery_status"), RecoveryStatus);
		Object->SetBoolField(TEXT("replayed"), bReplayed);
		Object->SetNumberField(TEXT("artifact_count"), ArtifactCount);
		if (!JournalPath.IsEmpty()) Object->SetStringField(TEXT("journal_path"), JournalPath);

		TArray<TSharedPtr<FJsonValue>> HistoryValues;
		for (const FString& Phase : PhaseHistory)
		{
			HistoryValues.Add(MakeShared<FJsonValueString>(Phase));
		}
		Object->SetArrayField(TEXT("phase_history"), HistoryValues);
		if (bRestoreAttempted)
		{
			TSharedRef<FJsonObject> RestoreReceipt = MakeShared<FJsonObject>();
			RestoreReceipt->SetStringField(
				TEXT("status"), bRestoreSucceeded ? TEXT("restored") : TEXT("unconfirmed"));
			RestoreReceipt->SetBoolField(TEXT("attempted"), true);
			RestoreReceipt->SetBoolField(TEXT("confirmed"), bRestoreSucceeded);
			Object->SetObjectField(TEXT("restore_receipt"), RestoreReceipt);
		}
		return Object;
	}

	bool ITransactionBackend::SupportsDurableTransactions() const
	{
		return false;
	}

	bool ITransactionBackend::BuildDurableArtifactInventory(
		const TArray<IPreparedMutation*>& Mutations,
		TArray<FDurableArtifact>& OutArtifacts,
		FOperationFailure& OutFailure)
	{
		OutArtifacts.Reset();
		OutFailure = FOperationFailure();
		OutFailure.ErrorCode = TEXT("durable_backend_not_supported");
		OutFailure.ErrorDetail = OutFailure.ErrorCode;
		OutFailure.Diagnostic.Code = OutFailure.ErrorCode;
		OutFailure.Diagnostic.Stage = TEXT("workspace_durable_preflight");
		OutFailure.Diagnostic.Message = OutFailure.ErrorDetail;
		OutFailure.Diagnostic.bRetrySafe = false;
		return false;
	}

	bool ITransactionBackend::StageDurableArtifacts(
		TArray<FDurableArtifact>& InOutArtifacts,
		FOperationFailure& OutFailure)
	{
		OutFailure = FOperationFailure();
		OutFailure.ErrorCode = TEXT("durable_backend_not_supported");
		OutFailure.ErrorDetail = OutFailure.ErrorCode;
		OutFailure.Diagnostic.Code = OutFailure.ErrorCode;
		OutFailure.Diagnostic.Stage = TEXT("workspace_durable_stage");
		OutFailure.Diagnostic.Message = OutFailure.ErrorDetail;
		OutFailure.Diagnostic.bRetrySafe = false;
		return false;
	}

	void ITransactionBackend::FinalizeDurableCommit(bool bCommitted)
	{
	}

	bool FPreflightResult::IsReady() const
	{
		return bValidated && Status == TEXT("ready") && ErrorCode.IsEmpty();
	}

	bool FExecutionResult::IsApplied() const
	{
		return Status == TEXT("applied") && ErrorCode.IsEmpty();
	}

	TSharedRef<FJsonObject> FExecutionResult::ToJsonObject() const
	{
		TSharedRef<FJsonObject> Object = MakeShared<FJsonObject>();
		Object->SetStringField(TEXT("schema"), TEXT("uai.apply_workspace_execution.v1"));
		Object->SetStringField(TEXT("status"), Status);
		Object->SetStringField(TEXT("phase"), Phase);
		if (!ErrorCode.IsEmpty()) Object->SetStringField(TEXT("error"), ErrorCode);
		if (!CauseCode.IsEmpty()) Object->SetStringField(TEXT("cause"), CauseCode);
		if (!ErrorDetail.IsEmpty()) Object->SetStringField(TEXT("error_detail"), ErrorDetail);
		Object->SetNumberField(TEXT("mutation_attempts"), MutationAttempts);
		Object->SetNumberField(TEXT("mutations_applied"), MutationsApplied);
		Object->SetNumberField(TEXT("rollback_attempts"), RollbackAttempts);
		Object->SetBoolField(TEXT("rollback_attempted"), bRollbackAttempted);
		Object->SetBoolField(TEXT("rollback_succeeded"), bRollbackSucceeded);

		TArray<TSharedPtr<FJsonValue>> AttemptedValues;
		for (const FString& UnitKey : AttemptedUnitKeys)
		{
			AttemptedValues.Add(MakeShared<FJsonValueString>(UnitKey));
		}
		Object->SetArrayField(TEXT("attempted_unit_keys"), AttemptedValues);

		TArray<TSharedPtr<FJsonValue>> RolledBackValues;
		for (const FString& UnitKey : RolledBackUnitKeys)
		{
			RolledBackValues.Add(MakeShared<FJsonValueString>(UnitKey));
		}
		Object->SetArrayField(TEXT("rolled_back_unit_keys"), RolledBackValues);
		if (Durability.IsPresent())
		{
			Object->SetObjectField(TEXT("durability"), Durability.ToJsonObject());
		}
		if (!ErrorCode.IsEmpty()) Object->SetObjectField(TEXT("diagnostic"), Diagnostic.ToJson());
		return Object;
	}

	bool PreflightWorkspace(
		const UeAgentApplyWorkspaceContract::FContract& Contract,
		const UeAgentAuthoringWorkspaceRouter::FWorkspace& Workspace,
		const UeAgentAuthoringWorkspaceDependencyGraph::FDependencyGraph& Graph,
		const IPreflightProvider& Provider,
		FPreflightResult& OutResult)
	{
		OutResult = FPreflightResult();
		if (!Contract.IsValid()
			|| Contract.ContractId != Workspace.ContractId
			|| Contract.ContractVersion != Workspace.ContractVersion
			|| !Graph.IsReadyFor(Workspace))
		{
			FailPreflight(
				OutResult,
				TEXT("workspace_contract_or_graph_invalid"),
				TEXT("workspace contract, routed workspace, and dependency graph must describe the same request"),
				nullptr);
			return false;
		}
		OutResult.ContractId = Contract.ContractId;
		OutResult.ContractVersion = Contract.ContractVersion;
		for (int32 UnitIndex : Graph.OrderedUnitIndices)
		{
			OutResult.OrderedUnitKeys.Add(Workspace.Units[UnitIndex].CanonicalUnitKey);
		}

		TMap<FString, FString> TargetOwners;
		for (int32 UnitIndex : Graph.OrderedUnitIndices)
		{
			if (!Workspace.Units.IsValidIndex(UnitIndex))
			{
				FailPreflight(OutResult, TEXT("workspace_graph_index_invalid"), FString::FromInt(UnitIndex), nullptr);
				return false;
			}
			const FWorkspaceUnit& Unit = Workspace.Units[UnitIndex];
			++OutResult.UnitsChecked;
			if (Unit.Action == EUnitAction::NoChange) continue;

			TUniquePtr<IPreparedMutation> Mutation;
			FOperationFailure Failure;
			if (!Provider.PreflightUnit(Unit, Mutation, Failure) || !Mutation.IsValid())
			{
				CompleteDiagnostic(Failure, TEXT("workspace_unit_preflight_failed"), TEXT("workspace_preflight"), &Unit);
				FailPreflight(OutResult, Failure.ErrorCode, Failure.ErrorDetail, &Unit, &Failure);
				return false;
			}
			if (Mutation->GetUnitKey() != Unit.CanonicalUnitKey)
			{
				FailPreflight(
					OutResult,
					TEXT("workspace_prepared_unit_mismatch"),
					Unit.CanonicalUnitKey + TEXT(" != ") + Mutation->GetUnitKey(),
					&Unit);
				return false;
			}

			const FString TargetKey = Mutation->GetTargetKey();
			const FString FoldedTarget = FoldTargetKey(TargetKey);
			if (!FoldedTarget.IsEmpty())
			{
				if (const FString* ExistingUnit = TargetOwners.Find(FoldedTarget))
				{
					FailPreflight(
						OutResult,
						TEXT("workspace_duplicate_target"),
						TargetKey + TEXT(":") + *ExistingUnit + TEXT(":") + Unit.CanonicalUnitKey,
						&Unit);
					return false;
				}
				TargetOwners.Add(FoldedTarget, Unit.CanonicalUnitKey);
			}

			FPreparedUnit& PreparedUnit = OutResult.Units.AddDefaulted_GetRef();
			PreparedUnit.WorkspaceUnitIndex = UnitIndex;
			PreparedUnit.Mutation = MoveTemp(Mutation);
		}

		OutResult.Status = TEXT("ready");
		OutResult.bValidated = true;
		return true;
	}

	bool ExecuteAtomic(
		const UeAgentApplyWorkspaceContract::FContract& Contract,
		const UeAgentAuthoringWorkspaceRouter::FWorkspace& Workspace,
		const UeAgentAuthoringWorkspaceDependencyGraph::FDependencyGraph& Graph,
		FPreflightResult& Prepared,
		ITransactionBackend& Backend,
		const FExecutionOptions& Options,
		FExecutionResult& OutResult)
	{
		OutResult = FExecutionResult();
		OutResult.Status = TEXT("failed");
		OutResult.Phase = TEXT("preflight");
		if (!Contract.IsValid()
			|| Contract.ContractId != Workspace.ContractId
			|| Contract.ContractVersion != Workspace.ContractVersion
			|| !Graph.IsReadyFor(Workspace))
		{
			FOperationFailure Failure;
			Failure.ErrorCode = TEXT("workspace_contract_or_graph_invalid");
			Failure.ErrorDetail = Failure.ErrorCode;
			FailExecution(OutResult, TEXT("preflight"), TEXT("preflight_failed"), Failure);
			OutResult.ErrorCode = TEXT("workspace_preflight_failed");
			return false;
		}

		TArray<FString> CurrentOrderedUnitKeys;
		for (int32 UnitIndex : Graph.OrderedUnitIndices)
		{
			if (Workspace.Units.IsValidIndex(UnitIndex))
			{
				CurrentOrderedUnitKeys.Add(Workspace.Units[UnitIndex].CanonicalUnitKey);
			}
		}
		bool bPreparedUnitsMatch = true;
		int32 ExpectedPreparedIndex = 0;
		for (int32 UnitIndex : Graph.OrderedUnitIndices)
		{
			if (Workspace.Units[UnitIndex].Action == EUnitAction::NoChange) continue;
			if (!Prepared.Units.IsValidIndex(ExpectedPreparedIndex)
				|| Prepared.Units[ExpectedPreparedIndex].WorkspaceUnitIndex != UnitIndex
				|| !Prepared.Units[ExpectedPreparedIndex].Mutation.IsValid()
				|| Prepared.Units[ExpectedPreparedIndex].Mutation->GetUnitKey()
					!= Workspace.Units[UnitIndex].CanonicalUnitKey)
			{
				bPreparedUnitsMatch = false;
				break;
			}
			++ExpectedPreparedIndex;
		}
		bPreparedUnitsMatch = bPreparedUnitsMatch && ExpectedPreparedIndex == Prepared.Units.Num();
		if (!Prepared.IsReady()
			|| Prepared.ContractId != Contract.ContractId
			|| Prepared.ContractVersion != Contract.ContractVersion
			|| Prepared.OrderedUnitKeys != CurrentOrderedUnitKeys
			|| !bPreparedUnitsMatch)
		{
			FOperationFailure Failure;
			Failure.ErrorCode = TEXT("workspace_preflight_not_ready");
			Failure.ErrorDetail = Prepared.ErrorDetail.IsEmpty() ? Failure.ErrorCode : Prepared.ErrorDetail;
			FailExecution(OutResult, TEXT("preflight"), TEXT("preflight_failed"), Failure);
			OutResult.ErrorCode = TEXT("workspace_preflight_failed");
			return false;
		}

		const bool bCallerControlledSave =
			Contract.Execution.Save == TEXT("caller_controlled")
			|| Contract.Execution.Save
				== TEXT("caller_controlled_required_default");
		const bool bRunSave = Contract.Execution.Save == TEXT("required")
			|| (bCallerControlledSave && Options.bSaveWhenCallerControlled);
		TArray<IPreparedMutation*> EffectfulMutations;
		for (FPreparedUnit& Unit : Prepared.Units)
		{
			if (Unit.Mutation.IsValid() && !Unit.Mutation->IsNoOp())
			{
				EffectfulMutations.Add(Unit.Mutation.Get());
			}
		}
		const bool bHasRequestId = !Options.RequestId.TrimStartAndEnd().IsEmpty();
		const bool bHasJournalRoot = !Options.JournalRoot.TrimStartAndEnd().IsEmpty();
		if (bHasRequestId != bHasJournalRoot)
		{
			FOperationFailure Failure;
			Failure.ErrorCode = TEXT("durable_request_context_incomplete");
			Failure.ErrorDetail = Failure.ErrorCode;
			CompleteDiagnostic(Failure, Failure.ErrorCode, TEXT("workspace_durable_preflight"), nullptr);
			FailExecution(OutResult, TEXT("preflight"), TEXT("preflight_failed"), Failure);
			OutResult.ErrorCode = TEXT("workspace_preflight_failed");
			return false;
		}
		const bool bHasDurableContext = bHasRequestId && bHasJournalRoot;
		const bool bUseDurableSave = bRunSave && bHasDurableContext && !EffectfulMutations.IsEmpty();
		FDurableTransactionCoordinator Durable;
		FString DurableFingerprint;
		if (bHasDurableContext)
		{
			DurableFingerprint = BuildDurableRequestFingerprint(Contract, Workspace, Graph, Prepared);
			FOperationFailure DurableFailure;
			if (!Durable.RecoverPending(Options, OutResult.Durability, DurableFailure))
			{
				CompleteDiagnostic(DurableFailure, TEXT("durable_recovery_failed"), TEXT("workspace_durable_recovery"), nullptr);
				FailExecution(OutResult, TEXT("recovery"), TEXT("durable_recovery_failed"), DurableFailure);
				OutResult.Diagnostic.bRetrySafe = false;
				return false;
			}
			bool bReplayed = false;
			if (!Durable.TryReplay(
				Options, DurableFingerprint, bReplayed, OutResult.Durability, DurableFailure))
			{
				CompleteDiagnostic(DurableFailure, TEXT("durable_replay_failed"), TEXT("workspace_durable_replay"), nullptr);
				FailExecution(OutResult, TEXT("replay"), TEXT("durable_replay_failed"), DurableFailure);
				OutResult.Diagnostic.bRetrySafe = false;
				return false;
			}
			if (bReplayed)
			{
				OutResult.Status = TEXT("applied");
				OutResult.Phase = TEXT("complete");
				OutResult.ErrorCode.Reset();
				OutResult.ErrorDetail.Reset();
				OutResult.CauseCode.Reset();
				return true;
			}
		}

		FOperationFailure Failure;
		if (!Backend.BeginWorkspace(Workspace, Graph, Failure))
		{
			CompleteDiagnostic(Failure, TEXT("workspace_transaction_begin_failed"), TEXT("workspace_transaction_begin"), nullptr);
			FailExecution(OutResult, TEXT("begin"), TEXT("transaction_begin_failed"), Failure);
			return false;
		}
		bool bTransactionOpen = true;

		auto EndRollback = [&](int32 LastMutationIndex)
		{
			OutResult.bRollbackAttempted = true;
			bool bMemoryRollbackOk = true;
			for (int32 PreparedIndex = LastMutationIndex; PreparedIndex >= 0; --PreparedIndex)
			{
				FPreparedUnit& Unit = Prepared.Units[PreparedIndex];
				if (!Unit.Mutation.IsValid() || Unit.Mutation->IsNoOp()) continue;
				++OutResult.RollbackAttempts;
				FOperationFailure RollbackFailure;
				if (!Backend.RollbackUnit(*Unit.Mutation, RollbackFailure))
				{
					bMemoryRollbackOk = false;
					CompleteDiagnostic(RollbackFailure, TEXT("workspace_unit_rollback_failed"), TEXT("workspace_rollback"), nullptr);
					AppendRollbackFailure(OutResult, RollbackFailure);
				}
				else
				{
					OutResult.RolledBackUnitKeys.Add(Unit.Mutation->GetUnitKey());
				}
			}
			if (bTransactionOpen)
			{
				FOperationFailure EndFailure;
				if (!Backend.EndWorkspace(false, EndFailure))
				{
					bMemoryRollbackOk = false;
					CompleteDiagnostic(EndFailure, TEXT("workspace_transaction_abort_failed"), TEXT("workspace_rollback"), nullptr);
					AppendRollbackFailure(OutResult, EndFailure);
				}
				bTransactionOpen = false;
			}
			bool bDurableRollbackOk = true;
			if (Durable.IsPrepared())
			{
				FOperationFailure DurableRollbackFailure;
				bDurableRollbackOk = Durable.Restore(
					bMemoryRollbackOk,
					OutResult.Durability,
					DurableRollbackFailure);
				if (!bDurableRollbackOk)
				{
					CompleteDiagnostic(
						DurableRollbackFailure,
						TEXT("durable_restore_unconfirmed"),
						TEXT("workspace_durable_restore"),
						nullptr);
					AppendRollbackFailure(OutResult, DurableRollbackFailure);
				}
			}
			OutResult.bRollbackSucceeded = bMemoryRollbackOk && bDurableRollbackOk;
			if (!OutResult.bRollbackSucceeded)
			{
				OutResult.Diagnostic.bRetrySafe = false;
			}
		};

		for (FPreparedUnit& Unit : Prepared.Units)
		{
			if (Unit.Mutation.IsValid() && Unit.Mutation->IsNoOp()) continue;
			Failure = FOperationFailure();
			if (!Unit.Mutation.IsValid() || !Backend.CaptureRollback(*Unit.Mutation, Failure))
			{
				CompleteDiagnostic(Failure, TEXT("workspace_rollback_capture_failed"), TEXT("workspace_rollback_capture"), nullptr);
				FailExecution(OutResult, TEXT("capture_rollback"), TEXT("preflight_failed"), Failure);
				EndRollback(INDEX_NONE);
				return false;
			}
		}

		if (bUseDurableSave)
		{
			Failure = FOperationFailure();
			if (!Durable.Prepare(
				Options,
				DurableFingerprint,
				Backend,
				EffectfulMutations,
				OutResult.Durability,
				Failure))
			{
				CompleteDiagnostic(Failure, TEXT("durable_prepare_failed"), TEXT("workspace_durable_preflight"), nullptr);
				FailExecution(OutResult, TEXT("durable_prepare"), TEXT("preflight_failed"), Failure);
				OutResult.Diagnostic.bRetrySafe = false;
				EndRollback(INDEX_NONE);
				return false;
			}
		}

		for (int32 PreparedIndex = 0; PreparedIndex < Prepared.Units.Num(); ++PreparedIndex)
		{
			FPreparedUnit& Unit = Prepared.Units[PreparedIndex];
			if (Unit.Mutation.IsValid() && Unit.Mutation->IsNoOp()) continue;
			++OutResult.MutationAttempts;
			OutResult.AttemptedUnitKeys.Add(Unit.Mutation->GetUnitKey());
			Failure = FOperationFailure();
			if (!Backend.ApplyUnit(*Unit.Mutation, Failure))
			{
				CompleteDiagnostic(Failure, TEXT("workspace_unit_mutation_failed"), TEXT("workspace_mutation"), nullptr);
				FailExecution(OutResult, TEXT("mutation"), TEXT("mutation_failed"), Failure);
				EndRollback(PreparedIndex);
				return false;
			}
			++OutResult.MutationsApplied;
		}

		if (Contract.Execution.Compile == TEXT("provider_hook")
			|| Contract.Execution.Compile
				== TEXT("provider_hook_required"))
		{
			Failure = FOperationFailure();
			if (!Backend.Compile(Failure))
			{
				CompleteDiagnostic(Failure, TEXT("workspace_compile_failed"), TEXT("workspace_compile"), nullptr);
				FailExecution(OutResult, TEXT("compile"), TEXT("compile_failed"), Failure);
				EndRollback(Prepared.Units.Num() - 1);
				return false;
			}
		}

		Failure = FOperationFailure();
		if (!Backend.Verify(Failure))
		{
			CompleteDiagnostic(Failure, TEXT("workspace_verification_failed"), TEXT("workspace_verification"), nullptr);
			FailExecution(OutResult, TEXT("verification"), TEXT("verification_failed"), Failure);
			EndRollback(Prepared.Units.Num() - 1);
			return false;
		}

		if (bRunSave)
		{
			if (bUseDurableSave)
			{
				Failure = FOperationFailure();
				if (!Durable.Stage(Backend, OutResult.Durability, Failure)
					|| !Durable.Publish(OutResult.Durability, Failure))
				{
					CompleteDiagnostic(Failure, TEXT("workspace_save_failed"), TEXT("workspace_durable_save"), nullptr);
					FailExecution(OutResult, TEXT("save"), TEXT("save_failed"), Failure);
					OutResult.Diagnostic.bRetrySafe = false;
					EndRollback(Prepared.Units.Num() - 1);
					return false;
				}
				Failure = FOperationFailure();
				if (!Backend.Verify(Failure))
				{
					CompleteDiagnostic(Failure, TEXT("workspace_post_save_verification_failed"), TEXT("workspace_post_save_verification"), nullptr);
					FailExecution(OutResult, TEXT("verification"), TEXT("post_save_verification_failed"), Failure);
					EndRollback(Prepared.Units.Num() - 1);
					return false;
				}
			}
			else
			{
				Failure = FOperationFailure();
				if (!Backend.Save(Failure))
				{
					CompleteDiagnostic(Failure, TEXT("workspace_save_failed"), TEXT("workspace_save"), nullptr);
					FailExecution(OutResult, TEXT("save"), TEXT("save_failed"), Failure);
					EndRollback(Prepared.Units.Num() - 1);
					return false;
				}
			}
		}

		Failure = FOperationFailure();
		if (!Backend.EndWorkspace(true, Failure))
		{
			CompleteDiagnostic(Failure, TEXT("workspace_transaction_commit_failed"), TEXT("workspace_commit"), nullptr);
			FailExecution(OutResult, TEXT("commit"), TEXT("transaction_commit_failed"), Failure);
			EndRollback(Prepared.Units.Num() - 1);
			return false;
		}
		bTransactionOpen = false;
		if (Durable.IsPrepared())
		{
			Failure = FOperationFailure();
			if (!Durable.Commit(OutResult.Durability, Failure))
			{
				CompleteDiagnostic(Failure, TEXT("durable_commit_failed"), TEXT("workspace_durable_commit"), nullptr);
				FailExecution(OutResult, TEXT("commit"), TEXT("transaction_commit_failed"), Failure);
				OutResult.Diagnostic.bRetrySafe = false;
				EndRollback(Prepared.Units.Num() - 1);
				Backend.FinalizeDurableCommit(false);
				return false;
			}
			Backend.FinalizeDurableCommit(true);
		}
		OutResult.Status = TEXT("applied");
		OutResult.Phase = TEXT("complete");
		OutResult.ErrorCode.Reset();
		OutResult.ErrorDetail.Reset();
		OutResult.CauseCode.Reset();
		return true;
	}
}
