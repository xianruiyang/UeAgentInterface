// Copyright Epic Games, Inc. All Rights Reserved.

#include "Utilities/AuthoringExport/UeAgentSnapshotProjector.h"

#include "Core/Contract/UeAgentDiagnostic.h"
#include "Utilities/AuthoringCore/UeAgentAuthoringDiagnostics.h"

#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Utilities/AuthoringCore/UeAgentDetailsAuthoringService.h"
#include "Utilities/AuthoringExport/UeAgentCompoundProjectionExtension.h"

namespace
{
	using namespace UeAgentSnapshotProjector;
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

	void SetFailure(FProjectionResult& OutResult, const UE::UAI::Contract::FDiagnostic& Diagnostic,
        const FString& FileId = FString(), const FString& FieldId = FString())
    {
        OutResult.Status = TEXT("failed");
        OutResult.Diagnostic = Diagnostic;
        OutResult.FileId = FileId;
        OutResult.FieldId = FieldId;
        if (OutResult.Diagnostic.Source.SourceUnit.IsEmpty()) OutResult.Diagnostic.Source.SourceUnit = FileId;
        if (OutResult.Diagnostic.PropertyPath.IsEmpty()) OutResult.Diagnostic.PropertyPath = FieldId;
        if (OutResult.Diagnostic.CandidateName.IsEmpty()) OutResult.Diagnostic.CandidateName = FieldId;
    }

	int32 RolePriority(const FString& Role)
	{
		if (Role == TEXT("identity")) return 0;
		if (Role == TEXT("index")) return 1;
		if (Role == TEXT("authoring")) return 2;
		if (Role == TEXT("reference")) return 3;
		if (Role == TEXT("readonly")) return 4;
		if (Role == TEXT("readback")) return 5;
		if (Role == TEXT("diagnostics")) return 6;
		return 7;
	}

	TSharedPtr<FJsonValue> CloneValue(const TSharedPtr<FJsonValue>& Value)
	{
		return UeAgentAuthoringState::CloneCanonicalValue(Value);
	}

	void ApplyProfiledEnvelopePolicy(
		const TSharedPtr<FJsonValue>& Value,
		const UeAgentExportContract::FWriterPolicy& Policy,
		const FString& ProfileId,
		const bool bInsideCanonicalValue = false)
	{
		if (!Value.IsValid()) return;
		if (Value->Type == EJson::Array)
		{
			for (const TSharedPtr<FJsonValue>& Item : Value->AsArray())
			{
				ApplyProfiledEnvelopePolicy(Item, Policy, ProfileId, bInsideCanonicalValue);
			}
			return;
		}
		if (Value->Type != EJson::Object) return;

		const TSharedPtr<FJsonObject> Object = Value->AsObject();
		if (!Object.IsValid()) return;
		if (!bInsideCanonicalValue && !Policy.PreservesValueText(ProfileId))
		{
			const TSharedPtr<FJsonValue>* StructuredValue = Object->Values.Find(TEXT("value"));
			const bool bHasStructuredValue = StructuredValue && StructuredValue->IsValid()
				&& (*StructuredValue)->Type != EJson::None
				&& (*StructuredValue)->Type != EJson::Null;
			if (bHasStructuredValue) Object->RemoveField(TEXT("value_text"));
		}

		for (const TPair<FString, TSharedPtr<FJsonValue>>& Pair : Object->Values)
		{
			ApplyProfiledEnvelopePolicy(
				Pair.Value,
				Policy,
				ProfileId,
				bInsideCanonicalValue || Pair.Key.Equals(TEXT("value"), ESearchCase::IgnoreCase));
		}
	}

	TSharedPtr<FJsonValue> MakeProfiledEnvelopeValue(
		const UeAgentAuthoringState::FStateEntry& Entry,
		const UeAgentSnapshotProjector::FProjectorRequest& Request)
	{
		if (!Entry.bAvailable || !Entry.Value.IsValid()) return nullptr;
		TSharedPtr<FJsonValue> Value = CloneValue(Entry.Value);
		ApplyProfiledEnvelopePolicy(
			Value,
			Request.Contract->WriterPolicy,
			Request.Profile->ProfileId);
		return Value;
	}

	TSharedPtr<FJsonValue> MakeAvailabilityValue(const UeAgentAuthoringState::FStateEntry& Entry)
	{
		TSharedRef<FJsonObject> Object = MakeShared<FJsonObject>();
		Object->SetBoolField(TEXT("available"), Entry.bAvailable);
		Object->SetBoolField(TEXT("editable"), Entry.bEditable);
		if (!Entry.AvailabilityReason.IsEmpty()) Object->SetStringField(TEXT("reason"), Entry.AvailabilityReason);
		return MakeShared<FJsonValueObject>(Object);
	}

	TSharedPtr<FJsonValue> MakeConstraintsValue(const UeAgentAuthoringState::FStateEntry& Entry)
	{
		TSharedRef<FJsonObject> Object = MakeShared<FJsonObject>();
		TArray<FString> Keys;
		Entry.Constraints.GenerateKeyArray(Keys);
		Keys.Sort();
		for (const FString& Key : Keys)
		{
			Object->SetStringField(Key, Entry.Constraints.FindChecked(Key));
		}
		return MakeShared<FJsonValueObject>(Object);
	}

	TSharedPtr<FJsonValue> MakeDetailsValue(
		const UeAgentAuthoringState::FStateEntry& Entry,
		const FProjectorRequest& Request)
	{
		UeAgentDetailsAuthoringService::FProjectionPolicy Policy;
		Policy.Profile = Request.Profile->ProfileId;
		Policy.bRequireEditable = false;
		Policy.bPreserveValueText =
			Request.Contract->WriterPolicy.PreservesValueText(Request.Profile->ProfileId);
		const TSharedPtr<FJsonObject> Descriptor =
			UeAgentDetailsAuthoringService::BuildPropertyDescriptor(Entry, Policy);
		if (!Descriptor.IsValid()) return nullptr;
		for (const FString& RemovedField : Request.Contract->WriterPolicy.RemoveFields)
		{
			Descriptor->RemoveField(RemovedField);
		}
		return MakeShared<FJsonValueObject>(Descriptor);
	}

	TSharedPtr<FJsonValue> MakeEditablePropertyValue(
		const UeAgentAuthoringState::FStateEntry& Entry,
		const FProjectorRequest& Request,
		const bool bIncludeDetailsMetadata)
	{
		UeAgentDetailsAuthoringService::FProjectionPolicy Policy;
		Policy.Profile = Request.Profile->ProfileId;
		Policy.bIncludeDisplayName = bIncludeDetailsMetadata;
		Policy.bIncludeAvailability = bIncludeDetailsMetadata;
		Policy.bPreserveValueText =
			Request.Contract->WriterPolicy.PreservesValueText(Request.Profile->ProfileId);
		const TSharedPtr<FJsonObject> Descriptor =
			UeAgentDetailsAuthoringService::BuildPropertyDescriptor(
				Entry, Policy);
		if (!Descriptor.IsValid()) return nullptr;
		for (const FString& RemovedField : Request.Contract->WriterPolicy.RemoveFields)
		{
			Descriptor->RemoveField(RemovedField);
		}
		return MakeShared<FJsonValueObject>(Descriptor);
	}

