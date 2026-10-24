#pragma once

#include "CoreMinimal.h"

class FJsonObject;
class FJsonValue;

namespace UE::UAI::Contract
{
	enum class EUeAgentExecutionContextFacet : uint8;

	enum class ESeverity : uint8
	{
		Info,
		Warning,
		Error,
	};

	struct UEAGENTINTERFACE_API FPublicSourceLocation
	{
		FString SourceFile;
		FString JsonPath;
		int32 Line = 0;
		int32 Column = 0;
	};

	struct UEAGENTINTERFACE_API FInternalSourceLocation
	{
		FString WorkspaceSource;
		FString SourceUnit;
		FString InternalJsonPath;
		// Preserved with each source-map entry when command input has no physical file.
		bool bCommandInput = false;
	};

	struct UEAGENTINTERFACE_API FSourceLocation : FPublicSourceLocation, FInternalSourceLocation {};
	struct FDiagnostic;

	// Producer-selected public facts. Values are business data, never machine evidence.
	// Separate base types preserve direct producer access without maintaining copies.
	struct UEAGENTINTERFACE_API FPublicDiagnostic
	{
		FString Schema = TEXT("uai.authoring.diagnostic/1");
		FString Code;
		FString Stage;
		ESeverity Severity = ESeverity::Error;
		bool bRetrySafe = false;
		FString SuggestedFix;
		FString TargetPath;
		FString PropertyPath;
		FString PublicMessage;
		TSharedPtr<const FDiagnostic> PublicCause;
		FString CandidateName;
		TSharedPtr<FJsonValue> CandidateChoices;
		// Optional facts: absence means the producer did not report the outcome.
		FString TransactionStatus;
		TOptional<bool> RollbackAttempted;
		TOptional<bool> RollbackSucceeded;
		FString CauseCode;
		FString RecoveryAction;
		FString ExecutionStatus;
		FString VerificationStatus;
		FString EffectOutcome;
		FString TerminalStatus;
		TOptional<bool> MutationStarted;
		TOptional<bool> MutationMayHaveStarted;
		TSharedPtr<FJsonObject> Verification;
		TOptional<bool> AssetApplied;
		TOptional<bool> RetryImport;
		TSharedPtr<FJsonValue> ExpectedValue;
		TSharedPtr<FJsonValue> ActualValue;
		TSharedPtr<FJsonValue> ProposedValue;
		TOptional<bool> ExpectedValuePresent;
		TOptional<bool> ActualValuePresent;
		TOptional<int32> OperationIndex;
		FString SemanticPointer;
		FString SourceKind;
		FString DialectId;
		FString SchemaPath;
		FString Keyword;
		TSharedPtr<FJsonValue> SchemaExpected;
		FString ResourceDimension;
		TOptional<int64> ResourceObserved;
		TOptional<bool> StoppedEarly;
		TArray<TSharedPtr<FJsonValue>> RelatedSources;
		TArray<FDiagnostic> Diagnostics;
		TArray<TSharedPtr<FJsonValue>> Unchecked;

		TSharedRef<FJsonObject> ToPublicJson(const FPublicSourceLocation& Source) const;
		void AppendDifferenceValues(FJsonObject& Object, bool bPublic) const;
	protected:
		void AppendFacts(FJsonObject& Object, bool bPublic) const;
	};

	struct UEAGENTINTERFACE_API FInternalDiagnostic
	{
		FString InternalContext;
		FString SemanticId;
		FString SubjectId;
		EUeAgentExecutionContextFacet Facet{};
		TOptional<double> CancellationRequestedAt;
		TArray<FString> CyclePath;
		TSharedPtr<FJsonValue> ExpectedEvidence;
		TSharedPtr<FJsonValue> ActualEvidence;
	};

	struct UEAGENTINTERFACE_API FDiagnostic : FPublicDiagnostic, FInternalDiagnostic
	{
		FDiagnostic() = default;
		FSourceLocation Source;
		// Construct an explicit public reason at its producer; internal context stays separate.
		// Consumers retain the diagnostic or wrap its complete cause without decoding strings.
		static FDiagnostic Failure(FString Code, FString PublicReason, FString Stage = FString(), FString InternalContext = FString(), FString SubjectId = FString());
		// Add a distinct outer classification while retaining the complete cause and its facts.
		static FDiagnostic WrapFailure(FString Code, const FDiagnostic& Cause, FString Stage = FString(), FString PublicReason = FString());
		bool IsEmpty() const { return Code.IsEmpty(); }
		bool IsValid() const { return !Code.IsEmpty(); }
		void Reset() { *this = FDiagnostic(); }

		// Collect independent failures without replacing the first cause or its source.
		void Append(const FDiagnostic& Failure);

		TSharedRef<FJsonObject> ToJson() const;
		// One-way public projection; internal consumers retain the typed diagnostic.
		TSharedRef<FJsonObject> ToPublicJson() const { return FPublicDiagnostic::ToPublicJson(Source); }
		FString ToPublicDetail() const;
	};

	UEAGENTINTERFACE_API TArray<TSharedPtr<FJsonValue>> ToPublicDiagnostics(
		const TArray<FDiagnostic>& Diagnostics, bool bDeduplicate = false);
}
