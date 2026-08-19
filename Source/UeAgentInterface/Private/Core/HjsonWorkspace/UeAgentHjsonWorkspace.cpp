#include "Core/HjsonWorkspace/UeAgentHjsonWorkspace.h"

#include "HAL/FileManager.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"

namespace UE::UAI::HjsonWorkspace
{
	using namespace UE::UAI::ContextResource;
	using namespace UE::UAI::StateSemantic;

	namespace
	{
		enum class ETokenKind : uint8
		{
			OpenObject,
			CloseObject,
			OpenArray,
			CloseArray,
			Colon,
			Comma,
			Bare,
			String,
		};

		struct FToken
		{
			ETokenKind Kind = ETokenKind::Bare;
			FString Lexeme;
			FString Value;
			int32 Start = 0;
			int32 End = 0;
			int64 ByteStart = 0;
			int64 ByteEnd = 0;
			int32 Line = 1;
			int32 Column = 1;
			bool bNewlineBefore = false;
		};

		enum class EFrameState : uint8
		{
			KeyOrEnd,
			Colon,
			ValueOrEnd,
			AwaitingValue,
			AfterValue,
		};

		struct FParseFrame
		{
			bool bObject = false;
			FString Pointer;
			FString OriginalKey;
			FToken StartToken;
			EFrameState State = EFrameState::ValueOrEnd;
			TArray<TPair<FString, FHjsonNodeRef>> Entries;
			TArray<FHjsonNodeRef> Items;
			TSet<FString> Keys;
			FString PendingKey;
		};

		struct FSourceText
		{
			FString UnitId;
			FString Text;
		};

		FString NormalizeRequired(FString Value)
		{
			Value.TrimStartAndEndInline();
			return Value;
		}

		FString EscapePointerSegment(FString Value)
		{
			Value.ReplaceInline(TEXT("~"), TEXT("~0"));
			Value.ReplaceInline(TEXT("/"), TEXT("~1"));
			return Value;
		}

		FString JoinPointer(const FString& Parent, const FString& Segment)
		{
			const FString Escaped = EscapePointerSegment(Segment);
			return Parent == TEXT("/")
				? TEXT("/") + Escaped
				: Parent + TEXT("/") + Escaped;
		}

		bool Charge(
			FUeAgentResourceTracker& Resources,
			const TMap<EUeAgentResourceDimension, int64>& Amounts,
			const FString& Path,
			FWorkspaceError& OutError)
		{
			FUeAgentResourceBudgetExceeded BudgetError;
			if (Resources.TryChargeMany(Amounts, Path, BudgetError))
			{
				return true;
			}
			OutError.Set(
				BudgetError.Code,
				BudgetError.Path,
				TEXT("/"),
				BudgetError.Observed,
				BudgetError.bStoppedEarly);
			return false;
		}

		bool ChargeNode(
			FUeAgentResourceTracker& Resources,
			const FString& Path,
			int64 StringBytes,
			int64 Items,
			FWorkspaceError& OutError)
		{
			return Charge(
				Resources,
				{
					{EUeAgentResourceDimension::Nodes, 1},
					{EUeAgentResourceDimension::Items, Items},
					{EUeAgentResourceDimension::StringBytes, StringBytes},
					{EUeAgentResourceDimension::WorkUnits, 1 + Items},
					{EUeAgentResourceDimension::EstimatedMemoryBytes,
						80 + StringBytes + Items * 16},
				},
				Path,
				OutError);
		}

		TArray<int64> BuildUtf8Offsets(const FString& Text)
		{
			TArray<int64> Offsets;
			Offsets.SetNumZeroed(Text.Len() + 1);
			int64 Bytes = 0;
			for (int32 Index = 0; Index < Text.Len(); ++Index)
			{
				Offsets[Index] = Bytes;
				const uint32 Character = static_cast<uint32>(Text[Index]);
				if (Character >= 0xD800 && Character <= 0xDBFF
					&& Index + 1 < Text.Len())
				{
					const uint32 Low = static_cast<uint32>(Text[Index + 1]);
					if (Low >= 0xDC00 && Low <= 0xDFFF)
					{
						Bytes += 4;
						Offsets[++Index] = Bytes;
						continue;
					}
				}
				Bytes += Character <= 0x7F
					? 1
					: Character <= 0x7FF
						? 2
						: 3;
			}
			Offsets[Text.Len()] = Bytes;
			return Offsets;
		}

		bool IsPunctuation(TCHAR Character)
		{
			return Character == TEXT('{')
				|| Character == TEXT('}')
				|| Character == TEXT('[')
				|| Character == TEXT(']')
				|| Character == TEXT(':')
				|| Character == TEXT(',');
		}

		ETokenKind PunctuationKind(TCHAR Character)
		{
			switch (Character)
			{
			case TEXT('{'): return ETokenKind::OpenObject;
			case TEXT('}'): return ETokenKind::CloseObject;
			case TEXT('['): return ETokenKind::OpenArray;
			case TEXT(']'): return ETokenKind::CloseArray;
			case TEXT(':'): return ETokenKind::Colon;
			default: return ETokenKind::Comma;
			}
		}

		bool ScanTokens(
			const FString& Text,
			const FString& SourceUnit,
			FUeAgentResourceTracker& Resources,
			TArray<FToken>& OutTokens,
			FWorkspaceError& OutError)
		{
			OutTokens.Reset();
			const TArray<int64> Utf8Offsets = BuildUtf8Offsets(Text);
			int32 Index = 0;
			int32 Line = 1;
			int32 Column = 1;
			bool bNewlineBefore = false;
			auto Advance = [&]() -> TCHAR
			{
				const TCHAR Character = Text[Index++];
				if (Character == TEXT('\r'))
				{
					if (Index < Text.Len() && Text[Index] == TEXT('\n'))
					{
						++Index;
					}
					++Line;
					Column = 1;
					bNewlineBefore = true;
					return TEXT('\n');
				}
				if (Character == TEXT('\n'))
				{
					++Line;
					Column = 1;
					bNewlineBefore = true;
				}
				else
				{
					++Column;
				}
				return Character;
			};

			while (Index < Text.Len())
			{
				const TCHAR Character = Text[Index];
				if (FChar::IsWhitespace(Character))
				{
					Advance();
					continue;
				}
				if (Character == TEXT('#')
					|| (Character == TEXT('/')
						&& Index + 1 < Text.Len()
						&& Text[Index + 1] == TEXT('/')))
				{
					while (Index < Text.Len()
						&& Text[Index] != TEXT('\r')
						&& Text[Index] != TEXT('\n'))
					{
						Advance();
					}
					continue;
				}
				if (Character == TEXT('/')
					&& Index + 1 < Text.Len()
					&& Text[Index + 1] == TEXT('*'))
				{
					Advance();
					Advance();
					bool bClosed = false;
					while (Index < Text.Len())
					{
						if (Text[Index] == TEXT('*')
							&& Index + 1 < Text.Len()
							&& Text[Index + 1] == TEXT('/'))
						{
							Advance();
							Advance();
							bClosed = true;
							break;
						}
						Advance();
					}
					if (!bClosed)
					{
						OutError.Set(TEXT("hjson_comment_unclosed"), SourceUnit);
						return false;
					}
					continue;
				}

				const int32 Start = Index;
				const int32 TokenLine = Line;
				const int32 TokenColumn = Column;
				const bool bTokenNewline = bNewlineBefore;
				bNewlineBefore = false;
				if (IsPunctuation(Character))
				{
					Advance();
					OutTokens.Add({
						PunctuationKind(Character),
						FString::Chr(Character),
						FString::Chr(Character),
						Start,
						Index,
						Utf8Offsets[Start],
						Utf8Offsets[Index],
						TokenLine,
						TokenColumn,
						bTokenNewline,
					});
					continue;
				}
				if (Character == TEXT('"') || Character == TEXT('\''))
				{
					const TCHAR Quote = Character;
					Advance();
					FString Value;
					bool bClosed = false;
					while (Index < Text.Len())
					{
						const TCHAR Current = Advance();
						if (Current == Quote)
						{
							bClosed = true;
							break;
						}
						if (Current != TEXT('\\'))
						{
							Value.AppendChar(Current);
							continue;
						}
						if (Index >= Text.Len())
						{
							break;
						}
						const TCHAR Escaped = Advance();
						switch (Escaped)
						{
						case TEXT('"'): Value.AppendChar(TEXT('"')); break;
						case TEXT('\''): Value.AppendChar(TEXT('\'')); break;
						case TEXT('\\'): Value.AppendChar(TEXT('\\')); break;
						case TEXT('/'): Value.AppendChar(TEXT('/')); break;
						case TEXT('b'): Value.AppendChar(TEXT('\b')); break;
						case TEXT('f'): Value.AppendChar(TEXT('\f')); break;
						case TEXT('n'): Value.AppendChar(TEXT('\n')); break;
						case TEXT('r'): Value.AppendChar(TEXT('\r')); break;
						case TEXT('t'): Value.AppendChar(TEXT('\t')); break;
						case TEXT('u'):
						{
							if (Index + 4 > Text.Len())
							{
								OutError.Set(
									TEXT("hjson_unicode_escape_invalid"),
									SourceUnit);
								return false;
							}
							uint32 Code = 0;
							for (int32 DigitIndex = 0; DigitIndex < 4; ++DigitIndex)
							{
								const TCHAR Digit = Advance();
								if (!FChar::IsHexDigit(Digit))
								{
									OutError.Set(
										TEXT("hjson_unicode_escape_invalid"),
										SourceUnit);
									return false;
								}
								Code = Code * 16
									+ (FChar::IsDigit(Digit)
										? Digit - TEXT('0')
										: FChar::ToUpper(Digit) - TEXT('A') + 10);
							}
							Value.AppendChar(static_cast<TCHAR>(Code));
							break;
						}
						default:
							OutError.Set(TEXT("hjson_escape_invalid"), SourceUnit);
							return false;
						}
					}
					if (!bClosed)
					{
						OutError.Set(TEXT("hjson_string_unclosed"), SourceUnit);
						return false;
					}
					OutTokens.Add({
						ETokenKind::String,
						Text.Mid(Start, Index - Start),
						MoveTemp(Value),
						Start,
						Index,
						Utf8Offsets[Start],
						Utf8Offsets[Index],
						TokenLine,
						TokenColumn,
						bTokenNewline,
					});
					continue;
				}

				while (Index < Text.Len())
				{
					const TCHAR Current = Text[Index];
					if (FChar::IsWhitespace(Current) || IsPunctuation(Current))
					{
						break;
					}
					if (Current == TEXT('#')
						|| (Current == TEXT('/')
							&& Index + 1 < Text.Len()
							&& (Text[Index + 1] == TEXT('/')
								|| Text[Index + 1] == TEXT('*'))))
					{
						break;
					}
					Advance();
				}
				if (Index == Start)
				{
					OutError.Set(TEXT("hjson_token_invalid"), SourceUnit);
					return false;
				}
				const FString Lexeme = Text.Mid(Start, Index - Start);
				OutTokens.Add({
					ETokenKind::Bare,
					Lexeme,
					Lexeme,
					Start,
					Index,
					Utf8Offsets[Start],
					Utf8Offsets[Index],
					TokenLine,
					TokenColumn,
					bTokenNewline,
				});
			}
			return Charge(
				Resources,
				{
					{EUeAgentResourceDimension::Items, OutTokens.Num()},
					{EUeAgentResourceDimension::WorkUnits, OutTokens.Num()},
					{EUeAgentResourceDimension::EstimatedMemoryBytes,
						OutTokens.Num() * 64ll},
				},
				TEXT("hjson-tokenize:") + SourceUnit,
				OutError);
		}

