#include "Core/Discovery/UeAgentDiscovery.h"

#include "Algo/Sort.h"

namespace UE::UAI::Discovery
{
	namespace
	{
		using namespace UE::UAI::ContextResource;

		void ResetError(FUeAgentDiscoveryError& OutError)
		{
			OutError = FUeAgentDiscoveryError{};
		}

		bool Fail(
			FUeAgentDiscoveryError& OutError,
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
			return Value.TrimStartAndEnd();
		}

		FString NormalizeOptional(FString Value)
		{
			return Value.TrimStartAndEnd();
		}

		bool IsRequiredTextValid(const FString& Value)
		{
			return !Value.IsEmpty();
		}

		bool IsCategoryPathValid(const TArray<FString>& CategoryPath)
		{
			for (const FString& Part : CategoryPath)
			{
				if (!IsRequiredTextValid(Part))
				{
					return false;
				}
			}
			return true;
		}

		int64 TextBytes(const FString& Text)
		{
			return MeasureText(Text).Utf8Bytes;
		}

		bool Charge(
			FUeAgentResourceTracker& Resources,
			const TMap<EUeAgentResourceDimension, int64>& Amounts,
			const FString& Path,
			FUeAgentDiscoveryError& OutError)
		{
			FUeAgentResourceBudgetExceeded BudgetError;
			if (Resources.TryChargeMany(Amounts, Path, BudgetError))
			{
				return true;
			}
			OutError.Code = BudgetError.Code;
			OutError.SubjectId = BudgetError.Path;
			OutError.Detail = TEXT("discovery resource budget exceeded");
			OutError.ResourceDimension = BudgetError.Dimension;
			OutError.ResourceLimit = BudgetError.Limit;
			OutError.ResourceObserved = BudgetError.Observed;
			return false;
		}

		bool Checkpoint(
			const FString& Stage,
			const FUeAgentOperationDeadline& Deadline,
			const FUeAgentCancellationToken& Cancellation,
			FUeAgentDiscoveryError& OutError)
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

		int64 CandidateStringBytes(
			const FUeAgentCandidateSummary& Candidate)
		{
			int64 Result =
				TextBytes(Candidate.GetIdentity().GetCanonicalKey())
				+ TextBytes(Candidate.GetDescriptorIdentity().GetDescriptorId())
				+ TextBytes(Candidate.GetDescriptorIdentity().GetDescriptorHash())
				+ TextBytes(Candidate.GetCatalogKind())
				+ TextBytes(Candidate.GetDisplayName())
				+ TextBytes(Candidate.GetSummary());
			for (const FString& Part : Candidate.GetCategoryPath())
			{
				Result += TextBytes(Part);
			}
			return Result;
		}

		int64 DiagnosticStringBytes(
			const FUeAgentCatalogDiagnostic& Diagnostic)
		{
			return TextBytes(Diagnostic.GetSourceId())
				+ TextBytes(Diagnostic.GetCode())
				+ TextBytes(Diagnostic.GetMessage());
		}
	}

	FUeAgentCatalogRevision::FUeAgentCatalogRevision(
		FString InSourceId,
		FString InGeneration,
		int64 InSequence)
		: SourceId(NormalizeRequired(MoveTemp(InSourceId)))
		, Generation(NormalizeRequired(MoveTemp(InGeneration)))
		, Sequence(InSequence)
		, bValid(
			IsRequiredTextValid(SourceId)
			&& IsRequiredTextValid(Generation)
			&& Sequence >= 0)
	{
	}

	const FString& FUeAgentCatalogRevision::GetSourceId() const
	{
		return SourceId;
	}

	const FString& FUeAgentCatalogRevision::GetGeneration() const
	{
		return Generation;
	}

	int64 FUeAgentCatalogRevision::GetSequence() const
	{
		return Sequence;
	}

	bool FUeAgentCatalogRevision::IsValid() const
	{
		return bValid;
	}

