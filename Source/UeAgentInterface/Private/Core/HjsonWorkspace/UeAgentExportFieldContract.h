#pragma once

#include "CoreMinimal.h"
#include "Core/HjsonWorkspace/UeAgentHjsonWorkspace.h"
#include "Core/StateSemantic/UeAgentStateSemantic.h"
#include "Templates/SharedPointer.h"

namespace UE::UAI::HjsonWorkspace
{
	using UE::UAI::ContextResource::FUeAgentResourceTracker;
	using UE::UAI::StateSemantic::EUeAgentDefaultSourceKind;
	using UE::UAI::StateSemantic::FUeAgentCanonicalSnapshotRef;
	using UE::UAI::StateSemantic::FUeAgentCanonicalValueRef;

	enum class EExportFieldEmissionPolicy : uint8
	{
		Always,
		WhenPresent,
		WhenAuthored,
		WhenOverridden,
		WhenNonDefault,
		OnSelection,
	};

	enum class EExportFieldOwnerSource : uint8
	{
		Handwritten,
		Reflection,
	};

	class UEAGENTINTERFACE_API FExportFieldContract final
	{
	public:
		FExportFieldContract(
			FString InContractId,
			int32 InContractVersion,
			FString InFieldId,
			FString InExternalName,
			FString InOwnerSemanticId,
			FString InValueContractRef,
			FString InSemanticRole,
			FString InRoundTripRequirement,
			TArray<FString> InProfileVisibility,
			bool bInPresenceSensitive,
			EExportFieldEmissionPolicy InEmissionPolicy,
			EUeAgentDefaultSourceKind InDefaultSource,
			bool bInDefaultReproducible,
			FString InFieldSetId,
			FString InExposureReason,
			FString InDisplayName,
			FString InTooltip,
			FString InValueKind);

		const FString& GetContractId() const;
		int32 GetContractVersion() const;
		const FString& GetFieldId() const;
		const FString& GetExternalName() const;
		const FString& GetOwnerSemanticId() const;
		const FString& GetValueContractRef() const;
		const FString& GetSemanticRole() const;
		const FString& GetRoundTripRequirement() const;
		const TArray<FString>& GetProfileVisibility() const;
		bool IsPresenceSensitive() const;
		EExportFieldEmissionPolicy GetEmissionPolicy() const;
		EUeAgentDefaultSourceKind GetDefaultSource() const;
		bool IsDefaultReproducible() const;
		const FString& GetFieldSetId() const;
		const FString& GetExposureReason() const;
		const FString& GetDisplayName() const;
		const FString& GetTooltip() const;
		const FString& GetValueKind() const;
		bool IsValid() const;

	private:
		FString ContractId;
		int32 ContractVersion = 0;
		FString FieldId;
		FString ExternalName;
		FString OwnerSemanticId;
		FString ValueContractRef;
		FString SemanticRole;
		FString RoundTripRequirement;
		TArray<FString> ProfileVisibility;
		bool bPresenceSensitive = false;
		EExportFieldEmissionPolicy EmissionPolicy =
			EExportFieldEmissionPolicy::OnSelection;
		EUeAgentDefaultSourceKind DefaultSource =
			EUeAgentDefaultSourceKind::Factory;
		bool bDefaultReproducible = false;
		FString FieldSetId;
		FString ExposureReason;
		FString DisplayName;
		FString Tooltip;
		FString ValueKind;
		bool bValid = false;
	};

	class UEAGENTINTERFACE_API FExportFieldRegistration final
	{
	public:
		FExportFieldRegistration(
			FExportFieldContract InField,
			EExportFieldOwnerSource InProviderSource,
			FString InProviderId);

		const FExportFieldContract& GetField() const;
		EExportFieldOwnerSource GetProviderSource() const;
		const FString& GetProviderId() const;
		bool IsValid() const;

	private:
		FExportFieldContract Field;
		EExportFieldOwnerSource ProviderSource =
			EExportFieldOwnerSource::Handwritten;
		FString ProviderId;
		bool bValid = false;
	};

	class UEAGENTINTERFACE_API FExportFieldRegistry final
	{
	public:
		FExportFieldRegistry(
			FString InContractId,
			int32 InContractVersion,
			TArray<FExportFieldRegistration> InRegistrations);

