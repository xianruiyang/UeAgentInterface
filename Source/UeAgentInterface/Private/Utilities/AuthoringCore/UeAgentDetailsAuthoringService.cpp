// Copyright Epic Games, Inc. All Rights Reserved.

#include "Utilities/AuthoringCore/UeAgentDetailsAuthoringService.h"

#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "UObject/Class.h"
#include "UObject/UnrealType.h"

namespace UeAgentDetailsAuthoringService
{
	namespace
	{
		TSharedPtr<FJsonValue> CloneValue(const TSharedPtr<FJsonValue>& Value)
		{
			return UeAgentAuthoringState::CloneCanonicalValue(Value);
		}

		UeAgentAuthoringState::FEditablePropertyReadOptions MakeReadOptions(
			const FCapturePolicy& Policy)
		{
			UeAgentAuthoringState::FEditablePropertyReadOptions Result;
			Result.SemanticId = Policy.SemanticId;
			Result.Limits.MaxContainerItems = Policy.MaxContainerItems;
			Result.Limits.MaxStringChars = Policy.MaxStringChars;
			Result.Limits.StructFieldFilter = Policy.StructFieldFilter;
			Result.ExcludedProperties = Policy.ExcludedProperties;
			Result.IncludeProperty = Policy.IncludeProperty;
			Result.BuildSnapshotIdentity = Policy.BuildSnapshotIdentity;
			return Result;
		}

		bool CaptureRequests(
			const TArray<UeAgentAuthoringState::FReadRequest>& Requests,
			UeAgentAuthoringState::FStateSnapshot& OutSnapshot,
			FString& OutError)
		{
			FString RegistrationError;
			if (!UeAgentAuthoringState::RegisterBuiltInStateAccessors(
					UeAgentAuthoringState::FStateAccessorRegistry::Get(), RegistrationError))
			{
				OutError = TEXT("details_state_accessor_registration_failed:") + RegistrationError;
				return false;
			}
			UeAgentAuthoringState::FCaptureResult Capture;
			if (!UeAgentAuthoringState::CaptureSnapshot(Requests, Capture))
			{
				OutError = Capture.ErrorCode;
				if (!Capture.ErrorDetail.IsEmpty()) OutError += TEXT(":") + Capture.ErrorDetail;
				return false;
			}
			OutSnapshot = MoveTemp(Capture.Snapshot);
			OutError.Reset();
			return true;
		}

		void SetApplyFailure(
			const FString& ErrorCode,
			UeAgentAuthoringDirectApply::FResult& OutResult,
			FString& OutError)
		{
			OutResult = UeAgentAuthoringDirectApply::FResult();
			OutResult.Status = TEXT("failed");
			OutResult.Phase = TEXT("decode");
			OutResult.ErrorCode = ErrorCode;
			OutResult.ErrorDetail = ErrorCode;
			OutError = ErrorCode;
		}
	}

