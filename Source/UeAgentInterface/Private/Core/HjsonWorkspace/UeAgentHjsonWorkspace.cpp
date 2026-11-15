#include "Core/HjsonWorkspace/UeAgentHjsonWorkspace.h"
#include "Core/HjsonWorkspace/UeAgentHjsonDiagnostic.h"
#include "Core/HjsonWorkspace/UeAgentHjsonValidatedHeader.h"
#include "Core/HjsonWorkspace/UeAgentHjsonSyntaxCache.h"
#include "Core/HjsonWorkspace/UeAgentTextLineDiff.h"
#include "Core/HjsonWorkspace/UeAgentHjsonPointerText.h"
#include "Core/StateSemantic/UeAgentExactStringKeys.h"
#include "Utilities/AuthoringCore/UeAgentAuthoringDiagnostics.h"

#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "HAL/FileManager.h"
#include "HAL/CriticalSection.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "Misc/PackageName.h"
#include "Misc/ScopeExit.h"
#include "Misc/ScopeLock.h"
#include "Serialization/JsonSerializer.h"
#include "Serialization/JsonWriter.h"
#include <cstring>

namespace UE::UAI::HjsonWorkspace
{
	using namespace UE::UAI::ContextResource;
	using namespace UE::UAI::StateSemantic;

	struct FHjsonRootHeaderFacts
	{
		FString ContentHash;
		TArray<TPair<FString, FString>> Strings;
		TMap<EUeAgentResourceDimension, int64> ParseCharges;
	};

	namespace
	{
		thread_local FHjsonRootHeaderFactsPtr PreviousParsedRootHeader;

		struct FHjsonJsonProjectionTask
		{
			const FHjsonNode* Node = nullptr;
			bool bExit = false;
		};

