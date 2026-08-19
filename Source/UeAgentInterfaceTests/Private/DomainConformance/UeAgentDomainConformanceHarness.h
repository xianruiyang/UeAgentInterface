#pragma once

#include "CoreMinimal.h"
#include "Core/DomainCapability/UeAgentDomainCapability.h"
#include "Core/HjsonWorkspace/ImportInitialization/UeAgentHjsonDomainProviderRegistry.h"
#include "Dom/JsonObject.h"
#include "Templates/SharedPointer.h"

namespace UE::UAI::DomainConformance
{
	enum class EDomainConformanceLayer : uint8
	{
		StaticContract,
		DomainAdapter,
		PublicProduct,
	};

	enum class EDomainConformanceStatus : uint8
	{
		NotRun,
		Passed,
		Failed,
	};

	enum class EDomainConformanceScenario : uint8
	{
		CapabilityFreeze,
		ProcessAuthorityAdoption,
		CompositionSemanticRoutes,
		QueryReadOnlyCompleteness,
		CandidateDirectRead,
		InitializationContract,
		HjsonInline,
		HjsonSingleFile,
		HjsonDirectory,
		ExplicitIntentRevision,
		StaleRevisionReject,
		OperationRequestFactory,
		OperationStages,
		NoPrematureSuccess,
		PersistReloadReadback,
		FailureCleanup,
		LongRunningTerminal,
		TransientStateRecovery,
		GraphLayout,
		LegacyExit,
		PublicRoute,
	};

	struct FDomainConformanceFixture final
	{
		FString FixtureId;
		FString DomainId;
		FString TargetIdentity;
		FString BaseRevision;
		FString EntryFile;
		FString CanonicalDocument;
		FString WorkspaceRoot;
		FString MechanicsFamily;
		FString PublicRouteId;
		FString PublicAutomationTestId;
		bool bPublicRoute = false;
		bool bRealMechanics = false;
		bool bIndependentReadback = false;
		bool bPersistReload = false;
		bool bFailureCleanup = false;
		bool bLegacyExit = false;

		bool IsValid() const;
	};

	struct FDomainConformanceScenarioEntry final
	{
		EDomainConformanceScenario Scenario =
			EDomainConformanceScenario::CapabilityFreeze;
		EDomainConformanceLayer Layer =
			EDomainConformanceLayer::StaticContract;
		FString CapabilityId;
		FString MechanicsFamily;
	};

	class FDomainConformancePlan final
	{
	public:
		bool IsValid() const;
		bool Contains(EDomainConformanceScenario Scenario) const;
		const FString& GetDomainId() const;
		const FString& GetFixtureId() const;
		const TArray<FDomainConformanceScenarioEntry>& GetScenarios() const;
		void AddScenario(FDomainConformanceScenarioEntry Entry);

	private:
		friend bool TryBuildDomainConformancePlan(
			TSharedRef<const DomainCapability::
				FDomainCapabilityContribution> Contribution,
			const FDomainConformanceFixture& Fixture,
			FDomainConformancePlan& OutPlan,
			FString& OutError);

		FString DomainId;
		FString FixtureId;
		TArray<FDomainConformanceScenarioEntry> Scenarios;
		bool bValid = false;
	};

	class FNullHjsonDomainProvider final
		: public HjsonWorkspace::IImportInitializationDomainProvider
	{
	public:
		explicit FNullHjsonDomainProvider(FString InProviderId);

		const FString& GetProviderId() const override;
		bool Matches(
			const HjsonWorkspace::FImportInitializationDomainRequest& Request)
			const override;
		bool TryResolveInitializationContract(
			const HjsonWorkspace::FImportInitializationDomainRequest& Request,
			FString& OutInitializationContractId,
			HjsonWorkspace::FWorkspaceError& OutError) const override;
		bool TryExecute(
			const HjsonWorkspace::FImportInitializationDomainRequest& Request,
			const FString& InitializationContractId,
			HjsonWorkspace::FImportInitializationDomainResponse& OutResponse,
			HjsonWorkspace::FWorkspaceError& OutError) const override;

	private:
		FString ProviderId;
	};