	bool FUeAgentCatalogRevision::operator==(
		const FUeAgentCatalogRevision& Other) const
	{
		return SourceId == Other.SourceId
			&& Generation == Other.Generation
			&& Sequence == Other.Sequence;
	}

	bool FUeAgentCatalogRevision::operator!=(
		const FUeAgentCatalogRevision& Other) const
	{
		return !(*this == Other);
	}

	FUeAgentCatalogDiagnostic::FUeAgentCatalogDiagnostic(
		FString InSourceId,
		FString InCode,
		FString InMessage)
		: SourceId(NormalizeRequired(MoveTemp(InSourceId)))
		, Code(NormalizeRequired(MoveTemp(InCode)))
		, Message(NormalizeRequired(MoveTemp(InMessage)))
		, bValid(
			IsRequiredTextValid(SourceId)
			&& IsRequiredTextValid(Code)
			&& IsRequiredTextValid(Message))
	{
	}

	const FString& FUeAgentCatalogDiagnostic::GetSourceId() const
	{
		return SourceId;
	}

	const FString& FUeAgentCatalogDiagnostic::GetCode() const
	{
		return Code;
	}

	const FString& FUeAgentCatalogDiagnostic::GetMessage() const
	{
		return Message;
	}

	bool FUeAgentCatalogDiagnostic::IsValid() const
	{
		return bValid;
	}

	bool FUeAgentCatalogDiagnostic::operator==(
		const FUeAgentCatalogDiagnostic& Other) const
	{
		return SourceId == Other.SourceId
			&& Code == Other.Code
			&& Message == Other.Message;
	}

	FUeAgentDiscoveryQuery::FUeAgentDiscoveryQuery(
		FString InCatalogKind,
		FString InSearchText,
		EUeAgentDiscoveryMatchMode InMatchMode,
		EUeAgentMaterializationPolicy InMaterializationPolicy)
		: CatalogKind(NormalizeRequired(MoveTemp(InCatalogKind)))
		, SearchText(NormalizeOptional(MoveTemp(InSearchText)))
		, MatchMode(InMatchMode)
		, MaterializationPolicy(InMaterializationPolicy)
		, bValid(
			IsRequiredTextValid(CatalogKind)
			&& (MatchMode != EUeAgentDiscoveryMatchMode::Exact
				|| IsRequiredTextValid(SearchText)))
	{
	}

	const FString& FUeAgentDiscoveryQuery::GetCatalogKind() const
	{
		return CatalogKind;
	}

	const FString& FUeAgentDiscoveryQuery::GetSearchText() const
	{
		return SearchText;
	}

	EUeAgentDiscoveryMatchMode FUeAgentDiscoveryQuery::GetMatchMode() const
	{
		return MatchMode;
	}

	EUeAgentMaterializationPolicy
	FUeAgentDiscoveryQuery::GetMaterializationPolicy() const
	{
		return MaterializationPolicy;
	}

	bool FUeAgentDiscoveryQuery::IsValid() const
	{
		return bValid;
	}

	FUeAgentCandidateSummary::FUeAgentCandidateSummary(
		FUeAgentCanonicalIdentity InIdentity,
		FUeAgentDescriptorIdentity InDescriptorIdentity,
		FString InCatalogKind,
		FString InDisplayName,
		FString InSummary,
		TArray<FString> InCategoryPath)
		: Identity(MoveTemp(InIdentity))
		, DescriptorIdentity(MoveTemp(InDescriptorIdentity))
		, CatalogKind(NormalizeRequired(MoveTemp(InCatalogKind)))
		, DisplayName(NormalizeRequired(MoveTemp(InDisplayName)))
		, Summary(NormalizeOptional(MoveTemp(InSummary)))
		, CategoryPath(MoveTemp(InCategoryPath))
	{
		for (FString& Part : CategoryPath)
		{
			Part = NormalizeRequired(MoveTemp(Part));
		}
		bValid = Identity.IsValid()
			&& DescriptorIdentity.IsValid()
			&& IsRequiredTextValid(CatalogKind)
			&& IsRequiredTextValid(DisplayName)
			&& IsCategoryPathValid(CategoryPath);
	}

