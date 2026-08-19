// Copyright Epic Games, Inc. All Rights Reserved.

#include "Utilities/AuthoringCore/UeAgentAuthoringNativeDefaults.h"

#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "HAL/ThreadSafeCounter.h"
#include "Misc/Guid.h"
#include "Misc/ScopeRWLock.h"
#include "UObject/Class.h"
#include "UObject/Package.h"
#include "UObject/StrongObjectPtr.h"
#include "UObject/StructOnScope.h"
#include "UObject/UnrealType.h"
#include "Utilities/UeAgentPropertyValueCodec.h"

namespace UeAgentAuthoringNativeDefaults
{
	namespace
	{
		FThreadSafeCounter ActiveScratchScopes;
		FThreadSafeCounter TrackedScratchObjects;

		FString NormalizeId(FString Value)
		{
			Value.TrimStartAndEndInline();
			Value.ToLowerInline();
			return Value;
		}

		void NormalizeList(TArray<FString>& Values)
		{
			for (FString& Value : Values) Value = NormalizeId(MoveTemp(Value));
			Values.RemoveAll([](const FString& Value) { return Value.IsEmpty(); });
			Values.Sort();
			for (int32 Index = Values.Num() - 1; Index > 0; --Index)
			{
				if (Values[Index] == Values[Index - 1]) Values.RemoveAt(Index);
			}
		}

		FProviderDescriptor Normalize(FProviderDescriptor Descriptor)
		{
			Descriptor.ProviderId = NormalizeId(MoveTemp(Descriptor.ProviderId));
			NormalizeList(Descriptor.TargetKinds);
			NormalizeList(Descriptor.CapabilityTags);
			return Descriptor;
		}

		bool ValidateProvider(const FProviderDescriptor& Descriptor, const FProbe& Probe, FString& OutError)
		{
			if (Descriptor.ProviderId.IsEmpty()) OutError = TEXT("native_default_provider_id_missing");
			else if (Descriptor.SourceKind == UeAgentAuthoringSemanticRegistry::EDefaultSourceKind::None) OutError = TEXT("native_default_provider_source_missing");
			else if (Descriptor.TargetKinds.IsEmpty()) OutError = TEXT("native_default_provider_targets_missing");
			else if (!Probe) OutError = TEXT("native_default_provider_probe_missing");
			else
			{
				OutError.Reset();
				return true;
			}
			return false;
		}

		bool SupportsTargetKind(const FProviderDescriptor& Descriptor, const FString& TargetKind)
		{
			return Descriptor.TargetKinds.Contains(TEXT("*")) || Descriptor.TargetKinds.Contains(NormalizeId(TargetKind));
		}

		FString ProbeOutcomeToString(EProbeOutcome Outcome)
		{
			switch (Outcome)
			{
			case EProbeOutcome::Unavailable: return TEXT("unavailable");
			case EProbeOutcome::Resolved: return TEXT("resolved");
			case EProbeOutcome::Failed: return TEXT("failed");
			default: return TEXT("unknown");
			}
		}

		bool JsonValuesEqual(const TSharedPtr<FJsonValue>& Left, const TSharedPtr<FJsonValue>& Right)
		{
			if (!Left.IsValid() || !Right.IsValid()) return Left.IsValid() == Right.IsValid();
			if (Left->Type != Right->Type) return false;
			switch (Left->Type)
			{
			case EJson::None:
			case EJson::Null:
				return true;
			case EJson::String:
				return Left->AsString() == Right->AsString();
			case EJson::Number:
				return Left->AsNumber() == Right->AsNumber();
			case EJson::Boolean:
				return Left->AsBool() == Right->AsBool();
			case EJson::Array:
			{
				const TArray<TSharedPtr<FJsonValue>>& LeftValues = Left->AsArray();
				const TArray<TSharedPtr<FJsonValue>>& RightValues = Right->AsArray();
				if (LeftValues.Num() != RightValues.Num()) return false;
				for (int32 Index = 0; Index < LeftValues.Num(); ++Index)
				{
					if (!JsonValuesEqual(LeftValues[Index], RightValues[Index])) return false;
				}
				return true;
			}
			case EJson::Object:
			{
				const TSharedPtr<FJsonObject> LeftObject = Left->AsObject();
				const TSharedPtr<FJsonObject> RightObject = Right->AsObject();
				if (!LeftObject.IsValid() || !RightObject.IsValid()) return LeftObject.IsValid() == RightObject.IsValid();
				if (LeftObject->Values.Num() != RightObject->Values.Num()) return false;
				for (const TPair<FString, TSharedPtr<FJsonValue>>& Pair : LeftObject->Values)
				{
					const TSharedPtr<FJsonValue>* OtherValue = RightObject->Values.Find(Pair.Key);
					if (!OtherValue || !JsonValuesEqual(Pair.Value, *OtherValue)) return false;
				}
				return true;
			}
			default:
				return false;
			}
		}

