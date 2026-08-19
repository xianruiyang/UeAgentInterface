// Copyright Epic Games, Inc. All Rights Reserved.

#include "Utilities/AuthoringCore/UeAgentAuthoringReflectionDescriptors.h"

#include "Utilities/UeAgentPropertyValueCodec.h"

#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "ISinglePropertyView.h"
#include "Modules/ModuleManager.h"
#include "PropertyEditorModule.h"
#include "PropertyHandle.h"
#include "UObject/Class.h"
#include "UObject/UnrealType.h"

namespace UeAgentAuthoringReflection
{
	namespace
	{
		void AddPropertyFlag(const FProperty* Property, EPropertyFlags Flag, const TCHAR* Name, TArray<FString>& OutFlags)
		{
			if (Property && Property->HasAnyPropertyFlags(Flag)) OutFlags.Add(Name);
		}

		void AddConstraint(const FProperty* Property, const TCHAR* SourceKey, const TCHAR* OutputKey, TMap<FString, FString>& OutConstraints)
		{
			if (!Property) return;
			const FString Value = Property->GetMetaData(SourceKey);
			if (!Value.IsEmpty()) OutConstraints.Add(OutputKey, Value);
		}

		TArray<TSharedPtr<FJsonValue>> MakeStringArray(const TArray<FString>& Values)
		{
			TArray<TSharedPtr<FJsonValue>> Result;
			Result.Reserve(Values.Num());
			for (const FString& Value : Values) Result.Add(MakeShared<FJsonValueString>(Value));
			return Result;
		}
	}

	TSharedPtr<FJsonObject> FDescriptor::MakeAvailabilityJson() const
	{
		if (bEditable && EditCondition.IsEmpty() && !bEditConditionHides) return nullptr;
		TSharedPtr<FJsonObject> Availability = MakeShared<FJsonObject>();
		Availability->SetBoolField(TEXT("enabled"), bEditable);
		Availability->SetBoolField(TEXT("editable"), bEditable);
		if (!EditCondition.IsEmpty()) Availability->SetStringField(TEXT("edit_condition"), EditCondition);
		if (bEditConditionHides) Availability->SetBoolField(TEXT("edit_condition_hides"), true);
		if (!bEditable && !AvailabilityReason.IsEmpty()) Availability->SetStringField(TEXT("reason"), AvailabilityReason);
		return Availability;
	}

	TSharedPtr<FJsonObject> FDescriptor::MakeConstraintsJson() const
	{
		if (Constraints.IsEmpty()) return nullptr;
		TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
		for (const TPair<FString, FString>& Pair : Constraints) Result->SetStringField(Pair.Key, Pair.Value);
		return Result;
	}

	TArray<TSharedPtr<FJsonValue>> FDescriptor::MakeCategoryPathJson() const
	{
		return MakeStringArray(CategoryPath);
	}

	TArray<TSharedPtr<FJsonValue>> FDescriptor::MakePropertyFlagsJson() const
	{
		return MakeStringArray(PropertyFlags);
	}

	bool IsEditableByFlags(const FProperty* Property, const UObject* Owner)
	{
		if (!Property
			|| !Property->HasAnyPropertyFlags(CPF_Edit)
			|| Property->HasAnyPropertyFlags(CPF_EditConst | CPF_Deprecated | CPF_Transient
				| CPF_DuplicateTransient | CPF_NonPIEDuplicateTransient | CPF_TextExportTransient))
		{
			return false;
		}
		const bool bTemplateOwner = Owner && Owner->IsTemplate();
		if (Property->HasAnyPropertyFlags(CPF_DisableEditOnInstance) && (!Owner || !bTemplateOwner)) return false;
		if (Property->HasAnyPropertyFlags(CPF_DisableEditOnTemplate) && (!Owner || bTemplateOwner)) return false;
		return true;
	}

	bool IsOwnedObjectProperty(const FProperty* Property)
	{
		const FObjectPropertyBase* ObjectProperty = CastField<FObjectPropertyBase>(Property);
		if (!ObjectProperty || CastField<FClassProperty>(Property)) return false;
		return Property->HasAnyPropertyFlags(CPF_InstancedReference)
			|| (ObjectProperty->PropertyClass
				&& ObjectProperty->PropertyClass->HasAnyClassFlags(CLASS_EditInlineNew)
				&& Property->HasAnyPropertyFlags(CPF_ExportObject));
	}

	bool EvaluateEditCondition(UObject* Owner, const FProperty* Property, bool& bOutSatisfied, FString& OutReason)
	{
		bOutSatisfied = true;
		OutReason.Reset();
		if (!Property || !Property->HasMetaData(TEXT("EditCondition"))) return true;
		const UClass* PropertyOwnerClass = Cast<UClass>(Property->GetOwnerStruct());
		if (!Owner || !PropertyOwnerClass || !Owner->IsA(PropertyOwnerClass))
		{
			OutReason = TEXT("property_edit_condition_requires_object_context");
			return false;
		}

		FPropertyEditorModule* PropertyEditor = FModuleManager::LoadModulePtr<FPropertyEditorModule>(TEXT("PropertyEditor"));
		if (!PropertyEditor)
		{
			OutReason = TEXT("property_edit_condition_evaluator_unavailable");
			return false;
		}
		FSinglePropertyParams Params;
		Params.NamePlacement = EPropertyNamePlacement::Hidden;
		const TSharedPtr<ISinglePropertyView> View = PropertyEditor->CreateSingleProperty(Owner, Property->GetFName(), Params);
		const TSharedPtr<IPropertyHandle> Handle = View.IsValid() ? View->GetPropertyHandle() : nullptr;
		if (!Handle.IsValid() || !Handle->IsValidHandle())
		{
			OutReason = TEXT("property_edit_condition_evaluation_failed");
			return false;
		}

		bOutSatisfied = Handle->IsEditable();
		if (!bOutSatisfied) OutReason = TEXT("property_edit_condition_false");
		return true;
	}