		const FString& GetContractId() const;
		int32 GetContractVersion() const;
		const TArray<FExportFieldRegistration>& GetRegistrations() const;
		const FExportFieldRegistration* FindField(
			const FString& FieldId) const;
		const FExportFieldRegistration* FindFieldByExternalName(
			const FString& OwnerSemanticId,
			const FString& FieldSetId,
			const FString& ExternalName) const;
		bool IsValid() const;

	private:
		FString ContractId;
		int32 ContractVersion = 0;
		TArray<FExportFieldRegistration> Registrations;
		TMap<FString, int32> IndexByFieldId;
		bool bValid = false;
	};

	using FExportFieldRegistryPtr =
		TSharedPtr<const FExportFieldRegistry>;
	using FExportFieldRegistryRef =
		TSharedRef<const FExportFieldRegistry>;

	class UEAGENTINTERFACE_API FExportFieldDescription final
	{
	public:
		FExportFieldDescription() = default;
		explicit FExportFieldDescription(
			const FExportFieldRegistration& Registration);

		const FString& GetFieldId() const;
		const FString& GetExternalName() const;
		const FString& GetValueContractRef() const;
		const FString& GetDisplayName() const;
		const FString& GetTooltip() const;
		const FString& GetValueKind() const;
		EExportFieldOwnerSource GetProviderSource() const;
		const FString& GetProviderId() const;
		bool IsValid() const;

	private:
		FString FieldId;
		FString ExternalName;
		FString ValueContractRef;
		FString DisplayName;
		FString Tooltip;
		FString ValueKind;
		EExportFieldOwnerSource ProviderSource =
			EExportFieldOwnerSource::Handwritten;
		FString ProviderId;
		bool bValid = false;
	};

	class UEAGENTINTERFACE_API FPropertySetProjection final
	{
	public:
		FPropertySetProjection(
			FString InOwnerSemanticId,
			FString InFieldSetId,
			TMap<FString, FUeAgentCanonicalValueRef> InValues,
			TArray<FString> InEmittedFieldIds);

		const FString& GetOwnerSemanticId() const;
		const FString& GetFieldSetId() const;
		const TMap<FString, FUeAgentCanonicalValueRef>& GetValues() const;
		const TArray<FString>& GetEmittedFieldIds() const;
		const TMap<FString, FExportFieldDescription>&
			GetDescriptions() const;
		bool IsValid() const;

	private:
		FString OwnerSemanticId;
		FString FieldSetId;
		TMap<FString, FUeAgentCanonicalValueRef> Values;
		TArray<FString> EmittedFieldIds;
		TMap<FString, FExportFieldDescription> Descriptions;
		bool bValid = false;
	};

	using FPropertySetProjectionPtr =
		TSharedPtr<const FPropertySetProjection>;

	UEAGENTINTERFACE_API bool TryCompileExportFieldRegistry(
		const TArray<FExportFieldRegistration>& Registrations,
		const FString& ContractId,
		int32 ContractVersion,
		FExportFieldRegistryPtr& OutRegistry,
		FWorkspaceError& OutError);

	UEAGENTINTERFACE_API bool TryProjectPropertySet(
		const FExportFieldRegistryRef& Registry,
		const FUeAgentCanonicalSnapshotRef& Snapshot,
		const FString& OwnerSemanticId,
		const FString& FieldSetId,
		FUeAgentResourceTracker& Resources,
		FPropertySetProjectionPtr& OutProjection,
		FWorkspaceError& OutError);

	UEAGENTINTERFACE_API bool TryProjectPropertySet(
		const FExportFieldRegistryRef& Registry,
		const FUeAgentCanonicalSnapshotRef& Snapshot,
		const FString& StateOwnerId,
		const FString& OwnerSemanticId,
		const FString& FieldSetId,
		FUeAgentResourceTracker& Resources,
		FPropertySetProjectionPtr& OutProjection,
		FWorkspaceError& OutError);

	UEAGENTINTERFACE_API bool TryDescribeExportField(
		const FExportFieldRegistryRef& Registry,
		const FString& FieldId,
		FExportFieldDescription& OutDescription,
		FWorkspaceError& OutError);
}