		bool IsIntegerLexeme(const FString& Value)
		{
			if (Value.IsEmpty())
			{
				return false;
			}
			int32 Index = Value[0] == TEXT('-') ? 1 : 0;
			if (Index >= Value.Len())
			{
				return false;
			}
			if (Value[Index] == TEXT('0') && Index + 1 < Value.Len())
			{
				return false;
			}
			for (; Index < Value.Len(); ++Index)
			{
				if (!FChar::IsDigit(Value[Index]))
				{
					return false;
				}
			}
			return true;
		}

		bool IsDecimalLexeme(const FString& Value)
		{
			if (Value.IsEmpty())
			{
				return false;
			}
			int32 Index = Value[0] == TEXT('-') ? 1 : 0;
			bool bDigitBeforeDot = false;
			bool bDot = false;
			bool bDigitAfterDot = false;
			bool bExponent = false;
			bool bExponentDigit = false;
			for (; Index < Value.Len(); ++Index)
			{
				const TCHAR Character = Value[Index];
				if (FChar::IsDigit(Character))
				{
					if (bExponent)
					{
						bExponentDigit = true;
					}
					else if (bDot)
					{
						bDigitAfterDot = true;
					}
					else
					{
						bDigitBeforeDot = true;
					}
					continue;
				}
				if (Character == TEXT('.') && !bDot && !bExponent)
				{
					bDot = true;
					continue;
				}
				if ((Character == TEXT('e') || Character == TEXT('E'))
					&& !bExponent && bDigitBeforeDot
					&& (!bDot || bDigitAfterDot))
				{
					bExponent = true;
					if (Index + 1 < Value.Len()
						&& (Value[Index + 1] == TEXT('+')
							|| Value[Index + 1] == TEXT('-')))
					{
						++Index;
					}
					continue;
				}
				return false;
			}
			return bDigitBeforeDot
				&& ((bDot && bDigitAfterDot && !bExponent)
					|| (bExponent && bExponentDigit));
		}

		FSourceProvenance MakeProvenance(
			const FToken& Token,
			const FString& SourceUnit,
			const FString& Pointer,
			const FString& OriginalKey)
		{
			return FSourceProvenance(
				SourceUnit,
				Pointer,
				Token.Line,
				Token.Column,
				Token.ByteStart,
				Token.ByteEnd,
				OriginalKey);
		}

		bool MakeScalarNode(
			const FToken& Token,
			const FString& SourceUnit,
			const FString& Pointer,
			const FString& OriginalKey,
			FUeAgentResourceTracker& Resources,
			FHjsonNodePtr& OutNode,
			FWorkspaceError& OutError)
		{
			if (Token.Kind != ETokenKind::Bare
				&& Token.Kind != ETokenKind::String)
			{
				OutError.Set(TEXT("hjson_value_expected"), SourceUnit, Pointer);
				return false;
			}
			EHjsonNodeKind Kind = EHjsonNodeKind::String;
			FString Text = Token.Value;
			bool bBoolean = false;
			const FString Folded = Token.Value.ToLower();
			if (Token.Kind == ETokenKind::Bare && Folded == TEXT("null"))
			{
				Kind = EHjsonNodeKind::Null;
				Text.Reset();
			}
			else if (Token.Kind == ETokenKind::Bare
				&& (Folded == TEXT("true") || Folded == TEXT("false")))
			{
				Kind = EHjsonNodeKind::Boolean;
				bBoolean = Folded == TEXT("true");
				Text.Reset();
			}
			else if (Token.Kind == ETokenKind::Bare
				&& (Folded == TEXT("nan")
					|| Folded == TEXT("+nan")
					|| Folded == TEXT("-nan")
					|| Folded == TEXT("infinity")
					|| Folded == TEXT("+infinity")
					|| Folded == TEXT("-infinity")))
			{
				OutError.Set(
					TEXT("hjson_nonfinite_number"),
					SourceUnit,
					Pointer);
				return false;
			}
			else if (Token.Kind == ETokenKind::Bare
				&& IsIntegerLexeme(Token.Value))
			{
				Kind = EHjsonNodeKind::Integer;
				Text.Reset();
			}
			else if (Token.Kind == ETokenKind::Bare
				&& IsDecimalLexeme(Token.Value))
			{
				Kind = EHjsonNodeKind::Decimal;
				Text.Reset();
			}
			const int64 StringBytes = Token.ByteEnd - Token.ByteStart;
			if (!ChargeNode(Resources, Pointer, StringBytes, 0, OutError))
			{
				return false;
			}
			OutNode = MakeShared<FHjsonNode>(
				Kind,
				MakeProvenance(
					Token,
					SourceUnit,
					Pointer,
					OriginalKey),
				Token.Lexeme,
				MoveTemp(Text),
				bBoolean,
				TArray<FHjsonNodeRef>{},
				TArray<TPair<FString, FHjsonNodeRef>>{});
			return true;
		}

		FString UnescapePointerSegment(FString Value)
		{
			Value.ReplaceInline(TEXT("~1"), TEXT("/"));
			Value.ReplaceInline(TEXT("~0"), TEXT("~"));
			return Value;
		}

		bool IsBareKey(const FString& Value)
		{
			if (Value.IsEmpty()
				|| (!FChar::IsAlpha(Value[0]) && Value[0] != TEXT('_')))
			{
				return false;
			}
			for (const TCHAR Character : Value)
			{
				if (!FChar::IsAlnum(Character)
					&& Character != TEXT('_')
					&& Character != TEXT('.')
					&& Character != TEXT('-'))
				{
					return false;
				}
			}
			return true;
		}

		bool IsBareString(const FString& Value)
		{
			if (Value.IsEmpty()
				|| (!FChar::IsAlpha(Value[0]) && Value[0] != TEXT('_')))
			{
				return false;
			}
			for (const TCHAR Character : Value)
			{
				if (!FChar::IsAlnum(Character)
					&& Character != TEXT('_')
					&& Character != TEXT('/')
					&& Character != TEXT('-'))
				{
					return false;
				}
			}
			const FString Folded = Value.ToLower();
			return Folded != TEXT("true")
				&& Folded != TEXT("false")
				&& Folded != TEXT("null")
				&& Folded != TEXT("nan")
				&& Folded != TEXT("infinity");
		}

		FString QuoteString(const FString& Value)
		{
			FString Result = TEXT("\"");
			for (const TCHAR Character : Value)
			{
				switch (Character)
				{
				case TEXT('"'): Result += TEXT("\\\""); break;
				case TEXT('\\'): Result += TEXT("\\\\"); break;
				case TEXT('\b'): Result += TEXT("\\b"); break;
				case TEXT('\f'): Result += TEXT("\\f"); break;
				case TEXT('\n'): Result += TEXT("\\n"); break;
				case TEXT('\r'): Result += TEXT("\\r"); break;
				case TEXT('\t'): Result += TEXT("\\t"); break;
				default: Result.AppendChar(Character); break;
				}
			}
			Result += TEXT("\"");
			return Result;
		}

		FString RenderScalar(const FHjsonNode& Node)
		{
			switch (Node.GetKind())
			{
			case EHjsonNodeKind::Null:
				return TEXT("null");
			case EHjsonNodeKind::Boolean:
				return Node.GetBoolean() ? TEXT("true") : TEXT("false");
			case EHjsonNodeKind::Integer:
			case EHjsonNodeKind::Decimal:
				return Node.GetLexeme();
			default:
				if (!Node.GetLexeme().IsEmpty()
					&& Node.GetLexeme()[0] != TEXT('"')
					&& Node.GetLexeme()[0] != TEXT('\'')
					&& IsBareString(Node.GetText()))
				{
					return Node.GetText();
				}
				return QuoteString(Node.GetText());
			}
		}

		FString HashBytes(const TArray<uint8>& Bytes)
		{
			static constexpr uint32 RoundConstants[64] = {
				0x428a2f98u, 0x71374491u, 0xb5c0fbcfu, 0xe9b5dba5u,
				0x3956c25bu, 0x59f111f1u, 0x923f82a4u, 0xab1c5ed5u,
				0xd807aa98u, 0x12835b01u, 0x243185beu, 0x550c7dc3u,
				0x72be5d74u, 0x80deb1feu, 0x9bdc06a7u, 0xc19bf174u,
				0xe49b69c1u, 0xefbe4786u, 0x0fc19dc6u, 0x240ca1ccu,
				0x2de92c6fu, 0x4a7484aau, 0x5cb0a9dcu, 0x76f988dau,
				0x983e5152u, 0xa831c66du, 0xb00327c8u, 0xbf597fc7u,
				0xc6e00bf3u, 0xd5a79147u, 0x06ca6351u, 0x14292967u,
				0x27b70a85u, 0x2e1b2138u, 0x4d2c6dfcu, 0x53380d13u,
				0x650a7354u, 0x766a0abbu, 0x81c2c92eu, 0x92722c85u,
				0xa2bfe8a1u, 0xa81a664bu, 0xc24b8b70u, 0xc76c51a3u,
				0xd192e819u, 0xd6990624u, 0xf40e3585u, 0x106aa070u,
				0x19a4c116u, 0x1e376c08u, 0x2748774cu, 0x34b0bcb5u,
				0x391c0cb3u, 0x4ed8aa4au, 0x5b9cca4fu, 0x682e6ff3u,
				0x748f82eeu, 0x78a5636fu, 0x84c87814u, 0x8cc70208u,
				0x90befffau, 0xa4506cebu, 0xbef9a3f7u, 0xc67178f2u,
			};
			auto RotateRight = [](uint32 Value, uint32 Count)
			{
				return (Value >> Count) | (Value << (32u - Count));
			};

			const uint64 BitLength =
				static_cast<uint64>(Bytes.Num()) * 8ull;
			TArray<uint8> Padded = Bytes;
			Padded.Add(0x80u);
			while ((Padded.Num() % 64) != 56)
			{
				Padded.Add(0u);
			}
			for (int32 Shift = 56; Shift >= 0; Shift -= 8)
			{
				Padded.Add(
					static_cast<uint8>((BitLength >> Shift) & 0xffu));
			}

			uint32 State[8] = {
				0x6a09e667u,
				0xbb67ae85u,
				0x3c6ef372u,
				0xa54ff53au,
				0x510e527fu,
				0x9b05688cu,
				0x1f83d9abu,
				0x5be0cd19u,
			};
			for (
				int32 ChunkOffset = 0;
				ChunkOffset < Padded.Num();
				ChunkOffset += 64)
			{
				uint32 Words[64]{};
				for (int32 Index = 0; Index < 16; ++Index)
				{
					const int32 Offset = ChunkOffset + (Index * 4);
					Words[Index] =
						(static_cast<uint32>(Padded[Offset]) << 24)
						| (static_cast<uint32>(Padded[Offset + 1]) << 16)
						| (static_cast<uint32>(Padded[Offset + 2]) << 8)
						| static_cast<uint32>(Padded[Offset + 3]);
				}
				for (int32 Index = 16; Index < 64; ++Index)
				{
					const uint32 Sigma0 =
						RotateRight(Words[Index - 15], 7)
						^ RotateRight(Words[Index - 15], 18)
						^ (Words[Index - 15] >> 3);
					const uint32 Sigma1 =
						RotateRight(Words[Index - 2], 17)
						^ RotateRight(Words[Index - 2], 19)
						^ (Words[Index - 2] >> 10);
					Words[Index] = Words[Index - 16]
						+ Sigma0
						+ Words[Index - 7]
						+ Sigma1;
				}

				uint32 A = State[0];
				uint32 B = State[1];
				uint32 C = State[2];
				uint32 D = State[3];
				uint32 E = State[4];
				uint32 F = State[5];
				uint32 G = State[6];
				uint32 H = State[7];
				for (int32 Index = 0; Index < 64; ++Index)
				{
					const uint32 UpperSigma1 =
						RotateRight(E, 6)
						^ RotateRight(E, 11)
						^ RotateRight(E, 25);
					const uint32 Choice = (E & F) ^ ((~E) & G);
					const uint32 Temp1 =
						H
						+ UpperSigma1
						+ Choice
						+ RoundConstants[Index]
						+ Words[Index];
					const uint32 UpperSigma0 =
						RotateRight(A, 2)
						^ RotateRight(A, 13)
						^ RotateRight(A, 22);
					const uint32 Majority =
						(A & B) ^ (A & C) ^ (B & C);
					const uint32 Temp2 = UpperSigma0 + Majority;
					H = G;
					G = F;
					F = E;
					E = D + Temp1;
					D = C;
					C = B;
					B = A;
					A = Temp1 + Temp2;
				}
				State[0] += A;
				State[1] += B;
				State[2] += C;
				State[3] += D;
				State[4] += E;
				State[5] += F;
				State[6] += G;
				State[7] += H;
			}
			return FString::Printf(
				TEXT(
					"sha256:%08x%08x%08x%08x"
					"%08x%08x%08x%08x"),
				State[0],
				State[1],
				State[2],
				State[3],
				State[4],
				State[5],
				State[6],
				State[7]);
		}