	bool ProjectSingleEntry(
		const FProjectorRequest& Request,
		const TFunction<TSharedPtr<FJsonValue>(const UeAgentAuthoringState::FStateEntry&)>& Transform,
		FProjectedValue& OutValue,
		UE::UAI::Contract::FDiagnostic& OutError)
	{
		OutValue = FProjectedValue();
		if (Request.Entries.IsEmpty()) return true;
		if (Request.Entries.Num() != 1)
		{
			OutError = ProjectionFailure(TEXT("export_projection_cardinality_mismatch"), TEXT("snapshot_entries/length"),
				MakeShared<FJsonValueNumber>(1), MakeShared<FJsonValueNumber>(Request.Entries.Num()), TEXT("export_projection"), TEXT("A scalar projector requires exactly one matching snapshot entry."));
			if (Request.Field) OutError.CandidateName = Request.Field->FieldId;
			return false;
		}
		OutValue.Value = Transform(Request.Entries[0]);
		OutValue.bHasValue = OutValue.Value.IsValid();
		return true;
	}

	bool ProjectEntryList(
		const FProjectorRequest& Request,
		const TFunction<TSharedPtr<FJsonValue>(const UeAgentAuthoringState::FStateEntry&)>& Transform,
		FProjectedValue& OutValue,
		UE::UAI::Contract::FDiagnostic& OutError)
	{
		OutValue = FProjectedValue();
		if (Request.Entries.IsEmpty()) return true;
		TArray<TSharedPtr<FJsonValue>> Values;
		Values.Reserve(Request.Entries.Num());
		for (const UeAgentAuthoringState::FStateEntry& Entry : Request.Entries)
		{
			TSharedPtr<FJsonValue> Value = Transform(Entry);
			Values.Add(Value.IsValid() ? Value : MakeShared<FJsonValueNull>());
		}
		OutValue.Value = MakeShared<FJsonValueArray>(MoveTemp(Values));
		OutValue.bHasValue = true;
		return true;
	}

	bool DecodePointerSegment(const FString& Encoded, FString& OutDecoded)
	{
		OutDecoded.Reset();
		for (int32 Index = 0; Index < Encoded.Len(); ++Index)
		{
			const TCHAR Character = Encoded[Index];
			if (Character != TEXT('~'))
			{
				OutDecoded.AppendChar(Character);
				continue;
			}
			if (Index + 1 >= Encoded.Len()) return false;
			const TCHAR Escape = Encoded[++Index];
			if (Escape == TEXT('0')) OutDecoded.AppendChar(TEXT('~'));
			else if (Escape == TEXT('1')) OutDecoded.AppendChar(TEXT('/'));
			else return false;
		}
		return !OutDecoded.IsEmpty();
	}

	TSharedPtr<FJsonObject> CloneObject(const TSharedPtr<FJsonObject>& Object)
	{
		if (!Object.IsValid()) return nullptr;
		TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
		for (const TPair<FString, TSharedPtr<FJsonValue>>& Pair : Object->Values)
		{
			Result->SetField(Pair.Key, CloneValue(Pair.Value));
		}
		return Result;
	}

	bool SetJsonPointer(
		TSharedPtr<FJsonObject>& Root,
		const FString& JsonPointer,
		const TSharedPtr<FJsonValue>& Value,
		UE::UAI::Contract::FDiagnostic& OutError,
		bool bAllowEquivalentExisting = false)
	{
		if (!Value.IsValid())
		{
			OutError = ProjectionFailure(TEXT("export_projection_value_missing"), JsonPointer, StringValue(TEXT("JSON value")), nullptr,
				TEXT("export_projection"), TEXT("The output field has no projected JSON value."));
			OutError.ActualValuePresent = false;
			return false;
		}
		if (JsonPointer.IsEmpty())
		{
			const TSharedPtr<FJsonObject> ValueObject = Value->Type == EJson::Object
				? Value->AsObject()
				: nullptr;
			if (bAllowEquivalentExisting && ValueObject.IsValid() && ValueObject->Values.IsEmpty()
				&& Root.IsValid())
			{
				return true;
			}
			if (Value->Type != EJson::Object || (Root.IsValid() && !Root->Values.IsEmpty()))
			{
				OutError = ProjectionFailure(TEXT("export_projection_root_value_invalid"), JsonPointer,
					StringValue(Value->Type != EJson::Object ? TEXT("object") : TEXT("empty destination root")),
					Value->Type != EJson::Object ? Value : TSharedPtr<FJsonValue>(MakeShared<FJsonValueObject>(Root)), TEXT("export_projection"),
					Value->Type != EJson::Object ? TEXT("The output file root must be an object.") : TEXT("The output file root already contains projected fields."));
				return false;
			}
			Root = CloneObject(Value->AsObject());
			return Root.IsValid();
		}
		if (!JsonPointer.StartsWith(TEXT("/")) || JsonPointer.Contains(TEXT("*")))
		{
			OutError = ProjectionFailure(JsonPointer.Contains(TEXT("*")) ? TEXT("export_compound_projection_required") : TEXT("export_projection_json_pointer_invalid"),
				JsonPointer, StringValue(TEXT("concrete slash-prefixed JSON pointer")), StringValue(JsonPointer), TEXT("export_projection"),
				JsonPointer.Contains(TEXT("*")) ? TEXT("Wildcard output paths must be expanded by a compound projector before assignment.") : TEXT("The output JSON pointer must start with a slash."));
			return false;
		}

		TArray<FString> EncodedSegments;
		JsonPointer.Mid(1).ParseIntoArray(EncodedSegments, TEXT("/"), false);
		if (EncodedSegments.IsEmpty())
		{
			OutError = ProjectionFailure(TEXT("export_projection_json_pointer_invalid"), JsonPointer, StringValue(TEXT("nonempty path segments")), StringValue(JsonPointer),
				TEXT("export_projection"), TEXT("The output JSON pointer contains no path segments."));
			return false;
		}
		TArray<FString> Segments;
		for (const FString& Encoded : EncodedSegments)
		{
			FString Decoded;
			if (!DecodePointerSegment(Encoded, Decoded))
			{
				OutError = ProjectionFailure(TEXT("export_projection_json_pointer_escape_invalid"), JsonPointer, StringValue(TEXT("segment with only ~0 or ~1 escapes")), StringValue(Encoded),
					TEXT("export_projection"), TEXT("The output JSON pointer segment contains an invalid tilde escape."));
				return false;
			}
			Segments.Add(MoveTemp(Decoded));
		}

		if (!Root.IsValid()) Root = MakeShared<FJsonObject>();
		TSharedPtr<FJsonObject> Current = Root;
		FString TraversedPointer;
		for (int32 Index = 0; Index < Segments.Num() - 1; ++Index)
		{
			const FString& Segment = Segments[Index];
			TraversedPointer += TEXT("/") + EncodedSegments[Index];
			const TSharedPtr<FJsonValue> Existing = Current->TryGetField(Segment);
			if (!Existing.IsValid())
			{
				TSharedPtr<FJsonObject> Child = MakeShared<FJsonObject>();
				Current->SetObjectField(Segment, Child);
				Current = MoveTemp(Child);
			}
			else if (Existing->Type == EJson::Object)
			{
				Current = Existing->AsObject();
			}
			else
			{
				OutError = ProjectionFailure(TEXT("export_projection_path_collision"), TraversedPointer, StringValue(TEXT("object")), Existing,
					TEXT("export_projection"), TEXT("An intermediate output path is already occupied by a non-object value."));
				return false;
			}
		}
		const FString& Leaf = Segments.Last();
		if (Current->HasField(Leaf))
		{
			const TSharedPtr<FJsonValue> ExistingValue = Current->TryGetField(Leaf);
			const TSharedPtr<FJsonObject> ExistingObject = ExistingValue.IsValid()
				&& ExistingValue->Type == EJson::Object
				? ExistingValue->AsObject()
				: nullptr;
			const TSharedPtr<FJsonObject> ValueObject = Value->Type == EJson::Object
				? Value->AsObject()
				: nullptr;
			if (bAllowEquivalentExisting && ExistingObject.IsValid()
				&& ValueObject.IsValid() && ValueObject->Values.IsEmpty())
			{
				return true;
			}
			if (bAllowEquivalentExisting
				&& UeAgentAuthoringState::MakeStableCanonicalValueKey(ExistingValue)
					== UeAgentAuthoringState::MakeStableCanonicalValueKey(Value))
			{
				return true;
			}
			OutError = ProjectionFailure(TEXT("export_projection_path_collision"), JsonPointer, nullptr, ExistingValue,
				TEXT("export_projection"), TEXT("The output field path is already occupied by another projected value."));
			OutError.ExpectedValuePresent = false;
			OutError.ActualValuePresent = true;
			OutError.ProposedValue = Value;
			return false;
		}
		Current->SetField(Leaf, CloneValue(Value));
		return true;
	}

