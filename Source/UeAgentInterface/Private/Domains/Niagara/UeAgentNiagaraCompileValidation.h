// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "Core/Contract/UeAgentDiagnostic.h"

#include "CoreMinimal.h"
#include "NiagaraCommon.h"

class UObject;
class UNiagaraScript;
struct FNiagaraCompileEvent;

namespace UeAgentNiagaraCompileValidation
{
	using FDiagnostic = UE::UAI::Contract::FDiagnostic;

	struct FResult
	{
		bool bCompilationComplete = true;
		// Definitions compiled through a System instance do not own persisted VM freshness.
		bool bStoredCompileStateApplicable = true;
		bool bHasError = false;
		bool bHasWarning = false;
		int32 ScriptCount = 0;
		int32 ErrorScriptCount = 0;
		int32 WarningScriptCount = 0;
		TArray<FString> CompiledTargets;
		TArray<FString> UncompiledTargets;
		TArray<FDiagnostic> Diagnostics;
		UEAGENTINTERFACE_API FDiagnostic GetFailure(const FString& TargetPath = FString()) const;
	};

	UEAGENTINTERFACE_API bool HasScriptCompileError(
		ENiagaraScriptCompileStatus CompileStatus,
		const FString& ErrorMessage,
		const TArray<FNiagaraCompileEvent>& CompileEvents);

	UEAGENTINTERFACE_API FResult InspectScripts(const TArray<UNiagaraScript*>& Scripts, bool bCompilationComplete = true, bool bIncludeDiagnostics = false);
	UEAGENTINTERFACE_API FResult InspectAsset(UObject* Asset, bool bIncludeDiagnostics = false);
}