		bool ValidateDefaultTarget(const FRequest& Request, FResult& OutResult)
		{
			if (!Request.TargetIdentity.IsValid())
			{
				OutResult.Status = TEXT("failed");
				OutResult.ErrorCode = TEXT("native_default_target_identity_invalid");
				return false;
			}

			UeAgentAuthoringTargetLocator::FTargetQuery Query = Request.TargetQuery;
			if (Query.ModelId.IsEmpty()) Query.ModelId = Request.TargetIdentity.ModelId;
			if (Query.TargetKind.IsEmpty()) Query.TargetKind = Request.TargetIdentity.TargetKind;
			if (Query.CanonicalKey.IsEmpty()) Query.CanonicalKey = Request.TargetIdentity.CanonicalKey;
			TArray<UeAgentAuthoringIdentity::FCanonicalIdentity> Candidates = { Request.TargetIdentity };
			UeAgentAuthoringTargetLocator::FResolveResult TargetResult;
			if (!UeAgentAuthoringTargetLocator::ResolveExact(Query, Candidates, TargetResult))
			{
				OutResult.Status = TEXT("failed");
				OutResult.ErrorCode = TargetResult.ErrorCode.IsEmpty() ? FString(TEXT("native_default_target_resolution_failed")) : TargetResult.ErrorCode;
				return false;
			}
			return true;
		}

		bool EncodePropertyValue(
			const FRequest& Request,
			FProperty* Property,
			const void* ValuePtr,
			UObject* Owner,
			const UeAgentAuthoringIdentity::FCanonicalIdentity& SourceIdentity,
			const FString& Provenance,
			FProbeValue& OutValue,
			FString& OutError)
		{
			if (!Property || !ValuePtr)
			{
				OutError = TEXT("native_default_property_value_unavailable");
				return false;
			}

			OutValue.Value = UeAgentPropertyValueCodec::Encode(
				Property,
				ValuePtr,
				Owner,
				UeAgentPropertyValueCodec::FOptions());
			if (!OutValue.Value.IsValid())
			{
				OutError = TEXT("native_default_property_encode_failed");
				return false;
			}
			OutValue.PropertyValue = FPropertyValueStorage::CopyFrom(Property, ValuePtr, OutError);
			if (!OutValue.PropertyValue.IsValid()) return false;
			Property->ExportTextItem_Direct(OutValue.ValueText, ValuePtr, nullptr, Owner, PPF_None);
			OutValue.SourceIdentity = SourceIdentity.IsValid() ? SourceIdentity : Request.TargetIdentity;
			OutValue.Provenance = Provenance;
			OutError.Reset();
			return true;
		}

		EProbeOutcome ProbeExisting(
			const FRequest& Request,
			FScratchScope& Scratch,
			FProbeValue& OutValue,
			FString& OutError)
		{
			if (!Request.Property || !Request.ExistingValuePtr) return EProbeOutcome::Unavailable;
			if (!EncodePropertyValue(
				Request,
				Request.Property,
				Request.ExistingValuePtr,
				Request.ExistingOwner,
				Request.TargetIdentity,
				TEXT("existing_instance"),
				OutValue,
				OutError))
			{
				return EProbeOutcome::Failed;
			}
			return EProbeOutcome::Resolved;
		}

		EProbeOutcome ProbeConstructor(
			const FRequest& Request,
			FScratchScope& Scratch,
			FProbeValue& OutValue,
			FString& OutError)
		{
			if (!Request.Property || (!Request.ObjectClass && !Request.ArchetypeOrTemplate)) return EProbeOutcome::Unavailable;

			UObject* SourceObject = Request.ArchetypeOrTemplate;
			FString Provenance = TEXT("archetype_or_template");
			if (Request.bConstructScratchObject)
			{
				UClass* ObjectClass = Request.ObjectClass ? Request.ObjectClass : Request.ArchetypeOrTemplate->GetClass();
				SourceObject = Scratch.CreateObject(ObjectClass, Request.ProposedOuter, Request.ArchetypeOrTemplate, OutError);
				if (!SourceObject) return EProbeOutcome::Failed;
				Provenance = Request.ArchetypeOrTemplate ? TEXT("constructor_with_template") : TEXT("constructor");
			}
			else if (!SourceObject && Request.ObjectClass)
			{
				SourceObject = Request.ObjectClass->GetDefaultObject(false);
				Provenance = TEXT("class_default_object");
			}
			if (!SourceObject) return EProbeOutcome::Unavailable;

			FProperty* SourceProperty = Request.Property;
			void* SourceValuePtr = nullptr;
			UeAgentAuthoringIdentity::FCanonicalIdentity SourceIdentity;
			const FString PropertyPath = Request.PropertyPath.IsEmpty() ? Request.Property->GetName() : Request.PropertyPath;
			UeAgentAuthoringTargetLocator::FResolvedPropertyTarget ResolvedProperty;
			UeAgentAuthoringTargetLocator::FResolveResult TargetResult;
			if (UeAgentAuthoringTargetLocator::ResolvePropertyPath(SourceObject, PropertyPath, ResolvedProperty, TargetResult))
			{
				SourceProperty = ResolvedProperty.Property;
				SourceValuePtr = ResolvedProperty.ValuePtr;
				SourceIdentity = ResolvedProperty.Identity;
			}
			else
			{
				OutError = TargetResult.ErrorCode.IsEmpty() ? FString(TEXT("native_default_source_property_not_found")) : TargetResult.ErrorCode;
				return EProbeOutcome::Failed;
			}

			if (!EncodePropertyValue(Request, SourceProperty, SourceValuePtr, SourceObject, SourceIdentity, Provenance, OutValue, OutError))
			{
				return EProbeOutcome::Failed;
			}
			return EProbeOutcome::Resolved;
		}

