// Copyright Epic Games, Inc. All Rights Reserved.

#include "Utilities/AuthoringCore/UeAgentAuthoringNativeDefaults.h"
#include "Core/StateSemantic/UeAgentCanonicalValueJson.h"

#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "HAL/ThreadSafeCounter.h"
#include "Misc/Guid.h"
#include "Misc/ScopeExit.h"
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

		FDiagnostic NativeFailure(const TCHAR* Code, const TCHAR* Reason, const FString& Field = FString(), const FString& Name = FString())
		{
			auto Error = FDiagnostic::Failure(Code, Reason, TEXT("native_default"));
			Error.SourceKind = TEXT("native_result");
			Error.PropertyPath = Field;
			Error.CandidateName = Name;
			return Error;
		}
		TSharedPtr<FJsonValue> ProviderValue(const FString& Value) { return MakeShared<FJsonValueString>(Value); }
		TSharedPtr<FJsonValue> ProviderValue(bool Value) { return MakeShared<FJsonValueBoolean>(Value); }
		TSharedPtr<FJsonValue> ProviderValue(int32 Value) { return MakeShared<FJsonValueNumber>(Value); }
		template<typename T>
		bool ProviderValuesEqual(const T& Expected, const T& Actual, const FString& Path, FDiagnostic* Error)
		{
			if (Expected == Actual) return true;
			if (Error)
			{
				Error->PropertyPath = Path;
				Error->ExpectedValue = ProviderValue(Expected);
				Error->ActualValue = ProviderValue(Actual);
			}
			return false;
		}
		bool ProviderValuesEqual(const TArray<FString>& Expected, const TArray<FString>& Actual, const FString& Path, FDiagnostic* Error)
		{
			if (!ProviderValuesEqual(Expected.Num(), Actual.Num(), Path + TEXT("/count"), Error)) return false;
			for (int32 Index = 0; Index < Expected.Num(); ++Index)
				if (!ProviderValuesEqual(Expected[Index], Actual[Index], Path + TEXT("/") + FString::FromInt(Index), Error)) return false;
			return true;
		}
		bool ProviderDescriptorsEqual(const FProviderDescriptor& Expected, const FProviderDescriptor& Actual, FDiagnostic* Error)
		{
			return ProviderValuesEqual(Expected.ProviderId, Actual.ProviderId, TEXT("/provider_id"), Error)
				&& ProviderValuesEqual(static_cast<int32>(Expected.SourceKind), static_cast<int32>(Actual.SourceKind), TEXT("/source_kind"), Error)
				&& ProviderValuesEqual(Expected.TargetKinds, Actual.TargetKinds, TEXT("/target_kinds"), Error)
				&& ProviderValuesEqual(Expected.CapabilityTags, Actual.CapabilityTags, TEXT("/capability_tags"), Error)
				&& ProviderValuesEqual(Expected.bRequiresScratch, Actual.bRequiresScratch, TEXT("/requires_scratch"), Error);
		}

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

		bool ValidateProvider(const FProviderDescriptor& Descriptor, const FProbe& Probe, UE::UAI::Contract::FDiagnostic& OutError)
		{
			if (Descriptor.ProviderId.IsEmpty()) OutError = NativeFailure(TEXT("native_default_provider_id_missing"), TEXT("The native default provider identifier is empty."), TEXT("provider_id"));
			else if (Descriptor.SourceKind == UeAgentAuthoringSemanticRegistry::EDefaultSourceKind::None) OutError = NativeFailure(TEXT("native_default_provider_source_missing"), TEXT("The native default provider has no declared source kind."), TEXT("source_kind"));
			else if (Descriptor.TargetKinds.IsEmpty()) OutError = NativeFailure(TEXT("native_default_provider_targets_missing"), TEXT("The native default provider declares no supported target kinds."), TEXT("target_kinds"));
			else if (!Probe) OutError = NativeFailure(TEXT("native_default_provider_probe_missing"), TEXT("The native default provider has no probe callback."), TEXT("probe"));
			else
			{
				OutError.Reset();
				return true;
			}
			OutError.Stage = TEXT("contract");
			OutError.CandidateName = Descriptor.ProviderId;
			OutError.ExpectedValuePresent = true;
			OutError.ActualValuePresent = false;
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

		bool ValidateDefaultTarget(const FRequest& Request, FResult& OutResult)
		{
			if (!Request.TargetIdentity.IsValid())
			{
				OutResult.Status = TEXT("failed");
				const auto& Identity = Request.TargetIdentity;
				const TCHAR* Field = Identity.ModelId.IsEmpty() ? TEXT("model_id") : Identity.TargetKind.IsEmpty() ? TEXT("target_kind")
					: Identity.KeyPattern.IsEmpty() ? TEXT("key_pattern") : TEXT("canonical_key");
				OutResult.Diagnostic = NativeFailure(TEXT("native_default_target_identity_invalid"), TEXT("The native default target identity has an empty required field."), Field, Identity.TargetKind);
				OutResult.Diagnostic.ExpectedValuePresent = true;
				OutResult.Diagnostic.ActualValuePresent = false;
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
				OutResult.Diagnostic = TargetResult.Diagnostic;
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
			FDiagnostic& OutError)
		{
			if (!Property || !ValuePtr)
			{
				OutError = NativeFailure(TEXT("native_default_property_value_unavailable"), !Property
					? TEXT("Reading a native default requires a property descriptor.") : TEXT("The native default property has no value storage."),
					!Property ? TEXT("property") : Request.PropertyPath, Property ? Property->GetName() : FString());
				OutError.ExpectedValuePresent = true;
				OutError.ActualValuePresent = false;
				return false;
			}

			OutValue.Value = UeAgentPropertyValueCodec::Encode(
				Property,
				ValuePtr,
				Owner,
				UeAgentPropertyValueCodec::FOptions());
			if (!OutValue.Value.IsValid())
			{
				OutError = NativeFailure(TEXT("native_default_property_encode_failed"), TEXT("Encoding the native default property produced no value."), Request.PropertyPath, Property->GetName());
				OutError.ExpectedValuePresent = true;
				OutError.ActualValuePresent = false;
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
			FDiagnostic& OutError)
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
			FDiagnostic& OutError)
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
				OutError = TargetResult.Diagnostic;
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
			FDiagnostic& OutError)
		{
			if (!Request.Property || !Request.StructType) return EProbeOutcome::Unavailable;
			FStructOnScope InitializedStruct(Request.StructType);
			if (!InitializedStruct.IsValid())
			{
				OutError = NativeFailure(TEXT("native_default_struct_initialize_failed"), TEXT("Initializing the native struct for default-value inspection failed."), Request.PropertyPath, Request.StructType->GetName());
				OutError.InternalContext = Request.StructType->GetPathName();
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
			OutResult.Diagnostic.Reset();
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

	UPackage* FScratchScope::CreateScratchPackage(FDiagnostic& OutError)
	{
		if (!Impl || Impl->bCleaned)
		{
			OutError = NativeFailure(TEXT("native_default_scratch_scope_closed"), TEXT("A scratch package cannot be created after its scratch scope has closed."), TEXT("scratch_scope"));
			OutError.ExpectedValue = MakeShared<FJsonValueString>(TEXT("open"));
			OutError.ActualValue = MakeShared<FJsonValueString>(TEXT("closed"));
			return nullptr;
		}
		const FString PackageName = FString::Printf(TEXT("/Temp/UeAgentInterface/Scratch/%s"), *FGuid::NewGuid().ToString(EGuidFormats::Digits));
		UPackage* Package = CreatePackage(*PackageName);
		if (!Package)
		{
			OutError = NativeFailure(TEXT("native_default_scratch_package_create_failed"), TEXT("Unreal could not create a temporary package for native default inspection."), TEXT("scratch_package"));
			OutError.InternalContext = PackageName;
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
		FDiagnostic& OutError)
	{
		if (!Impl || Impl->bCleaned)
		{
			OutError = NativeFailure(TEXT("native_default_scratch_scope_closed"), TEXT("A scratch object cannot be created after its scratch scope has closed."), TEXT("scratch_scope"));
			OutError.ExpectedValue = MakeShared<FJsonValueString>(TEXT("open"));
			OutError.ActualValue = MakeShared<FJsonValueString>(TEXT("closed"));
			return nullptr;
		}
		if (!ObjectClass || ObjectClass->HasAnyClassFlags(CLASS_Abstract | CLASS_Deprecated | CLASS_NewerVersionExists))
		{
			const TCHAR* Reason = !ObjectClass ? TEXT("Creating a scratch object requires a native class.")
				: ObjectClass->HasAnyClassFlags(CLASS_Abstract) ? TEXT("An abstract class cannot be instantiated for native default inspection.")
				: ObjectClass->HasAnyClassFlags(CLASS_Deprecated) ? TEXT("A deprecated class cannot be instantiated for native default inspection.")
				: TEXT("The native class has a newer version and cannot be used for scratch construction.");
			OutError = NativeFailure(TEXT("native_default_scratch_class_invalid"), Reason, TEXT("object_class"), ObjectClass ? ObjectClass->GetName() : FString());
			OutError.ExpectedValuePresent = true;
			OutError.ActualValuePresent = ObjectClass != nullptr;
			if (ObjectClass) OutError.InternalContext = ObjectClass->GetPathName();
			return nullptr;
		}
		if (ArchetypeOrTemplate && !ArchetypeOrTemplate->IsA(ObjectClass))
		{
			OutError = NativeFailure(TEXT("native_default_scratch_template_class_mismatch"), TEXT("The scratch template is not an instance of the requested native class."), TEXT("template_class"));
			OutError.ExpectedValue = MakeShared<FJsonValueString>(ObjectClass->GetName());
			OutError.ActualValue = MakeShared<FJsonValueString>(ArchetypeOrTemplate->GetClass()->GetName());
			OutError.InternalContext = ArchetypeOrTemplate->GetPathName();
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
			OutError = NativeFailure(TEXT("native_default_scratch_object_create_failed"), TEXT("Unreal could not instantiate the scratch object for native default inspection."), TEXT("object_class"), ObjectClass->GetName());
			OutError.InternalContext = ObjectClass->GetPathName();
			return nullptr;
		}
		FImpl::FTracked& Tracked = Impl->Objects.AddDefaulted_GetRef();
		Tracked.Object.Reset(Object);
		Tracked.bWasRooted = Object->IsRooted();
		TrackedScratchObjects.Increment();
		OutError.Reset();
		return Object;
	}

	bool FScratchScope::AdoptObjectFromScratchPackage(UObject* Object, FDiagnostic& OutError)
	{
		if (!Impl || Impl->bCleaned)
		{
			OutError = NativeFailure(TEXT("native_default_scratch_scope_closed"), TEXT("An object cannot be adopted after its scratch scope has closed."), TEXT("scratch_scope"));
			OutError.ExpectedValue = MakeShared<FJsonValueString>(TEXT("open"));
			OutError.ActualValue = MakeShared<FJsonValueString>(TEXT("closed"));
			return false;
		}
		if (!Object)
		{
			OutError = NativeFailure(TEXT("native_default_scratch_object_invalid"), TEXT("Adopting a scratch object requires an existing native object."), TEXT("scratch_object"));
			OutError.ExpectedValuePresent = true;
			OutError.ActualValuePresent = false;
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
			OutError = NativeFailure(TEXT("native_default_scratch_object_not_owned"), TEXT("The object has no outer owned by this scratch scope."), TEXT("scratch_owner"), Object->GetClass()->GetName());
			OutError.InternalContext = Object->GetPathName();
			OutError.ExpectedValuePresent = true;
			OutError.ActualValuePresent = false;
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
		return ProviderDescriptorsEqual(*this, Other, nullptr);
	}

	FProviderRegistry& FProviderRegistry::Get()
	{
		static FProviderRegistry Registry;
		static const bool bBuiltInsRegistered = []()
		{
			UE::UAI::Contract::FDiagnostic Error;
			return RegisterBuiltInProviders(Registry, Error);
		}();
		ensureAlwaysMsgf(bBuiltInsRegistered, TEXT("Native default built-in provider registration failed."));
		return Registry;
	}

	bool FProviderRegistry::RegisterProvider(
		const FProviderDescriptor& Descriptor,
		FProbe Probe,
		ERegistrationResult& OutResult,
		UE::UAI::Contract::FDiagnostic& OutError)
	{
		const FProviderDescriptor Canonical = Normalize(Descriptor);
		if (!ValidateProvider(Canonical, Probe, OutError)) return false;
		FWriteScopeLock Lock(RegistryLock);
		if (const FEntry* Existing = Providers.Find(Canonical.ProviderId))
		{
			if (Existing->Descriptor != Canonical)
			{
				OutError = NativeFailure(TEXT("native_default_provider_conflict"), TEXT("The native default provider descriptor differs from its existing registration."), FString(), Canonical.ProviderId);
				OutError.Stage = TEXT("contract");
				ProviderDescriptorsEqual(Existing->Descriptor, Canonical, &OutError);
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
		return Status == TEXT("resolved") && Diagnostic.IsEmpty();
	}

	TSharedRef<FJsonObject> FResult::ToJsonObject(bool bIncludeValue) const
	{
		TSharedRef<FJsonObject> Object = MakeShared<FJsonObject>();
		Object->SetStringField(TEXT("status"), Status);
		const FString ResultCode = !Diagnostic.IsEmpty() ? Diagnostic.Code
			: Status == TEXT("deferred") ? TEXT("native_default_deferred_post_create")
			: FString(TEXT("native_default_")) + Status;
		Object->SetStringField(TEXT("error_code"), ResultCode);
		if (!Diagnostic.IsEmpty()) Object->SetObjectField(TEXT("diagnostic"), Diagnostic.ToJson());
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
			if (!Attempt.Diagnostic.IsEmpty()) AttemptObject->SetObjectField(TEXT("diagnostic"), Attempt.Diagnostic.ToJson());
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

	bool RegisterBuiltInProviders(FProviderRegistry& Registry, UE::UAI::Contract::FDiagnostic& OutError)
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
		ON_SCOPE_EXIT
		{
			if (!OutResult.Diagnostic.IsEmpty())
			{
				if (OutResult.Diagnostic.PropertyPath.IsEmpty()) OutResult.Diagnostic.PropertyPath = Request.PropertyPath;
				if (OutResult.Diagnostic.Stage.IsEmpty()) OutResult.Diagnostic.Stage = TEXT("native_default");
			}
		};
		OutResult.Operation = Request.Operation;
		OutResult.TargetIdentity = Request.TargetIdentity;
		if (!ValidateDefaultTarget(Request, OutResult)) return false;

		if (Request.Operation == EOperation::Update || Request.Operation == EOperation::Patch)
		{
			OutResult.Status = TEXT("preserve_existing");
			OutResult.Diagnostic.Reset();
			OutResult.bPreserveExisting = true;
			return true;
		}
		if (Request.Operation == EOperation::GeneratedAfterApply)
		{
			OutResult.Status = TEXT("deferred");
			OutResult.Diagnostic.Reset();
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
				OutResult.Diagnostic = NativeFailure(TEXT("native_default_provider_unknown"), TEXT("The requested native default provider is not registered."), TEXT("provider_ids"), CanonicalProviderId);
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
				FDiagnostic ProbeError;
				const EProbeOutcome Outcome = Probe(Request, Scratch, ProbeValue, ProbeError);
				FAttempt& Attempt = OutResult.Attempts.AddDefaulted_GetRef();
				Attempt.ProviderId = Descriptor.ProviderId;
				Attempt.SourceKind = Descriptor.SourceKind;
				Attempt.Outcome = ProbeOutcomeToString(Outcome);
				Attempt.Diagnostic = ProbeError;

				if (Outcome == EProbeOutcome::Failed)
				{
					OutResult.Status = TEXT("failed");
					OutResult.Diagnostic = ProbeError.IsEmpty()
						? NativeFailure(TEXT("native_default_probe_failed"), TEXT("The native default provider reported failure without a diagnostic."), Request.PropertyPath, Descriptor.ProviderId)
						: FDiagnostic::WrapFailure(TEXT("native_default_probe_failed"), ProbeError, TEXT("native_default"), TEXT("The native default provider failed while inspecting the target."));
					OutResult.Diagnostic.CandidateName = Descriptor.ProviderId;
					OutResult.Diagnostic.SourceKind = TEXT("native_result");
					return false;
				}
				if (Outcome != EProbeOutcome::Resolved) continue;
				if (Descriptor.bRequiresScratch && Scratch.NumTrackedObjects() == 0)
				{
					OutResult.Status = TEXT("failed");
					auto ScratchError = NativeFailure(TEXT("native_default_provider_scratch_required"), TEXT("The native default provider did not create its required scratch object."), TEXT("scratch_objects"), Descriptor.ProviderId);
					ScratchError.ExpectedValue = MakeShared<FJsonValueString>(TEXT("at least 1"));
					ScratchError.ActualValue = MakeShared<FJsonValueNumber>(Scratch.NumTrackedObjects());
					OutResult.Diagnostic = FDiagnostic::WrapFailure(TEXT("native_default_probe_failed"), ScratchError, TEXT("native_default"), TEXT("The native default provider violated its scratch requirement."));
					return false;
				}
				if (!ProbeValue.Value.IsValid())
				{
					OutResult.Status = TEXT("failed");
					OutResult.Diagnostic = NativeFailure(TEXT("native_default_probe_value_missing"), TEXT("The native default provider reported a resolved result without a value."), Request.PropertyPath, Descriptor.ProviderId);
					OutResult.Diagnostic.ExpectedValuePresent = true;
					OutResult.Diagnostic.ActualValuePresent = false;
					return false;
				}
				if (Request.Property && !ProbeValue.PropertyValue.IsValid())
				{
					FDiagnostic DecodeError;
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
						DecodeError.SourceKind = TEXT("native_result");
						OutResult.Diagnostic = FDiagnostic::WrapFailure(TEXT("native_default_provider_value_invalid"), DecodeError, TEXT("native_default"), TEXT("The native default provider returned a value that cannot be decoded for the property."));
						OutResult.Diagnostic.CandidateName = Descriptor.ProviderId;
						return false;
					}
				}
				if (Request.Property && ProbeValue.PropertyValue.IsValid())
				{
					UeAgentPropertyValueCodec::FOptions CanonicalOptions;
					ProbeValue.Value = UeAgentPropertyValueCodec::Encode(
						Request.Property,
						ProbeValue.PropertyValue->GetValuePtr(),
						Request.ExistingOwner,
						CanonicalOptions);
					FDiagnostic CanonicalError;
					if (!UeAgentPropertyValueCodec::IsReliableCanonicalValue(
						Request.Property,
						ProbeValue.Value,
						&CanonicalError))
					{
						OutResult.Status = TEXT("failed");
						CanonicalError.SourceKind = TEXT("native_result");
						OutResult.Diagnostic = FDiagnostic::WrapFailure(TEXT("native_default_provider_value_invalid"), CanonicalError, TEXT("native_default"), TEXT("The native default provider value has no reliable canonical representation."));
						OutResult.Diagnostic.CandidateName = Descriptor.ProviderId;
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
					&& UE::UAI::StateSemantic::CanonicalJson::AreJsonValuesEquivalent(UniqueCandidates.Last().Value.Value, Candidate.Value.Value))
				{
					continue;
				}
				UniqueCandidates.Add(MoveTemp(Candidate));
			}

			if (UniqueCandidates.Num() > 1)
			{
				OutResult.Status = TEXT("ambiguous");
				OutResult.Diagnostic = NativeFailure(TEXT("native_default_ambiguous"), TEXT("Multiple distinct native default candidates remain at the same source priority."), Request.PropertyPath, Request.TargetIdentity.TargetKind);
				OutResult.Diagnostic.ExpectedValue = MakeShared<FJsonValueNumber>(1);
				OutResult.Diagnostic.ActualValue = MakeShared<FJsonValueNumber>(UniqueCandidates.Num());
				TArray<TSharedPtr<FJsonValue>> PublicCandidates;
				for (const FResolvedCandidate& Candidate : UniqueCandidates)
				{
					FDiagnosticCandidate& Diagnostic = OutResult.Candidates.AddDefaulted_GetRef();
					Diagnostic.ProviderId = Candidate.Descriptor.ProviderId;
					Diagnostic.SourceKind = Candidate.Descriptor.SourceKind;
					Diagnostic.SourceIdentity = Candidate.Value.SourceIdentity;
					Diagnostic.Provenance = Candidate.Value.Provenance;
					auto PublicCandidate = MakeShared<FJsonObject>();
					PublicCandidate->SetStringField(TEXT("provider_id"), Candidate.Descriptor.ProviderId);
					PublicCandidate->SetStringField(TEXT("source_kind"), SourceKindToString(Candidate.Descriptor.SourceKind));
					PublicCandidates.Add(MakeShared<FJsonValueObject>(PublicCandidate));
				}
				OutResult.Diagnostic.CandidateChoices = MakeShared<FJsonValueArray>(MoveTemp(PublicCandidates));
				return false;
			}

			SetResolvedResult(UniqueCandidates[0], OutResult);
			return true;
		}

		if (Request.bRequired)
		{
			OutResult.Status = TEXT("failed");
			OutResult.Diagnostic = NativeFailure(TEXT("native_default_required_missing"), TEXT("No eligible provider supplied the required native default value."), Request.PropertyPath, Request.TargetIdentity.TargetKind);
			OutResult.Diagnostic.ExpectedValuePresent = true;
			OutResult.Diagnostic.ActualValuePresent = false;
			TArray<TSharedPtr<FJsonValue>> Attempts;
			for (const auto& Attempt : OutResult.Attempts)
			{
				auto Value = MakeShared<FJsonObject>();
				Value->SetStringField(TEXT("provider_id"), Attempt.ProviderId);
				Value->SetStringField(TEXT("source_kind"), SourceKindToString(Attempt.SourceKind));
				Value->SetStringField(TEXT("outcome"), Attempt.Outcome);
				Attempts.Add(MakeShared<FJsonValueObject>(Value));
				if (!Attempt.Diagnostic.IsEmpty()) OutResult.Diagnostic.Append(Attempt.Diagnostic);
			}
			OutResult.Diagnostic.CandidateChoices = MakeShared<FJsonValueArray>(MoveTemp(Attempts));
			return false;
		}
		OutResult.Status = TEXT("unavailable");
		OutResult.Diagnostic.Reset();
		return true;
	}
}
