#pragma once

#include "CoreMinimal.h"
#include "Core/Contract/UeAgentDiagnostic.h"
#include "MaterialShared.h"
#include "MaterialTypes.h"
#include "UObject/StrongObjectPtr.h"

class FJsonObject;
class FJsonValue;
class UMaterialInstanceConstant;
class UMaterialFunctionInstance;
class UTexture;
struct FMaterialLayersFunctions;

namespace UeAgentMaterialAuthoring
{
	// Native parameter intent, after the common workspace has resolved identity and values.
	struct FInstanceParameterEdit
	{
		FString Kind;
		FMaterialParameterInfo Info;
		bool bOverride = true;
		float Scalar = 0;
		FLinearColor Vector = FLinearColor::Black;
		TStrongObjectPtr<UTexture> Texture;
		bool bSwitch = false;
		FMaterialParameterMetadata Native;
		TStrongObjectPtr<UObject> Resource;
		TSharedPtr<FJsonObject> Source;
		TMap<FString, UE::UAI::Contract::FSourceLocation> SourceLocations;
	};

	UEAGENTINTERFACE_API bool CaptureInstanceParameters(UMaterialInstanceConstant& Instance,
		TArray<TSharedPtr<FJsonValue>>& OutParameters, UE::UAI::Contract::FDiagnostic& OutError,
		TArray<TSharedPtr<FJsonValue>>* OutDefinitions = nullptr);
	UEAGENTINTERFACE_API bool PrepareInstanceParameters(UMaterialInstanceConstant& Instance,
		const TArray<TSharedPtr<FJsonValue>>& Parameters,
		TArray<FInstanceParameterEdit>& OutEdits, UE::UAI::Contract::FDiagnostic& OutError);
	UEAGENTINTERFACE_API bool ApplyInstanceParameters(UMaterialInstanceConstant& Instance,
		const TArray<FInstanceParameterEdit>& Edits, UE::UAI::Contract::FDiagnostic& OutError);
	UEAGENTINTERFACE_API bool VerifyInstanceParameters(UMaterialInstanceConstant& Instance,
		const TArray<FInstanceParameterEdit>& Edits, UE::UAI::Contract::FDiagnostic& OutError);
	bool VerifyInstanceParameters(UMaterialFunctionInstance& Instance,
		const TArray<FInstanceParameterEdit>& Edits, UE::UAI::Contract::FDiagnostic& OutError);
	UEAGENTINTERFACE_API bool CaptureInstanceLayers(UMaterialInstanceConstant& Instance,
		TSharedPtr<FJsonValue>& Value, UE::UAI::Contract::FDiagnostic& OutError);
	bool CaptureLayerStack(UObject& Owner, FMaterialLayersFunctions& Layers,
		TSharedPtr<FJsonValue>& Value, UE::UAI::Contract::FDiagnostic& OutError);
	bool PrepareLayerStack(const FMaterialLayersFunctions& Before, const TSharedPtr<FJsonValue>& Value,
		FMaterialLayersFunctions& After, UE::UAI::Contract::FDiagnostic& OutError);
	UEAGENTINTERFACE_API bool ApplyInstanceLayers(UMaterialInstanceConstant& Instance,
		const TSharedPtr<FJsonValue>& Value, UE::UAI::Contract::FDiagnostic& OutError);
}
