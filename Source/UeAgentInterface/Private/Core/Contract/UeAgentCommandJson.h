#pragma once

#include "CoreMinimal.h"
#include "Dom/JsonObject.h"

namespace UE::UAI::CommandContract::Json
{
	UEAGENTINTERFACE_API bool TryParseObject(
		const FString& Text,
		TSharedPtr<FJsonObject>& OutObject,
		FString& OutError);

	UEAGENTINTERFACE_API bool TryParseObject(
		const TArray<uint8>& Utf8Bytes,
		TSharedPtr<FJsonObject>& OutObject,
		FString& OutError);

	UEAGENTINTERFACE_API bool TryCanonicalize(
		const TSharedPtr<FJsonValue>& Value,
		FString& OutText,
		FString& OutError);

	UEAGENTINTERFACE_API bool TryCanonicalize(
		const TSharedPtr<FJsonObject>& Object,
		FString& OutText,
		FString& OutError);

	UEAGENTINTERFACE_API bool TryCanonicalUtf8(
		const TSharedPtr<FJsonObject>& Object,
		TArray<uint8>& OutBytes,
		FString& OutError);

	UEAGENTINTERFACE_API FString Sha256Utf8(const FString& Text);
}