	bool IsEditableProperty(const FProperty* Property, UObject* Owner, FString* OutReason)
	{
		if (OutReason) OutReason->Reset();
		if (!IsEditableByFlags(Property, Owner))
		{
			if (OutReason) *OutReason = TEXT("not_editable_by_property_flags");
			return false;
		}
		// PropertyEditor can evaluate conditions only for fields owned directly by
		// a UClass. Struct authoring must keep CPF_Edit fields available so one
		// transaction can update a condition source and its dependent field.
		if (!Cast<UClass>(Property->GetOwnerStruct())) return true;
		bool bSatisfied = true;
		FString ConditionReason;
		if (!EvaluateEditCondition(Owner, Property, bSatisfied, ConditionReason) || !bSatisfied)
		{
			if (OutReason) *OutReason = ConditionReason;
			return false;
		}
		return true;
	}

	bool Build(const FProperty* Property, UObject* Owner, const void* ValuePtr, FDescriptor& OutDescriptor, FString& OutError)
	{
		OutDescriptor = FDescriptor();
		if (!Property)
		{
			OutError = TEXT("invalid_property");
			return false;
		}

		OutDescriptor.PropertyName = Property->GetName();
		OutDescriptor.OwnerStructPath = Property->GetOwnerStruct() ? Property->GetOwnerStruct()->GetPathName() : FString();
		OutDescriptor.DisplayName = Property->GetDisplayNameText().ToString();
		OutDescriptor.Tooltip = Property->GetToolTipText().ToString();
		OutDescriptor.Type = UeAgentAuthoringTypeDescriptors::Describe(Property);
		OutDescriptor.bEditorOnly = Property->IsEditorOnlyProperty();
		OutDescriptor.bTransient = Property->HasAnyPropertyFlags(CPF_Transient);
		OutDescriptor.bDeprecated = Property->HasAnyPropertyFlags(CPF_Deprecated) || Property->HasMetaData(TEXT("DeprecatedProperty"));
		OutDescriptor.EditCondition = Property->GetMetaData(TEXT("EditCondition"));
		OutDescriptor.bEditConditionHides = Property->HasMetaData(TEXT("EditConditionHides"));

		TArray<FString> CategoryParts;
		Property->GetMetaData(TEXT("Category")).ParseIntoArray(CategoryParts, TEXT("|"), true);
		for (FString Part : CategoryParts)
		{
			Part.TrimStartAndEndInline();
			if (!Part.IsEmpty()) OutDescriptor.CategoryPath.Add(MoveTemp(Part));
		}

		AddPropertyFlag(Property, CPF_Edit, TEXT("Edit"), OutDescriptor.PropertyFlags);
		AddPropertyFlag(Property, CPF_EditConst, TEXT("EditConst"), OutDescriptor.PropertyFlags);
		AddPropertyFlag(Property, CPF_BlueprintVisible, TEXT("BlueprintVisible"), OutDescriptor.PropertyFlags);
		AddPropertyFlag(Property, CPF_BlueprintReadOnly, TEXT("BlueprintReadOnly"), OutDescriptor.PropertyFlags);
		AddPropertyFlag(Property, CPF_Config, TEXT("Config"), OutDescriptor.PropertyFlags);
		AddPropertyFlag(Property, CPF_GlobalConfig, TEXT("GlobalConfig"), OutDescriptor.PropertyFlags);
		AddPropertyFlag(Property, CPF_Transient, TEXT("Transient"), OutDescriptor.PropertyFlags);
		AddPropertyFlag(Property, CPF_Deprecated, TEXT("Deprecated"), OutDescriptor.PropertyFlags);
		AddPropertyFlag(Property, CPF_DuplicateTransient, TEXT("DuplicateTransient"), OutDescriptor.PropertyFlags);
		AddPropertyFlag(Property, CPF_NonPIEDuplicateTransient, TEXT("NonPIEDuplicateTransient"), OutDescriptor.PropertyFlags);
		AddPropertyFlag(Property, CPF_TextExportTransient, TEXT("TextExportTransient"), OutDescriptor.PropertyFlags);
		AddPropertyFlag(Property, CPF_SaveGame, TEXT("SaveGame"), OutDescriptor.PropertyFlags);

		AddConstraint(Property, TEXT("ClampMin"), TEXT("clamp_min"), OutDescriptor.Constraints);
		AddConstraint(Property, TEXT("ClampMax"), TEXT("clamp_max"), OutDescriptor.Constraints);
		AddConstraint(Property, TEXT("UIMin"), TEXT("ui_min"), OutDescriptor.Constraints);
		AddConstraint(Property, TEXT("UIMax"), TEXT("ui_max"), OutDescriptor.Constraints);
		AddConstraint(Property, TEXT("Units"), TEXT("units"), OutDescriptor.Constraints);

		FString PropertyReason;
		OutDescriptor.bEditableByProperty = IsEditableProperty(Property, Owner, &PropertyReason);
		FString CodecReason;
		OutDescriptor.bCodecSupported = UeAgentPropertyValueCodec::IsAuthoringSupported(Property, ValuePtr, CodecReason);
		OutDescriptor.bEditable = OutDescriptor.bEditableByProperty && OutDescriptor.bCodecSupported;
		if (!OutDescriptor.bEditable)
		{
			OutDescriptor.AvailabilityReason = OutDescriptor.bCodecSupported ? PropertyReason : CodecReason;
		}

		OutError.Reset();
		return true;
	}
}
