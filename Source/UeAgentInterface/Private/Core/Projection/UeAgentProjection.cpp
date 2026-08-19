#include "Core/Projection/UeAgentProjection.h"

#include "Misc/SecureHash.h"

namespace UE::UAI::Projection
{
	namespace
	{
		using namespace UE::UAI::ContextResource;

		constexpr int32 CursorFieldCount = 13;

		struct FCursorSource
		{
			FString Kind;
			FString SourceId;
			FString RevisionSourceId;
			FString RevisionGeneration;
			int64 RevisionSequence = -1;
			int32 TotalItems = 0;
		};

		void ResetError(FUeAgentProjectionError& OutError)
		{
			OutError = FUeAgentProjectionError{};
		}

		bool Fail(
			FUeAgentProjectionError& OutError,
			const FString& Code,
			const FString& SubjectId = FString(),
			const FString& Detail = FString())
		{
			OutError.Code = Code;
			OutError.SubjectId = SubjectId;
			OutError.Detail = Detail;
			return false;
		}

		FString NormalizeRequired(FString Value)
		{
			Value.TrimStartAndEndInline();
			return Value;
		}

		FString NormalizeOptional(FString Value)
		{
			Value.TrimStartAndEndInline();
			return Value;
		}

		bool IsSha256(const FString& Value)
		{
			if (!Value.StartsWith(TEXT("sha256:")) || Value.Len() != 71)
			{
				return false;
			}
			for (int32 Index = 7; Index < Value.Len(); ++Index)
			{
				const TCHAR Character = Value[Index];
				if (!FChar::IsDigit(Character)
					&& (Character < TEXT('a') || Character > TEXT('f'))
					&& (Character < TEXT('A') || Character > TEXT('F')))
				{
					return false;
				}
			}
			return true;
		}

		bool Charge(
			FUeAgentResourceTracker& Resources,
			const TMap<EUeAgentResourceDimension, int64>& Amounts,
			const FString& Path,
			FUeAgentProjectionError& OutError)
		{
			FUeAgentResourceBudgetExceeded BudgetError;
			if (Resources.TryChargeMany(Amounts, Path, BudgetError))
			{
				return true;
			}
			OutError.Code = BudgetError.Code;
			OutError.SubjectId = BudgetError.Path;
			OutError.Detail = TEXT("projection resource budget exceeded");
			OutError.ResourceDimension = BudgetError.Dimension;
			OutError.ResourceLimit = BudgetError.Limit;
			OutError.ResourceObserved = BudgetError.Observed;
			return false;
		}

		bool Checkpoint(
			const FString& Stage,
			const FUeAgentOperationDeadline& Deadline,
			const FUeAgentCancellationToken& Cancellation,
			FUeAgentProjectionError& OutError)
		{
			FUeAgentCancellationRequested CancellationError;
			if (!Cancellation.TryCheckpoint(CancellationError))
			{
				return Fail(
					OutError,
					CancellationError.Code,
					Stage,
					CancellationError.Reason);
			}

			FUeAgentOperationDeadlineExceeded DeadlineError;
			if (!Deadline.TryCheckpoint(Stage, DeadlineError))
			{
				return Fail(
					OutError,
					DeadlineError.Code,
					Stage,
					DeadlineError.Stage);
			}
			return true;
		}

		int32 HexNibble(const TCHAR Character)
		{
			if (Character >= TEXT('0') && Character <= TEXT('9'))
			{
				return Character - TEXT('0');
			}
			if (Character >= TEXT('a') && Character <= TEXT('f'))
			{
				return Character - TEXT('a') + 10;
			}
			if (Character >= TEXT('A') && Character <= TEXT('F'))
			{
				return Character - TEXT('A') + 10;
			}
			return -1;
		}

		FString HexBytes(const uint8* Data, const int32 Count)
		{
			static const TCHAR Digits[] = TEXT("0123456789abcdef");
			FString Result;
			Result.Reserve(Count * 2);
			for (int32 Index = 0; Index < Count; ++Index)
			{
				Result.AppendChar(Digits[(Data[Index] >> 4) & 0x0f]);
				Result.AppendChar(Digits[Data[Index] & 0x0f]);
			}
			return Result;
		}

		FString EncodeCursorText(const FString& Value)
		{
			const FTCHARToUTF8 Utf8(*Value);
			return HexBytes(
				reinterpret_cast<const uint8*>(Utf8.Get()),
				Utf8.Length());
		}

		bool DecodeCursorText(const FString& Encoded, FString& OutValue)
		{
			if ((Encoded.Len() % 2) != 0)
			{
				return false;
			}
			const int32 ByteCount = Encoded.Len() / 2;
			TArray<ANSICHAR> Bytes;
			Bytes.SetNumUninitialized(ByteCount + 1);
			for (int32 Index = 0; Index < ByteCount; ++Index)
			{
				const int32 High = HexNibble(Encoded[Index * 2]);
				const int32 Low = HexNibble(Encoded[Index * 2 + 1]);
				if (High < 0 || Low < 0)
				{
					return false;
				}
				Bytes[Index] = static_cast<ANSICHAR>((High << 4) | Low);
			}
			Bytes[ByteCount] = '\0';
			const FUTF8ToTCHAR Converted(Bytes.GetData(), ByteCount);
			OutValue = FString(Converted.Length(), Converted.Get());
			return true;
		}

		FString CursorChecksum(const FString& Body)
		{
			const FTCHARToUTF8 Utf8(*Body);
			uint8 Digest[FSHA1::DigestSize];
			FSHA1::HashBuffer(Utf8.Get(), Utf8.Length(), Digest);
			return HexBytes(Digest, FSHA1::DigestSize);
		}

		FString ResultKindToken(const EUeAgentProjectionResultKind Kind)
		{
			return Kind == EUeAgentProjectionResultKind::Query
				? TEXT("q")
				: TEXT("o");
		}

		FCursorSource CursorSource(
			const FUeAgentCanonicalSnapshot& Snapshot)
		{
			return FCursorSource{
				TEXT("snapshot"),
				Snapshot.GetSnapshotId(),
				Snapshot.GetRevision().GetSourceId(),
				Snapshot.GetRevision().GetGeneration(),
				Snapshot.GetRevision().GetSequence(),
				Snapshot.GetEntries().Num(),
			};
		}

		FCursorSource CursorSource(const FUeAgentCatalogSnapshot& Catalog)
		{
			return FCursorSource{
				TEXT("catalog"),
				Catalog.GetCatalogId(),
				Catalog.GetRevision().GetSourceId(),
				Catalog.GetRevision().GetGeneration(),
				Catalog.GetRevision().GetSequence(),
				Catalog.GetCandidates().Num(),
			};
		}