	const FUeAgentCanonicalIdentity&
	FUeAgentCandidateSummary::GetIdentity() const
	{
		return Identity;
	}

	const FUeAgentDescriptorIdentity&
	FUeAgentCandidateSummary::GetDescriptorIdentity() const
	{
		return DescriptorIdentity;
	}

	const FString& FUeAgentCandidateSummary::GetCatalogKind() const
	{
		return CatalogKind;
	}

	const FString& FUeAgentCandidateSummary::GetDisplayName() const
	{
		return DisplayName;
	}

	const FString& FUeAgentCandidateSummary::GetSummary() const
	{
		return Summary;
	}

	const TArray<FString>& FUeAgentCandidateSummary::GetCategoryPath() const
	{
		return CategoryPath;
	}

	bool FUeAgentCandidateSummary::IsValid() const
	{
		return bValid;
	}

	bool FUeAgentCandidateSummary::operator==(
		const FUeAgentCandidateSummary& Other) const
	{
		return Identity == Other.Identity
			&& DescriptorIdentity == Other.DescriptorIdentity
			&& CatalogKind == Other.CatalogKind
			&& DisplayName == Other.DisplayName
			&& Summary == Other.Summary
			&& CategoryPath == Other.CategoryPath;
	}

	bool FUeAgentCandidateSummary::operator!=(
		const FUeAgentCandidateSummary& Other) const
	{
		return !(*this == Other);
	}

	FUeAgentCandidateBinding::FUeAgentCandidateBinding(
		FString InBindingId,
		FUeAgentCandidateSummary InSummary,
		FUeAgentCatalogRevision InCatalogRevision,
		FString InPublicRef,
		FString InReadPortId,
		TArray<EUeAgentCandidateOperation> InSupportedOperations,
		FString InBindingEvidence)
		: BindingId(NormalizeRequired(MoveTemp(InBindingId)))
		, Summary(MoveTemp(InSummary))
		, CatalogRevision(MoveTemp(InCatalogRevision))
		, PublicRef(NormalizeRequired(MoveTemp(InPublicRef)))
		, ReadPortId(NormalizeRequired(MoveTemp(InReadPortId)))
		, SupportedOperations(MoveTemp(InSupportedOperations))
		, BindingEvidence(NormalizeRequired(MoveTemp(InBindingEvidence)))
	{
		SupportedOperations.Sort(
			[](EUeAgentCandidateOperation Left, EUeAgentCandidateOperation Right)
			{
				return static_cast<uint8>(Left) < static_cast<uint8>(Right);
			});
		for (int32 Index = SupportedOperations.Num() - 1; Index > 0; --Index)
		{
			if (SupportedOperations[Index] == SupportedOperations[Index - 1])
			{
				SupportedOperations.RemoveAt(Index);
			}
		}
		bValid = IsRequiredTextValid(BindingId)
			&& Summary.IsValid()
			&& CatalogRevision.IsValid()
			&& IsRequiredTextValid(PublicRef)
			&& IsRequiredTextValid(ReadPortId)
			&& !SupportedOperations.IsEmpty()
			&& IsRequiredTextValid(BindingEvidence);
	}

	const FString& FUeAgentCandidateBinding::GetBindingId() const
	{
		return BindingId;
	}

	const FUeAgentCandidateSummary&
	FUeAgentCandidateBinding::GetSummary() const
	{
		return Summary;
	}

	const FUeAgentCatalogRevision&
	FUeAgentCandidateBinding::GetCatalogRevision() const
	{
		return CatalogRevision;
	}

	const FString& FUeAgentCandidateBinding::GetPublicRef() const
	{
		return PublicRef;
	}

	const FString& FUeAgentCandidateBinding::GetReadPortId() const
	{
		return ReadPortId;
	}