		EProbeOutcome ProbeStructInitialization(
			const FRequest& Request,
			FScratchScope& Scratch,
			FProbeValue& OutValue,
			FString& OutError)
		{
			if (!Request.Property || !Request.StructType) return EProbeOutcome::Unavailable;
			FStructOnScope InitializedStruct(Request.StructType);
			if (!InitializedStruct.IsValid())
			{
				OutError = TEXT("native_default_struct_initialize_failed");
				return EProbeOutcome::Failed;
			}

			const void* ValuePtr = nullptr;
			if (Request.Property->GetOwnerStruct() == Request.StructType)
			{
				ValuePtr = Request.Property->ContainerPtrToValuePtr<void>(InitializedStruct.GetStructMemory());
			}
			else if (const FStructProperty* StructProperty = CastField<FStructProperty>(Request.Property))
			{
				if (StructProperty->Struct == Request.StructType) ValuePtr = InitializedStruct.GetStructMemory();
			}
			if (!ValuePtr) return EProbeOutcome::Unavailable;

			if (!EncodePropertyValue(
				Request,
				Request.Property,
				ValuePtr,
				nullptr,
				Request.TargetIdentity,
				TEXT("uscriptstruct_initialize_struct"),
				OutValue,
				OutError))
			{
				return EProbeOutcome::Failed;
			}
			return EProbeOutcome::Resolved;
		}

		TArray<UeAgentAuthoringSemanticRegistry::EDefaultSourceKind> SourceOrderForOperation(EOperation Operation)
		{
			using UeAgentAuthoringSemanticRegistry::EDefaultSourceKind;
			if (Operation == EOperation::NestedPatch) return { EDefaultSourceKind::ExistingInstance };
			if (Operation == EOperation::PostCreateReadback) return { EDefaultSourceKind::PostCreate };
			return {
				EDefaultSourceKind::Factory,
				EDefaultSourceKind::Constructor,
				EDefaultSourceKind::StructInitialization,
				EDefaultSourceKind::Schema,
				EDefaultSourceKind::DomainNative
			};
		}

		struct FResolvedCandidate
		{
			FProviderDescriptor Descriptor;
			FProbeValue Value;
		};

		void SortCandidates(TArray<FResolvedCandidate>& Candidates)
		{
			Candidates.Sort([](const FResolvedCandidate& Left, const FResolvedCandidate& Right)
			{
				const int32 IdentityComparison = Left.Value.SourceIdentity.CanonicalKey.Compare(Right.Value.SourceIdentity.CanonicalKey, ESearchCase::CaseSensitive);
				if (IdentityComparison != 0) return IdentityComparison < 0;
				return Left.Descriptor.ProviderId < Right.Descriptor.ProviderId;
			});
		}

		void SetResolvedResult(const FResolvedCandidate& Candidate, FResult& OutResult)
		{
			OutResult.Status = TEXT("resolved");
			OutResult.ErrorCode = TEXT("native_default_resolved");
			OutResult.ProviderId = Candidate.Descriptor.ProviderId;
			OutResult.SourceKind = Candidate.Descriptor.SourceKind;
			OutResult.SourceIdentity = Candidate.Value.SourceIdentity;
			OutResult.Value = Candidate.Value.Value;
			OutResult.PropertyValue = Candidate.Value.PropertyValue;
			OutResult.ValueText = Candidate.Value.ValueText;
			OutResult.Provenance = Candidate.Value.Provenance;
		}
	}

	FString OperationToString(EOperation Operation)
	{
		switch (Operation)
		{
		case EOperation::Update: return TEXT("update");
		case EOperation::Patch: return TEXT("patch");
		case EOperation::NestedPatch: return TEXT("nested_patch");
		case EOperation::Create: return TEXT("create");
		case EOperation::Replace: return TEXT("replace");
		case EOperation::OwnedObjectCreate: return TEXT("owned_object_create");
		case EOperation::GeneratedAfterApply: return TEXT("generated_after_apply");
		case EOperation::PostCreateReadback: return TEXT("post_create_readback");
		case EOperation::InspectDefault: return TEXT("inspect_default");
		case EOperation::Reset: return TEXT("reset");
		default: return TEXT("unknown");
		}
	}