		FString EncodeCursor(
			const FUeAgentDescriptorIdentity& DescriptorIdentity,
			const EUeAgentProjectionResultKind ResultKind,
			const FUeAgentProjectionProfile& Profile,
			const FCursorSource& Source,
			const int32 NextIndex)
		{
			const TArray<FString> Fields = {
				TEXT("uai-c10-v1"),
				EncodeCursorText(DescriptorIdentity.GetDescriptorId()),
				FString::FromInt(DescriptorIdentity.GetVersion()),
				EncodeCursorText(DescriptorIdentity.GetDescriptorHash()),
				ResultKindToken(ResultKind),
				EncodeCursorText(Profile.GetProfileId()),
				Source.Kind,
				EncodeCursorText(Source.SourceId),
				EncodeCursorText(Source.RevisionSourceId),
				EncodeCursorText(Source.RevisionGeneration),
				FString::Printf(
					TEXT("%lld"),
					static_cast<long long>(Source.RevisionSequence)),
				FString::FromInt(NextIndex),
			};
			const FString Body = FString::Join(Fields, TEXT("."));
			return Body + TEXT(".") + CursorChecksum(Body);
		}

		bool DecodeCursor(
			const FString& Cursor,
			const FUeAgentDescriptorIdentity& DescriptorIdentity,
			const EUeAgentProjectionResultKind ResultKind,
			const FUeAgentProjectionProfile& Profile,
			const FCursorSource& Source,
			int32& OutNextIndex,
			FUeAgentProjectionError& OutError)
		{
			TArray<FString> Fields;
			Cursor.ParseIntoArray(Fields, TEXT("."), false);
			if (Fields.Num() != CursorFieldCount)
			{
				return Fail(
					OutError,
					TEXT("projection_cursor_invalid"),
					Source.SourceId,
					TEXT("cursor field count is invalid"));
			}
			TArray<FString> BodyFields = Fields;
			BodyFields.RemoveAt(CursorFieldCount - 1);
			const FString Body = FString::Join(BodyFields, TEXT("."));
			if (Fields[0] != TEXT("uai-c10-v1")
				|| Fields[12] != CursorChecksum(Body))
			{
				return Fail(
					OutError,
					TEXT("projection_cursor_invalid"),
					Source.SourceId,
					TEXT("cursor schema or integrity is invalid"));
			}

			FString CursorDescriptorId;
			FString CursorDescriptorHash;
			FString CursorProfileId;
			FString CursorSourceId;
			FString CursorRevisionSourceId;
			FString CursorRevisionGeneration;
			int32 CursorDescriptorVersion = 0;
			int64 CursorRevisionSequence = -1;
			int32 CursorNextIndex = -1;
			if (!DecodeCursorText(Fields[1], CursorDescriptorId)
				|| !LexTryParseString(
					CursorDescriptorVersion,
					*Fields[2])
				|| !DecodeCursorText(Fields[3], CursorDescriptorHash)
				|| !DecodeCursorText(Fields[5], CursorProfileId)
				|| !DecodeCursorText(Fields[7], CursorSourceId)
				|| !DecodeCursorText(
					Fields[8],
					CursorRevisionSourceId)
				|| !DecodeCursorText(
					Fields[9],
					CursorRevisionGeneration)
				|| !LexTryParseString(
					CursorRevisionSequence,
					*Fields[10])
				|| !LexTryParseString(CursorNextIndex, *Fields[11]))
			{
				return Fail(
					OutError,
					TEXT("projection_cursor_invalid"),
					Source.SourceId,
					TEXT("cursor field encoding is invalid"));
			}

			if (CursorDescriptorId
					!= DescriptorIdentity.GetDescriptorId()
				|| CursorDescriptorVersion
					!= DescriptorIdentity.GetVersion()
				|| CursorDescriptorHash
					!= DescriptorIdentity.GetDescriptorHash())
			{
				return Fail(
					OutError,
					TEXT("projection_cursor_descriptor_mismatch"),
					DescriptorIdentity.GetDescriptorId(),
					TEXT("cursor descriptor does not match result"));
			}
			if (Fields[4] != ResultKindToken(ResultKind))
			{
				return Fail(
					OutError,
					TEXT("projection_cursor_result_kind_mismatch"),
					Source.SourceId,
					TEXT("cursor result kind does not match result"));
			}
			if (CursorProfileId != Profile.GetProfileId())
			{
				return Fail(
					OutError,
					TEXT("projection_cursor_profile_mismatch"),
					Profile.GetProfileId(),
					TEXT("cursor profile does not match request"));
			}
			if (Fields[6] != Source.Kind
				|| CursorSourceId != Source.SourceId)
			{
				return Fail(
					OutError,
					TEXT("projection_cursor_source_mismatch"),
					Source.SourceId,
					TEXT("cursor source does not match payload"));
			}
			if (CursorRevisionSourceId != Source.RevisionSourceId
				|| CursorRevisionGeneration != Source.RevisionGeneration
				|| CursorRevisionSequence != Source.RevisionSequence)
			{
				return Fail(
					OutError,
					TEXT("projection_cursor_revision_mismatch"),
					Source.SourceId,
					TEXT("cursor revision does not match payload"));
			}
			if (CursorNextIndex <= 0
				|| CursorNextIndex >= Source.TotalItems)
			{
				return Fail(
					OutError,
					TEXT("projection_cursor_invalid"),
					Source.SourceId,
					TEXT("cursor position is outside payload"));
			}
			OutNextIndex = CursorNextIndex;
			return true;
		}

		bool TryPageStart(
			const FUeAgentPageRequest& Page,
			const FUeAgentDescriptorIdentity& DescriptorIdentity,
			const EUeAgentProjectionResultKind ResultKind,
			const FUeAgentProjectionProfile& Profile,
			const FCursorSource& Source,
			FUeAgentResourceTracker& Resources,
			int32& OutStart,
			FUeAgentProjectionError& OutError)
		{
			OutStart = 0;
			if (Page.GetCursor().IsEmpty())
			{
				return true;
			}
			const FUeAgentTextUnits Units = MeasureText(Page.GetCursor());
			if (!Charge(
				Resources,
				{
					{EUeAgentResourceDimension::InputBytes, Units.Utf8Bytes},
					{EUeAgentResourceDimension::StringBytes, Units.Utf8Bytes},
					{EUeAgentResourceDimension::WorkUnits, 1},
				},
				TEXT("/projection/cursor"),
				OutError))
			{
				return false;
			}
			return DecodeCursor(
				Page.GetCursor(),
				DescriptorIdentity,
				ResultKind,
				Profile,
				Source,
				OutStart,
				OutError);
		}

		bool ChargePayload(
			FUeAgentResourceTracker& Resources,
			const int32 ItemCount,
			FUeAgentProjectionError& OutError)
		{
			return Charge(
				Resources,
				{
					{EUeAgentResourceDimension::Nodes, 1 + ItemCount},
					{EUeAgentResourceDimension::Items, ItemCount},
					{EUeAgentResourceDimension::WorkUnits, 1 + ItemCount},
					{
						EUeAgentResourceDimension::EstimatedMemoryBytes,
						160 + static_cast<int64>(ItemCount) * 96,
					},
				},
				TEXT("/projection/payload"),
				OutError);
		}

		bool AllArtifactsValid(
			const TArray<FUeAgentArtifactDescriptor>& Artifacts)
		{
			for (const FUeAgentArtifactDescriptor& Artifact : Artifacts)
			{
				if (!Artifact.IsValid())
				{
					return false;
				}
			}
			return true;
		}