	const TArray<EUeAgentCandidateOperation>&
	FUeAgentCandidateBinding::GetSupportedOperations() const
	{
		return SupportedOperations;
	}

	const FString& FUeAgentCandidateBinding::GetBindingEvidence() const
	{
		return BindingEvidence;
	}

	bool FUeAgentCandidateBinding::Supports(
		EUeAgentCandidateOperation Operation) const
	{
		return SupportedOperations.Contains(Operation);
	}

	bool FUeAgentCandidateBinding::IsValid() const
	{
		return bValid;
	}

	bool FUeAgentCandidateBinding::operator==(
		const FUeAgentCandidateBinding& Other) const
	{
		return BindingId == Other.BindingId
			&& Summary == Other.Summary
			&& CatalogRevision == Other.CatalogRevision
			&& PublicRef == Other.PublicRef
			&& ReadPortId == Other.ReadPortId
			&& SupportedOperations == Other.SupportedOperations
			&& BindingEvidence == Other.BindingEvidence;
	}

	bool FUeAgentCatalogSnapshot::TryCreate(
		const FString& InCatalogId,
		const FString& InCatalogKind,
		const FUeAgentCatalogRevision& InRevision,
		EUeAgentCatalogCompleteness InCompleteness,
		const TArray<FUeAgentCandidateSummary>& InCandidates,
		const TArray<FUeAgentCatalogDiagnostic>& InDiagnostics,
		FUeAgentResourceTracker& Resources,
		TSharedPtr<const FUeAgentCatalogSnapshot>& OutCatalog,
		FUeAgentDiscoveryError& OutError)
	{
		ResetError(OutError);
		OutCatalog.Reset();
		const FString CatalogId = NormalizeRequired(InCatalogId);
		const FString CatalogKind = NormalizeRequired(InCatalogKind);
		if (!IsRequiredTextValid(CatalogId)
			|| !IsRequiredTextValid(CatalogKind)
			|| !InRevision.IsValid())
		{
			return Fail(
				OutError,
				TEXT("catalog_snapshot_invalid"),
				CatalogId,
				TEXT("catalog identity or revision is invalid"));
		}

		TArray<FUeAgentCandidateSummary> Candidates = InCandidates;
		TArray<FUeAgentCatalogDiagnostic> Diagnostics = InDiagnostics;
		int64 StringBytes = TextBytes(CatalogId)
			+ TextBytes(CatalogKind)
			+ TextBytes(InRevision.GetSourceId())
			+ TextBytes(InRevision.GetGeneration());
		for (const FUeAgentCandidateSummary& Candidate : Candidates)
		{
			if (!Candidate.IsValid())
			{
				return Fail(
					OutError,
					TEXT("catalog_candidate_invalid"),
					Candidate.GetIdentity().GetCanonicalKey());
			}
			if (Candidate.GetCatalogKind() != CatalogKind)
			{
				return Fail(
					OutError,
					TEXT("catalog_candidate_kind_mismatch"),
					Candidate.GetIdentity().GetCanonicalKey());
			}
			StringBytes += CandidateStringBytes(Candidate);
		}
		for (const FUeAgentCatalogDiagnostic& Diagnostic : Diagnostics)
		{
			if (!Diagnostic.IsValid())
			{
				return Fail(
					OutError,
					TEXT("catalog_diagnostic_invalid"),
					Diagnostic.GetSourceId());
			}
			StringBytes += DiagnosticStringBytes(Diagnostic);
		}

		if (InCompleteness != EUeAgentCatalogCompleteness::Complete
			&& Diagnostics.IsEmpty())
		{
			return Fail(
				OutError,
				TEXT("partial_catalog_requires_diagnostic"),
				CatalogId);
		}

		if (!Charge(
			Resources,
			{
				{EUeAgentResourceDimension::Nodes, 1},
				{EUeAgentResourceDimension::Items,
					static_cast<int64>(Candidates.Num() + Diagnostics.Num())},
				{EUeAgentResourceDimension::StringBytes, StringBytes},
				{EUeAgentResourceDimension::WorkUnits,
					static_cast<int64>(1 + Candidates.Num() + Diagnostics.Num())},
				{EUeAgentResourceDimension::EstimatedMemoryBytes,
					StringBytes
						+ static_cast<int64>(Candidates.Num())
							* static_cast<int64>(sizeof(FUeAgentCandidateSummary))
						+ static_cast<int64>(Diagnostics.Num())
							* static_cast<int64>(sizeof(FUeAgentCatalogDiagnostic))},
			},
			TEXT("/discovery/catalog"),
			OutError))
		{
			return false;
		}

		Candidates.Sort(
			[](const FUeAgentCandidateSummary& Left,
				const FUeAgentCandidateSummary& Right)
			{
				return Left.GetIdentity().GetCanonicalKey().Compare(
					Right.GetIdentity().GetCanonicalKey(),
					ESearchCase::CaseSensitive) < 0;
			});
		for (int32 Index = 1; Index < Candidates.Num(); ++Index)
		{
			if (Candidates[Index - 1].GetIdentity().GetCanonicalKey()
				== Candidates[Index].GetIdentity().GetCanonicalKey())
			{
				return Fail(
					OutError,
					TEXT("catalog_candidate_identity_duplicate"),
					Candidates[Index].GetIdentity().GetCanonicalKey());
			}
		}

		Diagnostics.Sort(
			[](const FUeAgentCatalogDiagnostic& Left,
				const FUeAgentCatalogDiagnostic& Right)
			{
				const int32 SourceOrder = Left.GetSourceId().Compare(
					Right.GetSourceId(),
					ESearchCase::CaseSensitive);
				if (SourceOrder != 0)
				{
					return SourceOrder < 0;
				}
				const int32 CodeOrder = Left.GetCode().Compare(
					Right.GetCode(),
					ESearchCase::CaseSensitive);
				if (CodeOrder != 0)
				{
					return CodeOrder < 0;
				}
				return Left.GetMessage().Compare(
					Right.GetMessage(),
					ESearchCase::CaseSensitive) < 0;
			});

		OutCatalog = MakeShareable(new FUeAgentCatalogSnapshot(
			CatalogId,
			CatalogKind,
			InRevision,
			InCompleteness,
			MoveTemp(Candidates),
			MoveTemp(Diagnostics)));
		return OutCatalog.IsValid();
	}

