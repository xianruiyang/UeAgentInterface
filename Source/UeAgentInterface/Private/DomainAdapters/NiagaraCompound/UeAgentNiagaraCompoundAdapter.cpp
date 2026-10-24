#include "DomainAdapters/NiagaraCompound/UeAgentNiagaraCompoundAdapter.h"

#include "Core/PlanningPreflight/UeAgentPlanningPreflight.h"
#include "Containers/Set.h"
#include "Dom/JsonValue.h"
#include "Dom/JsonObject.h"
#include "Core/Contract/UeAgentDiagnostic.h"

namespace UE::UAI::DomainAdapters::NiagaraCompound
{
	namespace
	{
		using namespace ContextResource;
		using namespace Discovery;
		using namespace ExecutionMutation;
		using namespace PlanningPreflight;
		using namespace StateSemantic;

		bool Fail(FExecutionMutationError& OutError, UE::UAI::Contract::FDiagnostic Error)
		{
			OutError = MoveTemp(Error);
			return false;
		}

		UE::UAI::Contract::FDiagnostic CompoundFact(const TCHAR* Code, const TCHAR* Reason,
			const FString& Path, TSharedPtr<FJsonValue> Actual, TSharedPtr<FJsonValue> Expected,
			const FString& Target = FString(), const FString& Stage = TEXT("execution"))
		{
			auto Error = UE::UAI::Contract::FDiagnostic::Failure(Code, Reason, Stage);
			Error.SourceKind = TEXT("native_result");
			Error.PropertyPath = Path;
			Error.TargetPath = Target;
			Error.ActualValuePresent = Actual.IsValid();
			Error.ExpectedValuePresent = Expected.IsValid();
			Error.ActualValue = MoveTemp(Actual);
			Error.ExpectedValue = MoveTemp(Expected);
			return Error;
		}


		TSharedPtr<FJsonValue> CompoundValueFact(const FUeAgentCanonicalValue* Value)
		{
			if (!Value) return nullptr;
			switch (Value->GetKind())
			{
			case EUeAgentValueKind::Null: return MakeShared<FJsonValueNull>();
			case EUeAgentValueKind::Boolean: return MakeShared<FJsonValueBoolean>(Value->GetBoolean());
			case EUeAgentValueKind::String: return MakeShared<FJsonValueString>(Value->GetText());
			default: break;
			}
			const TSharedRef<FJsonObject> Shape = MakeShared<FJsonObject>();
			const TCHAR* Kind = TEXT("unknown");
			switch (Value->GetKind())
			{
			case EUeAgentValueKind::Null: Kind = TEXT("null"); break;
			case EUeAgentValueKind::Boolean: Kind = TEXT("boolean"); break;
			case EUeAgentValueKind::String: Kind = TEXT("string"); break;
			case EUeAgentValueKind::Integer: Kind = TEXT("integer"); break;
			case EUeAgentValueKind::Decimal: Kind = TEXT("decimal"); break;
			case EUeAgentValueKind::Array: Kind = TEXT("array"); break;
			case EUeAgentValueKind::Object: Kind = TEXT("object"); break;
			case EUeAgentValueKind::Temporal: Kind = TEXT("temporal"); break;
			case EUeAgentValueKind::Spatial: Kind = TEXT("spatial"); break;
			}
			Shape->SetStringField(TEXT("kind"), Kind);
			if (Value->GetKind() == EUeAgentValueKind::String) Shape->SetBoolField(TEXT("non_empty"), !Value->GetText().TrimStartAndEnd().IsEmpty());
			if (Value->GetKind() == EUeAgentValueKind::Integer || Value->GetKind() == EUeAgentValueKind::Decimal)
				Shape->SetStringField(TEXT("lexeme"), Value->GetLexeme());
			if (Value->GetKind() == EUeAgentValueKind::Array) Shape->SetNumberField(TEXT("count"), Value->GetArrayItems().Num());
			if (Value->GetKind() == EUeAgentValueKind::Object) Shape->SetNumberField(TEXT("count"), Value->GetObjectItems().Num());
			return MakeShared<FJsonValueObject>(Shape);
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
				return Fail(OutError, CompoundFact(TEXT("niagara_compound_field_invalid"),
					TEXT("The Niagara compound input requires a non-empty string field."), Path + TEXT("/") + Name,
					CompoundValueFact(Field ? &Field->Get() : nullptr),
					MakeShared<FJsonValueString>(TEXT("non-empty string"))));
			}
			OutValue = (*Field)->GetText();
			OutValue.TrimStartAndEndInline();
			if (OutValue.IsEmpty())
			{
				return Fail(OutError, CompoundFact(TEXT("niagara_compound_field_invalid"),
					TEXT("The Niagara compound input requires a non-empty string field."), Path + TEXT("/") + Name,
					CompoundValueFact(Field ? &Field->Get() : nullptr),
					MakeShared<FJsonValueString>(TEXT("non-empty string"))));
			}
			return true;
		}

