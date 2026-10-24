// Copyright Epic Games, Inc. All Rights Reserved.

#include "Utilities/UeAgentUObjectProperties.h"

#include "Core/Contract/UeAgentDiagnostic.h"
#include "Utilities/AuthoringCore/UeAgentAuthoringTypeDescriptors.h"
#include "Utilities/AuthoringCore/UeAgentAuthoringDiagnostics.h"

#include "Utilities/AuthoringCore/UeAgentAuthoringIdentity.h"
#include "Utilities/AuthoringCore/UeAgentAuthoringNativeDefaults.h"
#include "Utilities/AuthoringCore/UeAgentAuthoringReflectionDescriptors.h"
#include "Utilities/AuthoringCore/UeAgentAuthoringSemanticCompare.h"
#include "Utilities/AuthoringCore/UeAgentAuthoringTargetLocator.h"
#include "Utilities/AuthoringOperationApply/UeAgentAuthoringDirectApply.h"
#include "Utilities/UeAgentPropertyValueCodec.h"

#include "Components/ActorComponent.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Engine/World.h"
#include "GameFramework/Actor.h"
#include "UObject/Class.h"
#include "UObject/Package.h"
#include "UObject/SoftObjectPath.h"
#include "UObject/UnrealType.h"

namespace UeAgentUObjectProperties
{
	FExportOptions::FExportOptions()
		: Profile(EExportProfile::Authoring)
		, bIncludeTransient(false)
		, bIncludeDeprecated(false)
		, bIncludeEditorOnly(false)
		, bIncludeReadOnly(false)
		, bIncludeNonEditable(false)
	{
	}

	FApplyOptions::FApplyOptions()
		: bAllowReadOnly(false)
		, bAllowTemplateOwners(false)
		, bAllowDomainAuthorizedOwners(false)
		, bEmitEditorNotifications(true)
		, bMarkPackageDirty(true)
	{
	}

	namespace
	{
		static FString GetObjectPathSafe(const UObject* Object)
		{
			return Object ? Object->GetPathName() : FString();
		}

		static FString GetPackagePathSafe(const UObject* Object)
		{
			return Object && Object->GetPackage() ? Object->GetPackage()->GetName() : FString();
		}

		static const TCHAR* GetProfileName(EExportProfile Profile)
		{
			switch (Profile)
			{
			case EExportProfile::Index:
				return TEXT("index");
			case EExportProfile::Authoring:
				return TEXT("authoring");
			case EExportProfile::Readback:
				return TEXT("readback");
			case EExportProfile::Debug:
				return TEXT("debug");
			default:
				return TEXT("authoring");
			}
		}

		static FExportOptions ResolveOptions(FExportOptions Options)
		{
			if (Options.Profile == EExportProfile::Readback || Options.Profile == EExportProfile::Debug)
			{
				Options.bIncludeReadOnly = true;
				Options.bIncludeNonEditable = true;
				Options.bIncludeEditorOnly = true;
			}
			return Options;
		}

		static bool ShouldIncludeExtendedMetadata(const FExportOptions& Options)
		{
			return Options.Profile == EExportProfile::Readback || Options.Profile == EExportProfile::Debug;
		}

		static bool ShouldIncludeAuthoringMetadata(const FExportOptions& Options)
		{
			return Options.Profile == EExportProfile::Authoring
				|| Options.Profile == EExportProfile::Readback
				|| Options.Profile == EExportProfile::Debug;
		}

		static bool ShouldIncludeTooltip(const FExportOptions& Options)
		{
			return Options.Profile != EExportProfile::Index;
		}

		static bool ShouldIncludeValues(const FExportOptions& Options)
		{
			return Options.Profile != EExportProfile::Index;
		}

		static bool ShouldIncludeDefaultValues(const FExportOptions& Options)
		{
			return Options.Profile == EExportProfile::Readback || Options.Profile == EExportProfile::Debug;
		}

		static bool ShouldAlwaysIncludeValueText(const FExportOptions& Options)
		{
			return Options.Profile == EExportProfile::Readback || Options.Profile == EExportProfile::Debug;
		}

		static bool ShouldOmitUnchangedProperties(const FExportOptions& Options)
		{
			return false;
		}

		static bool ShouldIncludeSkippedDetails(const FExportOptions& Options)
		{
			return Options.Profile == EExportProfile::Debug;
		}