	FUeAgentCatalogSnapshot::FUeAgentCatalogSnapshot(
		FString InCatalogId,
		FString InCatalogKind,
		FUeAgentCatalogRevision InRevision,
		EUeAgentCatalogCompleteness InCompleteness,
		TArray<FUeAgentCandidateSummary> InCandidates,
		TArray<FUeAgentCatalogDiagnostic> InDiagnostics)
		: CatalogId(MoveTemp(InCatalogId))
		, CatalogKind(MoveTemp(InCatalogKind))
		, Revision(MoveTemp(InRevision))
		, Completeness(InCompleteness)
		, Candidates(MoveTemp(InCandidates))
		, Diagnostics(MoveTemp(InDiagnostics))
		, bValid(true)
	{
	}

	const FString& FUeAgentCatalogSnapshot::GetCatalogId() const
	{
		return CatalogId;
	}

	const FString& FUeAgentCatalogSnapshot::GetCatalogKind() const
	{
		return CatalogKind;
	}

	const FUeAgentCatalogRevision& FUeAgentCatalogSnapshot::GetRevision() const
	{
		return Revision;
	}

	EUeAgentCatalogCompleteness
	FUeAgentCatalogSnapshot::GetCompleteness() const
	{
		return Completeness;
	}

	const TArray<FUeAgentCandidateSummary>&
	FUeAgentCatalogSnapshot::GetCandidates() const
	{
		return Candidates;
	}