	FString SourceKindToString(UeAgentAuthoringSemanticRegistry::EDefaultSourceKind SourceKind)
	{
		using UeAgentAuthoringSemanticRegistry::EDefaultSourceKind;
		switch (SourceKind)
		{
		case EDefaultSourceKind::ExistingInstance: return TEXT("existing_instance");
		case EDefaultSourceKind::Factory: return TEXT("factory");
		case EDefaultSourceKind::Constructor: return TEXT("constructor_cdo_archetype_template");
		case EDefaultSourceKind::StructInitialization: return TEXT("struct_initialization");
		case EDefaultSourceKind::Schema: return TEXT("schema");
		case EDefaultSourceKind::DomainNative: return TEXT("domain_native");
		case EDefaultSourceKind::PostCreate: return TEXT("post_create");
		default: return TEXT("none");
		}
	}

	int32 GetSourcePriority(UeAgentAuthoringSemanticRegistry::EDefaultSourceKind SourceKind)
	{
		using UeAgentAuthoringSemanticRegistry::EDefaultSourceKind;
		switch (SourceKind)
		{
		case EDefaultSourceKind::ExistingInstance: return 0;
		case EDefaultSourceKind::Factory: return 10;
		case EDefaultSourceKind::Constructor: return 20;
		case EDefaultSourceKind::StructInitialization: return 30;
		case EDefaultSourceKind::Schema: return 40;
		case EDefaultSourceKind::DomainNative: return 50;
		case EDefaultSourceKind::PostCreate: return 60;
		default: return MAX_int32;
		}
	}

	struct FScratchScope::FImpl
	{
		struct FTracked
		{
			TStrongObjectPtr<UObject> Object;
			bool bWasRooted = false;
		};

		TArray<FTracked> Objects;
		TMap<TWeakObjectPtr<UPackage>, bool> PackageDirtyStates;
		bool bCleaned = false;
	};

	FScratchScope::FScratchScope()
		: Impl(MakeUnique<FImpl>())
	{
		ActiveScratchScopes.Increment();
	}

	FScratchScope::~FScratchScope()
	{
		Cleanup();
		ActiveScratchScopes.Decrement();
	}

	UPackage* FScratchScope::CreateScratchPackage(FString& OutError)
	{
		if (!Impl || Impl->bCleaned)
		{
			OutError = TEXT("native_default_scratch_scope_closed");
			return nullptr;
		}
		const FString PackageName = FString::Printf(TEXT("/Temp/UeAgentInterface/Scratch/%s"), *FGuid::NewGuid().ToString(EGuidFormats::Digits));
		UPackage* Package = CreatePackage(*PackageName);
		if (!Package)
		{
			OutError = TEXT("native_default_scratch_package_create_failed");
			return nullptr;
		}
		Package->SetFlags(RF_Transient);
		Package->SetDirtyFlag(false);
		FImpl::FTracked& Tracked = Impl->Objects.AddDefaulted_GetRef();
		Tracked.Object.Reset(Package);
		Tracked.bWasRooted = Package->IsRooted();
		TrackedScratchObjects.Increment();
		OutError.Reset();
		return Package;
	}

	UObject* FScratchScope::CreateObject(
		UClass* ObjectClass,
		UObject* Outer,
		UObject* ArchetypeOrTemplate,
		FString& OutError)
	{
		if (!Impl || Impl->bCleaned)
		{
			OutError = TEXT("native_default_scratch_scope_closed");
			return nullptr;
		}
		if (!ObjectClass || ObjectClass->HasAnyClassFlags(CLASS_Abstract | CLASS_Deprecated | CLASS_NewerVersionExists))
		{
			OutError = TEXT("native_default_scratch_class_invalid");
			return nullptr;
		}
		if (ArchetypeOrTemplate && !ArchetypeOrTemplate->IsA(ObjectClass))
		{
			OutError = TEXT("native_default_scratch_template_class_mismatch");
			return nullptr;
		}
		UObject* ResolvedOuter = Outer ? Outer : GetTransientPackage();
		if (UPackage* Package = ResolvedOuter->GetOutermost())
		{
			Impl->PackageDirtyStates.FindOrAdd(Package, Package->IsDirty());
		}
		UObject* Object = NewObject<UObject>(ResolvedOuter, ObjectClass, NAME_None, RF_Transient, ArchetypeOrTemplate);
		if (!Object)
		{
			OutError = TEXT("native_default_scratch_object_create_failed");
			return nullptr;
		}
		FImpl::FTracked& Tracked = Impl->Objects.AddDefaulted_GetRef();
		Tracked.Object.Reset(Object);
		Tracked.bWasRooted = Object->IsRooted();
		TrackedScratchObjects.Increment();
		OutError.Reset();
		return Object;
	}

