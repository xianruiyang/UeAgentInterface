// Copyright Epic Games, Inc. All Rights Reserved.

#include "Utilities/AuthoringExport/UeAgentCompoundProjectionExtension.h"

namespace
{
	FString NormalizeId(FString Value)
	{
		Value.TrimStartAndEndInline();
		Value.ToLowerInline();
		return Value;
	}

	FString EscapeJsonPointerSegment(FString Segment)
	{
		Segment.ReplaceInline(TEXT("~"), TEXT("~0"), ESearchCase::CaseSensitive);
		Segment.ReplaceInline(TEXT("/"), TEXT("~1"), ESearchCase::CaseSensitive);
		return Segment;
	}

	bool DecodeJsonPointerSegment(const FString& Encoded, FString& OutDecoded)
	{
		OutDecoded.Reset();
		for (int32 Index = 0; Index < Encoded.Len(); ++Index)
		{
			if (Encoded[Index] != TEXT('~'))
			{
				OutDecoded.AppendChar(Encoded[Index]);
				continue;
			}
			if (++Index >= Encoded.Len()) return false;
			if (Encoded[Index] == TEXT('0')) OutDecoded.AppendChar(TEXT('~'));
			else if (Encoded[Index] == TEXT('1')) OutDecoded.AppendChar(TEXT('/'));
			else return false;
		}
		return !OutDecoded.IsEmpty();
	}

	bool ParseJsonPointer(
		const FString& JsonPointer,
		bool bAllowWildcards,
		TArray<FString>& OutSegments,
		FString& OutError)
	{
		OutSegments.Reset();
		if (!JsonPointer.StartsWith(TEXT("/")))
		{
			OutError = TEXT("json_pointer_must_start_with_slash");
			return false;
		}
		TArray<FString> EncodedSegments;
		JsonPointer.Mid(1).ParseIntoArray(EncodedSegments, TEXT("/"), false);
		if (EncodedSegments.IsEmpty())
		{
			OutError = TEXT("json_pointer_segments_missing");
			return false;
		}
		for (const FString& Encoded : EncodedSegments)
		{
			FString Decoded;
			if (!DecodeJsonPointerSegment(Encoded, Decoded))
			{
				OutError = TEXT("json_pointer_segment_invalid");
				return false;
			}
			if (!bAllowWildcards && Decoded == TEXT("*"))
			{
				OutError = TEXT("json_pointer_unexpected_wildcard");
				return false;
			}
			if (bAllowWildcards && Decoded.Contains(TEXT("*")) && Decoded != TEXT("*"))
			{
				OutError = TEXT("json_pointer_wildcard_must_fill_segment");
				return false;
			}
			OutSegments.Add(MoveTemp(Decoded));
		}
		OutError.Reset();
		return true;
	}

	FString BuildJsonPointer(const TArray<FString>& Segments, int32 Count)
	{
		if (Count <= 0) return FString();
		FString Result;
		for (int32 Index = 0; Index < Count; ++Index)
		{
			Result += TEXT("/") + EscapeJsonPointerSegment(Segments[Index]);
		}
		return Result;
	}
}

namespace UeAgentCompoundProjection
{
	bool FExtensionDescriptor::SupportsSourceKind(UeAgentExportContract::EFieldSourceKind Kind) const
	{
		return SourceKinds.Contains(Kind);
	}

	bool FExtensionDescriptor::operator==(const FExtensionDescriptor& Other) const
	{
		return ProjectionId == Other.ProjectionId
			&& ImplementationId == Other.ImplementationId
			&& SourceKinds == Other.SourceKinds;
	}

	FRegistry& FRegistry::Get()
	{
		static FRegistry Registry;
		return Registry;
	}

	bool FRegistry::RegisterExtension(
		const FExtensionDescriptor& Descriptor,
		FExtension Extension,
		ERegistrationResult& OutResult,
		FString& OutError)
	{
		FExtensionDescriptor Canonical = Descriptor;
		Canonical.ProjectionId = NormalizeId(Canonical.ProjectionId);
		Canonical.ImplementationId = NormalizeId(Canonical.ImplementationId);
		if (Canonical.ProjectionId.IsEmpty() || Canonical.ImplementationId.IsEmpty()
			|| Canonical.SourceKinds.IsEmpty() || !Extension)
		{
			OutError = TEXT("export_compound_extension_descriptor_invalid");
			return false;
		}
		TSet<UeAgentExportContract::EFieldSourceKind> UniqueKinds;
		for (UeAgentExportContract::EFieldSourceKind Kind : Canonical.SourceKinds)
		{
			if (UniqueKinds.Contains(Kind))
			{
				OutError = TEXT("export_compound_extension_source_kind_duplicate");
				return false;
			}
			UniqueKinds.Add(Kind);
		}

		FWriteScopeLock Lock(RegistryLock);
		if (const FEntry* Existing = Extensions.Find(Canonical.ProjectionId))
		{
			if (Existing->Descriptor != Canonical)
			{
				OutError = TEXT("conflicting_export_compound_extension_") + Canonical.ProjectionId;
				return false;
			}
			OutResult = ERegistrationResult::AlreadyRegistered;
			OutError.Reset();
			return true;
		}
		FEntry Entry;
		Entry.Descriptor = MoveTemp(Canonical);
		Entry.Extension = MoveTemp(Extension);
		Extensions.Add(Entry.Descriptor.ProjectionId, MoveTemp(Entry));
		OutResult = ERegistrationResult::Registered;
		OutError.Reset();
		return true;
	}

