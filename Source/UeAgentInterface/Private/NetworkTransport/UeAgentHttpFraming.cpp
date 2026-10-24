#include "NetworkTransport/UeAgentHttpFraming.h"

#include "Containers/StringConv.h"
#include "Containers/UnrealString.h"

namespace UE::UAI::NetworkTransport
{
	bool IsSupportedHttpMethod(const FString& Method)
	{
		return Method.Equals(TEXT("GET"), ESearchCase::IgnoreCase)
			|| Method.Equals(TEXT("POST"), ESearchCase::IgnoreCase)
			|| Method.Equals(TEXT("PUT"), ESearchCase::IgnoreCase)
			|| Method.Equals(TEXT("PATCH"), ESearchCase::IgnoreCase)
			|| Method.Equals(TEXT("DELETE"), ESearchCase::IgnoreCase)
			|| Method.Equals(TEXT("OPTIONS"), ESearchCase::IgnoreCase);
	}

	TArray<uint8> MakeHttpResponseHead(const FUeAgentHttpResponse& Response)
	{
		const TCHAR* Reason = TEXT("Error");
		switch (Response.StatusCode)
		{
		case 200: Reason = TEXT("OK"); break;
		case 400: Reason = TEXT("Bad Request"); break;
		case 401: Reason = TEXT("Unauthorized"); break;
		case 404: Reason = TEXT("Not Found"); break;
		case 405: Reason = TEXT("Method Not Allowed"); break;
		case 408: Reason = TEXT("Request Timeout"); break;
		case 413: Reason = TEXT("Content Too Large"); break;
		case 417: Reason = TEXT("Expectation Failed"); break;
		case 431: Reason = TEXT("Request Header Fields Too Large"); break;
		case 500: Reason = TEXT("Internal Server Error"); break;
		case 501: Reason = TEXT("Not Implemented"); break;
		case 502: Reason = TEXT("Bad Gateway"); break;
		case 503: Reason = TEXT("Service Unavailable"); break;
		}
		FString Text = FString::Printf(TEXT("HTTP/1.1 %d %s\r\nContent-Length: %d\r\nConnection: close\r\n"),
			Response.StatusCode, Reason, Response.Body.Num());
		if (!Response.Headers.Contains(TEXT("Content-Type"))) Text += TEXT("Content-Type: application/octet-stream\r\n");
		for (const TPair<FString, FString>& Header : Response.Headers)
		{
			if (!Header.Key.Equals(TEXT("Content-Length"), ESearchCase::IgnoreCase)
				&& !Header.Key.Equals(TEXT("Connection"), ESearchCase::IgnoreCase))
			{
				Text += Header.Key + TEXT(": ") + Header.Value.Replace(TEXT("\r"), TEXT(" ")).Replace(TEXT("\n"), TEXT(" ")) + TEXT("\r\n");
			}
		}
		Text += TEXT("\r\n");
		const FTCHARToUTF8 Bytes(*Text);
		TArray<uint8> Result;
		Result.Append(reinterpret_cast<const uint8*>(Bytes.Get()), Bytes.Length());
		return Result;
	}

	bool FHttpRequestReader::Consume(const uint8* Data, int32 Count, FUeAgentHttpResponse& OutFailure)
	{
		if (bHeadersParsed) return ConsumeBody(Data, Count, OutFailure);
		if (Count > MAX_int32 - HeaderBytes.Num())
		{
			OutFailure = FUeAgentHttpResponse::Failure(431, TEXT("http_header_length_overflow"), TEXT("HTTP headers exceed the supported buffer representation."));
			return false;
		}
		HeaderBytes.Append(Data, Count);
		for (; HeaderScanOffset + 3 < HeaderBytes.Num(); ++HeaderScanOffset)
		{
			const uint8* At = HeaderBytes.GetData() + HeaderScanOffset;
			if (At[0] != '\r' || At[1] != '\n' || At[2] != '\r' || At[3] != '\n') continue;
			const FUTF8ToTCHAR HeaderText(reinterpret_cast<const ANSICHAR*>(HeaderBytes.GetData()), HeaderScanOffset);
			if (!ParseHeaders(FString(HeaderText.Length(), HeaderText.Get()), OutFailure)) return false;
			bHeadersParsed = true;
			const int32 BodyOffset = HeaderScanOffset + 4;
			const bool bValid = ConsumeBody(HeaderBytes.GetData() + BodyOffset, HeaderBytes.Num() - BodyOffset, OutFailure);
			HeaderBytes.Empty();
			return bValid;
		}
		return true;
	}