	bool FScratchScope::AdoptObjectFromScratchPackage(UObject* Object, FString& OutError)
	{
		if (!Impl || Impl->bCleaned)
		{
			OutError = TEXT("native_default_scratch_scope_closed");
			return false;
		}
		if (!Object)
		{
			OutError = TEXT("native_default_scratch_object_invalid");
			return false;
		}
		if (Contains(Object))
		{
			OutError.Reset();
			return true;
		}
		bool bOwnedByTrackedOuter = false;
		for (const UObject* Outer = Object->GetOuter(); Outer; Outer = Outer->GetOuter())
		{
			if (Contains(Outer))
			{
				bOwnedByTrackedOuter = true;
				break;
			}
		}
		if (!bOwnedByTrackedOuter)
		{
			OutError = TEXT("native_default_scratch_object_not_owned");
			return false;
		}
		FImpl::FTracked& Tracked = Impl->Objects.AddDefaulted_GetRef();
		Tracked.Object.Reset(Object);
		Tracked.bWasRooted = Object->IsRooted();
		TrackedScratchObjects.Increment();
		OutError.Reset();
		return true;
	}

	bool FScratchScope::Contains(const UObject* Object) const
	{
		return Impl && Impl->Objects.ContainsByPredicate([Object](const FImpl::FTracked& Tracked)
		{
			return Tracked.Object.Get() == Object;
		});
	}

	int32 FScratchScope::NumTrackedObjects() const
	{
		return Impl ? Impl->Objects.Num() : 0;
	}

	void FScratchScope::Cleanup()
	{
		if (!Impl || Impl->bCleaned) return;
		for (int32 Index = Impl->Objects.Num() - 1; Index >= 0; --Index)
		{
			FImpl::FTracked& Tracked = Impl->Objects[Index];
			UObject* Object = Tracked.Object.Get();
			if (Object)
			{
				if (Object->IsRooted() && !Tracked.bWasRooted) Object->RemoveFromRoot();
				if (UPackage* Package = Cast<UPackage>(Object)) Package->SetDirtyFlag(false);
				Object->ClearFlags(RF_Public | RF_Standalone | RF_Transactional);
				Object->SetFlags(RF_Transient);
				Object->MarkAsGarbage();
			}
			Tracked.Object.Reset();
			TrackedScratchObjects.Decrement();
		}
		Impl->Objects.Reset();
		for (const TPair<TWeakObjectPtr<UPackage>, bool>& Pair : Impl->PackageDirtyStates)
		{
			if (UPackage* Package = Pair.Key.Get()) Package->SetDirtyFlag(Pair.Value);
		}
		Impl->PackageDirtyStates.Reset();
		Impl->bCleaned = true;
	}

	int32 FScratchScope::GetActiveScopeCount()
	{
		return ActiveScratchScopes.GetValue();
	}

	int32 FScratchScope::GetTrackedObjectCount()
	{
		return TrackedScratchObjects.GetValue();
	}

	bool FProviderDescriptor::operator==(const FProviderDescriptor& Other) const
	{
		return ProviderId == Other.ProviderId
			&& SourceKind == Other.SourceKind
			&& TargetKinds == Other.TargetKinds
			&& CapabilityTags == Other.CapabilityTags
			&& bRequiresScratch == Other.bRequiresScratch;
	}

	FProviderRegistry& FProviderRegistry::Get()
	{
		static FProviderRegistry Registry;
		static const bool bBuiltInsRegistered = []()
		{
			FString Error;
			return RegisterBuiltInProviders(Registry, Error);
		}();
		ensureAlwaysMsgf(bBuiltInsRegistered, TEXT("Native default built-in provider registration failed."));
		return Registry;
	}

	bool FProviderRegistry::RegisterProvider(
		const FProviderDescriptor& Descriptor,
		FProbe Probe,
		ERegistrationResult& OutResult,
		FString& OutError)
	{
		const FProviderDescriptor Canonical = Normalize(Descriptor);
		if (!ValidateProvider(Canonical, Probe, OutError)) return false;
		FWriteScopeLock Lock(RegistryLock);
		if (const FEntry* Existing = Providers.Find(Canonical.ProviderId))
		{
			if (Existing->Descriptor != Canonical)
			{
				OutError = FString::Printf(TEXT("native_default_provider_conflict_%s"), *Canonical.ProviderId);
				return false;
			}
			OutResult = ERegistrationResult::AlreadyRegistered;
			OutError.Reset();
			return true;
		}
		FEntry& Entry = Providers.Add(Canonical.ProviderId);
		Entry.Descriptor = Canonical;
		Entry.Probe = MoveTemp(Probe);
		OutResult = ERegistrationResult::Registered;
		OutError.Reset();
		return true;
	}

	bool FProviderRegistry::FindProvider(const FString& ProviderId, FProviderDescriptor& OutDescriptor) const
	{
		FReadScopeLock Lock(RegistryLock);
		if (const FEntry* Entry = Providers.Find(NormalizeId(ProviderId)))
		{
			OutDescriptor = Entry->Descriptor;
			return true;
		}
		return false;
	}

	TArray<FProviderDescriptor> FProviderRegistry::ListProviders() const
	{
		FReadScopeLock Lock(RegistryLock);
		TArray<FProviderDescriptor> Result;
		for (const TPair<FString, FEntry>& Pair : Providers) Result.Add(Pair.Value.Descriptor);
		Result.Sort([](const FProviderDescriptor& Left, const FProviderDescriptor& Right)
		{
			const int32 PriorityComparison = GetSourcePriority(Left.SourceKind) - GetSourcePriority(Right.SourceKind);
			if (PriorityComparison != 0) return PriorityComparison < 0;
			return Left.ProviderId < Right.ProviderId;
		});
		return Result;
	}