	bool IsNativeDefaultEquivalent(const TArray<UeAgentAuthoringState::FStateEntry>& Entries)
	{
		if (Entries.IsEmpty()) return false;
		for (const UeAgentAuthoringState::FStateEntry& Entry : Entries)
		{
			const FString* Marker = Entry.Constraints.Find(TEXT("native_default_equivalent"));
			if (!Marker || !Marker->Equals(TEXT("true"), ESearchCase::IgnoreCase)) return false;
		}
		return true;
	}

	FString ResolveProjectorId(const UeAgentExportContract::FFieldDescriptor& Field)
	{
		if (!Field.Source.ProjectorId.IsEmpty()) return Field.Source.ProjectorId;
		if (Field.Source.Kind != UeAgentExportContract::EFieldSourceKind::Semantic) return FString();
		return Field.Role == TEXT("identity")
			? FString(TEXT("uai.export.semantic.identity"))
			: FString(TEXT("uai.export.semantic.value"));
	}

	bool RegisterProjector(
		FProjectorRegistry& Registry,
		const FString& ProjectorId,
		const TArray<UeAgentExportContract::EFieldSourceKind>& SourceKinds,
		bool bListOutput,
		FProjector Projector,
		UE::UAI::Contract::FDiagnostic& OutError)
	{
		FProjectorDescriptor Descriptor;
		Descriptor.ProjectorId = ProjectorId;
		Descriptor.ImplementationId = ProjectorId + TEXT(".v1");
		Descriptor.SourceKinds = SourceKinds;
		Descriptor.bListOutput = bListOutput;
		ERegistrationResult Result;
		return Registry.RegisterProjector(Descriptor, MoveTemp(Projector), Result, OutError);
	}
}

namespace UeAgentSnapshotProjector
{
	bool FProjectorDescriptor::SupportsSourceKind(UeAgentExportContract::EFieldSourceKind Kind) const
	{
		return SourceKinds.Contains(Kind);
	}

	bool FProjectorDescriptor::operator==(const FProjectorDescriptor& Other) const
	{
		return ProjectorId == Other.ProjectorId && ImplementationId == Other.ImplementationId
			&& SourceKinds == Other.SourceKinds && bListOutput == Other.bListOutput;
	}

	FProjectorRegistry& FProjectorRegistry::Get()
	{
		static FProjectorRegistry Registry;
		static const bool bBuiltInsRegistered = []()
		{
			UE::UAI::Contract::FDiagnostic Error;
			return RegisterBuiltInProjectors(Registry, Error);
		}();
		(void)bBuiltInsRegistered;
		return Registry;
	}

	bool FProjectorRegistry::RegisterProjector(
		const FProjectorDescriptor& Descriptor,
		FProjector Projector,
		ERegistrationResult& OutResult,
		UE::UAI::Contract::FDiagnostic& OutError)
	{
		FProjectorDescriptor Canonical = Descriptor;
		Canonical.ProjectorId = NormalizeId(Canonical.ProjectorId);
		Canonical.ImplementationId = NormalizeId(Canonical.ImplementationId);
		if (Canonical.ProjectorId.IsEmpty() || Canonical.ImplementationId.IsEmpty()
			|| Canonical.SourceKinds.IsEmpty() || !Projector)
		{
			const bool bIdMissing = Canonical.ProjectorId.IsEmpty();
			const bool bImplementationMissing = Canonical.ImplementationId.IsEmpty();
			const bool bKindsMissing = Canonical.SourceKinds.IsEmpty();
			OutError = ProjectionFailure(TEXT("export_projector_descriptor_invalid"),
				bIdMissing ? TEXT("projector_id") : bImplementationMissing ? TEXT("implementation_id") : bKindsMissing ? TEXT("source_kinds") : TEXT("projector"),
				StringValue(bIdMissing || bImplementationMissing ? TEXT("nonempty identifier") : bKindsMissing ? TEXT("nonempty source kind array") : TEXT("callable projector")),
				bIdMissing ? StringValue(Descriptor.ProjectorId) : bImplementationMissing ? StringValue(Descriptor.ImplementationId) : bKindsMissing ? SourceKindsValue(Canonical.SourceKinds) : TSharedPtr<FJsonValue>(),
				TEXT("export_projection"), bIdMissing ? TEXT("The projector identifier is empty after normalization.") : bImplementationMissing ? TEXT("The projector implementation identifier is empty after normalization.")
				: bKindsMissing ? TEXT("The projector declares no supported source kinds.") : TEXT("The projector callback is missing."));
			OutError.CandidateName = Canonical.ProjectorId;
			if (!bIdMissing && !bImplementationMissing && !bKindsMissing) OutError.ActualValuePresent = false;
			return false;
		}
		TSet<UeAgentExportContract::EFieldSourceKind> UniqueKinds;
		for (int32 Index = 0; Index < Canonical.SourceKinds.Num(); ++Index)
		{
			const auto Kind = Canonical.SourceKinds[Index];
			if (UniqueKinds.Contains(Kind))
			{
				OutError = ProjectionFailure(TEXT("export_projector_source_kind_duplicate"), TEXT("source_kinds/") + FString::FromInt(Index), StringValue(TEXT("unique source kind")),
					StringValue(UeAgentExportContract::FieldSourceKindToString(Kind)), TEXT("export_projection"), TEXT("The projector source kind duplicates an earlier entry."));
				OutError.CandidateName = Canonical.ProjectorId;
				return false;
			}
			UniqueKinds.Add(Kind);
		}

		FWriteScopeLock Lock(RegistryLock);
		if (const FEntry* Existing = Projectors.Find(Canonical.ProjectorId))
		{
			if (Existing->Descriptor != Canonical)
			{
				const auto& Before = Existing->Descriptor;
				const bool bImplementationDiffers = Before.ImplementationId != Canonical.ImplementationId;
				const bool bKindsDiffer = Before.SourceKinds != Canonical.SourceKinds;
				OutError = ProjectionFailure(TEXT("conflicting_export_projector"), bImplementationDiffers ? TEXT("implementation_id") : bKindsDiffer ? TEXT("source_kinds") : TEXT("list_output"),
					bImplementationDiffers ? StringValue(Before.ImplementationId) : bKindsDiffer ? SourceKindsValue(Before.SourceKinds) : TSharedPtr<FJsonValue>(MakeShared<FJsonValueBoolean>(Before.bListOutput)),
					bImplementationDiffers ? StringValue(Canonical.ImplementationId) : bKindsDiffer ? SourceKindsValue(Canonical.SourceKinds) : TSharedPtr<FJsonValue>(MakeShared<FJsonValueBoolean>(Canonical.bListOutput)),
					TEXT("export_contract"), TEXT("The projector descriptor differs from the projector already registered with this identifier."));
				OutError.CandidateName = Canonical.ProjectorId;
				return false;
			}
			OutResult = ERegistrationResult::AlreadyRegistered;
			OutError.Reset();
			return true;
		}
		FEntry Entry;
		Entry.Descriptor = MoveTemp(Canonical);
		Entry.Projector = MoveTemp(Projector);
		Projectors.Add(Entry.Descriptor.ProjectorId, MoveTemp(Entry));
		OutResult = ERegistrationResult::Registered;
		OutError.Reset();
		return true;
	}

