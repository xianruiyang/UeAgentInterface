// Copyright Epic Games, Inc. All Rights Reserved.

#include "Domains/Niagara/UeAgentNiagaraEmitterCompileContext.h"

#include "NiagaraEditorUtilities.h"
#include "NiagaraEmitterHandle.h"
#include "NiagaraSystem.h"
#include "NiagaraSystemFactoryNew.h"

namespace UeAgentNiagaraEmitterCompileContext
{
	bool Compile(
		UNiagaraEmitter& Emitter,
		const FGuid& RequestedVersion,
		const bool bForceCompile,
		FResult& OutResult,
		FString& OutError)
	{
		OutResult = FResult();
		OutError.Reset();

		Emitter.UpdateEmitterAfterLoad();
		const FGuid VersionGuid = RequestedVersion.IsValid()
			? RequestedVersion
			: Emitter.GetExposedVersion().VersionGuid;
		if (!VersionGuid.IsValid() || !Emitter.GetEmitterData(VersionGuid))
		{
			OutError = TEXT("niagara_emitter_version_not_found");
			return false;
		}

		OutResult.TransientSystem.Reset(NewObject<UNiagaraSystem>(
			GetTransientPackage(),
			NAME_None,
			RF_Transient));
		UNiagaraSystem* TransientSystem = OutResult.TransientSystem.Get();
		if (!TransientSystem)
		{
			OutError = TEXT("niagara_emitter_compile_context_create_failed");
			return false;
		}

		// Mirrors NiagaraEditor's native standalone-emitter compilation path.
		UNiagaraSystemFactoryNew::InitializeSystem(TransientSystem, true);
		const FGuid EmitterHandleId = FNiagaraEditorUtilities::AddEmitterToSystem(
			*TransientSystem,
			Emitter,
			VersionGuid,
			true);
		const FNiagaraEmitterHandle* EmitterHandle = TransientSystem->GetEmitterHandles().FindByPredicate(
			[EmitterHandleId](const FNiagaraEmitterHandle& Candidate)
			{
				return Candidate.GetId() == EmitterHandleId;
			});
		OutResult.CompiledEmitter = EmitterHandle
			? EmitterHandle->GetInstance()
			: FVersionedNiagaraEmitter();
		if (!OutResult.CompiledEmitter.Emitter || !OutResult.CompiledEmitter.GetEmitterData())
		{
			OutError = TEXT("niagara_emitter_compile_context_missing_emitter");
			return false;
		}

		OutResult.bCompilationRequested = TransientSystem->RequestCompile(bForceCompile);
		TransientSystem->WaitForCompilationComplete(true, false);
		TransientSystem->PollForCompilationComplete(true);
		TransientSystem->CacheFromCompiledData();
		OutResult.bHasOutstandingCompilationRequests = TransientSystem->HasOutstandingCompilationRequests(true);
		OutResult.bCompilationComplete = !OutResult.bHasOutstandingCompilationRequests;

		TArray<UNiagaraScript*> CompiledScripts;
		GetCompiledScripts(OutResult, CompiledScripts);
		OutResult.Validation = UeAgentNiagaraCompileValidation::InspectScripts(CompiledScripts);
		return true;
	}

	void GetCompiledScripts(const FResult& Result, TArray<UNiagaraScript*>& OutScripts)
	{
		OutScripts.Reset();
		if (FVersionedNiagaraEmitterData* EmitterData = Result.CompiledEmitter.GetEmitterData())
		{
			EmitterData->GetScripts(OutScripts, false, false);
		}
	}
}
