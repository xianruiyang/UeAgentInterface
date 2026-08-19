#pragma once

#include "CoreMinimal.h"
#include "Core/Contract/UeAgentContract.h"
#include "Core/Contract/UeAgentDescriptorRegistry.h"
#include "Core/ExecutionMutation/UeAgentExecutionMutation.h"
#include "Core/PlanningPreflight/UeAgentPlanningPreflight.h"
#include "Core/StateSemantic/UeAgentStateSemantic.h"
#include "DomainAdapters/GenericEffect/UeAgentGenericEffectAdapter.h"
#include "Templates/SharedPointer.h"

class FConfigFile;
class FProperty;
class UObject;
class UStringTable;

namespace UE::UAI::DomainAdapters::EditorProjectSimpleEffect
{
	struct UEAGENTINTERFACE_API
		FUeAgentEditorProjectSimpleEffectProductionProfile
	{
		FString DescriptorId;
		int32 DescriptorVersion = 1;
		FString DescriptorHash;
		FString CapabilityId;
		FString ProfileId;
		FString NativeOperationType;
		FString NativeBindingId;
		FString NativeMechanicId;
		TArray<FString> FieldIds;

		bool IsValid() const
		{
			return !DescriptorId.IsEmpty()
				&& DescriptorVersion > 0
				&& !DescriptorHash.IsEmpty()
				&& !CapabilityId.IsEmpty()
				&& !ProfileId.IsEmpty()
				&& !NativeOperationType.IsEmpty()
				&& !NativeBindingId.IsEmpty()
				&& !NativeMechanicId.IsEmpty()
				&& !FieldIds.IsEmpty();
		}
	};

	struct UEAGENTINTERFACE_API
		FUeAgentEditorProjectSettingsFieldObservation
	{
		FString FieldRef;
		FString FieldRevision;
		FString TypeId;
		FString CanonicalValue;

		bool IsValid() const
		{
			return !FieldRef.IsEmpty()
				&& !FieldRevision.IsEmpty()
				&& !TypeId.IsEmpty();
		}
	};

	class UEAGENTINTERFACE_API
		FUeAgentEditorProjectSimpleEffectResolvedTarget final
	{
	public:
		FUeAgentEditorProjectSimpleEffectResolvedTarget() = default;
		FUeAgentEditorProjectSimpleEffectResolvedTarget(
			FString InCanonicalRef,
			FString InProfileId,
			FString InRevision,
			FConfigFile* InConfigDocument,
			FString InConfigSection,
			UObject* InSettingsObject,
			UStringTable* InStringTable,
			TSet<FString>* InLocalizationDirectories);

		const FString& GetCanonicalRef() const;
		const FString& GetProfileId() const;
		const FString& GetRevision() const;
		FConfigFile* GetConfigDocument() const;
		const FString& GetConfigSection() const;
		UObject* GetSettingsObject() const;
		UStringTable* GetStringTable() const;
		TSet<FString>* GetLocalizationDirectories() const;
		bool IsValid() const;

	private:
		FString CanonicalRef;
		FString ProfileId;
		FString Revision;
		FConfigFile* ConfigDocument = nullptr;
		FString ConfigSection;
		TWeakObjectPtr<UObject> SettingsObject;
		TWeakObjectPtr<UStringTable> StringTable;
		TSet<FString>* LocalizationDirectories = nullptr;
		bool bValid = false;
	};

	class UEAGENTINTERFACE_API
		IUeAgentEditorProjectSimpleEffectTargetResolver
	{
	public:
		virtual ~IUeAgentEditorProjectSimpleEffectTargetResolver() =
			default;

		virtual bool TryResolveTarget(
			const FString& CanonicalRef,
			FUeAgentEditorProjectSimpleEffectResolvedTarget& OutTarget,
			ExecutionMutation::FExecutionMutationError& OutError) = 0;

		virtual bool TryResolveSettingsField(
			UObject& SettingsObject,
			const FString& FieldRef,
			const FString& ExpectedRevision,
			FProperty*& OutProperty,
			FString& OutCurrentRevision,
			ExecutionMutation::FExecutionMutationError& OutError) = 0;

		virtual bool TryReadSettingsFields(
			UObject& SettingsObject,
			TArray<FUeAgentEditorProjectSettingsFieldObservation>& OutFields,
			ExecutionMutation::FExecutionMutationError& OutError)
		{
			OutFields.Reset();
			OutError = ExecutionMutation::FExecutionMutationError();
			return false;
		}
	};