		TArray<uint8> ToUtf8(const FString& Text)
		{
			const FTCHARToUTF8 Converted(*Text);
			return TArray<uint8>(
				reinterpret_cast<const uint8*>(Converted.Get()),
				Converted.Length());
		}
	}

	const FString& FWorkspaceError::GetCode() const { return Code; }
	const FString& FWorkspaceError::GetSourceUnit() const { return SourceUnit; }
	const FString& FWorkspaceError::GetJsonPointer() const { return JsonPointer; }
	int64 FWorkspaceError::GetObserved() const { return Observed; }
	bool FWorkspaceError::IsStoppedEarly() const { return bStoppedEarly; }
	const TArray<FString>& FWorkspaceError::GetCyclePath() const { return CyclePath; }

	void FWorkspaceError::Reset()
	{
		Code.Reset();
		SourceUnit.Reset();
		JsonPointer = TEXT("/");
		Observed = 0;
		bStoppedEarly = false;
		CyclePath.Reset();
	}

	void FWorkspaceError::Set(
		FString InCode,
		FString InSourceUnit,
		FString InJsonPointer,
		int64 InObserved,
		bool bInStoppedEarly,
		TArray<FString> InCyclePath)
	{
		Code = MoveTemp(InCode);
		SourceUnit = MoveTemp(InSourceUnit);
		JsonPointer = InJsonPointer.IsEmpty()
			? TEXT("/")
			: MoveTemp(InJsonPointer);
		Observed = InObserved;
		bStoppedEarly = bInStoppedEarly;
		CyclePath = MoveTemp(InCyclePath);
	}

	FSourceProvenance::FSourceProvenance(
		FString InSourceUnit,
		FString InJsonPointer,
		int32 InLine,
		int32 InColumn,
		int64 InByteStart,
		int64 InByteEnd,
		FString InOriginalKey)
		: SourceUnit(MoveTemp(InSourceUnit))
		, JsonPointer(MoveTemp(InJsonPointer))
		, Line(InLine)
		, Column(InColumn)
		, ByteStart(InByteStart)
		, ByteEnd(InByteEnd)
		, OriginalKey(MoveTemp(InOriginalKey))
	{
	}

	const FString& FSourceProvenance::GetSourceUnit() const { return SourceUnit; }
	const FString& FSourceProvenance::GetJsonPointer() const { return JsonPointer; }
	int32 FSourceProvenance::GetLine() const { return Line; }
	int32 FSourceProvenance::GetColumn() const { return Column; }
	int64 FSourceProvenance::GetByteStart() const { return ByteStart; }
	int64 FSourceProvenance::GetByteEnd() const { return ByteEnd; }
	const FString& FSourceProvenance::GetOriginalKey() const { return OriginalKey; }

	FHjsonNode::FHjsonNode(
		EHjsonNodeKind InKind,
		FSourceProvenance InProvenance,
		FString InLexeme,
		FString InText,
		bool bInBoolean,
		TArray<FHjsonNodeRef> InItems,
		TArray<TPair<FString, FHjsonNodeRef>> InEntries)
		: Kind(InKind)
		, Provenance(MoveTemp(InProvenance))
		, Lexeme(MoveTemp(InLexeme))
		, Text(MoveTemp(InText))
		, bBoolean(bInBoolean)
		, Items(MoveTemp(InItems))
		, Entries(MoveTemp(InEntries))
	{
	}

	EHjsonNodeKind FHjsonNode::GetKind() const { return Kind; }
	const FSourceProvenance& FHjsonNode::GetProvenance() const { return Provenance; }
	const FString& FHjsonNode::GetLexeme() const { return Lexeme; }
	const FString& FHjsonNode::GetText() const { return Text; }
	bool FHjsonNode::GetBoolean() const { return bBoolean; }
	const TArray<FHjsonNodeRef>& FHjsonNode::GetItems() const { return Items; }
	const TArray<TPair<FString, FHjsonNodeRef>>& FHjsonNode::GetEntries() const
	{
		return Entries;
	}

	const FHjsonNode* FHjsonNode::Get(const FString& Key) const
	{
		for (const TPair<FString, FHjsonNodeRef>& Entry : Entries)
		{
			if (Entry.Key == Key)
			{
				return &Entry.Value.Get();
			}
		}
		return nullptr;
	}

	FHjsonDocument::FHjsonDocument(
		FString InSourceUnit,
		FHjsonNodeRef InRoot)
		: SourceUnit(MoveTemp(InSourceUnit))
		, Root(MoveTemp(InRoot))
	{
	}

	const FString& FHjsonDocument::GetSourceUnit() const { return SourceUnit; }
	const FHjsonNodeRef& FHjsonDocument::GetRoot() const { return Root; }

	const FHjsonNode* FHjsonDocument::Find(const FString& JsonPointer) const
	{
		if (JsonPointer.IsEmpty() || JsonPointer == TEXT("/"))
		{
			return &Root.Get();
		}
		if (!JsonPointer.StartsWith(TEXT("/")))
		{
			return nullptr;
		}
		TArray<FString> Segments;
		JsonPointer.Mid(1).ParseIntoArray(Segments, TEXT("/"), false);
		const FHjsonNode* Current = &Root.Get();
		for (FString Segment : Segments)
		{
			Segment = UnescapePointerSegment(MoveTemp(Segment));
			if (Current->GetKind() == EHjsonNodeKind::Object)
			{
				Current = Current->Get(Segment);
				if (!Current)
				{
					return nullptr;
				}
			}
			else if (Current->GetKind() == EHjsonNodeKind::Array)
			{
				int32 ArrayIndex = INDEX_NONE;
				if (!LexTryParseString(ArrayIndex, *Segment)
					|| !Current->GetItems().IsValidIndex(ArrayIndex))
				{
					return nullptr;
				}
				Current = &Current->GetItems()[ArrayIndex].Get();
			}
			else
			{
				return nullptr;
			}
		}
		return Current;
	}

	FWorkspaceSource::FWorkspaceSource(
		EWorkspaceSourceKind InKind,
		FString InPath,
		FString InText,
		FString InSourceUnit,
		FString InSourceId)
		: Kind(InKind)
		, Path(MoveTemp(InPath))
		, Text(MoveTemp(InText))
		, SourceUnit(MoveTemp(InSourceUnit))
		, SourceId(MoveTemp(InSourceId))
	{
	}

	FWorkspaceSource FWorkspaceSource::Directory(FString Path)
	{
		return FWorkspaceSource(
			EWorkspaceSourceKind::Directory,
			MoveTemp(Path),
			FString(),
			FString(),
			FString());
	}

	FWorkspaceSource FWorkspaceSource::SingleFile(FString Path)
	{
		return FWorkspaceSource(
			EWorkspaceSourceKind::SingleFile,
			MoveTemp(Path),
			FString(),
			FString(),
			FString());
	}

	FWorkspaceSource FWorkspaceSource::Inline(
		FString Text,
		FString SourceUnit,
		FString SourceId)
	{
		return FWorkspaceSource(
			EWorkspaceSourceKind::Inline,
			FString(),
			MoveTemp(Text),
			MoveTemp(SourceUnit),
			MoveTemp(SourceId));
	}

	EWorkspaceSourceKind FWorkspaceSource::GetKind() const { return Kind; }
	const FString& FWorkspaceSource::GetPath() const { return Path; }
	const FString& FWorkspaceSource::GetText() const { return Text; }
	const FString& FWorkspaceSource::GetSourceUnit() const { return SourceUnit; }
	const FString& FWorkspaceSource::GetSourceId() const { return SourceId; }

	FWorkspacePolicy::FWorkspacePolicy(
		FString InDialectId,
		FString InContractId,
		int32 InContractVersion,
		FString InProfileId,
		FString InEntryUnit,
		TArray<FString> InReferenceFields,
		int32 InMaxFiles)
		: DialectId(NormalizeRequired(MoveTemp(InDialectId)))
		, ContractId(NormalizeRequired(MoveTemp(InContractId)))
		, ContractVersion(InContractVersion)
		, ProfileId(NormalizeRequired(MoveTemp(InProfileId)))
		, EntryUnit(MoveTemp(InEntryUnit))
		, ReferenceFields(MoveTemp(InReferenceFields))
		, MaxFiles(InMaxFiles)
	{
		FPaths::NormalizeFilename(EntryUnit);
		FPaths::RemoveDuplicateSlashes(EntryUnit);
		bValid = !DialectId.IsEmpty()
			&& !ContractId.IsEmpty()
			&& ContractVersion > 0
			&& !ProfileId.IsEmpty()
			&& !EntryUnit.IsEmpty()
			&& MaxFiles > 0;
	}

	const FString& FWorkspacePolicy::GetDialectId() const { return DialectId; }
	const FString& FWorkspacePolicy::GetContractId() const { return ContractId; }
	int32 FWorkspacePolicy::GetContractVersion() const { return ContractVersion; }
	const FString& FWorkspacePolicy::GetProfileId() const { return ProfileId; }
	const FString& FWorkspacePolicy::GetEntryUnit() const { return EntryUnit; }
	const TArray<FString>& FWorkspacePolicy::GetReferenceFields() const
	{
		return ReferenceFields;
	}
	int32 FWorkspacePolicy::GetMaxFiles() const { return MaxFiles; }
	bool FWorkspacePolicy::IsValid() const { return bValid; }

	FWorkspaceUnit::FWorkspaceUnit(
		FString InUnitId,
		FHjsonDocumentRef InDocument,
		TArray<uint8> InCanonicalUtf8,
		FString InContentHash)
		: UnitId(MoveTemp(InUnitId))
		, Document(MoveTemp(InDocument))
		, CanonicalUtf8(MoveTemp(InCanonicalUtf8))
		, ContentHash(MoveTemp(InContentHash))
	{
	}

