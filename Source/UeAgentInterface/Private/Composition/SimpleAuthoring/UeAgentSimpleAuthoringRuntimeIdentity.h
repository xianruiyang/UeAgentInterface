#pragma once

#include "CoreMinimal.h"

#include <openssl/sha.h>

namespace UE::UAI::Composition::SimpleAuthoring::RuntimePrivate
{
	inline FString Sha256(const FString& Text)
	{
		const FTCHARToUTF8 Utf8(*Text);
		uint8 Digest[SHA256_DIGEST_LENGTH];
		SHA256(
			reinterpret_cast<const uint8*>(Utf8.Get()),
			Utf8.Length(),
			Digest);
		FString Hex;
		Hex.Reserve(SHA256_DIGEST_LENGTH * 2);
		for (const uint8 Byte : Digest)
		{
			Hex += FString::Printf(TEXT("%02x"), Byte);
		}
		return TEXT("sha256:") + Hex;
	}

	inline FString EncodeUriSegment(const FString& Value)
	{
		const FTCHARToUTF8 Utf8(*Value);
		FString Result;
		for (int32 Index = 0; Index < Utf8.Length(); ++Index)
		{
			const uint8 Byte = static_cast<uint8>(Utf8.Get()[Index]);
			const bool bUnreserved =
				(Byte >= 'A' && Byte <= 'Z')
				|| (Byte >= 'a' && Byte <= 'z')
				|| (Byte >= '0' && Byte <= '9')
				|| Byte == '-'
				|| Byte == '_'
				|| Byte == '.'
				|| Byte == '~';
			Result += bUnreserved
				? FString::Chr(static_cast<TCHAR>(Byte))
				: FString::Printf(TEXT("%%%02X"), Byte);
		}
		return Result;
	}
}
