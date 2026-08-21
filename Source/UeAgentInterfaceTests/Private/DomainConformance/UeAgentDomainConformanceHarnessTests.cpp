#include "Misc/AutomationTest.h"

#include "Core/DomainCapability/UeAgentDomainCapability.h"
#include "Core/HjsonWorkspace/SourceLoader/UeAgentHjsonSourceLoader.h"
#include "DomainConformance/UeAgentDomainConformanceHarness.h"
#include "HAL/FileManager.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"

namespace
{
	using namespace UE::UAI;
	using namespace UE::UAI::DomainCapability;
	using namespace UE::UAI::DomainConformance;

	ContextResource::FUeAgentResourceTracker MakeResources()
	{
		return ContextResource::FUeAgentResourceTracker(
			ContextResource::FUeAgentResourceBudget({
				{ContextResource::EUeAgentResourceDimension::InputBytes,
					1024 * 1024},
				{ContextResource::EUeAgentResourceDimension::OutputBytes,
					1024 * 1024},
				{ContextResource::EUeAgentResourceDimension::StringBytes,
					1024 * 1024},
				{ContextResource::EUeAgentResourceDimension::Nodes, 4096},
				{ContextResource::EUeAgentResourceDimension::Items, 4096},
				{ContextResource::EUeAgentResourceDimension::ReferenceEdges,
					4096},
				{ContextResource::EUeAgentResourceDimension::WorkUnits, 32768},
				{ContextResource::EUeAgentResourceDimension::EstimatedMemoryBytes,
					8 * 1024 * 1024},
			}));
	}

	FDomainConformanceFixture MakeHjsonFixture(const FString& Root)
	{
		FDomainConformanceFixture Fixture;
		Fixture.FixtureId = TEXT("fixture:t66:hjson");
		Fixture.DomainId = TEXT("fixture-hjson-domain");
		Fixture.TargetIdentity =
			TEXT("/Game/UAI/T66/DA_Conformance.DA_Conformance");
		Fixture.BaseRevision = TEXT("absent");
		Fixture.EntryFile = TEXT("asset.hjson");
		Fixture.CanonicalDocument =
			TEXT("{\n")
			TEXT("  schema: uai.fixture.domain/1\n")
			TEXT("  target_kind: fixture_target\n")
			TEXT("  target_identity: /Game/UAI/T66/DA_Conformance.DA_Conformance\n")
			TEXT("  base_revision: absent\n")
			TEXT("  values: {\n")
			TEXT("    label: Conformance\n")
			TEXT("    count: 2\n")
			TEXT("  }\n")
			TEXT("}\n");
		Fixture.WorkspaceRoot = Root;
		Fixture.MechanicsFamily = TEXT("fixture_hjson_mechanics");
		Fixture.PublicRouteId = TEXT("content_graph.import_workspace");
		Fixture.bPublicRoute = true;
		Fixture.bRealMechanics = true;
		Fixture.bIndependentReadback = true;
		Fixture.bPersistReload = true;
		Fixture.bFailureCleanup = true;
		Fixture.bLegacyExit = true;
		return Fixture;
	}

	HjsonWorkspace::FTargetCompositionContract MakeFixtureCompositionContract(
		const bool bWithSemanticRoute)
	{
		HjsonWorkspace::FTargetCompositionContract Contract;
		Contract.SchemaId =
			HjsonWorkspace::GetTargetCompositionContractSchemaId();
		Contract.TargetKind = TEXT("fixture_target");
		Contract.Surfaces = {
			{
				TEXT("target_settings"),
				HjsonWorkspace::EAuthoringSurfaceKind::Properties,
				HjsonWorkspace::ESemanticUnitKind::PropertySet,
			},
			{
				TEXT("graph_collection"),
				HjsonWorkspace::EAuthoringSurfaceKind::Structure,
				HjsonWorkspace::ESemanticUnitKind::StructureState,
			},
		};
		if (bWithSemanticRoute)
		{
			Contract.SemanticRoutes.Add({
				HjsonWorkspace::ECompositionSemanticRouteMatch::Default,
				FString(),
				TEXT("target_settings"),
				HjsonWorkspace::ESemanticUnitKind::PropertySet,
				HjsonWorkspace::ECompositionSemanticRouteRole::Property,
				0,
			});
		}
		return Contract;
	}