		bool AllDiagnosticsValid(
			const TArray<FUeAgentProjectionDiagnostic>& Diagnostics)
		{
			for (const FUeAgentProjectionDiagnostic& Diagnostic : Diagnostics)
			{
				if (!Diagnostic.IsValid())
				{
					return false;
				}
			}
			return true;
		}

		bool AllEntriesValid(
			const TArray<FUeAgentProjectedEntry>& Entries)
		{
			for (const FUeAgentProjectedEntry& Entry : Entries)
			{
				if (!Entry.IsValid())
				{
					return false;
				}
			}
			return true;
		}

		bool AllCandidatesValid(
			const TArray<FUeAgentCandidateSummary>& Candidates)
		{
			for (const FUeAgentCandidateSummary& Candidate : Candidates)
			{
				if (!Candidate.IsValid())
				{
					return false;
				}
			}
			return true;
		}

		bool AllCatalogDiagnosticsValid(
			const TArray<FUeAgentCatalogDiagnostic>& Diagnostics)
		{
			for (const FUeAgentCatalogDiagnostic& Diagnostic : Diagnostics)
			{
				if (!Diagnostic.IsValid())
				{
					return false;
				}
			}
			return true;
		}
	}

	FUeAgentProjectionProfile::FUeAgentProjectionProfile(
		FString InProfileId,
		const bool bInIncludeDefaults,
		const bool bInIncludeDetails,
		const bool bInIncludeProvenance)
		: ProfileId(NormalizeRequired(MoveTemp(InProfileId)))
		, bIncludeDefaults(bInIncludeDefaults)
		, bIncludeDetails(bInIncludeDetails)
		, bIncludeProvenance(bInIncludeProvenance)
	{
	}

	const FString& FUeAgentProjectionProfile::GetProfileId() const
	{
		return ProfileId;
	}

	bool FUeAgentProjectionProfile::IncludesDefaults() const
	{
		return bIncludeDefaults;
	}

	bool FUeAgentProjectionProfile::IncludesDetails() const
	{
		return bIncludeDetails;
	}

	bool FUeAgentProjectionProfile::IncludesProvenance() const
	{
		return bIncludeProvenance;
	}

	bool FUeAgentProjectionProfile::IsValid() const
	{
		return !ProfileId.IsEmpty();
	}

	FUeAgentPageRequest::FUeAgentPageRequest() = default;

	FUeAgentPageRequest::FUeAgentPageRequest(FString InCursor)
		: Cursor(NormalizeOptional(MoveTemp(InCursor)))
		, bValid(!Cursor.IsEmpty())
	{
	}

	FUeAgentPageRequest::FUeAgentPageRequest(
		const int32 InMaxItems,
		FString InCursor)
		: MaxItems(InMaxItems)
		, Cursor(NormalizeOptional(MoveTemp(InCursor)))
		, bValid(InMaxItems > 0)
	{
	}

	bool FUeAgentPageRequest::HasMaxItems() const
	{
		return MaxItems.IsSet();
	}

	int32 FUeAgentPageRequest::GetMaxItems() const
	{
		return MaxItems.IsSet() ? MaxItems.GetValue() : 0;
	}

	const FString& FUeAgentPageRequest::GetCursor() const
	{
		return Cursor;
	}

	bool FUeAgentPageRequest::IsValid() const
	{
		return bValid;
	}

	FUeAgentProjectionDiagnostic::FUeAgentProjectionDiagnostic(
		FString InCode,
		const EUeAgentDiagnosticSeverity InSeverity,
		FString InMessage,
		FString InPath,
		FString InSubjectId,
		const bool bInRetrySafe)
		: Code(NormalizeRequired(MoveTemp(InCode)))
		, Severity(InSeverity)
		, Message(NormalizeRequired(MoveTemp(InMessage)))
		, Path(NormalizeOptional(MoveTemp(InPath)))
		, SubjectId(NormalizeOptional(MoveTemp(InSubjectId)))
		, bRetrySafe(bInRetrySafe)
		, bValid(
			!Code.IsEmpty()
			&& !Message.IsEmpty()
			&& (Path.IsEmpty() || Path.StartsWith(TEXT("/"))))
	{
	}

	const FString& FUeAgentProjectionDiagnostic::GetCode() const
	{
		return Code;
	}

	EUeAgentDiagnosticSeverity
	FUeAgentProjectionDiagnostic::GetSeverity() const
	{
		return Severity;
	}

	const FString& FUeAgentProjectionDiagnostic::GetMessage() const
	{
		return Message;
	}

	const FString& FUeAgentProjectionDiagnostic::GetPath() const
	{
		return Path;
	}

	const FString& FUeAgentProjectionDiagnostic::GetSubjectId() const
	{
		return SubjectId;
	}

	bool FUeAgentProjectionDiagnostic::IsRetrySafe() const
	{
		return bRetrySafe;
	}

	bool FUeAgentProjectionDiagnostic::IsValid() const
	{
		return bValid;
	}

	FUeAgentArtifactDescriptor::FUeAgentArtifactDescriptor(
		FString InArtifactId,
		FString InKind,
		FString InLocator,
		FString InMediaType,
		FString InContentHash,
		const int64 InByteCount,
		FString InReceiptId,
		const bool bInImportable)
		: ArtifactId(NormalizeRequired(MoveTemp(InArtifactId)))
		, Kind(NormalizeRequired(MoveTemp(InKind)))
		, Locator(NormalizeRequired(MoveTemp(InLocator)))
		, MediaType(NormalizeRequired(MoveTemp(InMediaType)))
		, ContentHash(NormalizeRequired(MoveTemp(InContentHash)).ToLower())
		, ByteCount(InByteCount)
		, ReceiptId(NormalizeRequired(MoveTemp(InReceiptId)))
		, bImportable(bInImportable)
		, bValid(
			!ArtifactId.IsEmpty()
			&& !Kind.IsEmpty()
			&& !Locator.IsEmpty()
			&& !MediaType.IsEmpty()
			&& IsSha256(ContentHash)
			&& ByteCount >= 0
			&& !ReceiptId.IsEmpty())
	{
	}

	const FString& FUeAgentArtifactDescriptor::GetArtifactId() const
	{
		return ArtifactId;
	}

	const FString& FUeAgentArtifactDescriptor::GetKind() const
	{
		return Kind;
	}

	const FString& FUeAgentArtifactDescriptor::GetLocator() const
	{
		return Locator;
	}

	const FString& FUeAgentArtifactDescriptor::GetMediaType() const
	{
		return MediaType;
	}

	const FString& FUeAgentArtifactDescriptor::GetContentHash() const
	{
		return ContentHash;
	}

	int64 FUeAgentArtifactDescriptor::GetByteCount() const
	{
		return ByteCount;
	}

	const FString& FUeAgentArtifactDescriptor::GetReceiptId() const
	{
		return ReceiptId;
	}

	bool FUeAgentArtifactDescriptor::IsImportable() const
	{
		return bImportable;
	}

	bool FUeAgentArtifactDescriptor::IsValid() const
	{
		return bValid;
	}

	FUeAgentTokenEstimate::FUeAgentTokenEstimate() = default;

