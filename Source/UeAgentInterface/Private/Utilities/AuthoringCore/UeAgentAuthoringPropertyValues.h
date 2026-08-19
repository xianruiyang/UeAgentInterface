// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"

class FProperty;

namespace UeAgentAuthoringPropertyValues
{
	class UEAGENTINTERFACE_API FPropertyValueStorage
	{
	public:
		~FPropertyValueStorage();

		FProperty* GetProperty() const { return Property; }
		const void* GetValuePtr() const { return ValuePtr; }
		void* GetMutableValuePtr() { return ValuePtr; }

		static TSharedPtr<FPropertyValueStorage> CreateInitialized(FProperty* Property, FString& OutError);
		static TSharedPtr<FPropertyValueStorage> CopyFrom(FProperty* Property, const void* SourceValuePtr, FString& OutError);

	private:
		explicit FPropertyValueStorage(FProperty* InProperty);

		FProperty* Property = nullptr;
		void* ValuePtr = nullptr;
	};
}
