#pragma once

#include "CoreMinimal.h"
#include "Misc/Crc.h"

namespace UE::UAI::StateSemantic
{
	// Text identities are distinct from Unreal's case-insensitive FString/FName keys.
	struct FExactStringKeyFuncs : DefaultKeyFuncs<FString>
	{
		static bool Matches(KeyInitType A, KeyInitType B)
		{
			return A.Equals(B, ESearchCase::CaseSensitive);
		}
		static uint32 GetKeyHash(KeyInitType Key)
		{
			return FCrc::StrCrc32(*Key);
		}
	};

	template <typename ValueType>
	struct TExactStringMapKeyFuncs : TDefaultMapHashableKeyFuncs<FString, ValueType, false>
	{
		static bool Matches(const FString& A, const FString& B)
		{
			return A.Equals(B, ESearchCase::CaseSensitive);
		}
		static uint32 GetKeyHash(const FString& Key)
		{
			return FCrc::StrCrc32(*Key);
		}
	};
}
