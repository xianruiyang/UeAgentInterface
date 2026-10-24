#include "Core/Discovery/UeAgentDiscovery.h"

#include "Algo/Sort.h"
#include "Dom/JsonValue.h"

namespace UE::UAI::Discovery
{
	namespace
	{
		using namespace UE::UAI::ContextResource;

		FUeAgentDiscoveryError ValidationFailure(const FString& Code, const FString& Reason, const FString& Path)
		{
			auto Error = FUeAgentDiscoveryError::Failure(Code, Reason, TEXT("discovery"));
			Error.PropertyPath = Path;
			return Error;
		}

		TSharedPtr<const FUeAgentDiscoveryError> StoreValidationFailure(FUeAgentDiscoveryError Error, const FString& Prefix = FString())
		{
			if (!Prefix.IsEmpty()) Error.PropertyPath = Prefix + (Error.PropertyPath.IsEmpty() ? FString()
				: (Error.PropertyPath.StartsWith(TEXT("/")) ? Error.PropertyPath : TEXT(".") + Error.PropertyPath));
			return MakeShared<FUeAgentDiscoveryError>(MoveTemp(Error));
		}

		FUeAgentDiscoveryError TextDifference(const FString& Code, const FString& Reason, const FString& Path,
			const FString& Actual, const FString& Expected, const bool bPublic)
		{
			auto Error = ValidationFailure(Code, Reason, Path);
			if (bPublic)
			{
				Error.ActualValue = MakeShared<FJsonValueString>(Actual);
				Error.ExpectedValue = MakeShared<FJsonValueString>(Expected);
			}
			else
			{
				Error.ActualEvidence = MakeShared<FJsonValueString>(Actual);
				Error.ExpectedEvidence = MakeShared<FJsonValueString>(Expected);
			}
			return Error;
		}

		FUeAgentDiscoveryError CompareRevision(const FUeAgentCatalogRevision& Actual, const FUeAgentCatalogRevision& Expected,
			const FString& Code)
		{
			if (Actual.GetSourceId() != Expected.GetSourceId()) return TextDifference(Code,
				TEXT("The resolved catalog revision belongs to a different source."), TEXT("catalog_revision.source_id"),
				Actual.GetSourceId(), Expected.GetSourceId(), false);
			if (Actual.GetGeneration() != Expected.GetGeneration()) return TextDifference(Code,
				TEXT("The resolved catalog revision belongs to a different generation."), TEXT("catalog_revision.generation"),
				Actual.GetGeneration(), Expected.GetGeneration(), false);
			if (Actual.GetSequence() != Expected.GetSequence()) return TextDifference(Code,
				TEXT("The resolved catalog revision has a different sequence."), TEXT("catalog_revision.sequence"),
				FString::Printf(TEXT("%lld"), Actual.GetSequence()), FString::Printf(TEXT("%lld"), Expected.GetSequence()), false);
			return {};
		}

		FUeAgentDiscoveryError CompareIdentity(const FUeAgentCanonicalIdentity& Actual, const FUeAgentCanonicalIdentity& Expected,
			const FString& Code)
		{
			if (Actual == Expected) return {};
			struct FField { const TCHAR* Name; const FString* Actual; const FString* Expected; bool bPublic; };
			const FField Fields[] = {
				{TEXT("semantic_id"), &Actual.GetSemanticId(), &Expected.GetSemanticId(), false},
				{TEXT("target_kind"), &Actual.GetTargetKind(), &Expected.GetTargetKind(), true},
				{TEXT("owner_id"), &Actual.GetOwnerId(), &Expected.GetOwnerId(), false},
				{TEXT("object_role"), &Actual.GetObjectRole(), &Expected.GetObjectRole(), true},
				{TEXT("generation"), &Actual.GetGeneration(), &Expected.GetGeneration(), false},
			};
			for (const FField& Field : Fields)
			{
				if (*Field.Actual != *Field.Expected) return TextDifference(Code,
					FString::Printf(TEXT("The resolved candidate identity differs at '%s'."), Field.Name),
					TEXT("identity.") + FString(Field.Name), *Field.Actual, *Field.Expected, Field.bPublic);
			}
			TArray<FString> Keys;
			Actual.GetSelectors().GetKeys(Keys);
			for (const auto& Pair : Expected.GetSelectors()) Keys.AddUnique(Pair.Key);
			Keys.Sort();
			for (const FString& Key : Keys)
			{
				const FString* Left = Actual.GetSelectors().Find(Key);
				const FString* Right = Expected.GetSelectors().Find(Key);
				if (Left && Right && *Left == *Right) continue;
				auto Error = ValidationFailure(Code, TEXT("The resolved candidate has a different selector value or selector presence."),
					TEXT("identity.selectors[") + Key + TEXT("]"));
				Error.ActualValuePresent = Left != nullptr;
				Error.ExpectedValuePresent = Right != nullptr;
				if (Left) Error.ActualValue = MakeShared<FJsonValueString>(*Left);
				if (Right) Error.ExpectedValue = MakeShared<FJsonValueString>(*Right);
				return Error;
			}
			return TextDifference(Code, TEXT("The resolved candidate has a different canonical identity."), TEXT("identity"),
				Actual.GetCanonicalKey(), Expected.GetCanonicalKey(), false);
		}