	bool FProjectorRegistry::FindProjector(const FString& ProjectorId, FProjectorDescriptor& OutDescriptor) const
	{
		FReadScopeLock Lock(RegistryLock);
		if (const FEntry* Found = Projectors.Find(NormalizeId(ProjectorId)))
		{
			OutDescriptor = Found->Descriptor;
			return true;
		}
		return false;
	}

	bool FProjectorRegistry::CopyProjector(const FString& ProjectorId, FProjector& OutProjector) const
	{
		FReadScopeLock Lock(RegistryLock);
		if (const FEntry* Found = Projectors.Find(NormalizeId(ProjectorId)))
		{
			OutProjector = Found->Projector;
			return true;
		}
		OutProjector = FProjector();
		return false;
	}

	TArray<FProjectorDescriptor> FProjectorRegistry::ListProjectors() const
	{
		FReadScopeLock Lock(RegistryLock);
		TArray<FProjectorDescriptor> Result;
		for (const TPair<FString, FEntry>& Pair : Projectors) Result.Add(Pair.Value.Descriptor);
		Result.Sort([](const FProjectorDescriptor& Left, const FProjectorDescriptor& Right)
		{
			return Left.ProjectorId < Right.ProjectorId;
		});
		return Result;
	}

	int32 FProjectorRegistry::GetProjectorCount() const
	{
		FReadScopeLock Lock(RegistryLock);
		return Projectors.Num();
	}

	bool FProjectionResult::IsProjected() const
	{
		return Status == TEXT("projected") && Diagnostic.IsEmpty();
	}

	TSharedRef<FJsonObject> FProjectionResult::ToJsonObject() const
	{
		TSharedRef<FJsonObject> Result = MakeShared<FJsonObject>();
		Result->SetStringField(TEXT("schema"), TEXT("uai.export_projection.v1"));
		Result->SetStringField(TEXT("status"), Status);
		Result->SetStringField(TEXT("contract_id"), ContractId);
		Result->SetStringField(TEXT("contract_version"), ContractVersion);
		Result->SetStringField(TEXT("profile_id"), ProfileId);
		if (!Diagnostic.IsEmpty())
		{
			Result->SetObjectField(TEXT("diagnostic"), Diagnostic.ToPublicJson());
			if (!FileId.IsEmpty()) Result->SetStringField(TEXT("file_id"), FileId);
			if (!FieldId.IsEmpty()) Result->SetStringField(TEXT("field_id"), FieldId);
		}
		TArray<TSharedPtr<FJsonValue>> FileValues;
		for (const FProjectedFile& File : Files)
		{
			TSharedRef<FJsonObject> FileObject = MakeShared<FJsonObject>();
			FileObject->SetStringField(TEXT("file_id"), File.FileId);
			FileObject->SetStringField(TEXT("path"), File.Path);
			FileObject->SetStringField(TEXT("role"), File.Role);
			FileObject->SetBoolField(TEXT("required"), File.bRequired);
			FileObject->SetObjectField(TEXT("document"), File.Root.IsValid() ? File.Root : MakeShared<FJsonObject>());
			FileValues.Add(MakeShared<FJsonValueObject>(FileObject));
		}
		Result->SetArrayField(TEXT("files"), FileValues);
		return Result;
	}

	bool BuildProjectedFileRoot(
		const FProjectedFile& File,
		TSharedPtr<FJsonObject>& OutRoot,
		UE::UAI::Contract::FDiagnostic& OutError)
	{
		OutRoot = MakeShared<FJsonObject>();
		OutError.Reset();
		for (const FProjectedField& Field : File.Fields)
		{
			if (!Field.Value.IsValid())
			{
				OutError = ProjectionFailure(TEXT("export_projected_field_value_invalid"), Field.JsonPointer, StringValue(TEXT("JSON value")), nullptr,
					TEXT("export_projection"), TEXT("The projected field has no JSON value."));
				OutError.CandidateName = Field.FieldId;
				OutError.TargetPath = File.Path;
				OutError.ActualValuePresent = false;
				OutRoot.Reset();
				return false;
			}
			if (!SetJsonPointer(
				OutRoot,
				Field.JsonPointer,
				CloneValue(Field.Value),
				OutError,
				Field.bCompoundContainerFallback))
			{
				if (OutError.PropertyPath.IsEmpty()) OutError.PropertyPath = Field.JsonPointer;
                if (OutError.CandidateName.IsEmpty()) OutError.CandidateName = Field.FieldId;
				if (OutError.TargetPath.IsEmpty()) OutError.TargetPath = File.Path;
				OutRoot.Reset();
				return false;
			}
		}
		return true;
	}