	FUeAgentTokenEstimate::FUeAgentTokenEstimate(
		const EUeAgentTokenEstimateKind InKind,
		const int64 InTokens,
		const int64 InUtf8Bytes)
		: Kind(InKind)
		, Tokens(InTokens)
		, Utf8Bytes(InUtf8Bytes)
		, bValid(InTokens >= 0 && InUtf8Bytes >= 0)
	{
	}

	EUeAgentTokenEstimateKind FUeAgentTokenEstimate::GetKind() const
	{
		return Kind;
	}

	int64 FUeAgentTokenEstimate::GetTokens() const
	{
		return Tokens;
	}

	int64 FUeAgentTokenEstimate::GetUtf8Bytes() const
	{
		return Utf8Bytes;
	}

	bool FUeAgentTokenEstimate::IsValid() const
	{
		return bValid;
	}

	FUeAgentProjectedEntry::FUeAgentProjectedEntry(
		FUeAgentCanonicalIdentity InIdentity,
		FUeAgentCanonicalType InType,
		FUeAgentCanonicalValueRef InValue,
		FUeAgentStateRevision InObservedRevision,
		TOptional<FUeAgentDefaultValue> InDefault,
		TOptional<FUeAgentStateDetails> InDetails)
		: Identity(MoveTemp(InIdentity))
		, Type(MoveTemp(InType))
		, Value(MoveTemp(InValue))
		, ObservedRevision(MoveTemp(InObservedRevision))
		, Default(MoveTemp(InDefault))
		, Details(MoveTemp(InDetails))
		, bValid(
			Identity.IsValid()
			&& Type.IsValid()
			&& ObservedRevision.IsValid()
			&& (!Default.IsSet() || Default->IsValid())
			&& (!Details.IsSet() || Details->IsValid()))
	{
	}

	const FUeAgentCanonicalIdentity&
	FUeAgentProjectedEntry::GetIdentity() const
	{
		return Identity;
	}

	const FUeAgentCanonicalType& FUeAgentProjectedEntry::GetType() const
	{
		return Type;
	}

	const FUeAgentCanonicalValueRef&
	FUeAgentProjectedEntry::GetValue() const
	{
		return Value;
	}

	const FUeAgentStateRevision&
	FUeAgentProjectedEntry::GetObservedRevision() const
	{
		return ObservedRevision;
	}

	const TOptional<FUeAgentDefaultValue>&
	FUeAgentProjectedEntry::GetDefault() const
	{
		return Default;
	}

	const TOptional<FUeAgentStateDetails>&
	FUeAgentProjectedEntry::GetDetails() const
	{
		return Details;
	}

	bool FUeAgentProjectedEntry::IsValid() const
	{
		return bValid;
	}

	FUeAgentPageInfo::FUeAgentPageInfo(
		const int32 InReturnedItems,
		const int32 InTotalItems,
		const bool bInHasMore,
		FString InNextCursor)
		: ReturnedItems(InReturnedItems)
		, TotalItems(InTotalItems)
		, bHasMore(bInHasMore)
		, NextCursor(NormalizeOptional(MoveTemp(InNextCursor)))
		, bValid(
			InReturnedItems >= 0
			&& InTotalItems >= 0
			&& InReturnedItems <= InTotalItems
			&& (bInHasMore
				? !NextCursor.IsEmpty()
				: NextCursor.IsEmpty()))
	{
	}

	int32 FUeAgentPageInfo::GetReturnedItems() const
	{
		return ReturnedItems;
	}

	int32 FUeAgentPageInfo::GetTotalItems() const
	{
		return TotalItems;
	}

	bool FUeAgentPageInfo::HasMore() const
	{
		return bHasMore;
	}

	const FString& FUeAgentPageInfo::GetNextCursor() const
	{
		return NextCursor;
	}

	bool FUeAgentPageInfo::IsValid() const
	{
		return bValid;
	}

	FUeAgentProjectedSnapshot::FUeAgentProjectedSnapshot(
		FString InSnapshotId,
		const EUeAgentStatePlane InStatePlane,
		FUeAgentStateRevision InRevision,
		FUeAgentTemporalPoint InCapturedAt,
		TOptional<FUeAgentStateProvenance> InProvenance,
		TArray<FUeAgentProjectedEntry> InEntries,
		FUeAgentPageInfo InPage,
		TArray<FString> InOmittedSections)
		: SnapshotId(NormalizeRequired(MoveTemp(InSnapshotId)))
		, StatePlane(InStatePlane)
		, Revision(MoveTemp(InRevision))
		, CapturedAt(MoveTemp(InCapturedAt))
		, Provenance(MoveTemp(InProvenance))
		, Entries(MoveTemp(InEntries))
		, Page(MoveTemp(InPage))
		, OmittedSections(MoveTemp(InOmittedSections))
		, bValid(
			!SnapshotId.IsEmpty()
			&& Revision.IsValid()
			&& CapturedAt.IsValid()
			&& (!Provenance.IsSet() || Provenance->IsValid())
			&& AllEntriesValid(Entries)
			&& Page.IsValid())
	{
	}

	const FString& FUeAgentProjectedSnapshot::GetSnapshotId() const
	{
		return SnapshotId;
	}

	EUeAgentStatePlane FUeAgentProjectedSnapshot::GetStatePlane() const
	{
		return StatePlane;
	}

	const FUeAgentStateRevision&
	FUeAgentProjectedSnapshot::GetRevision() const
	{
		return Revision;
	}

	const FUeAgentTemporalPoint&
	FUeAgentProjectedSnapshot::GetCapturedAt() const
	{
		return CapturedAt;
	}

	const TOptional<FUeAgentStateProvenance>&
	FUeAgentProjectedSnapshot::GetProvenance() const
	{
		return Provenance;
	}

	const TArray<FUeAgentProjectedEntry>&
	FUeAgentProjectedSnapshot::GetEntries() const
	{
		return Entries;
	}

	const FUeAgentPageInfo& FUeAgentProjectedSnapshot::GetPage() const
	{
		return Page;
	}

	const TArray<FString>&
	FUeAgentProjectedSnapshot::GetOmittedSections() const
	{
		return OmittedSections;
	}

	bool FUeAgentProjectedSnapshot::IsValid() const
	{
		return bValid;
	}

	FUeAgentProjectedCatalog::FUeAgentProjectedCatalog(
		FString InCatalogId,
		FString InCatalogKind,
		FUeAgentCatalogRevision InRevision,
		const EUeAgentCatalogCompleteness InCompleteness,
		TArray<FUeAgentCandidateSummary> InCandidates,
		TArray<FUeAgentCatalogDiagnostic> InDiagnostics,
		FUeAgentPageInfo InPage)
		: CatalogId(NormalizeRequired(MoveTemp(InCatalogId)))
		, CatalogKind(NormalizeRequired(MoveTemp(InCatalogKind)))
		, Revision(MoveTemp(InRevision))
		, Completeness(InCompleteness)
		, Candidates(MoveTemp(InCandidates))
		, Diagnostics(MoveTemp(InDiagnostics))
		, Page(MoveTemp(InPage))
		, bValid(
			!CatalogId.IsEmpty()
			&& !CatalogKind.IsEmpty()
			&& Revision.IsValid()
			&& AllCandidatesValid(Candidates)
			&& AllCatalogDiagnosticsValid(Diagnostics)
			&& Page.IsValid())
	{
	}

