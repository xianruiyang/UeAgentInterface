// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Core/Contract/UeAgentDiagnostic.h"
#include "Utilities/AuthoringCore/UeAgentAuthoringState.h"
#include "Utilities/AuthoringObjectApply/UeAgentAuthoringObjectApply.h"
#include "Utilities/AuthoringOperationApply/UeAgentAuthoringDirectApply.h"

class FJsonObject;
class FJsonValue;
class FProperty;
class UObject;
class UScriptStruct;
class UStruct;
class UPackage;
namespace UE::UAI::HjsonWorkspace { class FHjsonNode; class FWorkspaceError; }

namespace UeAgentDetailsAuthoringService
{
	using UE::UAI::Contract::FDiagnostic;
	struct UEAGENTINTERFACE_API FCapturePolicy
	{
		FString SemanticId = TEXT("core.property");
		bool bShowDefaultsOnly = false;
		bool bUseInstanceEditability = false;
		TSet<FName> ExcludedProperties;
		TFunction<bool(const FProperty* Property, const void* ValuePtr, const UObject* Owner)> IncludeProperty;
		TFunction<bool(const UScriptStruct* Struct, const FProperty* Field)> StructFieldFilter;
		TFunction<bool(const FProperty* Property)> AuthorizeProperty;
		TFunction<bool(
			const FString& PropertyPath,
			UeAgentAuthoringIdentity::FCanonicalIdentity& OutIdentity,
			FDiagnostic& OutError)> BuildSnapshotIdentity;
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
		FDiagnostic& OutError);

	UEAGENTINTERFACE_API bool CaptureObjectProperties(
		const UObject* Object,
		const FCapturePolicy& Policy,
		UeAgentAuthoringState::FStateSnapshot& OutSnapshot,
		TSharedPtr<FJsonObject>& OutDocument,
		FDiagnostic& OutError);

	UEAGENTINTERFACE_API bool CaptureObjectSnapshot(
		const UObject* Object, const FCapturePolicy& Policy,
		UeAgentAuthoringState::FStateSnapshot& OutSnapshot, FDiagnostic& OutError);

	UEAGENTINTERFACE_API bool CaptureStructProperties(
		UStruct* StructType,
		const void* StructData,
		const FString& RootOwnerPath,
		const UObject* NotificationOwner,
		const FCapturePolicy& Policy,
		UeAgentAuthoringState::FStateSnapshot& OutSnapshot,
		TSharedPtr<FJsonObject>& OutDocument,
		FDiagnostic& OutError);

	UEAGENTINTERFACE_API bool CaptureStructSnapshot(
		UStruct* StructType, const void* StructData, const FString& RootOwnerPath,
		const UObject* NotificationOwner, const FCapturePolicy& Policy,
		UeAgentAuthoringState::FStateSnapshot& OutSnapshot, FDiagnostic& OutError);

	UEAGENTINTERFACE_API bool AppendStructPropertyReadRequests(
		UStruct* StructType,
		const void* StructData,
		const FString& RootOwnerPath,
		const UObject* NotificationOwner,
		const FCapturePolicy& Policy,
		TArray<UeAgentAuthoringState::FReadRequest>& OutRequests,
		FDiagnostic& OutError);

	UEAGENTINTERFACE_API bool CaptureObjectProperty(
		const UObject* Object,
		const FString& PropertyPath,
		const FCapturePolicy& Policy,
		UeAgentAuthoringState::FStateEntry& OutEntry,
		TSharedPtr<FJsonObject>& OutDescriptor,
		FDiagnostic& OutError);

	UEAGENTINTERFACE_API bool CaptureStructPropertyPaths(
		UStruct* StructType,
		const void* StructData,
		const FString& RootOwnerPath,
		const UObject* NotificationOwner,
		const TArray<FString>& PropertyPaths,
		const FCapturePolicy& Policy,
		TSharedPtr<FJsonObject>& OutDocument,
		FDiagnostic& OutError,
		UeAgentAuthoringState::FStateSnapshot* OutSnapshot = nullptr);

	UEAGENTINTERFACE_API bool CaptureStructDeltaFields(
		UScriptStruct* StructType,
		const void* StructData,
		const void* DefaultData,
		const FString& RootOwnerPath,
		const UObject* NotificationOwner,
		TSharedPtr<FJsonObject>& OutFields,
		FDiagnostic& OutError);

	UEAGENTINTERFACE_API bool DecodePropertiesDocument(
		const TSharedPtr<FJsonObject>& Document,
		TArray<TSharedPtr<FJsonValue>>& OutDescriptors,
		FDiagnostic& OutError);