	const FString& FWorkspaceUnit::GetUnitId() const { return UnitId; }
	const FHjsonDocumentRef& FWorkspaceUnit::GetDocument() const { return Document; }
	const TArray<uint8>& FWorkspaceUnit::GetCanonicalUtf8() const
	{
		return CanonicalUtf8;
	}
	const FString& FWorkspaceUnit::GetContentHash() const { return ContentHash; }

	FReferenceEdge::FReferenceEdge(
		FString InSourceUnit,
		FString InJsonPointer,
		FString InTargetUnit)
		: SourceUnit(MoveTemp(InSourceUnit))
		, JsonPointer(MoveTemp(InJsonPointer))
		, TargetUnit(MoveTemp(InTargetUnit))
	{
	}

	const FString& FReferenceEdge::GetSourceUnit() const { return SourceUnit; }
	const FString& FReferenceEdge::GetJsonPointer() const { return JsonPointer; }
	const FString& FReferenceEdge::GetTargetUnit() const { return TargetUnit; }

	FCanonicalWorkspace::FCanonicalWorkspace(
		EWorkspaceSourceKind InSourceKind,
		FWorkspacePolicy InPolicy,
		TArray<FWorkspaceUnit> InUnits,
		TArray<FReferenceEdge> InReferenceEdges,
		FString InSemanticFingerprint)
		: SourceKind(InSourceKind)
		, Policy(MoveTemp(InPolicy))
		, Units(MoveTemp(InUnits))
		, ReferenceEdges(MoveTemp(InReferenceEdges))
		, SemanticFingerprint(MoveTemp(InSemanticFingerprint))
	{
	}

	EWorkspaceSourceKind FCanonicalWorkspace::GetSourceKind() const
	{
		return SourceKind;
	}
	const FWorkspacePolicy& FCanonicalWorkspace::GetPolicy() const { return Policy; }
	const TArray<FWorkspaceUnit>& FCanonicalWorkspace::GetUnits() const
	{
		return Units;
	}
	const TArray<FReferenceEdge>& FCanonicalWorkspace::GetReferenceEdges() const
	{
		return ReferenceEdges;
	}
	const FString& FCanonicalWorkspace::GetSemanticFingerprint() const
	{
		return SemanticFingerprint;
	}

	FExportPlanRequest::FExportPlanRequest(
		FUeAgentCanonicalSnapshotRef InSnapshot,
		EWorkspaceKind InWorkspaceKind,
		FString InEntryFile,
		FString InContractId,
		int32 InContractVersion,
		FString InProfileId,
		int64 InTokenBudget)
		: Snapshot(MoveTemp(InSnapshot))
		, WorkspaceKind(InWorkspaceKind)
		, EntryFile(MoveTemp(InEntryFile))
		, ContractId(NormalizeRequired(MoveTemp(InContractId)))
		, ContractVersion(InContractVersion)
		, ProfileId(NormalizeRequired(MoveTemp(InProfileId)))
		, TokenBudget(InTokenBudget)
	{
		FPaths::NormalizeFilename(EntryFile);
		FPaths::RemoveDuplicateSlashes(EntryFile);
		bValid = !EntryFile.IsEmpty()
			&& !ContractId.IsEmpty()
			&& ContractVersion > 0
			&& !ProfileId.IsEmpty()
			&& TokenBudget >= 0;
	}

	const FUeAgentCanonicalSnapshotRef& FExportPlanRequest::GetSnapshot() const
	{
		return Snapshot;
	}
	EWorkspaceKind FExportPlanRequest::GetWorkspaceKind() const { return WorkspaceKind; }
	const FString& FExportPlanRequest::GetEntryFile() const { return EntryFile; }
	const FString& FExportPlanRequest::GetContractId() const { return ContractId; }
	int32 FExportPlanRequest::GetContractVersion() const { return ContractVersion; }
	const FString& FExportPlanRequest::GetProfileId() const { return ProfileId; }
	int64 FExportPlanRequest::GetTokenBudget() const { return TokenBudget; }
	bool FExportPlanRequest::IsValid() const { return bValid; }

	FExportFilePlan::FExportFilePlan(
		FString InRelativePath,
		TArray<uint8> InCanonicalUtf8,
		FString InContentHash)
		: RelativePath(MoveTemp(InRelativePath))
		, CanonicalUtf8(MoveTemp(InCanonicalUtf8))
		, ContentHash(MoveTemp(InContentHash))
	{
	}

	const FString& FExportFilePlan::GetRelativePath() const { return RelativePath; }
	const TArray<uint8>& FExportFilePlan::GetCanonicalUtf8() const
	{
		return CanonicalUtf8;
	}
	const FString& FExportFilePlan::GetContentHash() const { return ContentHash; }

	FExportWorkspacePlan::FExportWorkspacePlan(
		EWorkspaceKind InWorkspaceKind,
		FString InEntryFile,
		FString InSnapshotId,
		FUeAgentStateRevision InSnapshotRevision,
		FString InSnapshotEvidenceId,
		TArray<FExportFilePlan> InFiles,
		FString InPlanFingerprint,
		int64 InObservedTokens)
		: WorkspaceKind(InWorkspaceKind)
		, EntryFile(MoveTemp(InEntryFile))
		, SnapshotId(MoveTemp(InSnapshotId))
		, SnapshotRevision(MoveTemp(InSnapshotRevision))
		, SnapshotEvidenceId(MoveTemp(InSnapshotEvidenceId))
		, Files(MoveTemp(InFiles))
		, PlanFingerprint(MoveTemp(InPlanFingerprint))
		, ObservedTokens(InObservedTokens)
	{
	}

	EWorkspaceKind FExportWorkspacePlan::GetWorkspaceKind() const
	{
		return WorkspaceKind;
	}
	const FString& FExportWorkspacePlan::GetEntryFile() const { return EntryFile; }
	const FString& FExportWorkspacePlan::GetSnapshotId() const { return SnapshotId; }
	const FUeAgentStateRevision& FExportWorkspacePlan::GetSnapshotRevision() const
	{
		return SnapshotRevision;
	}
	const FString& FExportWorkspacePlan::GetSnapshotEvidenceId() const
	{
		return SnapshotEvidenceId;
	}
	const TArray<FExportFilePlan>& FExportWorkspacePlan::GetFiles() const
	{
		return Files;
	}
	const FString& FExportWorkspacePlan::GetPlanFingerprint() const
	{
		return PlanFingerprint;
	}
	int64 FExportWorkspacePlan::GetObservedTokens() const { return ObservedTokens; }

	bool TryParseHjsonDocument(
		const FString& Text,
		const FString& SourceUnit,
		FUeAgentResourceTracker& Resources,
		FHjsonDocumentPtr& OutDocument,
		FWorkspaceError& OutError)
	{
		OutDocument.Reset();
		OutError.Reset();
		if (SourceUnit.IsEmpty())
		{
			OutError.Set(TEXT("hjson_source_unit_missing"));
			return false;
		}
		const FTCHARToUTF8 SourceUtf8(*Text);
		if (!Charge(
			Resources,
			{
				{EUeAgentResourceDimension::InputBytes, SourceUtf8.Length()},
				{EUeAgentResourceDimension::EstimatedMemoryBytes,
					SourceUtf8.Length() * 2ll},
				{EUeAgentResourceDimension::WorkUnits, 1},
			},
			TEXT("hjson:") + SourceUnit,
			OutError))
		{
			return false;
		}
		TArray<FToken> Tokens;
		if (!ScanTokens(Text, SourceUnit, Resources, Tokens, OutError))
		{
			return false;
		}
		if (Tokens.IsEmpty())
		{
			OutError.Set(TEXT("hjson_empty_document"), SourceUnit);
			return false;
		}

		int32 TokenIndex = 0;
		FHjsonNodePtr Root;
		TArray<FParseFrame> Frames;
		FString ExpectedPointer = TEXT("/");
		FString ExpectedKey;
		bool bExpectingValue = true;
		auto Attach = [&](FHjsonNodeRef Node) -> bool
		{
			if (Frames.IsEmpty())
			{
				if (Root.IsValid())
				{
					OutError.Set(
						TEXT("hjson_trailing_content"),
						SourceUnit);
					return false;
				}
				Root = Node;
				bExpectingValue = false;
				return true;
			}
			FParseFrame& Parent = Frames.Last();
			if (Parent.bObject)
			{
				Parent.Entries.Emplace(Parent.PendingKey, Node);
				Parent.PendingKey.Reset();
			}
			else
			{
				Parent.Items.Add(Node);
			}
			Parent.State = EFrameState::AfterValue;
			bExpectingValue = false;
			return true;
		};

		while (TokenIndex < Tokens.Num() || !Frames.IsEmpty())
		{
			if (bExpectingValue)
			{
				if (!Tokens.IsValidIndex(TokenIndex))
				{
					OutError.Set(
						TEXT("hjson_value_missing"),
						SourceUnit,
						ExpectedPointer);
					return false;
				}
				const FToken Token = Tokens[TokenIndex++];
				if (Token.Kind == ETokenKind::OpenObject
					|| Token.Kind == ETokenKind::OpenArray)
				{
					if (!ChargeNode(
						Resources,
						ExpectedPointer,
						0,
						0,
						OutError))
					{
						return false;
					}
					FParseFrame Frame;
					Frame.bObject = Token.Kind == ETokenKind::OpenObject;
					Frame.Pointer = ExpectedPointer;
					Frame.OriginalKey = ExpectedKey;
					Frame.StartToken = Token;
					Frame.State = Frame.bObject
						? EFrameState::KeyOrEnd
						: EFrameState::ValueOrEnd;
					Frames.Add(MoveTemp(Frame));
					bExpectingValue = false;
					continue;
				}
				FHjsonNodePtr Scalar;
				if (!MakeScalarNode(
					Token,
					SourceUnit,
					ExpectedPointer,
					ExpectedKey,
					Resources,
					Scalar,
					OutError)
					|| !Attach(Scalar.ToSharedRef()))
				{
					return false;
				}
				continue;
			}
			if (Frames.IsEmpty())
			{
				break;
			}
			if (!Tokens.IsValidIndex(TokenIndex))
			{
				OutError.Set(
					TEXT("hjson_container_unclosed"),
					SourceUnit,
					Frames.Last().Pointer);
				return false;
			}
			FParseFrame& Frame = Frames.Last();
			const FToken& Token = Tokens[TokenIndex];
			if (Frame.bObject)
			{
				if (Frame.State == EFrameState::KeyOrEnd)
				{
					if (Token.Kind == ETokenKind::CloseObject)
					{
						const FToken EndToken = Token;
						++TokenIndex;
						FParseFrame Closed = MoveTemp(Frames.Last());
						Frames.Pop(EAllowShrinking::No);
						int64 KeyBytes = 0;
						for (const TPair<FString, FHjsonNodeRef>& Entry
							: Closed.Entries)
						{
							KeyBytes += FTCHARToUTF8(*Entry.Key).Length();
						}
						if (!Charge(
							Resources,
							{
								{EUeAgentResourceDimension::Items,
									Closed.Entries.Num()},
								{EUeAgentResourceDimension::StringBytes,
									KeyBytes},
								{EUeAgentResourceDimension::WorkUnits,
									Closed.Entries.Num()},
								{EUeAgentResourceDimension::EstimatedMemoryBytes,
									Closed.Entries.Num() * 24ll},
							},
							Closed.Pointer,
							OutError))
						{
							return false;
						}
						FHjsonNodeRef Container = MakeShared<FHjsonNode>(
							EHjsonNodeKind::Object,
							FSourceProvenance(
								SourceUnit,
								Closed.Pointer,
								Closed.StartToken.Line,
								Closed.StartToken.Column,
								Closed.StartToken.ByteStart,
								EndToken.ByteEnd,
								Closed.OriginalKey),
							FString(),
							FString(),
							false,
							TArray<FHjsonNodeRef>{},
							MoveTemp(Closed.Entries));
						if (!Attach(MoveTemp(Container)))
						{
							return false;
						}
						continue;
					}
					if (Token.Kind != ETokenKind::Bare
						&& Token.Kind != ETokenKind::String)
					{
						OutError.Set(
							TEXT("hjson_object_key_expected"),
							SourceUnit,
							Frame.Pointer);
						return false;
					}
					if (Frame.Keys.Contains(Token.Value))
					{
						OutError.Set(
							TEXT("hjson_duplicate_key"),
							SourceUnit,
							JoinPointer(Frame.Pointer, Token.Value));
						return false;
					}
					Frame.Keys.Add(Token.Value);
					Frame.PendingKey = Token.Value;
					Frame.State = EFrameState::Colon;
					++TokenIndex;
					continue;
				}
				if (Frame.State == EFrameState::Colon)
				{
					if (Token.Kind != ETokenKind::Colon)
					{
						OutError.Set(
							TEXT("hjson_colon_expected"),
							SourceUnit,
							JoinPointer(Frame.Pointer, Frame.PendingKey));
						return false;
					}
					ExpectedPointer = JoinPointer(
						Frame.Pointer,
						Frame.PendingKey);
					ExpectedKey = Frame.PendingKey;
					Frame.State = EFrameState::AwaitingValue;
					++TokenIndex;
					bExpectingValue = true;
					continue;
				}
				if (Frame.State == EFrameState::AfterValue)
				{
					if (Token.Kind == ETokenKind::Comma)
					{
						++TokenIndex;
						Frame.State = EFrameState::KeyOrEnd;
						continue;
					}
					if (Token.Kind == ETokenKind::CloseObject
						|| Token.bNewlineBefore)
					{
						Frame.State = EFrameState::KeyOrEnd;
						continue;
					}
					OutError.Set(
						TEXT("hjson_object_separator_expected"),
						SourceUnit,
						Frame.Pointer);
					return false;
				}
			}
			else
			{
				if (Frame.State == EFrameState::ValueOrEnd)
				{
					if (Token.Kind == ETokenKind::CloseArray)
					{
						const FToken EndToken = Token;
						++TokenIndex;
						FParseFrame Closed = MoveTemp(Frames.Last());
						Frames.Pop(EAllowShrinking::No);
						if (!Charge(
							Resources,
							{
								{EUeAgentResourceDimension::Items,
									Closed.Items.Num()},
								{EUeAgentResourceDimension::WorkUnits,
									Closed.Items.Num()},
								{EUeAgentResourceDimension::EstimatedMemoryBytes,
									Closed.Items.Num() * 24ll},
							},
							Closed.Pointer,
							OutError))
						{
							return false;
						}
						FHjsonNodeRef Container = MakeShared<FHjsonNode>(
							EHjsonNodeKind::Array,
							FSourceProvenance(
								SourceUnit,
								Closed.Pointer,
								Closed.StartToken.Line,
								Closed.StartToken.Column,
								Closed.StartToken.ByteStart,
								EndToken.ByteEnd,
								Closed.OriginalKey),
							FString(),
							FString(),
							false,
							MoveTemp(Closed.Items),
							TArray<TPair<FString, FHjsonNodeRef>>{});
						if (!Attach(MoveTemp(Container)))
						{
							return false;
						}
						continue;
					}
					ExpectedPointer = JoinPointer(
						Frame.Pointer,
						::LexToString(Frame.Items.Num()));
					ExpectedKey.Reset();
					Frame.State = EFrameState::AwaitingValue;
					bExpectingValue = true;
					continue;
				}
				if (Frame.State == EFrameState::AfterValue)
				{
					if (Token.Kind == ETokenKind::Comma)
					{
						++TokenIndex;
						Frame.State = EFrameState::ValueOrEnd;
						continue;
					}
					if (Token.Kind == ETokenKind::CloseArray
						|| Token.bNewlineBefore)
					{
						Frame.State = EFrameState::ValueOrEnd;
						continue;
					}
					OutError.Set(
						TEXT("hjson_array_separator_expected"),
						SourceUnit,
						Frame.Pointer);
					return false;
				}
			}
		}
		if (!Root.IsValid())
		{
			OutError.Set(TEXT("hjson_document_missing_root"), SourceUnit);
			return false;
		}
		if (TokenIndex != Tokens.Num())
		{
			OutError.Set(TEXT("hjson_trailing_content"), SourceUnit);
			return false;
		}
		OutDocument = MakeShared<FHjsonDocument>(
			SourceUnit,
			Root.ToSharedRef());
		return true;
	}

