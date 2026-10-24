#pragma once

#include "NetworkTransport/UeAgentHttpTransportAdapter.h"

namespace UE::UAI::NetworkTransport
{
	bool IsSupportedHttpMethod(const FString& Method);
	TArray<uint8> MakeHttpResponseHead(const FUeAgentHttpResponse& Response);

	// The public transport uses one Content-Length framed request per exchange.
	// Parsing never interprets command payloads or touches UE objects.
	class FHttpRequestReader
	{
	public:
		bool Consume(const uint8* Data, int32 Count, FUeAgentHttpResponse& OutFailure);
		bool IsComplete() const { return bHeadersParsed && RemainingBodyBytes == 0; }
		FUeAgentHttpRequest Request;

	private:
		bool ParseHeaders(const FString& Text, FUeAgentHttpResponse& OutFailure);
		bool ConsumeBody(const uint8* Data, int32 Count, FUeAgentHttpResponse& OutFailure);
		TArray<uint8> HeaderBytes;
		int32 HeaderScanOffset = 0;
		int64 RemainingBodyBytes = 0;
		bool bHeadersParsed = false;
	};
}
