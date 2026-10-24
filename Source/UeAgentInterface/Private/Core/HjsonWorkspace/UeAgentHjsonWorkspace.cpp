#include "Core/HjsonWorkspace/UeAgentHjsonWorkspace.h"
#include "Core/HjsonWorkspace/UeAgentHjsonDiagnostic.h"
#include "Core/StateSemantic/UeAgentExactStringKeys.h"
#include "Utilities/AuthoringCore/UeAgentAuthoringDiagnostics.h"

#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "HAL/FileManager.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "Misc/PackageName.h"
#include "Misc/ScopeExit.h"
#include "Serialization/JsonSerializer.h"
#include "Serialization/JsonWriter.h"

namespace UE::UAI::HjsonWorkspace
{
	using namespace UE::UAI::ContextResource;
	using namespace UE::UAI::StateSemantic;

	namespace
	{
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
			TSet<FString, FExactStringKeyFuncs> Keys;
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

		bool ScanUtf8Text(const FString& Text, TArray<int64>* Offsets, int64& Bytes,
			FWorkspaceError& OutError, const FHjsonOperationCheckpoint* Checkpoint)
		{
			Bytes = 0;
			int32 NextCheckpoint = 0;
			for (int32 Index = 0; Index < Text.Len(); ++Index)
			{
				if (Index >= NextCheckpoint)
				{
					if (Checkpoint && !(*Checkpoint)(Offsets ? TEXT("hjson_utf8_offsets") : TEXT("hjson_utf8_measure"), OutError)) return false;
					NextCheckpoint = Index > MAX_int32 - 1024 ? MAX_int32 : Index + 1024;
				}
				if (Offsets) (*Offsets)[Index] = Bytes;
				const uint32 Character = static_cast<uint32>(Text[Index]);
				if (Character >= 0xD800 && Character <= 0xDBFF
					&& Index + 1 < Text.Len())
				{
					const uint32 Low = static_cast<uint32>(Text[Index + 1]);
					if (Low >= 0xDC00 && Low <= 0xDFFF)
					{
						Bytes += 4;
						++Index;
						if (Offsets) (*Offsets)[Index] = Bytes;
						continue;
					}
				}
				Bytes += Character <= 0x7F
					? 1
					: Character <= 0x7FF
						? 2
						: 3;
			}
			if (Offsets) (*Offsets)[Text.Len()] = Bytes;
			return !Checkpoint || (*Checkpoint)(Offsets ? TEXT("hjson_utf8_offsets_complete") : TEXT("hjson_utf8_measure_complete"), OutError);
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

		bool IsIntegerLexeme(const FString& Value);
		bool IsDecimalLexeme(const FString& Value);

		bool ScanTokens(
			const FString& Text,
			const FString& SourceUnit,
			FUeAgentResourceTracker& Resources,
			TArray<FToken>& OutTokens,
			FWorkspaceError& OutError,
			FWorkspaceError& Diagnostics,
			const FHjsonOperationCheckpoint* Checkpoint)
		{
			OutTokens.Reset();
			TArray<int64> Utf8Offsets;
			int32 TokenCount = 0;
			// The first pass validates ranges and charges their maximum storage without
			// constructing token strings or per-line multiline buffers. The second pass
			// materializes only after all scanner storage has been admitted.
			auto Scan = [&](bool bMaterialize) -> bool
			{
				int32 Index = 0, Line = 1, Column = 1;
				int32 NextCheckpoint = 0;
				bool bNewlineBefore = false;
				struct FScanFrame
				{
					bool bObject;
					EFrameState State;
				};
				TArray<FScanFrame> Frames;
				auto Check = [&](int32 Position) -> bool
				{
					if (Position < NextCheckpoint) return true;
					NextCheckpoint = Position > MAX_int32 - 1024 ? MAX_int32 : Position + 1024;
					return !Checkpoint || (*Checkpoint)(TEXT("hjson_scan"), OutError);
				};
				auto Fail = [&](const TCHAR* Code, const TCHAR* Reason, int32 ErrorLine, int32 ErrorColumn,
					const FString& Actual, const FString& Expected) -> bool
				{
					SetSyntaxFailure(OutError, Code, Reason, SourceUnit, TEXT("/"), ErrorLine, ErrorColumn, Actual, Expected);
					return false;
				};
				auto Advance = [&]() -> TCHAR
				{
					TCHAR Ch = Text[Index++];
					if (Ch == TEXT('\r'))
					{
						if (Index < Text.Len() && Text[Index] == TEXT('\n')) ++Index;
						Ch = TEXT('\n');
					}
					if (Ch == TEXT('\n')) { ++Line; Column = 1; bNewlineBefore = true; }
					else ++Column;
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
					const bool bTokenNewline = bNewlineBefore;
					bNewlineBefore = false;
					// The implicit object root has the same lexical key/value states
					// as a braced object; quoted and bare values share one scanner.
					if (Frames.IsEmpty() && Ch != TEXT('{') && Ch != TEXT('[')
						&& (bMaterialize ? OutTokens.IsEmpty() : TokenCount == 0))
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
					bool bMultiline = false;
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
								if (!bMaterialize)
								{
									FWorkspaceError Error;
									SetSyntaxFailure(Error, TEXT("hjson_multiline_trailing_token"), TEXT("A multiline closing delimiter must be followed by a newline or end of input."),
				SourceUnit, TEXT("/"), Line, Column, FString::Chr(Text[Index]), TEXT("newline or end of input"));
									Error.MarkUnchecked(TEXT("syntax"), SourceUnit, TEXT("invalid_multiline_closing_line"), Line);
									Diagnostics.Append(Error);
								}
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
							if (Text[Index] == TEXT('"')) { ContentEnd = Index; Advance(); bClosed = true; break; }
							if (Text[Index] < 0x20)
								return Fail(TEXT("hjson_unescaped_control"), TEXT("A quoted string contains an unescaped control character."), Line, Column, FString::Printf(TEXT("U+%04X"), static_cast<uint32>(Text[Index])), TEXT("escaped control character"));
							if (Advance() != TEXT('\\')) continue;
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
								const FString Candidate = Text.Mid(Start, Index - Start).TrimEnd();
								const FString Folded = Candidate.ToLower();
								if (Folded == TEXT("true") || Folded == TEXT("false") || Folded == TEXT("null")
									|| Folded == TEXT("nan") || Folded == TEXT("+nan") || Folded == TEXT("-nan")
									|| Folded == TEXT("inf") || Folded == TEXT("+inf") || Folded == TEXT("-inf")
									|| Folded == TEXT("infinity") || Folded == TEXT("+infinity") || Folded == TEXT("-infinity")
									|| IsIntegerLexeme(Candidate) || IsDecimalLexeme(Candidate)) break;
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
						if (!Frames.IsEmpty()) Frames.Pop(EAllowShrinking::No);
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
					if (bMaterialize) Value.Reserve(RawLength + 1);
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
							if (bMaterialize && LineStart != ContentStart) Value.AppendChar(TEXT('\n'));
							while (Cursor < ContentEnd && Text[Cursor] != TEXT('\r') && Text[Cursor] != TEXT('\n'))
							{
								if (bMaterialize) Value.AppendChar(Text[Cursor]);
								++Cursor;
								if (Checkpoint && (Cursor - LineStart) % 1024 == 0 && !(*Checkpoint)(TEXT("hjson_multiline_content"), OutError)) return false;
							}
							if (Cursor < ContentEnd && Text[Cursor++] == TEXT('\r') && Cursor < ContentEnd && Text[Cursor] == TEXT('\n')) ++Cursor;
							++ContentLine;
							if (Checkpoint && !(*Checkpoint)(TEXT("hjson_multiline_line"), OutError)) return false;
						}
					}
					else if (bMaterialize && Kind == ETokenKind::String)
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
					if (!bMaterialize)
					{
						if (!Charge(Resources, {
							{EUeAgentResourceDimension::WorkUnits, 1},
							{EUeAgentResourceDimension::EstimatedMemoryBytes,
								2ll * (static_cast<int64>(RawLength) + 1) * static_cast<int64>(sizeof(TCHAR))}},
							TEXT("hjson-tokenize:") + SourceUnit, OutError)) return false;
						++TokenCount;
					}
					else
					{
						if (Checkpoint && !(*Checkpoint)(TEXT("hjson_token_materialize"), OutError)) return false;
						FString Lexeme = Text.Mid(Start, RawLength);
						if (Kind != ETokenKind::String) Value.Append(Lexeme);
						OutTokens.Add({Kind, MoveTemp(Lexeme), MoveTemp(Value), Start, TokenEnd,
							Utf8Offsets[Start], Utf8Offsets[TokenEnd], TokenLine, TokenColumn, bTokenNewline});
					}
				}
				return !Checkpoint || (*Checkpoint)(TEXT("hjson_scan_complete"), OutError);
			};
			if (!Scan(false)) return false;
			if (!Charge(Resources, {
				{EUeAgentResourceDimension::EstimatedMemoryBytes,
					(static_cast<int64>(TokenCount) + 2) * static_cast<int64>(sizeof(FToken))
					+ (static_cast<int64>(Text.Len()) + 1) * static_cast<int64>(sizeof(int64))}},
				TEXT("hjson-scanner-storage:") + SourceUnit, OutError)) return false;
			OutTokens.Reserve(TokenCount + 2);
			Utf8Offsets.SetNumZeroed(Text.Len() + 1);
			int64 SourceBytes = 0;
			if (!ScanUtf8Text(Text, &Utf8Offsets, SourceBytes, OutError, Checkpoint)) return false;
			return Scan(true);
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
				SetSyntaxFailure(OutError, TEXT("hjson_value_expected"), TEXT("Expected a scalar HJSON value at this position."),
				SourceUnit, Pointer, Token.Line, Token.Column, Token.Lexeme, TEXT("string, number, boolean or null"));
				return false;
			}
			EHjsonNodeKind Kind = EHjsonNodeKind::String;
			FString Text = Token.Value;
			bool bBoolean = false;
			const FString Folded = Token.Value.ToLower();
			if (Token.Kind == ETokenKind::Bare
				&& (Folded == TEXT("true") || Folded == TEXT("false")
					|| Folded == TEXT("null"))
				&& Token.Value != Folded)
			{
				SetSyntaxFailure(OutError, TEXT("hjson_ambiguous_literal"), TEXT("Boolean and null literals must use lowercase; quote this token to use it as text."),
				SourceUnit, Pointer, Token.Line, Token.Column, Token.Value, Folded);
				return false;
			}
			if (Token.Kind == ETokenKind::Bare && Token.Value == TEXT("null"))
			{
				Kind = EHjsonNodeKind::Null;
				Text.Reset();
			}
			else if (Token.Kind == ETokenKind::Bare
				&& (Token.Value == TEXT("true") || Token.Value == TEXT("false")))
			{
				Kind = EHjsonNodeKind::Boolean;
				bBoolean = Token.Value == TEXT("true");
				Text.Reset();
			}
			else if (Token.Kind == ETokenKind::Bare
				&& (Folded == TEXT("nan")
					|| Folded == TEXT("+nan")
					|| Folded == TEXT("-nan")
					|| Folded == TEXT("infinity")
					|| Folded == TEXT("+infinity")
					|| Folded == TEXT("-infinity")
					|| Folded == TEXT("inf")
					|| Folded == TEXT("+inf")
					|| Folded == TEXT("-inf")))
			{
				SetSyntaxFailure(OutError, TEXT("hjson_nonfinite_number"), TEXT("HJSON numbers must be finite."),
				SourceUnit, Pointer, Token.Line, Token.Column, Token.Value, TEXT("finite number"));
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
				// A decimal token is exact authoring data, not yet a native double.
				// Its destination codec owns the native range check. NaN/Inf
				// literals are rejected separately above.
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

	FString EscapeJsonPointerSegment(FString Segment)
	{
		return EscapePointerSegment(Segment);
	}

	FUeAgentResourceTracker MakeSemanticWorkspaceResourceTracker()
	{
		return FUeAgentResourceTracker();
	}

	TSharedRef<FUeAgentResourceTracker, ESPMode::ThreadSafe>
	MakeSemanticWorkspaceResourceTrackerOwner()
	{
		return MakeShared<FUeAgentResourceTracker, ESPMode::ThreadSafe>();
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
	}

	const FString& FHjsonDocument::GetSourceUnit() const { return SourceUnit; }
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
		FWorkspaceError& OutError,
		const FHjsonOperationCheckpoint* Checkpoint)
	{
		OutDocument.Reset();
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
		int64 SourceUtf8Length = 0;
		if (!ScanUtf8Text(Text, nullptr, SourceUtf8Length, OutError, Checkpoint)) return false;
		if (!Charge(
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
		TArray<FToken> Tokens;
		if (!ScanTokens(Text, SourceUnit, Resources, Tokens, OutError, Diagnostics, Checkpoint))
		{
			return false;
		}
		if (Tokens.IsEmpty())
		{
			SetSyntaxFailure(OutError, TEXT("hjson_empty_document"), TEXT("The HJSON document contains no value."),
				SourceUnit, TEXT("/"), 1, 1, TEXT("empty document"), TEXT("one root value"));
			return false;
		}
		int32 EndLine = 1;
		int32 EndColumn = 1;
		for (int32 Index = 0; Index < Text.Len(); ++Index)
		{
			if (Checkpoint && Index % 1024 < 2
				&& !(*Checkpoint)(TEXT("hjson_end_position"), OutError)) return false;
			if (Text[Index] == TEXT('\r'))
			{
				if (Index + 1 < Text.Len() && Text[Index + 1] == TEXT('\n')) ++Index;
				++EndLine;
				EndColumn = 1;
			}
			else if (Text[Index] == TEXT('\n'))
			{
				++EndLine;
				EndColumn = 1;
			}
			else
			{
				++EndColumn;
			}
		}
		const bool bImplicitRootObject = Tokens.Num() >= 2
			&& (Tokens[0].Kind == ETokenKind::Bare
				|| Tokens[0].Kind == ETokenKind::String)
			&& Tokens[1].Kind == ETokenKind::Colon;
		if (bImplicitRootObject)
		{
			if (!Charge(Resources, {
					{EUeAgentResourceDimension::WorkUnits, 2},
					{EUeAgentResourceDimension::EstimatedMemoryBytes,
						2 * static_cast<int64>(sizeof(FToken))}},
				TEXT("hjson-implicit-root:") + SourceUnit, OutError))
			{
				return false;
			}
			const FToken First = Tokens[0];
			Tokens.Insert({ETokenKind::OpenObject, TEXT("{"), TEXT("{"),
				0, 0, 0, 0, First.Line, First.Column, First.bNewlineBefore}, 0);
			const int64 EndByte = SourceUtf8Length;
			Tokens.Add({ETokenKind::CloseObject, TEXT("}"), TEXT("}"),
				Text.Len(), Text.Len(), EndByte, EndByte,
				EndLine, EndColumn, true});
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
			if (Checkpoint && !(*Checkpoint)(TEXT("hjson_parse"), OutError))
			{
				return false;
			}
			if (bExpectingValue)
			{
				if (!Tokens.IsValidIndex(TokenIndex))
				{
					SetSyntaxFailure(OutError, TEXT("hjson_value_missing"), TEXT("A value is required before the next delimiter or end of input."),
				SourceUnit, ExpectedPointer, EndLine, EndColumn, TEXT("end of input"), TEXT("HJSON value"));
					return false;
				}
				FToken Token = Tokens[TokenIndex++];
				if (!Frames.IsEmpty() && (Token.Kind == ETokenKind::Comma
					|| (Frames.Last().bObject && Token.Kind == ETokenKind::CloseObject)
					|| (!Frames.Last().bObject && Token.Kind == ETokenKind::CloseArray)))
				{
					FWorkspaceError Error;
					SetSyntaxFailure(Error, TEXT("hjson_value_missing"), TEXT("A value is required before the next delimiter or end of input."),
				SourceUnit, ExpectedPointer, Token.Line, Token.Column, Token.Lexeme, TEXT("HJSON value"));
					Diagnostics.Append(Error);
					--TokenIndex;
					Frames.Last().PendingKey.Reset();
					Frames.Last().State = EFrameState::AfterValue;
					bExpectingValue = false;
					continue;
				}
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
				SetSyntaxFailure(OutError, TEXT("hjson_container_unclosed"), TEXT("The HJSON container has no closing delimiter."),
				SourceUnit, Frames.Last().Pointer, EndLine, EndColumn, TEXT("end of input"), Frames.Last().bObject ? TEXT("}") : TEXT("]"));
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
					if ((Token.Kind != ETokenKind::Bare
						&& Token.Kind != ETokenKind::String)
						|| (Token.Kind == ETokenKind::Bare
							&& (Token.Value.Contains(TEXT(" ")) || Token.Value.Contains(TEXT("\t")))))
					{
						SetSyntaxFailure(OutError, TEXT("hjson_object_key_expected"), TEXT("Expected a quoted key or an unquoted key without whitespace."),
				SourceUnit, Frame.Pointer, Token.Line, Token.Column, Token.Lexeme, TEXT("object key"));
						return false;
					}
					if (Frame.Keys.Contains(Token.Value))
					{
						SetSyntaxFailure(OutError, TEXT("hjson_duplicate_key"), TEXT("Each key must be unique within its object."),
				SourceUnit, JoinPointer(Frame.Pointer, Token.Value), Token.Line, Token.Column, Token.Value, TEXT("unique object key"));
						Diagnostics.Append(OutError);
						OutError.Reset();
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
						SetSyntaxFailure(OutError, TEXT("hjson_colon_expected"), TEXT("Expected ':' after the object key."),
				SourceUnit, JoinPointer(Frame.Pointer, Frame.PendingKey), Token.Line, Token.Column, Token.Lexeme, TEXT(":"));
						// Only resume where a value token is already unambiguous.
						if (Token.Kind != ETokenKind::OpenObject && Token.Kind != ETokenKind::OpenArray
							&& Token.Kind != ETokenKind::Bare && Token.Kind != ETokenKind::String) return false;
						Diagnostics.Append(OutError);
						OutError.Reset();
						ExpectedPointer = JoinPointer(Frame.Pointer, Frame.PendingKey);
						ExpectedKey = Frame.PendingKey;
						Frame.State = EFrameState::AwaitingValue;
						bExpectingValue = true;
						continue;
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
					SetSyntaxFailure(OutError, TEXT("hjson_object_separator_expected"), TEXT("Expected a comma, newline or closing brace after the object value."),
				SourceUnit, Frame.Pointer, Token.Line, Token.Column, Token.Lexeme, TEXT("comma, newline or }"));
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
					SetSyntaxFailure(OutError, TEXT("hjson_array_separator_expected"), TEXT("Expected a comma, newline or closing bracket after the array value."),
				SourceUnit, Frame.Pointer, Token.Line, Token.Column, Token.Lexeme, TEXT("comma, newline or ]"));
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
				SourceUnit, TEXT("/"), Trailing.Line, Trailing.Column, Trailing.Lexeme, TEXT("end of input"));
			return false;
		}
		bReachedEnd = true;
		if (Diagnostics.HasErrors()) return false;
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
				OutError.SetSourceFailure(TEXT("hjson_writer_node_missing"), TEXT("The HJSON writer encountered a missing document node."),
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
					OutError.SetSourceFailure(TEXT("workspace_source_file_missing"), TEXT("The workspace source file does not exist."), Root);
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