	bool TryWriteHjsonDocument(
		FHjsonDocumentRef Document,
		FUeAgentResourceTracker& Resources,
		TArray<uint8>& OutCanonicalUtf8,
		FWorkspaceError& OutError)
	{
		OutCanonicalUtf8.Reset();
		OutError.Reset();
		struct FWriteTask
		{
			bool bText = false;
			FString Text;
			FHjsonNodePtr Node;
			int32 Level = 0;
		};
		TArray<FWriteTask> Tasks;
		Tasks.Add({false, FString(), Document->GetRoot(), 0});
		FString Output;
		while (!Tasks.IsEmpty())
		{
			FWriteTask Task = MoveTemp(Tasks.Last());
			Tasks.Pop(EAllowShrinking::No);
			if (Task.bText)
			{
				Output += Task.Text;
				continue;
			}
			if (!Task.Node.IsValid())
			{
				OutError.Set(
					TEXT("hjson_writer_node_missing"),
					Document->GetSourceUnit());
				return false;
			}
			if (!Charge(
				Resources,
				{{EUeAgentResourceDimension::WorkUnits, 1}},
				Task.Node->GetProvenance().GetJsonPointer(),
				OutError))
			{
				return false;
			}
			if (Task.Node->GetKind() == EHjsonNodeKind::Object)
			{
				TArray<TPair<FString, FHjsonNodeRef>> Entries =
					Task.Node->GetEntries();
				Entries.Sort([](
					const TPair<FString, FHjsonNodeRef>& Left,
					const TPair<FString, FHjsonNodeRef>& Right)
				{
					return Left.Key < Right.Key;
				});
				Tasks.Add({true, TEXT("}"), nullptr, Task.Level});
				for (int32 Index = Entries.Num() - 1; Index >= 0; --Index)
				{
					const FString Key = IsBareKey(Entries[Index].Key)
						? Entries[Index].Key
						: QuoteString(Entries[Index].Key);
					Tasks.Add({true, TEXT("\n"), nullptr, Task.Level + 1});
					Tasks.Add({
						false,
						FString(),
						Entries[Index].Value,
						Task.Level + 1,
					});
					Tasks.Add({true, TEXT(": "), nullptr, Task.Level + 1});
					Tasks.Add({true, Key, nullptr, Task.Level + 1});
					Tasks.Add({
						true,
						FString::ChrN(Task.Level + 1, TEXT('\t'))
							.Replace(TEXT("\t"), TEXT("  ")),
						nullptr,
						Task.Level + 1,
					});
				}
				Tasks.Add({
					true,
					Entries.IsEmpty() ? TEXT("{") : TEXT("{\n"),
					nullptr,
					Task.Level,
				});
			}
			else if (Task.Node->GetKind() == EHjsonNodeKind::Array)
			{
				const TArray<FHjsonNodeRef>& Items = Task.Node->GetItems();
				Tasks.Add({true, TEXT("]"), nullptr, Task.Level});
				for (int32 Index = Items.Num() - 1; Index >= 0; --Index)
				{
					Tasks.Add({true, TEXT("\n"), nullptr, Task.Level + 1});
					Tasks.Add({
						false,
						FString(),
						Items[Index],
						Task.Level + 1,
					});
					Tasks.Add({
						true,
						FString::ChrN(Task.Level + 1, TEXT('\t'))
							.Replace(TEXT("\t"), TEXT("  ")),
						nullptr,
						Task.Level + 1,
					});
				}
				Tasks.Add({
					true,
					Items.IsEmpty() ? TEXT("[") : TEXT("[\n"),
					nullptr,
					Task.Level,
				});
			}
			else
			{
				Tasks.Add({
					true,
					RenderScalar(*Task.Node),
					nullptr,
					Task.Level,
				});
			}
		}
		Output += TEXT("\n");
		OutCanonicalUtf8 = ToUtf8(Output);
		if (!Charge(
			Resources,
			{
				{EUeAgentResourceDimension::OutputBytes,
					OutCanonicalUtf8.Num()},
				{EUeAgentResourceDimension::EstimatedMemoryBytes,
					OutCanonicalUtf8.Num()},
			},
			TEXT("hjson-write:") + Document->GetSourceUnit(),
			OutError))
		{
			OutCanonicalUtf8.Reset();
			return false;
		}
		return true;
	}

	namespace
	{
		bool IsReservedStem(const FString& Part)
		{
			const FString Stem = FPaths::GetBaseFilename(Part).ToUpper();
			if (Stem == TEXT("CON")
				|| Stem == TEXT("PRN")
				|| Stem == TEXT("AUX")
				|| Stem == TEXT("NUL"))
			{
				return true;
			}
			for (int32 Index = 1; Index <= 9; ++Index)
			{
				if (Stem == FString::Printf(TEXT("COM%d"), Index)
					|| Stem == FString::Printf(TEXT("LPT%d"), Index))
				{
					return true;
				}
			}
			return false;
		}

		bool NormalizeUnitId(
			FString Value,
			FString& OutValue,
			FWorkspaceError& OutError)
		{
			FPaths::NormalizeFilename(Value);
			FPaths::RemoveDuplicateSlashes(Value);
			if (Value.IsEmpty()
				|| !FPaths::IsRelative(Value)
				|| Value.StartsWith(TEXT("../"))
				|| Value.Contains(TEXT("/../"))
				|| Value == TEXT(".."))
			{
				OutError.Set(TEXT("workspace_path_invalid"), Value);
				return false;
			}
			TArray<FString> Parts;
			Value.ParseIntoArray(Parts, TEXT("/"), true);
			for (const FString& Part : Parts)
			{
				if (Part == TEXT(".")
					|| Part == TEXT("..")
					|| IsReservedStem(Part))
				{
					OutError.Set(
						IsReservedStem(Part)
							? TEXT("workspace_path_reserved_name")
							: TEXT("workspace_path_invalid"),
						Value);
					return false;
				}
			}
			OutValue = MoveTemp(Value);
			return true;
		}

		bool ValidateUnitPaths(
			const TArray<FString>& UnitIds,
			FWorkspaceError& OutError)
		{
			TMap<FString, FString> Folded;
			for (const FString& Raw : UnitIds)
			{
				FString UnitId;
				if (!NormalizeUnitId(Raw, UnitId, OutError))
				{
					return false;
				}
				const FString Key = UnitId.ToLower();
				if (const FString* Existing = Folded.Find(Key))
				{
					if (*Existing != UnitId)
					{
						OutError.Set(
							TEXT("workspace_path_case_collision"),
							UnitId);
						return false;
					}
				}
				else
				{
					Folded.Add(Key, UnitId);
				}
			}
			return true;
		}