	int32 FProviderRegistry::GetProviderCount() const
	{
		FReadScopeLock Lock(RegistryLock);
		return Providers.Num();
	}

	bool FProviderRegistry::CopyProbe(const FString& ProviderId, FProbe& OutProbe) const
	{
		FReadScopeLock Lock(RegistryLock);
		if (const FEntry* Entry = Providers.Find(NormalizeId(ProviderId)))
		{
			OutProbe = Entry->Probe;
			return true;
		}
		return false;
	}

	bool FResult::IsResolved() const
	{
		return Status == TEXT("resolved") && ErrorCode == TEXT("native_default_resolved");
	}

	TSharedRef<FJsonObject> FResult::ToJsonObject(bool bIncludeValue) const
	{
		TSharedRef<FJsonObject> Object = MakeShared<FJsonObject>();
		Object->SetStringField(TEXT("status"), Status);
		Object->SetStringField(TEXT("error_code"), ErrorCode);
		if (!ErrorDetail.IsEmpty()) Object->SetStringField(TEXT("error_detail"), ErrorDetail);
		Object->SetStringField(TEXT("operation"), OperationToString(Operation));
		Object->SetBoolField(TEXT("preserve_existing"), bPreserveExisting);
		Object->SetBoolField(TEXT("deferred_until_post_create"), bDeferredUntilPostCreate);
		if (TargetIdentity.IsValid()) Object->SetObjectField(TEXT("target_identity"), TargetIdentity.ToJsonObject());
		if (!ProviderId.IsEmpty()) Object->SetStringField(TEXT("provider_id"), ProviderId);
		if (SourceKind != UeAgentAuthoringSemanticRegistry::EDefaultSourceKind::None) Object->SetStringField(TEXT("source_kind"), SourceKindToString(SourceKind));
		if (SourceIdentity.IsValid()) Object->SetObjectField(TEXT("source_identity"), SourceIdentity.ToJsonObject());
		if (!Provenance.IsEmpty()) Object->SetStringField(TEXT("provenance"), Provenance);
		if (bIncludeValue && Value.IsValid()) Object->SetField(TEXT("value"), Value);

		TArray<TSharedPtr<FJsonValue>> AttemptValues;
		for (const FAttempt& Attempt : Attempts)
		{
			TSharedRef<FJsonObject> AttemptObject = MakeShared<FJsonObject>();
			AttemptObject->SetStringField(TEXT("provider_id"), Attempt.ProviderId);
			AttemptObject->SetStringField(TEXT("source_kind"), SourceKindToString(Attempt.SourceKind));
			AttemptObject->SetStringField(TEXT("outcome"), Attempt.Outcome);
			if (!Attempt.ErrorCode.IsEmpty()) AttemptObject->SetStringField(TEXT("error_code"), Attempt.ErrorCode);
			AttemptValues.Add(MakeShared<FJsonValueObject>(AttemptObject));
		}
		Object->SetArrayField(TEXT("attempts"), AttemptValues);

		TArray<TSharedPtr<FJsonValue>> CandidateValues;
		for (const FDiagnosticCandidate& Candidate : Candidates)
		{
			TSharedRef<FJsonObject> CandidateObject = MakeShared<FJsonObject>();
			CandidateObject->SetStringField(TEXT("provider_id"), Candidate.ProviderId);
			CandidateObject->SetStringField(TEXT("source_kind"), SourceKindToString(Candidate.SourceKind));
			CandidateObject->SetObjectField(TEXT("source_identity"), Candidate.SourceIdentity.ToJsonObject());
			if (!Candidate.Provenance.IsEmpty()) CandidateObject->SetStringField(TEXT("provenance"), Candidate.Provenance);
			CandidateValues.Add(MakeShared<FJsonValueObject>(CandidateObject));
		}
		Object->SetArrayField(TEXT("candidates"), CandidateValues);
		return Object;
	}

	bool RegisterBuiltInProviders(FProviderRegistry& Registry, FString& OutError)
	{
		using UeAgentAuthoringSemanticRegistry::EDefaultSourceKind;
		ERegistrationResult Result = ERegistrationResult::Registered;
		FProviderDescriptor Existing;
		Existing.ProviderId = TEXT("core.existing_instance");
		Existing.SourceKind = EDefaultSourceKind::ExistingInstance;
		Existing.TargetKinds = { TEXT("*") };
		Existing.CapabilityTags = { TEXT("property"), TEXT("nested_patch") };
		if (!Registry.RegisterProvider(Existing, ProbeExisting, Result, OutError)) return false;

		FProviderDescriptor Constructor;
		Constructor.ProviderId = TEXT("core.constructor");
		Constructor.SourceKind = EDefaultSourceKind::Constructor;
		Constructor.TargetKinds = { TEXT("*") };
		Constructor.CapabilityTags = { TEXT("archetype"), TEXT("cdo"), TEXT("constructor"), TEXT("property"), TEXT("template") };
		if (!Registry.RegisterProvider(Constructor, ProbeConstructor, Result, OutError)) return false;

		FProviderDescriptor Struct;
		Struct.ProviderId = TEXT("core.struct_initialization");
		Struct.SourceKind = EDefaultSourceKind::StructInitialization;
		Struct.TargetKinds = { TEXT("*") };
		Struct.CapabilityTags = { TEXT("property"), TEXT("uscriptstruct") };
		if (!Registry.RegisterProvider(Struct, ProbeStructInitialization, Result, OutError)) return false;

		OutError.Reset();
		return true;
	}

