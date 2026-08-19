// Copyright Epic Games, Inc. All Rights Reserved.

#include "Utilities/AuthoringCore/UeAgentAuthoringTargetLocator.h"

#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "UObject/Class.h"
#include "UObject/UnrealType.h"

namespace UeAgentAuthoringTargetLocator
{
	namespace
	{
		struct FCriterion
		{
			FString Name;
			FString Value;
			bool bCanonicalKey = false;
			bool bCaseSensitive = false;
		};

		FString NormalizeId(FString Value)
		{
			Value.TrimStartAndEndInline();
			Value.ToLowerInline();
			return Value;
		}

		const UeAgentAuthoringIdentity::FSelectorDescriptor* FindSelector(
			const UeAgentAuthoringIdentity::FModelDescriptor& Model,
			const FString& Name)
		{
			return Model.Selectors.FindByPredicate([&Name](const UeAgentAuthoringIdentity::FSelectorDescriptor& Selector)
			{
				return Selector.Name == Name;
			});
		}

		bool MatchesCriterion(
			const UeAgentAuthoringIdentity::FCanonicalIdentity& Candidate,
			const FCriterion& Criterion)
		{
			if (Criterion.bCanonicalKey)
			{
				return Candidate.CanonicalKey.Equals(Criterion.Value, ESearchCase::IgnoreCase);
			}
			const FString CandidateValue = Candidate.GetSelector(Criterion.Name);
			return !CandidateValue.IsEmpty() && CandidateValue.Equals(
				Criterion.Value,
				Criterion.bCaseSensitive ? ESearchCase::CaseSensitive : ESearchCase::IgnoreCase);
		}

		void AddDiagnosticCandidates(
			const TArray<UeAgentAuthoringIdentity::FCanonicalIdentity>& Candidates,
			const TArray<int32>& SourceIndices,
			const TArray<FCriterion>& Criteria,
			int32 MaxDiagnosticCandidates,
			FResolveResult& OutResult)
		{
			for (const int32 SourceIndex : SourceIndices)
			{
				if (!Candidates.IsValidIndex(SourceIndex)) continue;
				FDiagnosticCandidate Diagnostic;
				Diagnostic.SourceIndex = SourceIndex;
				Diagnostic.Identity = Candidates[SourceIndex];
				for (const FCriterion& Criterion : Criteria)
				{
					if (MatchesCriterion(Diagnostic.Identity, Criterion)) ++Diagnostic.MatchedCriteria;
				}
				OutResult.Candidates.Add(MoveTemp(Diagnostic));
			}
			OutResult.Candidates.Sort([](const FDiagnosticCandidate& Left, const FDiagnosticCandidate& Right)
			{
				if (Left.MatchedCriteria != Right.MatchedCriteria) return Left.MatchedCriteria > Right.MatchedCriteria;
				return Left.Identity.CanonicalKey < Right.Identity.CanonicalKey;
			});
			if (MaxDiagnosticCandidates >= 0 && OutResult.Candidates.Num() > MaxDiagnosticCandidates)
			{
				OutResult.Candidates.SetNum(MaxDiagnosticCandidates, EAllowShrinking::No);
			}
		}

		void AddPropertyCandidates(
			const FString& RootOwnerPath,
			const UObject* NotificationOwner,
			const UStruct* CurrentStruct,
			const FString& Prefix,
			int32 MaxDiagnosticCandidates,
			FResolveResult& OutResult)
		{
			if (RootOwnerPath.IsEmpty() || !CurrentStruct || MaxDiagnosticCandidates == 0) return;
			TArray<UeAgentAuthoringIdentity::FCanonicalIdentity> Identities;
			for (TFieldIterator<FProperty> It(CurrentStruct, EFieldIterationFlags::IncludeSuper); It; ++It)
			{
				UeAgentAuthoringIdentity::FCanonicalIdentity Identity;
				FString Error;
				const FString CandidatePath = Prefix.IsEmpty() ? It->GetName() : Prefix + TEXT(".") + It->GetName();
				if (BuildPropertyIdentity(
					RootOwnerPath,
					CandidatePath,
					NotificationOwner ? NotificationOwner->GetPathName() : FString(),
					Identity,
					Error))
				{
					Identities.Add(MoveTemp(Identity));
				}
			}
			Identities.Sort([](const UeAgentAuthoringIdentity::FCanonicalIdentity& Left, const UeAgentAuthoringIdentity::FCanonicalIdentity& Right)
			{
				return Left.GetSelector(TEXT("property_path")) < Right.GetSelector(TEXT("property_path"));
			});
			if (MaxDiagnosticCandidates >= 0 && Identities.Num() > MaxDiagnosticCandidates)
			{
				Identities.SetNum(MaxDiagnosticCandidates, EAllowShrinking::No);
			}
			for (int32 Index = 0; Index < Identities.Num(); ++Index)
			{
				FDiagnosticCandidate Candidate;
				Candidate.SourceIndex = Index;
				Candidate.Identity = MoveTemp(Identities[Index]);
				OutResult.Candidates.Add(MoveTemp(Candidate));
			}
		}