	bool RegisterBuiltInProjectors(FProjectorRegistry& Registry, UE::UAI::Contract::FDiagnostic& OutError)
	{
		using UeAgentExportContract::EFieldSourceKind;
		if (!RegisterProjector(Registry, TEXT("uai.export.semantic.value"), { EFieldSourceKind::Semantic }, false,
			[](const FProjectorRequest& Request, FProjectedValue& OutValue, UE::UAI::Contract::FDiagnostic& OutProjectorError)
			{
				return ProjectSingleEntry(Request, [](const UeAgentAuthoringState::FStateEntry& Entry)
				{
					return Entry.bAvailable && Entry.Value.IsValid() ? CloneValue(Entry.Value) : nullptr;
				}, OutValue, OutProjectorError);
			}, OutError)) return false;
		if (!RegisterProjector(Registry, TEXT("uai.export.semantic.values"), { EFieldSourceKind::Semantic }, true,
			[](const FProjectorRequest& Request, FProjectedValue& OutValue, UE::UAI::Contract::FDiagnostic& OutProjectorError)
			{
				return ProjectEntryList(Request, [](const UeAgentAuthoringState::FStateEntry& Entry)
				{
					return Entry.bAvailable && Entry.Value.IsValid() ? CloneValue(Entry.Value) : nullptr;
				}, OutValue, OutProjectorError);
			}, OutError)) return false;
		if (!RegisterProjector(Registry, TEXT("uai.export.semantic.profiled_value"), { EFieldSourceKind::Semantic }, false,
			[](const FProjectorRequest& Request, FProjectedValue& OutValue, UE::UAI::Contract::FDiagnostic& OutProjectorError)
			{
				return ProjectSingleEntry(Request, [&Request](const UeAgentAuthoringState::FStateEntry& Entry)
				{
					return MakeProfiledEnvelopeValue(Entry, Request);
				}, OutValue, OutProjectorError);
			}, OutError)) return false;
		if (!RegisterProjector(Registry, TEXT("uai.export.semantic.profiled_values"), { EFieldSourceKind::Semantic }, true,
			[](const FProjectorRequest& Request, FProjectedValue& OutValue, UE::UAI::Contract::FDiagnostic& OutProjectorError)
			{
				return ProjectEntryList(Request, [&Request](const UeAgentAuthoringState::FStateEntry& Entry)
				{
					return MakeProfiledEnvelopeValue(Entry, Request);
				}, OutValue, OutProjectorError);
			}, OutError)) return false;
		if (!RegisterProjector(Registry, TEXT("uai.export.semantic.identity"), { EFieldSourceKind::Semantic }, false,
			[](const FProjectorRequest& Request, FProjectedValue& OutValue, UE::UAI::Contract::FDiagnostic& OutProjectorError)
			{
				return ProjectSingleEntry(Request, [](const UeAgentAuthoringState::FStateEntry& Entry)
				{
					return MakeShared<FJsonValueObject>(Entry.TargetIdentity.ToJsonObject());
				}, OutValue, OutProjectorError);
			}, OutError)) return false;
		if (!RegisterProjector(Registry, TEXT("uai.export.semantic.identities"), { EFieldSourceKind::Semantic }, true,
			[](const FProjectorRequest& Request, FProjectedValue& OutValue, UE::UAI::Contract::FDiagnostic& OutProjectorError)
			{
				return ProjectEntryList(Request, [](const UeAgentAuthoringState::FStateEntry& Entry)
				{
					return MakeShared<FJsonValueObject>(Entry.TargetIdentity.ToJsonObject());
				}, OutValue, OutProjectorError);
			}, OutError)) return false;
		if (!RegisterProjector(Registry, TEXT("uai.export.semantic.availability"), { EFieldSourceKind::Semantic }, false,
			[](const FProjectorRequest& Request, FProjectedValue& OutValue, UE::UAI::Contract::FDiagnostic& OutProjectorError)
			{
				return ProjectSingleEntry(Request, MakeAvailabilityValue, OutValue, OutProjectorError);
			}, OutError)) return false;
		if (!RegisterProjector(Registry, TEXT("uai.export.semantic.availabilities"), { EFieldSourceKind::Semantic }, true,
			[](const FProjectorRequest& Request, FProjectedValue& OutValue, UE::UAI::Contract::FDiagnostic& OutProjectorError)
			{
				return ProjectEntryList(Request, MakeAvailabilityValue, OutValue, OutProjectorError);
			}, OutError)) return false;
		if (!RegisterProjector(Registry, TEXT("uai.export.semantic.constraints"), { EFieldSourceKind::Semantic }, false,
			[](const FProjectorRequest& Request, FProjectedValue& OutValue, UE::UAI::Contract::FDiagnostic& OutProjectorError)
			{
				return ProjectSingleEntry(Request, MakeConstraintsValue, OutValue, OutProjectorError);
			}, OutError)) return false;
		if (!RegisterProjector(Registry, TEXT("uai.export.semantic.constraints_list"), { EFieldSourceKind::Semantic }, true,
			[](const FProjectorRequest& Request, FProjectedValue& OutValue, UE::UAI::Contract::FDiagnostic& OutProjectorError)
			{
				return ProjectEntryList(Request, MakeConstraintsValue, OutValue, OutProjectorError);
			}, OutError)) return false;
		if (!RegisterProjector(Registry, TEXT("uai.export.semantic.details"), { EFieldSourceKind::Semantic }, true,
			[](const FProjectorRequest& Request, FProjectedValue& OutValue, UE::UAI::Contract::FDiagnostic& OutProjectorError)
			{
				FProjectorRequest Filtered = Request;
				if (Request.Field->bEditable)
				{
					Filtered.Entries.RemoveAll([](const UeAgentAuthoringState::FStateEntry& Entry)
					{
						return !Entry.bAvailable || !Entry.bEditable;
					});
				}
				return ProjectEntryList(Filtered, [&Request](const UeAgentAuthoringState::FStateEntry& Entry)
				{
					return MakeDetailsValue(Entry, Request);
				}, OutValue, OutProjectorError);
			}, OutError)) return false;
		if (!RegisterProjector(Registry, TEXT("uai.export.semantic.editable_property"), { EFieldSourceKind::Semantic }, false,
			[](const FProjectorRequest& Request, FProjectedValue& OutValue, UE::UAI::Contract::FDiagnostic& OutProjectorError)
			{
				return ProjectSingleEntry(Request, [&Request](const UeAgentAuthoringState::FStateEntry& Entry)
				{
					return MakeEditablePropertyValue(Entry, Request, false);
				}, OutValue, OutProjectorError);
			}, OutError)) return false;
		if (!RegisterProjector(Registry, TEXT("uai.export.semantic.editable_properties"), { EFieldSourceKind::Semantic }, true,
			[](const FProjectorRequest& Request, FProjectedValue& OutValue, UE::UAI::Contract::FDiagnostic& OutProjectorError)
			{
				FProjectorRequest Filtered = Request;
				Filtered.Entries.RemoveAll([](const UeAgentAuthoringState::FStateEntry& Entry)
				{
					return !Entry.bAvailable || !Entry.bEditable || !Entry.Value.IsValid()
						|| Entry.TargetIdentity.GetSelector(TEXT("property_path")).IsEmpty();
				});
				return ProjectEntryList(Filtered, [&Request](const UeAgentAuthoringState::FStateEntry& Entry)
				{
					return MakeEditablePropertyValue(Entry, Request, true);
				}, OutValue, OutProjectorError);
			}, OutError)) return false;
		if (!RegisterProjector(Registry, TEXT("uai.export.semantic.property_document_schema"), { EFieldSourceKind::Semantic }, false,
			[](const FProjectorRequest& Request, FProjectedValue& OutValue, UE::UAI::Contract::FDiagnostic& OutProjectorError)
			{
				OutValue = FProjectedValue();
				OutProjectorError.Reset();
				if (Request.Entries.IsEmpty()) return true;
				OutValue.Value = MakeShared<FJsonValueString>(TEXT("uai.uobject_properties.v2"));
				OutValue.bHasValue = true;
				return true;
			}, OutError)) return false;
		if (!RegisterProjector(Registry, TEXT("uai.export.semantic.property_document_profile"), { EFieldSourceKind::Semantic }, false,
			[](const FProjectorRequest& Request, FProjectedValue& OutValue, UE::UAI::Contract::FDiagnostic& OutProjectorError)
			{
				OutValue = FProjectedValue();
				OutProjectorError.Reset();
				if (Request.Entries.IsEmpty()) return true;
				OutValue.Value = MakeShared<FJsonValueString>(TEXT("authoring"));
				OutValue.bHasValue = true;
				return true;
			}, OutError)) return false;
		if (!RegisterProjector(Registry, TEXT("uai.export.format_version"), { EFieldSourceKind::FormatConstant }, false,
			[](const FProjectorRequest& Request, FProjectedValue& OutValue, UE::UAI::Contract::FDiagnostic& OutProjectorError)
			{
				OutValue.bHasValue = true;
				OutValue.Value = MakeShared<FJsonValueString>(Request.Contract->ContractVersion);
				return true;
			}, OutError)) return false;
		if (!RegisterProjector(Registry, TEXT("uai.export.format.operation_update"), { EFieldSourceKind::FormatConstant }, false,
			[](const FProjectorRequest& Request, FProjectedValue& OutValue, UE::UAI::Contract::FDiagnostic& OutProjectorError)
			{
				OutValue.bHasValue = true;
				OutValue.Value = MakeShared<FJsonValueString>(TEXT("update"));
				return true;
			}, OutError)) return false;
		if (!RegisterProjector(Registry, TEXT("uai.export.format.operation_replace"), { EFieldSourceKind::FormatConstant }, false,
			[](const FProjectorRequest& Request, FProjectedValue& OutValue, UE::UAI::Contract::FDiagnostic& OutProjectorError)
			{
				OutValue.bHasValue = true;
				OutValue.Value = MakeShared<FJsonValueString>(TEXT("replace"));
				return true;
			}, OutError)) return false;
		if (!RegisterProjector(Registry, TEXT("uai.export.navigation.canonical_keys"), { EFieldSourceKind::DerivedNavigation }, true,
			[](const FProjectorRequest& Request, FProjectedValue& OutValue, UE::UAI::Contract::FDiagnostic& OutProjectorError)
			{
				TSet<FString> UniqueKeys;
				for (const UeAgentAuthoringState::FStateEntry& Entry : Request.Entries)
				{
					UniqueKeys.Add(Entry.TargetIdentity.CanonicalKey);
				}
				TArray<FString> Keys = UniqueKeys.Array();
				Keys.Sort();
				if (Keys.IsEmpty()) return true;
				TArray<TSharedPtr<FJsonValue>> Values;
				for (const FString& Key : Keys) Values.Add(MakeShared<FJsonValueString>(Key));
				OutValue.bHasValue = true;
				OutValue.Value = MakeShared<FJsonValueArray>(MoveTemp(Values));
				return true;
			}, OutError)) return false;
		OutError.Reset();
		return true;
	}

