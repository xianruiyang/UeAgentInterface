#pragma once

#include "CoreTypes.h"
#include "Containers/UnrealString.h"
#include "Containers/StringView.h"

namespace UE::UAI::HjsonWorkspace::PointerText
{
	// Preserve the two existing callers' embedded-NUL behavior explicitly.
	enum class ENulMode : uint8 { ReplaceCompatible, Omit };

	inline void AppendEscaped(FString& Out, FStringView Value, ENulMode NulMode)
	{
		int32 RunStart = 0, NullInput = INDEX_NONE;
		bool bReplaced = false;
		for (int32 Index = 0; Index < Value.Len(); ++Index)
		{
			const TCHAR Character = Value[Index];
			if (Character == TCHAR(0))
			{
				if (NulMode == ENulMode::Omit) {
					if (Index > RunStart) Out.AppendChars(Value.GetData() + RunStart, Index - RunStart);
					RunStart = Index + 1;
					continue;
				}
				if (NullInput == INDEX_NONE) {
					if (bReplaced) { if (Index > RunStart) Out.AppendChars(Value.GetData() + RunStart, Index - RunStart); return; }
					NullInput = Index;
				}
			}
			if (Character != TEXT('~') && Character != TEXT('/')) continue;
			if (NullInput != INDEX_NONE) { if (NullInput > RunStart) Out.AppendChars(Value.GetData() + RunStart, NullInput - RunStart); return; }
			if (Index > RunStart) Out.AppendChars(Value.GetData() + RunStart, Index - RunStart);
			Out.AppendChars(Character == TEXT('~') ? TEXT("~0") : TEXT("~1"), 2);
			RunStart = Index + 1;
			bReplaced = true;
		}
		if (Value.Len() > RunStart) Out.AppendChars(Value.GetData() + RunStart, Value.Len() - RunStart);
	}

	inline FString Child(FStringView Parent, FStringView Segment, ENulMode NulMode, int64 KnownLength = -1)
	{
		// FString's existing literal comparison stops at an embedded NUL.
		const bool bRoot = Parent.Len() > 0 && Parent[0] == TEXT('/') && (Parent.Len() == 1 || Parent[1] == TCHAR(0));
		const int64 ReserveLength = KnownLength >= 0 ? KnownLength : int64(bRoot ? 0 : Parent.Len()) + 1 + Segment.Len();
		FString Result;
		if (ReserveLength > 0 && ReserveLength <= MAX_int32) Result.Reserve(static_cast<int32>(ReserveLength));
		if (!bRoot && Parent.Len() > 0) Result.AppendChars(Parent.GetData(), Parent.Len());
		Result.AppendChar(TEXT('/'));
		AppendEscaped(Result, Segment, NulMode);
		return Result;
	}
}