	TSharedPtr<FJsonObject> BuildPropertyDescriptor(
		const UeAgentAuthoringState::FStateEntry& Entry,
		const FProjectionPolicy& Policy)
	{
		if (Policy.bRequireEditable && (!Entry.bAvailable || !Entry.bEditable))
		{
			return nullptr;
		}
		const FString PropertyPath = Entry.TargetIdentity.GetSelector(TEXT("property_path"));

		TSharedRef<FJsonObject> Descriptor = MakeShared<FJsonObject>();
		if (!PropertyPath.IsEmpty()) Descriptor->SetStringField(TEXT("property_path"), PropertyPath);
		if (Policy.bIncludeDisplayName
			&& !Entry.DisplayName.IsEmpty() && Entry.DisplayName != PropertyPath)
		{
			Descriptor->SetStringField(TEXT("display_name"), Entry.DisplayName);
		}
		if (Policy.bIncludeTooltip && !Entry.Tooltip.IsEmpty())
		{
			Descriptor->SetStringField(TEXT("tooltip"), Entry.Tooltip);
		}
		if (TSharedPtr<FJsonObject> Type = Entry.Type.ToJsonObject(); Type.IsValid())
		{
			FString Kind;
			if (Type->TryGetStringField(TEXT("kind"), Kind) && !Kind.IsEmpty())
			{
				Descriptor->SetStringField(TEXT("value_kind"), Kind);
			}
			for (const TPair<FString, TSharedPtr<FJsonValue>>& Pair : Type->Values)
			{
				if (Pair.Key != TEXT("kind")) Descriptor->SetField(Pair.Key, CloneValue(Pair.Value));
			}
		}

		if (Policy.bIncludeAvailability)
		{
			TSharedRef<FJsonObject> Availability = MakeShared<FJsonObject>();
			Availability->SetBoolField(TEXT("available"), Entry.bAvailable);
			Availability->SetBoolField(TEXT("editable"), Entry.bEditable);
			if (!Entry.AvailabilityReason.IsEmpty())
			{
				Availability->SetStringField(TEXT("reason"), Entry.AvailabilityReason);
			}
			Descriptor->SetObjectField(TEXT("availability"), Availability);
		}

		if (!Entry.Constraints.IsEmpty())
		{
			TSharedRef<FJsonObject> Constraints = MakeShared<FJsonObject>();
			TArray<FString> Keys;
			Entry.Constraints.GenerateKeyArray(Keys);
			Keys.Sort();
			for (const FString& Key : Keys)
			{
				Constraints->SetStringField(Key, Entry.Constraints.FindChecked(Key));
			}
			Descriptor->SetObjectField(TEXT("constraints"), Constraints);
		}
		if (Policy.bIncludeCategoryPath && !Entry.CategoryPath.IsEmpty())
		{
			TArray<TSharedPtr<FJsonValue>> Category;
			for (const FString& Part : Entry.CategoryPath)
			{
				Category.Add(MakeShared<FJsonValueString>(Part));
			}
			Descriptor->SetArrayField(TEXT("category_path"), Category);
		}
		if (Entry.CandidatesHeader.IsValid())
		{
			Descriptor->SetField(TEXT("candidates_header"), CloneValue(Entry.CandidatesHeader));
		}
		if (Entry.Candidates.IsValid())
		{
			Descriptor->SetField(TEXT("candidates"), CloneValue(Entry.Candidates));
		}
		const bool bStructured = Entry.bAvailable && Entry.Value.IsValid();
		if (bStructured) Descriptor->SetField(TEXT("value"), CloneValue(Entry.Value));
		if ((!bStructured || Policy.bPreserveValueText) && !Entry.ValueText.IsEmpty())
		{
			Descriptor->SetStringField(TEXT("value_text"), Entry.ValueText);
		}
		return Descriptor;
	}

	bool BuildPropertiesDocument(
		const TArray<UeAgentAuthoringState::FStateEntry>& Entries,
		const FProjectionPolicy& Policy,
		TSharedPtr<FJsonObject>& OutDocument,
		FString& OutError)
	{
		TArray<UeAgentAuthoringState::FStateEntry> Sorted = Entries;
		Sorted.Sort([](
			const UeAgentAuthoringState::FStateEntry& Left,
			const UeAgentAuthoringState::FStateEntry& Right)
		{
			return Left.TargetIdentity.GetSelector(TEXT("property_path"))
				< Right.TargetIdentity.GetSelector(TEXT("property_path"));
		});
		TArray<TSharedPtr<FJsonValue>> Properties;
		for (const UeAgentAuthoringState::FStateEntry& Entry : Sorted)
		{
			if (TSharedPtr<FJsonObject> Descriptor = BuildPropertyDescriptor(Entry, Policy))
			{
				Properties.Add(MakeShared<FJsonValueObject>(Descriptor));
			}
		}
		OutDocument = MakeShared<FJsonObject>();
		OutDocument->SetStringField(TEXT("schema"), TEXT("uai.uobject_properties.v2"));
		OutDocument->SetStringField(TEXT("profile"), Policy.Profile);
		OutDocument->SetNumberField(TEXT("property_count"), Properties.Num());
		OutDocument->SetArrayField(TEXT("properties"), Properties);
		OutError.Reset();
		return true;
	}

