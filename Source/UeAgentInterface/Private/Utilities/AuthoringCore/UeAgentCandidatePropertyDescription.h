#pragma once

#include "Core/Contract/UeAgentDiagnostic.h"

#include "CoreMinimal.h"
#include "Utilities/AuthoringCore/UeAgentAuthoringState.h"

class FJsonObject;
class UObject;

namespace UE::UAI::CandidateProperties
{
	struct FRequest
	{
		FString Name;
		TMap<FString, FString> Select;
		TSet<FName> Fields;
	};

	// Explicit selected candidate only; ordinary discovery stays names-only.
	bool ReadRequest(const TSharedPtr<FJsonObject>& Description, FRequest& Out, UE::UAI::Contract::FDiagnostic& OutError);
	bool DescribeObject(const UObject& Prototype, const FRequest& Request,
		UeAgentAuthoringState::FEditablePropertyReadOptions Policy,
		TSharedPtr<FJsonObject>& OutDescription, UE::UAI::Contract::FDiagnostic& OutError);
	// Native menu discovery and current HJSON creation coverage are distinct.
	void SetCreationSupport(FJsonObject& Description, bool bSupported, const FString& Reason);
	// Menus whose editable fields require a created node share this compact
	// capability description; field requests are explicitly unavailable here.
	bool BuildCreationSupportPayload(const FRequest& Request, bool bSupported,
		const FString& Reason, const FString& Basis, const FString& InterfaceScope,
		FString& OutPayload, UE::UAI::Contract::FDiagnostic& OutError);
	bool BuildPayload(const FRequest& Request, const TSharedPtr<FJsonObject>& Description,
		FString& OutPayload, UE::UAI::Contract::FDiagnostic& OutError);
}