	struct FDomainWorkspaceCarrier final
	{
		HjsonWorkspace::EWorkspaceSourceKind Kind =
			HjsonWorkspace::EWorkspaceSourceKind::Inline;
		FString CarrierId;
		FString Path;
		FString InlineDocument;

		TSharedPtr<FJsonObject> MakePublicSourceObject() const;
		bool IsValid() const;
	};

	class FDomainWorkspaceFixture final
	{
	public:
		bool IsValid() const;
		const FString& GetFixtureId() const;
		const FString& GetTargetIdentity() const;
		const FString& GetCanonicalSemanticHash() const;
		const TArray<FDomainWorkspaceCarrier>& GetCarriers() const;
		bool TryOverwriteSingleFileForTest(
			const FString& Document,
			FString& OutError);

	private:
		friend bool TryBuildEquivalentWorkspaceFixture(
			const FDomainConformanceFixture& Fixture,
			FDomainWorkspaceFixture& OutWorkspace,
			FString& OutError);
		friend bool ValidateEquivalentWorkspaceFixture(
			const FDomainWorkspaceFixture& Workspace,
			FString& OutError);

		FString FixtureId;
		FString TargetIdentity;
		FString CanonicalSemanticHash;
		TArray<FDomainWorkspaceCarrier> Carriers;
		bool bValid = false;
	};

	class FDomainConformanceLayerResults final
	{
	public:
		void Record(
			EDomainConformanceLayer Layer,
			EDomainConformanceStatus Status,
			FString EvidenceRef);
		EDomainConformanceStatus GetStatus(
			EDomainConformanceLayer Layer) const;
		const FString& GetEvidenceRef(
			EDomainConformanceLayer Layer) const;

	private:
		struct FEntry final
		{
			EDomainConformanceStatus Status =
				EDomainConformanceStatus::NotRun;
			FString EvidenceRef;
		};

		TMap<EDomainConformanceLayer, FEntry> Entries;
	};

	struct FDomainOwnedWorkerBatch final
	{
		FString BatchId;
		FString MechanicsFamily;
		TArray<FString> TestIds;
		bool bRequiresHeadless = true;
		bool bPreserveMainEditor = true;
		FString CleanupPolicy;

		bool IsValid() const;
	};

	struct FDomainProductEvidence final
	{
		FString RouteId;
		FString ReceiptRef;
		bool bReceiptExists = false;
		bool bSynthetic = false;
		bool bPublicRouteObserved = false;
		bool bIndependentReadbackObserved = false;
		bool bCleanupObserved = false;
	};

	bool TryBuildDomainConformancePlan(
		TSharedRef<const DomainCapability::FDomainCapabilityContribution>
			Contribution,
		const FDomainConformanceFixture& Fixture,
		FDomainConformancePlan& OutPlan,
		FString& OutError);

	bool ValidateRequestScopedOperationFactory(
		TSharedRef<const DomainCapability::FDomainCapabilityContribution>
			Contribution,
		const FString& OperationId,
		const CommandEdge::FUeAgentCommandRequest& FirstRequest,
		const CommandEdge::FUeAgentCommandRequest& SecondRequest,
		const Contract::FUeAgentCallerContext& CallerContext,
		const TSharedRef<Contract::IUeAgentCancellationSignal>& Cancellation,
		FString& OutError);

	bool TryBuildEquivalentWorkspaceFixture(
		const FDomainConformanceFixture& Fixture,
		FDomainWorkspaceFixture& OutWorkspace,
		FString& OutError);

	bool ValidateEquivalentWorkspaceFixture(
		const FDomainWorkspaceFixture& Workspace,
		FString& OutError);

	bool TryBuildOwnedWorkerBatch(
		const FDomainConformancePlan& Plan,
		const FDomainConformanceFixture& Fixture,
		const FDomainConformanceLayerResults& Results,
		FDomainOwnedWorkerBatch& OutBatch,
		FString& OutError);

	bool ValidateProductEvidence(
		const FDomainConformanceFixture& Fixture,
		const FDomainProductEvidence& Evidence,
		FString& OutError);
}
