// Copyright Epic Games, Inc. All Rights Reserved.

#include "Utilities/AuthoringCore/UeAgentAuthoringTargetLocator.h"

#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Core/StateSemantic/UeAgentStateSemantic.h"
#include "UObject/Class.h"
#include "UObject/UnrealType.h"

namespace UeAgentAuthoringTargetLocator
{
	bool EnsureArrayElement(FScriptArrayHelper& Array, int32 Index, FDiagnostic& OutError)
	{
		const int64 RequiredCount = static_cast<int64>(Index) + 1;
		if (Index < 0 || RequiredCount > MAX_int32)
		{
			OutError = FDiagnostic::Failure(TEXT("array_index_exceeds_native_storage"),
				TEXT("The array index cannot be represented by native array storage."), TEXT("target_resolve"));
			OutError.PropertyPath = TEXT("index");
			OutError.ExpectedValue = MakeShared<FJsonValueString>(TEXT("integer from 0 through 2147483646"));
			OutError.ActualValue = MakeShared<FJsonValueNumber>(Index);
			return false;
		}
		if (RequiredCount > Array.Num()) Array.AddValues(static_cast<int32>(RequiredCount - Array.Num()));
		return true;
	}

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
		}

		void AddPropertyCandidates(
			const FString& RootOwnerPath,
			const UObject* NotificationOwner,
			const UStruct* CurrentStruct,
			const FString& Prefix,
			int32 MaxDiagnosticCandidates,
			FResolveResult& OutResult)
		{
			if (RootOwnerPath.IsEmpty() || !CurrentStruct) return;
			TArray<UeAgentAuthoringIdentity::FCanonicalIdentity> Identities;
			for (TFieldIterator<FProperty> It(CurrentStruct, EFieldIterationFlags::IncludeSuper); It; ++It)
			{
				UeAgentAuthoringIdentity::FCanonicalIdentity Identity;
				FDiagnostic Error;
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
			for (int32 Index = 0; Index < Identities.Num(); ++Index)
			{
				FDiagnosticCandidate Candidate;
				Candidate.SourceIndex = Index;
				Candidate.Identity = MoveTemp(Identities[Index]);
				OutResult.Candidates.Add(MoveTemp(Candidate));
			}
		}

		void SetPropertyFailure(
            FDiagnostic Diagnostic,
            const FString& RootOwnerPath,
            const UObject* NotificationOwner,
            const UStruct* CurrentStruct,
            const FString& Prefix,
            int32 MaxDiagnosticCandidates,
            FResolveResult& OutResult)
        {
            OutResult = FResolveResult();
            OutResult.Status = TEXT("failed");
            OutResult.Diagnostic = MoveTemp(Diagnostic);
            if (OutResult.Diagnostic.Stage.IsEmpty()) OutResult.Diagnostic.Stage = TEXT("property_target_resolve");
            if (OutResult.Diagnostic.TargetPath.IsEmpty()) OutResult.Diagnostic.TargetPath = RootOwnerPath;
            if (OutResult.Diagnostic.PropertyPath.IsEmpty()) OutResult.Diagnostic.PropertyPath = Prefix;
            AddPropertyCandidates(RootOwnerPath, NotificationOwner, CurrentStruct, Prefix, MaxDiagnosticCandidates, OutResult);
        }

		FDiagnostic PropertyFailure(const TCHAR* Code, const TCHAR* Reason, const FString& Path,
			const TCHAR* Expected, const TSharedPtr<FJsonValue>& Actual)
		{
			auto Error = FDiagnostic::Failure(Code, Reason, TEXT("property_target_resolve"));
			Error.PropertyPath = Path;
			Error.ExpectedValue = MakeShared<FJsonValueString>(Expected);
			Error.ActualValue = Actual;
			Error.ExpectedValuePresent = true;
			Error.ActualValuePresent = Actual.IsValid();
			return Error;
		}
	}

	bool FResolveResult::IsResolved() const
	{
		return ResolvedIndex != INDEX_NONE && Status == TEXT("resolved") && Diagnostic.IsEmpty();
	}

	TSharedRef<FJsonObject> FResolveResult::ToJsonObject() const
	{
		TSharedRef<FJsonObject> Result = MakeShared<FJsonObject>();
		Result->SetStringField(TEXT("status"), Status);
		Result->SetStringField(TEXT("error_code"), IsResolved() ? TEXT("target_resolved") : Diagnostic.Code);
        if (!Diagnostic.IsEmpty()) Result->SetObjectField(TEXT("diagnostic"), Diagnostic.ToPublicJson());
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
			OutResult.Diagnostic = PropertyFailure(TEXT("target_query_invalid"), ModelId.IsEmpty()
				? TEXT("A target query requires an identity model.") : TEXT("A target query requires selectors or a canonical identity key."),
				ModelId.IsEmpty() ? TEXT("identity_model") : TEXT("selectors"), TEXT("non-empty query field"), nullptr);
			return false;
		}
		if (!ResolvedRegistry.FindModel(ModelId, Model))
		{
			OutResult.Diagnostic = PropertyFailure(TEXT("target_identity_model_unknown"), TEXT("The requested target identity model is not registered."),
				TEXT("identity_model"), TEXT("registered identity model"), MakeShared<FJsonValueString>(ModelId));
			return false;
		}
		if (!Model.TargetKind.IsEmpty() && (TargetKind.IsEmpty() || Model.TargetKind != TargetKind))
		{
			OutResult.Diagnostic = PropertyFailure(TEXT("target_identity_model_mismatch"), TEXT("The requested target kind does not match its identity model."),
				TEXT("target_kind"), *Model.TargetKind, MakeShared<FJsonValueString>(TargetKind));
			OutResult.Diagnostic.CandidateName = ModelId;
			return false;
		}
		UeAgentAuthoringIdentity::FCanonicalIdentity ParsedCanonicalIdentity;
		const bool bHasCanonicalKey = !Query.CanonicalKey.TrimStartAndEnd().IsEmpty();
		if (bHasCanonicalKey)
		{
			FDiagnostic ParseError;
			if (!UeAgentAuthoringIdentity::ParseCanonicalKey(Query.CanonicalKey, ParsedCanonicalIdentity, ParseError, &ResolvedRegistry))
			{
				OutResult.Diagnostic = FDiagnostic::WrapFailure(TEXT("target_query_invalid"), ParseError, TEXT("target_resolve"));
				return false;
			}
			if (ParsedCanonicalIdentity.ModelId != ModelId || (!TargetKind.IsEmpty() && ParsedCanonicalIdentity.TargetKind != TargetKind))
			{
				const bool bModelMismatch = ParsedCanonicalIdentity.ModelId != ModelId;
				OutResult.Diagnostic = PropertyFailure(TEXT("target_identity_model_mismatch"),
					TEXT("The supplied canonical identity does not match the query model or target kind."),
					bModelMismatch ? TEXT("identity_model") : TEXT("target_kind"),
					*(bModelMismatch ? ModelId : TargetKind), MakeShared<FJsonValueString>(bModelMismatch ? ParsedCanonicalIdentity.ModelId : ParsedCanonicalIdentity.TargetKind));
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
				OutResult.Diagnostic = FDiagnostic::Failure(TEXT("target_selector_unknown"),
					FString::Printf(TEXT("Selector '%s' is not defined for target model '%s'."), *SelectorName, *ModelId), TEXT("target_resolve"));
				OutResult.Diagnostic.PropertyPath = SelectorName;
				OutResult.Diagnostic.CandidateName = ModelId;
				OutResult.Diagnostic.ActualValue = MakeShared<FJsonValueString>(SelectorName);
				TArray<TSharedPtr<FJsonValue>> Allowed;
				for (const auto& Declared : Model.Selectors) Allowed.Add(MakeShared<FJsonValueString>(Declared.Name));
				OutResult.Diagnostic.ExpectedValue = MakeShared<FJsonValueArray>(Allowed);
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
					OutResult.Diagnostic = FDiagnostic::Failure(TEXT("target_identity_conflict"),
						FString::Printf(TEXT("Selector '%s' conflicts with the supplied target identity."), *SelectorName), TEXT("target_resolve"));
					OutResult.Diagnostic.PropertyPath = SelectorName;
					OutResult.Diagnostic.ExpectedEvidence = MakeShared<FJsonValueString>(CanonicalValue);
					OutResult.Diagnostic.ActualEvidence = MakeShared<FJsonValueString>(SelectorValue);
					OutResult.ConflictingSelectors.Add(SelectorName);
					return false;
				}
			}
			if (const FString* ExistingValue = QuerySelectorValues.Find(SelectorName))
			{
				const ESearchCase::Type SearchCase = SelectorDescriptor->bCaseSensitive ? ESearchCase::CaseSensitive : ESearchCase::IgnoreCase;
				if (!ExistingValue->Equals(SelectorValue, SearchCase))
				{
					OutResult.Diagnostic = FDiagnostic::Failure(TEXT("target_identity_conflict"),
						FString::Printf(TEXT("Selector '%s' was supplied more than once with conflicting values."), *SelectorName), TEXT("target_resolve"));
					OutResult.Diagnostic.PropertyPath = SelectorName;
					OutResult.Diagnostic.ExpectedEvidence = MakeShared<FJsonValueString>(*ExistingValue);
					OutResult.Diagnostic.ActualEvidence = MakeShared<FJsonValueString>(SelectorValue);
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
			OutResult.Diagnostic = FDiagnostic::Failure(TEXT("target_query_invalid"),
				TEXT("Target resolution requires at least one nonempty identity or selector."), TEXT("target_resolve"));
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
			OutResult.Diagnostic.Reset();
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
				OutResult.Diagnostic.Reset();
				OutResult.ResolvedIndex = FullMatches[0];
				AddDiagnosticCandidates(
					Candidates,
					{ FullMatches[0] },
					Criteria,
					MaxDiagnosticCandidates,
					OutResult);
				return true;
			}
			OutResult.Diagnostic = FDiagnostic::Failure(TEXT("target_ambiguous"),
				FString::Printf(TEXT("The supplied selectors match %d distinct targets."), FullMatches.Num()), TEXT("target_resolve"));
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
			if (!OutResult.RequiredDiscriminators.IsEmpty())
				OutResult.Diagnostic.SuggestedFix = TEXT("Specify a distinguishing target field: ")
					+ FString::Join(OutResult.RequiredDiscriminators, TEXT(", ")) + TEXT(".");
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
			OutResult.Diagnostic = FDiagnostic::Failure(TEXT("target_identity_conflict"),
				TEXT("The supplied selectors identify different targets; no target satisfies all of them."), TEXT("target_resolve"));
			for (const int32 CriterionIndex : NonEmptyCriterionIndices) OutResult.ConflictingSelectors.Add(Criteria[CriterionIndex].Name);
			TArray<int32> ConflictCandidates = ConflictCandidateSet.Array();
			AddDiagnosticCandidates(Candidates, ConflictCandidates, Criteria, MaxDiagnosticCandidates, OutResult);
			return false;
		}

		OutResult.Diagnostic = FDiagnostic::Failure(TEXT("target_not_found"),
			TEXT("No target in the requested model and context matches all supplied selectors."), TEXT("target_resolve"));
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
		FDiagnostic& OutError)
	{
		if (!RootObject)
		{
			OutError = PropertyFailure(TEXT("target_query_invalid"), TEXT("A root object is required to identify a property."),
				TEXT("root_object"), TEXT("a live object"), nullptr);
			OutError.SourceKind = TEXT("native_result");
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
		FDiagnostic& OutError)
	{
		if (RootOwnerPath.IsEmpty() || PropertyPath.IsEmpty())
		{
			OutError = PropertyFailure(TEXT("target_query_invalid"),
				RootOwnerPath.IsEmpty() ? TEXT("The property owner path is empty.") : TEXT("The property path is empty."),
				RootOwnerPath.IsEmpty() ? TEXT("owner_path") : TEXT("property_path"), TEXT("non-empty path"), MakeShared<FJsonValueString>(FString()));
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
			int32 MaxDiagnosticCandidates,
			bool bAllowArrayAppend)
		{
			OutTarget = FResolvedPropertyTarget();
			FString CanonicalPath = PropertyPath;
			if (!RootStruct || !RootData || RootOwnerPath.IsEmpty()
				|| CanonicalPath.IsEmpty() || CanonicalPath.StartsWith(TEXT("."))
				|| CanonicalPath.EndsWith(TEXT(".")) || CanonicalPath.Contains(TEXT("..")))
			{
				FDiagnostic Error;
				if (!RootStruct || !RootData)
				{
					Error = PropertyFailure(TEXT("target_query_invalid"), TEXT("The native property root has no type or storage."),
						!RootStruct ? TEXT("root_type") : TEXT("root_storage"), TEXT("present native property root"), nullptr);
					Error.SourceKind = TEXT("native_result");
				}
				else if (RootOwnerPath.IsEmpty())
					Error = PropertyFailure(TEXT("target_query_invalid"), TEXT("The property owner path is empty."),
						TEXT("owner_path"), TEXT("non-empty path"), MakeShared<FJsonValueString>(FString()));
				else
					Error = PropertyFailure(TEXT("target_query_invalid"), TEXT("A property path must contain non-empty dot-separated segments."),
						CanonicalPath, TEXT("non-empty dot-separated property path"), MakeShared<FJsonValueString>(CanonicalPath));
				SetPropertyFailure(MoveTemp(Error), RootOwnerPath, NotificationOwner, RootStruct,
					FString(), MaxDiagnosticCandidates, OutResult);
				return false;
			}

			TArray<FString> Segments;
			CanonicalPath.ParseIntoArray(Segments, TEXT("."), false);
			UStruct* CurrentStruct = RootStruct;
			void* CurrentContainer = RootData;
			UObject* CurrentOwner = NotificationOwner;
			TArray<FProperty*> NotificationChain;
			bool bOwnerRoot = CurrentOwner && RootStruct == CurrentOwner->GetClass() && RootData == CurrentOwner;
			FString Prefix;
			for (int32 Index = 0; Index < Segments.Num(); ++Index)
			{
				const FString ResolvedPath = Prefix.IsEmpty() ? Segments[Index] : Prefix + TEXT(".") + Segments[Index];
				FString PropertyName = Segments[Index];
				int32 ElementIndex = INDEX_NONE;
				const int32 OpenBracket = PropertyName.Find(TEXT("["));
				if (OpenBracket != INDEX_NONE)
				{
					if (!PropertyName.EndsWith(TEXT("]")) || OpenBracket == 0
						|| PropertyName.Find(TEXT("["), ESearchCase::CaseSensitive, ESearchDir::FromStart, OpenBracket + 1) != INDEX_NONE)
					{
						SetPropertyFailure(PropertyFailure(TEXT("target_query_invalid"), TEXT("The array segment must end with one bracketed index after the property name."),
							ResolvedPath, TEXT("property[index]"), MakeShared<FJsonValueString>(Segments[Index])), RootOwnerPath, CurrentOwner,
							CurrentStruct, Prefix, MaxDiagnosticCandidates, OutResult);
						return false;
					}
					const FString IndexText = PropertyName.Mid(OpenBracket + 1, PropertyName.Len() - OpenBracket - 2);
					PropertyName.LeftInline(OpenBracket, EAllowShrinking::No);
					int64 ParsedIndex = 0;
					if (IndexText.IsEmpty() || !IndexText.IsNumeric()
						|| !UE::UAI::StateSemantic::TryParseInt64Number(IndexText, ParsedIndex)
						|| ParsedIndex < 0 || ParsedIndex > MAX_int32)
					{
						SetPropertyFailure(PropertyFailure(TEXT("target_query_invalid"), TEXT("The array index must be a non-negative native integer."),
							ResolvedPath, TEXT("integer from 0 through 2147483647"), MakeShared<FJsonValueString>(IndexText)), RootOwnerPath, CurrentOwner,
							CurrentStruct, Prefix, MaxDiagnosticCandidates, OutResult);
						return false;
					}
					ElementIndex = static_cast<int32>(ParsedIndex);
				}
				FProperty* Property = FindFProperty<FProperty>(CurrentStruct, *PropertyName);
				if (!Property)
				{
					auto Error = PropertyFailure(TEXT("target_not_found"), TEXT("The property does not exist on the current type."),
						ResolvedPath, TEXT("a property declared by the current type"), MakeShared<FJsonValueString>(PropertyName));
					Error.CandidateName = CurrentStruct->GetName();
					SetPropertyFailure(MoveTemp(Error), RootOwnerPath, CurrentOwner, CurrentStruct,
						Prefix, MaxDiagnosticCandidates, OutResult);
					return false;
				}
				if (Index == 0) OutTarget.RootProperty = Property;
				void* ValuePtr = Property->ContainerPtrToValuePtr<void>(CurrentContainer);
				if (bOwnerRoot) NotificationChain.Add(Property);
				if (ElementIndex != INDEX_NONE)
				{
					auto* ArrayProperty = CastField<FArrayProperty>(Property);
					if (!ArrayProperty || !ValuePtr)
					{
						auto Error = PropertyFailure(TEXT("target_not_found"), !ArrayProperty
							? TEXT("The indexed property is not an array.") : TEXT("The indexed array has no native storage."),
							ResolvedPath, TEXT("array with native storage"), !ArrayProperty ? TSharedPtr<FJsonValue>(MakeShared<FJsonValueString>(Property->GetCPPType())) : nullptr);
						if (!ValuePtr) Error.SourceKind = TEXT("native_result");
						SetPropertyFailure(MoveTemp(Error), RootOwnerPath, CurrentOwner,
							CurrentStruct, Prefix, MaxDiagnosticCandidates, OutResult);
						return false;
					}
					FScriptArrayHelper Array(ArrayProperty, ValuePtr);
					FDiagnostic ArrayError;
					if (bAllowArrayAppend && !EnsureArrayElement(Array, ElementIndex, ArrayError))
					{
						SetPropertyFailure(ArrayError, RootOwnerPath, CurrentOwner,
							CurrentStruct, ResolvedPath, MaxDiagnosticCandidates, OutResult);
						return false;
					}
					if (!Array.IsValidIndex(ElementIndex))
					{
						auto Error = PropertyFailure(TEXT("target_not_found"), TEXT("The requested array element does not exist."),
							ResolvedPath, *FString::Printf(TEXT("index below array length %d"), Array.Num()), MakeShared<FJsonValueNumber>(ElementIndex));
						SetPropertyFailure(MoveTemp(Error), RootOwnerPath, CurrentOwner,
							CurrentStruct, Prefix, MaxDiagnosticCandidates, OutResult);
						return false;
					}
					Property = ArrayProperty->Inner;
					ValuePtr = Array.GetRawPtr(ElementIndex);
				}
				if (Index == Segments.Num() - 1)
				{
					FDiagnostic IdentityError;
					if (!ValuePtr)
					{
						auto Error = PropertyFailure(TEXT("target_not_found"), TEXT("The resolved property has no native storage."),
							ResolvedPath, TEXT("present property storage"), nullptr);
						Error.SourceKind = TEXT("native_result");
						SetPropertyFailure(MoveTemp(Error), RootOwnerPath, CurrentOwner, CurrentStruct, Prefix, MaxDiagnosticCandidates, OutResult);
						return false;
					}
					if (!BuildPropertyIdentity(
						RootOwnerPath,
						ResolvedPath,
						CurrentOwner ? CurrentOwner->GetPathName() : FString(),
						OutTarget.Identity,
						IdentityError))
					{
						SetPropertyFailure(
							MoveTemp(IdentityError),
							RootOwnerPath, CurrentOwner, CurrentStruct, ResolvedPath,
							MaxDiagnosticCandidates, OutResult);
						return false;
					}
					OutTarget.NotificationOwner = CurrentOwner;
					OutTarget.Property = Property;
					OutTarget.PropertyScope = CurrentStruct;
					OutTarget.ValuePtr = ValuePtr;
					OutTarget.NotificationChain = MoveTemp(NotificationChain);
					OutResult = FResolveResult();
					OutResult.Status = TEXT("resolved");
					OutResult.Diagnostic.Reset();
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
						SetPropertyFailure(PropertyFailure(TEXT("target_not_found"), TEXT("The intermediate object reference is null."),
							Prefix, TEXT("non-null object reference"), MakeShared<FJsonValueNull>()), RootOwnerPath, CurrentOwner, nullptr,
							Prefix, MaxDiagnosticCandidates, OutResult);
						return false;
					}
					CurrentOwner = NestedObject;
					NotificationChain.Reset();
					bOwnerRoot = true;
					CurrentStruct = NestedObject->GetClass();
					CurrentContainer = NestedObject;
					continue;
				}
				SetPropertyFailure(PropertyFailure(TEXT("target_not_found"), TEXT("The intermediate property cannot contain child properties."),
					Prefix, TEXT("struct or object property"), MakeShared<FJsonValueString>(Property->GetCPPType())), RootOwnerPath, CurrentOwner, CurrentStruct,
					Prefix, MaxDiagnosticCandidates, OutResult);
				return false;
			}

			SetPropertyFailure(PropertyFailure(TEXT("target_not_found"), TEXT("The property path resolved no leaf property."),
				CanonicalPath, TEXT("a resolvable leaf property"), MakeShared<FJsonValueString>(CanonicalPath)), RootOwnerPath, NotificationOwner, RootStruct,
				Prefix, MaxDiagnosticCandidates, OutResult);
			return false;
		}
	}

	bool ResolvePropertyPath(
		UObject* RootObject,
		const FString& PropertyPath,
		FResolvedPropertyTarget& OutTarget,
		FResolveResult& OutResult,
		int32 MaxDiagnosticCandidates,
		bool bAllowArrayAppend)
	{
		return ResolvePropertyPathInternal(
			RootObject ? RootObject->GetClass() : nullptr,
			RootObject,
			RootObject ? RootObject->GetPathName() : FString(),
			RootObject,
			PropertyPath,
			OutTarget,
			OutResult,
			MaxDiagnosticCandidates,
			bAllowArrayAppend);
	}

	bool ResolvePropertyPath(
		UStruct* RootStruct,
		void* RootData,
		const FString& RootOwnerPath,
		UObject* NotificationOwner,
		const FString& PropertyPath,
		FResolvedPropertyTarget& OutTarget,
		FResolveResult& OutResult,
		int32 MaxDiagnosticCandidates,
		bool bAllowArrayAppend)
	{
		return ResolvePropertyPathInternal(
			RootStruct,
			RootData,
			RootOwnerPath,
			NotificationOwner,
			PropertyPath,
			OutTarget,
			OutResult,
			MaxDiagnosticCandidates,
			bAllowArrayAppend);
	}
}
