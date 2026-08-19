#pragma once

#include "CoreMinimal.h"
#include "CommandEdge/UeAgentExactCommandDispatch.h"
#include "CommandRegistry/UeAgentImmutableCommandRegistry.h"
#include "Core/BindingGateway/Operation/UeAgentOperationLifecycleService.h"
#include "Core/BindingGateway/Query/UeAgentDomainQuery.h"
#include "Core/Discovery/UeAgentDiscovery.h"
#include "Core/HjsonWorkspace/ImportInitialization/UeAgentHjsonDomainProviderRegistry.h"
#include "Core/HjsonWorkspace/UeAgentAuthoringComposition.h"
#include "Core/StateSemantic/UeAgentSemanticSurface.h"
#include "Module/CommandRegistration/Active/UeAgentActiveCommandComposition.h"
#include "Templates/SharedPointer.h"

namespace UE::UAI::DomainCapability
{
	enum class EDomainCapabilitySupport : uint8
	{
		Undeclared,
		Unsupported,
		Supported,
	};

	enum class EDomainOperationKind : uint8
	{
		Create,
		Write,
		Delete,
	};

	enum class EDomainOperationTerminalPolicy : uint8
	{
		Undeclared,
		SucceededRejectedRolledBackOrStateUnknown,
	};

	enum class EDomainOperationExecutionPolicy : uint8
	{
		Undeclared,
		Immediate,
		LongRunning,
	};

	enum class EDomainTransientStatePolicy : uint8
	{
		Undeclared,
		None,
		RecoverableSession,
	};

	class UEAGENTINTERFACE_API FDomainOperationInvocationError final
	{
	public:
		FDomainOperationInvocationError() = default;
		FDomainOperationInvocationError(
			FString InCode,
			FString InSubjectId = FString(),
			FString InDetail = FString());

		const FString& GetCode() const;
		const FString& GetSubjectId() const;
		const FString& GetDetail() const;
		bool IsPresent() const;

	private:
		FString Code;
		FString SubjectId;
		FString Detail;
	};

	class UEAGENTINTERFACE_API IDomainOperationInvocationContext
	{
	public:
		virtual ~IDomainOperationInvocationContext() = default;

		virtual bool TryBuildSuccessPayload(
			const BindingGateway::Operation::FOperationLifecycleReceipt& Receipt,
			FString& OutPayloadJson,
			FString& OutError) const = 0;

		virtual FString GetFailureDetail() const
		{
			return FString();
		}
	};

	class UEAGENTINTERFACE_API FDomainOperationInvocation final
	{
	public:
		FDomainOperationInvocation() = default;

		static bool TryCreate(
			TSharedRef<const BindingGateway::Operation::FOperationLifecyclePlan>
				Plan,
			TSharedRef<const BindingGateway::Operation::
				FOperationImpactAuthorization> Authorization,
			TSharedRef<BindingGateway::Operation::FOperationLifecycleService>
				Service,
			TSharedRef<IDomainOperationInvocationContext> Context,
			FDomainOperationInvocation& OutInvocation,
			FDomainOperationInvocationError& OutError);

		bool IsValid() const;
		TSharedRef<const BindingGateway::Operation::FOperationLifecyclePlan>
			GetPlan() const;
		TSharedRef<const BindingGateway::Operation::
			FOperationImpactAuthorization> GetAuthorization() const;
		TSharedRef<BindingGateway::Operation::FOperationLifecycleService>
			GetService() const;
		TSharedRef<IDomainOperationInvocationContext> GetContext() const;

	private:
		TSharedPtr<const BindingGateway::Operation::FOperationLifecyclePlan>
			Plan;
		TSharedPtr<const BindingGateway::Operation::
			FOperationImpactAuthorization> Authorization;
		TSharedPtr<BindingGateway::Operation::FOperationLifecycleService>
			Service;
		TSharedPtr<IDomainOperationInvocationContext> Context;
	};

	class UEAGENTINTERFACE_API IDomainOperationInvocationFactory
	{
	public:
		virtual ~IDomainOperationInvocationFactory() = default;

		virtual const FString& GetOperationId() const = 0;
		virtual bool TryCreate(
			const CommandEdge::FUeAgentCommandRequest& Request,
			const Contract::FUeAgentCallerContext& CallerContext,
			const TSharedRef<Contract::IUeAgentCancellationSignal>&
				Cancellation,
			FDomainOperationInvocation& OutInvocation,
			FDomainOperationInvocationError& OutError) const = 0;
	};