	bool FHttpRequestReader::ParseHeaders(const FString& Text, FUeAgentHttpResponse& OutFailure)
	{
		auto Invalid = [&OutFailure](const TCHAR* Code, const TCHAR* Message)
		{
			OutFailure = FUeAgentHttpResponse::Failure(400, Code, Message);
			return false;
		};
		TArray<FString> Lines;
		Text.ParseIntoArray(Lines, TEXT("\r\n"), false);
		TArray<FString> StartLine;
		if (!Lines.IsEmpty()) Lines[0].ParseIntoArray(StartLine, TEXT(" "), true);
		if (StartLine.Num() != 3 || !IsSupportedHttpMethod(StartLine[0])
			|| (!StartLine[2].Equals(TEXT("HTTP/1.1")) && !StartLine[2].Equals(TEXT("HTTP/1.0"))))
			return Invalid(TEXT("http_request_line_invalid"), TEXT("Expected a supported method, origin-form target and HTTP/1.0 or HTTP/1.1."));
		Request.Method = StartLine[0].ToUpper();
		Request.RoutePath = StartLine[1];
		int32 QueryOffset;
		if (Request.RoutePath.FindChar('?', QueryOffset)) Request.RoutePath.LeftInline(QueryOffset);
		if (!Request.RoutePath.StartsWith(TEXT("/")))
			return Invalid(TEXT("http_request_target_invalid"), TEXT("The HTTP target must be an origin-form path."));
		for (int32 Index = 1; Index < Lines.Num(); ++Index)
		{
			FString Name, Value;
			if (!Lines[Index].Split(TEXT(":"), &Name, &Value) || Name.IsEmpty() || Name.TrimStartAndEnd() != Name)
				return Invalid(TEXT("http_header_invalid"), TEXT("An HTTP header has an invalid field name or lacks a colon."));
			Name.ToLowerInline();
			Value.TrimStartAndEndInline();
			if (const FString* Existing = Request.Headers.Find(Name))
			{
				if (Name == TEXT("content-length") && *Existing != Value)
					return Invalid(TEXT("http_content_length_conflict"), TEXT("Repeated Content-Length fields disagree."));
				continue; // Preserve the previous adapter's first-value header mapping.
			}
			Request.Headers.Add(MoveTemp(Name), MoveTemp(Value));
		}
		if (Request.Headers.Contains(TEXT("transfer-encoding")))
		{
			OutFailure = FUeAgentHttpResponse::Failure(501, TEXT("http_transfer_encoding_unsupported"), TEXT("Use Content-Length framing; transfer encodings are not supported."));
			return false;
		}
		if (Request.Headers.Contains(TEXT("expect")))
		{
			OutFailure = FUeAgentHttpResponse::Failure(417, TEXT("http_expectation_unsupported"), TEXT("Send the Content-Length framed request without an Expect handshake."));
			return false;
		}
		const FString* ContentLength = Request.Headers.Find(TEXT("content-length"));
		if (!ContentLength && (Request.Method == TEXT("POST") || Request.Method == TEXT("PUT")))
			return Invalid(TEXT("http_content_length_required"), TEXT("POST and PUT requests require Content-Length."));
		if (ContentLength)
		{
			if (ContentLength->IsEmpty()) return Invalid(TEXT("http_content_length_invalid"), TEXT("Content-Length must contain a non-negative integer."));
			for (TCHAR Character : *ContentLength)
				if (Character < '0' || Character > '9') return Invalid(TEXT("http_content_length_invalid"), TEXT("Content-Length must contain a non-negative integer."));
			if (!LexTryParseString(RemainingBodyBytes, **ContentLength) || RemainingBodyBytes < 0)
				return Invalid(TEXT("http_content_length_invalid"), TEXT("Content-Length cannot be represented as a non-negative integer."));
		}
		if (RemainingBodyBytes > MAX_int32)
		{
			OutFailure = FUeAgentHttpResponse::Failure(413, TEXT("http_body_length_overflow"), TEXT("Content-Length exceeds the transport byte-array representation."));
			return false;
		}
		if (Request.Method == TEXT("GET") && RemainingBodyBytes != 0)
			return Invalid(TEXT("http_get_body_unsupported"), TEXT("GET requests must have an empty body."));
		return true;
	}

	bool FHttpRequestReader::ConsumeBody(const uint8* Data, int32 Count, FUeAgentHttpResponse& OutFailure)
	{
		if (Count > RemainingBodyBytes)
		{
			OutFailure = FUeAgentHttpResponse::Failure(400, TEXT("http_content_length_mismatch"), TEXT("Received bytes beyond Content-Length; pipelined requests are not supported."));
			return false;
		}
		Request.Body.Append(Data, Count);
		RemainingBodyBytes -= Count;
		return true;
	}
}