		static bool TryResolveAndCompareWithDefault(
			UObject* Object,
			FProperty* Property,
			bool& bOutIdentical,
			UeAgentAuthoringNativeDefaults::FResult& OutDefault)
		{
			bOutIdentical = true;
			if (!Object || !Property || !Object->GetClass() || Object->HasAnyFlags(RF_ClassDefaultObject))
			{
				return false;
			}
			UeAgentAuthoringNativeDefaults::FRequest Request;
			UeAgentAuthoringTargetLocator::FResolveResult TargetResult;
			if (!UeAgentAuthoringNativeDefaults::BuildPropertyRequest(
				Object,
				Property->GetName(),
				UeAgentAuthoringNativeDefaults::EOperation::InspectDefault,
				false,
				Request,
				TargetResult))
			{
				return false;
			}
			const void* ValuePtr = Property->ContainerPtrToValuePtr<void>(Object);
			if (!UeAgentAuthoringNativeDefaults::Resolve(Request, OutDefault)
				|| !OutDefault.IsResolved()
				|| !OutDefault.PropertyValue.IsValid()
				|| !ValuePtr)
			{
				return false;
			}
			UE::UAI::Contract::FDiagnostic CompareError;
			return UeAgentAuthoringSemanticCompare::ArePropertyValuesEquivalent(
				Property,
				ValuePtr,
				OutDefault.PropertyValue->GetValuePtr(),
				Object,
				bOutIdentical,
				CompareError, true);
		}

		static TSharedPtr<FJsonValue> MakeStringValue(const FString& Value)
		{
			return MakeShared<FJsonValueString>(Value);
		}


		static void ExportTextForProperty(FProperty* Property, const void* ValuePtr, UObject* Owner, FString& OutText)
		{
			OutText.Reset();
			if (Property && ValuePtr)
			{
				Property->ExportTextItem_Direct(OutText, ValuePtr, nullptr, Owner, PPF_None);
			}
		}

		

		


	}

	FString GetClassPath(const UClass* Class)
	{
		if (!Class)
		{
			return FString();
		}
		return Class->GetPathName();
	}

	TSharedPtr<FJsonObject> BuildObjectIdentity(const UObject* Object)
	{
		TSharedPtr<FJsonObject> Identity = MakeShared<FJsonObject>();
		if (Object)
		{
			TMap<FString, FString> Selectors;
			Selectors.Add(TEXT("name"), Object->GetName());
			Selectors.Add(TEXT("object_path"), GetObjectPathSafe(Object));
			Selectors.Add(TEXT("package_path"), GetPackagePathSafe(Object));
			Selectors.Add(TEXT("class_path"), GetClassPath(Object->GetClass()));
			Selectors.Add(TEXT("outer_path"), Object->GetOuter() ? Object->GetOuter()->GetPathName() : FString());
			UeAgentAuthoringIdentity::FCanonicalIdentity CanonicalIdentity;
			UE::UAI::Contract::FDiagnostic IdentityError;
			if (ensureAlwaysMsgf(
				UeAgentAuthoringIdentity::BuildCanonicalIdentity(
					TEXT("uobject_identity"), TEXT("uobject"), Selectors, CanonicalIdentity, IdentityError),
				TEXT("Failed to build UObject canonical identity: %s"), *IdentityError.ToPublicDetail()))
			{
				UeAgentAuthoringIdentity::WriteSelectorsFlat(CanonicalIdentity, Identity.ToSharedRef());
			}
		}
		for (const TCHAR* Field : { TEXT("name"), TEXT("object_path"), TEXT("package_path"), TEXT("class_path"), TEXT("outer_path") })
		{
			if (!Identity->HasField(Field)) Identity->SetStringField(Field, FString());
		}
		Identity->SetBoolField(TEXT("is_cdo"), Object ? Object->HasAnyFlags(RF_ClassDefaultObject) : false);
		return Identity;
	}

	bool ShouldSkipProperty(const FProperty* Property, const FExportOptions& Options, FString& OutReason, const UObject* Owner)
	{
		OutReason.Reset();
		if (!Property)
		{
			OutReason = TEXT("invalid_property");
			return true;
		}
		if (!Options.bIncludeTransient && Property->HasAnyPropertyFlags(CPF_Transient | CPF_DuplicateTransient | CPF_NonPIEDuplicateTransient | CPF_TextExportTransient))
		{
			OutReason = TEXT("transient");
			return true;
		}
		if (!Options.bIncludeDeprecated && (Property->HasAnyPropertyFlags(CPF_Deprecated) || Property->HasMetaData(TEXT("DeprecatedProperty"))))
		{
			OutReason = TEXT("deprecated");
			return true;
		}
		if (!Options.bIncludeEditorOnly && Property->IsEditorOnlyProperty())
		{
			OutReason = TEXT("editor_only");
			return true;
		}
		const bool bEditable = (Options.bIncludeNonEditable && Options.bIncludeReadOnly)
			|| UeAgentAuthoringReflection::IsEditableProperty(Property, const_cast<UObject*>(Owner));
		if (!Options.bIncludeNonEditable && !bEditable)
		{
			OutReason = TEXT("non_editable");
			return true;
		}
		if (!Options.bIncludeReadOnly && !bEditable)
		{
			OutReason = TEXT("read_only");
			return true;
		}
		return false;
	}