	class UEAGENTINTERFACE_API
		IUeAgentEditorProjectSimpleEffectNativeHook
	{
	public:
		virtual ~IUeAgentEditorProjectSimpleEffectNativeHook() = default;

		virtual bool TryApply(
			const ExecutionMutation::FExecutionMutationRequest& Request,
			const ExecutionMutation::FLockFenceLease& Lease,
			const TArray<
				GenericEffect::FUeAgentGenericEffectBinding>& Bindings,
			ContextResource::FUeAgentResourceTracker& Resources,
			GenericEffect::FUeAgentGenericEffectApplyResult& OutResult,
			ExecutionMutation::FExecutionMutationError& OutError) = 0;

		virtual bool TryRollback(
			const ExecutionMutation::FExecutionMutationRequest& Request,
			const GenericEffect::FUeAgentGenericEffectApplyResult& Applied,
			ContextResource::FUeAgentResourceTracker& Resources,
			bool& bOutConfirmed,
			ExecutionMutation::FExecutionMutationError& OutError) = 0;

		virtual void ReleaseVerifiedOperation(
			const FString& OperationId) = 0;

		virtual bool TryWaitForSettle(
			const ExecutionMutation::FBarrierRequest& Request,
			ContextResource::FUeAgentResourceTracker& Resources,
			ExecutionMutation::FBarrierResult& OutResult,
			ExecutionMutation::FExecutionMutationError& OutError) = 0;
	};

	UEAGENTINTERFACE_API bool
	TryCreateEditorProjectSimpleEffectDescriptorRegistry(
		TSharedPtr<const Contract::FUeAgentDescriptorRegistry>& OutRegistry,
		FString& OutError);

	UEAGENTINTERFACE_API TArray<FString>
	GetEditorProjectSimpleEffectAuthorityScopes(const FString& ProfileId);

	UEAGENTINTERFACE_API bool
	TryGetEditorProjectSimpleEffectProductionProfiles(
		TArray<FUeAgentEditorProjectSimpleEffectProductionProfile>&
			OutProfiles,
		FString& OutError);

	UEAGENTINTERFACE_API bool
	TryBuildEditorProjectSimpleEffectChangeSetDraft(
		const Contract::FUeAgentOperationRequest& Request,
		const ContextResource::FUeAgentExecutionContextRef& Context,
		const StateSemantic::FUeAgentCanonicalSnapshotRef& CurrentSnapshot,
		ContextResource::FUeAgentResourceTracker& Resources,
		TOptional<PlanningPreflight::FChangeSetDraft>& OutDraft,
		ExecutionMutation::FExecutionMutationError& OutError);

	UEAGENTINTERFACE_API bool
	TryResolveEditorProjectSimpleEffectNativeBinding(
		const FString& NativeBindingId,
		FString& OutNativeOperationType,
		FString& OutProfileId,
		FString& OutNativeMechanicId,
		FString& OutError);

	UEAGENTINTERFACE_API bool
	TryResolveEditorProjectSimpleEffectFieldContract(
		const FString& NativeBindingId,
		const FString& FieldId,
		FString& OutFieldBindingId,
		FString& OutTypeId,
		StateSemantic::EUeAgentValueKind& OutValueKind,
		TArray<FString>& OutAllowedPresences,
		FString& OutError);

	class UEAGENTINTERFACE_API
		FUeAgentEditorProjectSimpleEffectNativeHookFactory final
	{
	public:
		static bool TryCreate(
			TSharedRef<
				IUeAgentEditorProjectSimpleEffectTargetResolver,
				ESPMode::ThreadSafe> TargetResolver,
			TSharedPtr<
				IUeAgentEditorProjectSimpleEffectNativeHook>& OutHook,
			ExecutionMutation::FExecutionMutationError& OutError);
	};
}
