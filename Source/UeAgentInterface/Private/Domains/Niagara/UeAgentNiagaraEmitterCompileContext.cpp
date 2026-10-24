// Copyright Epic Games, Inc. All Rights Reserved.

#include "Domains/Niagara/UeAgentNiagaraEmitterCompileContext.h"

#include "Core/Contract/UeAgentDiagnostic.h"
#include "Dom/JsonValue.h"
#include "Domains/Niagara/UeAgentNiagaraRendererBindings.h"

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
		UE::UAI::Contract::FDiagnostic& OutError)
	{
		OutResult = FResult();
		OutError.Reset();
		const auto FailNative = [&OutError, &Emitter](const TCHAR* Code, const TCHAR* Reason, const TCHAR* Field) {
			OutError = UE::UAI::Contract::FDiagnostic::Failure(Code, Reason, TEXT("niagara_compile"));
			OutError.SourceKind = TEXT("native_result");
			OutError.PropertyPath = Field;
			OutError.ActualValue = MakeShared<FJsonValueBoolean>(false);
			OutError.ExpectedValue = MakeShared<FJsonValueBoolean>(true);
			OutError.InternalContext = Emitter.GetPathName();
		};

		Emitter.UpdateEmitterAfterLoad();
		const FGuid VersionGuid = RequestedVersion.IsValid()
			? RequestedVersion
			: Emitter.GetExposedVersion().VersionGuid;
		if (!VersionGuid.IsValid() || !Emitter.GetEmitterData(VersionGuid))
		{
			FailNative(TEXT("niagara_emitter_version_not_found"),
				RequestedVersion.IsValid() ? TEXT("The requested emitter version has no available emitter data.")
					: TEXT("The emitter has no exposed version with available emitter data."), TEXT("emitter_version_available"));
			OutError.InternalContext += TEXT(" version=") + VersionGuid.ToString();
			return false;
		}

		UeAgentNiagaraRendererBindings::Refresh(FVersionedNiagaraEmitter(&Emitter, VersionGuid));
		OutResult.TransientSystem.Reset(NewObject<UNiagaraSystem>(
			GetTransientPackage(),
			NAME_None,
			RF_Transient));
		UNiagaraSystem* TransientSystem = OutResult.TransientSystem.Get();
		if (!TransientSystem)
		{
			FailNative(TEXT("niagara_emitter_compile_context_create_failed"),
				TEXT("UE could not create the temporary system required to compile this emitter."), TEXT("compile_system_created"));
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
			FailNative(TEXT("niagara_emitter_compile_context_missing_emitter"),
				TEXT("Adding the emitter to its temporary compilation system did not produce usable emitter data."), TEXT("compile_emitter_available"));
			OutError.InternalContext += TEXT(" handle=") + EmitterHandleId.ToString();
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
		OutResult.Validation = UeAgentNiagaraCompileValidation::InspectScripts(
			CompiledScripts, OutResult.bCompilationComplete);
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
