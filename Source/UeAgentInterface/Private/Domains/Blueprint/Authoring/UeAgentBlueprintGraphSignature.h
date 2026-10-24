// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Core/Contract/UeAgentDiagnostic.h"

class FJsonObject;
class UEdGraph;
class UK2Node_CustomEvent;

namespace UeAgentBlueprintAuthoring
{
	using FDiagnostic = UE::UAI::Contract::FDiagnostic;
	UEAGENTINTERFACE_API bool CanAuthorFunctionGraphSignature(
		const UEdGraph& Graph);

	UEAGENTINTERFACE_API bool CaptureFunctionGraphSignature(
		const UEdGraph& Graph,
		TSharedRef<FJsonObject>& OutSignature,
		FDiagnostic& OutError);

	UEAGENTINTERFACE_API bool ApplyFunctionGraphSignature(
		UEdGraph& Graph,
		const TSharedPtr<const FJsonObject>& Signature,
		FDiagnostic& OutError);

	UEAGENTINTERFACE_API bool CanAuthorMacroGraphSignature(
		const UEdGraph& Graph);

	UEAGENTINTERFACE_API bool CaptureMacroGraphSignature(
		const UEdGraph& Graph,
		TSharedRef<FJsonObject>& OutSignature,
		FDiagnostic& OutError);

	UEAGENTINTERFACE_API bool ApplyMacroGraphSignature(
		UEdGraph& Graph,
		const TSharedPtr<const FJsonObject>& Signature,
		FDiagnostic& OutError);

	UEAGENTINTERFACE_API bool CaptureDelegateSignatureGraph(
		const UEdGraph& Graph,
		TSharedRef<FJsonObject>& OutSignature,
		FDiagnostic& OutError);

	UEAGENTINTERFACE_API bool ApplyDelegateSignatureGraph(
		UEdGraph& Graph,
		const TSharedPtr<const FJsonObject>& Signature,
		FDiagnostic& OutError);

	UEAGENTINTERFACE_API bool CaptureCustomEventSignature(
		const UK2Node_CustomEvent& Event,
		TSharedRef<FJsonObject>& OutSignature,
		FDiagnostic& OutError);

	UEAGENTINTERFACE_API bool ApplyCustomEventSignature(
		UK2Node_CustomEvent& Event,
		const TSharedPtr<const FJsonObject>& Signature,
		FDiagnostic& OutError);
}
