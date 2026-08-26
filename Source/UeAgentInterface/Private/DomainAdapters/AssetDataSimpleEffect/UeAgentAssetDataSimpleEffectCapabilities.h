#pragma once

#include "CoreMinimal.h"
#include "Core/Contract/UeAgentContract.h"
#include "Core/Contract/UeAgentDescriptorRegistry.h"
#include "Core/Discovery/UeAgentDiscovery.h"
#include "Core/PlanningPreflight/UeAgentPlanningPreflight.h"
#include "Core/StateSemantic/UeAgentStateSemantic.h"
#include "DomainAdapters/GenericEffect/UeAgentGenericEffectAdapter.h"
#include "Templates/SharedPointer.h"
#include "UObject/WeakObjectPtr.h"

class UClass;
class UObject;
class UPackage;

namespace UE::UAI::DomainAdapters::AssetDataSimpleEffect
{
	struct UEAGENTINTERFACE_API
		FUeAgentAssetDataSimpleEffectProductionField
	{
		FString FieldId;
		FString FieldBindingId;
		FString TypeId;
		StateSemantic::EUeAgentValueKind ValueKind =
			StateSemantic::EUeAgentValueKind::Null;
		TArray<FString> AllowedPresences;
		FString ReferencePolicy;

		bool IsValid() const
		{
			return !FieldId.IsEmpty()
				&& !FieldBindingId.IsEmpty()
				&& !TypeId.IsEmpty()
				&& ValueKind
					!= StateSemantic::EUeAgentValueKind::Null
				&& !AllowedPresences.IsEmpty()
				&& !ReferencePolicy.IsEmpty();
		}
	};

	struct UEAGENTINTERFACE_API
		FUeAgentAssetDataSimpleEffectProductionProfile
	{
		FString DescriptorId;
		int32 DescriptorVersion = 1;
		FString DescriptorHash;
		FString CapabilityId;
		FString ProfileId;
		FString NativeOperationType;
		FString NativeBindingId;
		FString NativeMechanicId;
		FString StaticCreateClassPath;
		FString FactoryContractId;
		FString ClassConstraintId;
		TArray<FString> FieldIds;
		TArray<FUeAgentAssetDataSimpleEffectProductionField> Fields;

		bool IsValid() const
		{
			return !DescriptorId.IsEmpty()
				&& DescriptorVersion > 0
				&& !DescriptorHash.IsEmpty()
				&& !CapabilityId.IsEmpty()
				&& !ProfileId.IsEmpty()
				&& !NativeOperationType.IsEmpty()
				&& !NativeBindingId.IsEmpty()
				&& !NativeMechanicId.IsEmpty();
		}

		bool IsCreate() const
		{
			return NativeOperationType
				== TEXT("asset.object.create");
		}

		bool IsClone() const
		{
			return NativeOperationType
				== TEXT("asset.object.clone");
		}

		bool UsesStaticCreateClass() const
		{
			return IsCreate()
				&& !StaticCreateClassPath.IsEmpty()
				&& FactoryContractId.IsEmpty()
				&& ClassConstraintId.IsEmpty();
		}

		bool UsesFactoryCandidate() const
		{
			return IsCreate()
				&& StaticCreateClassPath.IsEmpty()
				&& !FactoryContractId.IsEmpty()
				&& !ClassConstraintId.IsEmpty();
		}
	};

	class UEAGENTINTERFACE_API
		IUeAgentAssetDataSimpleEffectAssetResolver
	{
	public:
		virtual ~IUeAgentAssetDataSimpleEffectAssetResolver() = default;

		virtual bool TryResolveExisting(
			const FString& CanonicalRef,
			UObject*& OutObject,
			ExecutionMutation::FExecutionMutationError& OutError) = 0;
		virtual bool TryResolveCloneSource(
			const FString& CanonicalRef,
			UObject*& OutObject,
			ExecutionMutation::FExecutionMutationError& OutError)
		{
			return TryResolveExisting(CanonicalRef, OutObject, OutError);
		}
		virtual bool TryResolveCreationTarget(
			const FString& CanonicalRef,
			UPackage*& OutPackage,
			FName& OutObjectName,
			ExecutionMutation::FExecutionMutationError& OutError) = 0;
	};

	class UEAGENTINTERFACE_API
		FUeAgentAssetDataSimpleEffectFactoryBinding final
	{
	public:
		FUeAgentAssetDataSimpleEffectFactoryBinding() = default;
		FUeAgentAssetDataSimpleEffectFactoryBinding(
			Discovery::FUeAgentCandidateBinding InCandidateBinding,
			FString InProfileId,
			UClass* InConcreteClass,
			FString InFactoryMechanicId);

		const Discovery::FUeAgentCandidateBinding*
			GetCandidateBinding() const;
		const FString& GetProfileId() const;
		UClass* GetConcreteClass() const;
		const FString& GetFactoryMechanicId() const;
		bool IsValid() const;

	private:
		TSharedPtr<const Discovery::FUeAgentCandidateBinding>
			CandidateBinding;
		FString ProfileId;
		TWeakObjectPtr<UClass> ConcreteClass;
		FString FactoryMechanicId;
		bool bValid = false;
	};

