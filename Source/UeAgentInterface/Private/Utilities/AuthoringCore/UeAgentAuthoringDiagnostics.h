#pragma once

#include "Core/Contract/UeAgentDiagnostic.h"

class FJsonObject;
class FJsonValue;
class FCompilerResultsLog;
class FTokenizedMessage;
class UObject;

namespace UeAgentAuthoringDiagnostics
{
	using UE::UAI::Contract::ESeverity;
	using UE::UAI::Contract::FSourceLocation;
	using UE::UAI::Contract::FDiagnostic;

	UEAGENTINTERFACE_API void LocateAuthoringSource(FDiagnostic& Diagnostic, const FJsonObject& Object, const FString& Field = FString());
	UEAGENTINTERFACE_API void LocatePropertyValueSource(FDiagnostic& Diagnostic, const FJsonObject& Descriptor);
	UEAGENTINTERFACE_API void MarkUnchecked(FDiagnostic& Diagnostic, const FString& Stage, const FJsonObject& Object, const FString& Reason);
	UEAGENTINTERFACE_API void LocateFailure(FDiagnostic& Diagnostic, const FJsonObject& Object,
		const FString& Stage, const FString& TargetPath = FString(), const FString& Field = FString());
	// Opt in only when command input paths are relative to the supplied subobject.
	UEAGENTINTERFACE_API void PrefixDocumentFailure(FDiagnostic& Diagnostic, const FString& Prefix,
		bool bAppendPropertyPath = false, bool bRelativeCommandInput = false);
	// Canonical document keys can be native identities; qualify them only in the source map.
	UEAGENTINTERFACE_API void PrefixInternalDocumentFailure(FDiagnostic& Diagnostic, const FString& Prefix);
	UEAGENTINTERFACE_API void SetRollbackFacts(FDiagnostic& Diagnostic, bool bSucceeded);

	// Bind the parser's existing source table while a provider inspects its
	// immutable document. This sidecar never inserts metadata into business values.
	class UEAGENTINTERFACE_API FScopedSourceLocations final
	{
	public:
		FScopedSourceLocations(const FJsonObject* Root, TFunction<FSourceLocation(const FString&)> Resolve);
		// Command parameters have real JSON pointers without a physical source file.
		FScopedSourceLocations(const FJsonObject* Root, TFunction<FSourceLocation(const FString&)> Resolve,
			bool bCommandInput);
		~FScopedSourceLocations();
		FScopedSourceLocations(const FScopedSourceLocations&) = delete;
		FScopedSourceLocations& operator=(const FScopedSourceLocations&) = delete;
		static bool Locate(const FJsonObject& Object, const FString& Field, FSourceLocation& Out,
			const FString& RelativePointer = FString());
		// Snapshot actual shared value identities before a generated document is reparsed.
		// The result owns only source values, never a scoped resolver or parser pointer.
		static TMap<FString, FSourceLocation> CaptureProjection(const FJsonObject& Object);
		// Resolve a preserved projection map without reconstructing a diagnostic.
		// An absent leaf resolves to its actual containing node and keeps that node's path.
		static FSourceLocation ResolveProjection(const TMap<FString, FSourceLocation>& Sources, FString Pointer);
	private:
		FScopedSourceLocations* Parent = nullptr;
		const FJsonObject* Root = nullptr;
		bool bCommandInput = false;
		TFunction<FSourceLocation(const FString&)> Resolve;
	};

	// One synchronous command owns its observations, including nested dispatches.
	// This collector does not change execution status or perform recovery/writes.
	class UEAGENTINTERFACE_API FScopedWarnings final
	{
	public:
		explicit FScopedWarnings(bool bForwardToParent = true);
		~FScopedWarnings();
		FScopedWarnings(const FScopedWarnings&) = delete;
		FScopedWarnings& operator=(const FScopedWarnings&) = delete;
		FString AppendToPayload(const FString& Payload) const;
		void Add(const FDiagnostic& Diagnostic);
		const TArray<FDiagnostic>& GetWarnings() const { return Warnings; }
		bool HasWarnings() const { return !Warnings.IsEmpty() || (Parent && bForward && Parent->HasWarnings()); }
	private:
		FScopedWarnings* Parent = nullptr;
		bool bForward = true;
		TArray<FDiagnostic> Warnings;
	};

	// Call only for observations after an operation, never for input validation
	// or a failed native operation. Pure comparison functions retain their result.
	UEAGENTINTERFACE_API void ReportConsistencyWarning(const FDiagnostic& Diagnostic);
	UEAGENTINTERFACE_API void ReportConsistencyWarning(const FString& FallbackCode,
		FDiagnostic Diagnostic, const FString& Stage);
	UEAGENTINTERFACE_API bool HasConsistencyWarnings();





	// Facts supplied by the owner of an authored field, not inferred from private prose.
	UEAGENTINTERFACE_API FDiagnostic MakeValueFailure(const FString& Code,
		const FString& Path, const TSharedPtr<FJsonValue>& Expected,
		const TSharedPtr<FJsonValue>& Actual, const FString& Stage = TEXT("input_decode"),
		const FString& PublicReason = TEXT("The authored value does not satisfy the required type or value constraint."));
	// Combine already-failed diagnostic collections without discarding later
	// causes. The caller remains responsible for deciding whether apply failed.
	UEAGENTINTERFACE_API FDiagnostic MakeIssuesFailure(const FString& FallbackCode,
		const TArray<FDiagnostic>& Issues, const FString& Stage,
		const FString& TargetPath = FString());


	// Add the current public selection context, preserving readable ambiguity choices.
	UEAGENTINTERFACE_API FDiagnostic CandidateFailure(const FString& Code, const FString& Name,
		const FString& TargetPath, const FString& Context, const FString& QueryCommand,
		const FDiagnostic& Diagnostic = FDiagnostic());


	UEAGENTINTERFACE_API FString SeverityToString(ESeverity Severity);
	// Preserve concrete errors from an existing native compile pass in the
	// authoring failure carrier; this does not perform another compilation.
	UEAGENTINTERFACE_API FDiagnostic MakeCompileFailure(const FString& Code,
		const FString& TargetPath, const TArray<TSharedPtr<FJsonValue>>& Messages);
	UEAGENTINTERFACE_API FDiagnostic MakeCompileFailure(const FString& Code,
		const FString& TargetPath, const FCompilerResultsLog& CompilerLog,
		const UObject* CompileTarget = nullptr);
	// UE tokens already resolve intermediate nodes and macro expansions to source
	// objects. Preserve their document identities for the existing workspace mapper.
	UEAGENTINTERFACE_API void AppendCompilerMessageSources(const FTokenizedMessage& Message,
		const UObject* CompileTarget, FJsonObject& OutMessage);

}