	const TArray<FUeAgentCatalogDiagnostic>&
	FUeAgentCatalogSnapshot::GetDiagnostics() const
	{
		return Diagnostics;
	}

	bool FUeAgentCatalogSnapshot::IsValid() const
	{
		return bValid;
	}

	FUeAgentStateReadRequest::FUeAgentStateReadRequest(
		FUeAgentCandidateBinding InBinding,
		EUeAgentStatePlane InStatePlane)
		: Binding(MoveTemp(InBinding))
		, StatePlane(InStatePlane)
		, bValid(Binding.IsValid())
	{
	}

	const FUeAgentCandidateBinding&
	FUeAgentStateReadRequest::GetBinding() const
	{
		return Binding;
	}

	EUeAgentStatePlane FUeAgentStateReadRequest::GetStatePlane() const
	{
		return StatePlane;
	}

	bool FUeAgentStateReadRequest::IsValid() const
	{
		return bValid;
	}

	bool FUeAgentDiscoveryService::TryCreate(
		const TArray<TSharedRef<IUeAgentCatalogSource>>& CatalogSources,
		const TArray<TSharedRef<IUeAgentStateReadPort>>& InReadPorts,
		TSharedPtr<const FUeAgentDiscoveryService>& OutService,
		FUeAgentDiscoveryError& OutError)
	{
		ResetError(OutError);
		OutService.Reset();
		TSet<FString> SourceIds;
		TMap<FString, TSharedPtr<IUeAgentCatalogSource>> SourcesByKind;
		for (const TSharedRef<IUeAgentCatalogSource>& Source : CatalogSources)
		{
			const FString SourceId = NormalizeRequired(Source->GetSourceId());
			if (!IsRequiredTextValid(SourceId))
			{
				return Fail(
					OutError,
					TEXT("catalog_source_id_invalid"));
			}
			if (SourceIds.Contains(SourceId))
			{
				return Fail(
					OutError,
					TEXT("catalog_source_id_duplicate"),
					SourceId);
			}
			SourceIds.Add(SourceId);
			if (Source->GetCatalogKinds().IsEmpty())
			{
				return Fail(
					OutError,
					TEXT("catalog_source_kind_missing"),
					SourceId);
			}
			TSet<FString> LocalKinds;
			for (const FString& RawKind : Source->GetCatalogKinds())
			{
				const FString Kind = NormalizeRequired(RawKind);
				if (!IsRequiredTextValid(Kind))
				{
					return Fail(
						OutError,
						TEXT("catalog_source_kind_invalid"),
						SourceId);
				}
				if (LocalKinds.Contains(Kind) || SourcesByKind.Contains(Kind))
				{
					return Fail(
						OutError,
						TEXT("catalog_kind_source_conflict"),
						Kind);
				}
				LocalKinds.Add(Kind);
				SourcesByKind.Add(Kind, Source);
			}
		}

		TMap<FString, TSharedPtr<IUeAgentStateReadPort>> ReadPorts;
		for (const TSharedRef<IUeAgentStateReadPort>& ReadPort : InReadPorts)
		{
			const FString PortId = NormalizeRequired(ReadPort->GetPortId());
			if (!IsRequiredTextValid(PortId))
			{
				return Fail(
					OutError,
					TEXT("state_read_port_id_invalid"));
			}
			if (ReadPorts.Contains(PortId))
			{
				return Fail(
					OutError,
					TEXT("state_read_port_id_duplicate"),
					PortId);
			}
			ReadPorts.Add(PortId, ReadPort);
		}

		OutService = MakeShareable(new FUeAgentDiscoveryService(
			MoveTemp(SourcesByKind),
			MoveTemp(ReadPorts)));
		return OutService.IsValid();
	}

	FUeAgentDiscoveryService::FUeAgentDiscoveryService(
		TMap<FString, TSharedPtr<IUeAgentCatalogSource>> InSourcesByKind,
		TMap<FString, TSharedPtr<IUeAgentStateReadPort>> InReadPorts)
		: SourcesByKind(MoveTemp(InSourcesByKind))
		, ReadPorts(MoveTemp(InReadPorts))
	{
	}

