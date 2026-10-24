#include "Domains/PCG/UeAgentPcgGraphCompilation.h"

#include "Dom/JsonValue.h"
#include "HAL/PlatformTLS.h"
#include "Misc/OutputDevice.h"
#include "Misc/OutputDeviceRedirector.h"
#include "PCGGraph.h"
#include "Utilities/AuthoringCore/UeAgentAuthoringDiagnostics.h"

namespace UE::UAI::PcgCompilation
{
	namespace
	{
		class FCompilerFeedback final : public FOutputDevice
		{
		public:
			FCompilerFeedback() : ThreadId(FPlatformTLS::GetCurrentThreadId())
			{ if (GLog) GLog->AddOutputDevice(this); }
			~FCompilerFeedback() override
			{ if (GLog) GLog->RemoveOutputDevice(this); }
			bool CanBeUsedOnAnyThread() const override { return true; }
			bool CanBeUsedOnMultipleThreads() const override { return true; }
			void Serialize(const TCHAR* Text, ELogVerbosity::Type Verbosity, const FName& Category) override
			{
				if (FPlatformTLS::GetCurrentThreadId() != ThreadId || Category != TEXT("LogPCG")) return;
				if (Verbosity <= ELogVerbosity::Error) Errors.AddUnique(Text);
				else if (Verbosity == ELogVerbosity::Warning) Warnings.AddUnique(Text);
			}
			TArray<FString> Errors;
			TArray<FString> Warnings;
		private:
			uint32 ThreadId;
		};
	}

	bool Compile(UPCGGraph& Graph, Contract::FDiagnostic& OutError)
	{
		OutError.Reset();
		FCompilerFeedback Feedback;
		Graph.Recompile();
		const bool bCompilerAvailable = Graph.PrimeGraphCompilationCache();
		auto NativeDiagnostic = [&Graph](const TCHAR* Code, const FString& Reason)
		{
			auto Diagnostic = Contract::FDiagnostic::Failure(Code, Reason, TEXT("compile"));
			Diagnostic.SourceKind = TEXT("native_result");
			Diagnostic.TargetPath = Graph.GetPathName();
			return Diagnostic;
		};
		if (!bCompilerAvailable)
		{
			OutError = NativeDiagnostic(TEXT("pcg_compiler_unavailable"), TEXT("The active Editor PCG subsystem has no graph compiler."));
			OutError.PropertyPath = TEXT("compiler_available");
			OutError.ActualValue = MakeShared<FJsonValueBoolean>(false);
			OutError.ExpectedValue = MakeShared<FJsonValueBoolean>(true);
		}
		for (const FString& Message : Feedback.Errors)
			OutError.Append(NativeDiagnostic(TEXT("pcg_native_compile_error"), Message));
		for (const FString& Message : Feedback.Warnings)
		{
			auto Warning = NativeDiagnostic(TEXT("pcg_native_compile_warning"), Message);
			Warning.Severity = Contract::ESeverity::Warning;
			if (OutError.IsEmpty()) UeAgentAuthoringDiagnostics::ReportConsistencyWarning(Warning);
			else OutError.Diagnostics.Add(MoveTemp(Warning));
		}
		return OutError.IsEmpty();
	}
}
