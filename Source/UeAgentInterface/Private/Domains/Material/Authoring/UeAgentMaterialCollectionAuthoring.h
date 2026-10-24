#pragma once

#include "Core/Contract/UeAgentDiagnostic.h"
#include "CoreMinimal.h"
#include "Utilities/AuthoringObjectApply/UeAgentAuthoringObjectApply.h"
class UMaterialParameterCollection;
class FJsonValue;
class FJsonObject;

namespace UeAgentMaterialAuthoring::Collection
{
struct FParameterIntent
{
    FName Name;
    FString SourceKey;
    TSharedPtr<FJsonValue> Settings;
    UeAgentAuthoringObjectApply::FPropertyIntent Properties;
};
struct FIntent
{
    TSharedPtr<FJsonObject> SourceDocument;
    TMap<FString, UE::UAI::Contract::FSourceLocation> SourceLocations;
    TArray<FParameterIntent> Scalars;
    TArray<FParameterIntent> Vectors;
    bool bScalars = false;
    bool bVectors = false;
    bool Verify(UMaterialParameterCollection& Asset, UE::UAI::Contract::FDiagnostic& Error) const;
};
bool Read(UMaterialParameterCollection& Asset, TSharedPtr<FJsonValue>& Value, UE::UAI::Contract::FDiagnostic& Error);
bool Apply(UMaterialParameterCollection& Asset, const TSharedPtr<FJsonValue>& Value, UE::UAI::Contract::FDiagnostic& Error,
    TSharedPtr<const FIntent>* OutIntent = nullptr);
TSharedPtr<FJsonObject> BuildValueDefinition();
}