		bool ChargeValue(
			FUeAgentResourceTracker& Resources,
			const int32 FieldCount,
			const FString& Path,
			FExecutionMutationError& OutError)
		{
			FUeAgentResourceChargeError ChargeError;
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
					ChargeError))
			{
				return true;
			}
			OutError = MoveTemp(ChargeError);
			return false;
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
				OutError = MoveTemp(Cancelled);
				if (OutError.Stage.IsEmpty()) OutError.Stage = Stage;
				return false;
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
				OutError = MoveTemp(Cancelled);
				if (OutError.SubjectId.IsEmpty()) OutError.SubjectId = SubjectId;
				if (OutError.Stage.IsEmpty()) OutError.Stage = Stage;
				return false;
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
				return Fail(OutError, CompoundFact(TEXT("niagara_compound_cycle_detected"),
					TEXT("A Niagara dynamic input recursively references an active ancestor."), Frame.Path,
					MakeShared<FJsonValueBoolean>(true), MakeShared<FJsonValueBoolean>(false)));
			}
			if (Frame.Value->GetKind() != EUeAgentValueKind::Object)
			{
				return Fail(OutError, CompoundFact(TEXT("niagara_compound_value_not_object"),
					TEXT("The Niagara compound input must be an object."), Frame.Path,
					CompoundValueFact(&Frame.Value.Get()), MakeShared<FJsonValueString>(TEXT("object"))));
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
				return Fail(OutError, CompoundFact(TEXT("niagara_compound_kind_invalid"),
					TEXT("The compound value kind must identify a Niagara input."), Frame.Path + TEXT("/kind"),
					MakeShared<FJsonValueString>(Kind), MakeShared<FJsonValueString>(TEXT("niagara_input"))));
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
			auto BranchFact = [&]() -> TSharedPtr<FJsonValue>
			{
				TArray<TSharedPtr<FJsonValue>> Branches;
				if (bHasLiteral) Branches.Add(MakeShared<FJsonValueString>(TEXT("literal")));
				if (bHasLinked) Branches.Add(MakeShared<FJsonValueString>(TEXT("linked_parameter")));
				if (bHasDataInterface) Branches.Add(MakeShared<FJsonValueString>(TEXT("data_interface")));
				if (bHasDynamic) Branches.Add(MakeShared<FJsonValueString>(TEXT("dynamic_input")));
				return MakeShared<FJsonValueArray>(Branches);
			};
			if (BranchCount > 1)
			{
				return Fail(OutError, CompoundFact(TEXT("niagara_compound_branch_conflict"),
					TEXT("The Niagara input supplies multiple mutually exclusive value branches."), Frame.Path,
					BranchFact(), MakeShared<FJsonValueString>(TEXT("exactly one of literal|linked_parameter|data_interface|dynamic_input"))));
			}
			if (BranchCount == 0)
			{
				return Fail(OutError, CompoundFact(TEXT("niagara_compound_branch_missing"),
					TEXT("The Niagara input supplies no value branch."), Frame.Path,
					BranchFact(), MakeShared<FJsonValueString>(TEXT("exactly one of literal|linked_parameter|data_interface|dynamic_input"))));
			}

			if (Mode == TEXT("literal"))
			{
				if (!bHasLiteral)
				{
					return Fail(OutError, CompoundFact(TEXT("niagara_compound_mode_branch_mismatch"),
						TEXT("Literal mode requires the literal value branch."), Frame.Path + TEXT("/literal"),
						nullptr, MakeShared<FJsonValueString>(TEXT("present literal value"))));
				}
			}
			else if (Mode == TEXT("linked_parameter"))
			{
				if (!bHasLinked
					|| (*Linked)->GetKind()
						!= EUeAgentValueKind::String
					|| (*Linked)->GetText().TrimStartAndEnd().IsEmpty())
				{
					return Fail(OutError, CompoundFact(TEXT("niagara_compound_mode_branch_mismatch"),
						TEXT("Linked parameter mode requires a non-empty parameter name."), Frame.Path + TEXT("/linked_parameter"),
						CompoundValueFact(Linked ? &Linked->Get() : nullptr), MakeShared<FJsonValueString>(TEXT("non-empty string"))));
				}
			}
			else if (Mode == TEXT("data_interface"))
			{
				if (!bHasDataInterface
					|| (*DataInterface)->GetKind()
						!= EUeAgentValueKind::Object)
				{
					return Fail(OutError, CompoundFact(TEXT("niagara_compound_mode_branch_mismatch"),
						TEXT("Data interface mode requires an object value branch."), Frame.Path + TEXT("/data_interface"),
						CompoundValueFact(DataInterface ? &DataInterface->Get() : nullptr), MakeShared<FJsonValueString>(TEXT("object"))));
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
					if (!DynamicScript || (*DynamicScript)->GetKind() != EUeAgentValueKind::String
						|| (*DynamicScript)->GetText().TrimStartAndEnd().IsEmpty())
						return Fail(OutError, CompoundFact(TEXT("niagara_compound_mode_branch_mismatch"),
							TEXT("Dynamic input mode requires a non-empty script identity."), Frame.Path + TEXT("/dynamic_script_identity"),
							CompoundValueFact(DynamicScript ? &DynamicScript->Get() : nullptr),
							MakeShared<FJsonValueString>(TEXT("non-empty string identity"))));
					return Fail(OutError, CompoundFact(TEXT("niagara_compound_mode_branch_mismatch"),
						TEXT("Dynamic input mode requires an input array."), Frame.Path + TEXT("/inputs"),
						CompoundValueFact(Inputs ? &Inputs->Get() : nullptr), MakeShared<FJsonValueString>(TEXT("array"))));
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
				return Fail(OutError, CompoundFact(TEXT("niagara_compound_mode_invalid"),
					TEXT("The Niagara input mode is unsupported."), Frame.Path + TEXT("/mode"),
					MakeShared<FJsonValueString>(Mode),
					MakeShared<FJsonValueString>(TEXT("literal|linked_parameter|data_interface|dynamic_input"))));
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
		if (!bValid)
		{
			Contract::FDiagnostic Error;
			if (SubjectGeneration < 0)
			{
				Error = CompoundFact(TEXT("niagara_settle_receipt_invalid"),
					TEXT("The Niagara settle receipt has no valid subject generation."), TEXT("/subject_generation/valid"),
					MakeShared<FJsonValueBoolean>(false), MakeShared<FJsonValueBoolean>(true));
				Error.ActualEvidence = MakeShared<FJsonValueString>(FString::Printf(TEXT("%lld"), SubjectGeneration));
			}
			else
			{
				for (const auto& Count : {TPair<const TCHAR*, int32>{TEXT("compile_error_count"), CompileErrorCount},
					TPair<const TCHAR*, int32>{TEXT("stack_issue_count"), StackIssueCount},
					TPair<const TCHAR*, int32>{TEXT("compile_warning_count"), CompileWarningCount},
					TPair<const TCHAR*, int32>{TEXT("stack_warning_count"), StackWarningCount}})
				{
					if (Count.Value >= 0) continue;
					Error = CompoundFact(TEXT("niagara_settle_receipt_invalid"),
						TEXT("A Niagara settle receipt count cannot be negative."), FString(TEXT("/")) + Count.Key,
						MakeShared<FJsonValueNumber>(Count.Value), MakeShared<FJsonValueString>(TEXT("non-negative count")));
					break;
				}
				if (!Error.IsValid())
					Error = CompoundFact(TEXT("niagara_settle_receipt_invalid"),
						TEXT("The Niagara settle receipt has no evidence identity."), TEXT("/evidence_identity/present"),
						MakeShared<FJsonValueBoolean>(false), MakeShared<FJsonValueBoolean>(true));
			}
			ValidationDiagnostic = MakeShared<Contract::FDiagnostic>(MoveTemp(Error));
		}
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

	const Contract::FDiagnostic& FUeAgentNiagaraCompileSettleReceipt::GetValidationDiagnostic() const
	{
		static const Contract::FDiagnostic Empty;
		return ValidationDiagnostic.IsValid() ? *ValidationDiagnostic : Empty;
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
			if (const auto* Validation = Request.GetBinding().GetValidationDiagnostic())
			{
				OutError = *Validation;
				return false;
			}
			return Fail(OutError, CompoundFact(TEXT("niagara_compound_read_request_invalid"),
				TEXT("The Niagara compound read request is not valid."), TEXT("/read_request/valid"),
				MakeShared<FJsonValueBoolean>(false), MakeShared<FJsonValueBoolean>(true), FString(), TEXT("generic_read")));
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
			if (OutError.IsEmpty())
			{
				if (OutCapture.IsValid() && !OutCapture->GetValidationDiagnostic().IsEmpty())
					OutError = GenericRead::FUeAgentGenericReadError::WrapFailure(TEXT("niagara_compound_capture_invalid"), OutCapture->GetValidationDiagnostic());
				else
				{
					OutError = GenericRead::FUeAgentGenericReadError::Failure(TEXT("niagara_compound_capture_invalid"),
						OutCapture.IsValid() ? TEXT("The native Niagara capture must contain exactly one entry.")
							: TEXT("The native Niagara hook returned no capture."), TEXT("generic_read"));
					OutError.SourceKind = TEXT("native_result");
					OutError.PropertyPath = OutCapture.IsValid() ? TEXT("/entries/count") : TEXT("/capture_present");
					if (OutCapture.IsValid())
					{
						OutError.ActualValue = MakeShared<FJsonValueNumber>(OutCapture->GetEntries().Num());
						OutError.ExpectedValue = MakeShared<FJsonValueNumber>(1);
					}
					else
					{
						OutError.ActualValue = MakeShared<FJsonValueBoolean>(false);
						OutError.ExpectedValue = MakeShared<FJsonValueBoolean>(true);
					}
					OutError.ActualValuePresent = true;
					OutError.ExpectedValuePresent = true;
				}
			}
			if (OutError.TargetPath.IsEmpty()) OutError.TargetPath = Request.GetBinding().GetPublicRef();
			if (OutError.SubjectId.IsEmpty()) OutError.SubjectId = SubjectId;
			return false;
		}
		if (!OutCapture->IsSettled())
		{
			return Fail(OutError, CompoundFact(TEXT("niagara_compound_capture_unsettled"),
				TEXT("The native Niagara input capture has not settled."), TEXT("/capture/settled"),
				MakeShared<FJsonValueBoolean>(false), MakeShared<FJsonValueBoolean>(true),
				Request.GetBinding().GetPublicRef(), TEXT("generic_read")));
		}
		if (OutCapture->GetStatePlane() != Request.GetStatePlane())
		{
			auto Error = CompoundFact(TEXT("niagara_compound_state_plane_mismatch"),
				TEXT("The native Niagara capture does not use the requested state plane."), TEXT("/capture/state_plane_matches_requested"),
				MakeShared<FJsonValueBoolean>(false), MakeShared<FJsonValueBoolean>(true),
				Request.GetBinding().GetPublicRef(), TEXT("generic_read"));
			Error.ActualEvidence = MakeShared<FJsonValueNumber>(static_cast<int32>(OutCapture->GetStatePlane()));
			Error.ExpectedEvidence = MakeShared<FJsonValueNumber>(static_cast<int32>(Request.GetStatePlane()));
			return Fail(OutError, MoveTemp(Error));
		}
		const GenericRead::FUeAgentUeReadEntry& Entry =
			OutCapture->GetEntries()[0];
		if (Entry.GetIdentity()
				!= Request.GetBinding().GetSummary().GetIdentity())
		{
			auto Error = CompoundFact(TEXT("niagara_compound_identity_mismatch"),
				TEXT("The native Niagara capture returned a different input identity."), TEXT("/capture/identity_matches_requested"),
				MakeShared<FJsonValueBoolean>(false), MakeShared<FJsonValueBoolean>(true),
				Request.GetBinding().GetPublicRef(), TEXT("generic_read"));
			Error.ActualEvidence = MakeShared<FJsonValueString>(Entry.GetIdentity().GetCanonicalKey());
			Error.ExpectedEvidence = MakeShared<FJsonValueString>(SubjectId);
			return Fail(OutError, MoveTemp(Error));
		}
		FExecutionMutationError ValidationError;
		if (!TryValidateNiagaraCompoundInput(
				Entry.GetValue(),
				Resources,
				ValidationError))
		{
			OutError = MoveTemp(ValidationError);
			if (OutError.TargetPath.IsEmpty()) OutError.TargetPath = Request.GetBinding().GetPublicRef();
			if (OutError.SubjectId.IsEmpty()) OutError.SubjectId = SubjectId;
			return false;
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
			const auto& Validation = !Request.IsValid() ? Request.GetValidationDiagnostic() : Lease.GetValidationDiagnostic();
			return Fail(OutError, Validation.IsValid() ? Validation : CompoundFact(TEXT("niagara_compound_apply_request_invalid"),
				!Request.IsValid() ? TEXT("The Niagara compound apply request is not valid.")
					: TEXT("The Niagara compound apply lease is not valid."),
				!Request.IsValid() ? TEXT("/apply_request/valid") : TEXT("/lease/valid"),
				MakeShared<FJsonValueBoolean>(false), MakeShared<FJsonValueBoolean>(true)));
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
			const auto& Validation = !Request.IsValid() ? Request.GetValidationDiagnostic() : Applied.GetValidationDiagnostic();
			return Fail(OutError, Validation.IsValid() ? Validation : CompoundFact(TEXT("niagara_compound_rollback_request_invalid"),
				!Request.IsValid() ? TEXT("The Niagara compound rollback request is not valid.")
					: TEXT("The Niagara compound rollback has no valid applied result."),
				!Request.IsValid() ? TEXT("/rollback_request/valid") : TEXT("/applied_result/valid"),
				MakeShared<FJsonValueBoolean>(false), MakeShared<FJsonValueBoolean>(true)));
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
			auto Error = CompoundFact(TEXT("niagara_settle_request_invalid"),
				TEXT("The Niagara settle request has no valid subject generation."), TEXT("/subject_generation/valid"),
				MakeShared<FJsonValueBoolean>(false), MakeShared<FJsonValueBoolean>(true));
			Error.ActualEvidence = MakeShared<FJsonValueString>(FString::Printf(TEXT("%lld"), Request.GetSubjectGeneration()));
			return Fail(OutError, MoveTemp(Error));
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
			return Fail(OutError, Receipt.GetValidationDiagnostic().IsValid() ? Receipt.GetValidationDiagnostic()
				: CompoundFact(TEXT("niagara_settle_receipt_invalid"),
					TEXT("The native Niagara hook returned no valid compile and settle receipt."), TEXT("/settle_receipt/valid"),
					MakeShared<FJsonValueBoolean>(false), MakeShared<FJsonValueBoolean>(true)));
		}
		if (Receipt.GetSubjectGeneration()
			!= Request.GetSubjectGeneration())
		{
			auto Error = CompoundFact(TEXT("niagara_settle_generation_mismatch"),
				TEXT("The Niagara settle receipt belongs to a different subject generation."), TEXT("/subject_generation/matches_requested"),
				MakeShared<FJsonValueBoolean>(false), MakeShared<FJsonValueBoolean>(true));
			Error.ActualEvidence = MakeShared<FJsonValueString>(FString::Printf(TEXT("%lld"), Receipt.GetSubjectGeneration()));
			Error.ExpectedEvidence = MakeShared<FJsonValueString>(FString::Printf(TEXT("%lld"), Request.GetSubjectGeneration()));
			return Fail(OutError, MoveTemp(Error));
		}
		if (!Receipt.IsCompilationComplete())
		{
			return Fail(OutError, CompoundFact(TEXT("niagara_compile_incomplete"),
				TEXT("Niagara compilation did not complete."), TEXT("/compilation/complete"), MakeShared<FJsonValueBoolean>(false), MakeShared<FJsonValueBoolean>(true)));
		}
		if (Receipt.GetCompileErrorCount() > 0)
		{
			return Fail(OutError, CompoundFact(TEXT("niagara_compile_error"),
				TEXT("Niagara compilation reported errors."), TEXT("/compilation/error_count"), MakeShared<FJsonValueNumber>(Receipt.GetCompileErrorCount()), MakeShared<FJsonValueNumber>(0)));
		}
		if (Receipt.GetStackIssueCount() > 0)
		{
			return Fail(OutError, CompoundFact(TEXT("niagara_stack_issue"),
				TEXT("The Niagara stack reported blocking issues."), TEXT("/stack/issue_count"), MakeShared<FJsonValueNumber>(Receipt.GetStackIssueCount()), MakeShared<FJsonValueNumber>(0)));
		}
		if (Receipt.GetCompiledTargets().IsEmpty())
		{
			return Fail(OutError, CompoundFact(TEXT("niagara_compile_target_missing"),
				TEXT("The Niagara settle receipt contains no compiled targets."), TEXT("/compiled_targets/count"), MakeShared<FJsonValueNumber>(0), MakeShared<FJsonValueString>(TEXT("at least one compiled target"))));
		}
		if (!ContainsAll(
				Receipt.GetObservedSignals(),
				Request.GetRequiredSignals()))
		{
			TArray<TSharedPtr<FJsonValue>> Actual, Expected;
			for (const FString& Signal : Receipt.GetObservedSignals()) Actual.Add(MakeShared<FJsonValueString>(Signal));
			for (const FString& Signal : Request.GetRequiredSignals()) Expected.Add(MakeShared<FJsonValueString>(Signal));
			return Fail(OutError, CompoundFact(TEXT("niagara_settle_signal_missing"),
				TEXT("The Niagara settle receipt is missing one or more required signals."), TEXT("/observed_signals"),
				MakeShared<FJsonValueArray>(Actual), MakeShared<FJsonValueArray>(Expected)));
		}
		if (Receipt.GetPostBarrierRevision().IsEmpty())
		{
			return Fail(OutError, CompoundFact(TEXT("niagara_post_barrier_revision_missing"),
				TEXT("The Niagara settle receipt has no post-barrier revision."), TEXT("/post_barrier_revision/present"), MakeShared<FJsonValueBoolean>(false), MakeShared<FJsonValueBoolean>(true)));
		}
		OutResult = FBarrierResult(
			Receipt.GetSubjectGeneration(),
			Receipt.GetObservedSignals(),
			Receipt.GetPostBarrierRevision(),
			true);
		return true;
	}
}