	bool CaptureObjectProperties(
		const UObject* Object,
		const FCapturePolicy& Policy,
		UeAgentAuthoringState::FStateSnapshot& OutSnapshot,
		TSharedPtr<FJsonObject>& OutDocument,
		FString& OutError)
	{
		TArray<UeAgentAuthoringState::FReadRequest> Requests;
		if (!UeAgentAuthoringState::AppendEditableObjectPropertyReadRequests(
				Object, MakeReadOptions(Policy), Requests, OutError)) return false;
		if (!CaptureRequests(Requests, OutSnapshot, OutError)) return false;
		return BuildPropertiesDocument(
			OutSnapshot.CopyEntries(), FProjectionPolicy(), OutDocument, OutError);
	}

	bool CaptureStructProperties(
		UStruct* StructType,
		const void* StructData,
		const FString& RootOwnerPath,
		const UObject* NotificationOwner,
		const FCapturePolicy& Policy,
		UeAgentAuthoringState::FStateSnapshot& OutSnapshot,
		TSharedPtr<FJsonObject>& OutDocument,
		FString& OutError)
	{
		TArray<UeAgentAuthoringState::FReadRequest> Requests;
		if (!UeAgentAuthoringState::AppendEditableStructPropertyReadRequests(
				StructType, StructData, RootOwnerPath, NotificationOwner,
				MakeReadOptions(Policy), Requests, OutError)) return false;
		if (!CaptureRequests(Requests, OutSnapshot, OutError)) return false;
		return BuildPropertiesDocument(
			OutSnapshot.CopyEntries(), FProjectionPolicy(), OutDocument, OutError);
	}

	bool AppendStructPropertyReadRequests(
		UStruct* StructType,
		const void* StructData,
		const FString& RootOwnerPath,
		const UObject* NotificationOwner,
		const FCapturePolicy& Policy,
		TArray<UeAgentAuthoringState::FReadRequest>& OutRequests,
		FString& OutError)
	{
		return UeAgentAuthoringState::AppendEditableStructPropertyReadRequests(
			StructType, StructData, RootOwnerPath, NotificationOwner,
			MakeReadOptions(Policy), OutRequests, OutError);
	}

	bool CaptureObjectProperty(
		const UObject* Object,
		const FString& PropertyPath,
		const FCapturePolicy& Policy,
		UeAgentAuthoringState::FStateEntry& OutEntry,
		TSharedPtr<FJsonObject>& OutDescriptor,
		FString& OutError)
	{
		UeAgentAuthoringState::FReadRequest Request;
		UeAgentAuthoringTargetLocator::FResolveResult TargetResult;
		if (!UeAgentAuthoringState::BuildPropertyReadRequest(
				Object, PropertyPath, Policy.SemanticId, Request, TargetResult))
		{
			OutError = TargetResult.ErrorCode;
			return false;
		}
		Request.Limits = MakeReadOptions(Policy).Limits;
		if (Policy.BuildSnapshotIdentity)
		{
			UeAgentAuthoringIdentity::FCanonicalIdentity Identity;
			if (!Policy.BuildSnapshotIdentity(Request.PropertyPath, Identity, OutError)) return false;
			Request.SnapshotIdentityOverride = MoveTemp(Identity);
		}
		UeAgentAuthoringState::FStateSnapshot Snapshot;
		if (!CaptureRequests({ Request }, Snapshot, OutError)) return false;
		const TArray<UeAgentAuthoringState::FStateEntry> Entries = Snapshot.CopyEntries();
		if (Entries.Num() != 1)
		{
			OutError = TEXT("details_single_property_cardinality_invalid");
			return false;
		}
		OutEntry = Entries[0];
		OutDescriptor = BuildPropertyDescriptor(OutEntry, FProjectionPolicy());
		if (!OutDescriptor.IsValid())
		{
			OutError = TEXT("details_single_property_descriptor_unavailable");
			return false;
		}
		OutError.Reset();
		return true;
	}