		void SetPropertyFailure(
			const FString& ErrorCode,
			const FString& RootOwnerPath,
			const UObject* NotificationOwner,
			const UStruct* CurrentStruct,
			const FString& Prefix,
			int32 MaxDiagnosticCandidates,
			FResolveResult& OutResult)
		{
			OutResult = FResolveResult();
			OutResult.Status = TEXT("failed");
			OutResult.ErrorCode = ErrorCode;
			AddPropertyCandidates(RootOwnerPath, NotificationOwner, CurrentStruct, Prefix, MaxDiagnosticCandidates, OutResult);
		}
	}

	bool FResolveResult::IsResolved() const
	{
		return ResolvedIndex != INDEX_NONE && ErrorCode == TEXT("target_resolved");
	}

	TSharedRef<FJsonObject> FResolveResult::ToJsonObject() const
	{
		TSharedRef<FJsonObject> Result = MakeShared<FJsonObject>();
		Result->SetStringField(TEXT("status"), Status);
		Result->SetStringField(TEXT("error_code"), ErrorCode);
		if (ResolvedIndex != INDEX_NONE) Result->SetNumberField(TEXT("resolved_index"), ResolvedIndex);
		TArray<TSharedPtr<FJsonValue>> ConflictValues;
		for (const FString& Selector : ConflictingSelectors) ConflictValues.Add(MakeShared<FJsonValueString>(Selector));
		Result->SetArrayField(TEXT("conflicting_selectors"), ConflictValues);
		TArray<TSharedPtr<FJsonValue>> DiscriminatorValues;
		for (const FString& Selector : RequiredDiscriminators) DiscriminatorValues.Add(MakeShared<FJsonValueString>(Selector));
		Result->SetArrayField(TEXT("required_discriminators"), DiscriminatorValues);
		TArray<TSharedPtr<FJsonValue>> CandidateValues;
		for (const FDiagnosticCandidate& Candidate : Candidates)
		{
			TSharedRef<FJsonObject> CandidateObject = Candidate.Identity.ToJsonObject();
			CandidateObject->SetNumberField(TEXT("source_index"), Candidate.SourceIndex);
			CandidateObject->SetNumberField(TEXT("matched_criteria"), Candidate.MatchedCriteria);
			CandidateValues.Add(MakeShared<FJsonValueObject>(CandidateObject));
		}
		Result->SetArrayField(TEXT("candidates"), CandidateValues);
		return Result;
	}