	FDomainCapabilityDeclaration MakeHjsonDeclaration(
		const bool bWithSemanticRoute = true)
	{
		FDomainCapabilityDeclaration Declaration =
			MakeExplicitlyUnsupportedDomainCapabilityDeclaration(
				TEXT("fixture-hjson-domain"));
		Declaration.Hjson.Support = EDomainCapabilitySupport::Supported;
		Declaration.Hjson.Providers.Add(
			MakeShared<FNullHjsonDomainProvider>(TEXT("fixture-hjson-provider")));
		Declaration.Hjson.Routes.Add({
			TEXT("fixture-hjson-provider"),
			TEXT("content_graph.import_workspace"),
			TEXT("uai.fixture.domain/1"),
			TEXT("fixture_target"),
			TEXT("/Script/CoreUObject.Object"),
			HjsonWorkspace::EInitializationNodeRole::RootTarget,
			TEXT("uai.init.fixture.root/1"),
			true,
		});
		Declaration.Composition.Support = EDomainCapabilitySupport::Supported;
		Declaration.Composition.Contracts.Add(
			MakeFixtureCompositionContract(bWithSemanticRoute));
		Declaration.Initialization.Support =
			EDomainCapabilitySupport::Supported;
		Declaration.Initialization.ContractIds.Add(
			TEXT("uai.init.fixture.root/1"));
		Declaration.Migration.Support = EDomainCapabilitySupport::Supported;
		Declaration.Migration.OwnerId = TEXT("fixture-migration-owner");
		return Declaration;
	}

	TSharedPtr<const FDomainCapabilityContribution> MakeHjsonContribution(
		FAutomationTestBase* Test)
	{
		TSharedPtr<const FDomainCapabilityContribution> Contribution;
		FString Error;
		Test->TestTrue(
			TEXT("freeze fixture HJSON capability"),
			FDomainCapabilityContribution::TryCreate(
				MakeHjsonDeclaration(),
				Contribution,
				Error));
		return Contribution;
	}

	class FNeverCancelled final : public Contract::IUeAgentCancellationSignal
	{
	public:
		bool IsCancelled() const override { return false; }
	};

	class FConformanceInvocationPorts final
		: public BindingGateway::Operation::IOperationVersionFencePort
		, public BindingGateway::Operation::IOperationDomainMutationPort
		, public BindingGateway::Operation::IOperationStabilityPort
		, public BindingGateway::Operation::IOperationPersistencePort
		, public BindingGateway::Operation::IOperationIndependentVerificationPort
	{
	public:
		explicit FConformanceInvocationPorts(FString InRequestId)
			: RequestId(MoveTemp(InRequestId))
		{
		}

		bool Check(
			const BindingGateway::Operation::FOperationLifecyclePlan&,
			BindingGateway::Operation::EOperationLifecycleStage,
			bool& bOutCurrent,
			BindingGateway::Operation::FOperationLifecycleError&) override
		{
			bOutCurrent = true;
			return true;
		}

		bool Execute(
			const BindingGateway::Operation::FOperationLifecyclePlan&,
			BindingGateway::Operation::FOperationMutationFacts& OutFacts,
			BindingGateway::Operation::FOperationLifecycleError&) override
		{
			OutFacts = BindingGateway::Operation::FOperationMutationFacts(
				TEXT("mutation:") + RequestId,
				{TEXT("target:") + RequestId},
				true);
			return true;
		}

		bool Rollback(
			const BindingGateway::Operation::FOperationLifecyclePlan&,
			const BindingGateway::Operation::FOperationMutationFacts&,
			bool& bOutConfirmed,
			BindingGateway::Operation::FOperationLifecycleError&) override
		{
			bOutConfirmed = true;
			return true;
		}

		bool Settle(
			const BindingGateway::Operation::FOperationLifecyclePlan&,
			const BindingGateway::Operation::FOperationMutationFacts&,
			BindingGateway::Operation::FOperationLifecycleError&) override
		{
			return true;
		}

		bool Persist(
			const BindingGateway::Operation::FOperationLifecyclePlan&,
			const BindingGateway::Operation::FOperationMutationFacts&,
			FString& OutReceipt,
			BindingGateway::Operation::FOperationLifecycleError&) override
		{
			OutReceipt = TEXT("receipt:") + RequestId;
			return true;
		}

		bool Verify(
			const BindingGateway::Operation::FOperationLifecyclePlan&,
			const BindingGateway::Operation::FOperationMutationFacts&,
			const FString&,
			bool& bOutEquivalent,
			BindingGateway::Operation::FOperationLifecycleError&) override
		{
			bOutEquivalent = true;
			return true;
		}

	private:
		FString RequestId;
	};

	class FConformanceInvocationContext final
		: public IDomainOperationInvocationContext
	{
	public:
		explicit FConformanceInvocationContext(FString InRequestId)
			: RequestId(MoveTemp(InRequestId))
		{
		}

		bool TryBuildSuccessPayload(
			const BindingGateway::Operation::FOperationLifecycleReceipt&,
			FString& OutPayloadJson,
			FString& OutError) const override
		{
			OutError.Reset();
			OutPayloadJson = FString::Printf(
				TEXT("{\"request_id\":\"%s\"}"),
				*RequestId);
			return true;
		}

	private:
		FString RequestId;
	};