	bool FUeAgentDiscoveryService::TrySearch(
		const FUeAgentDiscoveryQuery& Query,
		FUeAgentResourceTracker& Resources,
		const FUeAgentOperationDeadline& Deadline,
		const FUeAgentCancellationToken& Cancellation,
		TSharedPtr<const FUeAgentCatalogSnapshot>& OutCatalog,
		FUeAgentDiscoveryError& OutError) const
	{
		ResetError(OutError);
		OutCatalog.Reset();
		if (!Query.IsValid())
		{
			return Fail(
				OutError,
				TEXT("discovery_query_invalid"),
				Query.GetCatalogKind());
		}
		if (!Checkpoint(
			TEXT("discovery.search"),
			Deadline,
			Cancellation,
			OutError))
		{
			return false;
		}
		const TSharedPtr<IUeAgentCatalogSource>* Source =
			SourcesByKind.Find(Query.GetCatalogKind());
		if (Source == nullptr || !Source->IsValid())
		{
			return Fail(
				OutError,
				TEXT("catalog_source_not_found"),
				Query.GetCatalogKind());
		}
		if (!(*Source)->TrySearch(
			Query,
			Resources,
			Deadline,
			Cancellation,
			OutCatalog,
			OutError))
		{
			if (OutError.Code.IsEmpty())
			{
				return Fail(
					OutError,
					TEXT("catalog_search_failed"),
					Query.GetCatalogKind());
			}
			return false;
		}
		if (!OutCatalog.IsValid() || !OutCatalog->IsValid())
		{
			OutCatalog.Reset();
			return Fail(
				OutError,
				TEXT("catalog_snapshot_invalid"),
				Query.GetCatalogKind());
		}
		if (OutCatalog->GetCatalogKind() != Query.GetCatalogKind())
		{
			OutCatalog.Reset();
			return Fail(
				OutError,
				TEXT("catalog_kind_mismatch"),
				Query.GetCatalogKind());
		}
		if (OutCatalog->GetRevision().GetSourceId()
			!= (*Source)->GetSourceId())
		{
			const FString RevisionSource =
				OutCatalog->GetRevision().GetSourceId();
			OutCatalog.Reset();
			return Fail(
				OutError,
				TEXT("catalog_revision_source_mismatch"),
				RevisionSource);
		}
		return true;
	}

	bool FUeAgentDiscoveryService::TryResolveExact(
		const FUeAgentCatalogSnapshot& Catalog,
		const FString& InCandidateId,
		FUeAgentResourceTracker& Resources,
		const FUeAgentOperationDeadline& Deadline,
		const FUeAgentCancellationToken& Cancellation,
		TSharedPtr<const FUeAgentCandidateBinding>& OutBinding,
		FUeAgentDiscoveryError& OutError) const
	{
		ResetError(OutError);
		OutBinding.Reset();
		if (!Catalog.IsValid())
		{
			return Fail(
				OutError,
				TEXT("catalog_snapshot_invalid"),
				Catalog.GetCatalogId());
		}
		const FString CandidateId = NormalizeRequired(InCandidateId);
		const FUeAgentCandidateSummary* ExpectedSummary = nullptr;
		for (const FUeAgentCandidateSummary& Candidate :
			Catalog.GetCandidates())
		{
			if (Candidate.GetIdentity().GetCanonicalKey() == CandidateId)
			{
				ExpectedSummary = &Candidate;
				break;
			}
		}
		if (ExpectedSummary == nullptr)
		{
			return Fail(
				OutError,
				TEXT("candidate_id_invalid"),
				CandidateId);
		}
		if (!Checkpoint(
			TEXT("discovery.resolve_exact"),
			Deadline,
			Cancellation,
			OutError))
		{
			return false;
		}
		const TSharedPtr<IUeAgentCatalogSource>* Source =
			SourcesByKind.Find(Catalog.GetCatalogKind());
		if (Source == nullptr || !Source->IsValid())
		{
			return Fail(
				OutError,
				TEXT("catalog_source_not_found"),
				Catalog.GetCatalogKind());
		}
		if (!(*Source)->TryResolveExact(
			CandidateId,
			Catalog.GetRevision(),
			Resources,
			Deadline,
			Cancellation,
			OutBinding,
			OutError))
		{
			if (OutError.Code.IsEmpty())
			{
				return Fail(
					OutError,
					TEXT("candidate_resolve_failed"),
					CandidateId);
			}
			return false;
		}
		if (!OutBinding.IsValid() || !OutBinding->IsValid())
		{
			OutBinding.Reset();
			return Fail(
				OutError,
				TEXT("candidate_binding_invalid"),
				CandidateId);
		}
		if (OutBinding->GetCatalogRevision() != Catalog.GetRevision())
		{
			OutBinding.Reset();
			return Fail(
				OutError,
				TEXT("catalog_revision_stale"),
				CandidateId);
		}
		if (OutBinding->GetSummary() != *ExpectedSummary)
		{
			OutBinding.Reset();
			return Fail(
				OutError,
				TEXT("candidate_binding_summary_mismatch"),
				CandidateId);
		}
		return true;
	}

