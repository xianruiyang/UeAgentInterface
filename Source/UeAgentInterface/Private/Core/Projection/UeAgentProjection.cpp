#include "Core/Projection/UeAgentProjection.h"

#include "Dom/JsonValue.h"
#include "Dom/JsonObject.h"

namespace UE::UAI::Projection
{
	namespace
	{
		using namespace UE::UAI::ContextResource;

		FUeAgentProjectionError ValidationFailure(const TCHAR* Code, const TCHAR* Reason, const FString& Path)
		{
			auto Error = FUeAgentProjectionError::Failure(Code, Reason, TEXT("projection"));
			Error.PropertyPath = Path;
			return Error;
		}

		const FUeAgentProjectionError& ValidationOf(const FUeAgentCandidateSummary& Item) { return *Item.GetValidationDiagnostic(); }
		const FUeAgentProjectionError& ValidationOf(const FUeAgentCatalogDiagnostic& Item) { return *Item.GetValidationDiagnostic(); }
		template <typename T> const FUeAgentProjectionError& ValidationOf(const T& Item) { return Item.GetValidationDiagnostic(); }

		template <typename T>
		bool ValidateItems(const TArray<T>& Items, const TCHAR* Field, FUeAgentProjectionError& OutError)
		{
			for (int32 Index = 0; Index < Items.Num(); ++Index)
			{
				if (Items[Index].IsValid()) continue;
				OutError = ValidationOf(Items[Index]);
				OutError.PropertyPath = FString::Printf(TEXT("%s[%d]"), Field, Index)
					+ (OutError.PropertyPath.IsEmpty() ? FString() : TEXT(".") + OutError.PropertyPath);
				return false;
			}
			return true;
		}

		void ResetError(FUeAgentProjectionError& OutError)
		{
			OutError = FUeAgentProjectionError{};
		}

		bool Fail(
			FUeAgentProjectionError& OutError,
			const FString& Code,
			const FString& SubjectId,
			const FString& PublicReason)
		{
			OutError = FUeAgentProjectionError::Failure(Code, PublicReason, TEXT("projection"), FString(), SubjectId);
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
			FUeAgentResourceChargeError ChargeError;
			if (Resources.TryChargeMany(Amounts, Path, ChargeError))
			{
				return true;
			}
			OutError = ChargeError;
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
				OutError = CancellationError;
				OutError.Stage = Stage;
				return false;
			}

			return true;
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
		if (ProfileId.IsEmpty())
			ValidationDiagnostic = ValidationFailure(TEXT("projection_profile_invalid"),
				TEXT("The projection profile identifier must not be empty."), TEXT("profile_id"));
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
		return ValidationDiagnostic.IsEmpty();
	}

	const FUeAgentProjectionError& FUeAgentProjectionProfile::GetValidationDiagnostic() const { return ValidationDiagnostic; }

	FUeAgentProjectionDiagnostic::FUeAgentProjectionDiagnostic(
		FUeAgentProjectionError InDiagnostic)
		: Diagnostic(MoveTemp(InDiagnostic))
	{
		if (Diagnostic.Code.TrimStartAndEnd().IsEmpty() || Diagnostic.PublicMessage.TrimStartAndEnd().IsEmpty())
		{
			const bool bCodeMissing = Diagnostic.Code.TrimStartAndEnd().IsEmpty();
			ValidationDiagnostic = ValidationFailure(TEXT("projection_diagnostic_invalid"),
				bCodeMissing ? TEXT("The projection diagnostic has no code.") : TEXT("The projection diagnostic has no public reason."),
				bCodeMissing ? TEXT("code") : TEXT("message"));
			if (!Diagnostic.IsEmpty()) ValidationDiagnostic.PublicCause = MakeShared<FUeAgentProjectionError>(Diagnostic);
		}
	}

	const FUeAgentProjectionError& FUeAgentProjectionDiagnostic::GetDiagnostic() const { return Diagnostic; }
	const FUeAgentProjectionError& FUeAgentProjectionDiagnostic::GetValidationDiagnostic() const { return ValidationDiagnostic; }

