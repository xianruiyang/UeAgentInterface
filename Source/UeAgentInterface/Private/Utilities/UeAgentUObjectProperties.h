// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"

class FJsonObject;
class FJsonValue;
class FProperty;
class AActor;
class UActorComponent;
class UObject;
class UScriptStruct;

namespace UeAgentUObjectProperties
{
	enum class EExportProfile : uint8
	{
		Index,
		Authoring,
		Readback,
		Debug
	};

	struct UEAGENTINTERFACE_API FExportOptions
	{
		FExportOptions();

		EExportProfile Profile;
		bool bIncludeTransient;
		bool bIncludeDeprecated;
		bool bIncludeEditorOnly;
		bool bIncludeReadOnly;
		bool bIncludeNonEditable;
		int32 MaxProperties;
		int32 MaxDepth;
		int32 MaxContainerItems;
		int32 MaxStringChars;
		TFunction<TOptional<bool>(const UScriptStruct* Struct, const FProperty* Field)> StructFieldOverride;
		TFunction<bool(const UScriptStruct* Struct, const FProperty* Field)> StructFieldFilter;
	};

	struct UEAGENTINTERFACE_API FApplyOptions
	{
		FApplyOptions();

		bool bAllowReadOnly;
		bool bAllowTemplateOwners;
		bool bAllowDomainAuthorizedOwners;
		bool bEmitEditorNotifications;
		bool bMarkPackageDirty;
		int32 MaxContainerItems;
		int32 MaxStringChars;
	};

	struct UEAGENTINTERFACE_API FApplySummaryResult
	{
		bool bApplied = false;
		int32 PropertiesApplied = 0;
		FString ErrorCode;
		FString ErrorDetail;
		FString PropertyPath;
	};

	UEAGENTINTERFACE_API FString GetClassPath(const UClass* Class);
	UEAGENTINTERFACE_API TSharedPtr<FJsonObject> BuildObjectIdentity(const UObject* Object);
	UEAGENTINTERFACE_API bool ShouldSkipProperty(const FProperty* Property, const FExportOptions& Options, FString& OutReason, const UObject* Owner = nullptr);
	UEAGENTINTERFACE_API bool ShouldIncludeStructField(
		const UScriptStruct* Struct,
		const FProperty* Field,
		const FExportOptions& Options,
		const UObject* Owner = nullptr);
	UEAGENTINTERFACE_API TSharedPtr<FJsonValue> ExportPropertyValue(FProperty* Property, const void* ValuePtr, UObject* Owner, const FExportOptions& Options, int32 Depth = 0);
	UEAGENTINTERFACE_API TSharedPtr<FJsonObject> BuildPropertyDescriptor(UObject* Object, FProperty* Property, const FExportOptions& Options);
	UEAGENTINTERFACE_API TSharedPtr<FJsonObject> ExportObjectProperties(UObject* Object, const FExportOptions& Options);
	UEAGENTINTERFACE_API TSharedPtr<FJsonObject> ExportActorProperties(AActor* Actor, const FExportOptions& Options);
	UEAGENTINTERFACE_API TSharedPtr<FJsonObject> ExportComponentProperties(UActorComponent* Component, const FExportOptions& Options);
	UEAGENTINTERFACE_API TArray<TSharedPtr<FJsonValue>> ExportActorComponentProperties(AActor* Actor, const FExportOptions& Options);
	UEAGENTINTERFACE_API bool ValidatePropertyDescriptorShape(
		const TSharedPtr<FJsonObject>& Descriptor,
		FString& OutPropertyPath,
		TSharedPtr<FJsonValue>& OutValue,
		FString& OutError);
	UEAGENTINTERFACE_API TSharedPtr<FJsonObject> ApplyStructProperties(
		UStruct* StructType,
		void* StructData,
		UObject* Owner,
		const TSharedPtr<FJsonObject>& PropertiesJson,
		const FApplyOptions& Options);
	UEAGENTINTERFACE_API TSharedPtr<FJsonObject> ApplyObjectProperties(UObject* Object, const TSharedPtr<FJsonObject>& PropertiesJson, const FApplyOptions& Options);
	UEAGENTINTERFACE_API bool ParseApplySummary(
		const TSharedPtr<FJsonObject>& Summary,
		FApplySummaryResult& OutResult);
}