	bool ProjectSnapshot(
		const UeAgentExportContract::FContract& Contract,
		const FString& ProfileId,
		const UeAgentAuthoringState::FStateSnapshot& Snapshot,
		FProjectionResult& OutResult,
		const FProjectorRegistry* ProjectorRegistry,
		const UeAgentCompoundProjection::FRegistry* CompoundProjectionRegistry)
	{
		OutResult = FProjectionResult();
		OutResult.Status = TEXT("failed");
		OutResult.ContractId = Contract.ContractId;
		OutResult.ContractVersion = Contract.ContractVersion;
		OutResult.ProfileId = NormalizeId(ProfileId.IsEmpty() ? Contract.DefaultProfile : ProfileId);
		UE::UAI::Contract::FDiagnostic ContractError;
		if (!UeAgentExportContract::ValidateContract(Contract, ContractError))
		{
			SetFailure(OutResult, ContractError);
			return false;
		}
		const UeAgentExportContract::FProfileDescriptor* Profile = Contract.FindProfile(OutResult.ProfileId);
		if (!Profile)
		{
			TArray<TSharedPtr<FJsonValue>> Profiles;
			for (const auto& Available : Contract.Profiles) Profiles.Add(StringValue(Available.ProfileId));
			SetFailure(OutResult, ProjectionFailure(TEXT("export_profile_not_found"), TEXT("profile"), MakeShared<FJsonValueArray>(MoveTemp(Profiles)), StringValue(OutResult.ProfileId),
				TEXT("export_projection"), TEXT("The export contract does not declare the requested profile.")));
			return false;
		}
		const FProjectorRegistry& Registry = ProjectorRegistry ? *ProjectorRegistry : FProjectorRegistry::Get();
		const UeAgentCompoundProjection::FRegistry& CompoundRegistry = CompoundProjectionRegistry
			? *CompoundProjectionRegistry
			: UeAgentCompoundProjection::FRegistry::Get();

		TArray<const UeAgentExportContract::FFileDescriptor*> ActiveFiles;
		for (const UeAgentExportContract::FFileDescriptor& File : Contract.Files)
		{
			if (File.IsEnabledForProfile(Profile->ProfileId)) ActiveFiles.Add(&File);
		}
		ActiveFiles.Sort([](const UeAgentExportContract::FFileDescriptor& Left, const UeAgentExportContract::FFileDescriptor& Right)
		{
			const int32 LeftPriority = RolePriority(Left.Role);
			const int32 RightPriority = RolePriority(Right.Role);
			if (LeftPriority != RightPriority) return LeftPriority < RightPriority;
			if (Left.Path != Right.Path) return Left.Path < Right.Path;
			return Left.FileId < Right.FileId;
		});
		for (const UeAgentExportContract::FFileDescriptor* File : ActiveFiles)
		{
			FProjectedFile& Projected = OutResult.Files.AddDefaulted_GetRef();
			Projected.FileId = File->FileId;
			Projected.Path = File->Path;
			Projected.Role = File->Role;
			Projected.bRequired = File->bRequired;
			Projected.Root = MakeShared<FJsonObject>();
		}

		TArray<const UeAgentExportContract::FFieldDescriptor*> ActiveFields;
		for (const UeAgentExportContract::FFieldDescriptor& Field : Contract.Fields)
		{
			if (Profile->IncludesRole(Field.Role)) ActiveFields.Add(&Field);
		}
		ActiveFields.Sort([](const UeAgentExportContract::FFieldDescriptor& Left, const UeAgentExportContract::FFieldDescriptor& Right)
		{
			const int32 LeftPriority = RolePriority(Left.Role);
			const int32 RightPriority = RolePriority(Right.Role);
			if (LeftPriority != RightPriority) return LeftPriority < RightPriority;
			if (Left.OutputPath != Right.OutputPath) return Left.OutputPath < Right.OutputPath;
			return Left.FieldId < Right.FieldId;
		});

		const TArray<UeAgentAuthoringState::FStateEntry> SnapshotEntries = Snapshot.CopyEntries();
		for (const UeAgentExportContract::FFieldDescriptor* Field : ActiveFields)
		{
			FString FilePath;
			FString JsonPointer;
			if (!UeAgentExportContract::SplitOutputPath(Field->OutputPath, FilePath, JsonPointer))
			{
				SetFailure(OutResult, ProjectionFailure(TEXT("export_field_output_path_invalid"), TEXT("output_path"), StringValue(TEXT("file.hjson#/json/pointer")), StringValue(Field->OutputPath),
					TEXT("export_projection"), TEXT("The export field output path cannot be split into a file and JSON pointer.")), FString(), Field->FieldId);
				return false;
			}
			FProjectedFile* ProjectedFile = OutResult.Files.FindByPredicate([&FilePath](const FProjectedFile& File)
			{
				return File.Path == FilePath;
			});
			if (!ProjectedFile) continue;
			const UeAgentExportContract::FFileDescriptor* ContractFile = Contract.FindFile(ProjectedFile->FileId);
			if (!ContractFile)
			{
				SetFailure(OutResult, ProjectionFailure(TEXT("export_projected_file_contract_missing"), TEXT("file_id"), StringValue(TEXT("declared export file identifier")), StringValue(ProjectedFile->FileId),
					TEXT("export_projection"), TEXT("The projected file has no matching file descriptor in the export contract.")));
				return false;
			}

			const FString ProjectorId = ResolveProjectorId(*Field);
			FProjectorDescriptor ProjectorDescriptor;
			FProjector Projector;
			const bool bProjectorFound = !ProjectorId.IsEmpty() && Registry.FindProjector(ProjectorId, ProjectorDescriptor);
			const bool bProjectorCallable = bProjectorFound && Registry.CopyProjector(ProjectorId, Projector);
			if (!bProjectorCallable || !ProjectorDescriptor.SupportsSourceKind(Field->Source.Kind))
			{
				auto Error = ProjectionFailure(TEXT("export_projector_not_found_or_incompatible"), TEXT("source/kind"),
					bProjectorFound ? SourceKindsValue(ProjectorDescriptor.SourceKinds) : StringValue(TEXT("registered projector")), StringValue(UeAgentExportContract::FieldSourceKindToString(Field->Source.Kind)), TEXT("export_projection"),
					ProjectorId.IsEmpty() ? TEXT("The export field does not resolve to a projector identifier.") : !bProjectorFound ? TEXT("The requested export projector is not registered.")
					: !bProjectorCallable ? TEXT("The registered export projector callback is unavailable.") : TEXT("The export projector does not support this field source kind."));
				Error.CandidateName = ProjectorId; Error.TargetPath = FilePath;
				SetFailure(OutResult, Error, ProjectedFile->FileId, Field->FieldId);
				return false;
			}

			FProjectorRequest Request;
			Request.Contract = &Contract;
			Request.Profile = Profile;
			Request.Field = Field;
			if (Field->Source.Kind == UeAgentExportContract::EFieldSourceKind::Semantic)
			{
				for (const UeAgentAuthoringState::FStateEntry& Entry : SnapshotEntries)
				{
					if (Entry.SemanticId == Field->Source.SemanticId) Request.Entries.Add(Entry);
				}
			}
			else if (Field->Source.Kind == UeAgentExportContract::EFieldSourceKind::DerivedNavigation)
			{
				Request.Entries = SnapshotEntries;
			}

			auto ProjectRequest = [&Field, &Projector, &ProjectorId](
				const FProjectorRequest& ValueRequest,
				FProjectedValue& OutValue,
				UE::UAI::Contract::FDiagnostic& OutProjectorError)
			{
				if (Field->DefaultPolicy == TEXT("omit_if_native_default")
					&& IsNativeDefaultEquivalent(ValueRequest.Entries))
				{
					OutValue = FProjectedValue();
					OutProjectorError.Reset();
					return true;
				}
				const bool bProjected = Projector(ValueRequest, OutValue, OutProjectorError);
				if (!bProjected && OutProjectorError.IsEmpty()) {
					OutProjectorError = UE::UAI::Contract::FDiagnostic::Failure(TEXT("export_projector_failed_without_diagnostic"), TEXT("The export projector returned failure without a diagnostic."), TEXT("export_projection"));
					OutProjectorError.CandidateName = ProjectorId;
					OutProjectorError.SourceKind = TEXT("native_result");
				}
				return bProjected;
			};
			auto AddProjectedField = [&Field, ProjectedFile](
				const FString& InstanceId,
				const FString& ConcretePointer,
				const TSharedPtr<FJsonValue>& Value,
				bool bCompoundContainerFallback)
			{
				FProjectedField& ProjectedField = ProjectedFile->Fields.AddDefaulted_GetRef();
				ProjectedField.FieldId = InstanceId;
				ProjectedField.ContractFieldId = Field->FieldId;
				ProjectedField.JsonPointer = ConcretePointer;
				ProjectedField.Role = Field->Role;
				ProjectedField.bEditable = Field->bEditable;
				ProjectedField.bCompoundContainerFallback = bCompoundContainerFallback;
				ProjectedField.Value = CloneValue(Value);
			};
			auto ApplyEmptyPolicy = [&Field, &ProjectorDescriptor](FProjectedValue& InOutValue)
			{
				if (InOutValue.bHasValue) return;
				InOutValue.bHasValue = true;
				if (Field->EmptyPolicy == TEXT("emit_null"))
				{
					InOutValue.Value = MakeShared<FJsonValueNull>();
				}
				else if (ProjectorDescriptor.bListOutput)
				{
					InOutValue.Value = MakeShared<FJsonValueArray>(TArray<TSharedPtr<FJsonValue>>());
				}
				else
				{
					InOutValue.Value = MakeShared<FJsonValueObject>(MakeShared<FJsonObject>());
				}
			};

			if (!JsonPointer.Contains(TEXT("*")))
			{
				FProjectedValue ProjectedValue;
				UE::UAI::Contract::FDiagnostic ProjectorError;
				if (!ProjectRequest(Request, ProjectedValue, ProjectorError))
				{
					if (ProjectorError.PropertyPath.IsEmpty()) ProjectorError.PropertyPath = JsonPointer;
					if (ProjectorError.TargetPath.IsEmpty()) ProjectorError.TargetPath = FilePath;
					SetFailure(OutResult, ProjectorError, ProjectedFile->FileId, Field->FieldId);
					return false;
				}
				if (!ProjectedValue.bHasValue && Field->EmptyPolicy == TEXT("omit")) continue;
				ApplyEmptyPolicy(ProjectedValue);
				AddProjectedField(Field->FieldId, JsonPointer, ProjectedValue.Value, false);
				continue;
			}

			UeAgentCompoundProjection::FJsonPointerPatternInfo PatternInfo;
			UE::UAI::Contract::FDiagnostic CompoundError;
			if (!UeAgentCompoundProjection::InspectJsonPointerPattern(JsonPointer, PatternInfo, CompoundError))
			{
				if (CompoundError.PropertyPath.IsEmpty()) CompoundError.PropertyPath = JsonPointer;
				if (CompoundError.TargetPath.IsEmpty()) CompoundError.TargetPath = FilePath;
				SetFailure(OutResult, CompoundError, ProjectedFile->FileId, Field->FieldId);
				return false;
			}
			UeAgentCompoundProjection::FExtensionDescriptor ExtensionDescriptor;
			UeAgentCompoundProjection::FExtension Extension;
			const bool bExtensionFound = CompoundRegistry.FindExtension(ContractFile->ProjectionId, ExtensionDescriptor);
			const bool bExtensionCallable = bExtensionFound && CompoundRegistry.CopyExtension(ContractFile->ProjectionId, Extension);
			if (!bExtensionCallable || !ExtensionDescriptor.SupportsSourceKind(Field->Source.Kind))
			{
				auto Error = ProjectionFailure(TEXT("export_compound_projection_required"), JsonPointer,
					bExtensionFound ? SourceKindsValue(ExtensionDescriptor.SourceKinds) : StringValue(TEXT("registered compound projection")), StringValue(UeAgentExportContract::FieldSourceKindToString(Field->Source.Kind)), TEXT("export_projection"),
					!bExtensionFound ? TEXT("The compound projection extension is not registered.") : !bExtensionCallable ? TEXT("The compound projection callback is unavailable.") : TEXT("The compound projection does not support this field source kind."));
				Error.CandidateName = ContractFile->ProjectionId; Error.TargetPath = FilePath;
				SetFailure(OutResult, Error, ProjectedFile->FileId, Field->FieldId);
				return false;
			}

			UeAgentCompoundProjection::FRequest CompoundRequest;
			CompoundRequest.Contract = &Contract;
			CompoundRequest.Profile = Profile;
			CompoundRequest.File = ContractFile;
			CompoundRequest.Field = Field;
			CompoundRequest.JsonPointerPattern = JsonPointer;
			CompoundRequest.WildcardCount = PatternInfo.WildcardCount;
			CompoundRequest.Entries = Request.Entries;
			TArray<UeAgentCompoundProjection::FExpansionSelection> Selections;
			CompoundError.Reset();
			if (!Extension(CompoundRequest, Selections, CompoundError))
			{
				if (CompoundError.IsEmpty()) {
					CompoundError = UE::UAI::Contract::FDiagnostic::Failure(TEXT("export_compound_failed_without_diagnostic"), TEXT("The compound projection extension returned failure without a diagnostic."), TEXT("export_projection"));
					CompoundError.CandidateName = ContractFile->ProjectionId;
					CompoundError.SourceKind = TEXT("native_result");
				}
				if (CompoundError.PropertyPath.IsEmpty()) CompoundError.PropertyPath = JsonPointer;
				if (CompoundError.TargetPath.IsEmpty()) CompoundError.TargetPath = FilePath;
				SetFailure(OutResult, CompoundError, ProjectedFile->FileId, Field->FieldId);
				return false;
			}
			CompoundError.Reset();

			struct FPendingExpansion
			{
				FString JsonPointer;
				TArray<int32> EntryIndices;
			};
			TArray<FPendingExpansion> Pending;
			TSet<FString> SeenPointers;
			for (int32 SelectionIndex = 0; SelectionIndex < Selections.Num(); ++SelectionIndex)
			{
				const auto& Selection = Selections[SelectionIndex];
				FPendingExpansion Expansion;
				if (Selection.WildcardValues.Num() != PatternInfo.WildcardCount)
				{
					CompoundError = ProjectionFailure(TEXT("wildcard_cardinality_mismatch"), JsonPointer,
						MakeShared<FJsonValueNumber>(PatternInfo.WildcardCount), MakeShared<FJsonValueNumber>(Selection.WildcardValues.Num()), TEXT("export_projection"),
						FString::Printf(TEXT("Expansion selection %d supplies the wrong number of wildcard replacements."), SelectionIndex));
				}
				else if (UeAgentCompoundProjection::ExpandJsonPointerPattern(
					JsonPointer, Selection.WildcardValues, Expansion.JsonPointer, CompoundError)
					&& SeenPointers.Contains(Expansion.JsonPointer))
				{
					CompoundError = ProjectionFailure(TEXT("duplicate_expanded_json_pointer"), Expansion.JsonPointer, StringValue(TEXT("unique expanded output path")), StringValue(Expansion.JsonPointer), TEXT("export_projection"),
						FString::Printf(TEXT("Expansion selection %d duplicates an earlier output path."), SelectionIndex));
				}
				if (!CompoundError.IsEmpty())
				{
					if (CompoundError.PropertyPath.IsEmpty()) CompoundError.PropertyPath = JsonPointer;
				if (CompoundError.TargetPath.IsEmpty()) CompoundError.TargetPath = FilePath;
				SetFailure(OutResult, CompoundError, ProjectedFile->FileId, Field->FieldId);
					return false;
				}
				TSet<int32> UniqueIndices;
				for (int32 EntryIndex : Selection.EntryIndices)
				{
					if (!CompoundRequest.Entries.IsValidIndex(EntryIndex) || UniqueIndices.Contains(EntryIndex))
					{
						auto Error = ProjectionFailure(TEXT("export_compound_projection_invalid"), Expansion.JsonPointer,
							StringValue(FString::Printf(TEXT("unique snapshot index in [0, %d)"), CompoundRequest.Entries.Num())), MakeShared<FJsonValueNumber>(EntryIndex), TEXT("export_projection"),
							!CompoundRequest.Entries.IsValidIndex(EntryIndex)
								? FString::Printf(TEXT("Expansion selection %d refers to an out-of-range snapshot entry."), SelectionIndex)
								: FString::Printf(TEXT("Expansion selection %d repeats a snapshot entry."), SelectionIndex));
						Error.TargetPath = FilePath;
						SetFailure(OutResult, Error, ProjectedFile->FileId, Field->FieldId);
						return false;
					}
					UniqueIndices.Add(EntryIndex);
					Expansion.EntryIndices.Add(EntryIndex);
				}
				SeenPointers.Add(Expansion.JsonPointer);
				Pending.Add(MoveTemp(Expansion));
			}
			Pending.Sort([](const FPendingExpansion& Left, const FPendingExpansion& Right)
			{
				return Left.JsonPointer < Right.JsonPointer;
			});

			if (Pending.IsEmpty())
			{
				if (Field->EmptyPolicy == TEXT("omit")) continue;
				if (PatternInfo.EmptyContainerPointer.IsEmpty() && Field->EmptyPolicy == TEXT("emit_null"))
				{
					auto Error = ProjectionFailure(TEXT("export_compound_projection_invalid"), JsonPointer, StringValue(TEXT("object root")), StringValue(Field->EmptyPolicy), TEXT("export_projection"), TEXT("An empty root compound container cannot use the emit-null policy."));
					Error.TargetPath = FilePath;
					SetFailure(OutResult, Error, ProjectedFile->FileId, Field->FieldId);
					return false;
				}
				TSharedPtr<FJsonValue> EmptyValue;
				if (Field->EmptyPolicy == TEXT("emit_null"))
				{
					EmptyValue = MakeShared<FJsonValueNull>();
				}
				else
				{
					EmptyValue = MakeShared<FJsonValueObject>(MakeShared<FJsonObject>());
				}
				AddProjectedField(
					Field->FieldId + TEXT("@empty"),
					PatternInfo.EmptyContainerPointer,
					EmptyValue,
					true);
				continue;
			}

			for (const FPendingExpansion& Expansion : Pending)
			{
				FProjectorRequest ExpansionRequest = Request;
				ExpansionRequest.Entries.Reset();
				for (int32 EntryIndex : Expansion.EntryIndices)
				{
					ExpansionRequest.Entries.Add(CompoundRequest.Entries[EntryIndex]);
				}
				FProjectedValue ProjectedValue;
				UE::UAI::Contract::FDiagnostic ProjectorError;
				if (!ProjectRequest(ExpansionRequest, ProjectedValue, ProjectorError))
				{
					if (ProjectorError.PropertyPath.IsEmpty()) ProjectorError.PropertyPath = Expansion.JsonPointer;
					if (ProjectorError.Source.JsonPath.IsEmpty()) ProjectorError.Source.JsonPath = Expansion.JsonPointer;
					if (ProjectorError.TargetPath.IsEmpty()) ProjectorError.TargetPath = FilePath;
					SetFailure(OutResult, ProjectorError, ProjectedFile->FileId, Field->FieldId);
					return false;
				}
				if (!ProjectedValue.bHasValue && Field->EmptyPolicy == TEXT("omit")) continue;
				ApplyEmptyPolicy(ProjectedValue);
				AddProjectedField(
					Field->FieldId + TEXT("@") + Expansion.JsonPointer,
					Expansion.JsonPointer,
					ProjectedValue.Value,
					false);
			}
		}

		for (FProjectedFile& File : OutResult.Files)
		{
			UE::UAI::Contract::FDiagnostic BuildError;
			if (!BuildProjectedFileRoot(File, File.Root, BuildError))
			{
				SetFailure(OutResult, BuildError, File.FileId);
				return false;
			}
		}
		OutResult.Files.RemoveAll([](const FProjectedFile& File)
		{
			return !File.bRequired && File.Fields.IsEmpty();
		});
		OutResult.Status = TEXT("projected");
		OutResult.Diagnostic.Reset();
		OutResult.FileId.Reset();
		OutResult.FieldId.Reset();
		return true;
	}
}
