// Copyright Epic Games, Inc. All Rights Reserved.

#include "Domains/Niagara/UeAgentNiagaraCompileValidation.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"

#include "NiagaraEmitter.h"
#include "NiagaraEmitterHandle.h"
#include "NiagaraScript.h"
#include "NiagaraSystem.h"

namespace UeAgentNiagaraCompileValidation
{
	namespace
	{
		void AppendUniqueScript(UNiagaraScript* Script, TArray<UNiagaraScript*>& OutScripts, TSet<const UNiagaraScript*>& Seen)
		{
			if (Script && !Seen.Contains(Script))
			{
				Seen.Add(Script);
				OutScripts.Add(Script);
			}
		}

		void AppendEmitterScripts(
			const FVersionedNiagaraEmitterData* EmitterData,
			TArray<UNiagaraScript*>& OutScripts,
			TSet<const UNiagaraScript*>& Seen,
			const bool bCompilableOnly)
		{
			if (!EmitterData)
			{
				return;
			}

			TArray<UNiagaraScript*> EmitterScripts;
			// Match the native editor's compile-status scope for diagnostic reads.
			// Auxiliary emitter/stage scripts do not have independent VM freshness.
			EmitterData->GetScripts(EmitterScripts, bCompilableOnly, bCompilableOnly);
			for (UNiagaraScript* Script : EmitterScripts)
			{
				AppendUniqueScript(Script, OutScripts, Seen);
			}
		}
	}

	bool HasScriptCompileError(
		const ENiagaraScriptCompileStatus CompileStatus, const FString& ErrorMessage,
		const TArray<FNiagaraCompileEvent>& CompileEvents)
	{
		return CompileStatus == ENiagaraScriptCompileStatus::NCS_Error || !ErrorMessage.IsEmpty()
			|| CompileEvents.ContainsByPredicate([](const FNiagaraCompileEvent& Event) {
				return Event.Severity == FNiagaraCompileEventSeverity::Error;
			});
	}

	FDiagnostic FResult::GetFailure(const FString& TargetPath) const
	{
		if (!bHasError && bCompilationComplete) return FDiagnostic();
		auto Failure = FDiagnostic::Failure(bCompilationComplete ? TEXT("niagara_compile_failed") : TEXT("niagara_compile_incomplete"),
			bCompilationComplete ? TEXT("Niagara compilation reported errors.") : TEXT("Niagara compilation did not complete."), TEXT("compile"));
		Failure.TargetPath = TargetPath;
		Failure.SourceKind = TEXT("native_result");
		Failure.PropertyPath = bCompilationComplete ? TEXT("error_script_count") : TEXT("compilation_complete");
		Failure.ActualValue = bCompilationComplete ? TSharedPtr<FJsonValue>(MakeShared<FJsonValueNumber>(ErrorScriptCount))
			: TSharedPtr<FJsonValue>(MakeShared<FJsonValueBoolean>(false));
		Failure.ExpectedValue = bCompilationComplete ? TSharedPtr<FJsonValue>(MakeShared<FJsonValueNumber>(0))
			: TSharedPtr<FJsonValue>(MakeShared<FJsonValueBoolean>(true));
		for (const auto& Diagnostic : Diagnostics)
			if (Diagnostic.Severity == UE::UAI::Contract::ESeverity::Error) Failure.Diagnostics.Add(Diagnostic);
		return Failure;
	}

