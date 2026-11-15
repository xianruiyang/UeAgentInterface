// Copyright Epic Games, Inc. All Rights Reserved.

#include "Utilities/AuthoringWorkspaceApply/UeAgentAuthoringWorkspaceTransaction.h"

#include "Utilities/AuthoringWorkspaceApply/UeAgentAuthoringDurableTransaction.h"

#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"

namespace
{
	using namespace UeAgentAuthoringWorkspaceRouter;
	using namespace UeAgentAuthoringWorkspaceTransaction;

	thread_local const FFinalReadbackEvidence* ActiveFinalReadbackEvidence = nullptr;

	FString FoldTargetKey(const FString& Value)
	{
		FString Result = Value;
		Result.TrimStartAndEndInline();
		return Result.ToLower();
	}

	UeAgentAuthoringDiagnostics::FSourceLocation UnitSource(const FWorkspaceUnit* Unit)
	{
		return Unit ? Unit->ResolveDiagnosticSource() : UeAgentAuthoringDiagnostics::FSourceLocation();
	}

	void CompleteDiagnostic(
		FOperationFailure& Failure,
		const FString& DefaultCode,
		const FString& Stage,
		const FWorkspaceUnit* Unit)
	{
		if (Failure.Diagnostic.IsEmpty())
			Failure.Diagnostic = UeAgentAuthoringDiagnostics::FDiagnostic::Failure(DefaultCode,
				TEXT("The provider failed without reporting a diagnostic."), Stage);
		if (Failure.Diagnostic.Stage.IsEmpty()) Failure.Diagnostic.Stage = Stage;
		Failure.Diagnostic.Severity = UeAgentAuthoringDiagnostics::ESeverity::Error;
		if (Unit && Unit->Document)
		{
			// Prepared mutations execute after the preflight source scope has ended.
			// Resolve the producer's path against the original unit, including child causes.
			UeAgentAuthoringDiagnostics::FScopedSourceLocations Sources(Unit->Document.Get(), [Unit](const FString& Pointer) {
				return Unit->ResolveDiagnosticSource(Pointer);
			});
			UeAgentAuthoringDiagnostics::LocateAuthoringSource(Failure.Diagnostic, *Unit->Document);
		}
		if (Failure.Diagnostic.Source.SourceFile.IsEmpty()
			&& Failure.Diagnostic.SourceKind != TEXT("native_result")
			&& Failure.Diagnostic.SourceKind != TEXT("command_input"))
		{
			const auto Source = UnitSource(Unit);
			if (Failure.Diagnostic.Source.WorkspaceSource.IsEmpty()) Failure.Diagnostic.Source.WorkspaceSource = Source.WorkspaceSource;
			if (Failure.Diagnostic.Source.SourceUnit.IsEmpty()) Failure.Diagnostic.Source.SourceUnit = Source.SourceUnit;
			Failure.Diagnostic.Source.SourceFile = Source.SourceFile;
			if (Failure.Diagnostic.Source.JsonPath.IsEmpty()) Failure.Diagnostic.Source.JsonPath = Source.JsonPath;
			if (Failure.Diagnostic.Source.Line <= 0) Failure.Diagnostic.Source.Line = Source.Line;
			if (Failure.Diagnostic.Source.Column <= 0) Failure.Diagnostic.Source.Column = Source.Column;
		}
	}

	void FailPreflight(
		FPreflightResult& OutResult,
		UeAgentAuthoringDiagnostics::FDiagnostic Diagnostic,
		const FWorkspaceUnit* Unit)
	{
		OutResult.Status = TEXT("failed");

		if (Diagnostic.Stage.IsEmpty()) Diagnostic.Stage = TEXT("workspace_preflight");
		Diagnostic.Severity = UeAgentAuthoringDiagnostics::ESeverity::Error;
		if (Diagnostic.Source.SourceFile.IsEmpty() && Unit && Unit->Document)
			UeAgentAuthoringDiagnostics::LocateAuthoringSource(Diagnostic, *Unit->Document);
		OutResult.Diagnostic.Append(Diagnostic);

		OutResult.Units.Reset();
	}

    bool ValidateWorkspaceInputs(const UeAgentApplyWorkspaceContract::FContract& Contract,
        const FWorkspace& Workspace, const UeAgentAuthoringWorkspaceDependencyGraph::FDependencyGraph& Graph,
        UeAgentAuthoringDiagnostics::FDiagnostic& OutError)
    {
        if (!UeAgentApplyWorkspaceContract::ValidateContract(Contract, OutError)) return false;
        if (Contract.ContractId != Workspace.ContractId || Contract.ContractVersion != Workspace.ContractVersion) {
            const bool bIdMismatch = Contract.ContractId != Workspace.ContractId;
            OutError = UeAgentAuthoringDiagnostics::MakeValueFailure(TEXT("workspace_contract_mismatch"),
                bIdMismatch ? TEXT("/workspace/contract_id") : TEXT("/workspace/contract_version"),
                MakeShared<FJsonValueString>(bIdMismatch ? Contract.ContractId : Contract.ContractVersion),
                MakeShared<FJsonValueString>(bIdMismatch ? Workspace.ContractId : Workspace.ContractVersion),
                TEXT("workspace_preflight"), bIdMismatch ? TEXT("The workspace was routed using a different contract.") : TEXT("The workspace was routed using a different contract version."));
            OutError.SourceKind = TEXT("native_result");
            return false;
        }
        return Graph.IsReadyFor(Workspace, &OutError);
    }