	class UEAGENTINTERFACE_API
		IUeAgentAssetDataSimpleEffectFactoryCandidateResolver
	{
	public:
		virtual ~IUeAgentAssetDataSimpleEffectFactoryCandidateResolver() =
			default;

		virtual bool TryResolveForPlanning(
			const FString& FactoryRef,
			const Discovery::FUeAgentCatalogRevision& ExpectedRevision,
			const FString& ProfileId,
			FUeAgentAssetDataSimpleEffectFactoryBinding& OutBinding,
			ExecutionMutation::FExecutionMutationError& OutError) = 0;
		virtual bool TryResolveFrozenBinding(
			const FString& FactoryBindingId,
			const Discovery::FUeAgentCatalogRevision& ExpectedRevision,
			const FString& ProfileId,
			FUeAgentAssetDataSimpleEffectFactoryBinding& OutBinding,
			ExecutionMutation::FExecutionMutationError& OutError) = 0;
	};

	class UEAGENTINTERFACE_API
		IUeAgentAssetDataSimpleEffectNativeHook
	{
	public:
		virtual ~IUeAgentAssetDataSimpleEffectNativeHook() = default;

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
	TryCreateAssetDataSimpleEffectDescriptorRegistry(
		TSharedPtr<const Contract::FUeAgentDescriptorRegistry>&
			OutRegistry,
		FString& OutError);

	UEAGENTINTERFACE_API TArray<FString>
	GetAssetDataSimpleEffectAuthorityScopes();

	UEAGENTINTERFACE_API bool
	TryGetAssetDataSimpleEffectProductionProfiles(
		TArray<FUeAgentAssetDataSimpleEffectProductionProfile>&
			OutProfiles,
		FString& OutError);

	UEAGENTINTERFACE_API bool
	CanProjectAssetDataSimpleEffectFieldState(
		const FString& NativeMechanicId);

	UEAGENTINTERFACE_API bool
	TryProjectAssetDataSimpleEffectCurrentFieldState(
		IUeAgentAssetDataSimpleEffectAssetResolver& Resolver,
		const FString& NativeMechanicId,
		UObject* Target,
		const TArray<FString>& SelectedFieldIds,
		ContextResource::FUeAgentResourceTracker& Resources,
		StateSemantic::FUeAgentCanonicalValuePtr& OutCurrentFields,
		StateSemantic::FUeAgentCanonicalValuePtr& OutDefaultFields,
		StateSemantic::FUeAgentCanonicalValuePtr& OutProjectionContext,
		ExecutionMutation::FExecutionMutationError& OutError);

	UEAGENTINTERFACE_API bool
	TryProjectAssetDataSimpleEffectExpectedFieldState(
		IUeAgentAssetDataSimpleEffectAssetResolver& Resolver,
		const FString& NativeMechanicId,
		const StateSemantic::FUeAgentCanonicalValue& CurrentFields,
		const StateSemantic::FUeAgentCanonicalValue& DefaultFields,
		const StateSemantic::FUeAgentCanonicalValue& ProjectionContext,
		const StateSemantic::FUeAgentCanonicalValue& ProposedAfter,
		ContextResource::FUeAgentResourceTracker& Resources,
		StateSemantic::FUeAgentCanonicalValuePtr& OutExpectedFields,
		ExecutionMutation::FExecutionMutationError& OutError);

	UEAGENTINTERFACE_API bool
	TryBuildAssetDataSimpleEffectChangeSetDraft(
		const Contract::FUeAgentOperationRequest& Request,
		const ContextResource::FUeAgentExecutionContextRef& Context,
		const StateSemantic::FUeAgentCanonicalSnapshotRef& CurrentSnapshot,
		IUeAgentAssetDataSimpleEffectFactoryCandidateResolver&
			FactoryCandidates,
		ContextResource::FUeAgentResourceTracker& Resources,
		TOptional<PlanningPreflight::FChangeSetDraft>& OutDraft,
		ExecutionMutation::FExecutionMutationError& OutError);

	UEAGENTINTERFACE_API bool
	TryResolveAssetDataSimpleEffectNativeBinding(
		const FString& NativeBindingId,
		FString& OutNativeOperationType,
		FString& OutProfileId,
		FString& OutNativeMechanicId,
		FString& OutStaticCreateClassPath,
		FString& OutError);

	UEAGENTINTERFACE_API bool
	TryResolveAssetDataSimpleEffectFactoryContract(
		const FString& NativeBindingId,
		FString& OutFactoryContractId,
		FString& OutClassConstraintId,
		FString& OutError);

	UEAGENTINTERFACE_API bool
	TryValidateAssetDataSimpleEffectFactoryBinding(
		const FString& NativeBindingId,
		const FString& FactoryBindingId,
		const Discovery::FUeAgentCatalogRevision& ExpectedRevision,
		const FUeAgentAssetDataSimpleEffectFactoryBinding& Binding,
		FString& OutError);

	UEAGENTINTERFACE_API bool
	TryValidateAssetDataSimpleEffectFieldBinding(
		const FString& NativeBindingId,
		const FString& FieldBindingId,
		const FString& FieldId,
		const FString& Presence,
		const FString& TypeId,
		bool bHasValue,
		StateSemantic::EUeAgentValueKind ValueKind,
		FString& OutError);

	class UEAGENTINTERFACE_API
		FUeAgentAssetDataSimpleEffectNativeHookFactory final
	{
	public:
			static bool TryCreate(
				TSharedRef<
					IUeAgentAssetDataSimpleEffectAssetResolver,
					ESPMode::ThreadSafe> AssetResolver,
				TSharedRef<
					IUeAgentAssetDataSimpleEffectFactoryCandidateResolver,
					ESPMode::ThreadSafe> FactoryResolver,
				TSharedPtr<
					IUeAgentAssetDataSimpleEffectNativeHook>& OutHook,
			ExecutionMutation::FExecutionMutationError& OutError);
	};
}