	bool FUeAgentProjectionDiagnostic::IsValid() const
	{
		return ValidationDiagnostic.IsEmpty();
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
	{
		auto Reject = [this](const TCHAR* Reason, const TCHAR* Path, const FString& Actual, bool bPublic)
		{
			ValidationDiagnostic = ValidationFailure(TEXT("projection_artifact_invalid"), Reason, Path);
			if (bPublic) ValidationDiagnostic.ActualValue = MakeShared<FJsonValueString>(Actual);
			else ValidationDiagnostic.ActualEvidence = MakeShared<FJsonValueString>(Actual);
		};
		if (ArtifactId.IsEmpty()) Reject(TEXT("The projected artifact has no identifier."), TEXT("artifact_id"), ArtifactId, false);
		else if (Kind.IsEmpty()) Reject(TEXT("The projected artifact has no kind."), TEXT("kind"), Kind, true);
		else if (Locator.IsEmpty()) Reject(TEXT("The projected artifact has no locator."), TEXT("locator"), Locator, false);
		else if (MediaType.IsEmpty()) Reject(TEXT("The projected artifact has no media type."), TEXT("media_type"), MediaType, true);
		else if (!IsSha256(ContentHash)) Reject(TEXT("The projected artifact content hash must be 'sha256:' followed by 64 hexadecimal digits."), TEXT("content_hash"), ContentHash, false);
		else if (ByteCount < 0)
		{
			ValidationDiagnostic = ValidationFailure(TEXT("projection_artifact_invalid"), TEXT("The projected artifact byte count must be non-negative."), TEXT("byte_count"));
			ValidationDiagnostic.ActualValue = MakeShared<FJsonValueNumber>(static_cast<double>(ByteCount));
		}
		else if (ReceiptId.IsEmpty()) Reject(TEXT("The projected artifact has no receipt identifier."), TEXT("receipt_id"), ReceiptId, false);
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
		return ValidationDiagnostic.IsEmpty();
	}

	const FUeAgentProjectionError& FUeAgentArtifactDescriptor::GetValidationDiagnostic() const { return ValidationDiagnostic; }

	FUeAgentTokenEstimate::FUeAgentTokenEstimate()
		: FUeAgentTokenEstimate(EUeAgentTokenEstimateKind::Exact, -1, -1)
	{
	}

	FUeAgentTokenEstimate::FUeAgentTokenEstimate(
		const EUeAgentTokenEstimateKind InKind,
		const int64 InTokens,
		const int64 InUtf8Bytes)
		: Kind(InKind)
		, Tokens(InTokens)
		, Utf8Bytes(InUtf8Bytes)
	{
		if (Tokens < 0 || Utf8Bytes < 0)
		{
			ValidationDiagnostic = ValidationFailure(TEXT("projection_token_estimate_invalid"),
				Tokens < 0 ? TEXT("The projection token estimate must be non-negative.") : TEXT("The projection UTF-8 byte estimate must be non-negative."),
				Tokens < 0 ? TEXT("tokens") : TEXT("utf8_bytes"));
			ValidationDiagnostic.ActualValue = MakeShared<FJsonValueNumber>(static_cast<double>(Tokens < 0 ? Tokens : Utf8Bytes));
		}
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
		return ValidationDiagnostic.IsEmpty();
	}

	const FUeAgentProjectionError& FUeAgentTokenEstimate::GetValidationDiagnostic() const { return ValidationDiagnostic; }

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
	{
		if (!Identity.IsValid()) ValidationDiagnostic = Identity.GetValidationDiagnostic();
		else if (!Type.IsValid()) ValidationDiagnostic = Type.GetValidationDiagnostic();
		else if (!ObservedRevision.IsValid()) ValidationDiagnostic = ObservedRevision.GetValidationDiagnostic();
		else if (Default.IsSet() && !Default->IsValid()) ValidationDiagnostic = Default->GetValidationDiagnostic();
		else if (Details.IsSet() && !Details->IsValid()) ValidationDiagnostic = Details->GetValidationDiagnostic();
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
		return ValidationDiagnostic.IsEmpty();
	}

	const FUeAgentProjectionError& FUeAgentProjectedEntry::GetValidationDiagnostic() const { return ValidationDiagnostic; }

	FUeAgentPageInfo::FUeAgentPageInfo(
		const int32 InReturnedItems,
		const int32 InTotalItems)
		: ReturnedItems(InReturnedItems)
		, TotalItems(InTotalItems)
	{
		if (ReturnedItems < 0 || TotalItems < 0 || ReturnedItems > TotalItems)
		{
			ValidationDiagnostic = ValidationFailure(TEXT("projection_page_invalid"), TEXT("Returned and total item counts must be non-negative, and returned items cannot exceed the total."), ReturnedItems < 0 || ReturnedItems > TotalItems ? TEXT("returned_items") : TEXT("total_items"));
			TSharedRef<FJsonObject> Counts = MakeShared<FJsonObject>();
			Counts->SetNumberField(TEXT("returned_items"), ReturnedItems);
			Counts->SetNumberField(TEXT("total_items"), TotalItems);
			ValidationDiagnostic.ActualValue = MakeShared<FJsonValueObject>(Counts);
		}
	}

	int32 FUeAgentPageInfo::GetReturnedItems() const
	{
		return ReturnedItems;
	}

	int32 FUeAgentPageInfo::GetTotalItems() const
	{
		return TotalItems;
	}

	bool FUeAgentPageInfo::IsValid() const
	{
		return ValidationDiagnostic.IsEmpty();
	}

	const FUeAgentProjectionError& FUeAgentPageInfo::GetValidationDiagnostic() const { return ValidationDiagnostic; }

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
	{
		if (SnapshotId.IsEmpty()) ValidationDiagnostic = ValidationFailure(TEXT("projection_payload_invalid"), TEXT("The projected snapshot identifier is empty."), TEXT("snapshot_id"));
		else if (!Revision.IsValid()) ValidationDiagnostic = Revision.GetValidationDiagnostic();
		else if (!CapturedAt.IsValid()) ValidationDiagnostic = CapturedAt.GetValidationDiagnostic();
		else if (Provenance.IsSet() && !Provenance->IsValid()) ValidationDiagnostic = Provenance->GetValidationDiagnostic();
		else if (!ValidateItems(Entries, TEXT("entries"), ValidationDiagnostic)) {}
		else if (!Page.IsValid()) ValidationDiagnostic = Page.GetValidationDiagnostic();
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
		return ValidationDiagnostic.IsEmpty();
	}

	const FUeAgentProjectionError& FUeAgentProjectedSnapshot::GetValidationDiagnostic() const { return ValidationDiagnostic; }

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
	{
		if (CatalogId.IsEmpty()) ValidationDiagnostic = ValidationFailure(TEXT("projection_payload_invalid"), TEXT("The projected catalog identifier is empty."), TEXT("catalog_id"));
		else if (CatalogKind.IsEmpty()) ValidationDiagnostic = ValidationFailure(TEXT("projection_payload_invalid"), TEXT("The projected catalog kind is empty."), TEXT("catalog_kind"));
		else if (!Revision.IsValid()) ValidationDiagnostic = *Revision.GetValidationDiagnostic();
		else if (!ValidateItems(Candidates, TEXT("candidates"), ValidationDiagnostic)) {}
		else if (!ValidateItems(Diagnostics, TEXT("diagnostics"), ValidationDiagnostic)) {}
		else if (!Page.IsValid()) ValidationDiagnostic = Page.GetValidationDiagnostic();
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
		return ValidationDiagnostic.IsEmpty();
	}

	const FUeAgentProjectionError& FUeAgentProjectedCatalog::GetValidationDiagnostic() const { return ValidationDiagnostic; }

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
		TArray<FUeAgentArtifactDescriptor> InArtifacts,
		TArray<FUeAgentProjectionDiagnostic> InDiagnostics,
		const EUeAgentProjectionPageUnit InPageUnit)
		: Profile(MoveTemp(InProfile))
		, Artifacts(MoveTemp(InArtifacts))
		, Diagnostics(MoveTemp(InDiagnostics))
		, PageUnit(InPageUnit)
	{
		if (!Profile.IsValid()) ValidationDiagnostic = Profile.GetValidationDiagnostic();
		else if (ValidateItems(Artifacts, TEXT("artifacts"), ValidationDiagnostic))
			ValidateItems(Diagnostics, TEXT("diagnostics"), ValidationDiagnostic);
	}