	void FailExecution(
		FExecutionResult& OutResult,
		const FString& Phase,
		const FString& CauseCode,
		const FOperationFailure& Failure)
	{
		OutResult.Status = TEXT("failed");
		OutResult.Phase = Phase;


		OutResult.Diagnostic = Failure.Diagnostic;
		// A failed native apply may have changed state before returning. Preserve
		// the execution fact even when rollback restores that state successfully.
		OutResult.Diagnostic.MutationStarted = OutResult.MutationAttempts > 0
			|| OutResult.Diagnostic.MutationStarted.Get(false);
		if (OutResult.Diagnostic.CauseCode.IsEmpty()) OutResult.Diagnostic.CauseCode = CauseCode;
		if (OutResult.Diagnostic.Stage.IsEmpty()) OutResult.Diagnostic.Stage = Phase;
		OutResult.Diagnostic.Severity = UeAgentAuthoringDiagnostics::ESeverity::Error;
	}

	void AppendRollbackFailure(FExecutionResult& OutResult, const FOperationFailure& Failure)
	{
		FOperationFailure Rollback = Failure;
		CompleteDiagnostic(Rollback, TEXT("workspace_rollback_failed"), TEXT("workspace_rollback"), nullptr);
		Rollback.Diagnostic.bRetrySafe = false;
		OutResult.Diagnostic.Append(Rollback.Diagnostic);
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
			|| bRestoreAttempted
			|| !Warnings.IsEmpty();
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
		if (!JournalPath.IsEmpty()) Object->SetStringField(TEXT("journal_id"), MakeDurableJournalRecoveryId(JournalPath));

		TArray<TSharedPtr<FJsonValue>> HistoryValues;
		for (const FString& Phase : PhaseHistory)
		{
			HistoryValues.Add(MakeShared<FJsonValueString>(Phase));
		}
		Object->SetArrayField(TEXT("phase_history"), HistoryValues);
		if (!Warnings.IsEmpty()) Object->SetArrayField(TEXT("warnings"), UE::UAI::Contract::ToPublicDiagnostics(Warnings));
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
		OutFailure.Diagnostic = UeAgentAuthoringDiagnostics::FDiagnostic::Failure(
			TEXT("durable_backend_not_supported"), TEXT("The backend does not support durable transactions."), TEXT("workspace_durable_preflight"));
		OutFailure.Diagnostic.bRetrySafe = false;
		return false;
	}

	bool ITransactionBackend::StageDurableArtifacts(
		TArray<FDurableArtifact>& InOutArtifacts,
		FOperationFailure& OutFailure)
	{
		OutFailure = FOperationFailure();
		OutFailure.Diagnostic = UeAgentAuthoringDiagnostics::FDiagnostic::Failure(
			TEXT("durable_backend_not_supported"), TEXT("The backend does not support durable transactions."), TEXT("workspace_durable_stage"));
		OutFailure.Diagnostic.bRetrySafe = false;
		return false;
	}

	bool ITransactionBackend::FinalizeDurableCommit(bool bCommitted, FOperationFailure& OutFailure)
	{
		OutFailure = FOperationFailure();
		return true;
	}

 bool FPreflightResult::IsReady() const { return IsReady(nullptr); }

 bool FPreflightResult::IsReady(UeAgentAuthoringDiagnostics::FDiagnostic* OutError) const
 {
   if (OutError) OutError->Reset();
   if (bValidated && Status == TEXT("ready") && Diagnostic.IsEmpty()) return true;
   if (OutError) {
     if (!Diagnostic.IsEmpty()) *OutError = UeAgentAuthoringDiagnostics::FDiagnostic::WrapFailure(
         TEXT("workspace_preflight_not_ready"), Diagnostic, TEXT("workspace_preflight"), TEXT("The workspace preflight reported a failure."));
     else if (!bValidated) *OutError = UeAgentAuthoringDiagnostics::MakeValueFailure(TEXT("workspace_preflight_not_ready"),
         TEXT("/preflight/validated"), MakeShared<FJsonValueBoolean>(true), MakeShared<FJsonValueBoolean>(false), TEXT("workspace_preflight"), TEXT("The prepared workspace has not completed validation."));
     else *OutError = UeAgentAuthoringDiagnostics::MakeValueFailure(TEXT("workspace_preflight_not_ready"),
         TEXT("/preflight/status"), MakeShared<FJsonValueString>(TEXT("ready")), MakeShared<FJsonValueString>(Status), TEXT("workspace_preflight"), TEXT("The preflight status does not permit execution."));
     if (Diagnostic.IsEmpty()) OutError->SourceKind = TEXT("native_result");
   }
   return false;
 }

