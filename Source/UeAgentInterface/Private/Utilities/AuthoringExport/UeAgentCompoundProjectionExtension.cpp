// Copyright Epic Games, Inc. All Rights Reserved.

#include "Utilities/AuthoringExport/UeAgentCompoundProjectionExtension.h"
#include "Core/HjsonWorkspace/UeAgentHjsonWorkspace.h"
#include "Utilities/AuthoringCore/UeAgentAuthoringDiagnostics.h"
#include "Dom/JsonValue.h"

namespace
{
	UE::UAI::Contract::FDiagnostic ProjectionFailure(const FString& Code, const FString& Path,
		const TSharedPtr<FJsonValue>& Expected, const TSharedPtr<FJsonValue>& Actual, const FString& Stage, const FString& Reason)
	{
		auto Error = UeAgentAuthoringDiagnostics::MakeValueFailure(Code, Path, Expected, Actual, Stage, Reason);
		// These paths describe generated output or native projector metadata,
		// not fields in an imported HJSON document.
		Error.SourceKind = TEXT("native_result");
		return Error;
	}
	TSharedPtr<FJsonValue> StringValue(const FString& Value) { return MakeShared<FJsonValueString>(Value); }
	TSharedPtr<FJsonValue> SourceKindsValue(const TArray<UeAgentExportContract::EFieldSourceKind>& Kinds)
	{
		TArray<TSharedPtr<FJsonValue>> Values;
		for (auto Kind : Kinds) Values.Add(StringValue(UeAgentExportContract::FieldSourceKindToString(Kind)));
		return MakeShared<FJsonValueArray>(MoveTemp(Values));
	}

