// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"

class FJsonObject;
class UEnum;
struct FNiagaraTypeDefinition;

namespace UeAgentNiagaraOps
{
	bool IsUserSettableNiagaraEnumValue(const UEnum& EnumType, int64 EnumValue);
	TSharedPtr<FJsonObject> BuildNiagaraInputTypeDescriptorJson(const FString& InTypeText, const FNiagaraTypeDefinition& TypeDefinition);
}