		bool ReadWorkspaceSources(
			const FWorkspaceSource& Source,
			const FWorkspacePolicy& Policy,
			FUeAgentResourceTracker& Resources,
			TArray<FSourceText>& OutSources,
			FWorkspaceError& OutError)
		{
			OutSources.Reset();
			if (Source.GetKind() == EWorkspaceSourceKind::Inline)
			{
				FString UnitId;
				if (!NormalizeUnitId(
					Source.GetSourceUnit().IsEmpty()
						? Policy.GetEntryUnit()
						: Source.GetSourceUnit(),
					UnitId,
					OutError))
				{
					return false;
				}
				if (!Charge(
					Resources,
					{{EUeAgentResourceDimension::Files, 1}},
					TEXT("workspace:") + UnitId,
					OutError))
				{
					return false;
				}
				OutSources.Add({UnitId, Source.GetText()});
				return true;
			}

			const FString Root = FPaths::ConvertRelativePathToFull(
				Source.GetPath());
			if (Source.GetKind() == EWorkspaceSourceKind::SingleFile)
			{
				if (!IFileManager::Get().FileExists(*Root))
				{
					OutError.Set(
						TEXT("workspace_source_file_missing"),
						Root);
					return false;
				}
				if (!Charge(
					Resources,
					{{EUeAgentResourceDimension::Files, 1}},
					TEXT("workspace:") + Policy.GetEntryUnit(),
					OutError))
				{
					return false;
				}
				FString Text;
				if (!FFileHelper::LoadFileToString(Text, *Root))
				{
					OutError.Set(
						TEXT("workspace_source_read_failed"),
						Policy.GetEntryUnit());
					return false;
				}
				OutSources.Add({Policy.GetEntryUnit(), MoveTemp(Text)});
				return true;
			}
			if (!IFileManager::Get().DirectoryExists(*Root))
			{
				OutError.Set(
					TEXT("workspace_source_directory_missing"),
					Root);
				return false;
			}
			TArray<FString> Files;
			IFileManager::Get().FindFilesRecursive(
				Files,
				*Root,
				TEXT("*.hjson"),
				true,
				false);
			Files.Sort();
			TArray<FString> UnitIds;
			FString RelativeBase = Root;
			if (!RelativeBase.EndsWith(TEXT("/"))
				&& !RelativeBase.EndsWith(TEXT("\\")))
			{
				RelativeBase += TEXT("/");
			}
			for (const FString& File : Files)
			{
				FString UnitId = File;
				if (!FPaths::MakePathRelativeTo(UnitId, *RelativeBase))
				{
					OutError.Set(TEXT("workspace_path_invalid"), File);
					return false;
				}
				FPaths::NormalizeFilename(UnitId);
				FPaths::RemoveDuplicateSlashes(UnitId);
				UnitIds.Add(UnitId);
				if (UnitIds.Num() > Policy.GetMaxFiles())
				{
					OutError.Set(
						TEXT("workspace_file_budget_exceeded"),
						UnitId,
						TEXT("/"),
						UnitIds.Num(),
						true);
					return false;
				}
				if (!Charge(
					Resources,
					{{EUeAgentResourceDimension::Files, 1}},
					TEXT("workspace:") + UnitId,
					OutError))
				{
					return false;
				}
			}
			if (!ValidateUnitPaths(UnitIds, OutError))
			{
				return false;
			}
			for (int32 Index = 0; Index < Files.Num(); ++Index)
			{
				FString Text;
				if (!FFileHelper::LoadFileToString(Text, *Files[Index]))
				{
					OutError.Set(
						TEXT("workspace_source_read_failed"),
						UnitIds[Index]);
					return false;
				}
				OutSources.Add({UnitIds[Index], MoveTemp(Text)});
			}
			return true;
		}

		FString NodeScalarText(const FHjsonNode& Node)
		{
			return Node.GetKind() == EHjsonNodeKind::Integer
				|| Node.GetKind() == EHjsonNodeKind::Decimal
				? Node.GetLexeme()
				: Node.GetText();
		}

		bool ValidatePolicy(
			const FHjsonNode& Root,
			const FString& SourceUnit,
			const FWorkspacePolicy& Policy,
			FWorkspaceError& OutError)
		{
			if (Root.GetKind() != EHjsonNodeKind::Object)
			{
				OutError.Set(
					TEXT("workspace_unit_root_not_object"),
					SourceUnit);
				return false;
			}
			const TArray<TPair<FString, FString>> Expected = {
				{TEXT("contract_id"), Policy.GetContractId()},
				{TEXT("contract_version"),
					::LexToString(Policy.GetContractVersion())},
				{TEXT("profile_id"), Policy.GetProfileId()},
			};
			for (const TPair<FString, FString>& Entry : Expected)
			{
				const FHjsonNode* Node = Root.Get(Entry.Key);
				if (!Node)
				{
					OutError.Set(
						TEXT("workspace_policy_field_missing"),
						SourceUnit,
						TEXT("/") + Entry.Key);
					return false;
				}
				if (NodeScalarText(*Node) != Entry.Value)
				{
					OutError.Set(
						TEXT("workspace_policy_mismatch"),
						SourceUnit,
						TEXT("/") + Entry.Key);
					return false;
				}
			}
			return true;
		}

		bool ResolveReference(
			const FString& SourceUnit,
			const FString& Reference,
			FString& OutTarget,
			FWorkspaceError& OutError)
		{
			FString Candidate = FPaths::Combine(
				FPaths::GetPath(SourceUnit),
				Reference);
			FPaths::NormalizeFilename(Candidate);
			FPaths::RemoveDuplicateSlashes(Candidate);
			TArray<FString> RawParts;
			Candidate.ParseIntoArray(RawParts, TEXT("/"), true);
			TArray<FString> Parts;
			for (const FString& Part : RawParts)
			{
				if (Part == TEXT("."))
				{
					continue;
				}
				if (Part == TEXT(".."))
				{
					if (Parts.IsEmpty())
					{
						OutError.Set(
							TEXT("workspace_ref_escape"),
							SourceUnit);
						return false;
					}
					Parts.Pop();
					continue;
				}
				Parts.Add(Part);
			}
			Candidate = FString::Join(Parts, TEXT("/"));
			if (!NormalizeUnitId(Candidate, OutTarget, OutError))
			{
				if (OutError.GetCode() == TEXT("workspace_path_invalid"))
				{
					OutError.Set(
						TEXT("workspace_ref_escape"),
						SourceUnit);
				}
				return false;
			}
			return true;
		}

		bool CollectEdges(
			const TArray<FWorkspaceUnit>& Units,
			const FWorkspacePolicy& Policy,
			FUeAgentResourceTracker& Resources,
			TArray<FReferenceEdge>& OutEdges,
			FWorkspaceError& OutError)
		{
			TSet<FString> UnitIds;
			for (const FWorkspaceUnit& Unit : Units)
			{
				UnitIds.Add(Unit.GetUnitId());
			}
			TSet<FString> ReferenceFields;
			for (const FString& Field : Policy.GetReferenceFields())
			{
				ReferenceFields.Add(Field);
			}
			for (const FWorkspaceUnit& Unit : Units)
			{
				TArray<FHjsonNodePtr> Stack;
				Stack.Add(Unit.GetDocument()->GetRoot());
				while (!Stack.IsEmpty())
				{
					FHjsonNodePtr Node = Stack.Pop(EAllowShrinking::No);
					if (Node->GetKind() == EHjsonNodeKind::Object)
					{
						for (const TPair<FString, FHjsonNodeRef>& Entry
							: Node->GetEntries())
						{
							if (ReferenceFields.Contains(Entry.Key)
								&& Entry.Value->GetKind()
									== EHjsonNodeKind::String)
							{
								FString Target;
								if (!ResolveReference(
									Unit.GetUnitId(),
									Entry.Value->GetText(),
									Target,
									OutError))
								{
									return false;
								}
								if (!UnitIds.Contains(Target))
								{
									OutError.Set(
										TEXT("workspace_ref_missing"),
										Unit.GetUnitId(),
										Entry.Value->GetProvenance()
											.GetJsonPointer());
									return false;
								}
								if (!Charge(
									Resources,
									{{EUeAgentResourceDimension::ReferenceEdges, 1}},
									Entry.Value->GetProvenance().GetJsonPointer(),
									OutError))
								{
									return false;
								}
								OutEdges.Emplace(
									Unit.GetUnitId(),
									Entry.Value->GetProvenance().GetJsonPointer(),
									Target);
							}
							Stack.Add(Entry.Value);
						}
					}
					else if (Node->GetKind() == EHjsonNodeKind::Array)
					{
						for (const FHjsonNodeRef& Item : Node->GetItems())
						{
							Stack.Add(Item);
						}
					}
				}
			}
			return true;
		}

		bool RejectCycles(
			const TArray<FWorkspaceUnit>& Units,
			const TArray<FReferenceEdge>& Edges,
			FWorkspaceError& OutError)
		{
			TMap<FString, TArray<FString>> Adjacency;
			for (const FWorkspaceUnit& Unit : Units)
			{
				Adjacency.Add(Unit.GetUnitId(), {});
			}
			for (const FReferenceEdge& Edge : Edges)
			{
				Adjacency.FindChecked(Edge.GetSourceUnit()).Add(
					Edge.GetTargetUnit());
			}
			for (TPair<FString, TArray<FString>>& Entry : Adjacency)
			{
				Entry.Value.Sort();
			}
			TMap<FString, uint8> Color;
			for (const FWorkspaceUnit& Unit : Units)
			{
				Color.Add(Unit.GetUnitId(), 0);
			}
			struct FDfsFrame
			{
				FString Unit;
				int32 ChildIndex = 0;
			};
			TArray<FString> Starts;
			Adjacency.GetKeys(Starts);
			Starts.Sort();
			for (const FString& Start : Starts)
			{
				if (Color.FindChecked(Start) != 0)
				{
					continue;
				}
				TArray<FDfsFrame> Stack = {{Start, 0}};
				TArray<FString> Path = {Start};
				TMap<FString, int32> Positions = {{Start, 0}};
				Color.FindChecked(Start) = 1;
				while (!Stack.IsEmpty())
				{
					FDfsFrame& Frame = Stack.Last();
					const TArray<FString>& Children =
						Adjacency.FindChecked(Frame.Unit);
					if (Frame.ChildIndex >= Children.Num())
					{
						Color.FindChecked(Frame.Unit) = 2;
						Positions.Remove(Frame.Unit);
						Path.Pop(EAllowShrinking::No);
						Stack.Pop(EAllowShrinking::No);
						continue;
					}
					const FString Target = Children[Frame.ChildIndex++];
					if (Color.FindChecked(Target) == 1)
					{
						const int32 CycleStart = Positions.FindChecked(Target);
						TArray<FString> Cycle;
						for (int32 Index = CycleStart; Index < Path.Num(); ++Index)
						{
							Cycle.Add(Path[Index]);
						}
						Cycle.Add(Target);
						OutError.Set(
							TEXT("workspace_ref_cycle"),
							Frame.Unit,
							TEXT("/"),
							0,
							false,
							MoveTemp(Cycle));
						return false;
					}
					if (Color.FindChecked(Target) == 2)
					{
						continue;
					}
					Color.FindChecked(Target) = 1;
					Positions.Add(Target, Path.Num());
					Path.Add(Target);
					Stack.Add({Target, 0});
				}
			}
			return true;
		}
	}