		FUeAgentDiscoveryError CompareStringList(const TArray<FString>& Actual, const TArray<FString>& Expected,
			const FString& Code, const FString& Path)
		{
			const int32 Count = FMath::Max(Actual.Num(), Expected.Num());
			for (int32 Index = 0; Index < Count; ++Index)
			{
				const bool bActual = Actual.IsValidIndex(Index), bExpected = Expected.IsValidIndex(Index);
				if (bActual && bExpected && Actual[Index] == Expected[Index]) continue;
				auto Error = ValidationFailure(Code, TEXT("The resolved candidate summary differs at this list item."),
					Path + FString::Printf(TEXT("[%d]"), Index));
				Error.ActualValuePresent = bActual;
				Error.ExpectedValuePresent = bExpected;
				if (bActual) Error.ActualValue = MakeShared<FJsonValueString>(Actual[Index]);
				if (bExpected) Error.ExpectedValue = MakeShared<FJsonValueString>(Expected[Index]);
				return Error;
			}
			return {};
		}

		FUeAgentDiscoveryError CompareSummary(const FUeAgentCandidateSummary& Actual, const FUeAgentCandidateSummary& Expected)
		{
			const FString Code = TEXT("candidate_binding_summary_mismatch");
			auto Error = CompareIdentity(Actual.GetIdentity(), Expected.GetIdentity(), Code);
			if (!Error.IsEmpty()) return Error;
			const auto& ActualDescriptor = Actual.GetDescriptorIdentity();
			const auto& ExpectedDescriptor = Expected.GetDescriptorIdentity();
			if (ActualDescriptor.GetDescriptorId() != ExpectedDescriptor.GetDescriptorId()) return TextDifference(Code,
				TEXT("The resolved candidate uses a different descriptor."), TEXT("descriptor.descriptor_id"),
				ActualDescriptor.GetDescriptorId(), ExpectedDescriptor.GetDescriptorId(), false);
			if (ActualDescriptor.GetVersion() != ExpectedDescriptor.GetVersion()) return TextDifference(Code,
				TEXT("The resolved candidate uses a different descriptor version."), TEXT("descriptor.version"),
				FString::FromInt(ActualDescriptor.GetVersion()), FString::FromInt(ExpectedDescriptor.GetVersion()), false);
			if (ActualDescriptor.GetDescriptorHash() != ExpectedDescriptor.GetDescriptorHash()) return TextDifference(Code,
				TEXT("The resolved candidate uses a different descriptor contract."), TEXT("descriptor.descriptor_hash"),
				ActualDescriptor.GetDescriptorHash(), ExpectedDescriptor.GetDescriptorHash(), false);
			if (Actual.GetCatalogKind() != Expected.GetCatalogKind()) return TextDifference(Code,
				TEXT("The resolved candidate belongs to a different catalog kind."), TEXT("catalog_kind"), Actual.GetCatalogKind(), Expected.GetCatalogKind(), true);
			if (Actual.GetDisplayName() != Expected.GetDisplayName()) return TextDifference(Code,
				TEXT("The resolved candidate has a different display name."), TEXT("display_name"), Actual.GetDisplayName(), Expected.GetDisplayName(), true);
			if (Actual.GetSummary() != Expected.GetSummary()) return TextDifference(Code,
				TEXT("The resolved candidate has different summary text."), TEXT("summary"), Actual.GetSummary(), Expected.GetSummary(), true);
			Error = CompareStringList(Actual.GetCategoryPath(), Expected.GetCategoryPath(), Code, TEXT("category_path"));
			if (Error.IsEmpty()) Error = CompareStringList(Actual.GetDiscoveryScopes(), Expected.GetDiscoveryScopes(), Code, TEXT("discovery_scopes"));
			return Error;
		}