	bool FUeAgentDiscoveryService::TryRead(
		const FUeAgentCandidateBinding& Binding,
		EUeAgentStatePlane StatePlane,
		FUeAgentResourceTracker& Resources,
		const FUeAgentOperationDeadline& Deadline,
		const FUeAgentCancellationToken& Cancellation,
		TSharedPtr<const FUeAgentCanonicalSnapshot>& OutSnapshot,
		FUeAgentDiscoveryError& OutError) const
	{
		ResetError(OutError);
		OutSnapshot.Reset();
		if (!Binding.IsValid())
		{
			return Fail(
				OutError,
				TEXT("candidate_binding_invalid"),
				Binding.GetBindingId());
		}
		if (!Binding.Supports(EUeAgentCandidateOperation::Read))
		{
			return Fail(
				OutError,
				TEXT("candidate_read_unsupported"),
				Binding.GetBindingId());
		}
		const TSharedPtr<IUeAgentStateReadPort>* ReadPort =
			ReadPorts.Find(Binding.GetReadPortId());
		if (ReadPort == nullptr || !ReadPort->IsValid())
		{
			return Fail(
				OutError,
				TEXT("state_read_port_not_found"),
				Binding.GetReadPortId());
		}
		if (!Checkpoint(
			TEXT("discovery.read"),
			Deadline,
			Cancellation,
			OutError))
		{
			return false;
		}
		const FUeAgentStateReadRequest Request(Binding, StatePlane);
		if (!(*ReadPort)->TryRead(
			Request,
			Resources,
			Deadline,
			Cancellation,
			OutSnapshot,
			OutError))
		{
			if (OutError.Code.IsEmpty())
			{
				return Fail(
					OutError,
					TEXT("state_read_failed"),
					Binding.GetReadPortId());
			}
			return false;
		}
		if (!OutSnapshot.IsValid())
		{
			return Fail(
				OutError,
				TEXT("state_read_snapshot_invalid"),
				Binding.GetReadPortId());
		}
		if (OutSnapshot->GetStatePlane() != StatePlane)
		{
			OutSnapshot.Reset();
			return Fail(
				OutError,
				TEXT("state_read_plane_mismatch"),
				Binding.GetReadPortId());
		}
		if (OutSnapshot->GetProvenance().GetSourceKind()
			!= TEXT("independent_readback"))
		{
			OutSnapshot.Reset();
			return Fail(
				OutError,
				TEXT("state_read_provenance_invalid"),
				Binding.GetReadPortId());
		}
		return true;
	}
}