	bool BuildPropertyRequest(
		UObject* Object,
		const FString& PropertyPath,
		EOperation Operation,
		bool bRequired,
		FRequest& OutRequest,
		UeAgentAuthoringTargetLocator::FResolveResult& OutTargetResult)
	{
		OutRequest = FRequest();
		UeAgentAuthoringTargetLocator::FResolvedPropertyTarget Target;
		if (!UeAgentAuthoringTargetLocator::ResolvePropertyPath(Object, PropertyPath, Target, OutTargetResult)) return false;
		OutRequest.Operation = Operation;
		OutRequest.TargetIdentity = Target.Identity;
		OutRequest.TargetQuery.ModelId = Target.Identity.ModelId;
		OutRequest.TargetQuery.TargetKind = Target.Identity.TargetKind;
		OutRequest.TargetQuery.CanonicalKey = Target.Identity.CanonicalKey;
		OutRequest.Property = Target.Property;
		OutRequest.PropertyPath = PropertyPath;
		OutRequest.ExistingOwner = Target.NotificationOwner ? Target.NotificationOwner : Object;
		OutRequest.ExistingValuePtr = Target.ValuePtr;
		OutRequest.ObjectClass = Object ? Object->GetClass() : nullptr;
		OutRequest.ProposedOuter = Object ? Object->GetOuter() : nullptr;
		OutRequest.ArchetypeOrTemplate = Object ? Object->GetArchetype() : nullptr;
		OutRequest.bRequired = bRequired;
		return true;
	}