	bool TryLoadWorkspace(
		const FWorkspaceSource& Source,
		const FWorkspacePolicy& Policy,
		FUeAgentResourceTracker& Resources,
		FCanonicalWorkspacePtr& OutWorkspace,
		FWorkspaceError& OutError)
	{
		OutWorkspace.Reset();
		OutError.Reset();
		if (!Policy.IsValid())
		{
			OutError.Set(TEXT("workspace_policy_invalid"));
			return false;
		}
		TArray<FSourceText> SourceTexts;
		if (!ReadWorkspaceSources(
			Source,
			Policy,
			Resources,
			SourceTexts,
			OutError))
		{
			return false;
		}
		TArray<FString> UnitIds;
		for (const FSourceText& SourceText : SourceTexts)
		{
			UnitIds.Add(SourceText.UnitId);
		}
		if (!ValidateUnitPaths(UnitIds, OutError))
		{
			return false;
		}
		TArray<FWorkspaceUnit> Units;
		for (const FSourceText& SourceText : SourceTexts)
		{
			FHjsonDocumentPtr Document;
			if (!TryParseHjsonDocument(
				SourceText.Text,
				SourceText.UnitId,
				Resources,
				Document,
				OutError)
				|| !ValidatePolicy(
					Document->GetRoot().Get(),
					SourceText.UnitId,
					Policy,
					OutError))
			{
				return false;
			}
			TArray<uint8> CanonicalUtf8;
			if (!TryWriteHjsonDocument(
				Document.ToSharedRef(),
				Resources,
				CanonicalUtf8,
				OutError))
			{
				return false;
			}
			Units.Emplace(
				SourceText.UnitId,
				Document.ToSharedRef(),
				CanonicalUtf8,
				HashBytes(CanonicalUtf8));
		}
		if (!UnitIds.Contains(Policy.GetEntryUnit()))
		{
			OutError.Set(
				TEXT("workspace_entry_unit_missing"),
				Policy.GetEntryUnit());
			return false;
		}
		TArray<FReferenceEdge> Edges;
		if (!CollectEdges(Units, Policy, Resources, Edges, OutError)
			|| !RejectCycles(Units, Edges, OutError))
		{
			return false;
		}
		Units.Sort([](const FWorkspaceUnit& Left, const FWorkspaceUnit& Right)
		{
			return Left.GetUnitId() < Right.GetUnitId();
		});
		Edges.Sort([](const FReferenceEdge& Left, const FReferenceEdge& Right)
		{
			if (Left.GetSourceUnit() != Right.GetSourceUnit())
			{
				return Left.GetSourceUnit() < Right.GetSourceUnit();
			}
			if (Left.GetJsonPointer() != Right.GetJsonPointer())
			{
				return Left.GetJsonPointer() < Right.GetJsonPointer();
			}
			return Left.GetTargetUnit() < Right.GetTargetUnit();
		});
		FString Material = Policy.GetDialectId()
			+ TEXT("\x1f") + Policy.GetContractId()
			+ TEXT("\x1f") + ::LexToString(Policy.GetContractVersion())
			+ TEXT("\x1f") + Policy.GetProfileId()
			+ TEXT("\x1f") + Policy.GetEntryUnit();
		for (const FWorkspaceUnit& Unit : Units)
		{
			Material += TEXT("\n") + Unit.GetUnitId()
				+ TEXT("\n") + Unit.GetContentHash();
		}
		for (const FReferenceEdge& Edge : Edges)
		{
			Material += TEXT("\n") + Edge.GetSourceUnit()
				+ TEXT("\x1f") + Edge.GetJsonPointer()
				+ TEXT("\x1f") + Edge.GetTargetUnit();
		}
		const TArray<uint8> FingerprintBytes = ToUtf8(Material);
		OutWorkspace = MakeShared<FCanonicalWorkspace>(
			Source.GetKind(),
			Policy,
			MoveTemp(Units),
			MoveTemp(Edges),
			HashBytes(FingerprintBytes));
		return true;
	}

	namespace
	{
		FString ValueKindText(EUeAgentValueKind Kind)
		{
			switch (Kind)
			{
			case EUeAgentValueKind::Null:
				return TEXT("null");
			case EUeAgentValueKind::Boolean:
				return TEXT("boolean");
			case EUeAgentValueKind::String:
				return TEXT("string");
			case EUeAgentValueKind::Integer:
				return TEXT("integer");
			case EUeAgentValueKind::Decimal:
				return TEXT("decimal");
			case EUeAgentValueKind::Array:
				return TEXT("array");
			case EUeAgentValueKind::Object:
				return TEXT("object");
			case EUeAgentValueKind::Temporal:
				return TEXT("temporal");
			case EUeAgentValueKind::Spatial:
				return TEXT("spatial");
			}
			return TEXT("unknown");
		}

		FString DefaultSourceKindText(EUeAgentDefaultSourceKind Kind)
		{
			switch (Kind)
			{
			case EUeAgentDefaultSourceKind::Factory:
				return TEXT("factory");
			case EUeAgentDefaultSourceKind::ClassDefaultObject:
				return TEXT("class_default_object");
			case EUeAgentDefaultSourceKind::Constructor:
				return TEXT("constructor");
			case EUeAgentDefaultSourceKind::StructInitializer:
				return TEXT("struct_initializer");
			case EUeAgentDefaultSourceKind::Schema:
				return TEXT("schema");
			case EUeAgentDefaultSourceKind::DomainNative:
				return TEXT("domain_native");
			case EUeAgentDefaultSourceKind::PostCreateReadback:
				return TEXT("post_create_readback");
			}
			return TEXT("unknown");
		}

		FString StatePlaneText(EUeAgentStatePlane StatePlane)
		{
			const TCHAR* Value =
				UE::UAI::Contract::LexToString(StatePlane);
			return *Value ? Value : TEXT("unknown");
		}

		FString SpatialQuantityKindText(EUeAgentSpatialQuantityKind Kind)
		{
			switch (Kind)
			{
			case EUeAgentSpatialQuantityKind::Point:
				return TEXT("point");
			case EUeAgentSpatialQuantityKind::Direction:
				return TEXT("direction");
			case EUeAgentSpatialQuantityKind::Normal:
				return TEXT("normal");
			case EUeAgentSpatialQuantityKind::Transform:
				return TEXT("transform");
			}
			return TEXT("unknown");
		}

		FString StringArrayText(const TArray<FString>& Values)
		{
			FString Text = TEXT("[");
			for (int32 Index = 0; Index < Values.Num(); ++Index)
			{
				if (Index > 0)
				{
					Text += TEXT(",");
				}
				Text += QuoteString(Values[Index]);
			}
			Text += TEXT("]");
			return Text;
		}

		FString StringMatrixText(const TArray<TArray<FString>>& Values)
		{
			FString Text = TEXT("[");
			for (int32 Index = 0; Index < Values.Num(); ++Index)
			{
				if (Index > 0)
				{
					Text += TEXT(",");
				}
				Text += StringArrayText(Values[Index]);
			}
			Text += TEXT("]");
			return Text;
		}

		FString StringMapText(const TMap<FString, FString>& Values)
		{
			TArray<FString> Keys;
			Values.GetKeys(Keys);
			Keys.Sort();
			FString Text = TEXT("{");
			for (int32 Index = 0; Index < Keys.Num(); ++Index)
			{
				if (Index > 0)
				{
					Text += TEXT(",");
				}
				Text += QuoteString(Keys[Index])
					+ TEXT(":")
					+ QuoteString(Values.FindChecked(Keys[Index]));
			}
			Text += TEXT("}");
			return Text;
		}

		FString TemporalText(const FUeAgentTemporalPoint& Temporal)
		{
			return TEXT("{\"clock_id\":")
				+ QuoteString(Temporal.GetClockId())
				+ TEXT(",\"tick\":")
				+ QuoteString(Temporal.GetTick())
				+ TEXT(",\"rate_numerator\":")
				+ QuoteString(Temporal.GetRateNumerator())
				+ TEXT(",\"rate_denominator\":")
				+ QuoteString(Temporal.GetRateDenominator())
				+ TEXT(",\"state\":")
				+ QuoteString(Temporal.GetState())
				+ TEXT("}");
		}

		FString SpatialText(const FUeAgentSpatialValue& Spatial)
		{
			const FUeAgentSpatialFrame& Frame = Spatial.GetFrame();
			return TEXT("{\"subject\":")
				+ QuoteString(Spatial.GetSubject())
				+ TEXT(",\"quantity_kind\":")
				+ QuoteString(
					SpatialQuantityKindText(Spatial.GetQuantityKind()))
				+ TEXT(",\"frame\":{\"frame_id\":")
				+ QuoteString(Frame.GetFrameId())
				+ TEXT(",\"basis_frame_id\":")
				+ QuoteString(Frame.GetBasisFrameId())
				+ TEXT(",\"dimension\":")
				+ ::LexToString(Frame.GetDimension())
				+ TEXT(",\"basis\":")
				+ StringMatrixText(Frame.GetBasis())
				+ TEXT(",\"origin\":")
				+ StringArrayText(Frame.GetOrigin())
				+ TEXT(",\"unit\":")
				+ QuoteString(Frame.GetUnit())
				+ TEXT(",\"basis_evidence\":")
				+ QuoteString(Frame.GetBasisEvidence())
				+ TEXT(",\"observed_at\":")
				+ TemporalText(Frame.GetObservedAt())
				+ TEXT("},\"components\":")
				+ StringArrayText(Spatial.GetComponents())
				+ TEXT(",\"observed_at\":")
				+ TemporalText(Spatial.GetObservedAt())
				+ TEXT(",\"conversion_evidence\":")
				+ StringArrayText(Spatial.GetConversionEvidence())
				+ TEXT("}");
		}

		struct FValueWriteTask
		{
			explicit FValueWriteTask(FString InText)
				: Text(MoveTemp(InText))
			{
			}

			explicit FValueWriteTask(
				const FUeAgentCanonicalValue* InValue)
				: Value(InValue)
			{
			}

			FString Text;
			const FUeAgentCanonicalValue* Value = nullptr;
		};

		FString StateValueText(
			const FUeAgentCanonicalValue& Root,
			FUeAgentResourceTracker& Resources,
			const FString& ResourceKey,
			FWorkspaceError& OutError)
		{
			FString Text;
			TArray<FValueWriteTask> Tasks;
			Tasks.Emplace(&Root);
			while (!Tasks.IsEmpty())
			{
				FValueWriteTask Task = MoveTemp(Tasks.Last());
				Tasks.Pop(EAllowShrinking::No);
				if (!Task.Value)
				{
					Text += Task.Text;
					continue;
				}
				if (!Charge(
					Resources,
					{{EUeAgentResourceDimension::WorkUnits, 1}},
					ResourceKey,
					OutError))
				{
					return FString();
				}

				const FUeAgentCanonicalValue& Value = *Task.Value;
				switch (Value.GetKind())
				{
				case EUeAgentValueKind::Null:
					Text += TEXT("null");
					break;
				case EUeAgentValueKind::Boolean:
					Text += Value.GetBoolean()
						? TEXT("true")
						: TEXT("false");
					break;
				case EUeAgentValueKind::String:
					Text += QuoteString(Value.GetText());
					break;
				case EUeAgentValueKind::Integer:
				case EUeAgentValueKind::Decimal:
					Text += Value.GetLexeme();
					break;
				case EUeAgentValueKind::Array:
				{
					const TArray<FUeAgentCanonicalValueRef>& Items =
						Value.GetArrayItems();
					Text += TEXT("[");
					Tasks.Emplace(TEXT("]"));
					for (int32 Index = Items.Num() - 1; Index >= 0; --Index)
					{
						Tasks.Emplace(&Items[Index].Get());
						if (Index > 0)
						{
							Tasks.Emplace(TEXT(","));
						}
					}
					break;
				}
				case EUeAgentValueKind::Object:
				{
					const TArray<
						TPair<FString, FUeAgentCanonicalValueRef>>& Items =
						Value.GetObjectItems();
					TArray<int32> Order;
					Order.Reserve(Items.Num());
					for (int32 Index = 0; Index < Items.Num(); ++Index)
					{
						Order.Add(Index);
					}
					Order.Sort(
						[&Items](int32 Left, int32 Right)
						{
							return Items[Left].Key < Items[Right].Key;
						});

					Text += TEXT("{");
					Tasks.Emplace(TEXT("}"));
					for (
						int32 OrderIndex = Order.Num() - 1;
						OrderIndex >= 0;
						--OrderIndex)
					{
						const int32 ItemIndex = Order[OrderIndex];
						Tasks.Emplace(&Items[ItemIndex].Value.Get());
						Tasks.Emplace(
							QuoteString(Items[ItemIndex].Key)
							+ TEXT(":"));
						if (OrderIndex > 0)
						{
							Tasks.Emplace(TEXT(","));
						}
					}
					break;
				}
				case EUeAgentValueKind::Temporal:
					Text += Value.GetTemporal().IsValid()
						? TemporalText(*Value.GetTemporal())
						: TEXT("null");
					break;
				case EUeAgentValueKind::Spatial:
					Text += Value.GetSpatial().IsValid()
						? SpatialText(*Value.GetSpatial())
						: TEXT("null");
					break;
				}
			}
			return Text;
		}