	bool FExecutionResult::IsApplied() const
	{
		return Status == TEXT("applied") && Diagnostic.IsEmpty();
	}

	TSharedRef<FJsonObject> FFinalReadbackEvidence::ToJsonObject() const
	{
		TSharedRef<FJsonObject> Object = MakeShared<FJsonObject>();
		Object->SetBoolField(TEXT("mutation_completed"), bMutationPhaseCompleted);
		Object->SetBoolField(TEXT("native_update_requested"), bNativeUpdateRequested);
		Object->SetBoolField(TEXT("native_update_completed"), bNativeUpdateCompleted);
		Object->SetBoolField(TEXT("save_completed"), bSaveCompleted);
		return Object;
	}

	bool FFinalReadbackEvidence::TryFromJsonObject(const FJsonObject& Object, FFinalReadbackEvidence& OutEvidence,
		UE::UAI::Contract::FDiagnostic& OutError)
	{
		OutEvidence = FFinalReadbackEvidence();
		OutError.Reset();
		FFinalReadbackEvidence Evidence;
		auto ReadBool = [&](const TCHAR* Field, bool& OutValue)
		{
			if (Object.TryGetBoolField(Field, OutValue)) return true;
			const auto Value = Object.TryGetField(Field);
			const TCHAR* Kind = TEXT("missing");
			if (Value.IsValid())
			{
				switch (Value->Type)
				{
				case EJson::None: Kind = TEXT("none"); break;
				case EJson::Null: Kind = TEXT("null"); break;
				case EJson::String: Kind = TEXT("string"); break;
				case EJson::Number: Kind = TEXT("number"); break;
				case EJson::Boolean: Kind = TEXT("boolean"); break;
				case EJson::Array: Kind = TEXT("array"); break;
				case EJson::Object: Kind = TEXT("object"); break;
				}
			}
			OutError = UE::UAI::Contract::FDiagnostic::Failure(TEXT("authoring_operation_stages_field_invalid"),
				Value.IsValid() ? TEXT("The native operation stage field must be a boolean.") : TEXT("The native operation stage field is missing."),
				TEXT("native_readback"));
			OutError.SourceKind = TEXT("native_result");
			OutError.PropertyPath = TEXT("/operation_stages/") + FString(Field);
			OutError.ActualValuePresent = Value.IsValid();
			if (Value.IsValid()) OutError.ActualValue = MakeShared<FJsonValueString>(Kind);
			OutError.ExpectedValuePresent = true;
			OutError.ExpectedValue = MakeShared<FJsonValueString>(TEXT("boolean"));
			return false;
		};
		if (!ReadBool(TEXT("mutation_completed"), Evidence.bMutationPhaseCompleted)
			|| !ReadBool(TEXT("native_update_requested"), Evidence.bNativeUpdateRequested)
			|| !ReadBool(TEXT("native_update_completed"), Evidence.bNativeUpdateCompleted)
			|| !ReadBool(TEXT("save_completed"), Evidence.bSaveCompleted)) return false;
		OutEvidence = Evidence;
		return true;
	}

	bool TryGetFinalReadbackEvidence(FFinalReadbackEvidence& OutEvidence)
	{
		OutEvidence = ActiveFinalReadbackEvidence ? *ActiveFinalReadbackEvidence : FFinalReadbackEvidence();
		return ActiveFinalReadbackEvidence != nullptr;
	}