	bool ShouldIncludeStructField(
		const UScriptStruct* Struct,
		const FProperty* Field,
		const FExportOptions& Options,
		const UObject* Owner)
	{
		if (!Struct || !Field) return false;
		if (!Options.bIncludeTransient && Field->HasAnyPropertyFlags(
			CPF_Transient | CPF_DuplicateTransient | CPF_NonPIEDuplicateTransient | CPF_TextExportTransient))
		{
			return false;
		}
		if (!Options.bIncludeDeprecated
			&& (Field->HasAnyPropertyFlags(CPF_Deprecated) || Field->HasMetaData(TEXT("DeprecatedProperty"))))
		{
			return false;
		}
		if (!Options.bIncludeEditorOnly && Field->IsEditorOnlyProperty()) return false;

		bool bIncluded = Options.bIncludeReadOnly && Options.bIncludeNonEditable;
		if (Options.StructFieldOverride)
		{
			const TOptional<bool> Override = Options.StructFieldOverride(Struct, Field);
			if (Override.IsSet()) bIncluded = Override.GetValue();
			else if (!bIncluded) bIncluded = UeAgentAuthoringReflection::IsEditableByFlags(Field, Owner, false, false, Struct);
		}
		else if (!bIncluded)
		{
			bIncluded = UeAgentAuthoringReflection::IsEditableByFlags(Field, Owner, false, false, Struct);
		}
		if (!bIncluded) return false;
		return !Options.StructFieldFilter || Options.StructFieldFilter(Struct, Field);
	}

	bool IsUnsignedNumericProperty(const FNumericProperty* Property)
	{
		return CastField<FByteProperty>(Property)
			|| CastField<FUInt16Property>(Property)
			|| CastField<FUInt32Property>(Property)
			|| CastField<FUInt64Property>(Property);
	}

	TSharedPtr<FJsonValue> ExportPropertyValue(FProperty* Property, const void* ValuePtr, UObject* Owner, const FExportOptions& Options, int32 Depth, TSharedPtr<FJsonObject>* OutValueDefinition)
	{
		if (!Property || !ValuePtr)
		{
			return MakeShared<FJsonValueNull>();
		}
		UeAgentPropertyValueCodec::FOptions CodecOptions;
		CodecOptions.bCaptureStructFieldDescriptors = OutValueDefinition != nullptr;
		CodecOptions.StructFieldFilter = [Options, Owner](
			const UScriptStruct* Struct,
			const FProperty* Field)
		{
			return ShouldIncludeStructField(Struct, Field, Options, Owner);
		};
		return UeAgentPropertyValueCodec::Encode(Property, ValuePtr, Owner, CodecOptions, Depth, OutValueDefinition);

	}