	bool ResolveExact(
		const FTargetQuery& Query,
		const TArray<UeAgentAuthoringIdentity::FCanonicalIdentity>& Candidates,
		FResolveResult& OutResult,
		int32 MaxDiagnosticCandidates,
		const UeAgentAuthoringIdentity::FRegistry* Registry)
	{
		OutResult = FResolveResult();
		OutResult.Status = TEXT("failed");
		const UeAgentAuthoringIdentity::FRegistry& ResolvedRegistry = Registry ? *Registry : UeAgentAuthoringIdentity::FRegistry::Get();
		UeAgentAuthoringIdentity::FModelDescriptor Model;
		const FString ModelId = NormalizeId(Query.ModelId);
		const FString TargetKind = NormalizeId(Query.TargetKind);
		if (ModelId.IsEmpty() || (Query.CanonicalKey.TrimStartAndEnd().IsEmpty() && Query.Selectors.IsEmpty()))
		{
			OutResult.ErrorCode = TEXT("target_query_invalid");
			return false;
		}
		if (!ResolvedRegistry.FindModel(ModelId, Model))
		{
			OutResult.ErrorCode = TEXT("target_identity_model_unknown");
			return false;
		}
		if (!Model.TargetKind.IsEmpty() && (TargetKind.IsEmpty() || Model.TargetKind != TargetKind))
		{
			OutResult.ErrorCode = TEXT("target_identity_model_mismatch");
			return false;
		}
		UeAgentAuthoringIdentity::FCanonicalIdentity ParsedCanonicalIdentity;
		const bool bHasCanonicalKey = !Query.CanonicalKey.TrimStartAndEnd().IsEmpty();
		if (bHasCanonicalKey)
		{
			FString ParseError;
			if (!UeAgentAuthoringIdentity::ParseCanonicalKey(Query.CanonicalKey, ParsedCanonicalIdentity, ParseError, &ResolvedRegistry))
			{
				OutResult.ErrorCode = TEXT("target_query_invalid");
				return false;
			}
			if (ParsedCanonicalIdentity.ModelId != ModelId || (!TargetKind.IsEmpty() && ParsedCanonicalIdentity.TargetKind != TargetKind))
			{
				OutResult.ErrorCode = TEXT("target_identity_model_mismatch");
				return false;
			}
		}

		TArray<FCriterion> Criteria;
		FString CanonicalKey = Query.CanonicalKey;
		CanonicalKey.TrimStartAndEndInline();
		if (!CanonicalKey.IsEmpty())
		{
			FCriterion& Criterion = Criteria.AddDefaulted_GetRef();
			Criterion.Name = TEXT("canonical_key");
			Criterion.Value = MoveTemp(CanonicalKey);
			Criterion.bCanonicalKey = true;
		}
		TMap<FString, FString> QuerySelectorValues;
		for (const TPair<FString, FString>& Pair : Query.Selectors)
		{
			const FString SelectorName = NormalizeId(Pair.Key);
			if (SelectorName.IsEmpty()) continue;
			const UeAgentAuthoringIdentity::FSelectorDescriptor* SelectorDescriptor = FindSelector(Model, SelectorName);
			if (!SelectorDescriptor)
			{
				OutResult.ErrorCode = TEXT("target_selector_unknown");
				return false;
			}
			FString SelectorValue = Pair.Value;
			if (SelectorDescriptor->bTrimValue) SelectorValue.TrimStartAndEndInline();
			if (SelectorValue.IsEmpty()) continue;
			if (bHasCanonicalKey)
			{
				const FString CanonicalValue = ParsedCanonicalIdentity.GetSelector(SelectorName);
				const ESearchCase::Type SearchCase = SelectorDescriptor->bCaseSensitive ? ESearchCase::CaseSensitive : ESearchCase::IgnoreCase;
				if (!CanonicalValue.IsEmpty() && !CanonicalValue.Equals(SelectorValue, SearchCase))
				{
					OutResult.ErrorCode = TEXT("target_identity_conflict");
					OutResult.ConflictingSelectors.Add(SelectorName);
					return false;
				}
			}
			if (const FString* ExistingValue = QuerySelectorValues.Find(SelectorName))
			{
				const ESearchCase::Type SearchCase = SelectorDescriptor->bCaseSensitive ? ESearchCase::CaseSensitive : ESearchCase::IgnoreCase;
				if (!ExistingValue->Equals(SelectorValue, SearchCase))
				{
					OutResult.ErrorCode = TEXT("target_identity_conflict");
					OutResult.ConflictingSelectors.Add(SelectorName);
					return false;
				}
				continue;
			}
			QuerySelectorValues.Add(SelectorName, SelectorValue);
			FCriterion& Criterion = Criteria.AddDefaulted_GetRef();
			Criterion.Name = SelectorName;
			Criterion.Value = MoveTemp(SelectorValue);
			Criterion.bCaseSensitive = SelectorDescriptor->bCaseSensitive;
		}
		if (Criteria.IsEmpty())
		{
			OutResult.ErrorCode = TEXT("target_query_invalid");
			return false;
		}

		TArray<int32> EligibleIndices;
		TArray<int32> FullMatches;
		TArray<TArray<int32>> CriterionMatches;
		CriterionMatches.SetNum(Criteria.Num());
		for (int32 CandidateIndex = 0; CandidateIndex < Candidates.Num(); ++CandidateIndex)
		{
			const UeAgentAuthoringIdentity::FCanonicalIdentity& Candidate = Candidates[CandidateIndex];
			if (!Candidate.IsValid() || Candidate.ModelId != ModelId || (!TargetKind.IsEmpty() && Candidate.TargetKind != TargetKind)) continue;
			EligibleIndices.Add(CandidateIndex);
			bool bMatchesAll = true;
			for (int32 CriterionIndex = 0; CriterionIndex < Criteria.Num(); ++CriterionIndex)
			{
				if (MatchesCriterion(Candidate, Criteria[CriterionIndex]))
				{
					CriterionMatches[CriterionIndex].Add(CandidateIndex);
				}
				else
				{
					bMatchesAll = false;
				}
			}
			if (bMatchesAll) FullMatches.Add(CandidateIndex);
		}

		if (FullMatches.Num() == 1)
		{
			OutResult.Status = TEXT("resolved");
			OutResult.ErrorCode = TEXT("target_resolved");
			OutResult.ResolvedIndex = FullMatches[0];
			AddDiagnosticCandidates(Candidates, FullMatches, Criteria, MaxDiagnosticCandidates, OutResult);
			return true;
		}
		if (FullMatches.Num() > 1)
		{
			const FString& FirstCanonicalKey = Candidates[FullMatches[0]].CanonicalKey;
			bool bSingleLogicalIdentity = !FirstCanonicalKey.IsEmpty();
			for (int32 MatchIndex = 1; MatchIndex < FullMatches.Num() && bSingleLogicalIdentity; ++MatchIndex)
			{
				bSingleLogicalIdentity = Candidates[FullMatches[MatchIndex]].CanonicalKey.Equals(
					FirstCanonicalKey,
					ESearchCase::IgnoreCase);
			}
			if (bSingleLogicalIdentity)
			{
				OutResult.Status = TEXT("resolved");
				OutResult.ErrorCode = TEXT("target_resolved");
				OutResult.ResolvedIndex = FullMatches[0];
				AddDiagnosticCandidates(
					Candidates,
					{ FullMatches[0] },
					Criteria,
					MaxDiagnosticCandidates,
					OutResult);
				return true;
			}
			OutResult.ErrorCode = TEXT("target_ambiguous");
			for (const UeAgentAuthoringIdentity::FSelectorDescriptor& Selector : Model.Selectors)
			{
				if (QuerySelectorValues.Contains(Selector.Name)) continue;
				TSet<FString> Values;
				for (const int32 MatchIndex : FullMatches)
				{
					FString Value = Candidates[MatchIndex].GetSelector(Selector.Name);
					if (!Selector.bCaseSensitive) Value.ToLowerInline();
					if (!Value.IsEmpty()) Values.Add(MoveTemp(Value));
				}
				if (Values.Num() > 1) OutResult.RequiredDiscriminators.Add(Selector.Name);
			}
			AddDiagnosticCandidates(Candidates, FullMatches, Criteria, MaxDiagnosticCandidates, OutResult);
			return false;
		}

		TArray<int32> NonEmptyCriterionIndices;
		TSet<int32> ConflictCandidateSet;
		for (int32 CriterionIndex = 0; CriterionIndex < CriterionMatches.Num(); ++CriterionIndex)
		{
			if (CriterionMatches[CriterionIndex].IsEmpty()) continue;
			NonEmptyCriterionIndices.Add(CriterionIndex);
			for (const int32 CandidateIndex : CriterionMatches[CriterionIndex]) ConflictCandidateSet.Add(CandidateIndex);
		}
		bool bCriteriaConflict = false;
		if (NonEmptyCriterionIndices.Num() >= 2)
		{
			TArray<int32> CommonMatches = CriterionMatches[NonEmptyCriterionIndices[0]];
			for (int32 Index = 1; Index < NonEmptyCriterionIndices.Num() && !CommonMatches.IsEmpty(); ++Index)
			{
				const TArray<int32>& CurrentMatches = CriterionMatches[NonEmptyCriterionIndices[Index]];
				CommonMatches.RemoveAll([&CurrentMatches](int32 CandidateIndex) { return !CurrentMatches.Contains(CandidateIndex); });
			}
			bCriteriaConflict = CommonMatches.IsEmpty();
		}
		if (bCriteriaConflict)
		{
			OutResult.ErrorCode = TEXT("target_identity_conflict");
			for (const int32 CriterionIndex : NonEmptyCriterionIndices) OutResult.ConflictingSelectors.Add(Criteria[CriterionIndex].Name);
			TArray<int32> ConflictCandidates = ConflictCandidateSet.Array();
			AddDiagnosticCandidates(Candidates, ConflictCandidates, Criteria, MaxDiagnosticCandidates, OutResult);
			return false;
		}

		OutResult.ErrorCode = TEXT("target_not_found");
		TArray<int32> DiagnosticIndices;
		if (!NonEmptyCriterionIndices.IsEmpty()) DiagnosticIndices = CriterionMatches[NonEmptyCriterionIndices[0]];
		else DiagnosticIndices = EligibleIndices;
		AddDiagnosticCandidates(Candidates, DiagnosticIndices, Criteria, MaxDiagnosticCandidates, OutResult);
		return false;
	}

