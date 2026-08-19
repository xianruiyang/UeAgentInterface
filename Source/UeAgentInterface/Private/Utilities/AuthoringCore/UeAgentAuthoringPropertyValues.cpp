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
			Property->InitializeValue(ValuePtr);
		}
	}

	FPropertyValueStorage::~FPropertyValueStorage()
	{
		if (Property && ValuePtr) Property->DestroyValue(ValuePtr);
		FMemory::Free(ValuePtr);
		ValuePtr = nullptr;
	}

	TSharedPtr<FPropertyValueStorage> FPropertyValueStorage::CreateInitialized(FProperty* Property, FString& OutError)
	{
		if (!Property)
		{
			OutError = TEXT("property_value_storage_property_invalid");
			return nullptr;
		}
		TSharedPtr<FPropertyValueStorage> Storage = MakeShareable(new FPropertyValueStorage(Property));
		if (!Storage->ValuePtr)
		{
			OutError = TEXT("property_value_storage_allocation_failed");
			return nullptr;
		}
		OutError.Reset();
		return Storage;
	}

	TSharedPtr<FPropertyValueStorage> FPropertyValueStorage::CopyFrom(
		FProperty* Property,
		const void* SourceValuePtr,
		FString& OutError)
	{
		if (!Property || !SourceValuePtr)
		{
			OutError = TEXT("property_value_storage_copy_source_invalid");
			return nullptr;
		}
		TSharedPtr<FPropertyValueStorage> Storage = CreateInitialized(Property, OutError);
		if (!Storage.IsValid()) return nullptr;
		Property->CopyCompleteValue(Storage->ValuePtr, SourceValuePtr);
		OutError.Reset();
		return Storage;
	}
}