	class FConformanceInvocationFactory final
		: public IDomainOperationInvocationFactory
	{
	public:
		explicit FConformanceInvocationFactory(const bool bInReuseInvocation)
			: bReuseInvocation(bInReuseInvocation)
		{
		}

		const FString& GetOperationId() const override
		{
			static const FString Id = TEXT("fixture.operation");
			return Id;
		}

		bool TryCreate(
			const CommandEdge::FUeAgentCommandRequest& Request,
			const Contract::FUeAgentCallerContext&,
			const TSharedRef<Contract::IUeAgentCancellationSignal>&,
			FDomainOperationInvocation& OutInvocation,
			FDomainOperationInvocationError& OutError) const override
		{
			if (bReuseInvocation && CachedInvocation.IsSet())
			{
				OutInvocation = CachedInvocation.GetValue();
				OutError = {};
				return true;
			}
			const FString RequestId = Request.GetRequestId();
			const FString Fingerprint = TEXT("sha256:")
				+ FString::ChrN(64, RequestId.EndsWith(TEXT("a"))
					? TEXT('a') : TEXT('b'));
			const TSharedRef<FConformanceInvocationPorts> Ports =
				MakeShared<FConformanceInvocationPorts>(RequestId);
			FDomainOperationInvocation Created;
			if (!FDomainOperationInvocation::TryCreate(
					MakeShared<BindingGateway::Operation::FOperationLifecyclePlan>(
						TEXT("plan:") + RequestId,
						Fingerprint,
						TArray<FString>{TEXT("target:") + RequestId},
						TMap<FString, FString>{
							{TEXT("target:") + RequestId, TEXT("revision:1")}},
						TArray<FString>{TEXT("settled")}),
					MakeShared<BindingGateway::Operation::
						FOperationImpactAuthorization>(
						Fingerprint,
						TArray<FString>{TEXT("target:") + RequestId}),
					MakeShared<BindingGateway::Operation::FOperationLifecycleService>(
						StaticCastSharedRef<BindingGateway::Operation::
							IOperationVersionFencePort>(Ports),
						StaticCastSharedRef<BindingGateway::Operation::
							IOperationDomainMutationPort>(Ports),
						StaticCastSharedRef<BindingGateway::Operation::
							IOperationStabilityPort>(Ports),
						StaticCastSharedRef<BindingGateway::Operation::
							IOperationPersistencePort>(Ports),
						StaticCastSharedRef<BindingGateway::Operation::
							IOperationIndependentVerificationPort>(Ports)),
					MakeShared<FConformanceInvocationContext>(RequestId),
					Created,
					OutError))
			{
				return false;
			}
			if (bReuseInvocation)
			{
				CachedInvocation = Created;
			}
			OutInvocation = MoveTemp(Created);
			return true;
		}

	private:
		bool bReuseInvocation = false;
		mutable TOptional<FDomainOperationInvocation> CachedInvocation;
	};

	FDomainCapabilityDeclaration MakeOperationDeclaration(
		const bool bReuseInvocation)
	{
		FDomainCapabilityDeclaration Declaration =
			MakeExplicitlyUnsupportedDomainCapabilityDeclaration(
				TEXT("fixture-operation-domain"));
		Declaration.Operation.Support = EDomainCapabilitySupport::Supported;
		FDomainOperationEntry Entry;
		Entry.OperationId = TEXT("fixture.operation");
		Entry.Kind = EDomainOperationKind::Write;
		Entry.TerminalPolicy = EDomainOperationTerminalPolicy::
			SucceededRejectedRolledBackOrStateUnknown;
		Entry.ExecutionPolicy = EDomainOperationExecutionPolicy::Immediate;
		Entry.TransientStatePolicy = EDomainTransientStatePolicy::None;
		Entry.bPersists = true;
		Entry.bIndependentReadback = true;
		Entry.InvocationFactory =
			MakeShared<const FConformanceInvocationFactory>(bReuseInvocation);
		Declaration.Operation.Operations.Add(MoveTemp(Entry));
		return Declaration;
	}