	static FDiagnostic MakeCompileEvent(const FString& ScriptPath, bool bError,
		const FString& Message, const FNiagaraCompileEvent* Event = nullptr)
	{
		auto Diagnostic = FDiagnostic::Failure(bError ? TEXT("niagara_compile_error") : TEXT("niagara_compile_warning"),
			Message.IsEmpty() ? (bError ? TEXT("Unreal reported a Niagara compile error without a message.")
				: TEXT("Unreal reported a Niagara compile warning without a message.")) : Message, TEXT("compile"));
		Diagnostic.Severity = bError ? UE::UAI::Contract::ESeverity::Error : UE::UAI::Contract::ESeverity::Warning;
		Diagnostic.TargetPath = ScriptPath;
		Diagnostic.SourceKind = TEXT("native_compile_event");
		auto Source = MakeShared<FJsonObject>();
		Source->SetStringField(TEXT("source_kind"), Diagnostic.SourceKind);
		Source->SetStringField(TEXT("script_path"), ScriptPath);
		if (Event)
		{
			if (Event->NodeGuid.IsValid()) Source->SetStringField(TEXT("node_guid"), Event->NodeGuid.ToString());
			if (Event->PinGuid.IsValid()) Source->SetStringField(TEXT("pin_guid"), Event->PinGuid.ToString());
			TArray<TSharedPtr<FJsonValue>> Stack;
			for (const FGuid& Guid : Event->StackGuids) Stack.Add(MakeShared<FJsonValueString>(Guid.ToString()));
			if (!Stack.IsEmpty()) Source->SetArrayField(TEXT("stack_guids"), MoveTemp(Stack));
		}
		Diagnostic.RelatedSources.Add(MakeShared<FJsonValueObject>(Source));
		return Diagnostic;
	}

	FResult InspectScripts(const TArray<UNiagaraScript*>& Scripts, const bool bCompilationComplete, const bool bIncludeDiagnostics)
	{
		FResult Result;
		Result.bCompilationComplete = bCompilationComplete;
		Result.ScriptCount = Scripts.Num();
		for (UNiagaraScript* Script : Scripts)
		{
			if (!Script)
			{
				continue;
			}
			Result.CompiledTargets.AddUnique(Script->GetPathName());
			const FNiagaraVMExecutableData& ExecutableData = Script->GetVMExecutableData();
			const bool bScriptHasError = HasScriptCompileError(
				Script->GetLastCompileStatus(), ExecutableData.ErrorMsg,
				ExecutableData.LastCompileEvents);
			// Error provenance is needed by authoring even when optional query
			// diagnostics were not requested. Successful observations stay compact.
			if (bIncludeDiagnostics || bScriptHasError)
			{
				const ENiagaraScriptCompileStatus Status = Script->GetLastCompileStatus();
				if (Status == ENiagaraScriptCompileStatus::NCS_Unknown
					|| Status == ENiagaraScriptCompileStatus::NCS_Dirty
					|| Status == ENiagaraScriptCompileStatus::NCS_BeingCreated)
				{
					Result.UncompiledTargets.Add(Script->GetPathName());
				}
				bool bHasErrorEvent = false;
				bool bHasWarningEvent = false;
				for (const FNiagaraCompileEvent& Event : ExecutableData.LastCompileEvents)
				{
					const bool bError = Event.Severity == FNiagaraCompileEventSeverity::Error;
					const bool bWarning = bIncludeDiagnostics && Event.Severity == FNiagaraCompileEventSeverity::Warning;
					if (!bError && !bWarning) continue;
					bHasErrorEvent |= bError;
					bHasWarningEvent |= bWarning;
					Result.Diagnostics.Add(MakeCompileEvent(Script->GetPathName(), bError, Event.Message, &Event));
				}
				if (!bHasErrorEvent && (Status == ENiagaraScriptCompileStatus::NCS_Error
					|| !ExecutableData.ErrorMsg.IsEmpty()))
				{
					Result.Diagnostics.Add(MakeCompileEvent(Script->GetPathName(), true,
						ExecutableData.ErrorMsg.IsEmpty() ? TEXT("The Niagara script has a failed compile status.") : ExecutableData.ErrorMsg));
				}
				if (!bHasWarningEvent && (Status == ENiagaraScriptCompileStatus::NCS_UpToDateWithWarnings
					|| Status == ENiagaraScriptCompileStatus::NCS_ComputeUpToDateWithWarnings))
				{
					Result.Diagnostics.Add(MakeCompileEvent(Script->GetPathName(), false, TEXT("The Niagara script compiled with warnings.")));
				}
			}
			if (bScriptHasError)
			{
				++Result.ErrorScriptCount;
				Result.bHasError = true;

			}

			const bool bScriptHasWarning = Script->GetLastCompileStatus() == ENiagaraScriptCompileStatus::NCS_UpToDateWithWarnings
				|| ExecutableData.LastCompileEvents.ContainsByPredicate([](const FNiagaraCompileEvent& Event) {
					return Event.Severity == FNiagaraCompileEventSeverity::Warning;
				});
			if (bScriptHasWarning) { ++Result.WarningScriptCount; Result.bHasWarning = true; }

		}
		Result.CompiledTargets.Sort();
		return Result;
	}

