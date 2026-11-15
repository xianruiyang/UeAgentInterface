// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Core/Contract/UeAgentDiagnostic.h"
#include "Utilities/AuthoringCore/UeAgentAuthoringTypeDescriptors.h"

class FJsonObject;
class FJsonValue;
class FProperty;
class UObject;
class UStruct;

namespace UeAgentAuthoringReflection
{
using UE::UAI::Contract::FDiagnostic;
	struct FEditDeclarationScopeState;
	/** Details toggle relationships during native request construction. */
	class FScopedEditDeclarations final
	{
	public:
		FScopedEditDeclarations();
		~FScopedEditDeclarations();
		FScopedEditDeclarations(const FScopedEditDeclarations&) = delete;
		FScopedEditDeclarations& operator=(const FScopedEditDeclarations&) = delete;
	private:
		TUniquePtr<FEditDeclarationScopeState> State;
	};
	struct FDescriptorMetadataScopeState;
	/** Exact property metadata during one synchronous capture. This does not
	 * enable toggle relationship reuse across arbitrary accessor callbacks. */
	class FScopedDescriptorMetadata final
	{
	public:
		FScopedDescriptorMetadata();
		~FScopedDescriptorMetadata();
		FScopedDescriptorMetadata(const FScopedDescriptorMetadata&) = delete;
		FScopedDescriptorMetadata& operator=(const FScopedDescriptorMetadata&) = delete;
	private:
		TUniquePtr<FDescriptorMetadataScopeState> State;
	};
	/** Pure declaration data. Native source pointers/metadata checks remain in
	 * FScopedDescriptorMetadata; captured values may own this after that scope. */
	struct FSharedFieldDeclaration
	{
		UeAgentAuthoringTypeDescriptors::FDescriptor Type;
		TMap<FString, FString> Constraints;
		TSharedPtr<FJsonObject> Fragment;
	};

	struct UEAGENTINTERFACE_API FDescriptor
	{
		FString PropertyName;
		FString OwnerStructPath;
		FString DisplayName;
		FString Tooltip;
		TArray<FString> CategoryPath;
		TSharedPtr<const TArray<FString>> SharedCategoryPath;
		TArray<FString> PropertyFlags;
		UeAgentAuthoringTypeDescriptors::FDescriptor Type;
		TSharedPtr<FJsonObject> FieldDeclaration;
		TSharedPtr<const FSharedFieldDeclaration> SharedDeclaration;
		TMap<FString, FString> Constraints;
		FString EditCondition;
		FString AvailabilityReason;
		bool bEditConditionHides = false;
		bool bEditableByProperty = false;
		bool bCodecSupported = false;
		bool bEditable = false;
		bool bEditorOnly = false;
		bool bTransient = false;
		bool bDeprecated = false;
		const UeAgentAuthoringTypeDescriptors::FDescriptor& GetType() const { return SharedDeclaration ? SharedDeclaration->Type : Type; }
		const TMap<FString, FString>& GetConstraints() const { return SharedDeclaration ? SharedDeclaration->Constraints : Constraints; }
		const TArray<FString>& GetCategoryPath() const { return SharedCategoryPath ? *SharedCategoryPath : CategoryPath; }

		TSharedPtr<FJsonObject> MakeAvailabilityJson() const;
		TSharedPtr<FJsonObject> MakeConstraintsJson() const;
		TArray<TSharedPtr<FJsonValue>> MakeCategoryPathJson() const;
		TArray<TSharedPtr<FJsonValue>> MakePropertyFlagsJson() const;
	};

	/** Field declarations shared by top-level and nested value definitions. */
	UEAGENTINTERFACE_API void ReadConstraints(const FProperty* Property, TMap<FString, FString>& OutConstraints);
	/** Immutable declaration fragment, only for sealed capture consumers. Every
	 * hit verifies current type, constraints and the names-only enum sequence.
	 * No editable decision, value definition or callback result is cached. */
	UEAGENTINTERFACE_API TSharedPtr<FJsonObject> CaptureFieldDeclaration(const FProperty* Property);
	TSharedPtr<FJsonObject> CaptureFieldDeclaration(const FProperty* Property,
		UeAgentAuthoringTypeDescriptors::FDescriptor& OutType, TMap<FString, FString>& OutConstraints);
	/** Persistent native edit declarations, including boolean controls exposed by Details toggles. */
	UEAGENTINTERFACE_API bool IsEditableDeclaration(const FProperty* Property, const UStruct* Scope = nullptr);

	// Native Details views may show EditDefaultsOnly fields on non-template asset subobjects.
	UEAGENTINTERFACE_API bool IsEditableByFlags(const FProperty* Property, const UObject* Owner = nullptr,
		bool bShowDefaultsOnly = false, bool bUseInstanceEditability = false, const UStruct* Scope = nullptr);
	UEAGENTINTERFACE_API bool IsOwnedObjectProperty(const FProperty* Property);
	/** An existing object may expose editable descendants through native EditInline
	 * metadata even when its pointer is read-only and its class cannot be created. */
	UEAGENTINTERFACE_API bool IsExistingInlineObjectProperty(
		const FProperty* Property,
		const void* ValuePtr,
		const UObject* Owner);
	UEAGENTINTERFACE_API bool IsEditableProperty(
		const FProperty* Property,
		UObject* Owner = nullptr,
		FString* OutReason = nullptr,
		bool bShowDefaultsOnly = false, bool bUseInstanceEditability = false, const UStruct* Scope = nullptr);
	// Native toggle controls are editable declarations even without CPF_Edit.
	// The current EditCondition value controls the Details UI, not write permission.
	UEAGENTINTERFACE_API bool Build(
		const FProperty* Property,
		UObject* Owner,
		const void* ValuePtr,
		FDescriptor& OutDescriptor,
		FDiagnostic& OutError,
		bool bShowDefaultsOnly = false, bool bUseInstanceEditability = false, const UStruct* Scope = nullptr);
	/** Same native/type/editability owner; no unrequested display strings/flags. */
	bool BuildAuthoring(const FProperty* Property, UObject* Owner, const void* ValuePtr,
		FDescriptor& OutDescriptor, FDiagnostic& OutError,
		bool bShowDefaultsOnly, bool bUseInstanceEditability, const UStruct* Scope);
	/** Same descriptor producer, explicit pure declaration handoff for export.
	 * Build/BuildAuthoring retain their complete owning DTO defaults. */
	bool BuildAuthoringSharedDeclaration(const FProperty* Property, UObject* Owner, const void* ValuePtr,
		FDescriptor& OutDescriptor, FDiagnostic& OutError,
		bool bShowDefaultsOnly, bool bUseInstanceEditability, const UStruct* Scope);
}