	bool BuildPropertyIdentity(
		const UObject* RootObject,
		const FString& PropertyPath,
		const UObject* NotificationOwner,
		UeAgentAuthoringIdentity::FCanonicalIdentity& OutIdentity,
		FString& OutError)
	{
		if (!RootObject)
		{
			OutError = TEXT("target_query_invalid");
			return false;
		}
		return BuildPropertyIdentity(
			RootObject->GetPathName(),
			PropertyPath,
			NotificationOwner ? NotificationOwner->GetPathName() : FString(),
			OutIdentity,
			OutError);
	}

	bool BuildPropertyIdentity(
		const FString& RootOwnerPath,
		const FString& PropertyPath,
		const FString& NotificationOwnerPath,
		UeAgentAuthoringIdentity::FCanonicalIdentity& OutIdentity,
		FString& OutError)
	{
		if (RootOwnerPath.IsEmpty() || PropertyPath.IsEmpty())
		{
			OutError = TEXT("target_query_invalid");
			return false;
		}
		TMap<FString, FString> Selectors;
		Selectors.Add(TEXT("owner_path"), RootOwnerPath);
		Selectors.Add(TEXT("property_path"), PropertyPath);
		if (!NotificationOwnerPath.IsEmpty()) Selectors.Add(TEXT("notification_owner_path"), NotificationOwnerPath);
		return UeAgentAuthoringIdentity::BuildCanonicalIdentity(
			TEXT("property_identity"), TEXT("property"), Selectors, OutIdentity, OutError);
	}