	TSharedPtr<FJsonObject> BuildPropertyDescriptor(UObject* Object, FProperty* Property, const FExportOptions& Options)
	{
		if (!Object || !Property)
		{
			return nullptr;
		}

		const FExportOptions ResolvedOptions = ResolveOptions(Options);
		FString SkipReason;
		if (ShouldSkipProperty(Property, ResolvedOptions, SkipReason, Object))
		{
			return nullptr;
		}

		TSharedPtr<FJsonObject> Descriptor = MakeShared<FJsonObject>();
		const void* ValuePtr = Property->ContainerPtrToValuePtr<void>(Object);
		UeAgentAuthoringReflection::FDescriptor Reflection;
		UE::UAI::Contract::FDiagnostic ReflectionError;
		if (!UeAgentAuthoringReflection::Build(Property, Object, ValuePtr, Reflection, ReflectionError))
		{
			return nullptr;
		}
		const UClass* ObjectClass = Object->GetClass();
		const UStruct* OwnerStruct = Property->GetOwnerStruct();
		const UClass* OwnerClass = Cast<UClass>(OwnerStruct);
		bool bIdenticalToDefault = true;
		UeAgentAuthoringNativeDefaults::FResult NativeDefault;
		const bool bHasDefaultComparison = TryResolveAndCompareWithDefault(Object, Property, bIdenticalToDefault, NativeDefault);
		if (ShouldOmitUnchangedProperties(ResolvedOptions) && bHasDefaultComparison && bIdenticalToDefault)
		{
			return nullptr;
		}

		UeAgentAuthoringIdentity::FCanonicalIdentity PropertyIdentity;
		UE::UAI::Contract::FDiagnostic PropertyIdentityError;
		if (!UeAgentAuthoringTargetLocator::BuildPropertyIdentity(
			Object, Property->GetName(), Object, PropertyIdentity, PropertyIdentityError))
		{
			return nullptr;
		}
		Descriptor->SetStringField(TEXT("property_path"), PropertyIdentity.GetSelector(TEXT("property_path")));
		if (ShouldIncludeAuthoringMetadata(ResolvedOptions))
		{
			const FString& DisplayName = Reflection.DisplayName;
			if (!DisplayName.IsEmpty() && DisplayName != Property->GetName())
			{
				Descriptor->SetStringField(TEXT("display_name"), DisplayName);
			}
			Descriptor->SetStringField(TEXT("kind"), Reflection.Type.Kind);
			UeAgentPropertyValueCodec::AddTypeMetadata(Property, Descriptor);
			if (TSharedPtr<FJsonObject> Availability = Reflection.MakeAvailabilityJson(); Availability.IsValid())
			{
				Descriptor->SetObjectField(TEXT("availability"), Availability);
			}
			if (TSharedPtr<FJsonObject> ConstraintsRef = Reflection.MakeConstraintsJson(); ConstraintsRef.IsValid())
			{
				Descriptor->SetObjectField(TEXT("constraints"), ConstraintsRef);
			}
			if (const UEnum* PropEnum = UeAgentAuthoringTypeDescriptors::GetPropertyEnum(Property))
			{
				if (const auto Candidates = UeAgentAuthoringTypeDescriptors::BuildEnumCandidates(PropEnum, true, Property))
					for (const auto& Field : Candidates->Values) Descriptor->SetField(Field.Key, Field.Value);
			}
		}
		if (ShouldIncludeExtendedMetadata(ResolvedOptions))
		{
			Descriptor->SetArrayField(TEXT("category_path"), Reflection.MakeCategoryPathJson());
			Descriptor->SetStringField(TEXT("property_name"), Property->GetName());
			Descriptor->SetStringField(TEXT("cpp_type"), Property->GetCPPType());
			Descriptor->SetStringField(TEXT("property_class"), Property->GetClass() ? Property->GetClass()->GetName() : FString());
			Descriptor->SetStringField(TEXT("owner_struct"), OwnerStruct ? OwnerStruct->GetName() : FString());
			Descriptor->SetStringField(TEXT("owner_class_path"), OwnerClass ? GetClassPath(OwnerClass) : FString());
			Descriptor->SetStringField(TEXT("category"), Property->GetMetaData(TEXT("Category")));
			Descriptor->SetNumberField(TEXT("array_dim"), Property->ArrayDim);
			Descriptor->SetBoolField(TEXT("inherited"), ObjectClass && OwnerClass && OwnerClass != ObjectClass);
			Descriptor->SetBoolField(TEXT("is_transient"), Property->HasAnyPropertyFlags(CPF_Transient));
			Descriptor->SetBoolField(TEXT("is_deprecated"), Property->HasAnyPropertyFlags(CPF_Deprecated) || Property->HasMetaData(TEXT("DeprecatedProperty")));
		}

		if (ShouldIncludeTooltip(ResolvedOptions))
		{
			const FString& Tooltip = Reflection.Tooltip;
			if (!Tooltip.IsEmpty())
			{
				Descriptor->SetStringField(TEXT("tooltip"), Tooltip);
			}
		}

		if (ShouldIncludeExtendedMetadata(ResolvedOptions))
		{
			Descriptor->SetBoolField(TEXT("editable"), Reflection.bEditableByProperty);
			Descriptor->SetBoolField(TEXT("read_only"), !Reflection.bEditableByProperty);
			Descriptor->SetBoolField(TEXT("is_editor_only"), Property->IsEditorOnlyProperty());
			Descriptor->SetArrayField(TEXT("property_flags"), Reflection.MakePropertyFlagsJson());
			Descriptor->SetBoolField(TEXT("has_cdo_default"), bHasDefaultComparison);
			Descriptor->SetBoolField(TEXT("differs_from_cdo"), bHasDefaultComparison ? !bIdenticalToDefault : false);
		}

		if (ShouldIncludeValues(ResolvedOptions))
		{
			FString ValueText;
			ExportTextForProperty(Property, ValuePtr, Object, ValueText);
			TSharedPtr<FJsonObject> ValueDefinition;
			TSharedPtr<FJsonValue> Value = ExportPropertyValue(Property, ValuePtr, Object, ResolvedOptions, 0,
				ResolvedOptions.Profile == EExportProfile::Authoring ? &ValueDefinition : nullptr);
			UE::UAI::Contract::FDiagnostic ReliabilityReason;
			const bool bReliable = UeAgentPropertyValueCodec::IsReliableCanonicalValue(Property, Value, &ReliabilityReason);
			if (ResolvedOptions.Profile == EExportProfile::Authoring) {
				TSharedPtr<FJsonValue> ProjectedValue, ProjectedDefinition;
				if (!UeAgentPropertyValueCodec::ProjectAuthoringValue(Reflection.Type.ToJsonObject(), Value,
					ValueDefinition ? TSharedPtr<FJsonValue>(MakeShared<FJsonValueObject>(ValueDefinition)) : nullptr,
					ProjectedValue, ProjectedDefinition)) return nullptr;
				Value = ProjectedValue;
			}
			if (bReliable || ResolvedOptions.Profile != EExportProfile::Authoring)
			{
				Descriptor->SetField(TEXT("value"), Value);
			}
			if (!bReliable && ResolvedOptions.Profile == EExportProfile::Authoring)
			{
				TSharedPtr<FJsonObject> Availability = MakeShared<FJsonObject>();
				Availability->SetBoolField(TEXT("editable"), false);
				Availability->SetStringField(TEXT("reason"), ReliabilityReason.PublicMessage);
            Availability->SetObjectField(TEXT("diagnostic"), ReliabilityReason.ToPublicJson());
				Descriptor->SetObjectField(TEXT("availability"), Availability);
			}
			if (ShouldAlwaysIncludeValueText(ResolvedOptions))
			{
				Descriptor->SetField(TEXT("value_text"), MakeStringValue(ValueText));
			}

			if (ShouldIncludeDefaultValues(ResolvedOptions) && bHasDefaultComparison)
			{
				Descriptor->SetField(TEXT("default_value"), NativeDefault.Value);
				Descriptor->SetField(TEXT("default_value_text"), MakeStringValue(NativeDefault.ValueText));
			}
			else if (ShouldIncludeDefaultValues(ResolvedOptions))
			{
				Descriptor->SetField(TEXT("default_value"), MakeShared<FJsonValueNull>());
				Descriptor->SetField(TEXT("default_value_text"), MakeShared<FJsonValueNull>());
			}
		}

		return Descriptor;
	}

