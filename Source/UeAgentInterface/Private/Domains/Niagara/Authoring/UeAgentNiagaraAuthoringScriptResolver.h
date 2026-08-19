// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"

class FJsonObject;
class UNiagaraScript;

namespace UeAgentNiagaraAuthoring
{
	enum class EScriptKind : uint8
	{
		Any,
		Module,
		DynamicInput,
	};

	struct UEAGENTINTERFACE_API FScriptResolveRequest
	{
		EScriptKind Kind = EScriptKind::Any;
		TSharedPtr<const FJsonObject> Input;
		bool bAllowPrivateInline = false;
	};

	struct UEAGENTINTERFACE_API FScriptResolveResult
	{
		UNiagaraScript* Script = nullptr;
		FString ResolvedObjectPath;
		FString Source;

		bool IsResolved() const { return Script != nullptr && !ResolvedObjectPath.IsEmpty(); }
	};

	UEAGENTINTERFACE_API bool ResolveScript(
		const FScriptResolveRequest& Request,
		FScriptResolveResult& OutResult,
		FString& OutError);
}