	bool CaptureStructPropertyPaths(
		UStruct* StructType,
		const void* StructData,
		const FString& RootOwnerPath,
		const UObject* NotificationOwner,
		const TArray<FString>& PropertyPaths,
		const FCapturePolicy& Policy,
		TSharedPtr<FJsonObject>& OutDocument,
		FString& OutError)
	{
		TArray<UeAgentAuthoringState::FReadRequest> Requests;
		for (const FString& PropertyPath : PropertyPaths)
		{
			UeAgentAuthoringState::FReadRequest Request;
			UeAgentAuthoringTargetLocator::FResolveResult TargetResult;
			if (!UeAgentAuthoringState::BuildStructPropertyReadRequest(
					StructType, StructData, RootOwnerPath, NotificationOwner,
					PropertyPath, Policy.SemanticId, Request, TargetResult))
			{
				OutError = TargetResult.ErrorCode;
				return false;
			}
			Request.Limits = MakeReadOptions(Policy).Limits;
			Requests.Add(MoveTemp(Request));
		}
		UeAgentAuthoringState::FStateSnapshot Snapshot;
		if (!CaptureRequests(Requests, Snapshot, OutError)) return false;
		return BuildPropertiesDocument(
			Snapshot.CopyEntries(), FProjectionPolicy(), OutDocument, OutError);
	}

	bool CaptureStructDeltaFields(
		UScriptStruct* StructType,
		const void* StructData,
		const void* DefaultData,
		const FString& RootOwnerPath,
		const UObject* NotificationOwner,
		TSharedPtr<FJsonObject>& OutFields,
		FString& OutError)
	{
		OutFields = MakeShared<FJsonObject>();
		if (!StructType || !StructData)
		{
			OutError = TEXT("details_struct_delta_input_invalid");
			return false;
		}
		TArray<FString> Paths;
		for (TFieldIterator<FProperty> It(StructType, EFieldIterationFlags::IncludeSuper); It; ++It)
		{
			FProperty* Property = *It;
			if (!Property || Property->HasAnyPropertyFlags(CPF_Transient | CPF_Deprecated)) continue;
			const void* ValuePtr = Property->ContainerPtrToValuePtr<void>(StructData);
			const void* DefaultPtr = DefaultData
				? Property->ContainerPtrToValuePtr<void>(DefaultData)
				: nullptr;
			if (DefaultPtr && Property->Identical(ValuePtr, DefaultPtr, PPF_None)) continue;
			Paths.Add(Property->GetName());
		}
		Paths.Sort();
		FCapturePolicy Policy;
		TSharedPtr<FJsonObject> Document;
		if (!CaptureStructPropertyPaths(
				StructType, StructData, RootOwnerPath, NotificationOwner,
				Paths, Policy, Document, OutError)) return false;
		const TArray<TSharedPtr<FJsonValue>>* Properties = nullptr;
		if (!Document->TryGetArrayField(TEXT("properties"), Properties) || !Properties)
		{
			OutError = TEXT("details_struct_delta_properties_missing");
			return false;
		}
		for (const TSharedPtr<FJsonValue>& Value : *Properties)
		{
			const TSharedPtr<FJsonObject> Descriptor = Value.IsValid() && Value->Type == EJson::Object
				? Value->AsObject() : nullptr;
			FString PropertyPath;
			if (!Descriptor.IsValid() || !Descriptor->TryGetStringField(TEXT("property_path"), PropertyPath)) continue;
			if (const TSharedPtr<FJsonValue> CanonicalValue = Descriptor->TryGetField(TEXT("value")); CanonicalValue.IsValid())
			{
				OutFields->SetField(PropertyPath, CloneValue(CanonicalValue));
			}
		}
		OutError.Reset();
		return true;
	}

