#include "Core/StateSemantic/UeAgentStateSemanticNormalization.h"

#include "Containers/StringConv.h"
#include "unicode/unorm2.h"
#include "unicode/ustring.h"

namespace UE::UAI::StateSemantic::Private
{
	bool TryNormalizeNfcText(const FString& Value, FString& OutValue)
	{
		const FString Trimmed = Value.TrimStartAndEnd();
		if (Trimmed.IsEmpty())
		{
			OutValue.Reset();
			return false;
		}

		const FTCHARToUTF8 Utf8(*Trimmed);
		UErrorCode Error = U_ZERO_ERROR;
		int32 Utf16Length = 0;
		u_strFromUTF8(
			nullptr,
			0,
			&Utf16Length,
			Utf8.Get(),
			Utf8.Length(),
			&Error);
		if (Error != U_BUFFER_OVERFLOW_ERROR && U_FAILURE(Error))
		{
			OutValue.Reset();
			return false;
		}

		Error = U_ZERO_ERROR;
		TArray<UChar> Utf16;
		Utf16.SetNumUninitialized(Utf16Length);
		u_strFromUTF8(
			Utf16.GetData(),
			Utf16Length,
			nullptr,
			Utf8.Get(),
			Utf8.Length(),
			&Error);
		if (U_FAILURE(Error))
		{
			OutValue.Reset();
			return false;
		}

		Error = U_ZERO_ERROR;
		const UNormalizer2* Normalizer = unorm2_getNFCInstance(&Error);
		if (U_FAILURE(Error) || Normalizer == nullptr)
		{
			OutValue.Reset();
			return false;
		}
		const int32 NormalizedLength = unorm2_normalize(
			Normalizer,
			Utf16.GetData(),
			Utf16Length,
			nullptr,
			0,
			&Error);
		if (Error != U_BUFFER_OVERFLOW_ERROR && U_FAILURE(Error))
		{
			OutValue.Reset();
			return false;
		}

		Error = U_ZERO_ERROR;
		TArray<UChar> Normalized;
		Normalized.SetNumUninitialized(NormalizedLength);
		unorm2_normalize(
			Normalizer,
			Utf16.GetData(),
			Utf16Length,
			Normalized.GetData(),
			NormalizedLength,
			&Error);
		if (U_FAILURE(Error))
		{
			OutValue.Reset();
			return false;
		}

		Error = U_ZERO_ERROR;
		int32 NormalizedUtf8Length = 0;
		u_strToUTF8(
			nullptr,
			0,
			&NormalizedUtf8Length,
			Normalized.GetData(),
			NormalizedLength,
			&Error);
		if (Error != U_BUFFER_OVERFLOW_ERROR && U_FAILURE(Error))
		{
			OutValue.Reset();
			return false;
		}

		Error = U_ZERO_ERROR;
		TArray<ANSICHAR> NormalizedUtf8;
		NormalizedUtf8.SetNumUninitialized(NormalizedUtf8Length + 1);
		u_strToUTF8(
			NormalizedUtf8.GetData(),
			NormalizedUtf8Length + 1,
			nullptr,
			Normalized.GetData(),
			NormalizedLength,
			&Error);
		if (U_FAILURE(Error))
		{
			OutValue.Reset();
			return false;
		}
		NormalizedUtf8[NormalizedUtf8Length] = '\0';
		OutValue = UTF8_TO_TCHAR(NormalizedUtf8.GetData());
		return !OutValue.IsEmpty();
	}
}