	bool DecodeOperationRequest(
		const FString& RequestId,
		TOptional<CommandEdge::FUeAgentCommandRequest>& OutRequest)
	{
		const FString Envelope = FString::Printf(
			TEXT("{\"schema\":\"uai.command.v1\",\"version\":1,")
			TEXT("\"message_kind\":\"command_request\",")
			TEXT("\"correlation_id\":\"corr-t66\",")
			TEXT("\"request_id\":\"%s\",")
			TEXT("\"command_id\":\"fixture.operation\",")
			TEXT("\"command_binding_hash\":\"sha256:%s\",\"params\":{}}"),
			*RequestId,
			*FString::ChrN(64, TEXT('c')));
		FTCHARToUTF8 Encoded(*Envelope);
		TArray<uint8> Bytes;
		Bytes.Append(
			reinterpret_cast<const uint8*>(Encoded.Get()),
			Encoded.Length());
		CommandEdge::FUeAgentCommandProtocolDiagnostic Diagnostic;
		return CommandEdge::FUeAgentCommandProtocolCodec().DecodeRequest(
			Bytes,
			OutRequest,
			Diagnostic);
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUeAgentDomainConformanceScenarioMatrixTest,
	"UeAgentInterface.DomainConformance.ScenarioMatrix",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FUeAgentDomainConformanceScenarioMatrixTest::RunTest(
	const FString& Parameters)
{
	const FString Root = FPaths::ConvertRelativePathToFull(
		FPaths::ProjectSavedDir()
		/ TEXT("Automation/DomainConformance/T66/matrix"));
	const FDomainConformanceFixture Fixture = MakeHjsonFixture(Root);
	const TSharedPtr<const FDomainCapabilityContribution> Contribution =
		MakeHjsonContribution(this);
	if (!Contribution.IsValid())
	{
		return false;
	}

	FDomainConformancePlan Plan;
	FString Error;
	TestTrue(
		TEXT("capability and fixture expand into a conformance plan"),
		TryBuildDomainConformancePlan(
			Contribution.ToSharedRef(),
			Fixture,
			Plan,
			Error));
	TestTrue(TEXT("plan remains valid"), Plan.IsValid());
	TestTrue(TEXT("static conflict gate is present"),
		Plan.Contains(EDomainConformanceScenario::CapabilityFreeze));
	TestTrue(TEXT("process-authority adoption gate is present"),
		Plan.Contains(EDomainConformanceScenario::ProcessAuthorityAdoption));
	TestTrue(TEXT("composition semantic-route gate is present"),
		Plan.Contains(EDomainConformanceScenario::CompositionSemanticRoutes));
	TestTrue(TEXT("inline HJSON source is present"),
		Plan.Contains(EDomainConformanceScenario::HjsonInline));
	TestTrue(TEXT("single-file HJSON source is present"),
		Plan.Contains(EDomainConformanceScenario::HjsonSingleFile));
	TestTrue(TEXT("directory HJSON source is present"),
		Plan.Contains(EDomainConformanceScenario::HjsonDirectory));
	TestTrue(TEXT("stale revision is present"),
		Plan.Contains(EDomainConformanceScenario::StaleRevisionReject));
	TestTrue(TEXT("persist/reload is present"),
		Plan.Contains(EDomainConformanceScenario::PersistReloadReadback));
	TestTrue(TEXT("failure cleanup is present"),
		Plan.Contains(EDomainConformanceScenario::FailureCleanup));
	TestTrue(TEXT("legacy exit is present"),
		Plan.Contains(EDomainConformanceScenario::LegacyExit));
	TestFalse(TEXT("unsupported layout is not invented"),
		Plan.Contains(EDomainConformanceScenario::GraphLayout));
	TestFalse(TEXT("unsupported long running is not invented"),
		Plan.Contains(EDomainConformanceScenario::LongRunningTerminal));
	TestFalse(TEXT("unsupported operation factory is not invented"),
		Plan.Contains(EDomainConformanceScenario::OperationRequestFactory));

	FDomainConformanceFixture MissingCapability = Fixture;
	MissingCapability.DomainId = TEXT("different-domain");
	TestFalse(
		TEXT("a fixture cannot target a missing capability"),
		TryBuildDomainConformancePlan(
			Contribution.ToSharedRef(),
			MissingCapability,
			Plan,
			Error));
	TestEqual(
		TEXT("missing capability failure is exact"),
		Error,
		FString(TEXT("domain_conformance_fixture_domain_mismatch:different-domain")));

	FDomainConformanceFixture MissingReadback = Fixture;
	MissingReadback.bIndependentReadback = false;
	TestFalse(
		TEXT("a write fixture cannot omit independent readback"),
		TryBuildDomainConformancePlan(
			Contribution.ToSharedRef(),
			MissingReadback,
			Plan,
			Error));
	TestEqual(
		TEXT("readback failure is exact"),
		Error,
		FString(TEXT("domain_conformance_independent_readback_missing:fixture:t66:hjson")));

	FDomainConformanceFixture MissingPersistReload = Fixture;
	MissingPersistReload.bPersistReload = false;
	TestFalse(
		TEXT("a persisted write fixture cannot omit reload verification"),
		TryBuildDomainConformancePlan(
			Contribution.ToSharedRef(),
			MissingPersistReload,
			Plan,
			Error));
	TestEqual(
		TEXT("persist/reload failure is exact"),
		Error,
		FString(TEXT("domain_conformance_persist_reload_missing:fixture:t66:hjson")));

	TSharedPtr<const FDomainCapabilityContribution> MissingRoutes;
	TestTrue(
		TEXT("a composition declaration without routes can still reach conformance"),
		FDomainCapabilityContribution::TryCreate(
			MakeHjsonDeclaration(false), MissingRoutes, Error));
	if (!MissingRoutes.IsValid())
	{
		return false;
	}
	TestFalse(
		TEXT("conformance rejects a composition contract without semantic routes"),
		TryBuildDomainConformancePlan(
			MissingRoutes.ToSharedRef(), Fixture, Plan, Error));
	TestEqual(
		TEXT("missing semantic routes failure is exact"),
		Error,
		FString(TEXT("domain_conformance_composition_routes_missing:fixture_target")));

	FDomainCapabilityDeclaration Ambiguous = MakeHjsonDeclaration();
	Ambiguous.Composition.Contracts[0].SemanticRoutes.Add({
		HjsonWorkspace::ECompositionSemanticRouteMatch::Suffix,
		TEXT(".node"),
		TEXT("target_settings"),
		HjsonWorkspace::ESemanticUnitKind::PropertySet,
		HjsonWorkspace::ECompositionSemanticRouteRole::Property,
		0,
	});
	TSharedPtr<const FDomainCapabilityContribution> InvalidContribution;
	TestFalse(
		TEXT("conformance input cannot bypass ambiguous semantic-route validation"),
		FDomainCapabilityContribution::TryCreate(
			MoveTemp(Ambiguous), InvalidContribution, Error));
	TestEqual(
		TEXT("ambiguous semantic-route failure is exact"),
		Error,
		FString(TEXT("domain_capability_composition_invalid:fixture_target:"))
			+ TEXT("target_composition_semantic_route_priority_duplicate:0"));

	FDomainCapabilityDeclaration Dangling = MakeHjsonDeclaration();
	Dangling.Composition.Contracts[0].SemanticRoutes[0].SurfaceId =
		TEXT("missing_surface");
	TestFalse(
		TEXT("conformance input cannot bypass dangling semantic-route validation"),
		FDomainCapabilityContribution::TryCreate(
			MoveTemp(Dangling), InvalidContribution, Error));
	TestEqual(
		TEXT("dangling semantic-route failure is exact"),
		Error,
		FString(TEXT("domain_capability_composition_invalid:fixture_target:"))
			+ TEXT("target_composition_semantic_route_surface_missing:missing_surface"));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUeAgentDomainConformanceOperationFactoryTest,
	"UeAgentInterface.DomainConformance.OperationFactory",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FUeAgentDomainConformanceOperationFactoryTest::RunTest(
	const FString& Parameters)
{
	FString Error;
	TSharedPtr<const FDomainCapabilityContribution> Contribution;
	TestTrue(
		TEXT("freeze request-scoped operation contribution"),
		FDomainCapabilityContribution::TryCreate(
			MakeOperationDeclaration(false),
			Contribution,
			Error));
	if (!Contribution.IsValid())
	{
		return false;
	}
	FDomainConformanceFixture Fixture = MakeHjsonFixture(
		FPaths::ConvertRelativePathToFull(
			FPaths::ProjectSavedDir()
				/ TEXT("Automation/DomainConformance/T66/operation")));
	Fixture.FixtureId = TEXT("fixture:t66:operation");
	Fixture.DomainId = TEXT("fixture-operation-domain");
	Fixture.MechanicsFamily = TEXT("fixture_operation_mechanics");
	Fixture.PublicRouteId = TEXT("fixture.operation");
	Fixture.EntryFile.Reset();
	Fixture.CanonicalDocument.Reset();
	Fixture.WorkspaceRoot.Reset();
	FDomainConformancePlan Plan;
	TestTrue(
		TEXT("non-HJSON operation expands without workspace-only fields"),
		TryBuildDomainConformancePlan(
			Contribution.ToSharedRef(), Fixture, Plan, Error));
	TestTrue(
		TEXT("operation request-factory scenario is required"),
		Plan.Contains(EDomainConformanceScenario::OperationRequestFactory));

	TOptional<CommandEdge::FUeAgentCommandRequest> FirstRequest;
	TOptional<CommandEdge::FUeAgentCommandRequest> SecondRequest;
	TestTrue(TEXT("decode first operation request"),
		DecodeOperationRequest(TEXT("request-a"), FirstRequest));
	TestTrue(TEXT("decode second operation request"),
		DecodeOperationRequest(TEXT("request-b"), SecondRequest));
	if (!FirstRequest.IsSet() || !SecondRequest.IsSet())
	{
		return false;
	}
	const Contract::FUeAgentCallerContext Caller(
		TEXT("t66-caller"), {TEXT("uai:operate")}, TEXT("corr-t66"));
	const TSharedRef<Contract::IUeAgentCancellationSignal> Cancellation =
		MakeShared<FNeverCancelled>();
	TestTrue(
		TEXT("two requests receive isolated operation invocation state"),
		ValidateRequestScopedOperationFactory(
			Contribution.ToSharedRef(),
			TEXT("fixture.operation"),
			FirstRequest.GetValue(),
			SecondRequest.GetValue(),
			Caller,
			Cancellation,
			Error));

	TSharedPtr<const FDomainCapabilityContribution> SharedContribution;
	TestTrue(
		TEXT("freeze deliberately shared invocation fixture"),
		FDomainCapabilityContribution::TryCreate(
			MakeOperationDeclaration(true),
			SharedContribution,
			Error));
	TestFalse(
		TEXT("the conformance oracle rejects shared request state"),
		ValidateRequestScopedOperationFactory(
			SharedContribution.ToSharedRef(),
			TEXT("fixture.operation"),
			FirstRequest.GetValue(),
			SecondRequest.GetValue(),
			Caller,
			Cancellation,
			Error));
	TestEqual(
		TEXT("shared request state failure is exact"),
		Error,
		FString(TEXT("domain_conformance_operation_invocation_shared_state:fixture.operation")));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUeAgentDomainConformanceWorkspaceFixtureTest,
	"UAI.Root.T36.HjsonSource.SemanticCarrierIdentity",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FUeAgentDomainConformanceWorkspaceFixtureTest::RunTest(
	const FString& Parameters)
{
	const FString Root = FPaths::ConvertRelativePathToFull(
		FPaths::ProjectSavedDir()
		/ TEXT("Automation/DomainConformance/T66/workspace"));
	IFileManager::Get().DeleteDirectory(*Root, false, true);
	const FDomainConformanceFixture Fixture = MakeHjsonFixture(Root);
	FDomainWorkspaceFixture Workspace;
	FString Error;
	if (!TestTrue(
		TEXT("one semantic fixture builds all three carriers"),
		TryBuildEquivalentWorkspaceFixture(Fixture, Workspace, Error)))
	{
		AddError(Error);
		return false;
	}
	TestTrue(TEXT("workspace fixture is valid"), Workspace.IsValid());
	FDomainWorkspaceFixture Formatted = Workspace;
	TestTrue(TEXT("single-file carrier can vary formatting without semantic drift"),
		Formatted.TryOverwriteSingleFileForTest(
			Fixture.CanonicalDocument.Replace(
				TEXT("  schema:"), TEXT("schema:")),
			Error));

	TArray<FString> Documents;
	TArray<FString> SourceFingerprints;
	TArray<FString> SemanticFingerprints;
	TArray<HjsonWorkspace::EWorkspaceSourceKind> Kinds;
	for (const FDomainWorkspaceCarrier& Carrier : Formatted.GetCarriers())
	{
		ContextResource::FUeAgentResourceTracker Resources = MakeResources();
		HjsonWorkspace::FResolvedContentGraphImportSourcePtr Resolved;
		HjsonWorkspace::FWorkspaceError WorkspaceError;
		TestTrue(
			TEXT("existing source loader accepts generated carrier"),
			HjsonWorkspace::TryLoadContentGraphImportSource(
				Carrier.MakePublicSourceObject(),
				Fixture.TargetIdentity,
				Resources,
				Resolved,
				WorkspaceError));
		TestTrue(TEXT("resolved carrier exists"), Resolved.IsValid());
		if (Resolved.IsValid() && Resolved->GetEntryDocument())
		{
			Kinds.Add(Resolved->GetKind());
			Documents.Add(Resolved->GetEntryDocument()->GetDocument());
			SourceFingerprints.Add(Resolved->GetSourceFingerprint());
			SemanticFingerprints.Add(Resolved->GetSemanticFingerprint());
		}
	}
	TestEqual(TEXT("three carriers resolve"), Documents.Num(), 3);
	if (SemanticFingerprints.Num() == 3)
	{
		TestEqual(TEXT("inline and formatted single-file semantics match"),
			SemanticFingerprints[0], SemanticFingerprints[1]);
		TestEqual(TEXT("inline and directory semantics match"),
			SemanticFingerprints[0], SemanticFingerprints[2]);
	}
	if (SourceFingerprints.Num() == 3)
	{
		TSet<FString> ProvenanceIdentities;
		for (const FString& Fingerprint : SourceFingerprints)
		{
			ProvenanceIdentities.Add(Fingerprint);
		}
		TestEqual(TEXT("carrier provenance remains independently identifiable"),
			ProvenanceIdentities.Num(), 3);
	}
	TestTrue(TEXT("inline carrier is explicit"),
		Kinds.Contains(HjsonWorkspace::EWorkspaceSourceKind::Inline));
	TestTrue(TEXT("single-file carrier is explicit"),
		Kinds.Contains(HjsonWorkspace::EWorkspaceSourceKind::SingleFile));
	TestTrue(TEXT("directory carrier is explicit"),
		Kinds.Contains(HjsonWorkspace::EWorkspaceSourceKind::Directory));

	FDomainWorkspaceFixture Drifted = Workspace;
	TestTrue(TEXT("test fixture can introduce one semantic drift"),
		Drifted.TryOverwriteSingleFileForTest(
			Fixture.CanonicalDocument.Replace(
				TEXT("count: 2"),
				TEXT("count: 3")),
			Error));
	TestFalse(TEXT("carrier semantic drift is rejected"),
		ValidateEquivalentWorkspaceFixture(Drifted, Error));
	TestEqual(TEXT("source drift failure is exact"), Error,
		FString(TEXT("domain_conformance_source_semantic_drift:single_file")));

	IFileManager::Get().DeleteDirectory(*Root, false, true);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUeAgentT36HjsonSourceResourceBudgetTest,
	"UAI.Root.T36.HjsonSource.ResourceBudget",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FUeAgentT36HjsonSourceResourceBudgetTest::RunTest(
	const FString& Parameters)
{
	(void)Parameters;
	using namespace UE::UAI;
	const TSharedRef<FJsonObject> Source = MakeShared<FJsonObject>();
	Source->SetStringField(TEXT("kind"), TEXT("inline"));
	Source->SetStringField(
		TEXT("document"),
		TEXT("{schema:uai.fixture/1,root:{type:/Script/CoreUObject.Object}}"));
	ContextResource::FUeAgentResourceTracker Resources(
		ContextResource::FUeAgentResourceBudget({
			{ContextResource::EUeAgentResourceDimension::InputBytes, 1},
			{ContextResource::EUeAgentResourceDimension::StringBytes, 1},
			{ContextResource::EUeAgentResourceDimension::WorkUnits, 1},
			{ContextResource::EUeAgentResourceDimension::EstimatedMemoryBytes, 1},
		}));
	HjsonWorkspace::FResolvedContentGraphImportSourcePtr Resolved;
	HjsonWorkspace::FWorkspaceError Error;
	TestFalse(TEXT("the source loader stops before parsing beyond its budget"),
		HjsonWorkspace::TryLoadContentGraphImportSource(
			Source,
			TEXT("/Game/UAI/T36/Fixture.Fixture"),
			Resources,
			Resolved,
			Error));
	TestEqual(TEXT("the resource failure remains typed"),
		Error.GetCode(), FString(TEXT("resource_budget_exceeded")));
	TestFalse(TEXT("no resolved source escapes the failed gate"),
		Resolved.IsValid());
	return !HasAnyErrors();
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUeAgentDomainConformanceLayerGateTest,
	"UeAgentInterface.DomainConformance.LayerGate",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FUeAgentDomainConformanceLayerGateTest::RunTest(
	const FString& Parameters)
{
	const FString Root = FPaths::ConvertRelativePathToFull(
		FPaths::ProjectSavedDir()
		/ TEXT("Automation/DomainConformance/T66/layers"));
	const FDomainConformanceFixture Fixture = MakeHjsonFixture(Root);
	const TSharedPtr<const FDomainCapabilityContribution> Contribution =
		MakeHjsonContribution(this);
	FDomainConformancePlan Plan;
	FString Error;
	TestTrue(TEXT("build layered plan"),
		TryBuildDomainConformancePlan(
			Contribution.ToSharedRef(), Fixture, Plan, Error));

	FDomainConformanceLayerResults Results;
	Results.Record(
		EDomainConformanceLayer::StaticContract,
		EDomainConformanceStatus::Passed,
		TEXT("fixture:static"));
	Results.Record(
		EDomainConformanceLayer::DomainAdapter,
		EDomainConformanceStatus::Failed,
		TEXT("fixture:adapter-red"));
	FDomainOwnedWorkerBatch Batch;
	TestFalse(
		TEXT("failed low-cost layer prevents UE worker planning"),
		TryBuildOwnedWorkerBatch(Plan, Fixture, Results, Batch, Error));
	TestEqual(TEXT("layer gate failure is exact"), Error,
		FString(TEXT("domain_conformance_low_cost_layer_incomplete:domain_adapter")));

	Results.Record(
		EDomainConformanceLayer::DomainAdapter,
		EDomainConformanceStatus::Passed,
		TEXT("fixture:adapter"));
	TestTrue(
		TEXT("passed low-cost layers produce an owned-worker batch"),
		TryBuildOwnedWorkerBatch(Plan, Fixture, Results, Batch, Error));
	TestTrue(TEXT("batch is explicitly headless and owned"),
		Batch.IsValid() && Batch.bRequiresHeadless && Batch.bPreserveMainEditor
			&& Batch.CleanupPolicy == TEXT("owned_processes_and_temp_artifacts"));

	FDomainProductEvidence Synthetic;
	Synthetic.RouteId = Fixture.PublicRouteId;
	Synthetic.ReceiptRef = TEXT("fixture-receipt.json");
	Synthetic.bReceiptExists = true;
	Synthetic.bSynthetic = true;
	Synthetic.bPublicRouteObserved = false;
	Synthetic.bIndependentReadbackObserved = false;
	Synthetic.bCleanupObserved = false;
	TestFalse(
		TEXT("synthetic receipt existence is not production-ready evidence"),
		ValidateProductEvidence(Fixture, Synthetic, Error));
	TestEqual(TEXT("synthetic failure is exact"), Error,
		FString(TEXT("domain_conformance_synthetic_product_evidence:fixture-receipt.json")));

	Synthetic.bSynthetic = false;
	TestFalse(
		TEXT("a real receipt without public route/readback/cleanup is insufficient"),
		ValidateProductEvidence(Fixture, Synthetic, Error));
	TestEqual(TEXT("public evidence failure is exact"), Error,
		FString(TEXT("domain_conformance_public_product_evidence_incomplete:fixture-receipt.json")));

	Synthetic.bPublicRouteObserved = true;
	Synthetic.bIndependentReadbackObserved = true;
	Synthetic.bCleanupObserved = true;
	TestTrue(
		TEXT("bounded real public evidence satisfies only its fixture"),
		ValidateProductEvidence(Fixture, Synthetic, Error));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUeAgentDomainConformanceRepresentativeCalibrationTest,
	"UeAgentInterface.DomainConformance.RepresentativeCalibration",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FUeAgentDomainConformanceRepresentativeCalibrationTest::RunTest(
	const FString& Parameters)
{
	TSharedPtr<const FFrozenDomainCapabilitySet> Frozen;
	FString Error;
	if (!TestTrue(
		TEXT("representative process contributions freeze"),
		TryFreezeProcessDomainCapabilities(Frozen, Error))
		|| !Frozen.IsValid())
	{
		AddError(Error);
		return false;
	}

	struct FRepresentative
	{
		const TCHAR* DomainId;
		const TCHAR* Target;
	};
	const FRepresentative Representatives[] = {
		{
			TEXT("uobject-dataasset"),
			TEXT("/Game/UAI/T66/T13.T13"),
		},
		{
			TEXT("datatable-curvetable"),
			TEXT("/Game/UAI/T66/T27.T27"),
		},
		{
			TEXT("localization-string-table"),
			TEXT("/Game/UAI/T66/T30.T30"),
		},
	};
	for (const FRepresentative& Representative : Representatives)
	{
		const TSharedRef<const FDomainCapabilityContribution>* Contribution =
			Frozen->GetContributions().FindByPredicate(
				[&Representative](
					const TSharedRef<const FDomainCapabilityContribution>& Item)
				{
					return Item->GetDomainId() == Representative.DomainId;
				});
		TestNotNull(TEXT("representative contribution is present"), Contribution);
		if (!Contribution)
		{
			continue;
		}
		FDomainConformanceFixture Fixture = MakeHjsonFixture(
			FPaths::ConvertRelativePathToFull(
				FPaths::ProjectSavedDir()
				/ TEXT("Automation/DomainConformance/T66/calibration")
				/ Representative.DomainId));
		Fixture.FixtureId = FString(TEXT("fixture:t66:"))
			+ Representative.DomainId;
		Fixture.DomainId = Representative.DomainId;
		Fixture.TargetIdentity = Representative.Target;
		Fixture.MechanicsFamily = FString(TEXT("calibration:"))
			+ Representative.DomainId;
		FDomainConformancePlan Plan;
		TestTrue(
			TEXT("representative contribution expands through the common harness"),
			TryBuildDomainConformancePlan(
				*Contribution,
				Fixture,
				Plan,
				Error));
		TestTrue(TEXT("representative plan includes all HJSON carriers"),
			Plan.Contains(EDomainConformanceScenario::HjsonInline)
				&& Plan.Contains(EDomainConformanceScenario::HjsonSingleFile)
				&& Plan.Contains(EDomainConformanceScenario::HjsonDirectory));
		TestTrue(TEXT("representative plan requires public persistence/readback"),
			Plan.Contains(EDomainConformanceScenario::PersistReloadReadback));
		TestTrue(TEXT("representative plan requires old-entry exit"),
			Plan.Contains(EDomainConformanceScenario::LegacyExit));
	}
	return !HasAnyErrors();
}
