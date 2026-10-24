#pragma once

#include "CoreMinimal.h"
#include "Core/Contract/UeAgentDiagnostic.h"
#include "Dom/JsonObject.h"

namespace UE::UAI::Contract::Json
{
	UEAGENTINTERFACE_API bool TryParseObject(
		const FString& Text,
		TSharedPtr<FJsonObject>& OutObject,
		UE::UAI::Contract::FDiagnostic& OutError);

	UEAGENTINTERFACE_API bool TryParseObject(
		const TArray<uint8>& Utf8Bytes,
		TSharedPtr<FJsonObject>& OutObject,
		UE::UAI::Contract::FDiagnostic& OutError);

	UEAGENTINTERFACE_API bool TryCanonicalize(
		const TSharedPtr<FJsonValue>& Value,
		FString& OutText,
		UE::UAI::Contract::FDiagnostic& OutError);

	UEAGENTINTERFACE_API bool TryCanonicalize(
		const TSharedPtr<FJsonObject>& Object,
		FString& OutText,
		UE::UAI::Contract::FDiagnostic& OutError);

	UEAGENTINTERFACE_API bool TryCanonicalUtf8(
		const TSharedPtr<FJsonObject>& Object,
		TArray<uint8>& OutBytes,
		UE::UAI::Contract::FDiagnostic& OutError);

	UEAGENTINTERFACE_API FString Sha256Utf8(const FString& Text);
}
