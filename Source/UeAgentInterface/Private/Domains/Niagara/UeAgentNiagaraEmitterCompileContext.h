// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "Core/Contract/UeAgentDiagnostic.h"

#include "Domains/Niagara/UeAgentNiagaraCompileValidation.h"
#include "NiagaraEmitter.h"
#include "UObject/StrongObjectPtr.h"

class UNiagaraSystem;

namespace UeAgentNiagaraEmitterCompileContext
{
	struct FResult
	{
		TStrongObjectPtr<UNiagaraSystem> TransientSystem;
		FVersionedNiagaraEmitter CompiledEmitter;
		bool bCompilationRequested = false;
		bool bCompilationComplete = false;
		bool bHasOutstandingCompilationRequests = false;
		UeAgentNiagaraCompileValidation::FResult Validation;
	};

	/** Compiles an emitter through the transient system context required by Niagara. */
	bool Compile(
		UNiagaraEmitter& Emitter,
		const FGuid& RequestedVersion,
		bool bForceCompile,
		FResult& OutResult,
		UE::UAI::Contract::FDiagnostic& OutError);

	void GetCompiledScripts(const FResult& Result, TArray<UNiagaraScript*>& OutScripts);
}