	const FString& FUeAgentProjectedCatalog::GetCatalogId() const
	{
		return CatalogId;
	}

	const FString& FUeAgentProjectedCatalog::GetCatalogKind() const
	{
		return CatalogKind;
	}

	const FUeAgentCatalogRevision&
	FUeAgentProjectedCatalog::GetRevision() const
	{
		return Revision;
	}

	EUeAgentCatalogCompleteness
	FUeAgentProjectedCatalog::GetCompleteness() const
	{
		return Completeness;
	}

	const TArray<FUeAgentCandidateSummary>&
	FUeAgentProjectedCatalog::GetCandidates() const
	{
		return Candidates;
	}

	const TArray<FUeAgentCatalogDiagnostic>&
	FUeAgentProjectedCatalog::GetDiagnostics() const
	{
		return Diagnostics;
	}

	const FUeAgentPageInfo& FUeAgentProjectedCatalog::GetPage() const
	{
		return Page;
	}

	bool FUeAgentProjectedCatalog::IsValid() const
	{
		return bValid;
	}

	FUeAgentProjectedPayload::FUeAgentProjectedPayload(
		TSharedRef<const FUeAgentProjectedSnapshot> InSnapshot)
		: Snapshot(MoveTemp(InSnapshot))
	{
	}

	FUeAgentProjectedPayload::FUeAgentProjectedPayload(
		TSharedRef<const FUeAgentProjectedCatalog> InCatalog)
		: Catalog(MoveTemp(InCatalog))
	{
	}

	FUeAgentProjectedPayload::FUeAgentProjectedPayload(
		TSharedRef<const FUeAgentCandidateBinding> InCandidateBinding)
		: CandidateBinding(MoveTemp(InCandidateBinding))
	{
	}

	FUeAgentProjectedPayload::FUeAgentProjectedPayload(
		FUeAgentCanonicalValueRef InValue)
		: Value(MoveTemp(InValue))
	{
	}

	FUeAgentProjectedPayload::FUeAgentProjectedPayload(
		TSharedRef<const FUeAgentSemanticComparison> InComparison)
		: Comparison(MoveTemp(InComparison))
	{
	}

	const TSharedPtr<const FUeAgentProjectedSnapshot>&
	FUeAgentProjectedPayload::GetSnapshot() const
	{
		return Snapshot;
	}

	const TSharedPtr<const FUeAgentProjectedCatalog>&
	FUeAgentProjectedPayload::GetCatalog() const
	{
		return Catalog;
	}

	const TSharedPtr<const FUeAgentCandidateBinding>&
	FUeAgentProjectedPayload::GetCandidateBinding() const
	{
		return CandidateBinding;
	}

	const TSharedPtr<const FUeAgentCanonicalValue>&
	FUeAgentProjectedPayload::GetValue() const
	{
		return Value;
	}

	const TSharedPtr<const FUeAgentSemanticComparison>&
	FUeAgentProjectedPayload::GetComparison() const
	{
		return Comparison;
	}

	bool FUeAgentProjectedPayload::IsValid() const
	{
		return static_cast<int32>(Snapshot.IsValid())
			+ static_cast<int32>(Catalog.IsValid())
			+ static_cast<int32>(CandidateBinding.IsValid())
			+ static_cast<int32>(Value.IsValid())
			+ static_cast<int32>(Comparison.IsValid())
			== 1;
	}

	FUeAgentProjectionRequest::FUeAgentProjectionRequest(
		FUeAgentProjectionProfile InProfile,
		FUeAgentPageRequest InPage,
		TOptional<int64> InTokenBudget,
		TArray<FUeAgentArtifactDescriptor> InArtifacts,
		TArray<FUeAgentProjectionDiagnostic> InDiagnostics)
		: Profile(MoveTemp(InProfile))
		, Page(MoveTemp(InPage))
		, TokenBudget(MoveTemp(InTokenBudget))
		, Artifacts(MoveTemp(InArtifacts))
		, Diagnostics(MoveTemp(InDiagnostics))
		, bValid(
			Profile.IsValid()
			&& Page.IsValid()
			&& (!TokenBudget.IsSet() || TokenBudget.GetValue() >= 0)
			&& AllArtifactsValid(Artifacts)
			&& AllDiagnosticsValid(Diagnostics))
	{
	}

	const FUeAgentProjectionProfile&
	FUeAgentProjectionRequest::GetProfile() const
	{
		return Profile;
	}

	const FUeAgentPageRequest& FUeAgentProjectionRequest::GetPage() const
	{
		return Page;
	}

	const TOptional<int64>&
	FUeAgentProjectionRequest::GetTokenBudget() const
	{
		return TokenBudget;
	}

	const TArray<FUeAgentArtifactDescriptor>&
	FUeAgentProjectionRequest::GetArtifacts() const
	{
		return Artifacts;
	}

	const TArray<FUeAgentProjectionDiagnostic>&
	FUeAgentProjectionRequest::GetDiagnostics() const
	{
		return Diagnostics;
	}

	bool FUeAgentProjectionRequest::IsValid() const
	{
		return bValid;
	}

	FUeAgentProjectionSourceValue::FUeAgentProjectionSourceValue(
		TSharedRef<const FUeAgentCanonicalSnapshot> InSnapshot)
		: Snapshot(MoveTemp(InSnapshot))
	{
	}

	FUeAgentProjectionSourceValue::FUeAgentProjectionSourceValue(
		TSharedRef<const FUeAgentCatalogSnapshot> InCatalog)
		: Catalog(MoveTemp(InCatalog))
	{
	}

	FUeAgentProjectionSourceValue::FUeAgentProjectionSourceValue(
		TSharedRef<const FUeAgentCandidateBinding> InCandidateBinding)
		: CandidateBinding(MoveTemp(InCandidateBinding))
	{
	}

	FUeAgentProjectionSourceValue::FUeAgentProjectionSourceValue(
		FUeAgentCanonicalValueRef InValue)
		: Value(MoveTemp(InValue))
	{
	}

	FUeAgentProjectionSourceValue::FUeAgentProjectionSourceValue(
		TSharedRef<const FUeAgentSemanticComparison> InComparison)
		: Comparison(MoveTemp(InComparison))
	{
	}

	const TSharedPtr<const FUeAgentCanonicalSnapshot>&
	FUeAgentProjectionSourceValue::GetSnapshot() const
	{
		return Snapshot;
	}

	const TSharedPtr<const FUeAgentCatalogSnapshot>&
	FUeAgentProjectionSourceValue::GetCatalog() const
	{
		return Catalog;
	}

	const TSharedPtr<const FUeAgentCandidateBinding>&
	FUeAgentProjectionSourceValue::GetCandidateBinding() const
	{
		return CandidateBinding;
	}

	const TSharedPtr<const FUeAgentCanonicalValue>&
	FUeAgentProjectionSourceValue::GetValue() const
	{
		return Value;
	}

	const TSharedPtr<const FUeAgentSemanticComparison>&
	FUeAgentProjectionSourceValue::GetComparison() const
	{
		return Comparison;
	}

