// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "NiagaraCommon.h"

class UObject;
class UNiagaraScript;
struct FNiagaraCompileEvent;

namespace UeAgentNiagaraCompileValidation
{
	struct FResult
	{
		bool bCompilationComplete = true;
		bool bHasError = false;
		bool bHasWarning = false;
		int32 ScriptCount = 0;
		int32 ErrorScriptCount = 0;
		int32 WarningScriptCount = 0;
		FString FirstError;
		FString FirstWarning;
		TArray<FString> CompiledTargets;
	};

	UEAGENTINTERFACE_API bool HasScriptCompileError(
		ENiagaraScriptCompileStatus CompileStatus,
		const FString& ErrorMessage,
		const TArray<FNiagaraCompileEvent>& CompileEvents,
		FString* OutFirstError = nullptr);

	UEAGENTINTERFACE_API FResult InspectScripts(const TArray<UNiagaraScript*>& Scripts, bool bCompilationComplete = true);
	UEAGENTINTERFACE_API FResult InspectAsset(UObject* Asset);
}
