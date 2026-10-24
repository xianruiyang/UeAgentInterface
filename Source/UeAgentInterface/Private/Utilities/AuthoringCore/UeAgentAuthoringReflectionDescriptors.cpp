// Copyright Epic Games, Inc. All Rights Reserved.

#include "Utilities/AuthoringCore/UeAgentAuthoringReflectionDescriptors.h"

#include "Utilities/UeAgentPropertyValueCodec.h"

#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Modules/ModuleManager.h"
#include "PropertyEditorModule.h"
#include "UObject/Class.h"
#include "UObject/UnrealType.h"

namespace UeAgentAuthoringReflection
{
	namespace
	{
		bool IsPersistentField(const FProperty* Property)
		{
			return Property && !Property->HasAnyPropertyFlags(CPF_EditConst | CPF_Deprecated | CPF_Transient
				| CPF_DuplicateTransient | CPF_NonPIEDuplicateTransient | CPF_TextExportTransient);
		}

		bool HasNativeEditDeclaration(const FProperty* Property, const UStruct* Scope,
			TFunctionRef<bool(const FProperty*)> IsDirectlyEditable)
		{
			if (!IsPersistentField(Property)) return false;
			if (Property->HasAnyPropertyFlags(CPF_Edit)) return IsDirectlyEditable(Property);
			if (!CastField<FBoolProperty>(Property)) return false;
			Scope = Scope ? Scope : Property->GetOwnerStruct();
			if (!Scope) return false;
			for (TFieldIterator<FProperty> It(Scope, EFieldIterationFlags::IncludeSuper); It; ++It)
			{
				const FProperty* Controlled = *It;
				if (!IsDirectlyEditable(Controlled) || !Controlled->HasMetaData(TEXT("EditCondition"))) continue;
				const auto& PropertyEditor = FModuleManager::LoadModuleChecked<FPropertyEditorModule>(TEXT("PropertyEditor"));
				if (PropertyEditor.GetEditConditionToggleProperty(Controlled) == Property) return true;
			}
			return false;
		}

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

	void ReadConstraints(const FProperty* Property, TMap<FString, FString>& OutConstraints)
	{
		OutConstraints.Reset();
		AddConstraint(Property, TEXT("ClampMin"), TEXT("clamp_min"), OutConstraints);
		AddConstraint(Property, TEXT("ClampMax"), TEXT("clamp_max"), OutConstraints);
		AddConstraint(Property, TEXT("UIMin"), TEXT("ui_min"), OutConstraints);
		AddConstraint(Property, TEXT("UIMax"), TEXT("ui_max"), OutConstraints);
		AddConstraint(Property, TEXT("Units"), TEXT("units"), OutConstraints);
		AddConstraint(Property, TEXT("ForceUnits"), TEXT("force_units"), OutConstraints);
	}

	bool IsEditableDeclaration(const FProperty* Property, const UStruct* Scope)
	{
		Property = Property ? Property->GetOwnerProperty() : nullptr;
		return HasNativeEditDeclaration(Property, Scope, [](const FProperty* Candidate)
		{
			return IsPersistentField(Candidate) && Candidate->HasAnyPropertyFlags(CPF_Edit);
		});
	}

	bool IsEditableByFlags(const FProperty* Property, const UObject* Owner, bool bShowDefaultsOnly, bool bUseInstanceEditability, const UStruct* Scope)
	{
		// Container elements inherit their declaration; toggle controls inherit the native editing context.
		Property = Property ? Property->GetOwnerProperty() : nullptr;
		if (!Property) return false;
		if (!Scope)
		{
			Scope = Property->GetOwnerStruct();
			const UClass* DeclaringClass = Cast<UClass>(Scope);
			if (Owner && DeclaringClass && Owner->IsA(DeclaringClass)) Scope = Owner->GetClass();
		}
		const bool bTemplateOwner = !bUseInstanceEditability && Owner && Owner->IsTemplate();
		auto IsAvailableInContext = [&](const FProperty* Candidate)
		{
			if (!bShowDefaultsOnly && Candidate->HasAnyPropertyFlags(CPF_DisableEditOnInstance) && (!Owner || !bTemplateOwner)) return false;
			return !Candidate->HasAnyPropertyFlags(CPF_DisableEditOnTemplate) || (Owner && !bTemplateOwner);
		};
		return IsAvailableInContext(Property) && HasNativeEditDeclaration(Property, Scope, [&](const FProperty* Candidate)
		{
			return IsPersistentField(Candidate) && Candidate->HasAnyPropertyFlags(CPF_Edit) && IsAvailableInContext(Candidate);
		});
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

	bool IsExistingInlineObjectProperty(
		const FProperty* Property,
		const void* ValuePtr,
		const UObject* Owner)
	{
		const FObjectPropertyBase* ObjectProperty = CastField<FObjectPropertyBase>(Property);
		if (!ObjectProperty || CastField<FClassProperty>(Property) || !ValuePtr || !Owner
			|| !Property->HasAnyPropertyFlags(CPF_ExportObject)
			|| !Property->HasMetaData(TEXT("EditInline"))
			|| Property->HasMetaData(TEXT("NoEditInline")))
		{
			return false;
		}
		const UObject* Referenced = ObjectProperty->GetObjectPropertyValue(ValuePtr);
		return Referenced && Referenced->IsIn(Owner);
	}

	bool IsEditableProperty(const FProperty* Property, UObject* Owner, FString* OutReason, bool bShowDefaultsOnly, bool bUseInstanceEditability, const UStruct* Scope)
	{
		if (OutReason) OutReason->Reset();
		if (!IsEditableByFlags(Property, Owner, bShowDefaultsOnly, bUseInstanceEditability, Scope))
		{
			if (OutReason) *OutReason = TEXT("not_editable_by_property_flags");
			return false;
		}
		return true;
	}

	bool Build(const FProperty* Property, UObject* Owner, const void* ValuePtr, FDescriptor& OutDescriptor, FDiagnostic& OutError,
		bool bShowDefaultsOnly, bool bUseInstanceEditability, const UStruct* Scope)
	{
		OutDescriptor = FDescriptor();
		if (!Property)
		{
			OutError = FDiagnostic::Failure(TEXT("invalid_property"),
				TEXT("A reflected property descriptor cannot be built without a native property."), TEXT("property_descriptor"));
			OutError.SourceKind = TEXT("native_result");
			OutError.PropertyPath = TEXT("property");
			OutError.ExpectedValuePresent = true;
			OutError.ActualValuePresent = false;
			if (Owner) OutError.InternalContext = Owner->GetPathName();
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

		ReadConstraints(Property, OutDescriptor.Constraints);

		FString PropertyReason;
		OutDescriptor.bEditableByProperty = IsEditableProperty(Property, Owner, &PropertyReason, bShowDefaultsOnly, bUseInstanceEditability, Scope)
			|| IsExistingInlineObjectProperty(Property, ValuePtr, Owner);
		FDiagnostic CodecReason;
		OutDescriptor.bCodecSupported = UeAgentPropertyValueCodec::IsAuthoringSupported(Property, ValuePtr, CodecReason);
		OutDescriptor.bEditable = OutDescriptor.bEditableByProperty && OutDescriptor.bCodecSupported;
		if (!OutDescriptor.bEditable)
		{
			OutDescriptor.AvailabilityReason = OutDescriptor.bCodecSupported ? PropertyReason : CodecReason.Code;
		}

		OutError.Reset();
		return true;
	}
}