	bool FUeAgentProjectionSourceValue::IsValid() const
	{
		return static_cast<int32>(Snapshot.IsValid())
			+ static_cast<int32>(Catalog.IsValid())
			+ static_cast<int32>(CandidateBinding.IsValid())
			+ static_cast<int32>(Value.IsValid())
			+ static_cast<int32>(Comparison.IsValid())
			== 1;
	}

	FUeAgentProjectedResponse::FUeAgentProjectedResponse(
		FUeAgentDescriptorIdentity InDescriptorIdentity,
		FString InRequestId,
		const EUeAgentProjectionResultKind InResultKind,
		FString InProfileId,
		const EUeAgentProjectionCompleteness InCompleteness,
		FUeAgentProjectedPayload InPayload,
		TArray<FUeAgentArtifactDescriptor> InArtifacts,
		TArray<FUeAgentProjectionDiagnostic> InDiagnostics,
		FUeAgentTokenEstimate InTokenEstimate)
		: DescriptorIdentity(MoveTemp(InDescriptorIdentity))
		, RequestId(NormalizeRequired(MoveTemp(InRequestId)))
		, ResultKind(InResultKind)
		, ProfileId(NormalizeRequired(MoveTemp(InProfileId)))
		, Completeness(InCompleteness)
		, Payload(MoveTemp(InPayload))
		, Artifacts(MoveTemp(InArtifacts))
		, Diagnostics(MoveTemp(InDiagnostics))
		, TokenEstimate(MoveTemp(InTokenEstimate))
		, bValid(
			DescriptorIdentity.IsValid()
			&& !RequestId.IsEmpty()
			&& !ProfileId.IsEmpty()
			&& Payload.IsValid()
			&& AllArtifactsValid(Artifacts)
			&& AllDiagnosticsValid(Diagnostics)
			&& TokenEstimate.IsValid())
	{
	}

	const FUeAgentDescriptorIdentity&
	FUeAgentProjectedResponse::GetDescriptorIdentity() const
	{
		return DescriptorIdentity;
	}

	const FString& FUeAgentProjectedResponse::GetRequestId() const
	{
		return RequestId;
	}

	EUeAgentProjectionResultKind
	FUeAgentProjectedResponse::GetResultKind() const
	{
		return ResultKind;
	}

	const FString& FUeAgentProjectedResponse::GetProfileId() const
	{
		return ProfileId;
	}

	EUeAgentProjectionCompleteness
	FUeAgentProjectedResponse::GetCompleteness() const
	{
		return Completeness;
	}

	const FUeAgentProjectedPayload&
	FUeAgentProjectedResponse::GetPayload() const
	{
		return Payload;
	}

	const TArray<FUeAgentArtifactDescriptor>&
	FUeAgentProjectedResponse::GetArtifacts() const
	{
		return Artifacts;
	}

	const TArray<FUeAgentProjectionDiagnostic>&
	FUeAgentProjectedResponse::GetDiagnostics() const
	{
		return Diagnostics;
	}

	const FUeAgentTokenEstimate&
	FUeAgentProjectedResponse::GetTokenEstimate() const
	{
		return TokenEstimate;
	}

	bool FUeAgentProjectedResponse::IsValid() const
	{
		return bValid;
	}

	FUeAgentResponseProjector::FUeAgentResponseProjector(
		TSharedRef<IUeAgentTokenEstimator> InEstimator)
		: Estimator(MoveTemp(InEstimator))
	{
	}

	bool FUeAgentResponseProjector::TryCreate(
		const TSharedRef<IUeAgentTokenEstimator>& Estimator,
		TSharedPtr<const FUeAgentResponseProjector>& OutProjector,
		FUeAgentProjectionError& OutError)
	{
		ResetError(OutError);
		OutProjector = MakeShareable(
			new FUeAgentResponseProjector(Estimator));
		return true;
	}

	bool FUeAgentResponseProjector::TryProject(
		const FUeAgentProjectionQueryResult& Result,
		const FUeAgentProjectionRequest& Request,
		FUeAgentResourceTracker& Resources,
		const FUeAgentOperationDeadline& Deadline,
		const FUeAgentCancellationToken& Cancellation,
		TSharedPtr<const FUeAgentProjectedResponse>& OutResponse,
		FUeAgentProjectionError& OutError) const
	{
		ResetError(OutError);
		OutResponse.Reset();
		if (!Result.IsValid())
		{
			return Fail(
				OutError,
				TEXT("projection_result_invalid"),
				Result.GetRequestId(),
				TEXT("C0 QueryResult is invalid"));
		}
		return TryProjectInternal(
			Result.GetDescriptorIdentity(),
			Result.GetRequestId(),
			EUeAgentProjectionResultKind::Query,
			Result.GetValue(),
			Request,
			Resources,
			Deadline,
			Cancellation,
			OutResponse,
			OutError);
	}

	bool FUeAgentResponseProjector::TryProject(
		const FUeAgentProjectionOperationResult& Result,
		const FUeAgentProjectionRequest& Request,
		FUeAgentResourceTracker& Resources,
		const FUeAgentOperationDeadline& Deadline,
		const FUeAgentCancellationToken& Cancellation,
		TSharedPtr<const FUeAgentProjectedResponse>& OutResponse,
		FUeAgentProjectionError& OutError) const
	{
		ResetError(OutError);
		OutResponse.Reset();
		if (!Result.IsValid())
		{
			return Fail(
				OutError,
				TEXT("projection_result_invalid"),
				Result.GetRequestId(),
				TEXT("C0 OperationResult is invalid"));
		}
		return TryProjectInternal(
			Result.GetDescriptorIdentity(),
			Result.GetRequestId(),
			EUeAgentProjectionResultKind::Operation,
			Result.GetValue(),
			Request,
			Resources,
			Deadline,
			Cancellation,
			OutResponse,
			OutError);
	}