		void ResetError(FUeAgentDiscoveryError& OutError)
		{
			OutError = FUeAgentDiscoveryError{};
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

		FUeAgentDiscoveryError ValidateCategoryPath(const TArray<FString>& CategoryPath, const FString& Path)
		{
			for (int32 Index = 0; Index < CategoryPath.Num(); ++Index)
			{
				if (!IsRequiredTextValid(CategoryPath[Index]))
				{
					auto Error = ValidationFailure(TEXT("catalog_candidate_invalid"),
						TEXT("A candidate category or discovery scope must not be empty."), Path + FString::Printf(TEXT("[%d]"), Index));
					Error.ActualValue = MakeShared<FJsonValueString>(CategoryPath[Index]);
					return Error;
				}
			}
			return {};
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
			FUeAgentDiscoveryError& OutError)
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
		for (const FString& Scope : Candidate.GetDiscoveryScopes())
		{
			Result += TextBytes(Scope);
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
	{
		if (!IsRequiredTextValid(SourceId)) ValidationDiagnostic = StoreValidationFailure(ValidationFailure(
			TEXT("catalog_revision_invalid"), TEXT("The catalog revision has no source identifier."), TEXT("source_id")));
		else if (!IsRequiredTextValid(Generation)) ValidationDiagnostic = StoreValidationFailure(ValidationFailure(
			TEXT("catalog_revision_invalid"), TEXT("The catalog revision has no generation identifier."), TEXT("generation")));
		else if (Sequence < 0)
		{
			auto Error = ValidationFailure(TEXT("catalog_revision_invalid"), TEXT("The catalog revision sequence must be non-negative."), TEXT("sequence"));
			Error.ActualEvidence = MakeShared<FJsonValueString>(FString::Printf(TEXT("%lld"), Sequence));
			ValidationDiagnostic = StoreValidationFailure(MoveTemp(Error));
		}
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
		return !ValidationDiagnostic.IsValid();
	}

	const FUeAgentDiscoveryError* FUeAgentCatalogRevision::GetValidationDiagnostic() const
	{
		return ValidationDiagnostic.Get();
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
		FUeAgentDiscoveryError InDiagnostic)
		: SourceId(NormalizeRequired(MoveTemp(InSourceId)))
		, Diagnostic(MoveTemp(InDiagnostic))
	{
		if (SourceId.IsEmpty()) ValidationDiagnostic = StoreValidationFailure(ValidationFailure(
			TEXT("catalog_diagnostic_invalid"), TEXT("The catalog diagnostic has no source identifier."), TEXT("source_id")));
		else if (Diagnostic.Code.TrimStartAndEnd().IsEmpty()) ValidationDiagnostic = StoreValidationFailure(ValidationFailure(
			TEXT("catalog_diagnostic_invalid"), TEXT("The catalog diagnostic has no error code."), TEXT("diagnostic.code")));
		else if (Diagnostic.PublicMessage.TrimStartAndEnd().IsEmpty()) ValidationDiagnostic = StoreValidationFailure(ValidationFailure(
			TEXT("catalog_diagnostic_invalid"), TEXT("The catalog diagnostic has no public reason."), TEXT("diagnostic.message")));
		if (ValidationDiagnostic.IsValid() && (!Diagnostic.IsEmpty() || !Diagnostic.PublicMessage.IsEmpty()
			|| Diagnostic.PublicCause.IsValid() || !Diagnostic.Diagnostics.IsEmpty()))
		{
			auto Error = *ValidationDiagnostic;
			Error.PublicCause = MakeShared<FUeAgentDiscoveryError>(Diagnostic);
			ValidationDiagnostic = StoreValidationFailure(MoveTemp(Error));
		}
	}

	const FString& FUeAgentCatalogDiagnostic::GetSourceId() const
	{
		return SourceId;
	}

	const FString& FUeAgentCatalogDiagnostic::GetCode() const
	{
		return Diagnostic.Code;
	}

	const FString& FUeAgentCatalogDiagnostic::GetMessage() const
	{
		return Diagnostic.PublicMessage;
	}

	const FUeAgentDiscoveryError& FUeAgentCatalogDiagnostic::GetDiagnostic() const
	{
		return Diagnostic;
	}

	bool FUeAgentCatalogDiagnostic::IsValid() const
	{
		return !ValidationDiagnostic.IsValid();
	}

	const FUeAgentDiscoveryError* FUeAgentCatalogDiagnostic::GetValidationDiagnostic() const
	{
		return ValidationDiagnostic.Get();
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
	{
		if (!IsRequiredTextValid(CatalogKind)) ValidationDiagnostic = StoreValidationFailure(ValidationFailure(
			TEXT("discovery_query_invalid"), TEXT("The discovery query has no catalog kind."), TEXT("catalog_kind")));
		else if (MatchMode == EUeAgentDiscoveryMatchMode::Exact && !IsRequiredTextValid(SearchText))
			ValidationDiagnostic = StoreValidationFailure(ValidationFailure(TEXT("discovery_query_invalid"),
				TEXT("An exact discovery query requires non-empty search text."), TEXT("search_text")));
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
		return !ValidationDiagnostic.IsValid();
	}

	const FUeAgentDiscoveryError* FUeAgentDiscoveryQuery::GetValidationDiagnostic() const
	{
		return ValidationDiagnostic.Get();
	}

	FUeAgentCandidateSummary::FUeAgentCandidateSummary(
		FUeAgentCanonicalIdentity InIdentity,
		FUeAgentDescriptorIdentity InDescriptorIdentity,
		FString InCatalogKind,
		FString InDisplayName,
		FString InSummary,
		TArray<FString> InCategoryPath,
		TArray<FString> InDiscoveryScopes)
		: Identity(MoveTemp(InIdentity))
		, DescriptorIdentity(MoveTemp(InDescriptorIdentity))
		, CatalogKind(NormalizeRequired(MoveTemp(InCatalogKind)))
		, DisplayName(NormalizeRequired(MoveTemp(InDisplayName)))
		, Summary(NormalizeOptional(MoveTemp(InSummary)))
		, CategoryPath(MoveTemp(InCategoryPath))
		, DiscoveryScopes(MoveTemp(InDiscoveryScopes))
	{
		for (FString& Part : CategoryPath)
		{
			Part = NormalizeRequired(MoveTemp(Part));
		}
		for (FString& Scope : DiscoveryScopes)
		{
			Scope = NormalizeRequired(MoveTemp(Scope));
		}
		if (!Identity.IsValid()) ValidationDiagnostic = StoreValidationFailure(Identity.GetValidationDiagnostic(), TEXT("identity"));
		else if (!DescriptorIdentity.IsValid()) ValidationDiagnostic = StoreValidationFailure(DescriptorIdentity.GetValidationDiagnostic(), TEXT("descriptor"));
		else if (!IsRequiredTextValid(CatalogKind)) ValidationDiagnostic = StoreValidationFailure(ValidationFailure(
			TEXT("catalog_candidate_invalid"), TEXT("The candidate has no catalog kind."), TEXT("catalog_kind")));
		else if (!IsRequiredTextValid(DisplayName)) ValidationDiagnostic = StoreValidationFailure(ValidationFailure(
			TEXT("catalog_candidate_invalid"), TEXT("The candidate has no display name."), TEXT("display_name")));
		else
		{
			auto Error = ValidateCategoryPath(CategoryPath, TEXT("category_path"));
			if (Error.IsEmpty()) Error = ValidateCategoryPath(DiscoveryScopes, TEXT("discovery_scopes"));
			if (!Error.IsEmpty()) ValidationDiagnostic = StoreValidationFailure(MoveTemp(Error));
		}
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

	const TArray<FString>& FUeAgentCandidateSummary::GetDiscoveryScopes() const
	{
		return DiscoveryScopes;
	}

	bool FUeAgentCandidateSummary::IsValid() const
	{
		return !ValidationDiagnostic.IsValid();
	}

	const FUeAgentDiscoveryError* FUeAgentCandidateSummary::GetValidationDiagnostic() const
	{
		return ValidationDiagnostic.Get();
	}

	bool FUeAgentCandidateSummary::operator==(
		const FUeAgentCandidateSummary& Other) const
	{
		return Identity == Other.Identity
			&& DescriptorIdentity == Other.DescriptorIdentity
			&& CatalogKind == Other.CatalogKind
			&& DisplayName == Other.DisplayName
			&& Summary == Other.Summary
			&& CategoryPath == Other.CategoryPath
			&& DiscoveryScopes == Other.DiscoveryScopes;
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
		if (!IsRequiredTextValid(BindingId)) ValidationDiagnostic = StoreValidationFailure(ValidationFailure(
			TEXT("candidate_binding_invalid"), TEXT("The candidate binding has no binding identifier."), TEXT("binding_id")));
		else if (!Summary.IsValid()) ValidationDiagnostic = StoreValidationFailure(*Summary.GetValidationDiagnostic(), TEXT("summary"));
		else if (!CatalogRevision.IsValid()) ValidationDiagnostic = StoreValidationFailure(*CatalogRevision.GetValidationDiagnostic(), TEXT("catalog_revision"));
		else if (!IsRequiredTextValid(PublicRef)) ValidationDiagnostic = StoreValidationFailure(ValidationFailure(
			TEXT("candidate_binding_invalid"), TEXT("The candidate binding has no public target reference."), TEXT("public_ref")));
		else if (!IsRequiredTextValid(ReadPortId)) ValidationDiagnostic = StoreValidationFailure(ValidationFailure(
			TEXT("candidate_binding_invalid"), TEXT("The candidate binding has no state read port identifier."), TEXT("read_port_id")));
		else if (SupportedOperations.IsEmpty()) ValidationDiagnostic = StoreValidationFailure(ValidationFailure(
			TEXT("candidate_binding_invalid"), TEXT("The candidate binding declares no supported operations."), TEXT("supported_operations")));
		else if (!IsRequiredTextValid(BindingEvidence)) ValidationDiagnostic = StoreValidationFailure(ValidationFailure(
			TEXT("candidate_binding_invalid"), TEXT("The candidate binding has no binding evidence."), TEXT("binding_evidence")));
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
		return !ValidationDiagnostic.IsValid();
	}

	const FUeAgentDiscoveryError* FUeAgentCandidateBinding::GetValidationDiagnostic() const
	{
		return ValidationDiagnostic.Get();
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
			|| !IsRequiredTextValid(CatalogKind))
		{
			OutError = ValidationFailure(TEXT("catalog_snapshot_invalid"),
				CatalogId.IsEmpty() ? TEXT("The catalog snapshot has no catalog identifier.") : TEXT("The catalog snapshot has no catalog kind."),
				CatalogId.IsEmpty() ? TEXT("catalog_id") : TEXT("catalog_kind"));
			return false;
		}
		if (!InRevision.IsValid())
		{
			OutError = *StoreValidationFailure(*InRevision.GetValidationDiagnostic(), TEXT("revision"));
			return false;
		}

		TArray<FUeAgentCandidateSummary> Candidates = InCandidates;
		TArray<FUeAgentCatalogDiagnostic> Diagnostics = InDiagnostics;
		int64 StringBytes = TextBytes(CatalogId)
			+ TextBytes(CatalogKind)
			+ TextBytes(InRevision.GetSourceId())
			+ TextBytes(InRevision.GetGeneration());
		for (int32 CandidateIndex = 0; CandidateIndex < Candidates.Num(); ++CandidateIndex)
		{
			const FUeAgentCandidateSummary& Candidate = Candidates[CandidateIndex];
			if (!Candidate.IsValid())
			{
				OutError = *StoreValidationFailure(*Candidate.GetValidationDiagnostic(), FString::Printf(TEXT("candidates[%d]"), CandidateIndex));
				return false;
			}
			if (Candidate.GetCatalogKind() != CatalogKind)
			{
				OutError = TextDifference(TEXT("catalog_candidate_kind_mismatch"),
					TEXT("The candidate belongs to a different kind of catalog."),
					FString::Printf(TEXT("candidates[%d].catalog_kind"), CandidateIndex), Candidate.GetCatalogKind(), CatalogKind, true);
				OutError.CandidateName = Candidate.GetDisplayName();
				return false;
			}
			StringBytes += CandidateStringBytes(Candidate);
		}
		for (int32 DiagnosticIndex = 0; DiagnosticIndex < Diagnostics.Num(); ++DiagnosticIndex)
		{
			const FUeAgentCatalogDiagnostic& Diagnostic = Diagnostics[DiagnosticIndex];
			if (!Diagnostic.IsValid())
			{
				OutError = *StoreValidationFailure(*Diagnostic.GetValidationDiagnostic(), FString::Printf(TEXT("diagnostics[%d]"), DiagnosticIndex));
				return false;
			}
			StringBytes += DiagnosticStringBytes(Diagnostic);
		}

		if (InCompleteness != EUeAgentCatalogCompleteness::Complete
			&& Diagnostics.IsEmpty())
		{
			OutError = ValidationFailure(TEXT("partial_catalog_requires_diagnostic"),
				TEXT("A partial or degraded catalog must explain the missing coverage in its diagnostics."), TEXT("diagnostics"));
			OutError.SubjectId = CatalogId;
			return false;
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
				OutError = ValidationFailure(TEXT("catalog_candidate_identity_duplicate"),
					TEXT("The catalog contains the same candidate identity more than once."),
					FString::Printf(TEXT("candidates[%d].identity"), Index));
				OutError.CandidateName = Candidates[Index].GetDisplayName();
				OutError.ActualEvidence = MakeShared<FJsonValueString>(Candidates[Index].GetIdentity().GetCanonicalKey());
				return false;
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

	bool IUeAgentCatalogSource::TrySearchResolved(
		const FUeAgentDiscoveryQuery& Query,
		FUeAgentResourceTracker& Resources,
		const FUeAgentOperationDeadline& Deadline,
		const FUeAgentCancellationToken& Cancellation,
		TSharedPtr<const FUeAgentCatalogSnapshot>& OutCatalog,
		TArray<TSharedRef<const FUeAgentCandidateBinding>>& OutBindings,
		FUeAgentDiscoveryError& OutError)
	{
		OutCatalog.Reset();
		OutBindings.Reset();
		ResetError(OutError);
		const bool bSearched = TrySearch(
				Query,
				Resources,
				Deadline,
				Cancellation,
				OutCatalog,
				OutError);
		if (!bSearched || !OutCatalog.IsValid())
		{
			if (OutError.IsEmpty()) OutError = ValidationFailure(TEXT("catalog_search_failed"), bSearched
				? TEXT("The catalog source reported success without returning a catalog snapshot.")
				: TEXT("The catalog source search failed without reporting a diagnostic."), TEXT("catalog"));
			OutCatalog.Reset();
			OutBindings.Reset();
			return false;
		}
		for (const FUeAgentCandidateSummary& Summary
			: OutCatalog->GetCandidates())
		{
			TSharedPtr<const FUeAgentCandidateBinding> Binding;
			const bool bResolved = TryResolveExact(
					Summary.GetIdentity().GetCanonicalKey(),
					OutCatalog->GetRevision(),
					Resources,
					Deadline,
					Cancellation,
					Binding,
					OutError);
			if (!bResolved || !Binding.IsValid())
			{
				if (OutError.IsEmpty())
				{
					OutError = ValidationFailure(TEXT("candidate_resolve_failed"), bResolved
						? TEXT("The catalog source resolved the candidate without returning a binding.")
						: TEXT("The catalog source could not resolve the candidate and reported no diagnostic."), TEXT("binding"));
					OutError.CandidateName = Summary.GetDisplayName();
				}
				OutCatalog.Reset();
				OutBindings.Reset();
				return false;
			}
			if (Binding->GetCatalogRevision() != OutCatalog->GetRevision())
			{
				OutError = CompareRevision(Binding->GetCatalogRevision(), OutCatalog->GetRevision(), TEXT("resolved_search_revision_mismatch"));
				OutError.TargetPath = Binding->GetPublicRef();
				OutCatalog.Reset();
				OutBindings.Reset();
				return false;
			}
			if (Binding->GetSummary().GetIdentity().GetCanonicalKey()
				!= Summary.GetIdentity().GetCanonicalKey())
			{
				OutError = CompareIdentity(Binding->GetSummary().GetIdentity(), Summary.GetIdentity(), TEXT("resolved_search_identity_mismatch"));
				OutError.TargetPath = Binding->GetPublicRef();
				OutError.CandidateName = Summary.GetDisplayName();
				OutCatalog.Reset();
				OutBindings.Reset();
				return false;
			}
			OutBindings.Add(Binding.ToSharedRef());
		}
		return true;
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
		for (int32 SourceIndex = 0; SourceIndex < CatalogSources.Num(); ++SourceIndex)
		{
			const auto& Source = CatalogSources[SourceIndex];
			const FString SourcePath = FString::Printf(TEXT("catalog_sources[%d]"), SourceIndex);
			const FString SourceId = NormalizeRequired(Source->GetSourceId());
			if (!IsRequiredTextValid(SourceId))
			{
				OutError = ValidationFailure(TEXT("catalog_source_id_invalid"), TEXT("The catalog source has no source identifier."), SourcePath + TEXT(".source_id"));
				return false;
			}
			if (SourceIds.Contains(SourceId))
			{
				OutError = ValidationFailure(TEXT("catalog_source_id_duplicate"), TEXT("The same catalog source identifier is registered more than once."), SourcePath + TEXT(".source_id"));
				OutError.ActualEvidence = MakeShared<FJsonValueString>(SourceId);
				return false;
			}
			SourceIds.Add(SourceId);
			if (Source->GetCatalogKinds().IsEmpty())
			{
				OutError = ValidationFailure(TEXT("catalog_source_kind_missing"), TEXT("The catalog source declares no catalog kinds."), SourcePath + TEXT(".catalog_kinds"));
				OutError.SubjectId = SourceId;
				return false;
			}
			TSet<FString> LocalKinds;
			int32 KindIndex = 0;
			for (const FString& RawKind : Source->GetCatalogKinds())
			{
				const FString Kind = NormalizeRequired(RawKind);
				const FString KindPath = SourcePath + FString::Printf(TEXT(".catalog_kinds[%d]"), KindIndex++);
				if (!IsRequiredTextValid(Kind))
				{
					OutError = ValidationFailure(TEXT("catalog_source_kind_invalid"), TEXT("A declared catalog kind must not be empty."), KindPath);
					OutError.ActualValue = MakeShared<FJsonValueString>(RawKind);
					return false;
				}
				if (LocalKinds.Contains(Kind) || SourcesByKind.Contains(Kind))
				{
					OutError = ValidationFailure(TEXT("catalog_kind_source_conflict"), LocalKinds.Contains(Kind)
						? TEXT("The source declares the same catalog kind more than once.")
						: TEXT("More than one source is registered for this catalog kind."), KindPath);
					OutError.ActualValue = MakeShared<FJsonValueString>(Kind);
					OutError.SubjectId = SourceId;
					return false;
				}
				LocalKinds.Add(Kind);
				SourcesByKind.Add(Kind, Source);
			}
		}

		TMap<FString, TSharedPtr<IUeAgentStateReadPort>> ReadPorts;
		for (int32 PortIndex = 0; PortIndex < InReadPorts.Num(); ++PortIndex)
		{
			const auto& ReadPort = InReadPorts[PortIndex];
			const FString PortPath = FString::Printf(TEXT("read_ports[%d].port_id"), PortIndex);
			const FString PortId = NormalizeRequired(ReadPort->GetPortId());
			if (!IsRequiredTextValid(PortId))
			{
				OutError = ValidationFailure(TEXT("state_read_port_id_invalid"), TEXT("The state read port has no port identifier."), PortPath);
				return false;
			}
			if (ReadPorts.Contains(PortId))
			{
				OutError = ValidationFailure(TEXT("state_read_port_id_duplicate"), TEXT("The same state read port identifier is registered more than once."), PortPath);
				OutError.ActualEvidence = MakeShared<FJsonValueString>(PortId);
				return false;
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
			OutError = *Query.GetValidationDiagnostic();
			return false;
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
			OutError = ValidationFailure(TEXT("catalog_source_not_found"),
				TEXT("No available catalog source is registered for the requested catalog kind."), TEXT("catalog_kind"));
			OutError.ActualValue = MakeShared<FJsonValueString>(Query.GetCatalogKind());
			return false;
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
				OutError = ValidationFailure(TEXT("catalog_search_failed"),
					TEXT("The catalog source search failed without reporting a diagnostic."), TEXT("catalog_kind"));
				OutError.ActualValue = MakeShared<FJsonValueString>(Query.GetCatalogKind());
			}
			return false;
		}
		if (!OutCatalog.IsValid() || !OutCatalog->IsValid())
		{
			OutError = ValidationFailure(TEXT("catalog_snapshot_invalid"), OutCatalog.IsValid()
				? TEXT("The catalog source returned an invalid catalog snapshot.")
				: TEXT("The catalog source reported success without returning a catalog snapshot."), TEXT("catalog"));
			OutCatalog.Reset();
			return false;
		}
		if (OutCatalog->GetCatalogKind() != Query.GetCatalogKind())
		{
			OutError = TextDifference(TEXT("catalog_kind_mismatch"), TEXT("The returned catalog has a different kind from the query."),
				TEXT("catalog_kind"), OutCatalog->GetCatalogKind(), Query.GetCatalogKind(), true);
			OutCatalog.Reset();
			return false;
		}
		if (OutCatalog->GetRevision().GetSourceId()
			!= (*Source)->GetSourceId())
		{
			OutError = TextDifference(TEXT("catalog_revision_source_mismatch"), TEXT("The returned catalog revision identifies a different source from the source that handled the query."),
				TEXT("catalog_revision.source_id"), OutCatalog->GetRevision().GetSourceId(), (*Source)->GetSourceId(), false);
			OutCatalog.Reset();
			return false;
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
			OutError = ValidationFailure(TEXT("catalog_snapshot_invalid"), TEXT("Exact resolution requires a valid catalog snapshot."), TEXT("catalog"));
			OutError.SubjectId = Catalog.GetCatalogId();
			return false;
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
			OutError = ValidationFailure(TEXT("candidate_id_invalid"), TEXT("The requested candidate identifier is absent from the supplied catalog."), TEXT("candidate_id"));
			OutError.ActualEvidence = MakeShared<FJsonValueString>(CandidateId);
			return false;
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
			OutError = ValidationFailure(TEXT("catalog_source_not_found"),
				TEXT("No available catalog source is registered for the supplied catalog kind."), TEXT("catalog_kind"));
			OutError.ActualValue = MakeShared<FJsonValueString>(Catalog.GetCatalogKind());
			return false;
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
				OutError = ValidationFailure(TEXT("candidate_resolve_failed"),
					TEXT("The catalog source could not resolve the candidate and reported no diagnostic."), TEXT("candidate_id"));
				OutError.CandidateName = ExpectedSummary->GetDisplayName();
				OutError.ActualEvidence = MakeShared<FJsonValueString>(CandidateId);
			}
			return false;
		}
		if (!OutBinding.IsValid() || !OutBinding->IsValid())
		{
			OutError = OutBinding.IsValid() ? *OutBinding->GetValidationDiagnostic()
				: ValidationFailure(TEXT("candidate_binding_invalid"),
					TEXT("The catalog source resolved the candidate without returning a binding."), TEXT("binding"));
			OutBinding.Reset();
			return false;
		}
		if (OutBinding->GetCatalogRevision() != Catalog.GetRevision())
		{
			OutError = CompareRevision(OutBinding->GetCatalogRevision(), Catalog.GetRevision(), TEXT("catalog_revision_stale"));
			OutError.TargetPath = OutBinding->GetPublicRef();
			OutBinding.Reset();
			return false;
		}
		if (OutBinding->GetSummary() != *ExpectedSummary)
		{
			OutError = CompareSummary(OutBinding->GetSummary(), *ExpectedSummary);
			OutError.TargetPath = OutBinding->GetPublicRef();
			OutBinding.Reset();
			return false;
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
			OutError = *Binding.GetValidationDiagnostic();
			if (OutError.TargetPath.IsEmpty()) OutError.TargetPath = Binding.GetPublicRef();
			return false;
		}
		if (!Binding.Supports(EUeAgentCandidateOperation::Read))
		{
			OutError = ValidationFailure(TEXT("candidate_read_unsupported"), TEXT("The candidate binding does not support reading state."), TEXT("supported_operations"));
			OutError.TargetPath = Binding.GetPublicRef();
			OutError.ExpectedValue = MakeShared<FJsonValueString>(TEXT("read"));
			return false;
		}
		const TSharedPtr<IUeAgentStateReadPort>* ReadPort =
			ReadPorts.Find(Binding.GetReadPortId());
		if (ReadPort == nullptr || !ReadPort->IsValid())
		{
			OutError = ValidationFailure(TEXT("state_read_port_not_found"), TEXT("The state read port selected by the candidate binding is unavailable."), TEXT("read_port_id"));
			OutError.TargetPath = Binding.GetPublicRef();
			OutError.ActualEvidence = MakeShared<FJsonValueString>(Binding.GetReadPortId());
			return false;
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
				OutError = ValidationFailure(TEXT("state_read_failed"), TEXT("The state read port failed without reporting a diagnostic."), TEXT("snapshot"));
				OutError.ActualEvidence = MakeShared<FJsonValueString>(Binding.GetReadPortId());
			}
			if (OutError.TargetPath.IsEmpty()) OutError.TargetPath = Binding.GetPublicRef();
			return false;
		}
		if (!OutSnapshot.IsValid())
		{
			OutError = ValidationFailure(TEXT("state_read_snapshot_invalid"), TEXT("The state read port reported success without returning a snapshot."), TEXT("snapshot"));
			OutError.TargetPath = Binding.GetPublicRef();
			return false;
		}
		if (OutSnapshot->GetStatePlane() != StatePlane)
		{
			OutError = TextDifference(TEXT("state_read_plane_mismatch"), TEXT("The observed snapshot describes a different state plane from the requested one."),
				TEXT("snapshot.state_plane"), Contract::LexToString(OutSnapshot->GetStatePlane()), Contract::LexToString(StatePlane), true);
			OutError.TargetPath = Binding.GetPublicRef();
			OutSnapshot.Reset();
			return false;
		}
		if (OutSnapshot->GetProvenance().GetSourceKind()
			!= TEXT("independent_readback"))
		{
			OutError = TextDifference(TEXT("state_read_provenance_invalid"), TEXT("The observed snapshot does not identify its source as independent readback."),
				TEXT("snapshot.provenance.source_kind"), OutSnapshot->GetProvenance().GetSourceKind(), TEXT("independent_readback"), true);
			OutError.TargetPath = Binding.GetPublicRef();
			OutSnapshot.Reset();
			return false;
		}
		return true;
	}
}