	TSharedPtr<FJsonObject> ExportObjectProperties(UObject* Object, const FExportOptions& Options)
	{
		const FExportOptions ResolvedOptions = ResolveOptions(Options);
		TSharedPtr<FJsonObject> Root = MakeShared<FJsonObject>();
		Root->SetStringField(TEXT("schema"), TEXT("uai.uobject_properties.v2"));
		Root->SetStringField(TEXT("profile"), GetProfileName(ResolvedOptions.Profile));
		Root->SetObjectField(TEXT("object"), BuildObjectIdentity(Object));

		TArray<TSharedPtr<FJsonValue>> Properties;
		TArray<TSharedPtr<FJsonValue>> Skipped;
		bool bTruncated = false;

		if (Object && Object->GetClass())
		{
			for (TFieldIterator<FProperty> It(Object->GetClass(), EFieldIterationFlags::IncludeSuper); It; ++It)
			{
				FProperty* Property = *It;
				FString SkipReason;
				if (ShouldSkipProperty(Property, ResolvedOptions, SkipReason, Object))
				{
					if (ShouldIncludeSkippedDetails(ResolvedOptions))
					{
						TSharedPtr<FJsonObject> SkipObj = MakeShared<FJsonObject>();
						SkipObj->SetStringField(TEXT("property_path"), Property ? Property->GetName() : FString());
						SkipObj->SetStringField(TEXT("reason"), SkipReason);
						Skipped.Add(MakeShared<FJsonValueObject>(SkipObj));
					}
					else
					{
						Skipped.Add(MakeShared<FJsonValueNull>());
					}
					continue;
				}

				if (TSharedPtr<FJsonObject> Descriptor = BuildPropertyDescriptor(Object, Property, ResolvedOptions))
				{
					Properties.Add(MakeShared<FJsonValueObject>(Descriptor));
				}
			}
		}

		Root->SetNumberField(TEXT("property_count"), Properties.Num());
		Root->SetBoolField(TEXT("truncated"), bTruncated);
		Root->SetArrayField(TEXT("properties"), Properties);
		Root->SetNumberField(TEXT("skipped_count"), Skipped.Num());
		if (ShouldIncludeSkippedDetails(ResolvedOptions))
		{
			Root->SetArrayField(TEXT("skipped"), Skipped);
		}
		return Root;
	}

	TSharedPtr<FJsonObject> ExportActorProperties(AActor* Actor, const FExportOptions& Options)
	{
		return ExportObjectProperties(Actor, Options);
	}

	TSharedPtr<FJsonObject> ExportComponentProperties(UActorComponent* Component, const FExportOptions& Options)
	{
		return ExportObjectProperties(Component, Options);
	}

