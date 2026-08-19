// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"

class FJsonObject;
class UEnum;
struct FNiagaraTypeDefinition;

namespace UeAgentNiagaraOps
{
	UObject* LoadAssetObject(const FString& InPath);
	bool IsUserSettableNiagaraEnumValue(const UEnum& EnumType, int64 EnumValue);
	UEAGENTINTERFACE_API TSharedPtr<FJsonObject> BuildNiagaraInputTypeDescriptorJson(const FString& InTypeText, const FNiagaraTypeDefinition& TypeDefinition);
}