	bool Resolve(const FRequest& Request, FResult& OutResult, const FProviderRegistry* Registry)
	{
		OutResult = FResult();
		OutResult.Operation = Request.Operation;
		OutResult.TargetIdentity = Request.TargetIdentity;
		if (!ValidateDefaultTarget(Request, OutResult)) return false;

		if (Request.Operation == EOperation::Update || Request.Operation == EOperation::Patch)
		{
			OutResult.Status = TEXT("preserve_existing");
			OutResult.ErrorCode = TEXT("native_default_preserve_existing");
			OutResult.bPreserveExisting = true;
			return true;
		}
		if (Request.Operation == EOperation::GeneratedAfterApply)
		{
			OutResult.Status = TEXT("deferred");
			OutResult.ErrorCode = TEXT("native_default_deferred_post_create");
			OutResult.bDeferredUntilPostCreate = true;
			return true;
		}

		const FProviderRegistry& ResolvedRegistry = Registry ? *Registry : FProviderRegistry::Get();
		TSet<FString> AllowedProviderIds;
		for (const FString& ProviderId : Request.ProviderIds)
		{
			const FString CanonicalProviderId = NormalizeId(ProviderId);
			FProviderDescriptor Ignored;
			if (!ResolvedRegistry.FindProvider(CanonicalProviderId, Ignored))
			{
				OutResult.Status = TEXT("failed");
				OutResult.ErrorCode = TEXT("native_default_provider_unknown");
				OutResult.ErrorDetail = CanonicalProviderId;
				return false;
			}
			AllowedProviderIds.Add(CanonicalProviderId);
		}

		const TArray<FProviderDescriptor> Providers = ResolvedRegistry.ListProviders();
		for (const UeAgentAuthoringSemanticRegistry::EDefaultSourceKind SourceKind : SourceOrderForOperation(Request.Operation))
		{
			TArray<FResolvedCandidate> ResolvedCandidates;
			for (const FProviderDescriptor& Descriptor : Providers)
			{
				if (Descriptor.SourceKind != SourceKind || !SupportsTargetKind(Descriptor, Request.TargetIdentity.TargetKind)) continue;
				if (!AllowedProviderIds.IsEmpty() && !AllowedProviderIds.Contains(Descriptor.ProviderId)) continue;

				FProbe Probe;
				if (!ResolvedRegistry.CopyProbe(Descriptor.ProviderId, Probe)) continue;
				FScratchScope Scratch;
				FProbeValue ProbeValue;
				FString ProbeError;
				const EProbeOutcome Outcome = Probe(Request, Scratch, ProbeValue, ProbeError);
				FAttempt& Attempt = OutResult.Attempts.AddDefaulted_GetRef();
				Attempt.ProviderId = Descriptor.ProviderId;
				Attempt.SourceKind = Descriptor.SourceKind;
				Attempt.Outcome = ProbeOutcomeToString(Outcome);
				Attempt.ErrorCode = ProbeError;

				if (Outcome == EProbeOutcome::Failed)
				{
					OutResult.Status = TEXT("failed");
					OutResult.ErrorCode = TEXT("native_default_probe_failed");
					OutResult.ErrorDetail = ProbeError;
					return false;
				}
				if (Outcome != EProbeOutcome::Resolved) continue;
				if (Descriptor.bRequiresScratch && Scratch.NumTrackedObjects() == 0)
				{
					OutResult.Status = TEXT("failed");
					OutResult.ErrorCode = TEXT("native_default_probe_failed");
					OutResult.ErrorDetail = TEXT("native_default_provider_scratch_required");
					return false;
				}
				if (!ProbeValue.Value.IsValid())
				{
					OutResult.Status = TEXT("failed");
					OutResult.ErrorCode = TEXT("native_default_probe_value_missing");
					return false;
				}
				if (Request.Property && !ProbeValue.PropertyValue.IsValid())
				{
					FString DecodeError;
					ProbeValue.PropertyValue = FPropertyValueStorage::CreateInitialized(Request.Property, DecodeError);
					if (!ProbeValue.PropertyValue.IsValid()
						|| !UeAgentPropertyValueCodec::Decode(
							Request.Property,
							ProbeValue.Value,
							ProbeValue.PropertyValue->GetMutableValuePtr(),
							Request.ExistingOwner,
							UeAgentPropertyValueCodec::FOptions(),
							DecodeError))
					{
						OutResult.Status = TEXT("failed");
						OutResult.ErrorCode = TEXT("native_default_provider_value_invalid");
						OutResult.ErrorDetail = DecodeError;
						return false;
					}
				}
				if (Request.Property && ProbeValue.PropertyValue.IsValid())
				{
					UeAgentPropertyValueCodec::FOptions CanonicalOptions;
					CanonicalOptions.MaxDepth = -1;
					ProbeValue.Value = UeAgentPropertyValueCodec::Encode(
						Request.Property,
						ProbeValue.PropertyValue->GetValuePtr(),
						Request.ExistingOwner,
						CanonicalOptions);
					FString CanonicalError;
					if (!UeAgentPropertyValueCodec::IsReliableCanonicalValue(
						Request.Property,
						ProbeValue.Value,
						&CanonicalError))
					{
						OutResult.Status = TEXT("failed");
						OutResult.ErrorCode = TEXT("native_default_provider_value_invalid");
						OutResult.ErrorDetail = CanonicalError;
						return false;
					}
					Request.Property->ExportTextItem_Direct(
						ProbeValue.ValueText,
						ProbeValue.PropertyValue->GetValuePtr(),
						nullptr,
						Request.ExistingOwner,
						PPF_None);
				}
				if (!ProbeValue.SourceIdentity.IsValid()) ProbeValue.SourceIdentity = Request.TargetIdentity;
				FResolvedCandidate& Candidate = ResolvedCandidates.AddDefaulted_GetRef();
				Candidate.Descriptor = Descriptor;
				Candidate.Value = MoveTemp(ProbeValue);
			}

			if (ResolvedCandidates.IsEmpty()) continue;
			SortCandidates(ResolvedCandidates);
			TArray<FResolvedCandidate> UniqueCandidates;
			for (FResolvedCandidate& Candidate : ResolvedCandidates)
			{
				if (!UniqueCandidates.IsEmpty()
					&& UniqueCandidates.Last().Value.SourceIdentity.CanonicalKey == Candidate.Value.SourceIdentity.CanonicalKey
					&& JsonValuesEqual(UniqueCandidates.Last().Value.Value, Candidate.Value.Value))
				{
					continue;
				}
				UniqueCandidates.Add(MoveTemp(Candidate));
			}

			if (UniqueCandidates.Num() > 1)
			{
				OutResult.Status = TEXT("ambiguous");
				OutResult.ErrorCode = TEXT("native_default_ambiguous");
				for (const FResolvedCandidate& Candidate : UniqueCandidates)
				{
					FDiagnosticCandidate& Diagnostic = OutResult.Candidates.AddDefaulted_GetRef();
					Diagnostic.ProviderId = Candidate.Descriptor.ProviderId;
					Diagnostic.SourceKind = Candidate.Descriptor.SourceKind;
					Diagnostic.SourceIdentity = Candidate.Value.SourceIdentity;
					Diagnostic.Provenance = Candidate.Value.Provenance;
				}
				return false;
			}

			SetResolvedResult(UniqueCandidates[0], OutResult);
			return true;
		}

		if (Request.bRequired)
		{
			OutResult.Status = TEXT("failed");
			OutResult.ErrorCode = TEXT("native_default_required_missing");
			return false;
		}
		OutResult.Status = TEXT("unavailable");
		OutResult.ErrorCode = TEXT("native_default_unavailable");
		return true;
	}
}
