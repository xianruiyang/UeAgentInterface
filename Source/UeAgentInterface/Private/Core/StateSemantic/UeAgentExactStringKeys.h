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
		// Borrowed parser keys need the same exact FString contract without
		// materializing every successful key. Preserve its embedded-NUL edge.
		static bool MatchesView(FStringView A, FStringView B)
		{
			for (TCHAR Ch : A) if (Ch == TCHAR(0)) return Matches(FString(A), FString(B));
			for (TCHAR Ch : B) if (Ch == TCHAR(0)) return Matches(FString(A), FString(B));
			return A.Equals(B, ESearchCase::CaseSensitive);
		}
		static uint32 GetViewKeyHash(FStringView Key)
		{
			int32 Length = 0;
			while (Length < Key.Len() && Key[Length] != TCHAR(0)) ++Length;
			return FCrc::StrCrc32Len(Key.GetData(), Length);
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
