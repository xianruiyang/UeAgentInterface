// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "Core/Contract/UeAgentDiagnostic.h"

#include "CoreMinimal.h"
#include "Utilities/AuthoringCore/UeAgentAuthoringState.h"

class UMaterialExpression;

namespace UeAgentMaterialAuthoring
{
	struct FInstanceParameterEdit;
	inline constexpr TCHAR ObservedGraphName[] = TEXT("observed_material_graph");
	inline constexpr TCHAR ObservedFunctionRoot[] = TEXT("observed_function_root");
	inline const TCHAR* GetAuthoringGraphName(bool bFunction) { return bFunction ? TEXT("MaterialFunctionGraph") : TEXT("MaterialGraph"); }
	bool BindObservedInstanceParameters(const TArray<FInstanceParameterEdit>& Edits, bool bObserveLayers, UeAgentAuthoringState::FReadRequest& Request,
		UE::UAI::Contract::FDiagnostic& OutError);
	UEAGENTINTERFACE_API UeAgentAuthoringState::FEditablePropertyReadOptions ExpressionPropertyPolicy(const UMaterialExpression& Expression);
	UEAGENTINTERFACE_API UObject* CreateFactorySeed(
		const FString& AssetKind,
		UObject* Outer,
		FName Name,
		EObjectFlags Flags,
		UE::UAI::Contract::FDiagnostic& OutError);
	UEAGENTINTERFACE_API bool BuildAssetSnapshotReadRequest(
		UObject* Asset,
		const FString& CanonicalAssetPath,
		const FString& AssetKind,
		bool bExpectedAbsent,
		TArray<UeAgentAuthoringState::FReadRequest>& OutRequests,
		UE::UAI::Contract::FDiagnostic& OutError);
	UEAGENTINTERFACE_API bool BuildAssetExportSnapshotReadRequests(
		UObject& Asset,
		TArray<UeAgentAuthoringState::FReadRequest>& OutRequests,
		UE::UAI::Contract::FDiagnostic& OutError);
	UEAGENTINTERFACE_API bool BuildRootPropertyReadRequests(UObject& Asset, TArray<UeAgentAuthoringState::FReadRequest>& Requests,
		UE::UAI::Contract::FDiagnostic& Error);
	UEAGENTINTERFACE_API bool RegisterStateAccessor(UE::UAI::Contract::FDiagnostic& OutError);
	UEAGENTINTERFACE_API bool RegisterNativeDefaultProvider(UE::UAI::Contract::FDiagnostic& OutError);
}
