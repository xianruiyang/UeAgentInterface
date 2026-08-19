// Copyright Epic Games, Inc. All Rights Reserved.

#include "Utilities/AuthoringExport/UeAgentSnapshotProjector.h"

#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Utilities/AuthoringCore/UeAgentDetailsAuthoringService.h"
#include "Utilities/AuthoringExport/UeAgentCompoundProjectionExtension.h"

namespace
{
	using namespace UeAgentSnapshotProjector;

	FString NormalizeId(FString Value)
	{
		Value.TrimStartAndEndInline();
		Value.ToLowerInline();
		return Value;
	}

	void SetFailure(
		FProjectionResult& OutResult,
		const FString& ErrorCode,
		const FString& ErrorDetail = FString(),
		const FString& FileId = FString(),
		const FString& FieldId = FString())
	{
		OutResult.Status = TEXT("failed");
		OutResult.ErrorCode = ErrorCode;
		OutResult.ErrorDetail = ErrorDetail.IsEmpty() ? ErrorCode : ErrorDetail;
		OutResult.FileId = FileId;
		OutResult.FieldId = FieldId;
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
		FString& OutError)
	{
		OutValue = FProjectedValue();
		if (Request.Entries.IsEmpty()) return true;
		if (Request.Entries.Num() != 1)
		{
			OutError = FString::Printf(TEXT("export_projection_cardinality_mismatch:%d"), Request.Entries.Num());
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
		FString& OutError)
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
		FString& OutError,
		bool bAllowEquivalentExisting = false)
	{
		if (!Value.IsValid())
		{
			OutError = TEXT("export_projection_value_missing");
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
				OutError = TEXT("export_projection_root_value_invalid");
				return false;
			}
			Root = CloneObject(Value->AsObject());
			return Root.IsValid();
		}
		if (!JsonPointer.StartsWith(TEXT("/")) || JsonPointer.Contains(TEXT("*")))
		{
			OutError = JsonPointer.Contains(TEXT("*"))
				? TEXT("export_compound_projection_required")
				: TEXT("export_projection_json_pointer_invalid");
			return false;
		}

		TArray<FString> EncodedSegments;
		JsonPointer.Mid(1).ParseIntoArray(EncodedSegments, TEXT("/"), false);
		if (EncodedSegments.IsEmpty())
		{
			OutError = TEXT("export_projection_json_pointer_invalid");
			return false;
		}
		TArray<FString> Segments;
		for (const FString& Encoded : EncodedSegments)
		{
			FString Decoded;
			if (!DecodePointerSegment(Encoded, Decoded))
			{
				OutError = TEXT("export_projection_json_pointer_escape_invalid");
				return false;
			}
			Segments.Add(MoveTemp(Decoded));
		}

