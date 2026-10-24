#include "Core/Contract/UeAgentCommandJson.h"

#include "Core/Contract/UeAgentContractJson.h"

namespace UE::UAI::CommandContract::Json
{
	namespace
	{
		void TranslateError(UE::UAI::Contract::FDiagnostic& Error)
		{
			if (Error.Code.StartsWith(TEXT("contract_json_")))
			{
				auto Cause = MakeShared<UE::UAI::Contract::FDiagnostic>(Error);
				Error = UE::UAI::Contract::FDiagnostic::Failure(TEXT("command_json_") + Cause->Code.RightChop(14),
					Cause->PublicMessage, TEXT("command_json"));
				Error.CauseCode = Cause->Code;
				Error.PublicCause = MoveTemp(Cause);
			}
		}
	}

	bool TryParseObject(
		const FString& Text,
		TSharedPtr<FJsonObject>& OutObject,
		UE::UAI::Contract::FDiagnostic& OutError)
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
		UE::UAI::Contract::FDiagnostic& OutError)
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
		UE::UAI::Contract::FDiagnostic& OutError)
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
		UE::UAI::Contract::FDiagnostic& OutError)
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
		UE::UAI::Contract::FDiagnostic& OutError)
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