	namespace
	{
		bool ResolvePropertyPathInternal(
			UStruct* RootStruct,
			void* RootData,
			const FString& RootOwnerPath,
			UObject* NotificationOwner,
			const FString& PropertyPath,
			FResolvedPropertyTarget& OutTarget,
			FResolveResult& OutResult,
			int32 MaxDiagnosticCandidates)
		{
			OutTarget = FResolvedPropertyTarget();
			FString CanonicalPath = PropertyPath;
			if (!RootStruct || !RootData || RootOwnerPath.IsEmpty()
				|| CanonicalPath.IsEmpty() || CanonicalPath.StartsWith(TEXT("."))
				|| CanonicalPath.EndsWith(TEXT(".")) || CanonicalPath.Contains(TEXT("..")))
			{
				SetPropertyFailure(
					TEXT("target_query_invalid"), RootOwnerPath, NotificationOwner, RootStruct,
					FString(), MaxDiagnosticCandidates, OutResult);
				return false;
			}

			TArray<FString> Segments;
			CanonicalPath.ParseIntoArray(Segments, TEXT("."), false);
			UStruct* CurrentStruct = RootStruct;
			void* CurrentContainer = RootData;
			UObject* CurrentOwner = NotificationOwner;
			FString Prefix;
			for (int32 Index = 0; Index < Segments.Num(); ++Index)
			{
				FProperty* Property = FindFProperty<FProperty>(CurrentStruct, *Segments[Index]);
				if (!Property)
				{
					SetPropertyFailure(
						TEXT("target_not_found"), RootOwnerPath, CurrentOwner, CurrentStruct,
						Prefix, MaxDiagnosticCandidates, OutResult);
					return false;
				}
				void* ValuePtr = Property->ContainerPtrToValuePtr<void>(CurrentContainer);
				const FString ResolvedPath = Prefix.IsEmpty() ? Segments[Index] : Prefix + TEXT(".") + Segments[Index];
				if (Index == Segments.Num() - 1)
				{
					FString IdentityError;
					if (!ValuePtr || !BuildPropertyIdentity(
						RootOwnerPath,
						ResolvedPath,
						CurrentOwner ? CurrentOwner->GetPathName() : FString(),
						OutTarget.Identity,
						IdentityError))
					{
						SetPropertyFailure(
							IdentityError.IsEmpty() ? TEXT("target_not_found") : IdentityError,
							RootOwnerPath, CurrentOwner, CurrentStruct, Prefix,
							MaxDiagnosticCandidates, OutResult);
						return false;
					}
					OutTarget.NotificationOwner = CurrentOwner;
					OutTarget.Property = Property;
					OutTarget.ValuePtr = ValuePtr;
					OutResult = FResolveResult();
					OutResult.Status = TEXT("resolved");
					OutResult.ErrorCode = TEXT("target_resolved");
					OutResult.ResolvedIndex = 0;
					FDiagnosticCandidate& Candidate = OutResult.Candidates.AddDefaulted_GetRef();
					Candidate.SourceIndex = 0;
					Candidate.MatchedCriteria = 2;
					Candidate.Identity = OutTarget.Identity;
					return true;
				}

				Prefix = ResolvedPath;
				if (FStructProperty* StructProperty = CastField<FStructProperty>(Property))
				{
					CurrentStruct = StructProperty->Struct;
					CurrentContainer = ValuePtr;
					continue;
				}
				if (FObjectPropertyBase* ObjectProperty = CastField<FObjectPropertyBase>(Property))
				{
					UObject* NestedObject = ObjectProperty->GetObjectPropertyValue(ValuePtr);
					if (!NestedObject)
					{
						SetPropertyFailure(
							TEXT("target_not_found"), RootOwnerPath, CurrentOwner, nullptr,
							Prefix, MaxDiagnosticCandidates, OutResult);
						return false;
					}
					CurrentOwner = NestedObject;
					CurrentStruct = NestedObject->GetClass();
					CurrentContainer = NestedObject;
					continue;
				}
				SetPropertyFailure(
					TEXT("target_not_found"), RootOwnerPath, CurrentOwner, CurrentStruct,
					Prefix, MaxDiagnosticCandidates, OutResult);
				return false;
			}

			SetPropertyFailure(
				TEXT("target_not_found"), RootOwnerPath, NotificationOwner, RootStruct,
				Prefix, MaxDiagnosticCandidates, OutResult);
			return false;
		}
	}

	bool ResolvePropertyPath(
		UObject* RootObject,
		const FString& PropertyPath,
		FResolvedPropertyTarget& OutTarget,
		FResolveResult& OutResult,
		int32 MaxDiagnosticCandidates)
	{
		return ResolvePropertyPathInternal(
			RootObject ? RootObject->GetClass() : nullptr,
			RootObject,
			RootObject ? RootObject->GetPathName() : FString(),
			RootObject,
			PropertyPath,
			OutTarget,
			OutResult,
			MaxDiagnosticCandidates);
	}

	bool ResolvePropertyPath(
		UStruct* RootStruct,
		void* RootData,
		const FString& RootOwnerPath,
		UObject* NotificationOwner,
		const FString& PropertyPath,
		FResolvedPropertyTarget& OutTarget,
		FResolveResult& OutResult,
		int32 MaxDiagnosticCandidates)
	{
		return ResolvePropertyPathInternal(
			RootStruct,
			RootData,
			RootOwnerPath,
			NotificationOwner,
			PropertyPath,
			OutTarget,
			OutResult,
			MaxDiagnosticCandidates);
	}
}
