// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "Core/Contract/UeAgentDiagnostic.h"

#include "CoreMinimal.h"
#include "NiagaraTypes.h"
#include "UObject/StrongObjectPtr.h"

class FJsonObject;
class FJsonValue;
struct FNiagaraParameterStore;
class UNiagaraSystem;

namespace UeAgentNiagaraUserParameterAuthoring
{
	struct FPatch
	{
		FString SourceKey;
		FString ParameterName;
		FNiagaraTypeDefinition TypeDefinition;
		TSharedPtr<FJsonValue> Value;
		bool bTypeExplicit = false;
		bool bHasValue = false;
		bool bRemove = false;
		TSharedPtr<const FJsonObject> SourceDocument;
		TSharedPtr<const TMap<FString, UE::UAI::Contract::FSourceLocation>> SourceLocations;
	};

	struct FSnapshotEntry
	{
		FNiagaraVariable Variable;
		TArray<uint8> ValueBytes;
		TStrongObjectPtr<UObject> ObjectValue;
		bool bHasValue = false;
	};

	UEAGENTINTERFACE_API FString CanonicalizeName(const FString& Name);
	UEAGENTINTERFACE_API bool FindByName(
		const UNiagaraSystem& System,
		const FString& ParameterName,
		FNiagaraVariable& OutVariable);
	UEAGENTINTERFACE_API bool ParsePatches(
		UNiagaraSystem* System,
		const TSharedPtr<const FJsonObject>& ProviderExtension,
		TArray<FPatch>& OutPatches,
		UE::UAI::Contract::FDiagnostic& OutError);
	UEAGENTINTERFACE_API bool ApplyPatches(
		UNiagaraSystem& System,
		const TArray<FPatch>& Patches,
		UE::UAI::Contract::FDiagnostic& OutError);
	UEAGENTINTERFACE_API bool ApplyValuePatchesToStore(
		const UNiagaraSystem& System,
		FNiagaraParameterStore& Store,
		const TArray<FPatch>& Patches,
		UE::UAI::Contract::FDiagnostic& OutError);
	UEAGENTINTERFACE_API void CaptureSnapshot(
		const UNiagaraSystem& System,
		TArray<FSnapshotEntry>& OutSnapshot);
	UEAGENTINTERFACE_API bool RestoreSnapshot(
		UNiagaraSystem& System,
		const TArray<FSnapshotEntry>& Snapshot,
		UE::UAI::Contract::FDiagnostic& OutError);
	UEAGENTINTERFACE_API void Enumerate(
		const UNiagaraSystem& System,
		TArray<FNiagaraVariable>& OutVariables);
	UEAGENTINTERFACE_API TSharedRef<FJsonObject> BuildState(
		const UNiagaraSystem& System,
		const FNiagaraVariable& Variable,
		bool bIncludeValueText);
	UEAGENTINTERFACE_API TSharedRef<FJsonObject> BuildStoreState(
		const FNiagaraParameterStore& Store,
		const FNiagaraVariable& Variable,
		bool bIncludeValueText);
}