	struct UEAGENTINTERFACE_API FDomainCommandCapability
	{
		EDomainCapabilitySupport Support =
			EDomainCapabilitySupport::Undeclared;
		TArray<TSharedRef<Module::CommandRegistration::Active::
			IUeAgentActiveCommandContribution>> Contributions;
	};

	struct UEAGENTINTERFACE_API FDomainQueryCapability
	{
		EDomainCapabilitySupport Support =
			EDomainCapabilitySupport::Undeclared;
		TArray<BindingGateway::Query::
			FUeAgentDomainQueryProviderRegistration> Providers;
	};

	struct UEAGENTINTERFACE_API FDomainDiscoverySource
	{
		FDomainDiscoverySource(
			TSharedRef<Discovery::IUeAgentCatalogSource> InSource,
			FString InReadPortId);

		TSharedRef<Discovery::IUeAgentCatalogSource> Source;
		FString ReadPortId;
	};

	struct UEAGENTINTERFACE_API FDomainDiscoveryCapability
	{
		EDomainCapabilitySupport Support =
			EDomainCapabilitySupport::Undeclared;
		TArray<FDomainDiscoverySource> Sources;
		TArray<TSharedRef<Discovery::IUeAgentStateReadPort>> ReadPorts;
	};

	struct UEAGENTINTERFACE_API FDomainHjsonRoute
	{
		FString ProviderId;
		FString CommandId;
		FString SchemaId;
		FString TargetKind;
		FString RootTypeIdentity;
		HjsonWorkspace::EInitializationNodeRole Role =
			HjsonWorkspace::EInitializationNodeRole::Value;
		FString InitializationContractId;
		bool bCreate = false;
	};

	struct UEAGENTINTERFACE_API FDomainHjsonCapability
	{
		EDomainCapabilitySupport Support =
			EDomainCapabilitySupport::Undeclared;
		TArray<TSharedRef<
			HjsonWorkspace::IImportInitializationDomainProvider>> Providers;
		TArray<FDomainHjsonRoute> Routes;
	};

	struct UEAGENTINTERFACE_API FDomainCompositionCapability
	{
		EDomainCapabilitySupport Support =
			EDomainCapabilitySupport::Undeclared;
		TArray<HjsonWorkspace::FTargetCompositionContract> Contracts;
	};

	struct UEAGENTINTERFACE_API FDomainInitializationCapability
	{
		EDomainCapabilitySupport Support =
			EDomainCapabilitySupport::Undeclared;
		TArray<FString> ContractIds;
	};

	struct UEAGENTINTERFACE_API FDomainOperationEntry
	{
		FString OperationId;
		EDomainOperationKind Kind = EDomainOperationKind::Write;
		EDomainOperationTerminalPolicy TerminalPolicy =
			EDomainOperationTerminalPolicy::Undeclared;
		EDomainOperationExecutionPolicy ExecutionPolicy =
			EDomainOperationExecutionPolicy::Undeclared;
		EDomainTransientStatePolicy TransientStatePolicy =
			EDomainTransientStatePolicy::Undeclared;
		bool bPersists = false;
		bool bIndependentReadback = false;
		TSharedPtr<const IDomainOperationInvocationFactory> InvocationFactory;
	};

	struct UEAGENTINTERFACE_API FDomainOperationCapability
	{
		EDomainCapabilitySupport Support =
			EDomainCapabilitySupport::Undeclared;
		TArray<FDomainOperationEntry> Operations;
	};

	struct UEAGENTINTERFACE_API FDomainLayoutEntry
	{
		FString ProviderId;
		FString GraphIdentity;
		FString MechanicsId;
		FString ReadPortId;
		bool bPersists = false;
		bool bIndependentReadback = false;
	};

	struct UEAGENTINTERFACE_API FDomainLayoutCapability
	{
		EDomainCapabilitySupport Support =
			EDomainCapabilitySupport::Undeclared;
		TArray<FDomainLayoutEntry> Layouts;
	};

	struct UEAGENTINTERFACE_API FDomainStateCapability
	{
		EDomainCapabilitySupport Support =
			EDomainCapabilitySupport::Undeclared;
		TArray<StateSemantic::EUeAgentStatePlane> Planes;
	};

	struct UEAGENTINTERFACE_API FDomainSemanticSurfaceEntry
	{
		FString SurfaceId;
		StateSemantic::EUeAgentSemanticSurfaceKind Kind =
			StateSemantic::EUeAgentSemanticSurfaceKind::PropertyCollection;
		FString MechanicsId;
		FString ReadPortId;
	};