	TSharedRef<FJsonObject> FExecutionResult::ToJsonObject() const
	{
		TSharedRef<FJsonObject> Object = MakeShared<FJsonObject>();
		Object->SetStringField(TEXT("schema"), TEXT("uai.apply_workspace_execution.v1"));
		Object->SetStringField(TEXT("status"), Status);
		Object->SetStringField(TEXT("phase"), Phase);
		if (!Diagnostic.IsEmpty())
		{
			// Compatibility fields are derived only at the report boundary.
			Object->SetStringField(TEXT("error"), Phase == TEXT("preflight")
				? TEXT("workspace_preflight_failed") : TEXT("workspace_transaction_failed"));
			Object->SetStringField(TEXT("cause"), Diagnostic.CauseCode.IsEmpty() ? Diagnostic.Code : Diagnostic.CauseCode);
		}
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
		if (!Diagnostic.IsEmpty()) Object->SetObjectField(TEXT("diagnostic"), Diagnostic.ToPublicJson());
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
        UeAgentAuthoringDiagnostics::FDiagnostic InputFailure;
        if (!ValidateWorkspaceInputs(Contract, Workspace, Graph, InputFailure)) {
            FailPreflight(OutResult, MoveTemp(InputFailure), nullptr);
            return false;
        }
		OutResult.ContractId = Contract.ContractId;
		OutResult.ContractVersion = Contract.ContractVersion;
		for (int32 UnitIndex : Graph.OrderedUnitIndices)
		{
			OutResult.OrderedUnitKeys.Add(Workspace.Units[UnitIndex].CanonicalUnitKey);
		}

		TMap<FString, FString> TargetOwners;
		TSet<int32> FailedUnits;
		for (int32 UnitIndex : Graph.OrderedUnitIndices)
		{
			const FWorkspaceUnit& Unit = Workspace.Units[UnitIndex];
			if (Graph.Edges.ContainsByPredicate([&](const auto& Edge) {
				return Edge.AfterUnitIndex == UnitIndex && FailedUnits.Contains(Edge.BeforeUnitIndex);
			}))
			{
				const auto Skipped = MakeShared<FJsonObject>();
				Skipped->SetStringField(TEXT("stage"), TEXT("workspace_preflight"));
				Skipped->SetStringField(TEXT("reason"), TEXT("dependency_failed"));
				const auto Source = UnitSource(&Unit);
				if (!Source.SourceFile.IsEmpty()) Skipped->SetStringField(TEXT("source_file"), Source.SourceFile);
				if (!Source.JsonPath.IsEmpty()) Skipped->SetStringField(TEXT("json_pointer"), Source.JsonPath);
				OutResult.Diagnostic.Unchecked.Add(MakeShared<FJsonValueObject>(Skipped));
				FailedUnits.Add(UnitIndex);
				continue;
			}
			++OutResult.UnitsChecked;
			if (Unit.Action == EUnitAction::NoChange) continue;

			TUniquePtr<IPreparedMutation> Mutation;
			FOperationFailure Failure;
			UeAgentAuthoringDiagnostics::FScopedSourceLocations Sources(Unit.Document.Get(), [&](const FString& Pointer) {
				return Unit.ResolveDiagnosticSource(Pointer);
			});
            const bool bProviderReady = Provider.PreflightUnit(Unit, Mutation, Failure);
            if (!bProviderReady || !Mutation.IsValid())
            {
                if (bProviderReady && !Mutation.IsValid()) {
                    auto MissingMutation = UeAgentAuthoringDiagnostics::MakeValueFailure(TEXT("workspace_unit_preflight_failed"),
                        TEXT("prepared_mutation"), MakeShared<FJsonValueString>(TEXT("prepared mutation object")), nullptr,
                        TEXT("workspace_preflight"), TEXT("The provider reported successful preflight without returning a prepared mutation."));
                    MissingMutation.SourceKind = TEXT("native_result");
                    Failure.Diagnostic.Append(MissingMutation);
                }
                CompleteDiagnostic(Failure, TEXT("workspace_unit_preflight_failed"), TEXT("workspace_preflight"), &Unit);
				FailPreflight(OutResult, Failure.Diagnostic, &Unit);
				const auto Unchecked = MakeShared<FJsonObject>();
				Unchecked->SetStringField(TEXT("stage"), TEXT("unit_preflight"));
				Unchecked->SetStringField(TEXT("reason"), TEXT("provider_stopped_at_error"));
				const auto Source = UnitSource(&Unit);
				if (!Source.SourceFile.IsEmpty()) Unchecked->SetStringField(TEXT("source_file"), Source.SourceFile);
				if (!Source.JsonPath.IsEmpty()) Unchecked->SetStringField(TEXT("json_pointer"), Source.JsonPath);
				OutResult.Diagnostic.Unchecked.Add(MakeShared<FJsonValueObject>(Unchecked));
				FailedUnits.Add(UnitIndex);
				continue;
			}
			if (Mutation->GetUnitKey() != Unit.CanonicalUnitKey)
			{
                auto Diagnostic = UeAgentAuthoringDiagnostics::MakeValueFailure(TEXT("workspace_prepared_unit_mismatch"),
                    TEXT("prepared_mutation/unit_key"), MakeShared<FJsonValueString>(Unit.CanonicalUnitKey), MakeShared<FJsonValueString>(Mutation->GetUnitKey()),
                    TEXT("workspace_preflight"), TEXT("The provider returned a mutation for a different workspace unit."));
                Diagnostic.SourceKind = TEXT("native_result");
                FailPreflight(OutResult, MoveTemp(Diagnostic), &Unit);
				FailedUnits.Add(UnitIndex);
				continue;
			}

			const FString TargetKey = Mutation->GetTargetKey();
			const FString FoldedTarget = FoldTargetKey(TargetKey);
			if (!FoldedTarget.IsEmpty())
			{
				if (const FString* ExistingUnit = TargetOwners.Find(FoldedTarget))
				{
                    auto Diagnostic = UeAgentAuthoringDiagnostics::MakeValueFailure(TEXT("workspace_duplicate_target"),
                        TEXT("target_owner"), MakeShared<FJsonValueString>(*ExistingUnit), MakeShared<FJsonValueString>(Unit.CanonicalUnitKey),
                        TEXT("workspace_preflight"), TEXT("Two workspace units resolve to the same mutation target."));
                    Diagnostic.TargetPath = TargetKey;
                    Diagnostic.Source = UnitSource(&Unit);
                    FailPreflight(OutResult, MoveTemp(Diagnostic), &Unit);
					FailedUnits.Add(UnitIndex);
					continue;
				}
				TargetOwners.Add(FoldedTarget, Unit.CanonicalUnitKey);
			}

			FPreparedUnit& PreparedUnit = OutResult.Units.AddDefaulted_GetRef();
			PreparedUnit.WorkspaceUnitIndex = UnitIndex;
			PreparedUnit.Mutation = MoveTemp(Mutation);
		}

		if (!FailedUnits.IsEmpty())
		{
			OutResult.Units.Reset();
			return false;
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
        const auto Reject = [&](UeAgentAuthoringDiagnostics::FDiagnostic Diagnostic) {
            FOperationFailure Failure;
            Failure.Diagnostic = MoveTemp(Diagnostic);
            FailExecution(OutResult, TEXT("preflight"), TEXT("preflight_failed"), Failure);
            return false;
        };
        const auto Difference = [&](const TCHAR* Reason, const FString& Path,
                                    const TSharedPtr<FJsonValue>& Expected, const TSharedPtr<FJsonValue>& Actual) {
            auto Diagnostic = UeAgentAuthoringDiagnostics::MakeValueFailure(TEXT("workspace_preflight_not_ready"), Path, Expected, Actual, TEXT("workspace_preflight"), Reason);
            Diagnostic.SourceKind = TEXT("native_result");
            return Reject(MoveTemp(Diagnostic));
        };
        UeAgentAuthoringDiagnostics::FDiagnostic ReadyError;
        if (!ValidateWorkspaceInputs(Contract, Workspace, Graph, ReadyError)) return Reject(MoveTemp(ReadyError));
        if (!Prepared.IsReady(&ReadyError)) return Reject(MoveTemp(ReadyError));
        if (Prepared.ContractId != Contract.ContractId)
            return Difference(TEXT("The preflight belongs to a different workspace contract."), TEXT("/preflight/contract_id"), MakeShared<FJsonValueString>(Contract.ContractId), MakeShared<FJsonValueString>(Prepared.ContractId));
        if (Prepared.ContractVersion != Contract.ContractVersion)
            return Difference(TEXT("The preflight belongs to a different contract version."), TEXT("/preflight/contract_version"), MakeShared<FJsonValueString>(Contract.ContractVersion), MakeShared<FJsonValueString>(Prepared.ContractVersion));
        if (Prepared.OrderedUnitKeys.Num() != Graph.OrderedUnitIndices.Num())
            return Difference(TEXT("The prepared unit order has a different number of entries."), TEXT("/preflight/ordered_unit_keys/length"), MakeShared<FJsonValueNumber>(Graph.OrderedUnitIndices.Num()), MakeShared<FJsonValueNumber>(Prepared.OrderedUnitKeys.Num()));
        int32 PreparedCursor = 0;
        for (int32 OrderIndex = 0; OrderIndex < Graph.OrderedUnitIndices.Num(); ++OrderIndex) {
            const int32 UnitIndex = Graph.OrderedUnitIndices[OrderIndex];
            const auto& Unit = Workspace.Units[UnitIndex];
            if (Prepared.OrderedUnitKeys[OrderIndex] != Unit.CanonicalUnitKey)
                return Difference(TEXT("The prepared unit order differs from the current workspace dependency order."), TEXT("/preflight/ordered_unit_keys/") + FString::FromInt(OrderIndex), MakeShared<FJsonValueString>(Unit.CanonicalUnitKey), MakeShared<FJsonValueString>(Prepared.OrderedUnitKeys[OrderIndex]));
            if (Unit.Action == EUnitAction::NoChange) continue;
            const FString Path = TEXT("/preflight/units/") + FString::FromInt(PreparedCursor);
            if (!Prepared.Units.IsValidIndex(PreparedCursor))
                return Difference(TEXT("A workspace unit has no prepared mutation."), Path, MakeShared<FJsonValueNumber>(UnitIndex), nullptr);
            const auto& PreparedUnit = Prepared.Units[PreparedCursor];
            if (PreparedUnit.WorkspaceUnitIndex != UnitIndex)
                return Difference(TEXT("The prepared mutation is assigned to a different workspace unit."), Path + TEXT("/workspace_unit_index"), MakeShared<FJsonValueNumber>(UnitIndex), MakeShared<FJsonValueNumber>(PreparedUnit.WorkspaceUnitIndex));
            if (!PreparedUnit.Mutation.IsValid())
                return Difference(TEXT("The prepared unit has no mutation object."), Path + TEXT("/mutation"), MakeShared<FJsonValueString>(TEXT("prepared mutation")), nullptr);
            if (PreparedUnit.Mutation->GetUnitKey() != Unit.CanonicalUnitKey)
                return Difference(TEXT("The prepared mutation's unit identity differs from the workspace unit."), Path + TEXT("/unit_key"), MakeShared<FJsonValueString>(Unit.CanonicalUnitKey), MakeShared<FJsonValueString>(PreparedUnit.Mutation->GetUnitKey()));
            ++PreparedCursor;
        }
        if (PreparedCursor != Prepared.Units.Num())
            return Difference(TEXT("The preflight contains extra prepared mutations."), TEXT("/preflight/units/length"), MakeShared<FJsonValueNumber>(PreparedCursor), MakeShared<FJsonValueNumber>(Prepared.Units.Num()));

		const bool bCallerControlledSave =
			Contract.Execution.Save == TEXT("caller_controlled")
			|| Contract.Execution.Save
				== TEXT("caller_controlled_required_default");
		const bool bRunSave = Contract.Execution.Save == TEXT("required")
			|| (bCallerControlledSave && Options.bSaveWhenCallerControlled);
		FFinalReadbackEvidence ReadbackEvidence;
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
            Failure.Diagnostic = UeAgentAuthoringDiagnostics::FDiagnostic::Failure(TEXT("durable_request_context_incomplete"),
                bHasRequestId ? TEXT("A durable request ID was supplied without a journal storage root.") : TEXT("A journal storage root was supplied without a durable request ID."), TEXT("workspace_durable_preflight"));
            Failure.Diagnostic.SourceKind = TEXT("native_result");
            Failure.Diagnostic.PropertyPath = bHasRequestId ? TEXT("journal_root") : TEXT("request_id");
            Failure.Diagnostic.ExpectedValuePresent = true;
            Failure.Diagnostic.ActualValuePresent = false;
			FailExecution(OutResult, TEXT("preflight"), TEXT("preflight_failed"), Failure);

			return false;
		}
		const bool bHasDurableContext = bHasRequestId && bHasJournalRoot;
		const bool bUseDurableSave = bRunSave && bHasDurableContext && !EffectfulMutations.IsEmpty();
		if (Backend.RequiresPersistentReadback() && !EffectfulMutations.IsEmpty() && !bUseDurableSave)
		{
			FOperationFailure Failure;
            Failure.Diagnostic = UeAgentAuthoringDiagnostics::FDiagnostic::Failure(TEXT("workspace_persistent_readback_requires_durability"),
                !bRunSave ? TEXT("This backend requires persistent readback, but saving is disabled for an effectful transaction.") : TEXT("This backend requires persistent readback, but durable request context is missing."), TEXT("workspace_durable_preflight"));
            Failure.Diagnostic.SourceKind = TEXT("native_result");
            Failure.Diagnostic.PropertyPath = !bRunSave ? TEXT("save_enabled") : TEXT("durable_context");
            Failure.Diagnostic.ExpectedValue = MakeShared<FJsonValueBoolean>(true);
            Failure.Diagnostic.ActualValue = MakeShared<FJsonValueBoolean>(false);
			FailExecution(OutResult, TEXT("preflight"), TEXT("preflight_failed"), Failure);
			return false;
		}
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
		bool bMemoryCheckpointReady = false;

		auto EndRollback = [&](int32 LastMutationIndex)
		{
			OutResult.bRollbackAttempted = true;
			// A native call may allocate owned resources before returning failure.
			// Enlist them before releasing live state or restoring its checkpoint.
			FOperationFailure InventoryFailure;
			const bool bInventoryOk = Durable.RefreshInventory(Backend, OutResult.Durability, InventoryFailure);
			if (!bInventoryOk)
				AppendRollbackFailure(OutResult, InventoryFailure);
			if (Backend.HasReleasedNativeState()
				|| (Backend.RequiresCheckpointRollback() && bMemoryCheckpointReady
					&& OutResult.MutationAttempts > 0))
			{
				FOperationFailure RestoreFailure;
				bool bMemoryOk = Durable.IsPrepared()
					&& Backend.PrepareCheckpointRestore(RestoreFailure)
					&& Durable.PublishMemoryCheckpoint(OutResult.Durability, RestoreFailure)
					&& Backend.ReloadCheckpoint(RestoreFailure);
				if (!bMemoryOk) AppendRollbackFailure(OutResult, RestoreFailure);
				FOperationFailure DiskFailure;
				const bool bDiskOk = Durable.RestoreDiskArtifacts(OutResult.Durability, DiskFailure);
				if (!bDiskOk) AppendRollbackFailure(OutResult, DiskFailure);
				RestoreFailure = FOperationFailure();
				bMemoryOk = bMemoryOk && Backend.VerifyRestoredState(RestoreFailure);
				if (!bMemoryOk && !RestoreFailure.Diagnostic.Code.IsEmpty())
					AppendRollbackFailure(OutResult, RestoreFailure);
				FOperationFailure EndFailure;
				if (bTransactionOpen && !Backend.EndWorkspace(false, EndFailure))
				{
					bMemoryOk = false;
					AppendRollbackFailure(OutResult, EndFailure);
				}
				bTransactionOpen = false;
				FOperationFailure FinalFailure;
				const bool bConfirmed = Durable.FinalizeRestore(
					bInventoryOk && bMemoryOk && bDiskOk, OutResult.Durability, FinalFailure);
				if (!bConfirmed) AppendRollbackFailure(OutResult, FinalFailure);
				OutResult.bRollbackSucceeded = bInventoryOk && bMemoryOk && bDiskOk && bConfirmed;
				OutResult.RollbackAttempts = OutResult.MutationAttempts;
				if (OutResult.bRollbackSucceeded)
					OutResult.RolledBackUnitKeys = OutResult.AttemptedUnitKeys;
				else
				{
					OutResult.Status = TEXT("state_unknown");
					OutResult.Diagnostic.bRetrySafe = false;
				}
				return;
			}
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
					CompleteDiagnostic(RollbackFailure, TEXT("workspace_unit_rollback_failed"), TEXT("workspace_rollback"),
						&Workspace.Units[Unit.WorkspaceUnitIndex]);
					AppendRollbackFailure(OutResult, RollbackFailure);
				}
				else
				{
					OutResult.RolledBackUnitKeys.Add(Unit.Mutation->GetUnitKey());
				}
			}
			FOperationFailure VerifyRollbackFailure;
			if (!Backend.VerifyRestoredState(VerifyRollbackFailure))
			{
				bMemoryRollbackOk = false;
				AppendRollbackFailure(OutResult, VerifyRollbackFailure);
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
					bInventoryOk && bMemoryRollbackOk,
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
			OutResult.bRollbackSucceeded = bInventoryOk && bMemoryRollbackOk && bDurableRollbackOk;
			if (!OutResult.bRollbackSucceeded)
			{
				OutResult.Status = TEXT("state_unknown");
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

		// A native edit can need an identity-preserving rollback even when the
		// caller leaves the result unsaved. Reuse the same before-image journal.
		const bool bUseDurableCheckpoint = bHasDurableContext && Backend.RequiresCheckpointRollback();
		if (bUseDurableSave || bUseDurableCheckpoint)
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
		if (Backend.RequiresCheckpointRollback()
			&& (!Backend.RequiresMemoryCheckpoint() || !(bUseDurableSave || bUseDurableCheckpoint)))
		{
			Failure = FOperationFailure();
            Failure.Diagnostic = UeAgentAuthoringDiagnostics::MakeValueFailure(TEXT("workspace_checkpoint_rollback_unavailable"),
                !Backend.RequiresMemoryCheckpoint() ? TEXT("memory_checkpoint_required") : TEXT("durable_context"),
                MakeShared<FJsonValueBoolean>(true), MakeShared<FJsonValueBoolean>(false), TEXT("workspace_checkpoint"),
                !Backend.RequiresMemoryCheckpoint() ? TEXT("The backend requires checkpoint rollback but does not request a memory checkpoint.") : TEXT("Checkpoint rollback requires durable request context before applying mutations."));
            Failure.Diagnostic.SourceKind = TEXT("native_result");
			FailExecution(OutResult, TEXT("checkpoint"), TEXT("preflight_failed"), Failure);
			EndRollback(INDEX_NONE);
			return false;
		}
		if (Backend.RequiresMemoryCheckpoint() && (bUseDurableSave || bUseDurableCheckpoint))
		{
			Failure = FOperationFailure();
			if (!Durable.CaptureMemoryCheckpoint(Backend, OutResult.Durability, Failure)
				|| !Backend.VerifyRestoredState(Failure))
			{
				CompleteDiagnostic(Failure, TEXT("workspace_memory_checkpoint_failed"), TEXT("workspace_checkpoint"), nullptr);
				FailExecution(OutResult, TEXT("checkpoint"), TEXT("preflight_failed"), Failure);
				EndRollback(INDEX_NONE);
				return false;
			}
			bMemoryCheckpointReady = true;
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
				CompleteDiagnostic(Failure, TEXT("workspace_unit_mutation_failed"), TEXT("workspace_mutation"),
					&Workspace.Units[Unit.WorkspaceUnitIndex]);
				FailExecution(OutResult, TEXT("mutation"), TEXT("mutation_failed"), Failure);
				EndRollback(PreparedIndex);
				return false;
			}
			++OutResult.MutationsApplied;
			Failure = FOperationFailure();
			if (!Durable.RefreshInventory(Backend, OutResult.Durability, Failure))
			{
				FailExecution(OutResult, TEXT("inventory"), TEXT("mutation_failed"), Failure);
				EndRollback(PreparedIndex);
				return false;
			}
		}

		ReadbackEvidence.bMutationPhaseCompleted = true;
		if (Contract.Execution.Compile == TEXT("provider_hook")
			|| Contract.Execution.Compile
				== TEXT("provider_hook_required"))
		{
			ReadbackEvidence.bNativeUpdateRequested = true;
			Failure = FOperationFailure();
			if (!Backend.Compile(Failure))
			{
				CompleteDiagnostic(Failure, TEXT("workspace_compile_failed"), TEXT("workspace_compile"), nullptr);
				FailExecution(OutResult, TEXT("compile"), TEXT("compile_failed"), Failure);
				EndRollback(Prepared.Units.Num() - 1);
				return false;
			}
			ReadbackEvidence.bNativeUpdateCompleted = true;
		}

		Failure = FOperationFailure();
		if (!Durable.RefreshInventory(Backend, OutResult.Durability, Failure))
		{
			FailExecution(OutResult, TEXT("inventory"), TEXT("mutation_failed"), Failure);
			EndRollback(Prepared.Units.Num() - 1);
			return false;
		}
		if (!Backend.Verify(Failure))
		{
			CompleteDiagnostic(Failure, TEXT("workspace_verification_failed"), TEXT("workspace_verification"), nullptr);
			UeAgentAuthoringDiagnostics::ReportConsistencyWarning(Failure.Diagnostic);
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
					UeAgentAuthoringDiagnostics::ReportConsistencyWarning(Failure.Diagnostic);
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

		ReadbackEvidence.bSaveCompleted = bRunSave;
		Failure = FOperationFailure();
		bool bFinalReadbackCompleted = false;
		{
			TGuardValue<const FFinalReadbackEvidence*> Scope(
				ActiveFinalReadbackEvidence, &ReadbackEvidence);
			bFinalReadbackCompleted = Backend.FinalizeVerification(Failure);
		}
		if (!bFinalReadbackCompleted)
		{
			CompleteDiagnostic(Failure, TEXT("workspace_final_readback_failed"), TEXT("workspace_final_readback"), nullptr);
			FailExecution(OutResult, TEXT("final_readback"), TEXT("verification_failed"), Failure);
			EndRollback(Prepared.Units.Num() - 1);
			return false;
		}

		Failure = FOperationFailure();
		if (!Durable.SealConsumerEvidence(Failure))
		{
			CompleteDiagnostic(Failure, TEXT("workspace_consumer_evidence_failed"), TEXT("workspace_final_readback"), nullptr);
			FailExecution(OutResult, TEXT("final_readback"), TEXT("verification_failed"), Failure);
			EndRollback(Prepared.Units.Num() - 1);
			return false;
		}
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
			if (!(bUseDurableSave ? Durable.Commit(OutResult.Durability, Failure)
				: Durable.CompleteMemoryOnly(OutResult.Durability, Failure)))
			{
				CompleteDiagnostic(Failure, TEXT("durable_commit_failed"), TEXT("workspace_durable_commit"), nullptr);
				FailExecution(OutResult, TEXT("commit"), TEXT("transaction_commit_failed"), Failure);
				OutResult.Diagnostic.bRetrySafe = false;
				EndRollback(Prepared.Units.Num() - 1);
				FOperationFailure FinalizationFailure;
				if (!Backend.FinalizeDurableCommit(false, FinalizationFailure))
				{
					AppendRollbackFailure(OutResult, FinalizationFailure);
					OutResult.bRollbackSucceeded = false;
					OutResult.Status = TEXT("state_unknown");
				}
				return false;
			}
			Failure = FOperationFailure();
			if (bUseDurableSave && !Backend.FinalizeDurableCommit(true, Failure))
			{
				CompleteDiagnostic(Failure, TEXT("workspace_durable_finalization_failed"), TEXT("workspace_durable_finalize"), nullptr);
				FailExecution(OutResult, TEXT("commit"), TEXT("transaction_finalization_failed"), Failure);
				OutResult.Status = TEXT("state_unknown");
				OutResult.Diagnostic.bRetrySafe = false;
				return false;
			}
		}
		OutResult.Status = TEXT("applied");
		OutResult.Phase = TEXT("complete");


		return true;
	}
}