	/** One property seed contract: descriptor array or its existing v2 document wrapper.
	 * Absent fields are optional; explicitly malformed fields are never treated as absent. */
	UEAGENTINTERFACE_API bool NormalizePropertyPayload(
		const TSharedPtr<FJsonValue>& Payload, TSharedPtr<FJsonObject>& OutDocument, FDiagnostic& OutError);
	UEAGENTINTERFACE_API bool ReadPropertyPayload(
		const TSharedPtr<const FJsonObject>& Owner, const TCHAR* Field,
		TSharedPtr<FJsonObject>& OutDocument, FDiagnostic& OutError);
	/** A captured workspace document also contains unchanged context. Project its
	 * changed descriptors before native structural edits. Direct operation payloads
	 * remain explicit and must not call this without a captured baseline. */
	UEAGENTINTERFACE_API bool ProjectPropertyDocumentChanges(
		const TSharedPtr<FJsonObject>& Captured, const TSharedPtr<FJsonObject>& Desired,
		TSharedPtr<FJsonObject>& OutChanges, FDiagnostic& OutError);
	// Import preflight, before canonicalization loses field-level source provenance.
	UEAGENTINTERFACE_API bool ValidatePropertyPayload(
		const UE::UAI::HjsonWorkspace::FHjsonNode& Payload,
		UE::UAI::HjsonWorkspace::FWorkspaceError& OutError);

	/** Ordinary category/name/type/value rows, with native capture as the only schema. */
	UEAGENTINTERFACE_API bool ProjectReadableProperties(
		const UeAgentAuthoringState::FStateSnapshot& Snapshot,
		TSharedPtr<FJsonObject>& OutCategories, FDiagnostic& OutError,
		TSharedPtr<FJsonObject>* OutDefinitions = nullptr);
	UEAGENTINTERFACE_API bool DecodeReadableProperties(
		const UeAgentAuthoringState::FStateSnapshot& Snapshot,
		const TSharedPtr<FJsonValue>& Categories,
		TArray<TSharedPtr<FJsonValue>>& OutDescriptors, FDiagnostic& OutError);
	/** Returns only readable rows actually bound to the supplied native schema and passed to ObjectApply. */
	UEAGENTINTERFACE_API bool DecodeReadableProperties(
		const UeAgentAuthoringState::FStateSnapshot& Snapshot, const TSharedPtr<FJsonValue>& Categories,
		TArray<TSharedPtr<FJsonValue>>& OutDescriptors, FDiagnostic& OutError, TSharedPtr<FJsonObject>* BoundRows);
	/** Resolve unbound property failures against these exact rows and confirmed encoded-value leaves. */
	UEAGENTINTERFACE_API void LocateReadablePropertyFailure(FDiagnostic& Error,
		const TSharedPtr<FJsonValue>& Categories, const FString& DocumentPointer);
	/** Merged-value consumers can distinguish a confirmed leaf from an ancestor fallback.
	 * The callback only supplements source attribution and must retain the diagnostic facts. */
	UEAGENTINTERFACE_API void LocateReadablePropertyFailure(FDiagnostic& Error,
		const TSharedPtr<FJsonValue>& Categories, const FString& DocumentPointer,
		const TFunction<void(FDiagnostic&, bool bExact)>& OnLocated);

	UEAGENTINTERFACE_API bool ApplyObjectProperties(
		UObject* Object,
		const TSharedPtr<FJsonObject>& Document,
		const UeAgentAuthoringObjectApply::FOptions& Options,
		UeAgentAuthoringDirectApply::FResult& OutResult);

	// Internal document replay. All scoped descriptors bind and prepare before mutation.
	// The caller owns structural reconstruction, transaction and native notification policy.
	// Optional order lists every owner once; owners sharing a native target form one
	// group, in first-owner order, with descriptors appended in the supplied order.
	struct FScopedPropertyIntent
	{
		FString SemanticId;
		FString OwnerSelector;
		FString OwnerKey;
		FString PropertyPath;
		TMap<FString, FString> OwnerContext;
		UeAgentAuthoringObjectApply::FPropertyIntent Intent;
	};
	// Object-patch intents cover their whole native owner. Bind that immutable
	// intent to the current exported owner without retaining a storage pointer.
	UEAGENTINTERFACE_API void AppendScopedObjectPropertyIntents(
		const TArray<UeAgentAuthoringObjectApply::FPropertyIntent>& Intents,
		const FString& SemanticId, const FString& OwnerSelector, const FString& OwnerKey,
		TArray<FScopedPropertyIntent>& OutIntents, const TMap<FString, FString>& OwnerContext = {});
	UEAGENTINTERFACE_API bool VerifyScopedPropertyIntents(
		const TArray<FScopedPropertyIntent>& Intents,
		const TArray<UeAgentAuthoringState::FReadRequest>& CurrentRequests, FDiagnostic& OutError);

	UEAGENTINTERFACE_API bool ApplyScopedProperties(
		const TMap<FString, TSharedPtr<FJsonObject>>& PropertiesByOwner,
		const TArray<UeAgentAuthoringState::FReadRequest>& Requests,
		const FString& PropertySemanticId, const FString& OwnerSelector, const FString& ErrorPrefix,
		TFunctionRef<UeAgentAuthoringObjectApply::FOptions(const UeAgentAuthoringState::FReadRequest&)> OptionsForTarget,
		FDiagnostic& OutError, UPackage* TextIdentityPackage = nullptr,
		const TArray<FString>& OwnerApplyOrder = {}, TArray<FScopedPropertyIntent>* OutIntents = nullptr);

	UEAGENTINTERFACE_API bool ApplyStructProperties(
		UStruct* StructType,
		void* StructData,
		UObject* NotificationOwner,
		const TSharedPtr<FJsonObject>& Document,
		const UeAgentAuthoringObjectApply::FOptions& Options,
		UeAgentAuthoringDirectApply::FResult& OutResult);
}