	TArray<TSharedPtr<FJsonValue>> ExportActorComponentProperties(AActor* Actor, const FExportOptions& Options)
	{
		TArray<TSharedPtr<FJsonValue>> OutComponents;
		if (!Actor)
		{
			return OutComponents;
		}

		TArray<UActorComponent*> Components;
		Actor->GetComponents(Components);
		for (UActorComponent* Component : Components)
		{
			if (!Component)
			{
				continue;
			}

			TSharedPtr<FJsonObject> Entry = MakeShared<FJsonObject>();
			Entry->SetStringField(TEXT("component_name"), Component->GetName());
			Entry->SetStringField(TEXT("component_class_path"), GetClassPath(Component->GetClass()));
			Entry->SetStringField(TEXT("component_schema"), TEXT("uai.uobject_properties.v2"));
			Entry->SetObjectField(TEXT("properties"), ExportComponentProperties(Component, Options));
			OutComponents.Add(MakeShared<FJsonValueObject>(Entry));
		}
		return OutComponents;
	}

	namespace
	{
		UE::UAI::Contract::FDiagnostic PropertyInputFailure(const TCHAR* Code, const TCHAR* Reason,
			const FJsonObject* Object, const FString& Field, const TSharedPtr<FJsonValue>& Expected)
		{
			auto Error = UE::UAI::Contract::FDiagnostic::Failure(Code, Reason, TEXT("property_preflight"));
			Error.ActualValue = Object ? Object->TryGetField(Field) : nullptr;
			Error.ActualValuePresent = Error.ActualValue.IsValid();
			Error.ExpectedValue = Expected;
			Error.ExpectedValuePresent = Expected.IsValid();
			if (!Field.IsEmpty())
				Error.SemanticPointer = TEXT("/") + Field.Replace(TEXT("~"), TEXT("~0")).Replace(TEXT("/"), TEXT("~1"));
			if (Object) UeAgentAuthoringDiagnostics::LocateAuthoringSource(Error, *Object, Field);
			return Error;
		}

		bool TryReadPropertyDescriptors(const TSharedPtr<FJsonObject>& Document,
			const TArray<TSharedPtr<FJsonValue>>*& Properties, UE::UAI::Contract::FDiagnostic& Error)
		{
			FString Schema;
			if (!Document || !Document->TryGetStringField(TEXT("schema"), Schema)
				|| Schema != TEXT("uai.uobject_properties.v2"))
			{
				Error = PropertyInputFailure(TEXT("schema_mismatch_expected_uai.uobject_properties.v2"),
					Document ? TEXT("The property document must use schema uai.uobject_properties.v2.")
						: TEXT("The property document is missing."),
					Document.Get(), TEXT("schema"), MakeShared<FJsonValueString>(TEXT("uai.uobject_properties.v2")));
				return false;
			}
			if (!Document->TryGetArrayField(TEXT("properties"), Properties) || !Properties)
			{
				Error = PropertyInputFailure(TEXT("missing_properties_array"),
					TEXT("The property document must contain a properties array."),
					Document.Get(), TEXT("properties"), MakeShared<FJsonValueString>(TEXT("array")));
				return false;
			}
			return true;
		}
	}

	bool ValidatePropertyDescriptorShape(
		const TSharedPtr<FJsonObject>& Descriptor,
		FString& OutPropertyPath,
		TSharedPtr<FJsonValue>& OutValue,
		UE::UAI::Contract::FDiagnostic& OutError)
	{
		OutPropertyPath.Reset();
		OutValue.Reset();
		OutError.Reset();
		if (!Descriptor.IsValid())
		{
			OutError = PropertyInputFailure(TEXT("invalid_property_descriptor"),
				TEXT("Each property descriptor must be a JSON object."), nullptr, FString(),
				MakeShared<FJsonValueString>(TEXT("object")));
			return false;
		}
		static const TSet<FString> AllowedDescriptorFields = {
			TEXT("property_path"), TEXT("kind"), TEXT("value"), TEXT("display_name"), TEXT("tooltip"),
			TEXT("availability"), TEXT("constraints"), TEXT("candidates"), TEXT("candidates_header"), TEXT("candidates_ref"),
			TEXT("element_type"), TEXT("key_type"), TEXT("mapped_type"), TEXT("inner_type"), TEXT("fixed_length"),
			TEXT("struct_path"), TEXT("allowed_class"), TEXT("enum_path")
		};
		for (const TPair<FString, TSharedPtr<FJsonValue>>& Field : Descriptor->Values)
		{
			if (!AllowedDescriptorFields.Contains(Field.Key))
			{
				OutError = PropertyInputFailure(TEXT("unexpected_property_descriptor_field"),
					TEXT("The property descriptor contains an unsupported field."), Descriptor.Get(), Field.Key, nullptr);
				return false;
			}
		}
		if (!Descriptor->TryGetStringField(TEXT("property_path"), OutPropertyPath) || OutPropertyPath.IsEmpty())
		{
			OutError = PropertyInputFailure(TEXT("missing_property_path"),
				TEXT("The property descriptor must contain a nonempty string property_path."),
				Descriptor.Get(), TEXT("property_path"), MakeShared<FJsonValueString>(TEXT("nonempty string")));
			return false;
		}
		OutValue = Descriptor->TryGetField(TEXT("value"));
		if (!OutValue.IsValid())
		{
			OutError = PropertyInputFailure(TEXT("missing_value"),
				TEXT("The property descriptor is missing its value field."),
				Descriptor.Get(), TEXT("value"), nullptr);
			OutError.ExpectedValuePresent = true;
			if (OutError.PropertyPath.IsEmpty()) OutError.PropertyPath = OutPropertyPath;
			return false;
		}
		return true;
	}