		FString RevisionText(const FUeAgentStateRevision& Revision)
		{
			return TEXT("{\"source_id\":")
				+ QuoteString(Revision.GetSourceId())
				+ TEXT(",\"generation\":")
				+ QuoteString(Revision.GetGeneration())
				+ TEXT(",\"sequence\":")
				+ ::LexToString(Revision.GetSequence())
				+ TEXT("}");
		}

		FString BuildSnapshotHjson(
			const FExportPlanRequest& Request,
			FUeAgentResourceTracker& Resources,
			FWorkspaceError& OutError)
		{
			const FUeAgentCanonicalSnapshot& Snapshot =
				Request.GetSnapshot().Get();
			FString Text;
			Text += TEXT("{\n");
			Text += TEXT("  contract_id: ")
				+ QuoteString(Request.GetContractId()) + TEXT("\n");
			Text += TEXT("  contract_version: ")
				+ ::LexToString(Request.GetContractVersion()) + TEXT("\n");
			Text += TEXT("  profile_id: ")
				+ QuoteString(Request.GetProfileId()) + TEXT("\n");
			Text += TEXT("  operation: \"update\"\n");
			Text += TEXT("  snapshot: {\n");
			Text += TEXT("    snapshot_id: ")
				+ QuoteString(Snapshot.GetSnapshotId()) + TEXT("\n");
			Text += TEXT("    state_plane: ")
				+ QuoteString(StatePlaneText(Snapshot.GetStatePlane()))
				+ TEXT("\n");
			Text += TEXT("    captured_at: ")
				+ TemporalText(Snapshot.GetCapturedAt()) + TEXT("\n");
			Text += TEXT("    revision: {\n");
			Text += TEXT("      source_id: ")
				+ QuoteString(Snapshot.GetRevision().GetSourceId())
				+ TEXT("\n");
			Text += TEXT("      generation: ")
				+ QuoteString(Snapshot.GetRevision().GetGeneration())
				+ TEXT("\n");
			Text += TEXT("      sequence: ")
				+ ::LexToString(Snapshot.GetRevision().GetSequence())
				+ TEXT("\n");
			Text += TEXT("    }\n");
			Text += TEXT("    provenance: {\n");
			Text += TEXT("      source_kind: ")
				+ QuoteString(Snapshot.GetProvenance().GetSourceKind())
				+ TEXT("\n");
			Text += TEXT("      source_id: ")
				+ QuoteString(Snapshot.GetProvenance().GetSourceId())
				+ TEXT("\n");
			Text += TEXT("      evidence_id: ")
				+ QuoteString(Snapshot.GetProvenance().GetEvidenceId())
				+ TEXT("\n");
			Text += TEXT("    }\n");
			Text += TEXT("    entries: [\n");
			for (const FUeAgentStateEntry& Entry : Snapshot.GetEntries())
			{
				if (!Charge(
					Resources,
					{{EUeAgentResourceDimension::WorkUnits, 1}},
					TEXT("export:") + Entry.GetIdentity().GetCanonicalKey(),
					OutError))
				{
					return FString();
				}
				Text += TEXT("      {\n");
				Text += TEXT("        identity: {\n");
				Text += TEXT("          semantic_id: ")
					+ QuoteString(Entry.GetIdentity().GetSemanticId())
					+ TEXT("\n");
				Text += TEXT("          target_kind: ")
					+ QuoteString(Entry.GetIdentity().GetTargetKind())
					+ TEXT("\n");
				Text += TEXT("          owner_id: ")
					+ QuoteString(Entry.GetIdentity().GetOwnerId())
					+ TEXT("\n");
				Text += TEXT("          selector: ")
					+ QuoteString(Entry.GetIdentity().GetSelector())
					+ TEXT("\n");
				Text += TEXT("          object_role: ")
					+ QuoteString(Entry.GetIdentity().GetObjectRole())
					+ TEXT("\n");
				Text += TEXT("          generation: ")
					+ QuoteString(Entry.GetIdentity().GetGeneration())
					+ TEXT("\n");
				Text += TEXT("          canonical_key: ")
					+ QuoteString(Entry.GetIdentity().GetCanonicalKey())
					+ TEXT("\n");
				Text += TEXT("        }\n");
				Text += TEXT("        type: {\n");
				Text += TEXT("          type_id: ")
					+ QuoteString(Entry.GetType().GetTypeId()) + TEXT("\n");
				Text += TEXT("          value_kind: ")
					+ QuoteString(
						ValueKindText(Entry.GetType().GetValueKind()))
					+ TEXT("\n");
				Text += TEXT("          nullable: ")
					+ FString(
						Entry.GetType().IsNullable()
							? TEXT("true")
							: TEXT("false"))
					+ TEXT("\n");
				Text += TEXT("          container_kind: ")
					+ QuoteString(Entry.GetType().GetContainerKind())
					+ TEXT("\n");
				Text += TEXT("        }\n");
				Text += TEXT("        value: ")
					+ StateValueText(
						Entry.GetValue().Get(),
						Resources,
						TEXT("export:value:")
							+ Entry.GetIdentity().GetCanonicalKey(),
						OutError)
					+ TEXT("\n");
				if (!OutError.GetCode().IsEmpty())
				{
					return FString();
				}
				Text += TEXT("        default: {\n");
				Text += TEXT("          source_kind: ")
					+ QuoteString(
						DefaultSourceKindText(
							Entry.GetDefault().GetSourceKind()))
					+ TEXT("\n");
				Text += TEXT("          source_identity: ")
					+ QuoteString(
						Entry.GetDefault().GetSourceIdentity())
					+ TEXT("\n");
				Text += TEXT("          provenance: ")
					+ QuoteString(Entry.GetDefault().GetProvenance())
					+ TEXT("\n");
				Text += TEXT("          value: ")
					+ StateValueText(
						Entry.GetDefault().GetValue().Get(),
						Resources,
						TEXT("export:default:")
							+ Entry.GetIdentity().GetCanonicalKey(),
						OutError)
					+ TEXT("\n");
				if (!OutError.GetCode().IsEmpty())
				{
					return FString();
				}
				Text += TEXT("        }\n");
				Text += TEXT("        details: {\n");
				Text += TEXT("        display_name: ")
					+ QuoteString(Entry.GetDetails().GetDisplayName())
					+ TEXT("\n");
				Text += TEXT("          tooltip: ")
					+ QuoteString(Entry.GetDetails().GetTooltip())
					+ TEXT("\n");
				Text += TEXT("          category_path: ")
					+ StringArrayText(
						Entry.GetDetails().GetCategoryPath())
					+ TEXT("\n");
				Text += TEXT("          editable: ")
					+ FString(
						Entry.GetDetails().IsEditable()
							? TEXT("true")
							: TEXT("false"))
					+ TEXT("\n");
				Text += TEXT("          availability: ")
					+ QuoteString(
						Entry.GetDetails().GetAvailability())
					+ TEXT("\n");
				Text += TEXT("          constraints: ")
					+ StringMapText(
						Entry.GetDetails().GetConstraints())
					+ TEXT("\n");
				Text += TEXT("        }\n");
				Text += TEXT("        observed_revision: ")
					+ RevisionText(Entry.GetObservedRevision())
					+ TEXT("\n");
				Text += TEXT("      }\n");
			}
			Text += TEXT("    ]\n");
			Text += TEXT("  }\n");
			Text += TEXT("}\n");
			return Text;
		}
	}

	bool TryBuildExportPlan(
		const FExportPlanRequest& Request,
		FUeAgentResourceTracker& Resources,
		FExportWorkspacePlanPtr& OutPlan,
		FWorkspaceError& OutError)
	{
		OutPlan.Reset();
		OutError.Reset();
		if (!Request.IsValid())
		{
			OutError.Set(TEXT("export_plan_request_invalid"));
			return false;
		}
		const FString Text = BuildSnapshotHjson(
			Request,
			Resources,
			OutError);
		if (!OutError.GetCode().IsEmpty())
		{
			return false;
		}
		TArray<uint8> CanonicalUtf8 = ToUtf8(Text);
		if (!Charge(
			Resources,
			{
				{EUeAgentResourceDimension::OutputBytes,
					CanonicalUtf8.Num()},
				{EUeAgentResourceDimension::StringBytes,
					CanonicalUtf8.Num()},
				{EUeAgentResourceDimension::EstimatedMemoryBytes,
					CanonicalUtf8.Num()},
			},
			TEXT("export:canonical"),
			OutError))
		{
			return false;
		}
		const int64 ObservedTokens = (CanonicalUtf8.Num() + 3ll) / 4ll;
		const FString ContentHash = HashBytes(CanonicalUtf8);
		FString PlanMaterial = Request.GetEntryFile()
			+ TEXT("\x1f")
			+ Request.GetSnapshot()->GetSnapshotId()
			+ TEXT("\x1f")
			+ Request.GetSnapshot()->GetRevision().GetSourceId()
			+ TEXT("\x1f")
			+ Request.GetSnapshot()->GetRevision().GetGeneration()
			+ TEXT("\x1f")
			+ ::LexToString(
				Request.GetSnapshot()->GetRevision().GetSequence())
			+ TEXT("\x1f")
			+ ContentHash;
		const FString PlanFingerprint = HashBytes(ToUtf8(PlanMaterial));
		CanonicalUtf8.Add(0);
		CanonicalUtf8.Pop(EAllowShrinking::No);
		TArray<FExportFilePlan> Files;
		Files.Emplace(
			Request.GetEntryFile(),
			MoveTemp(CanonicalUtf8),
			ContentHash);
		OutPlan = MakeShared<FExportWorkspacePlan>(
			Request.GetWorkspaceKind(),
			Request.GetEntryFile(),
			Request.GetSnapshot()->GetSnapshotId(),
			Request.GetSnapshot()->GetRevision(),
			Request.GetSnapshot()->GetProvenance().GetEvidenceId(),
			MoveTemp(Files),
			PlanFingerprint,
			ObservedTokens);
		return true;
	}
}
