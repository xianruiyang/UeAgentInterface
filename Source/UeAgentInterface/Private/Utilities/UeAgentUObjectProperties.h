// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "Core/Contract/UeAgentDiagnostic.h"

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
		// Original properties-array occurrence; supplement the whole diagnostic only.
		TFunction<void(UE::UAI::Contract::FDiagnostic&, int32)> SupplementDiagnosticSource;
	};

	struct UEAGENTINTERFACE_API FApplySummaryResult
	{
		bool bApplied = false;
		int32 PropertiesApplied = 0;
		UE::UAI::Contract::FDiagnostic Diagnostic;
        TArray<FString> AppliedPropertyPaths;
	};

	UEAGENTINTERFACE_API FString GetClassPath(const UClass* Class);
	UEAGENTINTERFACE_API TSharedPtr<FJsonObject> BuildObjectIdentity(const UObject* Object);
	UEAGENTINTERFACE_API bool ShouldSkipProperty(const FProperty* Property, const FExportOptions& Options, FString& OutReason, const UObject* Owner = nullptr);
	UEAGENTINTERFACE_API bool ShouldIncludeStructField(
		const UScriptStruct* Struct,
		const FProperty* Field,
		const FExportOptions& Options,
		const UObject* Owner = nullptr);
	UEAGENTINTERFACE_API TSharedPtr<FJsonValue> ExportPropertyValue(FProperty* Property, const void* ValuePtr, UObject* Owner, const FExportOptions& Options, int32 Depth = 0, TSharedPtr<FJsonObject>* OutValueDefinition = nullptr);
	UEAGENTINTERFACE_API TSharedPtr<FJsonObject> BuildPropertyDescriptor(UObject* Object, FProperty* Property, const FExportOptions& Options);
	UEAGENTINTERFACE_API TSharedPtr<FJsonObject> ExportObjectProperties(UObject* Object, const FExportOptions& Options);
	UEAGENTINTERFACE_API TSharedPtr<FJsonObject> ExportActorProperties(AActor* Actor, const FExportOptions& Options);
	UEAGENTINTERFACE_API TSharedPtr<FJsonObject> ExportComponentProperties(UActorComponent* Component, const FExportOptions& Options);
	UEAGENTINTERFACE_API TArray<TSharedPtr<FJsonValue>> ExportActorComponentProperties(AActor* Actor, const FExportOptions& Options);
	UEAGENTINTERFACE_API bool ValidatePropertyDescriptorShape(
		const TSharedPtr<FJsonObject>& Descriptor,
		FString& OutPropertyPath,
		TSharedPtr<FJsonValue>& OutValue,
		UE::UAI::Contract::FDiagnostic& OutError);
	UEAGENTINTERFACE_API bool ApplyStructProperties(
		UStruct* StructType,
		void* StructData,
		UObject* Owner,
		const TSharedPtr<FJsonObject>& PropertiesJson,
		const FApplyOptions& Options, FApplySummaryResult& OutResult);
    UEAGENTINTERFACE_API bool ApplyObjectProperties(UObject* Object, const TSharedPtr<FJsonObject>& PropertiesJson, const FApplyOptions& Options, FApplySummaryResult& OutResult);
}