	namespace
	{
		bool ProjectLegacyApplyDescriptors(
			const TArray<TSharedPtr<FJsonValue>>& Input,
			const FJsonObject& SourceDocument,
			TArray<TSharedPtr<FJsonValue>>& OutProjected,
			FString& OutPropertyPath,
			int32& OutFailedIndex,
			UE::UAI::Contract::FDiagnostic& OutError)
		{
			static const TSet<FString> AssertionFields = {
				TEXT("kind"), TEXT("struct_path"), TEXT("enum_path"),
				TEXT("element_type"), TEXT("key_type"), TEXT("mapped_type"),
				TEXT("inner_type"), TEXT("fixed_length"), TEXT("allowed_class")
			};
			OutProjected.Reset();
			OutPropertyPath.Reset();
			OutFailedIndex = INDEX_NONE;
			OutError.Reset();
			OutProjected.Reserve(Input.Num());
			for (int32 Index = 0; Index < Input.Num(); ++Index)
			{
				const TSharedPtr<FJsonValue>& Value = Input[Index];
				const TSharedPtr<FJsonObject> Descriptor = Value.IsValid() && Value->Type == EJson::Object
					? Value->AsObject()
					: nullptr;
				TSharedPtr<FJsonValue> CanonicalValue;
				if (!ValidatePropertyDescriptorShape(Descriptor, OutPropertyPath, CanonicalValue, OutError))
				{
					OutFailedIndex = Index;
					if (!Descriptor.IsValid())
					{
						OutError.ActualValue = Value;
						OutError.ActualValuePresent = Value.IsValid();
						OutError.SemanticPointer = TEXT("/properties/") + FString::FromInt(Index);
						UeAgentAuthoringDiagnostics::FScopedSourceLocations::Locate(SourceDocument,
							TEXT("properties"), OutError.Source, OutError.SemanticPointer);
					}
					return false;
				}

				TSharedRef<FJsonObject> Projected = MakeShared<FJsonObject>();
				Projected->SetStringField(TEXT("property_path"), OutPropertyPath);
				Projected->SetField(TEXT("value"), CanonicalValue);
				for (const FString& Field : AssertionFields)
				{
					if (const TSharedPtr<FJsonValue> Assertion = Descriptor->TryGetField(Field); Assertion.IsValid())
					{
						Projected->SetField(Field, Assertion);
					}
				}
				OutProjected.Add(MakeShared<FJsonValueObject>(Projected));
			}
			return true;
		}

		UeAgentAuthoringObjectApply::FOptions MakeObjectApplyOptions(const FApplyOptions& Options, const UStruct* RootStruct, const void* RootData)
		{
			UeAgentAuthoringObjectApply::FOptions Result;
			Result.bAllowReadOnly = Options.bAllowReadOnly;
			Result.bAllowTemplateOwners = Options.bAllowTemplateOwners;
			Result.bAllowDomainAuthorizedOwners = Options.bAllowDomainAuthorizedOwners;
			Result.bEmitEditorNotifications = Options.bEmitEditorNotifications;
			Result.bMarkPackageDirty = Options.bMarkPackageDirty;
			if (Options.SupplementDiagnosticSource)
			{
				Result.SupplementDiagnosticSource = [Supplement = Options.SupplementDiagnosticSource, RootStruct, RootData](
					UE::UAI::Contract::FDiagnostic& Error, const FJsonObject&, const UStruct* CurrentStruct, const void* CurrentData, int32 Index)
				{
					if (CurrentStruct == RootStruct && CurrentData == RootData) Supplement(Error, Index);
				};
			}
			return Result;
		}

	}

