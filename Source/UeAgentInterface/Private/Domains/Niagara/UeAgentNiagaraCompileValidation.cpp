// Copyright Epic Games, Inc. All Rights Reserved.

#include "Domains/Niagara/UeAgentNiagaraCompileValidation.h"

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
			TSet<const UNiagaraScript*>& Seen)
		{
			if (!EmitterData)
			{
				return;
			}

			TArray<UNiagaraScript*> EmitterScripts;
			EmitterData->GetScripts(EmitterScripts, false, false);
			for (UNiagaraScript* Script : EmitterScripts)
			{
				AppendUniqueScript(Script, OutScripts, Seen);
			}
		}
	}

	bool HasScriptCompileError(
		const ENiagaraScriptCompileStatus CompileStatus,
		const FString& ErrorMessage,
		const TArray<FNiagaraCompileEvent>& CompileEvents,
		FString* OutFirstError)
	{
		if (CompileStatus == ENiagaraScriptCompileStatus::NCS_Error || !ErrorMessage.IsEmpty())
		{
			if (OutFirstError)
			{
				*OutFirstError = ErrorMessage.IsEmpty() ? TEXT("script_compile_status_error") : ErrorMessage;
			}
			return true;
		}

		for (const FNiagaraCompileEvent& Event : CompileEvents)
		{
			if (Event.Severity == FNiagaraCompileEventSeverity::Error)
			{
				if (OutFirstError)
				{
					*OutFirstError = Event.Message;
				}
				return true;
			}
		}
		return false;
	}

	FResult InspectScripts(const TArray<UNiagaraScript*>& Scripts, const bool bCompilationComplete)
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
			FString ScriptError;
			const bool bScriptHasError = HasScriptCompileError(
					Script->GetLastCompileStatus(),
					ExecutableData.ErrorMsg,
					ExecutableData.LastCompileEvents,
					&ScriptError);
			if (bScriptHasError)
			{
				++Result.ErrorScriptCount;
				Result.bHasError = true;
				if (Result.FirstError.IsEmpty())
				{
					Result.FirstError = ScriptError;
				}
			}

			FString ScriptWarning;
			const bool bScriptHasWarning =
				Script->GetLastCompileStatus()
					== ENiagaraScriptCompileStatus::NCS_UpToDateWithWarnings
				|| ExecutableData.LastCompileEvents.ContainsByPredicate(
					[&ScriptWarning](const FNiagaraCompileEvent& Event)
					{
						if (Event.Severity
							!= FNiagaraCompileEventSeverity::Warning)
						{
							return false;
						}
						if (ScriptWarning.IsEmpty())
						{
							ScriptWarning = Event.Message;
						}
						return true;
					});
			if (bScriptHasWarning)
			{
				++Result.WarningScriptCount;
				Result.bHasWarning = true;
				if (Result.FirstWarning.IsEmpty())
				{
					Result.FirstWarning = ScriptWarning.IsEmpty()
						? TEXT("script_compile_status_warning")
						: ScriptWarning;
				}
			}
		}
		Result.CompiledTargets.Sort();
		return Result;
	}

	FResult InspectAsset(UObject* Asset)
	{
		TArray<UNiagaraScript*> Scripts;
		TSet<const UNiagaraScript*> SeenScripts;
		bool bCompilationComplete = true;

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
				AppendEmitterScripts(Data, Scripts, SeenScripts);
			}
		}
		else if (UNiagaraEmitter* Emitter = Cast<UNiagaraEmitter>(Asset))
		{
			AppendEmitterScripts(Emitter->GetLatestEmitterData(), Scripts, SeenScripts);
		}
		else if (UNiagaraScript* Script = Cast<UNiagaraScript>(Asset))
		{
			AppendUniqueScript(Script, Scripts, SeenScripts);
		}
		else
		{
			FResult Result;
			Result.bCompilationComplete = false;
			Result.bHasError = true;
			Result.FirstError = TEXT("niagara_compile_target_invalid");
			return Result;
		}
		return InspectScripts(Scripts, bCompilationComplete);
	}
}
