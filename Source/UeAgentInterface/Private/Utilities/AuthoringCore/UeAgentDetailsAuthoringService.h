// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Utilities/AuthoringCore/UeAgentAuthoringState.h"
#include "Utilities/AuthoringObjectApply/UeAgentAuthoringObjectApply.h"
#include "Utilities/AuthoringOperationApply/UeAgentAuthoringDirectApply.h"

class FJsonObject;
class FJsonValue;
class FProperty;
class UObject;
class UScriptStruct;
class UStruct;

namespace UeAgentDetailsAuthoringService
{
	struct UEAGENTINTERFACE_API FCapturePolicy
	{
		FString SemanticId = TEXT("core.property");
		int32 MaxContainerItems = 256;
		int32 MaxStringChars = 4096;
		TSet<FName> ExcludedProperties;
		TFunction<bool(const FProperty* Property, const void* ValuePtr, const UObject* Owner)> IncludeProperty;
		TFunction<bool(const UScriptStruct* Struct, const FProperty* Field)> StructFieldFilter;
		TFunction<bool(
			const FString& PropertyPath,
			UeAgentAuthoringIdentity::FCanonicalIdentity& OutIdentity,
			FString& OutError)> BuildSnapshotIdentity;
	};

	struct UEAGENTINTERFACE_API FProjectionPolicy
	{
		FString Profile = TEXT("authoring");
		bool bRequireEditable = true;
		bool bIncludeDisplayName = true;
		bool bIncludeAvailability = true;
		bool bIncludeCategoryPath = true;
		bool bIncludeTooltip = true;
		bool bPreserveValueText = false;
	};

	UEAGENTINTERFACE_API TSharedPtr<FJsonObject> BuildPropertyDescriptor(
		const UeAgentAuthoringState::FStateEntry& Entry,
		const FProjectionPolicy& Policy = FProjectionPolicy());

	UEAGENTINTERFACE_API bool BuildPropertiesDocument(
		const TArray<UeAgentAuthoringState::FStateEntry>& Entries,
		const FProjectionPolicy& Policy,
		TSharedPtr<FJsonObject>& OutDocument,
		FString& OutError);

	UEAGENTINTERFACE_API bool CaptureObjectProperties(
		const UObject* Object,
		const FCapturePolicy& Policy,
		UeAgentAuthoringState::FStateSnapshot& OutSnapshot,
		TSharedPtr<FJsonObject>& OutDocument,
		FString& OutError);

	UEAGENTINTERFACE_API bool CaptureStructProperties(
		UStruct* StructType,
		const void* StructData,
		const FString& RootOwnerPath,
		const UObject* NotificationOwner,
		const FCapturePolicy& Policy,
		UeAgentAuthoringState::FStateSnapshot& OutSnapshot,
		TSharedPtr<FJsonObject>& OutDocument,
		FString& OutError);

	UEAGENTINTERFACE_API bool AppendStructPropertyReadRequests(
		UStruct* StructType,
		const void* StructData,
		const FString& RootOwnerPath,
		const UObject* NotificationOwner,
		const FCapturePolicy& Policy,
		TArray<UeAgentAuthoringState::FReadRequest>& OutRequests,
		FString& OutError);

	UEAGENTINTERFACE_API bool CaptureObjectProperty(
		const UObject* Object,
		const FString& PropertyPath,
		const FCapturePolicy& Policy,
		UeAgentAuthoringState::FStateEntry& OutEntry,
		TSharedPtr<FJsonObject>& OutDescriptor,
		FString& OutError);

	UEAGENTINTERFACE_API bool CaptureStructPropertyPaths(
		UStruct* StructType,
		const void* StructData,
		const FString& RootOwnerPath,
		const UObject* NotificationOwner,
		const TArray<FString>& PropertyPaths,
		const FCapturePolicy& Policy,
		TSharedPtr<FJsonObject>& OutDocument,
		FString& OutError);

	UEAGENTINTERFACE_API bool CaptureStructDeltaFields(
		UScriptStruct* StructType,
		const void* StructData,
		const void* DefaultData,
		const FString& RootOwnerPath,
		const UObject* NotificationOwner,
		TSharedPtr<FJsonObject>& OutFields,
		FString& OutError);

	UEAGENTINTERFACE_API bool DecodePropertiesDocument(
		const TSharedPtr<FJsonObject>& Document,
		TArray<TSharedPtr<FJsonValue>>& OutDescriptors,
		FString& OutError);

	UEAGENTINTERFACE_API bool ApplyObjectProperties(
		UObject* Object,
		const TSharedPtr<FJsonObject>& Document,
		const UeAgentAuthoringObjectApply::FOptions& Options,
		UeAgentAuthoringDirectApply::FResult& OutResult,
		FString& OutError);

	UEAGENTINTERFACE_API bool ApplyStructProperties(
		UStruct* StructType,
		void* StructData,
		UObject* NotificationOwner,
		const TSharedPtr<FJsonObject>& Document,
		const UeAgentAuthoringObjectApply::FOptions& Options,
		UeAgentAuthoringDirectApply::FResult& OutResult,
		FString& OutError);
}
