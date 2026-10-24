// Copyright Epic Games, Inc. All Rights Reserved.

#include "Utilities/AuthoringCore/UeAgentAuthoringPropertyValues.h"

#include "UObject/UnrealType.h"

namespace UeAgentAuthoringPropertyValues
{
	FPropertyValueStorage::FPropertyValueStorage(FProperty* InProperty)
		: Property(InProperty)
	{
		if (Property)
		{
			ValuePtr = FMemory::Malloc(Property->GetSize(), Property->GetMinAlignment());
			if (ValuePtr) Property->InitializeValue(ValuePtr);
		}
	}

	FPropertyValueStorage::~FPropertyValueStorage()
	{
		if (Property && ValuePtr) Property->DestroyValue(ValuePtr);
		FMemory::Free(ValuePtr);
		ValuePtr = nullptr;
	}

	TSharedPtr<FPropertyValueStorage> FPropertyValueStorage::CreateInitialized(FProperty* Property, FDiagnostic& OutError)
	{
		if (!Property)
		{
			OutError = FDiagnostic::Failure(TEXT("property_value_storage_property_invalid"), TEXT("Property value storage requires a native property descriptor."), TEXT("property_value_storage"));
			OutError.SourceKind = TEXT("native_result");
			OutError.PropertyPath = TEXT("property");
			OutError.ExpectedValuePresent = true;
			OutError.ActualValuePresent = false;
			return nullptr;
		}
		TSharedPtr<FPropertyValueStorage> Storage = MakeShareable(new FPropertyValueStorage(Property));
		if (!Storage->ValuePtr)
		{
			OutError = FDiagnostic::Failure(TEXT("property_value_storage_allocation_failed"), TEXT("Allocating storage for the native property value failed."), TEXT("property_value_storage"));
			OutError.SourceKind = TEXT("native_result");
			OutError.CandidateName = Property->GetName();
			OutError.ResourceDimension = TEXT("bytes");
			OutError.ResourceObserved = Property->GetSize();
			return nullptr;
		}
		OutError.Reset();
		return Storage;
	}

	TSharedPtr<FPropertyValueStorage> FPropertyValueStorage::CopyFrom(
		FProperty* Property,
		const void* SourceValuePtr,
		FDiagnostic& OutError)
	{
		if (!Property || !SourceValuePtr)
		{
			OutError = FDiagnostic::Failure(TEXT("property_value_storage_copy_source_invalid"), !Property
				? TEXT("Copying a property value requires a native property descriptor.")
				: TEXT("Copying a property value requires an existing source value."), TEXT("property_value_storage"));
			OutError.SourceKind = TEXT("native_result");
			OutError.CandidateName = Property ? Property->GetName() : FString();
			OutError.PropertyPath = !Property ? TEXT("property") : TEXT("source_value");
			OutError.ExpectedValuePresent = true;
			OutError.ActualValuePresent = false;
			return nullptr;
		}
		TSharedPtr<FPropertyValueStorage> Storage = CreateInitialized(Property, OutError);
		if (!Storage.IsValid()) return nullptr;
		Property->CopyCompleteValue(Storage->ValuePtr, SourceValuePtr);
		OutError.Reset();
		return Storage;
	}
}