		if (!Root.IsValid()) Root = MakeShared<FJsonObject>();
		TSharedPtr<FJsonObject> Current = Root;
		for (int32 Index = 0; Index < Segments.Num() - 1; ++Index)
		{
			const FString& Segment = Segments[Index];
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
				OutError = TEXT("export_projection_path_collision");
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
			OutError = TEXT("export_projection_path_collision");
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
		FString& OutError)
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
			FString Error;
			return RegisterBuiltInProjectors(Registry, Error);
		}();
		(void)bBuiltInsRegistered;
		return Registry;
	}

	bool FProjectorRegistry::RegisterProjector(
		const FProjectorDescriptor& Descriptor,
		FProjector Projector,
		ERegistrationResult& OutResult,
		FString& OutError)
	{
		FProjectorDescriptor Canonical = Descriptor;
		Canonical.ProjectorId = NormalizeId(Canonical.ProjectorId);
		Canonical.ImplementationId = NormalizeId(Canonical.ImplementationId);
		if (Canonical.ProjectorId.IsEmpty() || Canonical.ImplementationId.IsEmpty()
			|| Canonical.SourceKinds.IsEmpty() || !Projector)
		{
			OutError = TEXT("export_projector_descriptor_invalid");
			return false;
		}
		TSet<UeAgentExportContract::EFieldSourceKind> UniqueKinds;
		for (UeAgentExportContract::EFieldSourceKind Kind : Canonical.SourceKinds)
		{
			if (UniqueKinds.Contains(Kind))
			{
				OutError = TEXT("export_projector_source_kind_duplicate");
				return false;
			}
			UniqueKinds.Add(Kind);
		}

		FWriteScopeLock Lock(RegistryLock);
		if (const FEntry* Existing = Projectors.Find(Canonical.ProjectorId))
		{
			if (Existing->Descriptor != Canonical)
			{
				OutError = TEXT("conflicting_export_projector_") + Canonical.ProjectorId;
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
		return Status == TEXT("projected") && ErrorCode.IsEmpty();
	}

	TSharedRef<FJsonObject> FProjectionResult::ToJsonObject() const
	{
		TSharedRef<FJsonObject> Result = MakeShared<FJsonObject>();
		Result->SetStringField(TEXT("schema"), TEXT("uai.export_projection.v1"));
		Result->SetStringField(TEXT("status"), Status);
		Result->SetStringField(TEXT("contract_id"), ContractId);
		Result->SetStringField(TEXT("contract_version"), ContractVersion);
		Result->SetStringField(TEXT("profile_id"), ProfileId);
		if (!ErrorCode.IsEmpty())
		{
			Result->SetStringField(TEXT("error_code"), ErrorCode);
			Result->SetStringField(TEXT("error_detail"), ErrorDetail);
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
		FString& OutError)
	{
		OutRoot = MakeShared<FJsonObject>();
		OutError.Reset();
		for (const FProjectedField& Field : File.Fields)
		{
			if (!Field.Value.IsValid())
			{
				OutError = TEXT("export_projected_field_value_invalid:") + Field.FieldId;
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
				OutError = FString::Printf(
					TEXT("export_projected_field_pointer_invalid:%s:%s"),
					*Field.FieldId,
					*OutError);
				OutRoot.Reset();
				return false;
			}
		}
		return true;
	}

	bool RegisterBuiltInProjectors(FProjectorRegistry& Registry, FString& OutError)
	{
		using UeAgentExportContract::EFieldSourceKind;
		if (!RegisterProjector(Registry, TEXT("uai.export.semantic.value"), { EFieldSourceKind::Semantic }, false,
			[](const FProjectorRequest& Request, FProjectedValue& OutValue, FString& OutProjectorError)
			{
				return ProjectSingleEntry(Request, [](const UeAgentAuthoringState::FStateEntry& Entry)
				{
					return Entry.bAvailable && Entry.Value.IsValid() ? CloneValue(Entry.Value) : nullptr;
				}, OutValue, OutProjectorError);
			}, OutError)) return false;
		if (!RegisterProjector(Registry, TEXT("uai.export.semantic.values"), { EFieldSourceKind::Semantic }, true,
			[](const FProjectorRequest& Request, FProjectedValue& OutValue, FString& OutProjectorError)
			{
				return ProjectEntryList(Request, [](const UeAgentAuthoringState::FStateEntry& Entry)
				{
					return Entry.bAvailable && Entry.Value.IsValid() ? CloneValue(Entry.Value) : nullptr;
				}, OutValue, OutProjectorError);
			}, OutError)) return false;
		if (!RegisterProjector(Registry, TEXT("uai.export.semantic.profiled_value"), { EFieldSourceKind::Semantic }, false,
			[](const FProjectorRequest& Request, FProjectedValue& OutValue, FString& OutProjectorError)
			{
				return ProjectSingleEntry(Request, [&Request](const UeAgentAuthoringState::FStateEntry& Entry)
				{
					return MakeProfiledEnvelopeValue(Entry, Request);
				}, OutValue, OutProjectorError);
			}, OutError)) return false;
		if (!RegisterProjector(Registry, TEXT("uai.export.semantic.profiled_values"), { EFieldSourceKind::Semantic }, true,
			[](const FProjectorRequest& Request, FProjectedValue& OutValue, FString& OutProjectorError)
			{
				return ProjectEntryList(Request, [&Request](const UeAgentAuthoringState::FStateEntry& Entry)
				{
					return MakeProfiledEnvelopeValue(Entry, Request);
				}, OutValue, OutProjectorError);
			}, OutError)) return false;
		if (!RegisterProjector(Registry, TEXT("uai.export.semantic.identity"), { EFieldSourceKind::Semantic }, false,
			[](const FProjectorRequest& Request, FProjectedValue& OutValue, FString& OutProjectorError)
			{
				return ProjectSingleEntry(Request, [](const UeAgentAuthoringState::FStateEntry& Entry)
				{
					return MakeShared<FJsonValueObject>(Entry.TargetIdentity.ToJsonObject());
				}, OutValue, OutProjectorError);
			}, OutError)) return false;
		if (!RegisterProjector(Registry, TEXT("uai.export.semantic.identities"), { EFieldSourceKind::Semantic }, true,
			[](const FProjectorRequest& Request, FProjectedValue& OutValue, FString& OutProjectorError)
			{
				return ProjectEntryList(Request, [](const UeAgentAuthoringState::FStateEntry& Entry)
				{
					return MakeShared<FJsonValueObject>(Entry.TargetIdentity.ToJsonObject());
				}, OutValue, OutProjectorError);
			}, OutError)) return false;
		if (!RegisterProjector(Registry, TEXT("uai.export.semantic.availability"), { EFieldSourceKind::Semantic }, false,
			[](const FProjectorRequest& Request, FProjectedValue& OutValue, FString& OutProjectorError)
			{
				return ProjectSingleEntry(Request, MakeAvailabilityValue, OutValue, OutProjectorError);
			}, OutError)) return false;
		if (!RegisterProjector(Registry, TEXT("uai.export.semantic.availabilities"), { EFieldSourceKind::Semantic }, true,
			[](const FProjectorRequest& Request, FProjectedValue& OutValue, FString& OutProjectorError)
			{
				return ProjectEntryList(Request, MakeAvailabilityValue, OutValue, OutProjectorError);
			}, OutError)) return false;
		if (!RegisterProjector(Registry, TEXT("uai.export.semantic.constraints"), { EFieldSourceKind::Semantic }, false,
			[](const FProjectorRequest& Request, FProjectedValue& OutValue, FString& OutProjectorError)
			{
				return ProjectSingleEntry(Request, MakeConstraintsValue, OutValue, OutProjectorError);
			}, OutError)) return false;
		if (!RegisterProjector(Registry, TEXT("uai.export.semantic.constraints_list"), { EFieldSourceKind::Semantic }, true,
			[](const FProjectorRequest& Request, FProjectedValue& OutValue, FString& OutProjectorError)
			{
				return ProjectEntryList(Request, MakeConstraintsValue, OutValue, OutProjectorError);
			}, OutError)) return false;
		if (!RegisterProjector(Registry, TEXT("uai.export.semantic.details"), { EFieldSourceKind::Semantic }, true,
			[](const FProjectorRequest& Request, FProjectedValue& OutValue, FString& OutProjectorError)
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
			[](const FProjectorRequest& Request, FProjectedValue& OutValue, FString& OutProjectorError)
			{
				return ProjectSingleEntry(Request, [&Request](const UeAgentAuthoringState::FStateEntry& Entry)
				{
					return MakeEditablePropertyValue(Entry, Request, false);
				}, OutValue, OutProjectorError);
			}, OutError)) return false;
		if (!RegisterProjector(Registry, TEXT("uai.export.semantic.editable_properties"), { EFieldSourceKind::Semantic }, true,
			[](const FProjectorRequest& Request, FProjectedValue& OutValue, FString& OutProjectorError)
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
			[](const FProjectorRequest& Request, FProjectedValue& OutValue, FString& OutProjectorError)
			{
				OutValue = FProjectedValue();
				OutProjectorError.Reset();
				if (Request.Entries.IsEmpty()) return true;
				OutValue.Value = MakeShared<FJsonValueString>(TEXT("uai.uobject_properties.v2"));
				OutValue.bHasValue = true;
				return true;
			}, OutError)) return false;
		if (!RegisterProjector(Registry, TEXT("uai.export.semantic.property_document_profile"), { EFieldSourceKind::Semantic }, false,
			[](const FProjectorRequest& Request, FProjectedValue& OutValue, FString& OutProjectorError)
			{
				OutValue = FProjectedValue();
				OutProjectorError.Reset();
				if (Request.Entries.IsEmpty()) return true;
				OutValue.Value = MakeShared<FJsonValueString>(TEXT("authoring"));
				OutValue.bHasValue = true;
				return true;
			}, OutError)) return false;
		if (!RegisterProjector(Registry, TEXT("uai.export.format_version"), { EFieldSourceKind::FormatConstant }, false,
			[](const FProjectorRequest& Request, FProjectedValue& OutValue, FString& OutProjectorError)
			{
				OutValue.bHasValue = true;
				OutValue.Value = MakeShared<FJsonValueString>(Request.Contract->ContractVersion);
				return true;
			}, OutError)) return false;
		if (!RegisterProjector(Registry, TEXT("uai.export.format.operation_update"), { EFieldSourceKind::FormatConstant }, false,
			[](const FProjectorRequest& Request, FProjectedValue& OutValue, FString& OutProjectorError)
			{
				OutValue.bHasValue = true;
				OutValue.Value = MakeShared<FJsonValueString>(TEXT("update"));
				return true;
			}, OutError)) return false;
		if (!RegisterProjector(Registry, TEXT("uai.export.format.operation_replace"), { EFieldSourceKind::FormatConstant }, false,
			[](const FProjectorRequest& Request, FProjectedValue& OutValue, FString& OutProjectorError)
			{
				OutValue.bHasValue = true;
				OutValue.Value = MakeShared<FJsonValueString>(TEXT("replace"));
				return true;
			}, OutError)) return false;
		if (!RegisterProjector(Registry, TEXT("uai.export.navigation.canonical_keys"), { EFieldSourceKind::DerivedNavigation }, true,
			[](const FProjectorRequest& Request, FProjectedValue& OutValue, FString& OutProjectorError)
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
		FString ContractError;
		if (!UeAgentExportContract::ValidateContract(Contract, ContractError))
		{
			SetFailure(OutResult, TEXT("export_contract_invalid"), ContractError);
			return false;
		}
		const UeAgentExportContract::FProfileDescriptor* Profile = Contract.FindProfile(OutResult.ProfileId);
		if (!Profile)
		{
			SetFailure(OutResult, TEXT("export_profile_not_found"), OutResult.ProfileId);
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
				SetFailure(OutResult, TEXT("export_field_output_path_invalid"), Field->OutputPath, FString(), Field->FieldId);
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
				SetFailure(OutResult, TEXT("export_projected_file_contract_missing"), ProjectedFile->FileId);
				return false;
			}

			const FString ProjectorId = ResolveProjectorId(*Field);
			FProjectorDescriptor ProjectorDescriptor;
			FProjector Projector;
			if (ProjectorId.IsEmpty() || !Registry.FindProjector(ProjectorId, ProjectorDescriptor)
				|| !Registry.CopyProjector(ProjectorId, Projector)
				|| !ProjectorDescriptor.SupportsSourceKind(Field->Source.Kind))
			{
				SetFailure(OutResult, TEXT("export_projector_not_found_or_incompatible"), ProjectorId, ProjectedFile->FileId, Field->FieldId);
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

			auto ProjectRequest = [&Field, &Projector](
				const FProjectorRequest& ValueRequest,
				FProjectedValue& OutValue,
				FString& OutProjectorError)
			{
				if (Field->DefaultPolicy == TEXT("omit_if_native_default")
					&& IsNativeDefaultEquivalent(ValueRequest.Entries))
				{
					OutValue = FProjectedValue();
					OutProjectorError.Reset();
					return true;
				}
				return Projector(ValueRequest, OutValue, OutProjectorError);
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
				FString ProjectorError;
				if (!ProjectRequest(Request, ProjectedValue, ProjectorError))
				{
					SetFailure(OutResult, TEXT("export_projector_failed"), ProjectorError, ProjectedFile->FileId, Field->FieldId);
					return false;
				}
				if (!ProjectedValue.bHasValue && Field->EmptyPolicy == TEXT("omit")) continue;
				ApplyEmptyPolicy(ProjectedValue);
				AddProjectedField(Field->FieldId, JsonPointer, ProjectedValue.Value, false);
				continue;
			}

			UeAgentCompoundProjection::FJsonPointerPatternInfo PatternInfo;
			FString CompoundError;
			if (!UeAgentCompoundProjection::InspectJsonPointerPattern(JsonPointer, PatternInfo, CompoundError))
			{
				SetFailure(OutResult, TEXT("export_compound_projection_invalid"), CompoundError, ProjectedFile->FileId, Field->FieldId);
				return false;
			}
			UeAgentCompoundProjection::FExtensionDescriptor ExtensionDescriptor;
			UeAgentCompoundProjection::FExtension Extension;
			if (!CompoundRegistry.FindExtension(ContractFile->ProjectionId, ExtensionDescriptor)
				|| !CompoundRegistry.CopyExtension(ContractFile->ProjectionId, Extension)
				|| !ExtensionDescriptor.SupportsSourceKind(Field->Source.Kind))
			{
				SetFailure(
					OutResult,
					TEXT("export_compound_projection_required"),
					ContractFile->ProjectionId,
					ProjectedFile->FileId,
					Field->FieldId);
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
				SetFailure(OutResult, TEXT("export_compound_projection_failed"), CompoundError, ProjectedFile->FileId, Field->FieldId);
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
			for (const UeAgentCompoundProjection::FExpansionSelection& Selection : Selections)
			{
				FPendingExpansion Expansion;
				if (Selection.WildcardValues.Num() != PatternInfo.WildcardCount)
				{
					CompoundError = TEXT("wildcard_cardinality_mismatch");
				}
				else if (UeAgentCompoundProjection::ExpandJsonPointerPattern(
					JsonPointer, Selection.WildcardValues, Expansion.JsonPointer, CompoundError)
					&& SeenPointers.Contains(Expansion.JsonPointer))
				{
					CompoundError = TEXT("duplicate_expanded_json_pointer:") + Expansion.JsonPointer;
				}
				if (!CompoundError.IsEmpty())
				{
					SetFailure(
						OutResult,
						TEXT("export_compound_projection_invalid"),
						CompoundError,
						ProjectedFile->FileId,
						Field->FieldId);
					return false;
				}
				TSet<int32> UniqueIndices;
				for (int32 EntryIndex : Selection.EntryIndices)
				{
					if (!CompoundRequest.Entries.IsValidIndex(EntryIndex) || UniqueIndices.Contains(EntryIndex))
					{
						SetFailure(
							OutResult,
							TEXT("export_compound_projection_invalid"),
							TEXT("snapshot_entry_selection_invalid"),
							ProjectedFile->FileId,
							Field->FieldId);
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
					SetFailure(
						OutResult,
						TEXT("export_compound_projection_invalid"),
						TEXT("root_compound_container_cannot_emit_null"),
						ProjectedFile->FileId,
						Field->FieldId);
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
				FString ProjectorError;
				if (!ProjectRequest(ExpansionRequest, ProjectedValue, ProjectorError))
				{
					SetFailure(OutResult, TEXT("export_projector_failed"), ProjectorError, ProjectedFile->FileId, Field->FieldId);
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
			FString BuildError;
			if (!BuildProjectedFileRoot(File, File.Root, BuildError))
			{
				SetFailure(OutResult, TEXT("export_projection_document_invalid"), BuildError, File.FileId);
				return false;
			}
		}
		OutResult.Files.RemoveAll([](const FProjectedFile& File)
		{
			return !File.bRequired && File.Fields.IsEmpty();
		});
		OutResult.Status = TEXT("projected");
		OutResult.ErrorCode.Reset();
		OutResult.ErrorDetail.Reset();
		OutResult.FileId.Reset();
		OutResult.FieldId.Reset();
		return true;
	}
}