	bool ApplyStructProperties(
        UStruct* StructType, void* StructData, UObject* Owner, const TSharedPtr<FJsonObject>& PropertiesJson,
        const FApplyOptions& Options, FApplySummaryResult& OutResult)
    {
        OutResult = FApplySummaryResult();
        if (!StructType || !StructData || !Owner)
        {
            OutResult.Diagnostic = UE::UAI::Contract::FDiagnostic::Failure(TEXT("invalid_struct_owner"),
                !StructType ? TEXT("The property target has no native struct type.")
                    : !StructData ? TEXT("The property target has no native struct storage.")
                    : TEXT("The property target has no native owner."), TEXT("property_preflight"));
            OutResult.Diagnostic.SourceKind = TEXT("native_result");
            auto Actual = MakeShared<FJsonObject>();
            Actual->SetBoolField(TEXT("struct_type_present"), StructType != nullptr);
            Actual->SetBoolField(TEXT("struct_storage_present"), StructData != nullptr);
            Actual->SetBoolField(TEXT("owner_present"), Owner != nullptr);
            OutResult.Diagnostic.ActualValue = MakeShared<FJsonValueObject>(Actual);
            OutResult.Diagnostic.ExpectedValue = MakeShared<FJsonValueString>(TEXT("native struct type, storage and owner"));
            return false;
        }
        const TArray<TSharedPtr<FJsonValue>>* Properties = nullptr;
        if (!TryReadPropertyDescriptors(PropertiesJson, Properties, OutResult.Diagnostic)) return false;
        TArray<TSharedPtr<FJsonValue>> Projected;
        FString FailedPath;
        int32 FailedIndex = INDEX_NONE;
        if (!ProjectLegacyApplyDescriptors(*Properties, *PropertiesJson, Projected, FailedPath, FailedIndex, OutResult.Diagnostic))
        {
            if (OutResult.Diagnostic.PropertyPath.IsEmpty()) OutResult.Diagnostic.PropertyPath = FailedPath;
            if (Options.SupplementDiagnosticSource) Options.SupplementDiagnosticSource(OutResult.Diagnostic, FailedIndex);
            return false;
        }
        if (!Projected.IsEmpty())
        {
            UeAgentAuthoringDirectApply::FResult Applied;
            if (!UeAgentAuthoringDirectApply::ApplyStructPatch(
                StructType, StructData, Owner, Projected, MakeObjectApplyOptions(Options, StructType, StructData), Applied))
            {
                OutResult.Diagnostic = MoveTemp(Applied.Diagnostic);
                OutResult.Diagnostic.RollbackAttempted = Applied.bRollbackAttempted;
                OutResult.Diagnostic.RollbackSucceeded = Applied.bRollbackSucceeded;
                return false;
            }
            for (const auto& Value : Projected)
                OutResult.AppliedPropertyPaths.Add(Value->AsObject()->GetStringField(TEXT("property_path")));
        }
        OutResult.bApplied = true;
        OutResult.PropertiesApplied = Projected.Num();
        return true;
    }

	bool ApplyObjectProperties(
        UObject* Object, const TSharedPtr<FJsonObject>& PropertiesJson,
        const FApplyOptions& Options, FApplySummaryResult& OutResult)
    {
        OutResult = FApplySummaryResult();
        if (!Object || !Object->GetClass())
        {
            OutResult.Diagnostic = UE::UAI::Contract::FDiagnostic::Failure(TEXT("invalid_object"),
                !Object ? TEXT("The property target object is missing.")
                    : TEXT("The property target object has no native class."), TEXT("property_preflight"));
            OutResult.Diagnostic.SourceKind = TEXT("native_result");
            OutResult.Diagnostic.PropertyPath = !Object ? TEXT("object") : TEXT("class");
            OutResult.Diagnostic.ActualValuePresent = false;
            OutResult.Diagnostic.ExpectedValuePresent = true;
            return false;
        }
        const TArray<TSharedPtr<FJsonValue>>* Properties = nullptr;
        if (!TryReadPropertyDescriptors(PropertiesJson, Properties, OutResult.Diagnostic)) return false;
        TArray<TSharedPtr<FJsonValue>> Projected;
        FString FailedPath;
        int32 FailedIndex = INDEX_NONE;
        if (!ProjectLegacyApplyDescriptors(*Properties, *PropertiesJson, Projected, FailedPath, FailedIndex, OutResult.Diagnostic))
        {
            if (OutResult.Diagnostic.PropertyPath.IsEmpty()) OutResult.Diagnostic.PropertyPath = FailedPath;
            if (Options.SupplementDiagnosticSource) Options.SupplementDiagnosticSource(OutResult.Diagnostic, FailedIndex);
            return false;
        }
        if (!Projected.IsEmpty())
        {
            UeAgentAuthoringDirectApply::FResult Applied;
            if (!UeAgentAuthoringDirectApply::ApplyObjectPatch(
                Object, Projected, MakeObjectApplyOptions(Options, Object->GetClass(), Object), Applied))
            {
                OutResult.Diagnostic = MoveTemp(Applied.Diagnostic);
                OutResult.Diagnostic.RollbackAttempted = Applied.bRollbackAttempted;
                OutResult.Diagnostic.RollbackSucceeded = Applied.bRollbackSucceeded;
                return false;
            }
            for (const auto& Value : Projected)
                OutResult.AppliedPropertyPaths.Add(Value->AsObject()->GetStringField(TEXT("property_path")));
        }
        OutResult.bApplied = true;
        OutResult.PropertiesApplied = Projected.Num();
        return true;
    }

	

}
