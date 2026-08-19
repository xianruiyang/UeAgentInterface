#include "Core/Contract/UeAgentCommandJson.h"

#include "Core/Contract/UeAgentContractJson.h"

namespace UE::UAI::CommandContract::Json
{
	namespace
	{
		void TranslateError(FString& Error)
		{
			if (Error.StartsWith(TEXT("contract_json_")))
			{
				Error =
					TEXT("command_json_")
					+ Error.RightChop(14);
			}
		}
	}

	bool TryParseObject(
		const FString& Text,
		TSharedPtr<FJsonObject>& OutObject,
		FString& OutError)
	{
		const bool bResult =
			UE::UAI::Contract::Json::TryParseObject(
				Text,
				OutObject,
				OutError);
		TranslateError(OutError);
		return bResult;
	}

	bool TryParseObject(
		const TArray<uint8>& Utf8Bytes,
		TSharedPtr<FJsonObject>& OutObject,
		FString& OutError)
	{
		const bool bResult =
			UE::UAI::Contract::Json::TryParseObject(
				Utf8Bytes,
				OutObject,
				OutError);
		TranslateError(OutError);
		return bResult;
	}

	bool TryCanonicalize(
		const TSharedPtr<FJsonValue>& Value,
		FString& OutText,
		FString& OutError)
	{
		const bool bResult =
			UE::UAI::Contract::Json::TryCanonicalize(
				Value,
				OutText,
				OutError);
		TranslateError(OutError);
		return bResult;
	}

	bool TryCanonicalize(
		const TSharedPtr<FJsonObject>& Object,
		FString& OutText,
		FString& OutError)
	{
		const bool bResult =
			UE::UAI::Contract::Json::TryCanonicalize(
				Object,
				OutText,
				OutError);
		TranslateError(OutError);
		return bResult;
	}

	bool TryCanonicalUtf8(
		const TSharedPtr<FJsonObject>& Object,
		TArray<uint8>& OutBytes,
		FString& OutError)
	{
		const bool bResult =
			UE::UAI::Contract::Json::TryCanonicalUtf8(
				Object,
				OutBytes,
				OutError);
		TranslateError(OutError);
		return bResult;
	}

	FString Sha256Utf8(const FString& Text)
	{
		return UE::UAI::Contract::Json::Sha256Utf8(Text);
	}
}
