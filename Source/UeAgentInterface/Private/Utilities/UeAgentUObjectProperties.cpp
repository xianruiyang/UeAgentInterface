// Copyright Epic Games, Inc. All Rights Reserved.

#include "Utilities/UeAgentUObjectProperties.h"

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
		, MaxProperties(1000)
		, MaxDepth(-1)
		, MaxContainerItems(64)
		, MaxStringChars(2048)
	{
	}

	FApplyOptions::FApplyOptions()
		: bAllowReadOnly(false)
		, bAllowTemplateOwners(false)
		, bAllowDomainAuthorizedOwners(false)
		, bEmitEditorNotifications(true)
		, bMarkPackageDirty(true)
		, MaxContainerItems(256)
		, MaxStringChars(4096)
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
			FString CompareError;
			return UeAgentAuthoringSemanticCompare::ArePropertyValuesEquivalent(
				Property,
				ValuePtr,
				OutDefault.PropertyValue->GetValuePtr(),
				Object,
				bOutIdentical,
				CompareError);
		}

		static TSharedPtr<FJsonValue> MakeBoundedStringValue(const FString& Value, const FExportOptions& Options)
		{
			if (Options.MaxStringChars <= 0 || Value.Len() <= Options.MaxStringChars)
			{
				return MakeShared<FJsonValueString>(Value);
			}

			TSharedPtr<FJsonObject> Obj = MakeShared<FJsonObject>();
			Obj->SetStringField(TEXT("kind"), TEXT("string"));
			Obj->SetStringField(TEXT("preview"), Value.Left(Options.MaxStringChars));
			Obj->SetNumberField(TEXT("full_length"), Value.Len());
			Obj->SetBoolField(TEXT("truncated"), true);
			return MakeShared<FJsonValueObject>(Obj);
		}

		static TSharedPtr<FJsonValue> MakeDepthExceededValue(const FProperty* Property, int32 Depth)
		{
			TSharedPtr<FJsonObject> Obj = MakeShared<FJsonObject>();
			Obj->SetStringField(TEXT("kind"), TEXT("depth_exceeded"));
			Obj->SetStringField(TEXT("cpp_type"), Property ? Property->GetCPPType() : FString());
			Obj->SetNumberField(TEXT("depth"), Depth);
			return MakeShared<FJsonValueObject>(Obj);
		}

		static void WriteEnumCandidatesToDescriptor(TSharedPtr<FJsonObject>& Descriptor, const UEnum* Enum)
		{
			if (!Enum || !Descriptor.IsValid())
			{
				return;
			}

			// First pass: detect which optional columns exist
			bool bHasDisplayName = false;
			bool bHasTooltip = false;
			for (int32 Index = 0; Index < Enum->NumEnums(); ++Index)
			{
				if (Enum->HasMetaData(TEXT("Hidden"), Index))
				{
					continue;
				}
				const FString Name = Enum->GetNameStringByIndex(Index);
				if (Name.IsEmpty() || Name.Equals(TEXT("MAX"), ESearchCase::IgnoreCase) || Name.EndsWith(TEXT("_MAX"), ESearchCase::IgnoreCase))
				{
					continue;
				}
				const FString DisplayName = Enum->GetDisplayNameTextByIndex(Index).ToString();
				if (!DisplayName.IsEmpty() && DisplayName != Name)
				{
					bHasDisplayName = true;
				}
				if (!Enum->GetMetaData(TEXT("ToolTip"), Index).IsEmpty())
				{
					bHasTooltip = true;
				}
				if (bHasDisplayName && bHasTooltip)
				{
					break;
				}
			}

			// Second pass: build rows
			TArray<TSharedPtr<FJsonValue>> Rows;
			for (int32 Index = 0; Index < Enum->NumEnums(); ++Index)
			{
				if (Enum->HasMetaData(TEXT("Hidden"), Index))
				{
					continue;
				}
				const FString Name = Enum->GetNameStringByIndex(Index);
				if (Name.IsEmpty() || Name.Equals(TEXT("MAX"), ESearchCase::IgnoreCase) || Name.EndsWith(TEXT("_MAX"), ESearchCase::IgnoreCase))
				{
					continue;
				}

				TArray<TSharedPtr<FJsonValue>> Row;
				Row.Add(MakeShared<FJsonValueString>(Name));
				if (bHasDisplayName)
				{
					const FString DisplayName = Enum->GetDisplayNameTextByIndex(Index).ToString();
					if (!DisplayName.IsEmpty() && DisplayName != Name)
					{
						Row.Add(MakeShared<FJsonValueString>(DisplayName));
					}
					else
					{
						Row.Add(MakeShared<FJsonValueString>(Name));
					}
				}
				if (bHasTooltip)
				{
					const FString Tooltip = Enum->GetMetaData(TEXT("ToolTip"), Index);
					Row.Add(MakeShared<FJsonValueString>(Tooltip.IsEmpty() ? FString() : Tooltip));
				}
				Rows.Add(MakeShared<FJsonValueArray>(Row));
			}

			if (Rows.Num() == 0)
			{
				return;
			}

			TArray<TSharedPtr<FJsonValue>> Header;
			Header.Add(MakeShared<FJsonValueString>(TEXT("name")));
			if (bHasDisplayName)
			{
				Header.Add(MakeShared<FJsonValueString>(TEXT("display_name")));
			}
			if (bHasTooltip)
			{
				Header.Add(MakeShared<FJsonValueString>(TEXT("tooltip")));
			}
			Descriptor->SetArrayField(TEXT("candidates_header"), Header);
			Descriptor->SetArrayField(TEXT("candidates"), Rows);
		}

		static void ExportTextForProperty(FProperty* Property, const void* ValuePtr, UObject* Owner, FString& OutText)
		{
			OutText.Reset();
			if (Property && ValuePtr)
			{
				Property->ExportTextItem_Direct(OutText, ValuePtr, nullptr, Owner, PPF_None);
			}
		}

		static TSharedPtr<FJsonObject> MakeApplyResult(const FString& PropertyPath, const FString& Status, const FString& Reason = FString())
		{
			TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
			Result->SetStringField(TEXT("property_path"), PropertyPath);
			Result->SetStringField(TEXT("status"), Status);
			if (!Reason.IsEmpty())
			{
				Result->SetStringField(TEXT("reason"), Reason);
			}
			return Result;
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
			FString IdentityError;
			if (ensureAlwaysMsgf(
				UeAgentAuthoringIdentity::BuildCanonicalIdentity(
					TEXT("uobject_identity"), TEXT("uobject"), Selectors, CanonicalIdentity, IdentityError),
				TEXT("Failed to build UObject canonical identity: %s"), *IdentityError))
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
		const bool bEditable = UeAgentAuthoringReflection::IsEditableProperty(Property, const_cast<UObject*>(Owner));
		const bool bConditionallyEditable = Property->HasMetaData(TEXT("EditCondition"))
			&& Property->HasAnyPropertyFlags(CPF_Edit)
			&& !Property->HasAnyPropertyFlags(CPF_EditConst | CPF_Deprecated | CPF_Transient);
		if (!Options.bIncludeNonEditable && !bEditable && !bConditionallyEditable)
		{
			OutReason = TEXT("non_editable");
			return true;
		}
		if (!Options.bIncludeReadOnly && !bEditable && !bConditionallyEditable)
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
			else if (!bIncluded) bIncluded = UeAgentAuthoringReflection::IsEditableByFlags(Field, Owner);
		}
		else if (!bIncluded)
		{
			bIncluded = UeAgentAuthoringReflection::IsEditableByFlags(Field, Owner);
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

	static const UEnum* GetEnumFromProperty(FProperty* Property)
	{
		if (const FEnumProperty* EnumProperty = CastField<FEnumProperty>(Property))
		{
			return EnumProperty->GetEnum();
		}
		if (const FByteProperty* ByteProperty = CastField<FByteProperty>(Property))
		{
			return ByteProperty->Enum;
		}
		return nullptr;
	}

	TSharedPtr<FJsonValue> ExportPropertyValue(FProperty* Property, const void* ValuePtr, UObject* Owner, const FExportOptions& Options, int32 Depth)
	{
		if (!Property || !ValuePtr)
		{
			return MakeShared<FJsonValueNull>();
		}
		UeAgentPropertyValueCodec::FOptions CodecOptions;
		CodecOptions.MaxDepth = Options.MaxDepth;
		CodecOptions.MaxContainerItems = Options.MaxContainerItems;
		CodecOptions.MaxStringChars = Options.MaxStringChars;
		CodecOptions.StructFieldFilter = [Options, Owner](
			const UScriptStruct* Struct,
			const FProperty* Field)
		{
			return ShouldIncludeStructField(Struct, Field, Options, Owner);
		};
		return UeAgentPropertyValueCodec::Encode(Property, ValuePtr, Owner, CodecOptions, Depth);

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
		FString ReflectionError;
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
		FString PropertyIdentityError;
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
			Descriptor->SetStringField(TEXT("value_kind"), Reflection.Type.Kind);
			UeAgentPropertyValueCodec::AddTypeMetadata(Property, Descriptor);
			if (TSharedPtr<FJsonObject> Availability = Reflection.MakeAvailabilityJson(); Availability.IsValid())
			{
				Descriptor->SetObjectField(TEXT("availability"), Availability);
			}
			if (TSharedPtr<FJsonObject> ConstraintsRef = Reflection.MakeConstraintsJson(); ConstraintsRef.IsValid())
			{
				Descriptor->SetObjectField(TEXT("constraints"), ConstraintsRef);
			}
			if (const UEnum* PropEnum = GetEnumFromProperty(Property))
			{
				WriteEnumCandidatesToDescriptor(Descriptor, PropEnum);
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
			const TSharedPtr<FJsonValue> Value = ExportPropertyValue(Property, ValuePtr, Object, ResolvedOptions);
			FString ReliabilityReason;
			const bool bReliable = UeAgentPropertyValueCodec::IsReliableCanonicalValue(Property, Value, &ReliabilityReason);
			if (bReliable || ResolvedOptions.Profile != EExportProfile::Authoring)
			{
				Descriptor->SetField(TEXT("value"), Value);
			}
			if (!bReliable && ResolvedOptions.Profile == EExportProfile::Authoring)
			{
				TSharedPtr<FJsonObject> Availability = MakeShared<FJsonObject>();
				Availability->SetBoolField(TEXT("editable"), false);
				Availability->SetStringField(TEXT("reason"), ReliabilityReason);
				Descriptor->SetObjectField(TEXT("availability"), Availability);
			}
			if (ShouldAlwaysIncludeValueText(ResolvedOptions))
			{
				Descriptor->SetField(TEXT("value_text"), MakeBoundedStringValue(ValueText, ResolvedOptions));
			}

			if (ShouldIncludeDefaultValues(ResolvedOptions) && bHasDefaultComparison)
			{
				Descriptor->SetField(TEXT("default_value"), NativeDefault.Value);
				Descriptor->SetField(TEXT("default_value_text"), MakeBoundedStringValue(NativeDefault.ValueText, ResolvedOptions));
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

				if (ResolvedOptions.MaxProperties > 0 && Properties.Num() >= ResolvedOptions.MaxProperties)
				{
					bTruncated = true;
					break;
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

	bool ValidatePropertyDescriptorShape(
		const TSharedPtr<FJsonObject>& Descriptor,
		FString& OutPropertyPath,
		TSharedPtr<FJsonValue>& OutValue,
		FString& OutError)
	{
		OutPropertyPath.Reset();
		OutValue.Reset();
		OutError.Reset();
		if (!Descriptor.IsValid())
		{
			OutError = TEXT("invalid_property_descriptor");
			return false;
		}
		static const TSet<FString> AllowedDescriptorFields = {
			TEXT("property_path"), TEXT("value_kind"), TEXT("value"), TEXT("display_name"), TEXT("tooltip"),
			TEXT("availability"), TEXT("constraints"), TEXT("candidates"), TEXT("candidates_header"), TEXT("candidates_ref"),
			TEXT("element_type"), TEXT("key_type"), TEXT("mapped_type"), TEXT("inner_type"), TEXT("fixed_length"),
			TEXT("struct_path"), TEXT("allowed_class"), TEXT("enum_path")
		};
		for (const TPair<FString, TSharedPtr<FJsonValue>>& Field : Descriptor->Values)
		{
			if (!AllowedDescriptorFields.Contains(Field.Key))
			{
				OutError = TEXT("unexpected_property_descriptor_field:") + Field.Key;
				return false;
			}
		}
		if (!Descriptor->TryGetStringField(TEXT("property_path"), OutPropertyPath) || OutPropertyPath.IsEmpty())
		{
			OutError = TEXT("missing_property_path");
			return false;
		}
		OutValue = Descriptor->TryGetField(TEXT("value"));
		if (!OutValue.IsValid())
		{
			OutError = TEXT("missing_value");
			return false;
		}
		return true;
	}

	namespace
	{
		bool ProjectLegacyApplyDescriptors(
			const TArray<TSharedPtr<FJsonValue>>& Input,
			TArray<TSharedPtr<FJsonValue>>& OutProjected,
			FString& OutPropertyPath,
			FString& OutError)
		{
			static const TSet<FString> AssertionFields = {
				TEXT("value_kind"), TEXT("struct_path"), TEXT("enum_path"),
				TEXT("element_type"), TEXT("key_type"), TEXT("mapped_type"),
				TEXT("inner_type"), TEXT("fixed_length"), TEXT("allowed_class")
			};
			OutProjected.Reset();
			OutPropertyPath.Reset();
			OutError.Reset();
			OutProjected.Reserve(Input.Num());
			for (const TSharedPtr<FJsonValue>& Value : Input)
			{
				const TSharedPtr<FJsonObject> Descriptor = Value.IsValid() && Value->Type == EJson::Object
					? Value->AsObject()
					: nullptr;
				TSharedPtr<FJsonValue> CanonicalValue;
				if (!ValidatePropertyDescriptorShape(Descriptor, OutPropertyPath, CanonicalValue, OutError)) return false;

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

		UeAgentAuthoringObjectApply::FOptions MakeObjectApplyOptions(const FApplyOptions& Options)
		{
			UeAgentAuthoringObjectApply::FOptions Result;
			Result.bAllowReadOnly = Options.bAllowReadOnly;
			Result.bAllowTemplateOwners = Options.bAllowTemplateOwners;
			Result.bAllowDomainAuthorizedOwners = Options.bAllowDomainAuthorizedOwners;
			Result.bEmitEditorNotifications = Options.bEmitEditorNotifications;
			Result.bMarkPackageDirty = Options.bMarkPackageDirty;
			Result.MaxContainerItems = Options.MaxContainerItems;
			Result.MaxStringChars = Options.MaxStringChars;
			return Result;
		}

	}

	TSharedPtr<FJsonObject> ApplyStructProperties(
		UStruct* StructType,
		void* StructData,
		UObject* Owner,
		const TSharedPtr<FJsonObject>& PropertiesJson,
		const FApplyOptions& Options)
	{
		TSharedPtr<FJsonObject> Summary = MakeShared<FJsonObject>();
		Summary->SetStringField(TEXT("schema"), TEXT("uai.ustruct_properties_apply.v2"));
		Summary->SetObjectField(TEXT("object"), BuildObjectIdentity(Owner));
		TArray<TSharedPtr<FJsonValue>> Results;
		auto Finish = [&Summary, &Results](const TCHAR* Status, const FString& Reason, int32 Seen, int32 Applied, int32 Failed)
		{
			Summary->SetStringField(TEXT("status"), Status);
			if (!Reason.IsEmpty()) Summary->SetStringField(TEXT("reason"), Reason);
			Summary->SetNumberField(TEXT("properties_seen"), Seen);
			Summary->SetNumberField(TEXT("properties_applied"), Applied);
			Summary->SetNumberField(TEXT("properties_failed"), Failed);
			Summary->SetArrayField(TEXT("properties"), Results);
			return Summary;
		};
		if (!StructType || !StructData || !Owner)
		{
			return Finish(TEXT("failed"), TEXT("invalid_struct_owner"), 0, 0, 1);
		}
		FString InputSchema;
		if (!PropertiesJson.IsValid()
			|| !PropertiesJson->TryGetStringField(TEXT("schema"), InputSchema)
			|| InputSchema != TEXT("uai.uobject_properties.v2"))
		{
			return Finish(TEXT("failed"), TEXT("schema_mismatch_expected_uai.uobject_properties.v2"), 0, 0, 1);
		}
		const TArray<TSharedPtr<FJsonValue>>* Properties = nullptr;
		if (!PropertiesJson->TryGetArrayField(TEXT("properties"), Properties) || !Properties)
		{
			return Finish(TEXT("failed"), TEXT("missing_properties_array"), 0, 0, 1);
		}

		TArray<TSharedPtr<FJsonValue>> Projected;
		FString FailedPath;
		FString FailureReason;
		if (!ProjectLegacyApplyDescriptors(*Properties, Projected, FailedPath, FailureReason))
		{
			Results.Add(MakeShared<FJsonValueObject>(MakeApplyResult(FailedPath, TEXT("failed"), FailureReason)));
			return Finish(TEXT("failed"), TEXT("preflight_failed"), Properties->Num(), 0, 1);
		}

		if (Projected.IsEmpty())
		{
			return Finish(TEXT("applied"), FString(), 0, 0, 0);
		}

		UeAgentAuthoringDirectApply::FResult ApplyResult;
		if (!UeAgentAuthoringDirectApply::ApplyStructPatch(
			StructType, StructData, Owner, Projected, MakeObjectApplyOptions(Options), ApplyResult))
		{
			Results.Add(MakeShared<FJsonValueObject>(MakeApplyResult(
				ApplyResult.PropertyPath, TEXT("failed"), ApplyResult.ErrorCode)));
			return Finish(TEXT("failed"), ApplyResult.ErrorCode, Properties->Num(), 0, 1);
		}
		for (const TSharedPtr<FJsonValue>& Value : Projected)
		{
			Results.Add(MakeShared<FJsonValueObject>(MakeApplyResult(
				Value->AsObject()->GetStringField(TEXT("property_path")), TEXT("applied"))));
		}
		return Finish(TEXT("applied"), FString(), Properties->Num(), Projected.Num(), 0);
	}

	TSharedPtr<FJsonObject> ApplyObjectProperties(
		UObject* Object,
		const TSharedPtr<FJsonObject>& PropertiesJson,
		const FApplyOptions& Options)
	{
		TSharedPtr<FJsonObject> Summary = MakeShared<FJsonObject>();
		Summary->SetStringField(TEXT("schema"), TEXT("uai.uobject_properties_apply.v2"));
		Summary->SetObjectField(TEXT("object"), BuildObjectIdentity(Object));
		TArray<TSharedPtr<FJsonValue>> Results;
		auto Finish = [&Summary, &Results](const TCHAR* Status, const FString& Reason, int32 Seen, int32 Applied, int32 Failed)
		{
			Summary->SetStringField(TEXT("status"), Status);
			if (!Reason.IsEmpty()) Summary->SetStringField(TEXT("reason"), Reason);
			Summary->SetNumberField(TEXT("properties_seen"), Seen);
			Summary->SetNumberField(TEXT("properties_applied"), Applied);
			Summary->SetNumberField(TEXT("properties_skipped"),
				FCString::Stricmp(Status, TEXT("applied")) == 0 ? FMath::Max(0, Seen - Applied) : 0);
			Summary->SetNumberField(TEXT("properties_failed"), Failed);
			Summary->SetArrayField(TEXT("properties"), Results);
			return Summary;
		};
		if (!Object || !Object->GetClass())
		{
			return Finish(TEXT("failed"), TEXT("invalid_object"), 0, 0, 1);
		}
		FString InputSchema;
		if (!PropertiesJson.IsValid()
			|| !PropertiesJson->TryGetStringField(TEXT("schema"), InputSchema)
			|| InputSchema != TEXT("uai.uobject_properties.v2"))
		{
			return Finish(TEXT("failed"), TEXT("schema_mismatch_expected_uai.uobject_properties.v2"), 0, 0, 1);
		}
		const TArray<TSharedPtr<FJsonValue>>* Properties = nullptr;
		if (!PropertiesJson->TryGetArrayField(TEXT("properties"), Properties) || !Properties)
		{
			return Finish(TEXT("failed"), TEXT("missing_properties_array"), 0, 0, 1);
		}

		TArray<TSharedPtr<FJsonValue>> Projected;
		FString FailedPath;
		FString FailureReason;
		if (!ProjectLegacyApplyDescriptors(*Properties, Projected, FailedPath, FailureReason))
		{
			Results.Add(MakeShared<FJsonValueObject>(MakeApplyResult(FailedPath, TEXT("failed"), FailureReason)));
			return Finish(TEXT("failed"), TEXT("preflight_failed"), Properties->Num(), 0, 1);
		}

		if (Projected.IsEmpty())
		{
			return Finish(TEXT("applied"), FString(), 0, 0, 0);
		}

		UeAgentAuthoringDirectApply::FResult ApplyResult;
		if (!UeAgentAuthoringDirectApply::ApplyObjectPatch(
			Object, Projected, MakeObjectApplyOptions(Options), ApplyResult))
		{
			Results.Add(MakeShared<FJsonValueObject>(MakeApplyResult(
				ApplyResult.PropertyPath, TEXT("failed"), ApplyResult.ErrorCode)));
			return Finish(TEXT("failed"), ApplyResult.ErrorCode, Properties->Num(), 0, 1);
		}
		for (const TSharedPtr<FJsonValue>& Value : Projected)
		{
			Results.Add(MakeShared<FJsonValueObject>(MakeApplyResult(
				Value->AsObject()->GetStringField(TEXT("property_path")), TEXT("applied"))));
		}
		return Finish(TEXT("applied"), FString(), Properties->Num(), Projected.Num(), 0);
	}

	bool ParseApplySummary(
		const TSharedPtr<FJsonObject>& Summary,
		FApplySummaryResult& OutResult)
	{
		OutResult = FApplySummaryResult();
		if (!Summary.IsValid())
		{
			OutResult.ErrorCode = TEXT("property_apply_summary_missing");
			OutResult.ErrorDetail = OutResult.ErrorCode;
			return false;
		}

		FString Status;
		if (Summary->TryGetStringField(TEXT("status"), Status) && Status == TEXT("applied"))
		{
			double Applied = 0.0;
			Summary->TryGetNumberField(TEXT("properties_applied"), Applied);
			OutResult.bApplied = true;
			OutResult.PropertiesApplied = FMath::RoundToInt(Applied);
			return true;
		}

		Summary->TryGetStringField(TEXT("error_code"), OutResult.ErrorCode);
		Summary->TryGetStringField(TEXT("error_detail"), OutResult.ErrorDetail);
		FString SummaryReason;
		Summary->TryGetStringField(TEXT("reason"), SummaryReason);

		const TArray<TSharedPtr<FJsonValue>>* Properties = nullptr;
		if (Summary->TryGetArrayField(TEXT("properties"), Properties) && Properties)
		{
			for (const TSharedPtr<FJsonValue>& Value : *Properties)
			{
				const TSharedPtr<FJsonObject> Property = Value.IsValid() && Value->Type == EJson::Object
					? Value->AsObject()
					: nullptr;
				FString PropertyStatus;
				if (!Property.IsValid()
					|| !Property->TryGetStringField(TEXT("status"), PropertyStatus)
					|| PropertyStatus != TEXT("failed"))
				{
					continue;
				}
				Property->TryGetStringField(TEXT("property_path"), OutResult.PropertyPath);
				FString PropertyReason;
				Property->TryGetStringField(TEXT("reason"), PropertyReason);
				if (OutResult.ErrorCode.IsEmpty()) OutResult.ErrorCode = PropertyReason;
				if (OutResult.ErrorDetail.IsEmpty())
				{
					OutResult.ErrorDetail = OutResult.PropertyPath.IsEmpty()
						? PropertyReason
						: OutResult.PropertyPath + TEXT(":") + PropertyReason;
				}
				break;
			}
		}

		if (OutResult.ErrorCode.IsEmpty()) OutResult.ErrorCode = SummaryReason;
		if (OutResult.ErrorCode.IsEmpty()) OutResult.ErrorCode = TEXT("property_apply_failed");
		if (OutResult.ErrorDetail.IsEmpty()) OutResult.ErrorDetail = SummaryReason;
		if (OutResult.ErrorDetail.IsEmpty()) OutResult.ErrorDetail = OutResult.ErrorCode;
		return false;
	}

}