	struct UEAGENTINTERFACE_API FDomainSemanticSurfaceCapability
	{
		EDomainCapabilitySupport Support =
			EDomainCapabilitySupport::Undeclared;
		TArray<FDomainSemanticSurfaceEntry> Surfaces;
	};

	struct UEAGENTINTERFACE_API FDomainMigrationCapability
	{
		EDomainCapabilitySupport Support =
			EDomainCapabilitySupport::Undeclared;
		FString OwnerId;
	};

	struct UEAGENTINTERFACE_API FDomainCapabilityDeclaration
	{
		FString DomainId;
		FDomainCommandCapability Command;
		FDomainQueryCapability Query;
		FDomainDiscoveryCapability Discovery;
		FDomainHjsonCapability Hjson;
		FDomainCompositionCapability Composition;
		FDomainInitializationCapability Initialization;
		FDomainOperationCapability Operation;
		FDomainLayoutCapability Layout;
		FDomainStateCapability State;
		FDomainSemanticSurfaceCapability Surface;
		FDomainMigrationCapability Migration;
	};

	UEAGENTINTERFACE_API FDomainCapabilityDeclaration
	MakeExplicitlyUnsupportedDomainCapabilityDeclaration(
		FString DomainId);

	class UEAGENTINTERFACE_API FDomainCapabilityContribution final
	{
	public:
		static bool TryCreate(
			FDomainCapabilityDeclaration Declaration,
			TSharedPtr<const FDomainCapabilityContribution>& OutContribution,
			FString& OutError);

		const FString& GetDomainId() const;
		const FDomainCapabilityDeclaration& GetDeclaration() const;

	private:
		explicit FDomainCapabilityContribution(
			FDomainCapabilityDeclaration InDeclaration);

		FDomainCapabilityDeclaration Declaration;
	};

	class UEAGENTINTERFACE_API FDomainCapabilityAuthorities final
	{
	public:
		const TSharedPtr<const CommandRegistry::
			FUeAgentImmutableCommandRegistry>& GetCommandRegistry() const;
		const TSharedPtr<const CommandEdge::FUeAgentExactCommandDispatch>&
			GetCommandDispatch() const;
		const TSharedPtr<const HjsonWorkspace::
			FImportInitializationDomainProviderRegistry>&
			GetHjsonRegistry() const;

	private:
		friend class FFrozenDomainCapabilitySet;

		TSharedPtr<const CommandRegistry::FUeAgentImmutableCommandRegistry>
			CommandRegistry;
		TSharedPtr<const CommandEdge::FUeAgentExactCommandDispatch>
			CommandDispatch;
		TSharedPtr<const HjsonWorkspace::
			FImportInitializationDomainProviderRegistry> HjsonRegistry;
	};

	class UEAGENTINTERFACE_API FFrozenDomainCapabilitySet final
	{
	public:
		int32 Num() const;
		const TArray<TSharedRef<const FDomainCapabilityContribution>>&
			GetContributions() const;
		TSharedPtr<const IDomainOperationInvocationFactory>
			FindOperationFactory(const FString& OperationId) const;
		bool TryBuildAuthorities(
			FDomainCapabilityAuthorities& OutAuthorities,
			FString& OutError) const;

	private:
		friend class FDomainCapabilityComposition;
		explicit FFrozenDomainCapabilitySet(
			TArray<TSharedRef<const FDomainCapabilityContribution>>
				InContributions);

		TArray<TSharedRef<const FDomainCapabilityContribution>>
			Contributions;
	};

	class UEAGENTINTERFACE_API FDomainCapabilityComposition final
	{
	public:
		bool TryAdd(
			TSharedRef<const FDomainCapabilityContribution> Contribution,
			FString& OutError);
		bool TryFreeze(
			TSharedPtr<const FFrozenDomainCapabilitySet>& OutFrozen,
			FString& OutError);
		bool IsSealed() const;

	private:
		bool bSealed = false;
		TArray<TSharedRef<const FDomainCapabilityContribution>>
			Contributions;
		TSet<FString> DomainIds;
	};

	using FDomainCapabilityContributionFactory = bool (*)(
		TSharedPtr<const FDomainCapabilityContribution>& OutContribution,
		FString& OutError);

	class UEAGENTINTERFACE_API
		FDomainCapabilityContributionFactoryRegistration final
	{
	public:
		FDomainCapabilityContributionFactoryRegistration(
			const TCHAR* DomainId,
			FDomainCapabilityContributionFactory Factory);
	};

	UEAGENTINTERFACE_API bool TryFreezeProcessDomainCapabilities(
		TSharedPtr<const FFrozenDomainCapabilitySet>& OutFrozen,
		FString& OutError);
}