	const FUeAgentProjectionProfile&
	FUeAgentProjectionRequest::GetProfile() const
	{
		return Profile;
	}

	EUeAgentProjectionPageUnit FUeAgentProjectionRequest::GetPageUnit() const
	{
		return PageUnit;
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
		return ValidationDiagnostic.IsEmpty();
	}

	const FUeAgentProjectionError& FUeAgentProjectionRequest::GetValidationDiagnostic() const { return ValidationDiagnostic; }

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
		FUeAgentProjectedPayload InPayload,
		TArray<FUeAgentArtifactDescriptor> InArtifacts,
		TArray<FUeAgentProjectionDiagnostic> InDiagnostics,
		FUeAgentTokenEstimate InTokenEstimate)
		: DescriptorIdentity(MoveTemp(InDescriptorIdentity))
		, RequestId(NormalizeRequired(MoveTemp(InRequestId)))
		, ResultKind(InResultKind)
		, ProfileId(NormalizeRequired(MoveTemp(InProfileId)))
		, Payload(MoveTemp(InPayload))
		, Artifacts(MoveTemp(InArtifacts))
		, Diagnostics(MoveTemp(InDiagnostics))
		, TokenEstimate(MoveTemp(InTokenEstimate))
	{
		if (!DescriptorIdentity.IsValid()) ValidationDiagnostic = DescriptorIdentity.GetValidationDiagnostic();
		else if (RequestId.IsEmpty()) ValidationDiagnostic = ValidationFailure(TEXT("projection_response_invalid"), TEXT("The projected response has no request identifier."), TEXT("request_id"));
		else if (ProfileId.IsEmpty()) ValidationDiagnostic = ValidationFailure(TEXT("projection_response_invalid"), TEXT("The projected response has no profile identifier."), TEXT("profile_id"));
		else if (!Payload.IsValid())
		{
			ValidationDiagnostic = ValidationFailure(TEXT("projection_response_invalid"), TEXT("The projected response must contain exactly one payload variant."), TEXT("payload"));
			ValidationDiagnostic.ActualValue = MakeShared<FJsonValueNumber>(
				static_cast<int32>(Payload.GetSnapshot().IsValid()) + static_cast<int32>(Payload.GetCatalog().IsValid())
				+ static_cast<int32>(Payload.GetCandidateBinding().IsValid()) + static_cast<int32>(Payload.GetValue().IsValid())
				+ static_cast<int32>(Payload.GetComparison().IsValid()));
			ValidationDiagnostic.ExpectedValue = MakeShared<FJsonValueNumber>(1);
		}
		else if (ValidateItems(Artifacts, TEXT("artifacts"), ValidationDiagnostic)
			&& ValidateItems(Diagnostics, TEXT("diagnostics"), ValidationDiagnostic) && !TokenEstimate.IsValid())
			ValidationDiagnostic = TokenEstimate.GetValidationDiagnostic();
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
		return ValidationDiagnostic.IsEmpty();
	}

	const FUeAgentProjectionError& FUeAgentProjectedResponse::GetValidationDiagnostic() const { return ValidationDiagnostic; }

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
			if (!Result.GetDescriptorIdentity().IsValid()) OutError = Result.GetDescriptorIdentity().GetValidationDiagnostic();
			else if (Result.GetRequestId().IsEmpty()) OutError = ValidationFailure(TEXT("projection_result_invalid"), TEXT("The projection result request identifier is empty."), TEXT("request_id"));
			else OutError = ValidationFailure(TEXT("projection_result_invalid"), TEXT("The projection source must contain exactly one typed value."), TEXT("value"));
			return false;
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
			if (!Result.GetDescriptorIdentity().IsValid()) OutError = Result.GetDescriptorIdentity().GetValidationDiagnostic();
			else if (Result.GetRequestId().IsEmpty()) OutError = ValidationFailure(TEXT("projection_result_invalid"), TEXT("The projection result request identifier is empty."), TEXT("request_id"));
			else OutError = ValidationFailure(TEXT("projection_result_invalid"), TEXT("The projection source must contain exactly one typed value."), TEXT("value"));
			return false;
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
			OutError = (Request).GetValidationDiagnostic();
			return false;
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
					OutError = (OutEstimate).GetValidationDiagnostic();
					return false;
				}
				return true;
			};

		TSharedPtr<const FUeAgentProjectedSnapshot> ProjectedSnapshot;
		TSharedPtr<const FUeAgentProjectedCatalog> ProjectedCatalog;
		TOptional<FUeAgentProjectedPayload> FinalPayload;
		FUeAgentTokenEstimate FinalEstimate;

		if (Source.GetSnapshot().IsValid())
		{
			const FUeAgentCanonicalSnapshot& Snapshot =
				*Source.GetSnapshot();
			const EUeAgentProjectionPageUnit PageUnit =
				Request.GetPageUnit();
			struct FOwnerRecord final
			{
				FString OwnerId;
				TArray<int32> EntryIndices;
			};
			TArray<FOwnerRecord> OwnerRecords;
			if (PageUnit != EUeAgentProjectionPageUnit::Entry)
			{
				TMap<FString, int32> OwnerRecordIndices;
				for (int32 EntryIndex = 0;
					EntryIndex < Snapshot.GetEntries().Num();
					++EntryIndex)
				{
					const FString& OwnerId = Snapshot.GetEntries()[EntryIndex]
						.GetIdentity().GetOwnerId();
					int32* RecordIndex = OwnerRecordIndices.Find(OwnerId);
					if (!RecordIndex)
					{
						FOwnerRecord NewRecord;
						NewRecord.OwnerId = OwnerId;
						OwnerRecords.Add(MoveTemp(NewRecord));
						RecordIndex = &OwnerRecordIndices.Add(
							OwnerId,
							OwnerRecords.Num() - 1);
					}
					OwnerRecords[*RecordIndex].EntryIndices.Add(EntryIndex);
				}
				OwnerRecords.Sort(
					[](const FOwnerRecord& Left, const FOwnerRecord& Right)
					{
						return Left.OwnerId < Right.OwnerId;
					});
			}
			const int32 LogicalTotal = PageUnit ==
				EUeAgentProjectionPageUnit::Entry
				? Snapshot.GetEntries().Num()
				: OwnerRecords.Num();
			constexpr int32 Start = 0;
			const int32 MaximumCount = LogicalTotal;

			auto TryBuild =
				[&](
					const int32 Count,
					TSharedPtr<const FUeAgentProjectedSnapshot>& OutSnapshot,
					FUeAgentTokenEstimate& OutEstimate) -> bool
				{
					TArray<int32> SelectedEntryIndices;
					if (PageUnit == EUeAgentProjectionPageUnit::Entry)
					{
						SelectedEntryIndices.Reserve(Count);
						for (int32 Offset = 0; Offset < Count; ++Offset)
						{
							SelectedEntryIndices.Add(Start + Offset);
						}
					}
					else
					{
						for (int32 Group = Start;
							Group < Start + Count;
							++Group)
						{
							SelectedEntryIndices.Append(
								OwnerRecords[Group].EntryIndices);
						}
					}
					const int32 EntryCount = SelectedEntryIndices.Num();
					if (!Checkpoint(
						TEXT("projection_snapshot"),
						Deadline,
						Cancellation,
						OutError)
						|| !ChargePayload(
							Resources,
							EntryCount,
							OutError))
					{
						return false;
					}
					TArray<FUeAgentProjectedEntry> Entries;
					Entries.Reserve(EntryCount);
					for (int32 Offset = 0; Offset < EntryCount; ++Offset)
					{
						const int32 EntryIndex = SelectedEntryIndices[Offset];
						const FUeAgentStateEntry& Entry =
							Snapshot.GetEntries()[EntryIndex];
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
								LogicalTotal),
							MoveTemp(Omissions));
					if (!OutSnapshot->IsValid())
					{
						OutError = (*OutSnapshot).GetValidationDiagnostic();
						return false;
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
					return TryBuild(
						MaximumCount,
						OutSnapshot,
						OutEstimate);
				};

			if (!TryFit(ProjectedSnapshot, FinalEstimate))
			{
				return false;
			}
			FinalPayload.Emplace(ProjectedSnapshot.ToSharedRef());
		}
		else if (Source.GetCatalog().IsValid())
		{
			const FUeAgentCatalogSnapshot& Catalog = *Source.GetCatalog();
			constexpr int32 Start = 0;
			const int32 MaximumCount = Catalog.GetCandidates().Num();

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
					OutCatalog = MakeShared<FUeAgentProjectedCatalog>(
						Catalog.GetCatalogId(),
						Catalog.GetCatalogKind(),
						Catalog.GetRevision(),
						Catalog.GetCompleteness(),
						MoveTemp(Candidates),
						Catalog.GetDiagnostics(),
						FUeAgentPageInfo(
							Count,
							Catalog.GetCandidates().Num()));
					if (!OutCatalog->IsValid())
					{
						OutError = (*OutCatalog).GetValidationDiagnostic();
						return false;
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
					return TryBuild(
						MaximumCount,
						OutCatalog,
						OutEstimate);
				};

			if (!TryFit(ProjectedCatalog, FinalEstimate))
			{
				return false;
			}
			FinalPayload.Emplace(ProjectedCatalog.ToSharedRef());
		}
		else
		{
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
				MoveTemp(FinalPayload.GetValue()),
				Request.GetArtifacts(),
				Request.GetDiagnostics(),
				FinalEstimate));
		if (!OutResponse->IsValid())
		{
			OutError = OutResponse->GetValidationDiagnostic();
			OutResponse.Reset();
			return false;
		}
		return true;
	}
}