	FResult InspectAsset(UObject* Asset, const bool bIncludeDiagnostics)
	{
		TArray<UNiagaraScript*> Scripts;
		TSet<const UNiagaraScript*> SeenScripts;
		bool bCompilationComplete = true;
		bool bStoredCompileStateApplicable = true;

		if (UNiagaraSystem* System = Cast<UNiagaraSystem>(Asset))
		{
			bCompilationComplete = !System->HasOutstandingCompilationRequests(true);
			AppendUniqueScript(System->GetSystemSpawnScript(), Scripts, SeenScripts);
			AppendUniqueScript(System->GetSystemUpdateScript(), Scripts, SeenScripts);
			for (const FNiagaraEmitterHandle& Handle : System->GetEmitterHandles())
			{
				if (!Handle.GetIsEnabled())
				{
					continue;
				}
				const FVersionedNiagaraEmitter VersionedEmitter = Handle.GetInstance();
				const FVersionedNiagaraEmitterData* Data = VersionedEmitter.GetEmitterData();
				if (!Data && VersionedEmitter.Emitter)
				{
					Data = VersionedEmitter.Emitter->GetLatestEmitterData();
				}
				AppendEmitterScripts(Data, Scripts, SeenScripts, bIncludeDiagnostics);
			}
		}
		else if (UNiagaraEmitter* Emitter = Cast<UNiagaraEmitter>(Asset))
		{
			// Native standalone compilation uses a duplicated emitter in a transient
			// System. Its VM status is not written back to this source definition.
			bStoredCompileStateApplicable = Emitter->GetTypedOuter<UNiagaraSystem>() != nullptr;
			AppendEmitterScripts(Emitter->GetEmitterData(Emitter->GetExposedVersion().VersionGuid),
				Scripts, SeenScripts, bIncludeDiagnostics);
		}
		else if (UNiagaraScript* Script = Cast<UNiagaraScript>(Asset))
		{
			// Emitter Spawn/Update are auxiliary scripts, unlike standalone Module/
			// Function/DynamicInput scripts that can own their own compiled VM.
			bStoredCompileStateApplicable = Script->IsCompilable();
			AppendUniqueScript(Script, Scripts, SeenScripts);
		}
		else
		{
			FResult Result;
			Result.bCompilationComplete = false;
			Result.bHasError = true;
			auto Error = FDiagnostic::Failure(TEXT("niagara_compile_target_invalid"), TEXT("The target is not a Niagara asset that can be compiled."), TEXT("compile"));
			Error.SourceKind = TEXT("native_result");
			if (Asset && Asset->IsAsset()) Error.TargetPath = Asset->GetPathName();
			else if (Asset) Error.InternalContext = Asset->GetPathName();
			Error.PropertyPath = TEXT("asset.class");
			Error.ActualValue = Asset ? TSharedPtr<FJsonValue>(MakeShared<FJsonValueString>(Asset->GetClass()->GetName()))
				: TSharedPtr<FJsonValue>(MakeShared<FJsonValueNull>());
			Error.ExpectedValue = MakeShared<FJsonValueString>(TEXT("NiagaraSystem, NiagaraEmitter or NiagaraScript"));
			Result.Diagnostics.Add(MoveTemp(Error));
			return Result;
		}
		FResult Result = InspectScripts(Scripts, bCompilationComplete, bIncludeDiagnostics);
		Result.bStoredCompileStateApplicable = bStoredCompileStateApplicable;
		return Result;
	}
}