	bool FUeAgentResponseProjector::TryProjectInternal(
		const FUeAgentDescriptorIdentity& DescriptorIdentity,
		const FString& RequestId,
		const EUeAgentProjectionResultKind ResultKind,
		const FUeAgentProjectionSourceValue& Source,
		const FUeAgentProjectionRequest& Request,
		FUeAgentResourceTracker& Resources,
		const FUeAgentOperationDeadline& Deadline,
		const FUeAgentCancellationToken& Cancellation,
		TSharedPtr<const FUeAgentProjectedResponse>& OutResponse,
		FUeAgentProjectionError& OutError) const
	{
		if (!Request.IsValid())
		{
			return Fail(
				OutError,
				TEXT("projection_request_invalid"),
				RequestId,
				TEXT("projection request is invalid"));
		}
		if (!Source.IsValid())
		{
			return Fail(
				OutError,
				TEXT("projection_value_type_unsupported"),
				RequestId,
				TEXT("result does not contain one typed C2/C4 payload"));
		}
		if (!Checkpoint(
			TEXT("projection_begin"),
			Deadline,
			Cancellation,
			OutError))
		{
			return false;
		}

		auto TryEstimate =
			[&](
				const FUeAgentProjectedPayload& Payload,
				FUeAgentTokenEstimate& OutEstimate) -> bool
			{
				if (!Checkpoint(
					TEXT("projection_token_estimate"),
					Deadline,
					Cancellation,
					OutError))
				{
					return false;
				}
				FUeAgentProjectionError EstimateError;
				if (!Estimator->TryEstimate(
					Payload,
					Request.GetArtifacts(),
					Request.GetDiagnostics(),
					Resources,
					Deadline,
					Cancellation,
					OutEstimate,
					EstimateError))
				{
					OutError = MoveTemp(EstimateError);
					if (OutError.Code.IsEmpty())
					{
						return Fail(
							OutError,
							TEXT("projection_token_estimate_failed"),
							RequestId,
							TEXT("token estimator returned failure"));
					}
					return false;
				}
				if (!OutEstimate.IsValid())
				{
					return Fail(
						OutError,
						TEXT("projection_token_estimate_invalid"),
						RequestId,
						TEXT("token estimator returned invalid estimate"));
				}
				return true;
			};

		TSharedPtr<const FUeAgentProjectedSnapshot> ProjectedSnapshot;
		TSharedPtr<const FUeAgentProjectedCatalog> ProjectedCatalog;
		TOptional<FUeAgentProjectedPayload> FinalPayload;
		FUeAgentTokenEstimate FinalEstimate;
		bool bHasMore = false;

		if (Source.GetSnapshot().IsValid())
		{
			const FUeAgentCanonicalSnapshot& Snapshot =
				*Source.GetSnapshot();
			const FCursorSource PagedSource = CursorSource(Snapshot);
			int32 Start = 0;
			if (!TryPageStart(
				Request.GetPage(),
				DescriptorIdentity,
				ResultKind,
				Request.GetProfile(),
				PagedSource,
				Resources,
				Start,
				OutError))
			{
				return false;
			}
			int32 MaximumCount = PagedSource.TotalItems - Start;
			if (Request.GetPage().HasMaxItems())
			{
				MaximumCount = FMath::Min(
					MaximumCount,
					Request.GetPage().GetMaxItems());
			}

			auto TryBuild =
				[&](
					const int32 Count,
					TSharedPtr<const FUeAgentProjectedSnapshot>& OutSnapshot,
					FUeAgentTokenEstimate& OutEstimate) -> bool
				{
					if (!Checkpoint(
						TEXT("projection_snapshot"),
						Deadline,
						Cancellation,
						OutError)
						|| !ChargePayload(Resources, Count, OutError))
					{
						return false;
					}
					TArray<FUeAgentProjectedEntry> Entries;
					Entries.Reserve(Count);
					for (int32 Offset = 0; Offset < Count; ++Offset)
					{
						const FUeAgentStateEntry& Entry =
							Snapshot.GetEntries()[Start + Offset];
						TOptional<FUeAgentDefaultValue> Default;
						TOptional<FUeAgentStateDetails> Details;
						if (Request.GetProfile().IncludesDefaults())
						{
							Default.Emplace(Entry.GetDefault());
						}
						if (Request.GetProfile().IncludesDetails())
						{
							Details.Emplace(Entry.GetDetails());
						}
						Entries.Emplace(
							Entry.GetIdentity(),
							Entry.GetType(),
							Entry.GetValue(),
							Entry.GetObservedRevision(),
							MoveTemp(Default),
							MoveTemp(Details));
					}
					const int32 End = Start + Count;
					const bool bPageHasMore =
						End < PagedSource.TotalItems;
					const FString NextCursor = bPageHasMore
						? EncodeCursor(
							DescriptorIdentity,
							ResultKind,
							Request.GetProfile(),
							PagedSource,
							End)
						: FString();
					TOptional<FUeAgentStateProvenance> Provenance;
					if (Request.GetProfile().IncludesProvenance())
					{
						Provenance.Emplace(Snapshot.GetProvenance());
					}
					TArray<FString> Omissions;
					if (!Request.GetProfile().IncludesDefaults())
					{
						Omissions.Add(TEXT("defaults"));
					}
					if (!Request.GetProfile().IncludesDetails())
					{
						Omissions.Add(TEXT("details"));
					}
					if (!Request.GetProfile().IncludesProvenance())
					{
						Omissions.Add(TEXT("provenance"));
					}
					OutSnapshot =
						MakeShared<FUeAgentProjectedSnapshot>(
							Snapshot.GetSnapshotId(),
							Snapshot.GetStatePlane(),
							Snapshot.GetRevision(),
							Snapshot.GetCapturedAt(),
							MoveTemp(Provenance),
							MoveTemp(Entries),
							FUeAgentPageInfo(
								Count,
								PagedSource.TotalItems,
								bPageHasMore,
								NextCursor),
							MoveTemp(Omissions));
					if (!OutSnapshot->IsValid())
					{
						return Fail(
							OutError,
							TEXT("projection_payload_invalid"),
							Snapshot.GetSnapshotId(),
							TEXT("projected snapshot is invalid"));
					}
					const FUeAgentProjectedPayload Payload(
						OutSnapshot.ToSharedRef());
					return TryEstimate(Payload, OutEstimate);
				};

			auto TryFit =
				[&](
					TSharedPtr<const FUeAgentProjectedSnapshot>& OutSnapshot,
					FUeAgentTokenEstimate& OutEstimate) -> bool
				{
					if (!TryBuild(
						MaximumCount,
						OutSnapshot,
						OutEstimate))
					{
						return false;
					}
					if (!Request.GetTokenBudget().IsSet()
						|| OutEstimate.GetTokens()
							<= Request.GetTokenBudget().GetValue())
					{
						return true;
					}
					if (MaximumCount == 0)
					{
						return Fail(
							OutError,
							TEXT("projection_token_budget_exceeded"),
							RequestId,
							TEXT("required response metadata exceeds token budget"));
					}
					TSharedPtr<const FUeAgentProjectedSnapshot> Best;
					FUeAgentTokenEstimate BestEstimate;
					int32 Low = 1;
					int32 High = MaximumCount - 1;
					while (Low <= High)
					{
						const int32 Count = Low + (High - Low) / 2;
						TSharedPtr<const FUeAgentProjectedSnapshot>
							Candidate;
						FUeAgentTokenEstimate CandidateEstimate;
						if (!TryBuild(
							Count,
							Candidate,
							CandidateEstimate))
						{
							return false;
						}
						if (CandidateEstimate.GetTokens()
							<= Request.GetTokenBudget().GetValue())
						{
							Best = MoveTemp(Candidate);
							BestEstimate = CandidateEstimate;
							Low = Count + 1;
						}
						else
						{
							High = Count - 1;
						}
					}
					if (!Best.IsValid())
					{
						return Fail(
							OutError,
							TEXT("projection_token_budget_exceeded"),
							RequestId,
							TEXT("one complete projected entry exceeds token budget"));
					}
					OutSnapshot = MoveTemp(Best);
					OutEstimate = BestEstimate;
					return true;
				};

			if (!TryFit(ProjectedSnapshot, FinalEstimate))
			{
				return false;
			}
			bHasMore = ProjectedSnapshot->GetPage().HasMore();
			FinalPayload.Emplace(ProjectedSnapshot.ToSharedRef());
		}
		else if (Source.GetCatalog().IsValid())
		{
			const FUeAgentCatalogSnapshot& Catalog = *Source.GetCatalog();
			const FCursorSource PagedSource = CursorSource(Catalog);
			int32 Start = 0;
			if (!TryPageStart(
				Request.GetPage(),
				DescriptorIdentity,
				ResultKind,
				Request.GetProfile(),
				PagedSource,
				Resources,
				Start,
				OutError))
			{
				return false;
			}
			int32 MaximumCount = PagedSource.TotalItems - Start;
			if (Request.GetPage().HasMaxItems())
			{
				MaximumCount = FMath::Min(
					MaximumCount,
					Request.GetPage().GetMaxItems());
			}

			auto TryBuild =
				[&](
					const int32 Count,
					TSharedPtr<const FUeAgentProjectedCatalog>& OutCatalog,
					FUeAgentTokenEstimate& OutEstimate) -> bool
				{
					if (!Checkpoint(
						TEXT("projection_catalog"),
						Deadline,
						Cancellation,
						OutError)
						|| !ChargePayload(Resources, Count, OutError))
					{
						return false;
					}
					TArray<FUeAgentCandidateSummary> Candidates;
					Candidates.Reserve(Count);
					for (int32 Offset = 0; Offset < Count; ++Offset)
					{
						Candidates.Add(
							Catalog.GetCandidates()[Start + Offset]);
					}
					const int32 End = Start + Count;
					const bool bPageHasMore =
						End < PagedSource.TotalItems;
					const FString NextCursor = bPageHasMore
						? EncodeCursor(
							DescriptorIdentity,
							ResultKind,
							Request.GetProfile(),
							PagedSource,
							End)
						: FString();
					OutCatalog = MakeShared<FUeAgentProjectedCatalog>(
						Catalog.GetCatalogId(),
						Catalog.GetCatalogKind(),
						Catalog.GetRevision(),
						Catalog.GetCompleteness(),
						MoveTemp(Candidates),
						Catalog.GetDiagnostics(),
						FUeAgentPageInfo(
							Count,
							PagedSource.TotalItems,
							bPageHasMore,
							NextCursor));
					if (!OutCatalog->IsValid())
					{
						return Fail(
							OutError,
							TEXT("projection_payload_invalid"),
							Catalog.GetCatalogId(),
							TEXT("projected catalog is invalid"));
					}
					const FUeAgentProjectedPayload Payload(
						OutCatalog.ToSharedRef());
					return TryEstimate(Payload, OutEstimate);
				};

			auto TryFit =
				[&](
					TSharedPtr<const FUeAgentProjectedCatalog>& OutCatalog,
					FUeAgentTokenEstimate& OutEstimate) -> bool
				{
					if (!TryBuild(
						MaximumCount,
						OutCatalog,
						OutEstimate))
					{
						return false;
					}
					if (!Request.GetTokenBudget().IsSet()
						|| OutEstimate.GetTokens()
							<= Request.GetTokenBudget().GetValue())
					{
						return true;
					}
					if (MaximumCount == 0)
					{
						return Fail(
							OutError,
							TEXT("projection_token_budget_exceeded"),
							RequestId,
							TEXT("required response metadata exceeds token budget"));
					}
					TSharedPtr<const FUeAgentProjectedCatalog> Best;
					FUeAgentTokenEstimate BestEstimate;
					int32 Low = 1;
					int32 High = MaximumCount - 1;
					while (Low <= High)
					{
						const int32 Count = Low + (High - Low) / 2;
						TSharedPtr<const FUeAgentProjectedCatalog>
							Candidate;
						FUeAgentTokenEstimate CandidateEstimate;
						if (!TryBuild(
							Count,
							Candidate,
							CandidateEstimate))
						{
							return false;
						}
						if (CandidateEstimate.GetTokens()
							<= Request.GetTokenBudget().GetValue())
						{
							Best = MoveTemp(Candidate);
							BestEstimate = CandidateEstimate;
							Low = Count + 1;
						}
						else
						{
							High = Count - 1;
						}
					}
					if (!Best.IsValid())
					{
						return Fail(
							OutError,
							TEXT("projection_token_budget_exceeded"),
							RequestId,
							TEXT("one complete projected candidate exceeds token budget"));
					}
					OutCatalog = MoveTemp(Best);
					OutEstimate = BestEstimate;
					return true;
				};

			if (!TryFit(ProjectedCatalog, FinalEstimate))
			{
				return false;
			}
			bHasMore = ProjectedCatalog->GetPage().HasMore();
			FinalPayload.Emplace(ProjectedCatalog.ToSharedRef());
		}
		else
		{
			if (!Request.GetPage().GetCursor().IsEmpty())
			{
				return Fail(
					OutError,
					TEXT("projection_cursor_invalid"),
					RequestId,
					TEXT("cursor is only valid for paged payloads"));
			}
			if (!Checkpoint(
				TEXT("projection_payload"),
				Deadline,
				Cancellation,
				OutError)
				|| !ChargePayload(Resources, 1, OutError))
			{
				return false;
			}
			if (Source.GetCandidateBinding().IsValid())
			{
				FinalPayload.Emplace(
					Source.GetCandidateBinding().ToSharedRef());
			}
			else if (Source.GetValue().IsValid())
			{
				FinalPayload.Emplace(Source.GetValue().ToSharedRef());
			}
			else if (Source.GetComparison().IsValid())
			{
				FinalPayload.Emplace(
					Source.GetComparison().ToSharedRef());
			}
			else
			{
				return Fail(
					OutError,
					TEXT("projection_value_type_unsupported"),
					RequestId,
					TEXT("typed payload is unsupported"));
			}
			if (!TryEstimate(FinalPayload.GetValue(), FinalEstimate))
			{
				return false;
			}
			if (Request.GetTokenBudget().IsSet()
				&& FinalEstimate.GetTokens()
					> Request.GetTokenBudget().GetValue())
			{
				return Fail(
					OutError,
					TEXT("projection_token_budget_exceeded"),
					RequestId,
					TEXT("complete typed value exceeds token budget"));
			}
		}

		if (!Checkpoint(
			TEXT("projection_output_accounting"),
			Deadline,
			Cancellation,
			OutError)
			|| !Charge(
				Resources,
				{
					{
						EUeAgentResourceDimension::OutputBytes,
						FinalEstimate.GetUtf8Bytes(),
					},
					{EUeAgentResourceDimension::WorkUnits, 1},
				},
				TEXT("/projection/output"),
				OutError))
		{
			return false;
		}

		OutResponse = MakeShareable(
			new FUeAgentProjectedResponse(
				DescriptorIdentity,
				RequestId,
				ResultKind,
				Request.GetProfile().GetProfileId(),
				bHasMore
					? EUeAgentProjectionCompleteness::PartialPage
					: EUeAgentProjectionCompleteness::Complete,
				MoveTemp(FinalPayload.GetValue()),
				Request.GetArtifacts(),
				Request.GetDiagnostics(),
				FinalEstimate));
		if (!OutResponse->IsValid())
		{
			OutResponse.Reset();
			return Fail(
				OutError,
				TEXT("projection_response_invalid"),
				RequestId,
				TEXT("projected response is invalid"));
		}
		return true;
	}
}