	bool FRegistry::FindExtension(const FString& ProjectionId, FExtensionDescriptor& OutDescriptor) const
	{
		FReadScopeLock Lock(RegistryLock);
		if (const FEntry* Found = Extensions.Find(NormalizeId(ProjectionId)))
		{
			OutDescriptor = Found->Descriptor;
			return true;
		}
		return false;
	}

	bool FRegistry::CopyExtension(const FString& ProjectionId, FExtension& OutExtension) const
	{
		FReadScopeLock Lock(RegistryLock);
		if (const FEntry* Found = Extensions.Find(NormalizeId(ProjectionId)))
		{
			OutExtension = Found->Extension;
			return true;
		}
		OutExtension = FExtension();
		return false;
	}

	TArray<FExtensionDescriptor> FRegistry::ListExtensions() const
	{
		FReadScopeLock Lock(RegistryLock);
		TArray<FExtensionDescriptor> Result;
		for (const TPair<FString, FEntry>& Pair : Extensions) Result.Add(Pair.Value.Descriptor);
		Result.Sort([](const FExtensionDescriptor& Left, const FExtensionDescriptor& Right)
		{
			return Left.ProjectionId < Right.ProjectionId;
		});
		return Result;
	}

	int32 FRegistry::GetExtensionCount() const
	{
		FReadScopeLock Lock(RegistryLock);
		return Extensions.Num();
	}

	bool InspectJsonPointerPattern(
		const FString& JsonPointerPattern,
		FJsonPointerPatternInfo& OutInfo,
		FString& OutError)
	{
		OutInfo = FJsonPointerPatternInfo();
		TArray<FString> Segments;
		if (!ParseJsonPointer(JsonPointerPattern, true, Segments, OutError)) return false;
		int32 FirstWildcard = INDEX_NONE;
		for (int32 Index = 0; Index < Segments.Num(); ++Index)
		{
			if (Segments[Index] != TEXT("*")) continue;
			if (FirstWildcard == INDEX_NONE) FirstWildcard = Index;
			++OutInfo.WildcardCount;
		}
		if (OutInfo.WildcardCount == 0)
		{
			OutError = TEXT("json_pointer_wildcard_missing");
			return false;
		}
		OutInfo.EmptyContainerPointer = BuildJsonPointer(Segments, FirstWildcard);
		OutError.Reset();
		return true;
	}

	bool ExpandJsonPointerPattern(
		const FString& JsonPointerPattern,
		const TArray<FString>& WildcardValues,
		FString& OutJsonPointer,
		FString& OutError)
	{
		OutJsonPointer.Reset();
		TArray<FString> Segments;
		if (!ParseJsonPointer(JsonPointerPattern, true, Segments, OutError)) return false;
		int32 WildcardIndex = 0;
		for (FString& Segment : Segments)
		{
			if (Segment != TEXT("*")) continue;
			if (!WildcardValues.IsValidIndex(WildcardIndex)
				|| WildcardValues[WildcardIndex].IsEmpty()
				|| WildcardValues[WildcardIndex] == TEXT("*"))
			{
				OutError = TEXT("json_pointer_wildcard_value_invalid");
				return false;
			}
			Segment = WildcardValues[WildcardIndex++];
		}
		if (WildcardIndex != WildcardValues.Num())
		{
			OutError = TEXT("json_pointer_wildcard_cardinality_mismatch");
			return false;
		}
		OutJsonPointer = BuildJsonPointer(Segments, Segments.Num());
		OutError.Reset();
		return true;
	}

	bool MatchesExpandedJsonPointer(
		const FString& JsonPointerPattern,
		const FString& JsonPointer)
	{
		TArray<FString> PatternSegments;
		TArray<FString> ConcreteSegments;
		FString Error;
		if (!ParseJsonPointer(JsonPointerPattern, true, PatternSegments, Error)
			|| !ParseJsonPointer(JsonPointer, false, ConcreteSegments, Error)
			|| PatternSegments.Num() != ConcreteSegments.Num())
		{
			return false;
		}
		for (int32 Index = 0; Index < PatternSegments.Num(); ++Index)
		{
			if (PatternSegments[Index] != TEXT("*")
				&& PatternSegments[Index] != ConcreteSegments[Index])
			{
				return false;
			}
		}
		return true;
	}
}