		bool ChargeJsonProjectionNode(
			const FHjsonNode& Node,
			FUeAgentResourceTracker& Resources,
			FWorkspaceError& OutError)
		{
			if (!Resources.CollectsUsage()) return true;
			const int64 ChildCount = Node.GetKind() == EHjsonNodeKind::Array
				? Node.GetItems().Num()
				: Node.GetKind() == EHjsonNodeKind::Object
					? Node.GetEntries().Num()
					: 0;
			const int64 TextChars = Node.GetKind() == EHjsonNodeKind::String
				? Node.GetText().Len()
				: Node.GetLexeme().Len();
			int64 KeyChars = 0;
			if (Node.GetKind() == EHjsonNodeKind::Object)
			{
				for (const TPair<FString, FHjsonNodeRef>& Entry : Node.GetEntries())
				{
					KeyChars += Entry.Key.Len();
				}
			}
			const int64 ContainerBytes = Node.GetKind() == EHjsonNodeKind::Object
				? ChildCount * static_cast<int64>(
					sizeof(TPair<FString, TSharedPtr<FJsonValue>>))
					+ KeyChars * static_cast<int64>(sizeof(TCHAR))
				: ChildCount * static_cast<int64>(
					sizeof(TSharedPtr<FJsonValue>));
			FUeAgentResourceChargeError ResourceError;
			if (Resources.TryChargeMany(
				{
					{EUeAgentResourceDimension::WorkUnits, 1 + ChildCount},
					{EUeAgentResourceDimension::EstimatedMemoryBytes,
						128 + TextChars * static_cast<int64>(sizeof(TCHAR))
							+ ChildCount * static_cast<int64>(
								sizeof(FHjsonJsonProjectionTask))
							+ ContainerBytes},
				},
				Node.GetProvenance().GetJsonPointer(),
				ResourceError))
			{
				return true;
			}
			OutError.SetFailure(MoveTemp(ResourceError), Node.GetProvenance().GetSourceUnit(),
				Node.GetProvenance().GetJsonPointer());
			OutError.Locate(Node.GetProvenance());
			return false;
		}
	}

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
			mutable FString Lexeme;
			mutable FString Value;
			int32 Start = 0;
			int32 End = 0;
			int64 ByteStart = 0;
			int64 ByteEnd = 0;
			int32 Line = 1;
			int32 Column = 1;
			bool bNewlineBefore = false;
			FStringView BorrowedLexeme, BorrowedValue;
			mutable bool bLexemeBorrowed = false, bValueBorrowed = false;
			FStringView ReadLexeme() const { return bLexemeBorrowed ? BorrowedLexeme : FStringView(GetLexeme()); }
			FStringView ReadValue() const { return Kind == ETokenKind::Bare ? ReadLexeme() : bValueBorrowed ? BorrowedValue : FStringView(Value); }
			const FString& GetValue() const {
				if (Kind == ETokenKind::Bare) return GetLexeme();
				if (bValueBorrowed) { Value = FString(BorrowedValue); bValueBorrowed = false; }
				return Value;
			}
		const FString& GetLexeme() const
			{
				// Delimiters have no owned text consumer in a successful parse.
				// Their exact diagnostic spelling belongs to this tokenizer once.
				if (Kind <= ETokenKind::Comma) {
					static const FString Delimiters[]{TEXT("{"), TEXT("}"), TEXT("["), TEXT("]"), TEXT(":"), TEXT(",")};
					return Delimiters[static_cast<uint8>(Kind)];
					}
				if (bLexemeBorrowed) { Lexeme = FString(BorrowedLexeme); bLexemeBorrowed = false; }
				return Lexeme;
			}
		};

		void SetSyntaxFailure(FWorkspaceError& Error, const FString& Code, const FString& Reason,
			const FString& SourceUnit, const FString& Pointer, int32 Line, int32 Column,
			const FString& Actual, const FString& Expected)
		{
			auto Diagnostic = Contract::FDiagnostic::Failure(Code, Reason, TEXT("hjson_syntax"));
			Diagnostic.Source.Line = Line;
			Diagnostic.Source.Column = Column;
			Diagnostic.ActualValue = MakeShared<FJsonValueString>(Actual);
			if (!Expected.IsEmpty()) Diagnostic.ExpectedValue = MakeShared<FJsonValueString>(Expected);
			Error.SetFailure(MoveTemp(Diagnostic), SourceUnit, Pointer);
		}

		Contract::FDiagnostic WorkspaceValueFailure(const FString& Code, const FString& Reason,
			TSharedPtr<FJsonValue> Actual, TSharedPtr<FJsonValue> Expected = nullptr)
		{
			auto Diagnostic = Contract::FDiagnostic::Failure(Code, Reason, TEXT("workspace_resolve"));
			Diagnostic.ActualValuePresent = Actual.IsValid();
			Diagnostic.ActualValue = MoveTemp(Actual);
			Diagnostic.ExpectedValue = MoveTemp(Expected);
			return Diagnostic;
		}

		FString NodeKindName(EHjsonNodeKind Kind)
		{
			switch (Kind) {
			case EHjsonNodeKind::Null: return TEXT("null");
			case EHjsonNodeKind::Boolean: return TEXT("boolean");
			case EHjsonNodeKind::String: return TEXT("string");
			case EHjsonNodeKind::Integer: return TEXT("integer");
			case EHjsonNodeKind::Decimal: return TEXT("decimal");
			case EHjsonNodeKind::Array: return TEXT("array");
			case EHjsonNodeKind::Object: return TEXT("object");
			default: return FString::Printf(TEXT("unknown node kind %d"), static_cast<int32>(Kind));
			}
		}

		// Scan and implicit-root insertion finish before any frame. Key tokens
		// then retain their text unchanged until every frame has been destroyed.
		struct FParseKeyRefFuncs : DefaultKeyFuncs<FStringView>
		{
			static bool Matches(FStringView A, FStringView B) { return FExactStringKeyFuncs::MatchesView(A, B); }
			static uint32 GetKeyHash(FStringView Key) { return FExactStringKeyFuncs::GetViewKeyHash(Key); }
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
			bool bRetain = true;
			int32 ParsedItems = 0;
			FString Pointer;
			FString OriginalKey;
			FToken StartToken;
			EFrameState State = EFrameState::ValueOrEnd;
			TArray<TPair<FString, FHjsonNodeRef>> Entries;
			TArray<FHjsonNodeRef> Items;
			TSet<FStringView, FParseKeyRefFuncs> Keys;
			TOptional<FStringView> PendingKey; // Original immutable token/input, until attachment.
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

		FString JoinPointer(FStringView Parent, FStringView Segment)
		{
			return PointerText::Child(Parent, Segment, PointerText::ENulMode::ReplaceCompatible);
		}

		struct FParseValueLocation
		{
			FString Pointer;
			const FString* Parent = nullptr;
			FStringView Key;
			int32 ArrayIndex = INDEX_NONE;
			bool bResolved = true;
			void SetRoot(FString InPointer, FStringView InKey) {
				Pointer = MoveTemp(InPointer); Parent = nullptr; Key = InKey; ArrayIndex = INDEX_NONE; bResolved = true;
			}
			void SetChild(const FString& InParent, FStringView InKey, int32 InIndex = INDEX_NONE) {
				Pointer.Reset(); Parent = &InParent; Key = InKey; ArrayIndex = InIndex; bResolved = false;
			}
			const FString& GetPointer() {
				if (!bResolved) {
					Pointer = ArrayIndex == INDEX_NONE ? JoinPointer(*Parent, Key) : JoinPointer(*Parent, ::LexToString(ArrayIndex));
					bResolved = true;
				}
				return Pointer;
			}
			FString TakePointer() { GetPointer(); return MoveTemp(Pointer); }
			FHjsonScalarLocation ReadProjectionLocation() const { return {Parent ? FStringView(*Parent) : FStringView(), Key, ArrayIndex}; }
		};

		bool Charge(
			FUeAgentResourceTracker& Resources,
			std::initializer_list<TPair<EUeAgentResourceDimension, int64>> Amounts,
			const FString& Path,
			FWorkspaceError& OutError)
		{
			FUeAgentResourceChargeError ChargeError;
			if (Resources.TryChargeMany(Amounts, Path, ChargeError))
			{
				return true;
			}
			OutError.SetFailure(MoveTemp(ChargeError));
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

		int32 Utf8WidthAt(const FString& Text, int32 Index, int32& CodeUnits)
		{
			CodeUnits = 1;
			const uint32 Character = static_cast<uint32>(Text[Index]);
			if (Character >= 0xD800 && Character <= 0xDBFF && Index + 1 < Text.Len()) {
				const uint32 Low = static_cast<uint32>(Text[Index + 1]);
				if (Low >= 0xDC00 && Low <= 0xDFFF) { CodeUnits = 2; return 4; }
			}
			return Character <= 0x7F ? 1 : Character <= 0x7FF ? 2 : 3;
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

		bool IsIntegerLexeme(FStringView Value);
		bool IsDecimalLexeme(FStringView Value);

		TCHAR AdvanceSourcePosition(const FString& Text, int32& Index, int32& Line, int32& Column)
		{
			TCHAR Ch = Text[Index++];
			if (Ch == TEXT('\r')) {
				if (Index < Text.Len() && Text[Index] == TEXT('\n')) ++Index;
				Ch = TEXT('\n');
			}
			if (Ch == TEXT('\n')) { ++Line; Column = 1; }
			else ++Column;
			return Ch;
		}

		bool ScanTokens(
			const FString& Text,
			const FString& SourceUnit,
			FUeAgentResourceTracker& Resources,
			TArray<FToken>& OutTokens,
			int32& OutEndLine, int32& OutEndColumn,
			int64& OutSourceBytes,
			FWorkspaceError& OutError,
			FWorkspaceError& Diagnostics,
			const FHjsonOperationCheckpoint* Checkpoint, const FSourceProvenance* Origin, bool bBorrowScalarText,
			int32* FirstContainerEnd = nullptr)
		{
			OutTokens.Reset();
			if (FirstContainerEnd) *FirstContainerEnd = INDEX_NONE;
			// Scan and materialize once. Accounting has no storage-admission limit;
			// a second lexical pass cannot establish another syntax fact. The same
			// scanner advancement supplies byte positions without an offset per char.
			auto Scan = [&]() -> bool
			{
				int32 Index = 0, Line = Origin ? Origin->GetLine() : 1, Column = Origin ? Origin->GetColumn() : 1;
				int64 SourceByte = 0;
				int32 NextCheckpoint = 0;
				bool bNewlineBefore = false;
				struct FScanFrame
				{
					bool bObject;
					EFrameState State;
				};
				TArray<FScanFrame> Frames;
				bool bMatchingExtent = true;
				auto Check = [&](int32 Position) -> bool
				{
					if (Position < NextCheckpoint) return true;
					NextCheckpoint = Position > MAX_int32 - 1024 ? MAX_int32 : Position + 1024;
					return !Checkpoint || (*Checkpoint)(TEXT("hjson_scan"), OutError);
				};
				auto Fail = [&](const TCHAR* Code, const TCHAR* Reason, int32 ErrorLine, int32 ErrorColumn,
					const FString& Actual, const FString& Expected) -> bool
				{
					SetSyntaxFailure(OutError, Code, Reason, SourceUnit, Origin ? Origin->GetJsonPointer() : TEXT("/"), ErrorLine, ErrorColumn, Actual, Expected);
					return false;
				};
				auto Advance = [&]() -> TCHAR
				{
					const int32 Before = Index;
					const TCHAR Ch = AdvanceSourcePosition(Text, Index, Line, Column);
					int32 CodeUnits = 1;
					const uint32 Unit = static_cast<uint32>(Text[Before]);
					const bool bTrailingSurrogate = Unit >= 0xDC00 && Unit <= 0xDFFF && Before > 0
						&& static_cast<uint32>(Text[Before - 1]) >= 0xD800 && static_cast<uint32>(Text[Before - 1]) <= 0xDBFF;
					if (!bTrailingSurrogate) SourceByte += Utf8WidthAt(Text, Before, CodeUnits);
					if (Index == Before + 2) ++SourceByte; // The original Advance consumed CRLF together.
					if (Ch == TEXT('\n')) bNewlineBefore = true;
					return Ch;
				};
				auto IsIndent = [](TCHAR Ch) { return Ch == TEXT(' ') || Ch == TEXT('\t'); };
				while (Index < Text.Len())
				{
					if (!Check(Index)) return false;
					const TCHAR Ch = Text[Index];
					if (FChar::IsWhitespace(Ch)) { Advance(); continue; }
					if (Ch == TEXT('#') || (Ch == TEXT('/') && Index + 1 < Text.Len() && Text[Index + 1] == TEXT('/')))
					{
						while (Index < Text.Len() && Text[Index] != TEXT('\r') && Text[Index] != TEXT('\n'))
						{
							Advance();
							if (!Check(Index)) return false;
						}
						continue;
					}
					if (Ch == TEXT('/') && Index + 1 < Text.Len() && Text[Index + 1] == TEXT('*'))
						return Fail(TEXT("hjson_unsupported_feature"), TEXT("Block comments are not supported; use a line comment."), Line, Column, TEXT("/*"), TEXT("# or //"));

					const int32 Start = Index, TokenLine = Line, TokenColumn = Column;
					const int64 TokenByteStart = SourceByte;
					const bool bTokenNewline = bNewlineBefore;
					bNewlineBefore = false;
					// The implicit object root has the same lexical key/value states
					// as a braced object; quoted and bare values share one scanner.
					if (Frames.IsEmpty() && Ch != TEXT('{') && Ch != TEXT('[')
						&& OutTokens.IsEmpty())
					{
						Frames.Add({true, EFrameState::KeyOrEnd});
					}
					if (!Frames.IsEmpty() && bTokenNewline && Frames.Last().State == EFrameState::AfterValue)
					{
						Frames.Last().State = Frames.Last().bObject ? EFrameState::KeyOrEnd : EFrameState::ValueOrEnd;
					}
					const bool bKey = !Frames.IsEmpty() && Frames.Last().bObject
						&& Frames.Last().State == EFrameState::KeyOrEnd;
					ETokenKind Kind = ETokenKind::Bare;
					int32 ContentStart = Start, ContentEnd = Start, ClosingIndent = 0;
					bool bMultiline = false, bHasEscape = false;
					if (IsPunctuation(Ch))
					{
						Kind = PunctuationKind(Ch);
						Advance();
					}
					else if (Ch == TEXT('\''))
					{
						if (Index + 2 >= Text.Len() || Text[Index + 1] != TEXT('\'') || Text[Index + 2] != TEXT('\''))
							return Fail(TEXT("hjson_unsupported_feature"), TEXT("Single-quoted strings are not supported; use double quotes or a triple-quote multiline string."), Line, Column, FString::Chr(Ch), TEXT("double quote or triple single quote"));
						Kind = ETokenKind::String;
						bMultiline = true;
						Advance(); Advance(); Advance();
						while (Index < Text.Len() && IsIndent(Text[Index]))
						{
							Advance();
							if (!Check(Index)) return false;
						}
						if (Index == Text.Len()) return Fail(TEXT("hjson_unterminated_multiline"), TEXT("The multiline string has no closing triple-quote delimiter."), TokenLine, TokenColumn, TEXT("end of input"), TEXT("closing triple quote"));
						if (Text[Index] != TEXT('\r') && Text[Index] != TEXT('\n'))
							return Fail(TEXT("hjson_multiline_trailing_token"), TEXT("A multiline opening delimiter must be followed by a newline."), Line, Column, FString::Chr(Text[Index]), TEXT("newline"));
						Advance();
						ContentStart = Index;
						bool bClosed = false;
						while (Index < Text.Len())
						{
							const int32 LineStart = Index;
							while (Index < Text.Len() && IsIndent(Text[Index]))
							{
								Advance();
								if (!Check(Index)) return false;
							}
							const int32 Indent = Index - LineStart;
							if (Index + 2 < Text.Len() && Text[Index] == TEXT('\'') && Text[Index + 1] == TEXT('\'') && Text[Index + 2] == TEXT('\''))
							{
								ContentEnd = LineStart;
								ClosingIndent = Indent;
								Advance(); Advance(); Advance();
								while (Index < Text.Len() && IsIndent(Text[Index]))
								{
									Advance();
									if (!Check(Index)) return false;
								}
							if (Index < Text.Len() && Text[Index] != TEXT('\r') && Text[Index] != TEXT('\n'))
							{
								FWorkspaceError Error;
								SetSyntaxFailure(Error, TEXT("hjson_multiline_trailing_token"), TEXT("A multiline closing delimiter must be followed by a newline or end of input."),
				SourceUnit, TEXT("/"), Line, Column, FString::Chr(Text[Index]), TEXT("newline or end of input"));
								Error.MarkUnchecked(TEXT("syntax"), SourceUnit, TEXT("invalid_multiline_closing_line"), Line);
								Diagnostics.Append(Error);
								// The closing delimiter is known. Resume at the next line, without
								// interpreting the malformed suffix as object/array punctuation.
								while (Index < Text.Len() && Text[Index] != TEXT('\r') && Text[Index] != TEXT('\n'))
								{
									Advance();
									if (!Check(Index)) return false;
								}
							}
								bClosed = true;
								break;
							}
							while (Index < Text.Len() && Text[Index] != TEXT('\r') && Text[Index] != TEXT('\n'))
							{
								Advance();
								if (!Check(Index)) return false;
							}
							if (Index < Text.Len()) Advance();
						}
						if (!bClosed) return Fail(TEXT("hjson_unterminated_multiline"), TEXT("The multiline string has no closing triple-quote delimiter."), TokenLine, TokenColumn, TEXT("end of input"), TEXT("closing triple quote"));
					}
					else if (Ch == TEXT('"'))
					{
						Kind = ETokenKind::String;
						Advance();
						ContentStart = Index;
						bool bClosed = false;
						while (Index < Text.Len())
						{
							if (!Check(Index)) return false;
							// Plain ASCII within a quoted token changes neither line nor
							// UTF-16/UTF-8 width. Advance the original scanner's counters
							// once per run; stop at the same checkpoint or special token.
							const int32 RunStart = Index, RunEnd = FMath::Min(Text.Len(), NextCheckpoint);
							while (Index < RunEnd && Text[Index] >= TEXT(' ') && Text[Index] < TCHAR(0x80)
								&& Text[Index] != TEXT('"') && Text[Index] != TEXT('\\')) ++Index;
							if (Index != RunStart) { Column += Index - RunStart; SourceByte += Index - RunStart; continue; }
							if (Text[Index] == TEXT('"')) { ContentEnd = Index; Advance(); bClosed = true; break; }
							if (Text[Index] < 0x20)
								return Fail(TEXT("hjson_unescaped_control"), TEXT("A quoted string contains an unescaped control character."), Line, Column, FString::Printf(TEXT("U+%04X"), static_cast<uint32>(Text[Index])), TEXT("escaped control character"));
							if (Advance() != TEXT('\\')) continue;
							bHasEscape = true;
							if (Index == Text.Len()) break;
							const int32 EscapeLine = Line, EscapeColumn = Column;
							const TCHAR Escaped = Advance();
							switch (Escaped)
							{
							case TEXT('"'): case TEXT('\\'): case TEXT('/'): case TEXT('b'):
							case TEXT('f'): case TEXT('n'): case TEXT('r'): case TEXT('t'): break;
							case TEXT('u'):
								for (int32 Digit = 0; Digit < 4; ++Digit)
								{
									if (Index == Text.Len() || !FChar::IsHexDigit(Text[Index]))
										return Fail(TEXT("hjson_unicode_escape_invalid"), TEXT("A Unicode escape requires exactly four hexadecimal digits."), Line, Column, Index < Text.Len() ? FString::Chr(Text[Index]) : TEXT("end of input"), TEXT("hexadecimal digit"));
									Advance();
								}
								break;
							default: return Fail(TEXT("hjson_escape_invalid"), TEXT("The quoted string contains an unsupported escape sequence."), EscapeLine, EscapeColumn, FString::Chr(Escaped), TEXT("quote, backslash, slash, b, f, n, r, t or u"));
							}
						}
						if (!bClosed) return Fail(TEXT("hjson_string_unclosed"), TEXT("The quoted string has no closing double quote."), Line, Column, TEXT("end of input"), TEXT("closing double quote"));
					}
					else
					{
						bool bPossibleLiteral = true;
						while (Index < Text.Len())
						{
							const TCHAR Current = Text[Index];
							const bool bComment = Current == TEXT('#') || (Current == TEXT('/') && Index + 1 < Text.Len()
								&& (Text[Index + 1] == TEXT('/') || Text[Index + 1] == TEXT('*')));
							if (FChar::IsWhitespace(Current) && !IsIndent(Current)) break;
							if (bKey && (IsPunctuation(Current) || bComment)) break;
							if (!bKey && bPossibleLiteral && (Current == TEXT(',') || Current == TEXT('}') || Current == TEXT(']') || bComment))
							{
								const FStringView Candidate = FStringView(*Text + Start, Index - Start).TrimEnd();
								// Numeric candidates need neither a temporary substring nor
								// its lowercase copy. The original keyword comparison stays
								// explicit for nonnumeric bare text and its Unicode/NUL edge.
								bool bLiteral = IsIntegerLexeme(Candidate) || IsDecimalLexeme(Candidate);
								if (!bLiteral) {
									const FString Folded = FString(Candidate).ToLower();
									bLiteral = Folded == TEXT("true") || Folded == TEXT("false") || Folded == TEXT("null")
										|| Folded == TEXT("nan") || Folded == TEXT("+nan") || Folded == TEXT("-nan")
										|| Folded == TEXT("inf") || Folded == TEXT("+inf") || Folded == TEXT("-inf")
										|| Folded == TEXT("infinity") || Folded == TEXT("+infinity") || Folded == TEXT("-infinity");
								}
								if (bLiteral) break;
								bPossibleLiteral = false;
							}
							Advance();
							if (!Check(Index)) return false;
						}
						if (Index == Start) return Fail(TEXT("hjson_token_invalid"), TEXT("The character cannot begin a token at this input position."), Line, Column, FString::Chr(Text[Index]), TEXT("key, value or delimiter"));
					}

					if (Kind == ETokenKind::OpenObject || Kind == ETokenKind::OpenArray)
					{
						if (!Frames.IsEmpty()) Frames.Last().State = EFrameState::AfterValue;
						Frames.Add({Kind == ETokenKind::OpenObject,
							Kind == ETokenKind::OpenObject ? EFrameState::KeyOrEnd : EFrameState::ValueOrEnd});
					}
					else if (Kind == ETokenKind::CloseObject || Kind == ETokenKind::CloseArray)
					{
						if (!Frames.IsEmpty()) {
							if (FirstContainerEnd && *FirstContainerEnd == INDEX_NONE) {
								bMatchingExtent &= Frames.Last().bObject == (Kind == ETokenKind::CloseObject);
								if (Frames.Num() == 1 && bMatchingExtent) *FirstContainerEnd = OutTokens.Num();
							}
							Frames.Pop(EAllowShrinking::No);
						}
					}
					else if (!Frames.IsEmpty())
					{
						if (Kind == ETokenKind::Colon) Frames.Last().State = EFrameState::ValueOrEnd;
						else if (Kind == ETokenKind::Comma) Frames.Last().State = Frames.Last().bObject ? EFrameState::KeyOrEnd : EFrameState::ValueOrEnd;
						else Frames.Last().State = bKey ? EFrameState::Colon : EFrameState::AfterValue;
					}

					// Revisit source ranges, not allocated line strings. This checks actual
					// leading indentation even when the rest of a short line is long enough.
					FString Value;
					int32 TokenEnd = Index;
					if (Kind == ETokenKind::Bare)
					{
						// Spaces belong to an unquoted name/value; only trailing indentation
						// is outside its lexeme and provenance range.
						while (TokenEnd > Start && IsIndent(Text[TokenEnd - 1])) --TokenEnd;
					}
					const int32 RawLength = TokenEnd - Start;
					const bool bBorrowValue = bBorrowScalarText && Kind == ETokenKind::String && !bMultiline && !bHasEscape;
					if (Kind == ETokenKind::String && !bBorrowValue) Value.Reserve(RawLength + 1);
					if (bMultiline)
					{
						int32 Cursor = ContentStart, ContentLine = TokenLine + 1;
						while (Cursor < ContentEnd)
						{
							const int32 LineStart = Cursor;
							while (Cursor < ContentEnd && IsIndent(Text[Cursor]))
							{
								++Cursor;
								if (Checkpoint && (Cursor - LineStart) % 1024 == 0 && !(*Checkpoint)(TEXT("hjson_multiline_indent"), OutError)) return false;
							}
							const int32 Indent = Cursor - LineStart;
							if (Cursor < ContentEnd && Text[Cursor] != TEXT('\r') && Text[Cursor] != TEXT('\n') && Indent < ClosingIndent)
								return Fail(TEXT("hjson_multiline_indentation"), TEXT("The multiline content is indented less than its closing delimiter."), ContentLine, Indent + 1, FString::FromInt(Indent), FString::Printf(TEXT("at least %d indentation characters"), ClosingIndent));
							Cursor = LineStart + FMath::Min(Indent, ClosingIndent);
							if (LineStart != ContentStart) Value.AppendChar(TEXT('\n'));
							while (Cursor < ContentEnd && Text[Cursor] != TEXT('\r') && Text[Cursor] != TEXT('\n'))
							{
								Value.AppendChar(Text[Cursor]);
								++Cursor;
								if (Checkpoint && (Cursor - LineStart) % 1024 == 0 && !(*Checkpoint)(TEXT("hjson_multiline_content"), OutError)) return false;
							}
							if (Cursor < ContentEnd && Text[Cursor++] == TEXT('\r') && Cursor < ContentEnd && Text[Cursor] == TEXT('\n')) ++Cursor;
							++ContentLine;
							if (Checkpoint && !(*Checkpoint)(TEXT("hjson_multiline_line"), OutError)) return false;
						}
					}
					else if (Kind == ETokenKind::String && !bHasEscape)
					{
						if (!bBorrowValue) Value.AppendChars(*Text + ContentStart, ContentEnd - ContentStart);
					}
					else if (Kind == ETokenKind::String)
					{
						int32 NextDecodeCheckpoint = ContentStart;
						for (int32 Cursor = ContentStart; Cursor < ContentEnd; ++Cursor)
						{
							if (Cursor >= NextDecodeCheckpoint)
							{
								if (Checkpoint && !(*Checkpoint)(TEXT("hjson_string_decode"), OutError)) return false;
								NextDecodeCheckpoint = Cursor > MAX_int32 - 1024 ? MAX_int32 : Cursor + 1024;
							}
							TCHAR Decoded = Text[Cursor];
							if (Decoded == TEXT('\\'))
							{
								Decoded = Text[++Cursor];
								switch (Decoded)
								{
								case TEXT('b'): Decoded = TEXT('\b'); break;
								case TEXT('f'): Decoded = TEXT('\f'); break;
								case TEXT('n'): Decoded = TEXT('\n'); break;
								case TEXT('r'): Decoded = TEXT('\r'); break;
								case TEXT('t'): Decoded = TEXT('\t'); break;
								case TEXT('u'):
								{
									uint32 Code = 0;
									for (int32 Digit = 0; Digit < 4; ++Digit)
									{
										const TCHAR Hex = Text[++Cursor];
										Code = Code * 16 + (FChar::IsDigit(Hex) ? Hex - TEXT('0') : FChar::ToUpper(Hex) - TEXT('A') + 10);
									}
									Decoded = static_cast<TCHAR>(Code);
									break;
								}
								default: break;
								}
							}
							Value.AppendChar(Decoded);
						}
					}
					if (Resources.CollectsUsage() && !Charge(Resources, {
							{EUeAgentResourceDimension::WorkUnits, 1},
							{EUeAgentResourceDimension::EstimatedMemoryBytes,
								2ll * (static_cast<int64>(RawLength) + 1) * static_cast<int64>(sizeof(TCHAR))}},
							TEXT("hjson-tokenize:") + SourceUnit, OutError)) return false;
					if (Checkpoint && !(*Checkpoint)(TEXT("hjson_token_materialize"), OutError)) return false;
					const bool bBorrowLexeme = bBorrowScalarText && (Kind == ETokenKind::Bare || Kind == ETokenKind::String);
					FString Lexeme = !bBorrowLexeme && (Kind == ETokenKind::Bare || Kind == ETokenKind::String)
						? Text.Mid(Start, RawLength) : FString();
					auto& Token = OutTokens.Add_GetRef({Kind, MoveTemp(Lexeme), MoveTemp(Value), Start, TokenEnd,
						TokenByteStart + (Origin ? Origin->GetByteStart() : 0),
						SourceByte - (Index - TokenEnd) + (Origin ? Origin->GetByteStart() : 0), TokenLine, TokenColumn, bTokenNewline});
					if (bBorrowLexeme) { Token.BorrowedLexeme = FStringView(*Text + Start, RawLength); Token.bLexemeBorrowed = true; }
					if (bBorrowValue) { Token.BorrowedValue = FStringView(*Text + ContentStart, ContentEnd - ContentStart); Token.bValueBorrowed = true; }
				}
				OutEndLine = Line; OutEndColumn = Column;
				OutSourceBytes = SourceByte;
				return !Checkpoint || (*Checkpoint)(TEXT("hjson_scan_complete"), OutError);
			};
			if (!Scan()) return false;
			if (Resources.CollectsUsage() && !Charge(Resources, {
				{EUeAgentResourceDimension::EstimatedMemoryBytes,
					(static_cast<int64>(OutTokens.Num()) + 2) * static_cast<int64>(sizeof(FToken))}},
				TEXT("hjson-scanner-storage:") + SourceUnit, OutError)) return false;
			return true;
		}

		bool IsIntegerLexeme(FStringView Value)
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

		bool IsDecimalLexeme(FStringView Value)
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
			const TSharedRef<const FString>& SourceUnit,
			FString&& Pointer,
			FString&& OriginalKey)
		{
			return FSourceProvenance(
				SourceUnit,
				MoveTemp(Pointer),
				Token.Line,
				Token.Column,
				Token.ByteStart,
				Token.ByteEnd,
				MoveTemp(OriginalKey));
		}

		bool MakeScalarNode(
			FToken& Token,
			const FString& SourceUnit,
			const TSharedRef<const FString>& SharedSourceUnit,
			FParseValueLocation& Location,
			FUeAgentResourceTracker& Resources,
			FHjsonNodePtr& OutNode,
			FWorkspaceError& OutError, bool bMaterialize = true,
			TFunctionRef<bool(const FHjsonScalarLocation&, const FHjsonParsedScalar&)>* SelectScalar = nullptr)
		{
			if (Token.Kind != ETokenKind::Bare
				&& Token.Kind != ETokenKind::String)
			{
				SetSyntaxFailure(OutError, TEXT("hjson_value_expected"), TEXT("Expected a scalar HJSON value at this position."),
					SourceUnit, Location.GetPointer(), Token.Line, Token.Column, Token.GetLexeme(), TEXT("string, number, boolean or null"));
				return false;
			}
			EHjsonNodeKind Kind = EHjsonNodeKind::String;
			// The scanner already validated quoted-string grammar/escapes. The
			// synchronous scalar consumer can use its exact text without making
			// owned raw/decoded copies which no AST node will ever consume.
			if (Token.Kind == ETokenKind::String && SelectScalar && (*SelectScalar)(Location.ReadProjectionLocation(),
				{Kind, Token.ReadValue(), Token.ReadLexeme(), false, Token.Line, Token.Column, Token.ByteStart, Token.ByteEnd,
					Token.bValueBorrowed, Token.bLexemeBorrowed})) {
				OutNode.Reset(); return true;
				}
			FString Text;
			bool bBoolean = false;
			const FStringView Value = Token.ReadValue();
			// The former FString == TCHAR* tests were case-insensitive and
			// terminated at the first NUL. A borrowed view must keep that grammar.
			const TCHAR First = Value.IsEmpty() ? TCHAR(0) : FChar::ToLower(Value[0]);
			const bool bKeywordCandidate = Token.Kind == ETokenKind::Bare
				&& (First == TEXT('t') || First == TEXT('f') || First == TEXT('n') || First == TEXT('i') || First == TEXT('+') || First == TEXT('-'));
			int32 ComparisonLength = 0;
			if (bKeywordCandidate)
				while (ComparisonLength < Value.Len() && Value[ComparisonLength] != TCHAR(0)) ++ComparisonLength;
			const FStringView NativeComparison = Value.Left(ComparisonLength);
			TCHAR Folded[10]; Folded[0] = TCHAR(0);
			if (bKeywordCandidate && Value.Len() <= 9) {
				for (int32 Index = 0; Index < Value.Len(); ++Index) Folded[Index] = FChar::ToLower(Value[Index]);
				Folded[Value.Len()] = TCHAR(0);
			}
			if (Token.Kind == ETokenKind::Bare
				&& (FCString::Stricmp(Folded, TEXT("true")) == 0 || FCString::Stricmp(Folded, TEXT("false")) == 0
					|| FCString::Stricmp(Folded, TEXT("null")) == 0)
				&& !NativeComparison.Equals(FStringView(Folded), ESearchCase::IgnoreCase))
			{
				SetSyntaxFailure(OutError, TEXT("hjson_ambiguous_literal"), TEXT("Boolean and null literals must use lowercase; quote this token to use it as text."),
					SourceUnit, Location.GetPointer(), Token.Line, Token.Column, FString(Value), FString(Value.Len(), Folded));
				return false;
			}
			if (Token.Kind == ETokenKind::Bare && NativeComparison.Equals(TEXT("null"), ESearchCase::IgnoreCase))
			{
				Kind = EHjsonNodeKind::Null;
				Text.Reset();
			}
			else if (Token.Kind == ETokenKind::Bare
				&& (NativeComparison.Equals(TEXT("true"), ESearchCase::IgnoreCase) || NativeComparison.Equals(TEXT("false"), ESearchCase::IgnoreCase)))
			{
				Kind = EHjsonNodeKind::Boolean;
				bBoolean = NativeComparison.Equals(TEXT("true"), ESearchCase::IgnoreCase);
				Text.Reset();
			}
			else if (Token.Kind == ETokenKind::Bare
				&& (FCString::Stricmp(Folded, TEXT("nan")) == 0
					|| FCString::Stricmp(Folded, TEXT("+nan")) == 0
					|| FCString::Stricmp(Folded, TEXT("-nan")) == 0
					|| FCString::Stricmp(Folded, TEXT("infinity")) == 0
					|| FCString::Stricmp(Folded, TEXT("+infinity")) == 0
					|| FCString::Stricmp(Folded, TEXT("-infinity")) == 0
					|| FCString::Stricmp(Folded, TEXT("inf")) == 0
					|| FCString::Stricmp(Folded, TEXT("+inf")) == 0
					|| FCString::Stricmp(Folded, TEXT("-inf")) == 0))
			{
				SetSyntaxFailure(OutError, TEXT("hjson_nonfinite_number"), TEXT("HJSON numbers must be finite."),
					SourceUnit, Location.GetPointer(), Token.Line, Token.Column, FString(Value), TEXT("finite number"));
				return false;
			}
			else if (Token.Kind == ETokenKind::Bare
				&& IsIntegerLexeme(Value))
			{
				Kind = EHjsonNodeKind::Integer;
				Text.Reset();
			}
			else if (Token.Kind == ETokenKind::Bare
				&& IsDecimalLexeme(Value))
			{
				// A decimal token is exact authoring data, not yet a native double.
				// Its destination codec owns the native range check. NaN/Inf
				// literals are rejected separately above.
				Kind = EHjsonNodeKind::Decimal;
				Text.Reset();
			}
			if (Token.Kind != ETokenKind::String && SelectScalar && (*SelectScalar)(Location.ReadProjectionLocation(),
				{Kind, Value, Token.ReadLexeme(), bBoolean, Token.Line, Token.Column, Token.ByteStart, Token.ByteEnd,
					Token.bValueBorrowed, Token.bLexemeBorrowed}))
				bMaterialize = false;
			if (!bMaterialize) { OutNode.Reset(); return true; }
			Token.GetLexeme(); // A retained AST node owns its exact original spelling.
			if (Kind == EHjsonNodeKind::String) {
				if (Token.Kind == ETokenKind::String) { Token.GetValue(); Text = MoveTemp(Token.Value); }
				else Text = FString(Value);
			}
			const int64 StringBytes = Token.ByteEnd - Token.ByteStart;
			if (!ChargeNode(Resources, Location.GetPointer(), StringBytes, 0, OutError))
			{
				return false;
			}
			OutNode = MakeShared<FHjsonNode>(
				Kind,
				MakeProvenance(
					Token,
					SharedSourceUnit,
					Location.TakePointer(),
					FString(Location.Key)),
				MoveTemp(Token.Lexeme),
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

	FString EscapeJsonPointerSegment(FString Segment)
	{
		for (TCHAR Character : Segment) if (Character == TEXT('~') || Character == TEXT('/')) {
			FString Result; Result.Reserve(Segment.Len());
			PointerText::AppendEscaped(Result, Segment, PointerText::ENulMode::ReplaceCompatible);
			return Result;
		}
		return Segment;
	}

	FUeAgentResourceTracker MakeSemanticWorkspaceResourceTracker()
	{
		return FUeAgentResourceTracker(false);
	}

	TSharedRef<FUeAgentResourceTracker, ESPMode::ThreadSafe>
	MakeSemanticWorkspaceResourceTrackerOwner()
	{
		return MakeShared<FUeAgentResourceTracker, ESPMode::ThreadSafe>(false);
	}

	namespace
	{
		bool IsPublicWorkspaceSource(const FString& Source)
		{
			return Source.EndsWith(TEXT(".hjson")) && !Source.Contains(TEXT(".uai/")) && !Source.Contains(TEXT(".uai\\"));
		}
	}

	TSharedPtr<FJsonValue> HjsonDiagnosticValue(const FHjsonNode* Node)
	{
		if (!Node) return nullptr;
		switch (Node->GetKind())
		{
		case EHjsonNodeKind::Null: return MakeShared<FJsonValueNull>();
		case EHjsonNodeKind::Boolean: return MakeShared<FJsonValueBoolean>(Node->GetBoolean());
		case EHjsonNodeKind::String: return MakeShared<FJsonValueString>(Node->GetText());
		case EHjsonNodeKind::Integer:
		case EHjsonNodeKind::Decimal: return MakeShared<FJsonValueNumberString>(Node->GetLexeme());
		case EHjsonNodeKind::Array:
		case EHjsonNodeKind::Object:
		{
			const auto Summary = MakeShared<FJsonObject>();
			const bool bArray = Node->GetKind() == EHjsonNodeKind::Array;
			Summary->SetStringField(TEXT("type"), bArray ? TEXT("array") : TEXT("object"));
			Summary->SetNumberField(TEXT("count"), bArray ? Node->GetItems().Num() : Node->GetEntries().Num());
			return MakeShared<FJsonValueObject>(Summary);
		}
		default: return nullptr;
		}
	}

	Contract::FDiagnostic HjsonNodeFailure(const FString& Code, const FString& Reason,
		const FString& Stage, const FHjsonNode& Node, TSharedPtr<FJsonValue> Expected, bool bPublicActual)
	{
		auto Diagnostic = Contract::FDiagnostic::Failure(Code, Reason, Stage);
		Diagnostic.ExpectedValue = MoveTemp(Expected);
		Diagnostic.ActualValuePresent = true;
		if (bPublicActual) Diagnostic.ActualValue = HjsonDiagnosticValue(&Node);
		else
		{
			Diagnostic.ActualEvidence = HjsonDiagnosticValue(&Node);
			const TCHAR* Kind = TEXT("unknown");
			switch (Node.GetKind())
			{
			case EHjsonNodeKind::Null: Kind = TEXT("null"); break;
			case EHjsonNodeKind::Boolean: Kind = TEXT("boolean"); break;
			case EHjsonNodeKind::String: Kind = TEXT("string"); break;
			case EHjsonNodeKind::Integer: Kind = TEXT("integer"); break;
			case EHjsonNodeKind::Decimal: Kind = TEXT("decimal"); break;
			case EHjsonNodeKind::Array: Kind = TEXT("array"); break;
			case EHjsonNodeKind::Object: Kind = TEXT("object"); break;
			}
			Diagnostic.ActualValue = MakeShared<FJsonValueString>(Kind);
		}
		const auto& Source = Node.GetProvenance();
		Diagnostic.Source.SourceUnit = Source.GetSourceUnit();
		Diagnostic.Source.InternalJsonPath = Source.GetJsonPointer();
		if (IsPublicWorkspaceSource(Source.GetSourceUnit()))
		{
			Diagnostic.Source.SourceFile = Source.GetSourceUnit();
			Diagnostic.Source.JsonPath = Source.GetJsonPointer();
			Diagnostic.Source.Line = Source.GetLine();
			Diagnostic.Source.Column = Source.GetColumn();
			Diagnostic.PropertyPath = Source.GetJsonPointer();
			Diagnostic.SourceKind = TEXT("authored_value");
		}
		return Diagnostic;
	}

	Contract::FDiagnostic HjsonFieldFailure(const FString& Code, const FString& Reason,
		const FString& Stage, const FHjsonNode& Object, const FString& Field,
		TSharedPtr<FJsonValue> Expected, bool bPublicActual)
	{
		if (const auto* Node = Object.Get(Field))
			return HjsonNodeFailure(Code, Reason, Stage, *Node, MoveTemp(Expected), bPublicActual);
		auto Diagnostic = HjsonNodeFailure(Code, Reason, Stage, Object, MoveTemp(Expected), bPublicActual);
		Diagnostic.ActualValue.Reset();
		Diagnostic.ActualEvidence.Reset();
		Diagnostic.ActualValuePresent = false;
		Diagnostic.ExpectedValuePresent = true;
		const FString& Parent = Object.GetProvenance().GetJsonPointer();
		const FString Pointer = (Parent == TEXT("/") ? FString() : Parent) + TEXT("/") + EscapeJsonPointerSegment(Field);
		Diagnostic.Source.InternalJsonPath = Pointer;
		if (!Diagnostic.Source.SourceFile.IsEmpty())
		{
			Diagnostic.Source.JsonPath = Pointer;
			Diagnostic.PropertyPath = Pointer;
			Diagnostic.SemanticPointer = Pointer;
			Diagnostic.SourceKind = TEXT("related_input");
		}
		return Diagnostic;
	}

	void FWorkspaceError::SetFailure(Contract::FDiagnostic Failure, const FString& FallbackSource,
		const FString& FallbackJsonPointer)
	{
		Reset();
		Diagnostic = MoveTemp(Failure);
		const bool bNativeResult = Diagnostic.SourceKind == TEXT("native_result");
		if (Diagnostic.TargetPath.IsEmpty() && (FPackageName::IsValidObjectPath(FallbackSource)
			|| FPackageName::IsValidLongPackageName(FallbackSource)))
		{
			Diagnostic.TargetPath = FallbackSource;
			if (!bNativeResult && Diagnostic.PropertyPath.IsEmpty() && FallbackJsonPointer != TEXT("/"))
				Diagnostic.PropertyPath = FallbackJsonPointer;
		}
		if (bNativeResult) return;
		const bool bHasPointer = !Diagnostic.Source.JsonPath.IsEmpty()
			|| !Diagnostic.SemanticPointer.IsEmpty() || !Diagnostic.Source.InternalJsonPath.IsEmpty();
		if (Diagnostic.Source.SourceUnit.IsEmpty())
			Diagnostic.Source.SourceUnit = Diagnostic.Source.SourceFile.IsEmpty() ? FallbackSource : Diagnostic.Source.SourceFile;
		if (Diagnostic.Source.SourceFile.IsEmpty() && IsPublicWorkspaceSource(FallbackSource))
			Diagnostic.Source.SourceFile = FallbackSource;
		if (Diagnostic.Source.InternalJsonPath.IsEmpty())
			Diagnostic.Source.InternalJsonPath = !Diagnostic.Source.JsonPath.IsEmpty() ? Diagnostic.Source.JsonPath
				: !Diagnostic.SemanticPointer.IsEmpty() ? Diagnostic.SemanticPointer : FallbackJsonPointer;
		if (!bHasPointer)
		{
			if (!Diagnostic.Source.SourceFile.IsEmpty()) Diagnostic.Source.JsonPath = FallbackJsonPointer;
			else Diagnostic.SemanticPointer = FallbackJsonPointer;
		}
	}

	void FWorkspaceError::Append(const FWorkspaceError& Error)
	{
		if (!Error.HasErrors()) return;
		if (!HasErrors())
		{
			auto PriorUnchecked = MoveTemp(Diagnostic.Unchecked);
			*this = Error;
			Diagnostic.Unchecked.Append(PriorUnchecked);
			return;
		}
		Diagnostic.Append(Error.Diagnostic);
	}

	void FWorkspaceError::Locate(const FSourceProvenance& Source)
	{
		TMap<FString, FSourceProvenance> Sources;
		Sources.Add(Diagnostic.Source.SourceUnit, Source);
		LocateUnits(Sources);
	}

	void FWorkspaceError::Locate(const FHjsonDocument& Document)
	{
		if (!IsPublicWorkspaceSource(Document.GetSourceUnit())) return;
		TFunction<void(Contract::FDiagnostic&)> LocateItem = [&](Contract::FDiagnostic& Item)
		{
			if (Item.SourceKind != TEXT("native_result") && Item.SourceKind != TEXT("command_input")
				&& Item.Source.Line == 0 && !Item.Source.JsonPath.IsEmpty()
				&& (Item.Source.SourceFile == Document.GetSourceUnit()
					|| (Item.Source.SourceFile.IsEmpty() && (Item.Source.SourceUnit.IsEmpty() || Item.Source.SourceUnit == Document.GetSourceUnit()))))
			{
				if (const auto* Node = Document.Find(Item.Source.JsonPath))
				{
					Item.Source.SourceFile = Document.GetSourceUnit();
					Item.Source.Line = Node->GetProvenance().GetLine();
					Item.Source.Column = Node->GetProvenance().GetColumn();
				}
			}
			for (auto& Child : Item.Diagnostics) LocateItem(Child);
			if (Item.PublicCause)
			{
				auto Cause = MakeShared<Contract::FDiagnostic>(*Item.PublicCause);
				LocateItem(*Cause);
				Item.PublicCause = MoveTemp(Cause);
			}
		};
		LocateItem(Diagnostic);
	}

	void FWorkspaceError::PrependPath(const FString& PointerPrefix)
	{
		PrependPath(PointerPrefix, false);
	}

	void FWorkspaceError::PrependPath(const FString& PointerPrefix, bool bInputOnly)
	{
		if (PointerPrefix.IsEmpty() || PointerPrefix == TEXT("/")) return;
		const auto Qualify = [&](FString& Path)
		{
			if (Path.StartsWith(TEXT("/"))) Path = PointerPrefix + (Path == TEXT("/") ? FString() : Path);
		};
		TFunction<void(Contract::FDiagnostic&)> QualifyItem = [&](Contract::FDiagnostic& Item)
		{
			if (Item.Source.Line <= 0 && (!bInputOnly
				|| (Item.SourceKind != TEXT("native_result") && Item.SourceKind != TEXT("command_input"))))
			{
				Qualify(Item.Source.JsonPath);
				Qualify(Item.Source.InternalJsonPath);
				Qualify(Item.SemanticPointer);
				Qualify(Item.PropertyPath);
			}
			for (auto& Child : Item.Diagnostics) QualifyItem(Child);
			if (Item.PublicCause)
			{
				auto Cause = MakeShared<Contract::FDiagnostic>(*Item.PublicCause);
				QualifyItem(*Cause);
				Item.PublicCause = MoveTemp(Cause);
			}
		};
		QualifyItem(Diagnostic);
	}

	void FWorkspaceError::Locate(const FHjsonNode& Root)
	{
		const auto& RootSource = Root.GetProvenance();
		if (!IsPublicWorkspaceSource(RootSource.GetSourceUnit())) return;
		TFunction<void(Contract::FDiagnostic&)> LocateItem = [&](Contract::FDiagnostic& Item)
		{
			for (auto& Child : Item.Diagnostics) LocateItem(Child);
			if (Item.PublicCause)
			{
				auto Cause = MakeShared<Contract::FDiagnostic>(*Item.PublicCause);
				LocateItem(*Cause);
				Item.PublicCause = MoveTemp(Cause);
			}
			if (Item.Source.Line > 0 || Item.SourceKind == TEXT("native_result")
				|| Item.SourceKind == TEXT("command_input")) return;
			if (!Item.Source.SourceFile.IsEmpty() && Item.Source.SourceFile != RootSource.GetSourceUnit()) return;
			FString Pointer = !Item.Source.InternalJsonPath.IsEmpty() ? Item.Source.InternalJsonPath
				: !Item.SemanticPointer.IsEmpty() ? Item.SemanticPointer : Item.Source.JsonPath;
			const FString& Base = RootSource.GetJsonPointer();
			if (!Base.IsEmpty() && Base != TEXT("/")
				&& (Pointer == Base || Pointer.StartsWith(Base + TEXT("/")))) Pointer.RightChopInline(Base.Len());
			const bool bHasPointer = !Pointer.IsEmpty() && Pointer != TEXT("/");
			const FString Requested = Pointer;
			const FHjsonNode* Node = Root.Find(Pointer);
			const bool bExact = Node && bHasPointer;
			while (!Node)
			{
				int32 Slash = INDEX_NONE;
				if (!Pointer.FindLastChar(TEXT('/'), Slash) || Slash == 0) break;
				Pointer.LeftInline(Slash);
				Node = Root.Find(Pointer);
			}
			if (!Node) Node = &Root;
			const auto& Source = Node->GetProvenance();
			Item.Source.SourceFile = Source.GetSourceUnit();
			if (Item.Source.SourceUnit.IsEmpty()) Item.Source.SourceUnit = Source.GetSourceUnit();
			Item.Source.JsonPath = Source.GetJsonPointer();
			Item.Source.Line = Source.GetLine();
			Item.Source.Column = Source.GetColumn();
			// Keep the requested field even when only its parent exists in the input.
			Item.Source.InternalJsonPath = (Base == TEXT("/") ? FString() : Base)
				+ (bHasPointer ? Requested : FString());
			Item.SourceKind = bExact && Item.SourceKind != TEXT("related_input")
				? TEXT("authored_value") : TEXT("related_input");
		};
		LocateItem(Diagnostic);
	}

	void FWorkspaceError::MarkUnchecked(const FString& Stage, const FString& Source,
		const FString& Reason, int32 FromLine)
	{
		const FString SourceFile = IsPublicWorkspaceSource(Source) ? Source : FString();
		for (const auto& Value : Diagnostic.Unchecked)
		{
			if (!Value || Value->Type != EJson::Object) continue;
			const auto& Existing = Value->AsObject();
			FString ExistingStage, ExistingReason, ExistingSource;
			double ExistingLine = 0;
			Existing->TryGetStringField(TEXT("stage"), ExistingStage);
			Existing->TryGetStringField(TEXT("reason"), ExistingReason);
			Existing->TryGetStringField(TEXT("source_file"), ExistingSource);
			Existing->TryGetNumberField(TEXT("from_line"), ExistingLine);
			if (ExistingStage == Stage && ExistingReason == Reason
				&& ExistingSource == SourceFile && ExistingLine == FMath::Max(0, FromLine)) return;
		}
		const auto Item = MakeShared<FJsonObject>();
		Item->SetStringField(TEXT("stage"), Stage);
		Item->SetStringField(TEXT("reason"), Reason);
		if (!SourceFile.IsEmpty()) Item->SetStringField(TEXT("source_file"), SourceFile);
		if (FromLine > 0) Item->SetNumberField(TEXT("from_line"), FromLine);
		Diagnostic.Unchecked.Add(MakeShared<FJsonValueObject>(Item));
	}

	void FWorkspaceError::LocateUnits(const TMap<FString, FSourceProvenance>& Sources)
	{
		TArray<TPair<Contract::FDiagnostic*, const FSourceProvenance*>> Pending;
		Pending.Emplace(&Diagnostic, nullptr);
		while (!Pending.IsEmpty())
		{
			const auto Visit = Pending.Pop(EAllowShrinking::No);
			auto& Item = *Visit.Key;
			const auto* Source = Sources.Find(Item.Source.SourceUnit);
			const bool bMappedUnit = Source && !Item.Source.SourceUnit.IsEmpty()
				&& Item.Source.SourceUnit != Source->GetSourceUnit();
			if (!Source && Item.Source.SourceUnit.IsEmpty() && Item.Source.SourceFile.IsEmpty()) Source = Visit.Value;
			if (Source && IsPublicWorkspaceSource(Source->GetSourceUnit())
				&& Item.SourceKind != TEXT("native_result") && Item.SourceKind != TEXT("command_input"))
			{
				if (!IsPublicWorkspaceSource(Item.Source.SourceFile))
				{
					if (Item.Source.SourceUnit.IsEmpty()) Item.Source.SourceUnit = Source->GetSourceUnit();
					Item.Source.SourceFile = Source->GetSourceUnit();
					if (bMappedUnit)
					{
						// An explicit logical-unit mapping provides its physical node location.
						Item.Source.JsonPath = Source->GetJsonPointer();
						Item.Source.Line = Source->GetLine();
						Item.Source.Column = Source->GetColumn();
					}
				}
				if (Item.Source.SourceFile == Source->GetSourceUnit())
				{
					if (Item.Source.JsonPath.IsEmpty() || Item.Source.JsonPath == TEXT("/"))
					{
						Item.Source.JsonPath = Source->GetJsonPointer();
					}
					// A parent's position is not the exact position of a different child pointer.
					if (Item.Source.JsonPath == Source->GetJsonPointer())
					{
						if (Item.Source.Line <= 0) Item.Source.Line = Source->GetLine();
						if (Item.Source.Column <= 0) Item.Source.Column = Source->GetColumn();
					}
				}
			}
			for (auto& Child : Item.Diagnostics) Pending.Emplace(&Child, Source);
			if (Item.PublicCause)
			{
				// The shared cause is immutable; enrich a complete copy, without rebuilding its facts.
				auto Cause = MakeShared<Contract::FDiagnostic>(*Item.PublicCause);
				Item.PublicCause = Cause;
				Pending.Emplace(&Cause.Get(), Source);
			}
		}
	}

	const FString& FWorkspaceError::GetCode() const { return Diagnostic.Code; }
	const FString& FWorkspaceError::GetSourceUnit() const { return Diagnostic.Source.SourceUnit; }
	const FString& FWorkspaceError::GetJsonPointer() const
	{ return Diagnostic.Source.InternalJsonPath; }
	int64 FWorkspaceError::GetObserved() const { return Diagnostic.ResourceObserved.Get(0); }
	int32 FWorkspaceError::GetLine() const { return Diagnostic.Source.Line; }
	int32 FWorkspaceError::GetColumn() const { return Diagnostic.Source.Column; }
	bool FWorkspaceError::IsStoppedEarly() const { return Diagnostic.StoppedEarly.Get(false); }
	const TArray<FString>& FWorkspaceError::GetCyclePath() const { return Diagnostic.CyclePath; }

	void FWorkspaceError::Recode(const FString& InCode, const FString& PublicReason)
	{
		if (Diagnostic.Code == InCode) return;
		Diagnostic.PublicCause = MakeShared<Contract::FDiagnostic>(Diagnostic);
		Diagnostic.CauseCode = Diagnostic.Code;
		Diagnostic.Code = InCode;
		Diagnostic.PublicMessage = PublicReason;
	}

	void FWorkspaceError::Reset()
	{
		Diagnostic.Reset();
		Diagnostic.Source.InternalJsonPath = TEXT("/");
	}

	void FWorkspaceError::SetSourceFailure(FString InCode, FString PublicReason, FString InSourceUnit, FString InJsonPointer,
		int64 InObserved, bool bInStoppedEarly, TArray<FString> InCyclePath, int32 InLine, int32 InColumn)
	{
		Reset();
		Diagnostic = Contract::FDiagnostic::Failure(MoveTemp(InCode), MoveTemp(PublicReason), TEXT("workspace_resolve"));
		Diagnostic.Source.SourceUnit = MoveTemp(InSourceUnit);
		const FString Pointer = InJsonPointer.IsEmpty() ? TEXT("/") : MoveTemp(InJsonPointer);
		Diagnostic.Source.InternalJsonPath = Pointer;
		if (IsPublicWorkspaceSource(Diagnostic.Source.SourceUnit))
		{
			Diagnostic.Source.SourceFile = Diagnostic.Source.SourceUnit;
			Diagnostic.Source.JsonPath = Pointer;
		}
		else if (!Diagnostic.Source.SourceUnit.Contains(TEXT(".uai/")) && !Diagnostic.Source.SourceUnit.Contains(TEXT(".uai\\")))
			Diagnostic.SemanticPointer = Pointer;
		if (FPackageName::IsValidObjectPath(Diagnostic.Source.SourceUnit)
			|| FPackageName::IsValidLongPackageName(Diagnostic.Source.SourceUnit))
		{
			Diagnostic.TargetPath = Diagnostic.Source.SourceUnit;
			if (Pointer != TEXT("/")) Diagnostic.PropertyPath = Pointer;
		}
		if (InObserved != 0 || bInStoppedEarly) Diagnostic.ResourceObserved = InObserved;
		if (bInStoppedEarly) Diagnostic.StoppedEarly = true;
		Diagnostic.CyclePath = MoveTemp(InCyclePath);
		Diagnostic.Source.Line = InLine;
		Diagnostic.Source.Column = InColumn;
	}

	FSourceProvenance::FSourceProvenance(
		FString InSourceUnit,
		FString InJsonPointer,
		int32 InLine,
		int32 InColumn,
		int64 InByteStart,
		int64 InByteEnd,
		FString InOriginalKey)
		: FSourceProvenance(MakeShared<const FString>(MoveTemp(InSourceUnit)),
			MoveTemp(InJsonPointer), InLine, InColumn, InByteStart, InByteEnd, MoveTemp(InOriginalKey))
	{
	}

	FSourceProvenance::FSourceProvenance(
		TSharedRef<const FString> InSourceUnit, FString InJsonPointer,
		int32 InLine, int32 InColumn, int64 InByteStart, int64 InByteEnd, FString InOriginalKey)
		: SourceUnit(MoveTemp(InSourceUnit))
		, JsonPointer(MoveTemp(InJsonPointer))
		, Line(InLine)
		, Column(InColumn)
		, ByteStart(InByteStart)
		, ByteEnd(InByteEnd)
		, OriginalKey(MoveTemp(InOriginalKey))
	{
	}

	const FString& FSourceProvenance::GetSourceUnit() const { return *SourceUnit; }
	const FString& FSourceProvenance::GetJsonPointer() const { return JsonPointer; }
	int32 FSourceProvenance::GetLine() const { return Line; }
	int32 FSourceProvenance::GetColumn() const { return Column; }
	int64 FSourceProvenance::GetByteStart() const { return ByteStart; }
	int64 FSourceProvenance::GetByteEnd() const { return ByteEnd; }
	const FString& FSourceProvenance::GetOriginalKey() const { return OriginalKey; }

	struct FHjsonNodeRelocation
	{
		explicit FHjsonNodeRelocation(FHjsonNodeRef InStorage) : Storage(MoveTemp(InStorage)) {}
		FHjsonNodeRef Storage;
		FCriticalSection Lock;
		bool bChildrenReady = false;
		TArray<FHjsonNodeRef> Items;
		TArray<TPair<FString, FHjsonNodeRef>> Entries;

		void EnsureChildren(const FSourceProvenance& NewOrigin)
		{
			FScopeLock Guard(&Lock);
			if (bChildrenReady) return;
			const auto& OldOrigin = Storage->GetProvenance();
			const int64 ByteDelta = NewOrigin.GetByteStart() - OldOrigin.GetByteStart();
			const int32 LineDelta = NewOrigin.GetLine() - OldOrigin.GetLine();
			const int32 ColumnDelta = NewOrigin.GetColumn() - OldOrigin.GetColumn();
			auto MoveSource = [&](FHjsonNodeRef Child) -> FHjsonNodeRef {
				const auto& P = Child->GetProvenance();
				if (ByteDelta == 0 && LineDelta == 0 && (ColumnDelta == 0 || P.GetLine() != OldOrigin.GetLine())) return Child;
				return MakeShared<FHjsonNode>(Child, FSourceProvenance(P.GetSourceUnitBacking(), P.GetJsonPointer(),
					P.GetLine() + LineDelta, P.GetColumn() + (P.GetLine() == OldOrigin.GetLine() ? ColumnDelta : 0),
					P.GetByteStart() + ByteDelta, P.GetByteEnd() + ByteDelta, P.GetOriginalKey()));
			};
			Items.Reserve(Storage->GetItems().Num());
			for (const auto& Child : Storage->GetItems()) Items.Add(MoveSource(Child));
			Entries.Reserve(Storage->GetEntries().Num());
			for (const auto& Entry : Storage->GetEntries()) Entries.Emplace(Entry.Key, MoveSource(Entry.Value));
			bChildrenReady = true;
		}
	};

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

	FHjsonNode::FHjsonNode(FHjsonNodeRef Unchanged, FSourceProvenance InProvenance)
		: Kind(Unchanged->GetKind()), Provenance(MoveTemp(InProvenance))
		, Relocation(MakeShared<FHjsonNodeRelocation>(Unchanged->Relocation ? Unchanged->Relocation->Storage : Unchanged))
	{
	}

	const FHjsonNode* FHjsonNode::GetValueStorageIdentity() const
	{
		return Relocation ? &Relocation->Storage.Get() : this;
	}

	int64 FHjsonNode::GetOwnedStorageBytes(TArray<const FHjsonNode*>& OutRetainedNodes) const
	{
		int64 Bytes = sizeof(*this) + 32 + Lexeme.GetAllocatedSize() + Text.GetAllocatedSize()
			+ Provenance.GetJsonPointer().GetAllocatedSize() + Provenance.GetOriginalKey().GetAllocatedSize()
			+ Items.GetAllocatedSize() + Entries.GetAllocatedSize();
		for (const auto& Child : Items) OutRetainedNodes.Add(&Child.Get());
		for (const auto& Entry : Entries) { Bytes += Entry.Key.GetAllocatedSize(); OutRetainedNodes.Add(&Entry.Value.Get()); }
		if (Relocation) {
			FScopeLock Guard(&Relocation->Lock);
			Bytes += sizeof(FHjsonNodeRelocation) + 32 + Relocation->Items.GetAllocatedSize() + Relocation->Entries.GetAllocatedSize();
			OutRetainedNodes.Add(&Relocation->Storage.Get());
			for (const auto& Child : Relocation->Items) OutRetainedNodes.Add(&Child.Get());
			for (const auto& Entry : Relocation->Entries) { Bytes += Entry.Key.GetAllocatedSize(); OutRetainedNodes.Add(&Entry.Value.Get()); }
		}
		return Bytes;
	}

	EHjsonNodeKind FHjsonNode::GetKind() const { return Kind; }
	const FSourceProvenance& FHjsonNode::GetProvenance() const { return Provenance; }
	const FString& FHjsonNode::GetLexeme() const { return Relocation ? Relocation->Storage->GetLexeme() : Lexeme; }
	const FString& FHjsonNode::GetText() const { return Relocation ? Relocation->Storage->GetText() : Text; }
	bool FHjsonNode::GetBoolean() const { return Relocation ? Relocation->Storage->GetBoolean() : bBoolean; }
	const TArray<FHjsonNodeRef>& FHjsonNode::GetItems() const
	{
		if (!Relocation) return Items;
		Relocation->EnsureChildren(Provenance); return Relocation->Items;
	}
	const TArray<TPair<FString, FHjsonNodeRef>>& FHjsonNode::GetEntries() const
	{
		if (!Relocation) return Entries;
		Relocation->EnsureChildren(Provenance); return Relocation->Entries;
	}

	const FHjsonNode* FHjsonNode::Get(const FString& Key) const
	{
		for (const TPair<FString, FHjsonNodeRef>& Entry : GetEntries())
		{
			if (Entry.Key.Equals(Key, ESearchCase::CaseSensitive))
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
		bSourceFromRoot = SourceUnit.Equals(Root->GetProvenance().GetSourceUnit(), ESearchCase::CaseSensitive);
		if (bSourceFromRoot) SourceUnit.Empty();
	}

	FHjsonDocument::FHjsonDocument(FHjsonNodeRef InRoot)
		: Root(MoveTemp(InRoot)), bSourceFromRoot(true)
	{
	}

	const FString& FHjsonDocument::GetSourceUnit() const { return bSourceFromRoot ? Root->GetProvenance().GetSourceUnit() : SourceUnit; }
	const FHjsonNodeRef& FHjsonDocument::GetRoot() const { return Root; }

	const FHjsonNode* FHjsonDocument::Find(const FString& JsonPointer) const
	{
		return Root->Find(JsonPointer);
	}

	const FHjsonNode* FHjsonNode::Find(const FString& JsonPointer) const
	{
		if (JsonPointer.IsEmpty() || JsonPointer == TEXT("/"))
		{
			return this;
		}
		if (!JsonPointer.StartsWith(TEXT("/")))
		{
			return nullptr;
		}
		TArray<FString> Segments;
		JsonPointer.Mid(1).ParseIntoArray(Segments, TEXT("/"), false);
		const FHjsonNode* Current = this;
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
		TArray<FString> InReferenceFields)
		: DialectId(NormalizeRequired(MoveTemp(InDialectId)))
		, ContractId(NormalizeRequired(MoveTemp(InContractId)))
		, ContractVersion(InContractVersion)
		, ProfileId(NormalizeRequired(MoveTemp(InProfileId)))
		, EntryUnit(MoveTemp(InEntryUnit))
		, ReferenceFields(MoveTemp(InReferenceFields))
	{
		FPaths::NormalizeFilename(EntryUnit);
		FPaths::RemoveDuplicateSlashes(EntryUnit);
		bValid = !DialectId.IsEmpty()
			&& !ContractId.IsEmpty()
			&& ContractVersion > 0
			&& !ProfileId.IsEmpty()
			&& !EntryUnit.IsEmpty();
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

	FExportFilePlan::FExportFilePlan(
		FString InRelativePath,
		TArray<uint8> InCanonicalUtf8,
		FString InContentHash,
		TArray<FIntPoint> InGeneratedFragmentSpans,
		TSharedPtr<const FFragmentIdentities> InGeneratedFragmentIdentities)
		: RelativePath(MoveTemp(InRelativePath))
		, CanonicalUtf8(MakeShared<const TArray<uint8>>(MoveTemp(InCanonicalUtf8)))
		, ContentHash(MoveTemp(InContentHash))
		, RootHeaderFacts(FindHjsonRootHeaderFacts(ContentHash))
		, GeneratedFragmentIdentities(MoveTemp(InGeneratedFragmentIdentities))
	{
		if (!InGeneratedFragmentSpans.IsEmpty())
			GeneratedFragmentSpans = MakeShared<const TArray<FIntPoint>>(MoveTemp(InGeneratedFragmentSpans));
	}

	const FString& FExportFilePlan::GetRelativePath() const { return RelativePath; }
	const TArray<uint8>& FExportFilePlan::GetCanonicalUtf8() const
	{
		return CanonicalUtf8.Get();
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
		int64 InObservedTokens, TMap<FString, FString> InRetainedFileHashes, TOptional<TSet<FString>> InPendingAuthorDocuments)
		: WorkspaceKind(InWorkspaceKind)
		, EntryFile(MoveTemp(InEntryFile))
		, SnapshotId(MoveTemp(InSnapshotId))
		, SnapshotRevision(MoveTemp(InSnapshotRevision))
		, SnapshotEvidenceId(MoveTemp(InSnapshotEvidenceId))
		, Files(MoveTemp(InFiles))
		, RetainedFileHashes(MoveTemp(InRetainedFileHashes))
		, PendingAuthorDocuments(MoveTemp(InPendingAuthorDocuments))
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

	void RememberParsedHjsonRootHeader(const FString& ContentHash, const FHjsonDocument& Document,
		const TMap<EUeAgentResourceDimension, int64>& ParseCharges)
	{
		PreviousParsedRootHeader.Reset();
		if (ContentHash.IsEmpty() || Document.GetRoot()->GetKind() != EHjsonNodeKind::Object) return;
		auto Header = MakeShared<FHjsonRootHeaderFacts>();
		int64 Characters = 0;
		for (const auto& Pair : Document.GetRoot()->GetEntries())
		{
			if (Pair.Value->GetKind() != EHjsonNodeKind::String) continue;
			Characters += Pair.Key.Len() + Pair.Value->GetText().Len();
			if (Characters > 65536) return;
			Header->Strings.Emplace(Pair.Key, Pair.Value->GetText());
		}
		Header->ContentHash = ContentHash;
		Header->ParseCharges = ParseCharges;
		PreviousParsedRootHeader = MoveTemp(Header);
	}

	void RememberFormattedHjsonRootHeader(const FString& ContentHash, const FUeAgentCanonicalValue& Root)
	{
		PreviousParsedRootHeader.Reset();
		if (ContentHash.IsEmpty() || Root.GetKind() != EUeAgentValueKind::Object) return;
		auto Header = MakeShared<FHjsonRootHeaderFacts>();
		int64 Characters = 0;
		for (const auto& Pair : Root.GetObjectItems()) {
			if (Pair.Key != TEXT("workspace_schema") && Pair.Key != TEXT("publication_generation") && Pair.Key != TEXT("publication_state")) continue;
			if (Pair.Value->GetKind() != EUeAgentValueKind::String) continue;
			Characters += Pair.Key.Len() + Pair.Value->GetText().Len();
			if (Characters > 65536) return;
			Header->Strings.Emplace(Pair.Key, Pair.Value->GetText());
		}
		Header->ContentHash = ContentHash;
		PreviousParsedRootHeader = MoveTemp(Header);
	}

	FHjsonRootHeaderFactsPtr FindHjsonRootHeaderFacts(const FString& ContentHash)
	{
		return PreviousParsedRootHeader && PreviousParsedRootHeader->ContentHash == ContentHash ? PreviousParsedRootHeader : nullptr;
	}

	bool TryReadHjsonRootString(const FHjsonRootHeaderFactsPtr& Facts, const FString& ContentHash, const FString& Field, FString& OutValue,
		TMap<EUeAgentResourceDimension, int64>* OutParseCharges)
	{
		if (!Facts || !Facts->ContentHash.Equals(ContentHash, ESearchCase::CaseSensitive)) return false;
		for (const auto& Pair : Facts->Strings)
			if (Pair.Key.Equals(Field, ESearchCase::CaseSensitive))
			{
				OutValue = Pair.Value;
				if (OutParseCharges) *OutParseCharges = Facts->ParseCharges;
				return true;
			}
		return false;
	}

	bool TryReuseHjsonRootString(const FString& ContentHash, const FString& Field, FString& OutValue,
		TMap<EUeAgentResourceDimension, int64>* OutParseCharges)
	{
		return TryReadHjsonRootString(PreviousParsedRootHeader, ContentHash, Field, OutValue, OutParseCharges);
	}

	static bool TryParseHjsonDocumentImpl(
		const FString& Text,
		const FString& SourceUnit,
		FUeAgentResourceTracker& Resources,
		FHjsonDocumentPtr& OutDocument,
		FWorkspaceError& OutError,
		const FHjsonOperationCheckpoint* Checkpoint, const FSourceProvenance* Origin,
		TFunctionRef<bool(const FString&, const FString&)>* IncludeSubtree = nullptr,
		TFunctionRef<bool(const FHjsonScalarLocation&, const FHjsonParsedScalar&)>* SelectScalar = nullptr,
		TArray<FToken>* ScalarTokenOwner = nullptr, bool* OutExtendsIntoParent = nullptr)
	{
		OutDocument.Reset();
		if (OutExtendsIntoParent) *OutExtendsIntoParent = false;
		OutError.Reset();
		FWorkspaceError Diagnostics;
		bool bReachedEnd = false;
		ON_SCOPE_EXIT
		{
			if (OutError.HasErrors())
			{
				if (!bReachedEnd) OutError.MarkUnchecked(TEXT("syntax"), SourceUnit,
					TEXT("cannot_reliably_resume_after_error"), OutError.GetLine());
				Diagnostics.Append(OutError);
			}
			if (Diagnostics.HasErrors())
			{
				Diagnostics.MarkUnchecked(TEXT("structure_and_semantics"), SourceUnit, TEXT("syntax_errors"));
				OutError = MoveTemp(Diagnostics);
				OutDocument.Reset();
			}
		};
		if (SourceUnit.IsEmpty())
		{
			OutError.SetSourceFailure(TEXT("hjson_source_unit_missing"), TEXT("Parsing HJSON requires a nonempty source unit."));
			return false;
		}
		const auto SyntaxUsageBefore = Resources.Snapshot();
		if (!Origin && !IncludeSubtree && !SelectScalar && TryRestoreHjsonSyntax(Text, SourceUnit, Resources, OutDocument, OutError, Checkpoint)) {
			bReachedEnd = true;
			return true;
		}
		if (OutError.HasErrors()) return false;
		int64 SourceUtf8Length = 0;
		TArray<FToken> LocalTokens;
		TArray<FToken>& Tokens = ScalarTokenOwner ? *ScalarTokenOwner : LocalTokens;
		Tokens.Reset();
		int32 EndLine = 1, EndColumn = 1;
		int32 FirstContainerEnd = INDEX_NONE;
		if (!ScanTokens(Text, SourceUnit, Resources, Tokens, EndLine, EndColumn, SourceUtf8Length, OutError, Diagnostics, Checkpoint, Origin, SelectScalar != nullptr,
			OutExtendsIntoParent ? &FirstContainerEnd : nullptr))
		{
			return false;
		}
		// The mapped old delimiters may now enclose several siblings. Select
		// their parent from the original scanner's frames before parsing a root.
		if (OutExtendsIntoParent && !Diagnostics.HasErrors() && FirstContainerEnd != INDEX_NONE
			&& FirstContainerEnd + 1 < Tokens.Num()) {
			*OutExtendsIntoParent = true;
			return true;
		}
		if (Resources.CollectsUsage() && !Charge(
			Resources,
			{
				{EUeAgentResourceDimension::InputBytes, SourceUtf8Length},
				{EUeAgentResourceDimension::EstimatedMemoryBytes,
					SourceUtf8Length * 2ll},
				{EUeAgentResourceDimension::WorkUnits, 1},
			},
			TEXT("hjson:") + SourceUnit,
			OutError))
		{
			return false;
		}
		if (Tokens.IsEmpty())
		{
			SetSyntaxFailure(OutError, TEXT("hjson_empty_document"), TEXT("The HJSON document contains no value."),
				SourceUnit, TEXT("/"), 1, 1, TEXT("empty document"), TEXT("one root value"));
			return false;
		}
		// Advance already consumed every code unit with this exact CRLF/column
		// grammar. Preserve the stage checkpoint without rescanning the document.
		if (Checkpoint && !(*Checkpoint)(TEXT("hjson_end_position"), OutError)) return false;
		const bool bImplicitRootObject = Tokens.Num() >= 2
			&& (Tokens[0].Kind == ETokenKind::Bare
				|| Tokens[0].Kind == ETokenKind::String)
			&& Tokens[1].Kind == ETokenKind::Colon;
		if (bImplicitRootObject)
		{
			if (Resources.CollectsUsage() && !Charge(Resources, {
					{EUeAgentResourceDimension::WorkUnits, 2},
					{EUeAgentResourceDimension::EstimatedMemoryBytes,
						2 * static_cast<int64>(sizeof(FToken))}},
				TEXT("hjson-implicit-root:") + SourceUnit, OutError))
			{
				return false;
			}
			const int32 FirstLine = Tokens[0].Line, FirstColumn = Tokens[0].Column;
			const bool bFirstNewline = Tokens[0].bNewlineBefore;
			Tokens.Insert({ETokenKind::OpenObject, {}, {},
				0, 0, Origin ? Origin->GetByteStart() : 0, Origin ? Origin->GetByteStart() : 0, FirstLine, FirstColumn, bFirstNewline}, 0);
			const int64 EndByte = SourceUtf8Length + (Origin ? Origin->GetByteStart() : 0);
			Tokens.Add({ETokenKind::CloseObject, {}, {},
				Text.Len(), Text.Len(), EndByte, EndByte,
				EndLine, EndColumn, true});
		}

		const TSharedRef<const FString> SharedSourceUnit = MakeShared<const FString>(SourceUnit);
		int32 TokenIndex = 0;
		FHjsonNodePtr Root;
		TArray<FParseFrame> Frames;
		FParseValueLocation Expected;
		Expected.SetRoot(Origin ? Origin->GetJsonPointer() : TEXT("/"), Origin ? FStringView(Origin->GetOriginalKey()) : FStringView());
		bool bExpectingValue = true;
		auto Attach = [&](FHjsonNodePtr Node) -> bool
		{
			if (Frames.IsEmpty())
			{
				if (Root.IsValid())
				{
					SetSyntaxFailure(OutError, TEXT("hjson_trailing_content"), TEXT("The document contains extra content after its root value."),
				SourceUnit, TEXT("/"), Node->GetProvenance().GetLine(), Node->GetProvenance().GetColumn(), Node->GetLexeme(), TEXT("end of input"));
					return false;
				}
				Root = Node;
				bExpectingValue = false;
				return true;
			}
			FParseFrame& Parent = Frames.Last();
			if (Parent.bObject)
			{
				if (Parent.bRetain && Node) Parent.Entries.Emplace(FString(Parent.PendingKey.GetValue()), Node.ToSharedRef());
				Parent.PendingKey.Reset();
			}
			else
			{
				if (Parent.bRetain && Node) Parent.Items.Add(Node.ToSharedRef());
				++Parent.ParsedItems;
			}
			Parent.State = EFrameState::AfterValue;
			bExpectingValue = false;
			return true;
		};

		while (TokenIndex < Tokens.Num() || !Frames.IsEmpty())
		{
			if (Checkpoint && !(*Checkpoint)(TEXT("hjson_parse"), OutError))
			{
				return false;
			}
			if (bExpectingValue)
			{
				if (!Tokens.IsValidIndex(TokenIndex))
				{
					SetSyntaxFailure(OutError, TEXT("hjson_value_missing"), TEXT("A value is required before the next delimiter or end of input."),
						SourceUnit, Expected.GetPointer(), EndLine, EndColumn, TEXT("end of input"), TEXT("HJSON value"));
					return false;
				}
				FToken& Token = Tokens[TokenIndex++];
				if (!Frames.IsEmpty() && (Token.Kind == ETokenKind::Comma
					|| (Frames.Last().bObject && Token.Kind == ETokenKind::CloseObject)
					|| (!Frames.Last().bObject && Token.Kind == ETokenKind::CloseArray)))
				{
					FWorkspaceError Error;
					SetSyntaxFailure(Error, TEXT("hjson_value_missing"), TEXT("A value is required before the next delimiter or end of input."),
						SourceUnit, Expected.GetPointer(), Token.Line, Token.Column, Token.GetLexeme(), TEXT("HJSON value"));
					Diagnostics.Append(Error);
					--TokenIndex;
					Frames.Last().PendingKey.Reset();
					Frames.Last().State = EFrameState::AfterValue;
					bExpectingValue = false;
					continue;
				}
				const bool bRetain = Frames.IsEmpty() || (Frames.Last().bRetain
					&& (!IncludeSubtree || (*IncludeSubtree)(Expected.GetPointer(), FString(Expected.Key))));
				if (Token.Kind == ETokenKind::OpenObject
					|| Token.Kind == ETokenKind::OpenArray)
				{
					if (bRetain && !ChargeNode(
						Resources,
							Expected.GetPointer(),
						0,
						0,
						OutError))
					{
						return false;
					}
					FParseFrame Frame;
					Frame.bRetain = bRetain;
					Frame.bObject = Token.Kind == ETokenKind::OpenObject;
					Frame.Pointer = Expected.TakePointer();
					Frame.OriginalKey = FString(Expected.Key);
					Frame.StartToken = MoveTemp(Token);
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
					SharedSourceUnit,
					Expected,
					Resources,
					Scalar,
					OutError, bRetain, SelectScalar)
					|| !Attach(Scalar))
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
				SetSyntaxFailure(OutError, TEXT("hjson_container_unclosed"), TEXT("The HJSON container has no closing delimiter."),
				SourceUnit, Frames.Last().Pointer, EndLine, EndColumn, TEXT("end of input"), Frames.Last().bObject ? TEXT("}") : TEXT("]"));
				return false;
			}
			FParseFrame& Frame = Frames.Last();
			FToken& Token = Tokens[TokenIndex];
			if (Frame.bObject)
			{
				if (Frame.State == EFrameState::KeyOrEnd)
				{
					if (Token.Kind == ETokenKind::CloseObject)
					{
						const FToken& EndToken = Token;
						++TokenIndex;
						FParseFrame Closed = MoveTemp(Frames.Last());
						Frames.Pop(EAllowShrinking::No);
						int64 KeyBytes = 0;
						if (Resources.CollectsUsage()) for (const TPair<FString, FHjsonNodeRef>& Entry
							: Closed.Entries)
						{
							KeyBytes += FTCHARToUTF8(*Entry.Key).Length();
						}
						if (Resources.CollectsUsage() && !Charge(
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
							MoveTemp(Closed.Pointer),
							OutError))
						{
							return false;
						}
						FHjsonNodePtr Container;
						if (Closed.bRetain) Container = MakeShared<FHjsonNode>(
							EHjsonNodeKind::Object,
							FSourceProvenance(
								SharedSourceUnit,
								MoveTemp(Closed.Pointer),
								Closed.StartToken.Line,
								Closed.StartToken.Column,
								Closed.StartToken.ByteStart,
								EndToken.ByteEnd,
								MoveTemp(Closed.OriginalKey)),
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
					if ((Token.Kind != ETokenKind::Bare
						&& Token.Kind != ETokenKind::String)
						|| (Token.Kind == ETokenKind::Bare
						&& (Token.ReadValue().Contains(TEXT(" ")) || Token.ReadValue().Contains(TEXT("\t")))))
					{
						SetSyntaxFailure(OutError, TEXT("hjson_object_key_expected"), TEXT("Expected a quoted key or an unquoted key without whitespace."),
				SourceUnit, Frame.Pointer, Token.Line, Token.Column, Token.GetLexeme(), TEXT("object key"));
						return false;
					}
					bool bDuplicate = false;
					Frame.Keys.Add(Token.ReadValue(), &bDuplicate);
					if (bDuplicate)
					{
						SetSyntaxFailure(OutError, TEXT("hjson_duplicate_key"), TEXT("Each key must be unique within its object."),
				SourceUnit, JoinPointer(Frame.Pointer, Token.GetValue()), Token.Line, Token.Column, Token.GetValue(), TEXT("unique object key"));
						Diagnostics.Append(OutError);
						OutError.Reset();
					}
					Frame.PendingKey = Token.ReadValue();
					Frame.State = EFrameState::Colon;
					++TokenIndex;
					continue;
				}
				if (Frame.State == EFrameState::Colon)
				{
					if (Token.Kind != ETokenKind::Colon)
					{
						SetSyntaxFailure(OutError, TEXT("hjson_colon_expected"), TEXT("Expected ':' after the object key."),
						SourceUnit, JoinPointer(Frame.Pointer, Frame.PendingKey.GetValue()), Token.Line, Token.Column, Token.GetLexeme(), TEXT(":"));
						// Only resume where a value token is already unambiguous.
						if (Token.Kind != ETokenKind::OpenObject && Token.Kind != ETokenKind::OpenArray
							&& Token.Kind != ETokenKind::Bare && Token.Kind != ETokenKind::String) return false;
						Diagnostics.Append(OutError);
						OutError.Reset();
						Expected.SetChild(Frame.Pointer, Frame.PendingKey.GetValue());
						Frame.State = EFrameState::AwaitingValue;
						bExpectingValue = true;
						continue;
					}
					Expected.SetChild(Frame.Pointer, Frame.PendingKey.GetValue());
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
					SetSyntaxFailure(OutError, TEXT("hjson_object_separator_expected"), TEXT("Expected a comma, newline or closing brace after the object value."),
				SourceUnit, Frame.Pointer, Token.Line, Token.Column, Token.GetLexeme(), TEXT("comma, newline or }"));
					if ((Token.Kind != ETokenKind::Bare && Token.Kind != ETokenKind::String)
						|| !Tokens.IsValidIndex(TokenIndex + 1)
						|| Tokens[TokenIndex + 1].Kind != ETokenKind::Colon) return false;
					Diagnostics.Append(OutError);
					OutError.Reset();
					Frame.State = EFrameState::KeyOrEnd;
					continue;
				}
			}
			else
			{
				if (Frame.State == EFrameState::ValueOrEnd)
				{
					if (Token.Kind == ETokenKind::CloseArray)
					{
						const FToken& EndToken = Token;
						++TokenIndex;
						FParseFrame Closed = MoveTemp(Frames.Last());
						Frames.Pop(EAllowShrinking::No);
						if (Resources.CollectsUsage() && !Charge(
							Resources,
							{
								{EUeAgentResourceDimension::Items,
									Closed.Items.Num()},
								{EUeAgentResourceDimension::WorkUnits,
									Closed.Items.Num()},
								{EUeAgentResourceDimension::EstimatedMemoryBytes,
									Closed.Items.Num() * 24ll},
							},
							MoveTemp(Closed.Pointer),
							OutError))
						{
							return false;
						}
						FHjsonNodePtr Container;
						if (Closed.bRetain) Container = MakeShared<FHjsonNode>(
							EHjsonNodeKind::Array,
							FSourceProvenance(
								SharedSourceUnit,
								MoveTemp(Closed.Pointer),
								Closed.StartToken.Line,
								Closed.StartToken.Column,
								Closed.StartToken.ByteStart,
								EndToken.ByteEnd,
								MoveTemp(Closed.OriginalKey)),
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
					Expected.SetChild(Frame.Pointer, FStringView(), Frame.ParsedItems);
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
					SetSyntaxFailure(OutError, TEXT("hjson_array_separator_expected"), TEXT("Expected a comma, newline or closing bracket after the array value."),
				SourceUnit, Frame.Pointer, Token.Line, Token.Column, Token.GetLexeme(), TEXT("comma, newline or ]"));
					if (Token.Kind != ETokenKind::Bare && Token.Kind != ETokenKind::String
						&& Token.Kind != ETokenKind::OpenObject && Token.Kind != ETokenKind::OpenArray) return false;
					Diagnostics.Append(OutError);
					OutError.Reset();
					Frame.State = EFrameState::ValueOrEnd;
					continue;
				}
			}
		}
		if (!Root.IsValid())
		{
			SetSyntaxFailure(OutError, TEXT("hjson_document_missing_root"), TEXT("The parser did not produce a root value for the document."),
				SourceUnit, TEXT("/"), EndLine, EndColumn, TEXT("no root value"), TEXT("one root value"));
			return false;
		}
		if (TokenIndex != Tokens.Num())
		{
			const FToken& Trailing = Tokens[TokenIndex];
			SetSyntaxFailure(OutError, TEXT("hjson_trailing_content"), TEXT("The document contains extra content after its root value."),
				SourceUnit, TEXT("/"), Trailing.Line, Trailing.Column, Trailing.GetLexeme(), TEXT("end of input"));
			return false;
		}
		bReachedEnd = true;
		if (Diagnostics.HasErrors()) return false;
		OutDocument = MakeShared<FHjsonDocument>(
			SourceUnit,
			Root.ToSharedRef());
		if (!Origin && !IncludeSubtree && !SelectScalar) RememberHjsonSyntax(Text, SourceUnit, *OutDocument, SyntaxUsageBefore, Resources.Snapshot());
		return true;
	}
	bool TryParseHjsonDocument(const FString& Text, const FString& SourceUnit,
		FUeAgentResourceTracker& Resources, FHjsonDocumentPtr& OutDocument, FWorkspaceError& OutError,
		const FHjsonOperationCheckpoint* Checkpoint)
	{
		return TryParseHjsonDocumentImpl(Text, SourceUnit, Resources, OutDocument, OutError, Checkpoint, nullptr);
	}
	bool TryParseHjsonSelectedNodes(const FString& Text, const FString& SourceUnit,
		FUeAgentResourceTracker& Resources, FHjsonNodePtr& OutRoot, FWorkspaceError& OutError,
		TFunctionRef<bool(const FString&, const FString&)> IncludeSubtree,
		const FHjsonOperationCheckpoint* Checkpoint)
	{
		OutRoot.Reset();
		FHjsonDocumentPtr Projection;
		if (!TryParseHjsonDocumentImpl(Text, SourceUnit, Resources, Projection, OutError, Checkpoint, nullptr, &IncludeSubtree)) return false;
		OutRoot = Projection->GetRoot();
		return true;
	}

	bool TryConsumeHjsonScalarProjection(const FString& Text, const FString& SourceUnit,
		FUeAgentResourceTracker& Resources,
		TFunctionRef<bool(const FHjsonScalarLocation&, const FHjsonParsedScalar&)> SelectScalar,
		TFunctionRef<bool(const FHjsonNode&, FWorkspaceError&)> ConsumeRoot,
		FWorkspaceError& OutError, const FHjsonOperationCheckpoint* Checkpoint)
	{
		TArray<FToken> Tokens;
		FHjsonDocumentPtr Projection;
		if (!TryParseHjsonDocumentImpl(Text, SourceUnit, Resources, Projection, OutError,
			Checkpoint, nullptr, nullptr, &SelectScalar, &Tokens)) return false;
		return ConsumeRoot(*Projection->GetRoot(), OutError);
	}

	bool TryParseHjsonDocumentSpan(const FString& Text, const FSourceProvenance& Span,
		FUeAgentResourceTracker& Resources, FHjsonDocumentPtr& OutDocument, FWorkspaceError& OutError,
		const FHjsonOperationCheckpoint* Checkpoint)
	{
		OutDocument.Reset(); OutError.Reset();
		int32 Line = 1, Column = 1;
		int32 Position = 0, Start = INDEX_NONE, End = INDEX_NONE, StartLine = 1, StartColumn = 1;
		int32 NextCheckpoint = 0;
		int64 Bytes = 0;
		if (Span.GetByteStart() >= 0 && Span.GetByteEnd() > Span.GetByteStart()) {
			while (Position <= Text.Len() && Bytes <= Span.GetByteEnd()) {
				if (Bytes == Span.GetByteStart()) { Start = Position; StartLine = Line; StartColumn = Column; }
				if (Bytes == Span.GetByteEnd()) { End = Position; break; }
				if (Position == Text.Len()) break;
				if (Position >= NextCheckpoint) {
					if (Checkpoint && !(*Checkpoint)(TEXT("hjson_span_position"), OutError)) return false;
					NextCheckpoint = Position > MAX_int32 - 1024 ? MAX_int32 : Position + 1024;
				}
				int32 CodeUnits = 1;
				const int32 Width = Utf8WidthAt(Text, Position, CodeUnits), Before = Position;
				AdvanceSourcePosition(Text, Position, Line, Column);
				if (CodeUnits == 2) AdvanceSourcePosition(Text, Position, Line, Column);
				Bytes += Width + (Position - Before - CodeUnits); // CRLF consumes both ASCII bytes.
			}
		}
		if (Start == INDEX_NONE || End == INDEX_NONE) {
			OutError.SetSourceFailure(TEXT("hjson_source_span_invalid"), TEXT("The requested source span is outside the observed document."), Span.GetSourceUnit());
			return false;
		}
		const FSourceProvenance Origin(Span.GetSourceUnit(), Span.GetJsonPointer(), StartLine, StartColumn,
			Span.GetByteStart(), Span.GetByteEnd(), Span.GetOriginalKey());
		// No complete UTF-8 or prefix copy for every row. Only this consumed
		// fragment is materialized; width and CRLF rules are the original scanner's.
		return TryParseHjsonDocumentImpl(Text.Mid(Start, End - Start), Span.GetSourceUnit(),
			Resources, OutDocument, OutError, Checkpoint, &Origin);
	}

    static TOptional<FSourceProvenance> LocateGeneratedSpan(const TArray<uint8>& Utf8, const FSourceProvenance& Span)
    {
		if (Span.GetByteStart() < 0 || Span.GetByteEnd() <= Span.GetByteStart() || Span.GetByteEnd() > Utf8.Num()) return {};
		int32 Line = Span.GetLine(), Column = Span.GetColumn();
		if (Line <= 0 || Column <= 0) {
			// Only line/column are consumed. Do not manufacture and walk a
			// complete decoded prefix for every requested row. UTF-8 cannot
			// contain CR/LF inside a multibyte character; count those actual
			// delimiters, then let the original scanner consume the final line.
			const uint8* Begin = Utf8.GetData(); const uint8* End = Begin + Span.GetByteStart();
			const uint8* LineStart = Begin; Line = Column = 1;
			for (const uint8* Cursor = Begin; Cursor < End;) {
				const auto* Newline = static_cast<const uint8*>(std::memchr(Cursor, '\n', End - Cursor));
				if (!Newline) break;
				++Line; LineStart = Newline + 1; Cursor = Newline + 1;
			}
			for (const uint8* Cursor = Begin; Cursor < End;) {
				const auto* Return = static_cast<const uint8*>(std::memchr(Cursor, '\r', End - Cursor));
				if (!Return) break;
				if (Return + 1 == End && End < Begin + Utf8.Num() && *End == '\n') {
					return {};
				}
				if (Return + 1 == End || Return[1] != '\n') { ++Line; if (Return >= LineStart) LineStart = Return + 1; }
				Cursor = Return + 1;
			}
			const FUTF8ToTCHAR LastLine(reinterpret_cast<const ANSICHAR*>(LineStart), static_cast<int32>(End - LineStart));
			const FString PrefixText(LastLine.Length(), LastLine.Get()); int32 Position = 0;
			while (Position < PrefixText.Len()) AdvanceSourcePosition(PrefixText, Position, Line, Column);
		}
		return FSourceProvenance(Span.GetSourceUnit(), Span.GetJsonPointer(), Line, Column,
			Span.GetByteStart(), Span.GetByteEnd(), Span.GetOriginalKey());
    }
	bool TryParseGeneratedHjsonSpan(const TArray<uint8>& Utf8, const FSourceProvenance& Span,
		FUeAgentResourceTracker& Resources, FHjsonDocumentPtr& OutDocument, FWorkspaceError& OutError,
		TFunctionRef<bool(const FString&, const FString&)>* IncludeSubtree)
	{
		OutDocument.Reset(); OutError.Reset();
		if (Span.GetByteStart() < 0 || Span.GetByteEnd() <= Span.GetByteStart() || Span.GetByteEnd() > Utf8.Num()) {
			OutError.SetSourceFailure(TEXT("hjson_source_span_invalid"),
				TEXT("The generated source span is outside its formatter bytes."), Span.GetSourceUnit());
			return false;
		}
		const FUTF8ToTCHAR Decoded(reinterpret_cast<const ANSICHAR*>(Utf8.GetData() + Span.GetByteStart()),
			static_cast<int32>(Span.GetByteEnd() - Span.GetByteStart()));
		const auto Origin = LocateGeneratedSpan(Utf8, Span);
		if (!Origin) { OutError.SetSourceFailure(TEXT("hjson_source_span_invalid"), TEXT("The generated row starts inside a CRLF sequence."), Span.GetSourceUnit()); return false; }
		return TryParseHjsonDocumentImpl(FString(Decoded.Length(), Decoded.Get()), Span.GetSourceUnit(),
			Resources, OutDocument, OutError, nullptr, &Origin.GetValue(), IncludeSubtree);
	}

	bool TryConsumeGeneratedHjsonSpan(const TArray<uint8>& Utf8, const FSourceProvenance& Span,
		FUeAgentResourceTracker& Resources, TFunctionRef<bool(const FHjsonNode&, FWorkspaceError&)> Consumer,
		FWorkspaceError& OutError, TFunctionRef<bool(const FString&, const FString&)>* IncludeSubtree)
	{
		const bool bDeferred = Span.GetLine() <= 0 || Span.GetColumn() <= 0;
		const FSourceProvenance Local(Span.GetSourceUnitBacking(), Span.GetJsonPointer(), bDeferred ? 1 : Span.GetLine(),
			bDeferred ? 1 : Span.GetColumn(), Span.GetByteStart(), Span.GetByteEnd(), Span.GetOriginalKey());
		FHjsonDocumentPtr Parsed;
		if (TryParseGeneratedHjsonSpan(Utf8, Local, Resources, Parsed, OutError, IncludeSubtree)
			&& Consumer(*Parsed->GetRoot(), OutError)) return true;
		if (!OutError.HasErrors()) OutError.SetSourceFailure(TEXT("hjson_generated_fragment_consumer_failed"),
			TEXT("The generated fragment consumer returned no result or diagnostic. No alternate parser was selected."), Span.GetSourceUnit(), Span.GetJsonPointer());
		if (!bDeferred) return false;
		RelocateGeneratedHjsonFragmentError(Utf8, Span, OutError);
		return false;
	}

	bool TryConsumeGeneratedHjsonFragment(TConstArrayView<uint8> Bytes, const FSourceProvenance& Span,
		FUeAgentResourceTracker& Resources, TFunctionRef<bool(const FHjsonNode&, FWorkspaceError&)> Consumer,
		FWorkspaceError& OutError, TFunctionRef<bool(const FString&, const FString&)>* IncludeSubtree,
		FHjsonFragmentConsumeTimes* Times)
	{
		OutError.Reset();
		if (Span.GetByteStart() < 0 || Span.GetByteEnd() <= Span.GetByteStart()
			|| Span.GetByteEnd() - Span.GetByteStart() != Bytes.Num()) {
			OutError.SetSourceFailure(TEXT("hjson_source_span_invalid"), TEXT("The addressed bytes do not match their formatter row span."), Span.GetSourceUnit()); return false;
		}
		const bool bDeferred = Span.GetLine() <= 0 || Span.GetColumn() <= 0;
		const FSourceProvenance Origin(Span.GetSourceUnitBacking(), Span.GetJsonPointer(), bDeferred ? 1 : Span.GetLine(),
			bDeferred ? 1 : Span.GetColumn(), Span.GetByteStart(), Span.GetByteEnd(), Span.GetOriginalKey());
		double Started = Times ? FPlatformTime::Seconds() : 0;
		const FUTF8ToTCHAR Decoded(reinterpret_cast<const ANSICHAR*>(Bytes.GetData()), Bytes.Num());
		FString Text(Decoded.Length(), Decoded.Get());
		if (Times) { Times->Decode += FPlatformTime::Seconds() - Started; Started = FPlatformTime::Seconds(); }
		FHjsonDocumentPtr Parsed;
		const bool bParsed = TryParseHjsonDocumentImpl(Text, Span.GetSourceUnit(),
			Resources, Parsed, OutError, nullptr, &Origin, IncludeSubtree);
		if (Times) Times->Parse += FPlatformTime::Seconds() - Started;
		const bool bConsumed = bParsed && Consumer(*Parsed->GetRoot(), OutError);
		Started = Times ? FPlatformTime::Seconds() : 0;
		Parsed.Reset(); Text.Empty();
		if (Times) Times->Retire += FPlatformTime::Seconds() - Started;
		if (bConsumed) return true;
		if (!OutError.HasErrors()) OutError.SetSourceFailure(TEXT("hjson_generated_fragment_consumer_failed"),
			TEXT("The generated fragment consumer returned no result or diagnostic. No alternate parser was selected."), Span.GetSourceUnit(), Span.GetJsonPointer());
		return false;
	}

	void RelocateGeneratedHjsonFragmentError(const TArray<uint8>& Utf8, const FSourceProvenance& Span, FWorkspaceError& OutError)
	{
		const auto Absolute = LocateGeneratedSpan(Utf8, Span);
		if (!Absolute) return; // Keep the original parser/consumer diagnostic.
		auto Failure = OutError.GetDiagnostic();
		TFunction<void(Contract::FDiagnostic&)> Relocate = [&](Contract::FDiagnostic& Item) {
			if (Item.SourceKind != TEXT("native_result") && Item.SourceKind != TEXT("command_input")
				&& (Item.Source.SourceUnit == Span.GetSourceUnit() || Item.Source.SourceFile == Span.GetSourceUnit()
					|| (Item.Source.SourceUnit.IsEmpty() && Item.Source.SourceFile.IsEmpty())) && Item.Source.Line > 0) {
				if (Item.Source.Line == 1) Item.Source.Column += Absolute->GetColumn() - 1;
				Item.Source.Line += Absolute->GetLine() - 1;
			}
			for (auto& Child : Item.Diagnostics) Relocate(Child);
			if (Item.PublicCause) { auto Cause = MakeShared<Contract::FDiagnostic>(*Item.PublicCause); Relocate(*Cause); Item.PublicCause = MoveTemp(Cause); }
			for (auto& Value : Item.Unchecked) if (Value && Value->Type == EJson::Object) {
				const auto Object = Value->AsObject(); FString Path; double FromLine = 0;
				if (Object->TryGetStringField(TEXT("source_file"), Path) && Path == Span.GetSourceUnit()
					&& Object->TryGetNumberField(TEXT("from_line"), FromLine) && FromLine > 0) {
					auto Copy = MakeShared<FJsonObject>(*Object); Copy->SetNumberField(TEXT("from_line"), FromLine + Absolute->GetLine() - 1);
					Value = MakeShared<FJsonValueObject>(Copy);
				}
			}
		};
		Relocate(Failure); OutError.SetFailure(MoveTemp(Failure));
	}

	bool TryUpdateHjsonDocument(const FString& OldText, const FString& NewText, FHjsonDocumentRef Previous,
		FUeAgentResourceTracker& Resources, FHjsonDocumentPtr& OutDocument, FWorkspaceError& OutError,
		FHjsonDocumentUpdateMetrics* Metrics, const FHjsonOperationCheckpoint* Checkpoint)
	{
		OutDocument.Reset(); OutError.Reset();
		if (Metrics) *Metrics = {};
		const FTCHARToUTF8 OldUtf8(*OldText), NewUtf8(*NewText);
		const std::string_view Old(OldUtf8.Get(), OldUtf8.Length()), New(NewUtf8.Get(), NewUtf8.Length());
		const auto StopUpdate = [&](const TCHAR* Stage, const FString& Detail) {
			OutError.SetSourceFailure(TEXT("workspace_incremental_syntax_unavailable"),
				FString::Printf(TEXT("Incremental syntax update stopped at %s: %s. The original parser must provide the matching document version and safe boundary; no whole-document parser was selected."), Stage, *Detail), Previous->GetSourceUnit());
			return false;
		};
		TextLineDiff::FResult Difference; TextLineDiff::FError DiffError;
		if (!TextLineDiff::TryDiff(Old, New, Difference, DiffError))
			return StopUpdate(TEXT("line_diff"), FString::Printf(TEXT("code=%d side=%d byte=%llu"), static_cast<int32>(DiffError.Code), static_cast<int32>(DiffError.Side), static_cast<uint64>(DiffError.ByteOffset)));
		if (Metrics) Metrics->bComparedText = true;
		if (Difference.Edits.empty()) { OutDocument = Previous; return true; }
		if (Metrics) Metrics->DiffBlocks = Difference.Edits.size();

		// A line block includes its unchanged key/indentation. Refine only equal
		// bytes inside that block; neither line numbers nor array positions become
		// semantic identities. Keep cuts on UTF-8 character boundaries.
		struct FEdit { int64 OldBegin, OldEnd, NewBegin, NewEnd; };
		TArray<FEdit> Edits;
		for (const auto& Block : Difference.Edits) {
			std::size_t A = Block.OldBytes.Begin, B = Block.NewBytes.Begin;
			std::size_t AE = Block.OldBytes.End, BE = Block.NewBytes.End;
			while (A < AE && B < BE && Old[A] == New[B]) { ++A; ++B; }
			while (A > Block.OldBytes.Begin && A < Old.size() && (static_cast<uint8>(Old[A]) & 0xc0) == 0x80) { --A; --B; }
			while (AE > A && BE > B && Old[AE - 1] == New[BE - 1]) { --AE; --BE; }
			while (AE < Block.OldBytes.End && AE < Old.size() && (static_cast<uint8>(Old[AE]) & 0xc0) == 0x80) { ++AE; ++BE; }
			Edits.Add({static_cast<int64>(A), static_cast<int64>(AE), static_cast<int64>(B), static_cast<int64>(BE)});
		}
		if (Metrics) {
			Metrics->Changes.Reserve(Edits.Num());
			for (const auto& Edit : Edits) Metrics->Changes.Add({Edit.OldBegin, Edit.OldEnd, Edit.NewBegin, Edit.NewEnd});
		}
		auto MapOffset = [&](int64 Offset, bool bEnd) {
			int64 Delta = 0;
			for (const auto& Edit : Edits) {
				if (Offset < Edit.OldBegin || (Offset == Edit.OldBegin && bEnd)) break;
				if (Offset < Edit.OldEnd) return Edit.NewBegin;
				Delta = Edit.NewEnd - Edit.OldEnd;
			}
			return Offset + Delta;
		};

		// One bounded-density coordinate index for this observed text. It uses
		// the original scanner's CRLF/UTF-16 column rules, not splitlines or a
		// second lexer. Long single lines do not cause repeated prefix scans.
		struct FPosition { int64 Byte; int32 Character, Line, Column; };
		TArray<FPosition> Positions;
		int32 Character = 0, Line = 1, Column = 1; int64 Byte = 0;
		Positions.Add({0, 0, 1, 1});
		while (Character < NewText.Len()) {
			int32 Units = 1; const int32 Width = Utf8WidthAt(NewText, Character, Units), Before = Character;
			const TCHAR Ch = AdvanceSourcePosition(NewText, Character, Line, Column);
			if (Units == 2) AdvanceSourcePosition(NewText, Character, Line, Column);
			Byte += Width + Character - Before - Units;
			if (Ch == TEXT('\n') || Character - Positions.Last().Character >= 256)
				Positions.Add({Byte, Character, Line, Column});
		}
		if (Positions.Last().Byte != Byte) Positions.Add({Byte, Character, Line, Column});
		auto PositionAt = [&](int64 Wanted) {
			int32 Low = 0, High = Positions.Num();
			while (Low + 1 < High) { const int32 Mid = Low + (High - Low) / 2;
				if (Positions[Mid].Byte <= Wanted) Low = Mid; else High = Mid; }
			FPosition Result = Positions[Low];
			while (Result.Byte < Wanted && Result.Character < NewText.Len()) {
				int32 Units = 1; const int32 Width = Utf8WidthAt(NewText, Result.Character, Units), Before = Result.Character;
				AdvanceSourcePosition(NewText, Result.Character, Result.Line, Result.Column);
				if (Units == 2) AdvanceSourcePosition(NewText, Result.Character, Result.Line, Result.Column);
				Result.Byte += Width + Result.Character - Before - Units;
			}
			return Result;
		};
		auto NewProvenance = [&](const FHjsonNode& Node) {
			const auto& P = Node.GetProvenance();
			const auto Start = PositionAt(MapOffset(P.GetByteStart(), false));
			return FSourceProvenance(P.GetSourceUnitBacking(), P.GetJsonPointer(), Start.Line, Start.Column,
				Start.Byte, MapOffset(P.GetByteEnd(), true), P.GetOriginalKey());
		};
		auto Reuse = [&](FHjsonNodeRef Node) -> FHjsonNodeRef {
			if (Metrics) ++Metrics->ReusedSubtrees;
			const auto& OldP = Node->GetProvenance(); auto NewP = NewProvenance(*Node);
			if (OldP.GetByteStart() == NewP.GetByteStart() && OldP.GetLine() == NewP.GetLine() && OldP.GetColumn() == NewP.GetColumn()) return Node;
			return MakeShared<FHjsonNode>(Node, MoveTemp(NewP));
		};

		// Trivia outside an unchanged explicit root is not a tree edit. Use the
		// original scanner, including the boundary token, so an unterminated
		// comment cannot silently swallow that token. All edits still relocate
		// source coordinates; only proven exterior trivia is excluded below.
		TArray<FEdit> TreeEdits;
		const auto& RootP = Previous->GetRoot()->GetProvenance();
		bool bLeadingTrivia = false, bTrailingTrivia = false;
		for (const auto& Edit : Edits) {
			if (Edit.OldEnd <= RootP.GetByteStart()) bLeadingTrivia = true;
			else if (Edit.OldBegin >= RootP.GetByteEnd()) bTrailingTrivia = true;
			else TreeEdits.Add(Edit);
		}
		auto CheckExterior = [&](bool bLeading) {
			const int64 Boundary = bLeading ? MapOffset(RootP.GetByteStart(), false) : MapOffset(RootP.GetByteEnd(), true) - 1;
			const int64 OldBoundary = bLeading ? RootP.GetByteStart() : RootP.GetByteEnd() - 1;
			if (Boundary < 0 || Boundary >= static_cast<int64>(New.size()) || OldBoundary < 0
				|| OldBoundary >= static_cast<int64>(Old.size()) || Old[OldBoundary] != New[Boundary]
				|| (bLeading ? New[Boundary] != '{' && New[Boundary] != '[' : New[Boundary] != '}' && New[Boundary] != ']'))
				return StopUpdate(TEXT("exterior_boundary"), RootP.GetJsonPointer());
			const auto Start = PositionAt(bLeading ? 0 : Boundary);
			const auto End = PositionAt(bLeading ? Boundary + 1 : static_cast<int64>(New.size()));
			const FSourceProvenance Origin(RootP.GetSourceUnitBacking(), RootP.GetJsonPointer(), Start.Line, Start.Column,
				Start.Byte, End.Byte, RootP.GetOriginalKey());
			TArray<FToken> Tokens; int32 EndLine = 1, EndColumn = 1; int64 Bytes = 0;
			FWorkspaceError Diagnostics;
			if (!ScanTokens(NewText.Mid(Start.Character, End.Character - Start.Character), Previous->GetSourceUnit(),
				Resources, Tokens, EndLine, EndColumn, Bytes, OutError, Diagnostics, Checkpoint, &Origin, true)) return false;
			if (Diagnostics.HasErrors()) { OutError = MoveTemp(Diagnostics); return false; }
			const ETokenKind Expected = bLeading ? (New[Boundary] == '{' ? ETokenKind::OpenObject : ETokenKind::OpenArray)
				: (New[Boundary] == '}' ? ETokenKind::CloseObject : ETokenKind::CloseArray);
			if (Tokens.Num() != 1 || Tokens[0].Kind != Expected || Tokens[0].ByteStart != Boundary)
				return StopUpdate(TEXT("exterior_trivia"), RootP.GetJsonPointer());
			return true;
		};
		if ((bLeadingTrivia && !CheckExterior(true)) || (bTrailingTrivia && !CheckExterior(false))) return false;
		if (TreeEdits.IsEmpty()) {
			OutDocument = MakeShared<FHjsonDocument>(Previous->GetSourceUnit(), Reuse(Previous->GetRoot())); return true;
		}
		struct FTask { FHjsonNodeRef Node; int32 First, End; bool bAssemble; const FHjsonNode* Parent; };
		TArray<FTask> Tasks; Tasks.Add({Previous->GetRoot(), 0, TreeEdits.Num(), false, nullptr});
		TMap<const FHjsonNode*, FHjsonNodeRef> Replacements;
		TSet<const FHjsonNode*> EnclosingScopes;
		while (!Tasks.IsEmpty()) {
			if (Checkpoint && !(*Checkpoint)(TEXT("hjson_update"), OutError)) return false;
			const FTask Task = Tasks.Pop(EAllowShrinking::No);
			if (Task.Parent && EnclosingScopes.Contains(Task.Parent)) continue;
			if (Task.bAssemble && !EnclosingScopes.Remove(&Task.Node.Get())) {
				TArray<FHjsonNodeRef> Items; TArray<TPair<FString, FHjsonNodeRef>> Entries;
				auto ChildResult = [&](FHjsonNodeRef Child) {
					if (const auto* Result = Replacements.Find(&Child.Get())) return *Result;
					return Reuse(Child);
				};
				for (const auto& Child : Task.Node->GetItems()) Items.Add(ChildResult(Child));
				for (const auto& Entry : Task.Node->GetEntries()) Entries.Emplace(Entry.Key, ChildResult(Entry.Value));
				Replacements.Add(&Task.Node.Get(), MakeShared<FHjsonNode>(Task.Node->GetKind(), NewProvenance(*Task.Node),
					FString(), FString(), false, MoveTemp(Items), MoveTemp(Entries)));
				continue;
			}
			// Descend only when every edit is strictly inside an existing child
			// container. Keys, separators and insertion at a boundary belong to
			// their parent, whose original parser checks duplicates and context.
			if (!Task.bAssemble) {
				TArray<FTask> Children; int32 NextEdit = Task.First;
				auto SelectChild = [&](FHjsonNodeRef Child) {
					if (NextEdit == Task.End) return;
					const auto& P = Child->GetProvenance(); const int32 First = NextEdit;
					if (Child->GetKind() != EHjsonNodeKind::Object && Child->GetKind() != EHjsonNodeKind::Array) return;
					while (NextEdit < Task.End && P.GetByteStart() < TreeEdits[NextEdit].OldBegin && TreeEdits[NextEdit].OldEnd < P.GetByteEnd()) ++NextEdit;
					if (NextEdit > First) Children.Add({Child, First, NextEdit, false, &Task.Node.Get()});
				};
				for (const auto& Child : Task.Node->GetItems()) SelectChild(Child);
				for (const auto& Entry : Task.Node->GetEntries()) SelectChild(Entry.Value);
				if (NextEdit == Task.End && !Children.IsEmpty()) {
					Tasks.Add({Task.Node, Task.First, Task.End, true, Task.Parent});
					for (int32 I = Children.Num() - 1; I >= 0; --I) Tasks.Add(Children[I]);
					continue;
				}
			}
			const auto& P = Task.Node->GetProvenance();
			const int64 StartByte = MapOffset(P.GetByteStart(), false), EndByte = MapOffset(P.GetByteEnd(), true);
			const bool bExplicitContainer = P.GetByteStart() >= 0 && P.GetByteStart() < static_cast<int64>(Old.size())
				&& (Old[P.GetByteStart()] == '{' || Old[P.GetByteStart()] == '[');
			if (!bExplicitContainer || TreeEdits[Task.First].OldBegin <= P.GetByteStart() || TreeEdits[Task.End - 1].OldEnd >= P.GetByteEnd()
				|| StartByte < 0 || EndByte > static_cast<int64>(New.size()) || EndByte <= StartByte)
				return StopUpdate(TEXT("container_boundary"), P.GetJsonPointer());
			const auto Start = PositionAt(StartByte), End = PositionAt(EndByte);
			if (Start.Byte != StartByte || End.Byte != EndByte) return StopUpdate(TEXT("source_coordinate"), P.GetJsonPointer());
			FHjsonDocumentPtr Fragment; FWorkspaceError FragmentError;
			const auto Origin = NewProvenance(*Task.Node);
			bool bExtendsIntoParent = false;
			if (!TryParseHjsonDocumentImpl(NewText.Mid(Start.Character, End.Character - Start.Character), Previous->GetSourceUnit(),
				Resources, Fragment, FragmentError, Checkpoint, &Origin, nullptr, nullptr, nullptr,
				Task.Parent ? &bExtendsIntoParent : nullptr)) {
				// Keep the original fragment diagnostic. Failure never selects a
				// larger parser or retries with a different document algorithm.
				OutError = MoveTemp(FragmentError);
				if (!OutError.HasErrors()) return StopUpdate(TEXT("fragment_parser"), P.GetJsonPointer());
				return false;
			}
			if (bExtendsIntoParent) {
				EnclosingScopes.Add(Task.Parent);
				continue;
			}
			if (Metrics) { ++Metrics->ReparsedContainers; Metrics->ReparsedBytes += EndByte - StartByte;
				Metrics->bWholeDocument |= &Task.Node.Get() == &Previous->GetRoot().Get(); }
			Replacements.Add(&Task.Node.Get(), Fragment->GetRoot());
		}
		const auto* Root = Replacements.Find(&Previous->GetRoot().Get());
		if (!Root) return StopUpdate(TEXT("replacement_root"), TEXT("the update produced no root"));
		OutDocument = MakeShared<FHjsonDocument>(Previous->GetSourceUnit(), *Root);
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
				OutError.SetSourceFailure(TEXT("hjson_writer_node_missing"), TEXT("The HJSON writer encountered a missing document node."),
					Document->GetSourceUnit());
				return false;
			}
			if (Resources.CollectsUsage() && !Charge(
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
		if (Resources.CollectsUsage() && !Charge(
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

	bool TryProjectHjsonNodeToJsonValue(
		const FHjsonNode& Node,
		FUeAgentResourceTracker& Resources,
		TSharedPtr<FJsonValue>& OutValue,
		FWorkspaceError& OutError,
		const FHjsonOperationCheckpoint* Checkpoint)
	{
		OutValue.Reset();
		OutError.Reset();
		TArray<FHjsonJsonProjectionTask> Tasks;
		Tasks.Add({&Node, false});
		TMap<const FHjsonNode*, TSharedPtr<FJsonValue>> Converted;
		while (!Tasks.IsEmpty())
		{
			if (Checkpoint && !(*Checkpoint)(TEXT("hjson_json_projection"), OutError))
			{
				return false;
			}
			const FHjsonJsonProjectionTask Task = Tasks.Pop(EAllowShrinking::No);
			if (!Task.Node)
			{
				OutError.SetSourceFailure(TEXT("hjson_json_projection_node_missing"), TEXT("The HJSON-to-JSON projection encountered a missing node."), Node.GetProvenance().GetSourceUnit());
				return false;
			}
			if (!Task.bExit)
			{
				if (!ChargeJsonProjectionNode(*Task.Node, Resources, OutError))
				{
					return false;
				}
				switch (Task.Node->GetKind())
				{
				case EHjsonNodeKind::Null:
					Converted.Add(Task.Node, MakeShared<FJsonValueNull>());
					break;
				case EHjsonNodeKind::Boolean:
					Converted.Add(
						Task.Node,
						MakeShared<FJsonValueBoolean>(Task.Node->GetBoolean()));
					break;
				case EHjsonNodeKind::String:
					Converted.Add(
						Task.Node,
						MakeShared<FJsonValueString>(Task.Node->GetText()));
					break;
				case EHjsonNodeKind::Integer:
				case EHjsonNodeKind::Decimal:
					Converted.Add(
						Task.Node,
						MakeShared<FJsonValueNumberString>(Task.Node->GetLexeme()));
					break;
				case EHjsonNodeKind::Array:
				case EHjsonNodeKind::Object:
					Tasks.Add({Task.Node, true});
					if (Task.Node->GetKind() == EHjsonNodeKind::Array)
					{
						const TArray<FHjsonNodeRef>& Items = Task.Node->GetItems();
						for (int32 Index = Items.Num() - 1; Index >= 0; --Index)
						{
							Tasks.Add({&Items[Index].Get(), false});
						}
					}
					else
					{
						const TArray<TPair<FString, FHjsonNodeRef>>& Entries =
							Task.Node->GetEntries();
						for (int32 Index = Entries.Num() - 1; Index >= 0; --Index)
						{
							Tasks.Add({&Entries[Index].Value.Get(), false});
						}
					}
					break;
				default:
					OutError.SetFailure(WorkspaceValueFailure(TEXT("hjson_json_projection_node_kind_invalid"), TEXT("The HJSON node kind has no supported JSON representation."), MakeShared<FJsonValueString>(NodeKindName(Task.Node->GetKind()))),
						Task.Node->GetProvenance().GetSourceUnit(), Task.Node->GetProvenance().GetJsonPointer());
					OutError.Locate(Task.Node->GetProvenance());
					return false;
				}
				continue;
			}

			if (Task.Node->GetKind() == EHjsonNodeKind::Array)
			{
				TArray<TSharedPtr<FJsonValue>> Items;
				Items.Reserve(Task.Node->GetItems().Num());
				for (const FHjsonNodeRef& Item : Task.Node->GetItems())
				{
					const TSharedPtr<FJsonValue>* ConvertedItem = Converted.Find(&Item.Get());
					if (!ConvertedItem || !ConvertedItem->IsValid())
					{
						OutError.SetFailure(WorkspaceValueFailure(TEXT("hjson_json_projection_child_missing"), TEXT("The HJSON array item has no converted JSON value."), nullptr),
							Item->GetProvenance().GetSourceUnit(), Item->GetProvenance().GetJsonPointer());
						OutError.Locate(Item->GetProvenance());
						return false;
					}
					Items.Add(*ConvertedItem);
				}
				Converted.Add(
					Task.Node,
					MakeShared<FJsonValueArray>(MoveTemp(Items)));
			}
			else
			{
				const TSharedRef<FJsonObject> Object = MakeShared<FJsonObject>();
				for (const TPair<FString, FHjsonNodeRef>& Entry : Task.Node->GetEntries())
				{
					const TSharedPtr<FJsonValue>* ConvertedEntry =
						Converted.Find(&Entry.Value.Get());
					if (!ConvertedEntry || !ConvertedEntry->IsValid())
					{
						OutError.SetFailure(WorkspaceValueFailure(TEXT("hjson_json_projection_child_missing"), TEXT("The HJSON object field has no converted JSON value."), nullptr),
							Entry.Value->GetProvenance().GetSourceUnit(), Entry.Value->GetProvenance().GetJsonPointer());
						OutError.Locate(Entry.Value->GetProvenance());
						return false;
					}
					if (Object->Values.Contains(Entry.Key))
					{
						OutError.SetFailure(WorkspaceValueFailure(TEXT("hjson_json_object_key_collision"), TEXT("Two HJSON entries map to the same JSON object key."), MakeShared<FJsonValueString>(Entry.Key)),
							Entry.Value->GetProvenance().GetSourceUnit(), Entry.Value->GetProvenance().GetJsonPointer());
						OutError.Locate(Entry.Value->GetProvenance());
						return false;
					}
					Object->SetField(Entry.Key, *ConvertedEntry);
				}
				Converted.Add(Task.Node, MakeShared<FJsonValueObject>(Object));
			}
		}
		OutValue = Converted.FindRef(&Node);
		if (!OutValue.IsValid())
		{
			OutError.SetSourceFailure(TEXT("hjson_json_projection_failed"), TEXT("The HJSON root node has no converted JSON value."),
				Node.GetProvenance().GetSourceUnit(),
				Node.GetProvenance().GetJsonPointer());
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
				OutError.SetFailure(WorkspaceValueFailure(TEXT("workspace_path_invalid"),
					TEXT("The workspace unit path must be nonempty, relative and contained in the workspace."),
					MakeShared<FJsonValueString>(Value), MakeShared<FJsonValueString>(TEXT("relative workspace unit path"))), Value);
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
					OutError.SetFailure(WorkspaceValueFailure(IsReservedStem(Part)
						? TEXT("workspace_path_reserved_name") : TEXT("workspace_path_invalid"),
						IsReservedStem(Part) ? TEXT("A workspace path component uses a reserved file name.") : TEXT("A normalized workspace path cannot contain '.' or '..' components."),
						MakeShared<FJsonValueString>(Part)), Value);
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
						OutError.SetFailure(WorkspaceValueFailure(TEXT("workspace_path_case_collision"),
							TEXT("Two workspace unit paths differ only by letter case."),
							MakeShared<FJsonValueString>(UnitId), MakeShared<FJsonValueString>(*Existing)), UnitId);
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
				if (Resources.CollectsUsage() && !Charge(
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
					OutError.SetSourceFailure(TEXT("workspace_source_file_missing"), TEXT("The workspace source file does not exist."), Root);
					return false;
				}
				if (Resources.CollectsUsage() && !Charge(
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
					OutError.SetSourceFailure(TEXT("workspace_source_read_failed"), TEXT("The workspace source file could not be read."), Root);
					return false;
				}
				OutSources.Add({Policy.GetEntryUnit(), MoveTemp(Text)});
				return true;
			}
			if (!IFileManager::Get().DirectoryExists(*Root))
			{
				OutError.SetFailure(WorkspaceValueFailure(TEXT("workspace_source_directory_missing"),
					TEXT("The workspace source directory does not exist."), MakeShared<FJsonValueString>(Source.GetPath())), Root);
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
					OutError.SetFailure(WorkspaceValueFailure(TEXT("workspace_path_invalid"),
						TEXT("The source file path could not be made relative to the workspace directory."),
						MakeShared<FJsonValueString>(File), MakeShared<FJsonValueString>(RelativeBase)), File);
					return false;
				}
				FPaths::NormalizeFilename(UnitId);
				FPaths::RemoveDuplicateSlashes(UnitId);
				UnitIds.Add(UnitId);
				if (Resources.CollectsUsage() && !Charge(
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
					OutError.SetSourceFailure(TEXT("workspace_source_read_failed"), TEXT("The workspace source file could not be read."), Files[Index]);
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
				OutError.SetFailure(WorkspaceValueFailure(TEXT("workspace_unit_root_not_object"),
					TEXT("Each workspace unit must have an object root containing its policy fields."),
					MakeShared<FJsonValueString>(NodeKindName(Root.GetKind())), MakeShared<FJsonValueString>(TEXT("object"))), SourceUnit);
				OutError.Locate(Root.GetProvenance());
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
					OutError.SetFailure(WorkspaceValueFailure(TEXT("workspace_policy_field_missing"),
						TEXT("A required workspace policy field is missing."), nullptr, MakeShared<FJsonValueString>(Entry.Value)),
						SourceUnit, TEXT("/") + Entry.Key);
					OutError.Locate(Root.GetProvenance());
					return false;
				}
				if (NodeScalarText(*Node) != Entry.Value)
				{
					OutError.SetFailure(WorkspaceValueFailure(TEXT("workspace_policy_mismatch"),
						TEXT("The workspace policy field does not match the required contract."),
						MakeShared<FJsonValueString>(NodeScalarText(*Node)), MakeShared<FJsonValueString>(Entry.Value)),
						SourceUnit, TEXT("/") + Entry.Key);
					OutError.Locate(Node->GetProvenance());
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
						OutError.SetFailure(WorkspaceValueFailure(TEXT("workspace_ref_escape"),
							TEXT("The workspace reference traverses above the workspace root."), MakeShared<FJsonValueString>(Reference)), SourceUnit);
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
					auto Failure = Contract::FDiagnostic::WrapFailure(TEXT("workspace_ref_escape"), OutError.GetDiagnostic(),
						TEXT("workspace_resolve"), TEXT("The workspace reference does not resolve to a valid relative unit path."));
					Failure.ActualValue = MakeShared<FJsonValueString>(Reference);
					OutError.SetFailure(MoveTemp(Failure), SourceUnit);
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
									OutError.Locate(Entry.Value->GetProvenance());
									return false;
								}
								if (!UnitIds.Contains(Target))
								{
									OutError.SetFailure(WorkspaceValueFailure(TEXT("workspace_ref_missing"),
										TEXT("The referenced workspace unit is not present in the input."), MakeShared<FJsonValueString>(Entry.Value->GetText()),
										MakeShared<FJsonValueString>(Target)), Unit.GetUnitId(), Entry.Value->GetProvenance().GetJsonPointer());
									OutError.Locate(Entry.Value->GetProvenance());
									return false;
								}
								if (Resources.CollectsUsage() && !Charge(
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
						OutError.SetSourceFailure(
							TEXT("workspace_ref_cycle"), TEXT("The workspace references contain a dependency cycle."),
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
			const FString Field = Policy.GetDialectId().IsEmpty() ? TEXT("dialect_id")
				: Policy.GetContractId().IsEmpty() ? TEXT("contract_id")
				: Policy.GetContractVersion() <= 0 ? TEXT("contract_version")
				: Policy.GetProfileId().IsEmpty() ? TEXT("profile_id") : TEXT("entry_unit");
			auto Failure = Contract::FDiagnostic::Failure(TEXT("workspace_policy_invalid"),
				Field == TEXT("contract_version") ? TEXT("The workspace contract version must be positive.") : TEXT("The required workspace policy field is empty."), TEXT("workspace_resolve"));
			Failure.PropertyPath = Field;
			Failure.ActualValue = Field == TEXT("contract_version") ? TSharedPtr<FJsonValue>(MakeShared<FJsonValueNumber>(Policy.GetContractVersion())) : MakeShared<FJsonValueString>(TEXT(""));
			OutError.SetFailure(MoveTemp(Failure));
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
		FWorkspaceError Diagnostics;
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
				Diagnostics.Append(OutError);
				continue;
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
		if (Diagnostics.HasErrors())
		{
			Diagnostics.MarkUnchecked(TEXT("workspace_references"), FString(), TEXT("invalid_units"));
			OutError = MoveTemp(Diagnostics);
			return false;
		}
		if (!UnitIds.Contains(Policy.GetEntryUnit()))
		{
			OutError.SetSourceFailure(TEXT("workspace_entry_unit_missing"), TEXT("The required entry HJSON unit is not present in the workspace input."), Policy.GetEntryUnit());
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

}