	FString NormalizeId(FString Value)
	{
		Value.TrimStartAndEndInline();
		Value.ToLowerInline();
		return Value;
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
		UE::UAI::Contract::FDiagnostic& OutError)
	{
		OutSegments.Reset();
		if (!JsonPointer.StartsWith(TEXT("/")))
		{
			OutError = ProjectionFailure(TEXT("json_pointer_must_start_with_slash"), JsonPointer, StringValue(TEXT("slash-prefixed JSON pointer")), StringValue(JsonPointer), TEXT("export_projection"), TEXT("The output JSON pointer must start with a slash."));
			return false;
		}
		TArray<FString> EncodedSegments;
		JsonPointer.Mid(1).ParseIntoArray(EncodedSegments, TEXT("/"), false);
		if (EncodedSegments.IsEmpty())
		{
			OutError = ProjectionFailure(TEXT("json_pointer_segments_missing"), JsonPointer, StringValue(TEXT("at least one path segment")), StringValue(JsonPointer), TEXT("export_projection"), TEXT("The output JSON pointer has no path segments."));
			return false;
		}
		for (const FString& Encoded : EncodedSegments)
		{
			FString Decoded;
			if (!DecodeJsonPointerSegment(Encoded, Decoded))
			{
				OutError = ProjectionFailure(TEXT("json_pointer_segment_invalid"), JsonPointer, StringValue(TEXT("nonempty segment with only ~0 or ~1 escapes")), StringValue(Encoded), TEXT("export_projection"), TEXT("The output JSON pointer contains an empty segment or invalid tilde escape."));
				return false;
			}
			if (!bAllowWildcards && Decoded == TEXT("*"))
			{
				OutError = ProjectionFailure(TEXT("json_pointer_unexpected_wildcard"), JsonPointer, StringValue(TEXT("concrete path segment")), StringValue(Decoded), TEXT("export_projection"), TEXT("A concrete output JSON pointer cannot contain a wildcard."));
				return false;
			}
			if (bAllowWildcards && Decoded.Contains(TEXT("*")) && Decoded != TEXT("*"))
			{
				OutError = ProjectionFailure(TEXT("json_pointer_wildcard_must_fill_segment"), JsonPointer, StringValue(TEXT("* as a complete segment")), StringValue(Decoded), TEXT("export_projection"), TEXT("A wildcard must occupy a complete JSON pointer segment."));
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
			Result += TEXT("/") + UE::UAI::HjsonWorkspace::EscapeJsonPointerSegment(Segments[Index]);
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
		UE::UAI::Contract::FDiagnostic& OutError)
	{
		FExtensionDescriptor Canonical = Descriptor;
		Canonical.ProjectionId = NormalizeId(Canonical.ProjectionId);
		Canonical.ImplementationId = NormalizeId(Canonical.ImplementationId);
		if (Canonical.ProjectionId.IsEmpty() || Canonical.ImplementationId.IsEmpty()
			|| Canonical.SourceKinds.IsEmpty() || !Extension)
		{
			const bool bProjectionMissing = Canonical.ProjectionId.IsEmpty();
			const bool bImplementationMissing = Canonical.ImplementationId.IsEmpty();
			const bool bKindsMissing = Canonical.SourceKinds.IsEmpty();
			OutError = ProjectionFailure(TEXT("export_compound_extension_descriptor_invalid"),
				bProjectionMissing ? TEXT("projection_id") : bImplementationMissing ? TEXT("implementation_id") : bKindsMissing ? TEXT("source_kinds") : TEXT("extension"),
				StringValue(bProjectionMissing || bImplementationMissing ? TEXT("nonempty identifier") : bKindsMissing ? TEXT("nonempty source kind array") : TEXT("callable projection extension")),
				bProjectionMissing ? StringValue(Descriptor.ProjectionId) : bImplementationMissing ? StringValue(Descriptor.ImplementationId)
				: bKindsMissing ? SourceKindsValue(Canonical.SourceKinds) : TSharedPtr<FJsonValue>(), TEXT("export_projection"),
				bProjectionMissing ? TEXT("The projection identifier is empty after normalization.") : bImplementationMissing ? TEXT("The extension implementation identifier is empty after normalization.")
				: bKindsMissing ? TEXT("The projection extension declares no supported source kinds.") : TEXT("The projection extension callback is missing."));
			OutError.CandidateName = Canonical.ProjectionId;
			if (!bProjectionMissing && !bImplementationMissing && !bKindsMissing) OutError.ActualValuePresent = false;
			return false;
		}
		TSet<UeAgentExportContract::EFieldSourceKind> UniqueKinds;
		for (int32 Index = 0; Index < Canonical.SourceKinds.Num(); ++Index)
		{
			const auto Kind = Canonical.SourceKinds[Index];
			if (UniqueKinds.Contains(Kind))
			{
				OutError = ProjectionFailure(TEXT("export_compound_extension_source_kind_duplicate"), TEXT("source_kinds/") + FString::FromInt(Index),
					StringValue(TEXT("unique source kind")), StringValue(UeAgentExportContract::FieldSourceKindToString(Kind)), TEXT("export_projection"), TEXT("The projection extension source kind duplicates an earlier entry."));
				OutError.CandidateName = Canonical.ProjectionId;
				return false;
			}
			UniqueKinds.Add(Kind);
		}

		FWriteScopeLock Lock(RegistryLock);
		if (const FEntry* Existing = Extensions.Find(Canonical.ProjectionId))
		{
			if (Existing->Descriptor != Canonical)
			{
				const bool bImplementationDiffers = Existing->Descriptor.ImplementationId != Canonical.ImplementationId;
				OutError = ProjectionFailure(TEXT("conflicting_export_compound_extension"),
					bImplementationDiffers ? TEXT("implementation_id") : TEXT("source_kinds"),
					bImplementationDiffers ? StringValue(Existing->Descriptor.ImplementationId) : SourceKindsValue(Existing->Descriptor.SourceKinds),
					bImplementationDiffers ? StringValue(Canonical.ImplementationId) : SourceKindsValue(Canonical.SourceKinds),
					TEXT("export_registration"), TEXT("The extension descriptor differs from the projection already registered with this identifier."));
				OutError.CandidateName = Canonical.ProjectionId;
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
		UE::UAI::Contract::FDiagnostic& OutError)
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
			OutError = ProjectionFailure(TEXT("json_pointer_wildcard_missing"), JsonPointerPattern,
				StringValue(TEXT("at least one wildcard segment")), MakeShared<FJsonValueNumber>(0), TEXT("export_projection"), TEXT("The compound projection pattern contains no wildcard segments."));
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
		UE::UAI::Contract::FDiagnostic& OutError)
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
				OutError = ProjectionFailure(TEXT("json_pointer_wildcard_value_invalid"), JsonPointerPattern,
					StringValue(TEXT("nonempty concrete wildcard replacement")), WildcardValues.IsValidIndex(WildcardIndex) ? StringValue(WildcardValues[WildcardIndex]) : nullptr,
					TEXT("export_projection"), FString::Printf(TEXT("Wildcard replacement %d is missing, empty, or still a wildcard."), WildcardIndex));
				OutError.ActualValuePresent = WildcardValues.IsValidIndex(WildcardIndex);
				return false;
			}
			Segment = WildcardValues[WildcardIndex++];
		}
		if (WildcardIndex != WildcardValues.Num())
		{
			OutError = ProjectionFailure(TEXT("json_pointer_wildcard_cardinality_mismatch"), JsonPointerPattern,
				MakeShared<FJsonValueNumber>(WildcardIndex), MakeShared<FJsonValueNumber>(WildcardValues.Num()), TEXT("export_projection"), TEXT("The wildcard replacement count differs from the number of wildcard segments."));
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
		UE::UAI::Contract::FDiagnostic Error;
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