	bool DecodePropertiesDocument(
		const TSharedPtr<FJsonObject>& Document,
		TArray<TSharedPtr<FJsonValue>>& OutDescriptors,
		FString& OutError)
	{
		OutDescriptors.Reset();
		FString Schema;
		if (!Document.IsValid() || !Document->TryGetStringField(TEXT("schema"), Schema)
			|| Schema != TEXT("uai.uobject_properties.v2"))
		{
			OutError = TEXT("schema_mismatch_expected_uai.uobject_properties.v2");
			return false;
		}
		const TArray<TSharedPtr<FJsonValue>>* Properties = nullptr;
		if (!Document->TryGetArrayField(TEXT("properties"), Properties) || !Properties)
		{
			OutError = TEXT("missing_properties_array");
			return false;
		}
		OutDescriptors.Reserve(Properties->Num());
		for (const TSharedPtr<FJsonValue>& Value : *Properties)
		{
			const TSharedPtr<FJsonObject> Descriptor = Value.IsValid() && Value->Type == EJson::Object
				? Value->AsObject() : nullptr;
			const TSharedPtr<FJsonObject> Projected =
				UeAgentAuthoringObjectApply::ProjectDescriptorForApply(Descriptor);
			if (!Projected.IsValid())
			{
				FString PropertyPath;
				if (Descriptor.IsValid()) Descriptor->TryGetStringField(TEXT("property_path"), PropertyPath);
				OutError = TEXT("property_descriptor_invalid");
				if (!PropertyPath.IsEmpty()) OutError += TEXT(":") + PropertyPath;
				return false;
			}
			OutDescriptors.Add(MakeShared<FJsonValueObject>(Projected));
		}
		OutError.Reset();
		return true;
	}

	bool ApplyObjectProperties(
		UObject* Object,
		const TSharedPtr<FJsonObject>& Document,
		const UeAgentAuthoringObjectApply::FOptions& Options,
		UeAgentAuthoringDirectApply::FResult& OutResult,
		FString& OutError)
	{
		TArray<TSharedPtr<FJsonValue>> Descriptors;
		if (!DecodePropertiesDocument(Document, Descriptors, OutError))
		{
			SetApplyFailure(OutError, OutResult, OutError);
			return false;
		}
		if (Descriptors.IsEmpty())
		{
			OutResult = UeAgentAuthoringDirectApply::FResult();
			OutResult.Status = TEXT("applied");
			OutResult.Phase = TEXT("complete");
			OutError.Reset();
			return true;
		}
		if (!UeAgentAuthoringDirectApply::ApplyObjectPatch(Object, Descriptors, Options, OutResult))
		{
			OutError = OutResult.ErrorCode;
			if (!OutResult.ErrorDetail.IsEmpty()) OutError += TEXT(":") + OutResult.ErrorDetail;
			return false;
		}
		OutError.Reset();
		return true;
	}

	bool ApplyStructProperties(
		UStruct* StructType,
		void* StructData,
		UObject* NotificationOwner,
		const TSharedPtr<FJsonObject>& Document,
		const UeAgentAuthoringObjectApply::FOptions& Options,
		UeAgentAuthoringDirectApply::FResult& OutResult,
		FString& OutError)
	{
		TArray<TSharedPtr<FJsonValue>> Descriptors;
		if (!DecodePropertiesDocument(Document, Descriptors, OutError))
		{
			SetApplyFailure(OutError, OutResult, OutError);
			return false;
		}
		if (Descriptors.IsEmpty())
		{
			OutResult = UeAgentAuthoringDirectApply::FResult();
			OutResult.Status = TEXT("applied");
			OutResult.Phase = TEXT("complete");
			OutError.Reset();
			return true;
		}
		if (!UeAgentAuthoringDirectApply::ApplyStructPatch(
				StructType, StructData, NotificationOwner, Descriptors, Options, OutResult))
		{
			OutError = OutResult.ErrorCode;
			if (!OutResult.ErrorDetail.IsEmpty()) OutError += TEXT(":") + OutResult.ErrorDetail;
			return false;
		}
		OutError.Reset();
		return true;
	}
}
