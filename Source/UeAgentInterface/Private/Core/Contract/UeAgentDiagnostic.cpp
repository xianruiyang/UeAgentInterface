#include "Core/Contract/UeAgentDiagnostic.h"

#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Serialization/JsonSerializer.h"
#include "Serialization/JsonWriter.h"

namespace UE::UAI::Contract
{
TArray<TSharedPtr<FJsonValue>> ToPublicDiagnostics(const TArray<FDiagnostic>& Diagnostics, bool bDeduplicate)
{
    TArray<TSharedPtr<FJsonValue>> Result;
    TSet<FString> Seen;
    Result.Reserve(Diagnostics.Num());
    for (const FDiagnostic& Diagnostic : Diagnostics)
    {
        const auto Object = Diagnostic.ToPublicJson();
        if (bDeduplicate)
        {
            FString Key;
            FJsonSerializer::Serialize(Object, TJsonWriterFactory<>::Create(&Key));
            if (Seen.Contains(Key)) continue;
            Seen.Add(MoveTemp(Key));
        }
        Result.Add(MakeShared<FJsonValueObject>(Object));
    }
    return Result;
}

namespace
{

FString SeverityToString(ESeverity Severity)
{
    return Severity == ESeverity::Warning ? TEXT("warning")
        : Severity == ESeverity::Info ? TEXT("info") : TEXT("error");
}
}

FDiagnostic FDiagnostic::Failure(FString InCode, FString PublicReason, FString InStage, FString InInternalContext, FString InSubjectId)
{
    FDiagnostic Result;
    Result.Code = MoveTemp(InCode);
    Result.PublicMessage = MoveTemp(PublicReason);
    Result.Stage = MoveTemp(InStage);
    Result.InternalContext = MoveTemp(InInternalContext);
    Result.SubjectId = MoveTemp(InSubjectId);
    return Result;
}

FDiagnostic FDiagnostic::WrapFailure(FString InCode, const FDiagnostic& Cause, FString InStage, FString PublicReason)
{
    FDiagnostic Result = Cause;
    if (!InCode.IsEmpty() && InCode != Cause.Code)
    {
        Result.Code = MoveTemp(InCode);
        if (Cause.IsValid())
        {
            Result.CauseCode = Cause.Code;
            Result.PublicCause = MakeShared<FDiagnostic>(Cause);
        }
    }
    if (Result.Stage.IsEmpty()) Result.Stage = MoveTemp(InStage);
    if (!PublicReason.IsEmpty()) Result.PublicMessage = MoveTemp(PublicReason);
    return Result;
}

void FDiagnostic::Append(const FDiagnostic& Failure)
{
    if (Failure.IsEmpty()) return;
    if (IsEmpty())
    {
        auto PriorUnchecked = MoveTemp(Unchecked);
        *this = Failure;
        Unchecked.Append(PriorUnchecked);
        return;
    }
    // A nested cause keeps its own code, source, public reason and recovery facts.
    // Flattening its children would discard the parent that explains those facts.
    Diagnostics.Add(Failure);
    bRetrySafe = bRetrySafe && Failure.bRetrySafe;
}

void FPublicDiagnostic::AppendDifferenceValues(FJsonObject& Object, bool bPublic) const
{
    // The producer already selected public values; serialization preserves every business key.
    if (ExpectedValue) Object.SetField(TEXT("expected_value"), ExpectedValue);
    if (ActualValue) Object.SetField(TEXT("actual_value"), ActualValue);
    if (ProposedValue) Object.SetField(TEXT("proposed_value"), ProposedValue);
    if (ExpectedValuePresent.IsSet()) Object.SetBoolField(TEXT("expected_value_present"), ExpectedValuePresent.GetValue());
    if (ActualValuePresent.IsSet()) Object.SetBoolField(TEXT("actual_value_present"), ActualValuePresent.GetValue());
}

void FPublicDiagnostic::AppendFacts(FJsonObject& Object, bool bPublic) const
	{
		if (!TransactionStatus.IsEmpty()) Object.SetStringField(TEXT("transaction_status"), TransactionStatus);
		if (RollbackAttempted.IsSet()) Object.SetBoolField(TEXT("rollback_attempted"), RollbackAttempted.GetValue());
		if (RollbackSucceeded.IsSet()) Object.SetBoolField(TEXT("rollback_succeeded"), RollbackSucceeded.GetValue());
		if (!CauseCode.IsEmpty()) Object.SetStringField(TEXT("cause_code"), CauseCode);
		if (PublicCause.IsValid()) Object.SetObjectField(TEXT("cause"), bPublic ? PublicCause->ToPublicJson() : PublicCause->ToJson());
		if (!RecoveryAction.IsEmpty()) Object.SetStringField(TEXT("recovery_action"), RecoveryAction);
        if (!ExecutionStatus.IsEmpty()) Object.SetStringField(TEXT("execution_status"), ExecutionStatus);
        if (!VerificationStatus.IsEmpty()) Object.SetStringField(TEXT("verification_status"), VerificationStatus);
        if (!EffectOutcome.IsEmpty()) Object.SetStringField(TEXT("effect_outcome"), EffectOutcome);
        if (!TerminalStatus.IsEmpty()) Object.SetStringField(TEXT("terminal_status"), TerminalStatus);
        if (MutationStarted.IsSet()) Object.SetBoolField(TEXT("mutation_started"), MutationStarted.GetValue());
        if (MutationMayHaveStarted.IsSet()) Object.SetBoolField(TEXT("mutation_may_have_started"), MutationMayHaveStarted.GetValue());
        if (Verification)
        {
            Object.SetObjectField(TEXT("verification"), Verification);
        }
		if (AssetApplied.IsSet()) Object.SetBoolField(TEXT("asset_applied"), AssetApplied.GetValue());
		if (RetryImport.IsSet()) Object.SetBoolField(TEXT("retry_import"), RetryImport.GetValue());
		if (OperationIndex.IsSet()) Object.SetNumberField(TEXT("operation"), OperationIndex.GetValue());
		AppendDifferenceValues(Object, bPublic);
		if (!SemanticPointer.IsEmpty() && SemanticPointer != TEXT("/")) Object.SetStringField(TEXT("semantic_pointer"), SemanticPointer);
		if (!SourceKind.IsEmpty()) Object.SetStringField(TEXT("source_kind"), SourceKind);
        if (!DialectId.IsEmpty()) Object.SetStringField(TEXT("dialect"), DialectId);
        if (!SchemaPath.IsEmpty()) Object.SetStringField(TEXT("schema_path"), SchemaPath);
        if (!Keyword.IsEmpty()) Object.SetStringField(TEXT("keyword"), Keyword);
        if (SchemaExpected) Object.SetField(TEXT("expected"), SchemaExpected);
        if (!ResourceDimension.IsEmpty()) Object.SetStringField(TEXT("resource_dimension"), ResourceDimension);
        if (ResourceObserved.IsSet()) Object.SetField(TEXT("resource_observed"), MakeShared<FJsonValueNumberString>(LexToString(ResourceObserved.GetValue())));
        if (StoppedEarly.IsSet()) Object.SetBoolField(TEXT("stopped_early"), StoppedEarly.GetValue());
		if (!RelatedSources.IsEmpty()) Object.SetArrayField(TEXT("related_sources"), RelatedSources);
	}

TSharedRef<FJsonObject> FDiagnostic::ToJson() const
	{
		TSharedRef<FJsonObject> Object = MakeShared<FJsonObject>();
		Object->SetStringField(TEXT("schema"), Schema);
		Object->SetStringField(TEXT("code"), Code);
		if (!SubjectId.IsEmpty()) Object->SetStringField(TEXT("subject_id"), SubjectId);
		if (static_cast<uint8>(Facet) != 0) Object->SetNumberField(TEXT("context_facet"), static_cast<uint8>(Facet));
		if (CancellationRequestedAt.IsSet()) Object->SetNumberField(TEXT("cancellation_requested_at"), CancellationRequestedAt.GetValue());
		Object->SetStringField(TEXT("stage"), Stage);
		Object->SetStringField(TEXT("severity"), SeverityToString(Severity));
		Object->SetBoolField(TEXT("retry_safe"), bRetrySafe);
		if (!CandidateName.IsEmpty()) Object->SetStringField(TEXT("name"), CandidateName);
		if (CandidateChoices.IsValid()) Object->SetField(TEXT("choose"), CandidateChoices);
		if (!Diagnostics.IsEmpty())
		{
			TArray<TSharedPtr<FJsonValue>> Items;
			for (const auto& Child : Diagnostics) Items.Add(MakeShared<FJsonValueObject>(Child.ToJson()));
			Object->SetArrayField(TEXT("diagnostics"), Items);
		}
		if (!Unchecked.IsEmpty()) Object->SetArrayField(TEXT("unchecked"), Unchecked);
		Object->SetStringField(TEXT("message"), PublicMessage);
		if (!InternalContext.IsEmpty()) Object->SetStringField(TEXT("internal_context"), InternalContext);
		if (!SuggestedFix.IsEmpty())
		{
			Object->SetStringField(TEXT("suggested_fix"), SuggestedFix);
		}
		if (!TargetPath.IsEmpty())
		{
			Object->SetStringField(TEXT("target_path"), TargetPath);
		}
		if (!PropertyPath.IsEmpty())
		{
			Object->SetStringField(TEXT("property_path"), PropertyPath);
		}
		if (!SemanticId.IsEmpty())
		{
			Object->SetStringField(TEXT("semantic_id"), SemanticId);
		}

		TSharedRef<FJsonObject> SourceObject = MakeShared<FJsonObject>();
		SourceObject->SetStringField(TEXT("workspace_source"), Source.WorkspaceSource);
		SourceObject->SetStringField(TEXT("source_file"), Source.SourceFile);
		SourceObject->SetStringField(TEXT("source_unit"), Source.SourceUnit);
		SourceObject->SetStringField(TEXT("json_path"), Source.JsonPath);
		SourceObject->SetStringField(TEXT("internal_json_path"), Source.InternalJsonPath);
		SourceObject->SetNumberField(TEXT("line"), Source.Line);
		SourceObject->SetNumberField(TEXT("column"), Source.Column);
		Object->SetObjectField(TEXT("source"), SourceObject);
		AppendFacts(*Object, false);
		if (ExpectedEvidence) Object->SetField(TEXT("expected_evidence"), ExpectedEvidence);
		if (ActualEvidence) Object->SetField(TEXT("actual_evidence"), ActualEvidence);
		if (!CyclePath.IsEmpty())
		{
			TArray<TSharedPtr<FJsonValue>> Paths;
			for (const FString& Path : CyclePath) Paths.Add(MakeShared<FJsonValueString>(Path));
			Object->SetArrayField(TEXT("cycle_path"), MoveTemp(Paths));
		}
		return Object;
	}

TSharedRef<FJsonObject> FPublicDiagnostic::ToPublicJson(const FPublicSourceLocation& Source) const
	{
		TSharedRef<FJsonObject> Object = MakeShared<FJsonObject>();
		Object->SetStringField(TEXT("schema"), Schema);
		Object->SetStringField(TEXT("severity"), SeverityToString(Severity));
		Object->SetStringField(TEXT("code"), Code);
		Object->SetStringField(TEXT("stage"), Stage);
		Object->SetBoolField(TEXT("retry_safe"), bRetrySafe);
		if (!CandidateName.IsEmpty()) Object->SetStringField(TEXT("name"), CandidateName);
		if (CandidateChoices.IsValid()) Object->SetField(TEXT("choose"), CandidateChoices);
		if (!TargetPath.IsEmpty()) Object->SetStringField(TEXT("target"), TargetPath);
		if (!PropertyPath.IsEmpty()) Object->SetStringField(TEXT("property_path"), PropertyPath);
		if (!Source.SourceFile.IsEmpty())
		{
			Object->SetStringField(TEXT("source_file"), Source.SourceFile);
		}
		if (!Source.JsonPath.IsEmpty()) Object->SetStringField(TEXT("json_pointer"), Source.JsonPath);
		if (Source.Line > 0) Object->SetNumberField(TEXT("line"), Source.Line);
		if (Source.Column > 0) Object->SetNumberField(TEXT("column"), Source.Column);
		Object->SetStringField(TEXT("message"), PublicMessage);
		if (!SuggestedFix.IsEmpty()) Object->SetStringField(TEXT("suggested_fix"), SuggestedFix);
		if (!Diagnostics.IsEmpty()) Object->SetArrayField(TEXT("diagnostics"), ToPublicDiagnostics(Diagnostics));
		if (!Unchecked.IsEmpty()) Object->SetArrayField(TEXT("unchecked"), Unchecked);
		AppendFacts(*Object, true);
		return Object;
	}

FString FDiagnostic::ToPublicDetail() const
	{
		const auto Object = ToPublicJson();
		FString Detail;
		FJsonSerializer::Serialize(Object, TJsonWriterFactory<>::Create(&Detail));
		return Detail;
	}
}
