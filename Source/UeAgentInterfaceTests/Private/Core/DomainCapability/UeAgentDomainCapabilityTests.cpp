#include "Misc/AutomationTest.h"
#include "Dom/JsonValue.h"

#include "Core/DomainCapability/UeAgentDomainCapability.h"
#include "Core/BindingGateway/Query/UeAgentQueryBindingError.h"
#include "Module/CommandRegistration/UObjectDataAsset/UeAgentUObjectDataAssetRegistration.h"
#include "OperationOwners/DataTableCurveTable/UeAgentDataTableCurveTableDomainService.h"

namespace
{
	using namespace UE::UAI;
	using namespace UE::UAI::DomainCapability;

	FDomainCapabilityDeclaration MakeExplicitlyUnsupported(
		const FString& DomainId)
	{
		return MakeExplicitlyUnsupportedDomainCapabilityDeclaration(DomainId);
	}

	bool CreateDataTableProvider(
		TSharedPtr<HjsonWorkspace::IImportInitializationDomainProvider>&
			OutProvider,
		Contract::FDiagnostic& OutError)
	{
		HjsonWorkspace::FWorkspaceError WorkspaceError;
		const bool bCreated = OperationOwners::DataTableCurveTable::
			TryCreateDataTableCurveTableDomainProvider(
				OutProvider,
				WorkspaceError);
		OutError = WorkspaceError.GetDiagnostic();
		return bCreated;
	}

	FDomainCapabilityDeclaration MakeDataTableContribution(
		const FString& DomainId)
	{
		FDomainCapabilityDeclaration Declaration =
			MakeExplicitlyUnsupported(DomainId);
		TSharedPtr<HjsonWorkspace::IImportInitializationDomainProvider>
			Provider;
		Contract::FDiagnostic Error;
		CreateDataTableProvider(Provider, Error);
		Declaration.Hjson.Support = EDomainCapabilitySupport::Supported;
		Declaration.Hjson.Providers.Add(Provider.ToSharedRef());
		Declaration.Hjson.Routes = {
			{
				Provider->GetProviderId(),
				TEXT("content_graph.import_workspace"),
				TEXT("uai.datatable_curvetable.edit/1"),
				TEXT("data_table"),
				TEXT("/Script/Engine.DataTable"),
				HjsonWorkspace::EInitializationNodeRole::RootTarget,
				TEXT("uai.init.datatable_curvetable.data_table.root/1"),
				true,
			},
			{
				Provider->GetProviderId(),
				TEXT("content_graph.import_workspace"),
				TEXT("uai.datatable_curvetable.edit/1"),
				TEXT("curve_table"),
				TEXT("/Script/Engine.CurveTable"),
				HjsonWorkspace::EInitializationNodeRole::RootTarget,
				TEXT("uai.init.datatable_curvetable.curve_table.root/1"),
				true,
			},
		};
		Declaration.Initialization.Support =
			EDomainCapabilitySupport::Supported;
		Declaration.Initialization.ContractIds = {
			TEXT("uai.init.datatable_curvetable.data_table.root/1"),
			TEXT("uai.init.datatable_curvetable.curve_table.root/1"),
		};
		return Declaration;
	}

	class FDummyCatalogSource final : public Discovery::IUeAgentCatalogSource
	{
	public:
		FDummyCatalogSource()
			: Kinds({TEXT("fixture.catalog")})
		{
		}

		const FString& GetSourceId() const override
		{
			return SourceId;
		}

		const TArray<FString>& GetCatalogKinds() const override
		{
			return Kinds;
		}

		bool TrySearch(
			const Discovery::FUeAgentDiscoveryQuery&,
			ContextResource::FUeAgentResourceTracker&,
			const ContextResource::FUeAgentOperationDeadline&,
			const ContextResource::FUeAgentCancellationToken&,
			TSharedPtr<const Discovery::FUeAgentCatalogSnapshot>& OutCatalog,
			Discovery::FUeAgentDiscoveryError& OutError) override
		{
			OutCatalog.Reset();
			OutError.Code = TEXT("fixture_not_called");
			return false;
		}

		bool TryResolveExact(
			const FString&,
			const Discovery::FUeAgentCatalogRevision&,
			ContextResource::FUeAgentResourceTracker&,
			const ContextResource::FUeAgentOperationDeadline&,
			const ContextResource::FUeAgentCancellationToken&,
			TSharedPtr<const Discovery::FUeAgentCandidateBinding>& OutBinding,
			Discovery::FUeAgentDiscoveryError& OutError) override
		{
			OutBinding.Reset();
			OutError.Code = TEXT("fixture_not_called");
			return false;
		}

	private:
		FString SourceId = TEXT("fixture-catalog-source");
		TArray<FString> Kinds;
	};

	class FDummyReadPort final : public Discovery::IUeAgentStateReadPort
	{
	public:
		const FString& GetPortId() const override
		{
			return PortId;
		}

		bool TryRead(
			const Discovery::FUeAgentStateReadRequest&,
			ContextResource::FUeAgentResourceTracker&,
			const ContextResource::FUeAgentOperationDeadline&,
			const ContextResource::FUeAgentCancellationToken&,
			TSharedPtr<const StateSemantic::FUeAgentCanonicalSnapshot>& OutSnapshot,
			Discovery::FUeAgentDiscoveryError& OutError) override
		{
			OutSnapshot.Reset();
			OutError.Code = TEXT("fixture_not_called");
			return false;
		}

	private:
		FString PortId = TEXT("fixture-read-port");
	};

	class FDummyDomainQueryProvider final
		: public BindingGateway::Query::IUeAgentDomainQueryProvider
	{
	public:
		bool TryQuery(
			const Contract::FUeAgentQueryDescriptor&,
			const Contract::FUeAgentContractValue&,
			const TOptional<Discovery::FUeAgentCandidateBinding>&,
			const ContextResource::FUeAgentOperationContext&,
			TSharedPtr<const Projection::FUeAgentProjectionSourceValue>& OutSource,
			BindingGateway::Query::FUeAgentQueryBindingError& OutError) override
		{
			OutSource.Reset();
			OutError.Code = TEXT("fixture_not_called");
			return false;
		}
	};

	class FRenamedCommandContribution final
		: public Module::CommandRegistration::Active::
			IUeAgentActiveCommandContribution
	{
	public:
		FRenamedCommandContribution(
			FString InContributionId,
			TSharedRef<Module::CommandRegistration::Active::
				IUeAgentActiveCommandContribution> InInner)
			: ContributionId(MoveTemp(InContributionId))
			, Inner(MoveTemp(InInner))
		{
		}

		const FString& GetContributionId() const override
		{
			return ContributionId;
		}

		void CollectDeclarations(
			TArray<CommandContract::FUeAgentCommandDeclaration>&
				OutDeclarations) const override
		{
			Inner->CollectDeclarations(OutDeclarations);
		}

		bool CreateHandlers(
			TSharedRef<const CommandRegistry::FUeAgentImmutableCommandRegistry>
				Registry,
			TArray<TSharedRef<CommandEdge::IUeAgentExactCommandHandler>>&
				OutHandlers,
			Contract::FDiagnostic& OutError) const override
		{
			return Inner->CreateHandlers(Registry, OutHandlers, OutError);
		}

	private:
		FString ContributionId;
		TSharedRef<Module::CommandRegistration::Active::
			IUeAgentActiveCommandContribution> Inner;
	};

	class FNeverCancelled final : public Contract::IUeAgentCancellationSignal
	{
	public:
		bool IsCancelled() const override { return false; }
	};

	class FInvocationPorts final
		: public BindingGateway::Operation::IOperationVersionFencePort
		, public BindingGateway::Operation::IOperationDomainMutationPort
		, public BindingGateway::Operation::IOperationStabilityPort
		, public BindingGateway::Operation::IOperationPersistencePort
		, public BindingGateway::Operation::IOperationIndependentVerificationPort
	{
	public:
		explicit FInvocationPorts(FString InInvocationId)
			: InvocationId(MoveTemp(InInvocationId))
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
				TEXT("mutation:") + InvocationId,
				{TEXT("target:") + InvocationId},
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
			OutReceipt = TEXT("receipt:") + InvocationId;
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
		FString InvocationId;
	};

	class FInvocationContext final : public IDomainOperationInvocationContext
	{
	public:
		explicit FInvocationContext(FString InInvocationId)
			: InvocationId(MoveTemp(InInvocationId))
		{
		}

		bool TryBuildSuccessPayload(
			const BindingGateway::Operation::FOperationLifecycleReceipt&,
			FString& OutPayloadJson,
			Contract::FDiagnostic& OutError) const override
		{
			OutError.Reset();
			OutPayloadJson = FString::Printf(
				TEXT("{\"invocation_id\":\"%s\"}"), *InvocationId);
			return true;
		}

	private:
		FString InvocationId;
	};

	class FInvocationFactory final : public IDomainOperationInvocationFactory
	{
	public:
		explicit FInvocationFactory(
			FString InOperationId = TEXT("fixture.operation"))
			: OperationId(MoveTemp(InOperationId))
		{
		}

		const FString& GetOperationId() const override
		{
			return OperationId;
		}

		bool TryCreate(
			const CommandEdge::FUeAgentCommandRequest& Request,
			const Contract::FUeAgentCallerContext&,
			const TSharedRef<Contract::IUeAgentCancellationSignal>&,
			FDomainOperationInvocation& OutInvocation,
			FDomainOperationInvocationError& OutError) const override
		{
			OutError = FDomainOperationInvocationError();
			const FString InvocationId = Request.GetRequestId();
			const FString Fingerprint = TEXT("sha256:")
				+ FString::ChrN(64, InvocationId.EndsWith(TEXT("a"))
					? TEXT('a') : TEXT('b'));
			const TSharedRef<FInvocationPorts> Ports =
				MakeShared<FInvocationPorts>(InvocationId);
			return FDomainOperationInvocation::TryCreate(
				MakeShared<BindingGateway::Operation::FOperationLifecyclePlan>(
					TEXT("plan:") + InvocationId,
					Fingerprint,
					TArray<FString>{TEXT("target:") + InvocationId},
					TMap<FString, FString>{
						{TEXT("target:") + InvocationId, TEXT("revision:1")}},
					TArray<FString>{TEXT("settled")}),
				MakeShared<BindingGateway::Operation::FOperationImpactAuthorization>(
					Fingerprint,
					TArray<FString>{TEXT("target:") + InvocationId}),
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
				MakeShared<FInvocationContext>(InvocationId),
				OutInvocation,
				OutError);
		}

	private:
		FString OperationId;
	};

	bool DecodeInvocationRequest(
		const FString& RequestId,
		TOptional<CommandEdge::FUeAgentCommandRequest>& OutRequest)
	{
		const FString Envelope = FString::Printf(
			TEXT("{\"schema\":\"uai.command.v1\",\"version\":1,")
			TEXT("\"message_kind\":\"command_request\",")
			TEXT("\"correlation_id\":\"corr-t65\",")
			TEXT("\"request_id\":\"%s\",")
			TEXT("\"command_id\":\"fixture.operation\",")
			TEXT("\"command_binding_hash\":\"sha256:%s\",\"params\":{}}"),
			*RequestId,
			*FString::ChrN(64, TEXT('c')));
		FTCHARToUTF8 Encoded(*Envelope);
		TArray<uint8> Bytes;
		Bytes.Append(
			reinterpret_cast<const uint8*>(Encoded.Get()), Encoded.Length());
		CommandEdge::FUeAgentCommandProtocolDiagnostic Diagnostic;
		return CommandEdge::FUeAgentCommandProtocolCodec().DecodeRequest(
			Bytes, OutRequest, Diagnostic);
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUeAgentDomainCapabilityContractRejectionTest,
	"UeAgentInterface.Core.DomainCapability.ContractRejection",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FUeAgentDomainCapabilityContractRejectionTest::RunTest(
	const FString& Parameters)
{
	Contract::FDiagnostic Error;
	TSharedPtr<const FDomainCapabilityContribution> Contribution;

	FDomainCapabilityDeclaration Surface =
		MakeExplicitlyUnsupported(TEXT("invalid-surface"));
	Surface.Surface.Support = EDomainCapabilitySupport::Supported;
	Surface.Surface.Surfaces.Add({
		TEXT("fixture.graph"),
		StateSemantic::EUeAgentSemanticSurfaceKind::Graph,
		FString(),
		TEXT("fixture-read-port"),
	});
	TestFalse(
		TEXT("surface without mechanics is rejected"),
		FDomainCapabilityContribution::TryCreate(
			MoveTemp(Surface), Contribution, Error));
	TestTrue(
		TEXT("surface failure is exact"),
		Error.Code == TEXT("domain_capability_surface_mechanics_missing")
			&& Error.PropertyPath == TEXT("surface.surfaces[0].mechanics_id") && Error.SubjectId == TEXT("fixture.graph"));

	FDomainCapabilityDeclaration Create =
		MakeDataTableContribution(TEXT("invalid-create"));
	Create.Initialization.Support = EDomainCapabilitySupport::Unsupported;
	Create.Initialization.ContractIds.Reset();
	TestFalse(
		TEXT("create route without initialization capability is rejected"),
		FDomainCapabilityContribution::TryCreate(
			MoveTemp(Create), Contribution, Error));
	TestTrue(
		TEXT("initialization failure is exact"),
		Error.Code == TEXT("domain_capability_create_initialization_missing")
			&& Error.PropertyPath == TEXT("hjson.routes[0].initialization_contract_id"));

	FDomainCapabilityDeclaration Write =
		MakeExplicitlyUnsupported(TEXT("invalid-write"));
	Write.Operation.Support = EDomainCapabilitySupport::Supported;
	FDomainOperationEntry WriteEntry;
	WriteEntry.OperationId = TEXT("fixture.write");
	WriteEntry.Kind = EDomainOperationKind::Write;
	Write.Operation.Operations.Add(MoveTemp(WriteEntry));
	TestFalse(
		TEXT("write route without terminal policy is rejected"),
		FDomainCapabilityContribution::TryCreate(
			MoveTemp(Write), Contribution, Error));
	TestTrue(
		TEXT("terminal failure is exact"),
		Error.Code == TEXT("domain_capability_write_terminal_policy_missing")
			&& Error.SubjectId == TEXT("fixture.write"));

	FDomainCapabilityDeclaration MissingFactory =
		MakeExplicitlyUnsupported(TEXT("invalid-operation-factory"));
	MissingFactory.Operation.Support = EDomainCapabilitySupport::Supported;
	FDomainOperationEntry MissingFactoryEntry;
	MissingFactoryEntry.OperationId = TEXT("fixture.operation");
	MissingFactoryEntry.TerminalPolicy = EDomainOperationTerminalPolicy::
		SucceededRejectedRolledBackOrStateUnknown;
	MissingFactoryEntry.ExecutionPolicy = EDomainOperationExecutionPolicy::Immediate;
	MissingFactoryEntry.TransientStatePolicy = EDomainTransientStatePolicy::None;
	MissingFactoryEntry.bPersists = true;
	MissingFactoryEntry.bIndependentReadback = true;
	MissingFactory.Operation.Operations.Add(MoveTemp(MissingFactoryEntry));
	TestFalse(
		TEXT("operation declaration without a request factory is rejected"),
		FDomainCapabilityContribution::TryCreate(
			MoveTemp(MissingFactory), Contribution, Error));
	TestTrue(
		TEXT("missing operation factory failure is exact"),
		Error.Code == TEXT("domain_capability_operation_factory_missing")
			&& Error.SubjectId == TEXT("fixture.operation"));

	FDomainCapabilityDeclaration MismatchedFactory =
		MakeExplicitlyUnsupported(TEXT("mismatched-operation-factory"));
	MismatchedFactory.Operation.Support = EDomainCapabilitySupport::Supported;
	FDomainOperationEntry MismatchedFactoryEntry;
	MismatchedFactoryEntry.OperationId = TEXT("fixture.operation");
	MismatchedFactoryEntry.TerminalPolicy = EDomainOperationTerminalPolicy::
		SucceededRejectedRolledBackOrStateUnknown;
	MismatchedFactoryEntry.ExecutionPolicy =
		EDomainOperationExecutionPolicy::Immediate;
	MismatchedFactoryEntry.TransientStatePolicy = EDomainTransientStatePolicy::None;
	MismatchedFactoryEntry.bPersists = true;
	MismatchedFactoryEntry.bIndependentReadback = true;
	MismatchedFactoryEntry.InvocationFactory =
		MakeShared<const FInvocationFactory>(TEXT("fixture.other"));
	MismatchedFactory.Operation.Operations.Add(MoveTemp(MismatchedFactoryEntry));
	TestFalse(
		TEXT("operation factory identity must match the declaration"),
		FDomainCapabilityContribution::TryCreate(
			MoveTemp(MismatchedFactory), Contribution, Error));
	TestTrue(
		TEXT("operation factory identity failure is exact"),
		Error.Code == TEXT("domain_capability_operation_factory_identity_mismatch")
			&& Error.ActualEvidence.IsValid() && Error.ActualEvidence->AsString() == TEXT("fixture.other") && Error.ExpectedEvidence.IsValid() && Error.ExpectedEvidence->AsString() == TEXT("fixture.operation"));

	FDomainCapabilityDeclaration Candidate =
		MakeExplicitlyUnsupported(TEXT("invalid-candidate"));
	Candidate.Discovery.Support = EDomainCapabilitySupport::Supported;
	Candidate.Discovery.Sources.Add({
		MakeShared<FDummyCatalogSource>(),
		TEXT("missing-read-port"),
	});
	TestFalse(
		TEXT("candidate source without direct read port is rejected"),
		FDomainCapabilityContribution::TryCreate(
			MoveTemp(Candidate), Contribution, Error));
	TestTrue(
		TEXT("candidate routing failure is exact"),
		Error.Code == TEXT("domain_capability_candidate_read_port_missing")
			&& Error.SubjectId == TEXT("missing-read-port"));

	FDomainCapabilityDeclaration AmbiguousRoute =
		MakeExplicitlyUnsupported(TEXT("ambiguous-composition-route"));
	AmbiguousRoute.Composition.Support = EDomainCapabilitySupport::Supported;
	HjsonWorkspace::FTargetCompositionContract RouteContract;
	RouteContract.SchemaId =
		HjsonWorkspace::GetTargetCompositionContractSchemaId();
	RouteContract.TargetKind = TEXT("ue.fixture.route");
	RouteContract.Surfaces = {
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
	RouteContract.SemanticRoutes = {
		{
			HjsonWorkspace::ECompositionSemanticRouteMatch::Suffix,
			TEXT(".node"),
			TEXT("graph_collection"),
			HjsonWorkspace::ESemanticUnitKind::ItemState,
			HjsonWorkspace::ECompositionSemanticRouteRole::Item,
			100,
		},
		{
			HjsonWorkspace::ECompositionSemanticRouteMatch::Contains,
			TEXT("node"),
			TEXT("graph_collection"),
			HjsonWorkspace::ESemanticUnitKind::ItemState,
			HjsonWorkspace::ECompositionSemanticRouteRole::Item,
			100,
		},
		{
			HjsonWorkspace::ECompositionSemanticRouteMatch::Default,
			FString(),
			TEXT("target_settings"),
			HjsonWorkspace::ESemanticUnitKind::PropertySet,
			HjsonWorkspace::ECompositionSemanticRouteRole::Property,
			0,
		},
	};
	AmbiguousRoute.Composition.Contracts.Add(MoveTemp(RouteContract));
	TestFalse(
		TEXT("same-priority semantic routes are rejected before projection"),
		FDomainCapabilityContribution::TryCreate(
			MoveTemp(AmbiguousRoute), Contribution, Error));
	TestTrue(
		TEXT("semantic route ambiguity failure is exact"),
		Error.Code == TEXT("target_composition_semantic_route_priority_duplicate")
			&& Error.CandidateName == TEXT("ue.fixture.route") && Error.PropertyPath == TEXT("/semantic_routes/1/priority") && Error.ActualValue.IsValid() && Error.ActualValue->AsNumber() == 100);

	FDomainCapabilityDeclaration DanglingRoute =
		MakeExplicitlyUnsupported(TEXT("dangling-composition-route"));
	DanglingRoute.Composition.Support = EDomainCapabilitySupport::Supported;
	HjsonWorkspace::FTargetCompositionContract DanglingContract;
	DanglingContract.SchemaId =
		HjsonWorkspace::GetTargetCompositionContractSchemaId();
	DanglingContract.TargetKind = TEXT("ue.fixture.dangling");
	DanglingContract.Surfaces = {
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
	DanglingContract.SemanticRoutes = {
		{
			HjsonWorkspace::ECompositionSemanticRouteMatch::Default,
			FString(),
			TEXT("missing_surface"),
			HjsonWorkspace::ESemanticUnitKind::PropertySet,
			HjsonWorkspace::ECompositionSemanticRouteRole::Property,
			0,
		},
	};
	DanglingRoute.Composition.Contracts.Add(MoveTemp(DanglingContract));
	TestFalse(
		TEXT("semantic routes cannot reference an undeclared surface"),
		FDomainCapabilityContribution::TryCreate(
			MoveTemp(DanglingRoute), Contribution, Error));
	TestTrue(
		TEXT("dangling semantic route failure is exact"),
		Error.Code == TEXT("target_composition_semantic_route_surface_missing")
			&& Error.CandidateName == TEXT("ue.fixture.dangling") && Error.PropertyPath == TEXT("/semantic_routes/0/surface_id") && Error.ActualValue.IsValid() && Error.ActualValue->AsString() == TEXT("missing_surface"));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUeAgentDomainCapabilityFreezeTest,
	"UeAgentInterface.Core.DomainCapability.CrossRegistryFreeze",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FUeAgentDomainCapabilityFreezeTest::RunTest(const FString& Parameters)
{
	Contract::FDiagnostic Error;
	TSharedPtr<const FDomainCapabilityContribution> Left;
	TSharedPtr<const FDomainCapabilityContribution> Right;
	TestTrue(
		TEXT("first intersecting contribution is locally valid"),
		FDomainCapabilityContribution::TryCreate(
			MakeDataTableContribution(TEXT("table-left")), Left, Error));
	TestTrue(
		TEXT("second intersecting contribution is locally valid"),
		FDomainCapabilityContribution::TryCreate(
			MakeDataTableContribution(TEXT("table-right")), Right, Error));

	FDomainCapabilityComposition Composition;
	TestTrue(TEXT("add first contribution"), Composition.TryAdd(Left.ToSharedRef(), Error));
	TestTrue(TEXT("add second contribution"), Composition.TryAdd(Right.ToSharedRef(), Error));
	TSharedPtr<const FFrozenDomainCapabilitySet> Frozen;
	TestFalse(
		TEXT("intersecting provider identities fail before projection"),
		Composition.TryFreeze(Frozen, Error));
	TestTrue(
		TEXT("cross registry collision is exact"),
		Error.Code == TEXT("domain_capability_hjson_provider_conflict")
			&& Error.ActualEvidence.IsValid() && Error.ActualEvidence->AsString() == TEXT("datatable-curvetable-domain-provider"));
	TestFalse(TEXT("failed freeze publishes no frozen set"), Frozen.IsValid());
	TestFalse(
		TEXT("a frozen composition rejects late registration"),
		Composition.TryAdd(Left.ToSharedRef(), Error));
	TestTrue(
		TEXT("late registration failure is exact"),
		Error.Code == TEXT("domain_capability_composition_sealed")
			&& Error.SubjectId == TEXT("table-left"));

	TSharedPtr<Module::CommandRegistration::Active::
		IUeAgentActiveCommandContribution> RealCommand;
	TestTrue(
		TEXT("create real command contribution for overlap oracle"),
		Module::CommandRegistration::UObjectDataAsset::
			TryCreateProductionUObjectDataAssetContribution(
				RealCommand,
				Error));
	TArray<CommandContract::FUeAgentCommandDeclaration> Commands;
	RealCommand->CollectDeclarations(Commands);
	TestTrue(TEXT("real command contribution is non-empty"), !Commands.IsEmpty());
	if (Commands.IsEmpty())
	{
		return false;
	}

	FDomainCapabilityDeclaration CommandLeft =
		MakeExplicitlyUnsupported(TEXT("command-left"));
	CommandLeft.Command.Support = EDomainCapabilitySupport::Supported;
	CommandLeft.Command.Contributions.Add(
		MakeShared<FRenamedCommandContribution>(
			TEXT("command-left-contribution"),
			RealCommand.ToSharedRef()));
	FDomainCapabilityDeclaration CommandRight =
		MakeExplicitlyUnsupported(TEXT("command-right"));
	CommandRight.Command.Support = EDomainCapabilitySupport::Supported;
	CommandRight.Command.Contributions.Add(
		MakeShared<FRenamedCommandContribution>(
			TEXT("command-right-contribution"),
			RealCommand.ToSharedRef()));
	TSharedPtr<const FDomainCapabilityContribution> CommandLeftFrozen;
	TSharedPtr<const FDomainCapabilityContribution> CommandRightFrozen;
	TestTrue(
		TEXT("left command declaration is locally valid"),
		FDomainCapabilityContribution::TryCreate(
			MoveTemp(CommandLeft), CommandLeftFrozen, Error));
	TestTrue(
		TEXT("right command declaration is locally valid"),
		FDomainCapabilityContribution::TryCreate(
			MoveTemp(CommandRight), CommandRightFrozen, Error));
	FDomainCapabilityComposition CommandComposition;
	TestTrue(
		TEXT("add left command declaration"),
		CommandComposition.TryAdd(CommandLeftFrozen.ToSharedRef(), Error));
	TestTrue(
		TEXT("add right command declaration"),
		CommandComposition.TryAdd(CommandRightFrozen.ToSharedRef(), Error));
	TestFalse(
		TEXT("different contributions cannot claim the same exact command"),
		CommandComposition.TryFreeze(Frozen, Error));
	TestTrue(
		TEXT("command identity collision is exact"),
		Error.Code == TEXT("domain_capability_command_id_conflict")
			&& Error.ActualValue.IsValid() && Error.ActualValue->AsString() == Commands[0].CommandId);
	TestFalse(TEXT("command collision publishes no frozen set"), Frozen.IsValid());
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUeAgentDomainCapabilityQueryDiscoveryProjectionTest,
	"UeAgentInterface.Core.DomainCapability.QueryDiscoveryProjection",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FUeAgentDomainCapabilityQueryDiscoveryProjectionTest::RunTest(
	const FString& Parameters)
{
	FDomainCapabilityDeclaration Declaration =
		MakeExplicitlyUnsupported(TEXT("fixture-query-discovery"));
	Declaration.Query.Support = EDomainCapabilitySupport::Supported;
	Declaration.Query.Providers.Emplace(
		TEXT("fixture-query-provider"),
		MakeShared<FDummyDomainQueryProvider>());
	const TSharedRef<FDummyReadPort> ReadPort = MakeShared<FDummyReadPort>();
	Declaration.Discovery.Support = EDomainCapabilitySupport::Supported;
	Declaration.Discovery.ReadPorts.Add(ReadPort);
	Declaration.Discovery.Sources.Add({
		MakeShared<FDummyCatalogSource>(),
		ReadPort->GetPortId(),
	});

	Contract::FDiagnostic Error;
	TSharedPtr<const FDomainCapabilityContribution> Contribution;
	TestTrue(
		TEXT("query and discovery declaration is locally valid"),
		FDomainCapabilityContribution::TryCreate(
			MoveTemp(Declaration),
			Contribution,
			Error));
	if (!Contribution.IsValid())
	{
		AddError(TEXT("projection declaration failed: ") + Error.ToPublicDetail());
		return false;
	}
	FDomainCapabilityComposition Composition;
	TestTrue(
		TEXT("add projection declaration"),
		Composition.TryAdd(Contribution.ToSharedRef(), Error));
	TSharedPtr<const FFrozenDomainCapabilitySet> Frozen;
	TestTrue(
		TEXT("freeze projection declaration"),
		Composition.TryFreeze(Frozen, Error));
	if (!Frozen.IsValid())
	{
		AddError(TEXT("projection freeze failed: ") + Error.ToPublicDetail());
		return false;
	}
	FDomainCapabilityAuthorities Authorities;
	TestTrue(
		TEXT("build query and discovery authorities"),
		Frozen->TryBuildAuthorities(Authorities, Error));
	TestTrue(
		TEXT("query provider registry is projected"),
		Authorities.GetQueryProviderRegistry().IsValid());
	TestTrue(
		TEXT("generic discovery service is projected"),
		Authorities.GetDiscoveryService().IsValid());
	if (Authorities.GetQueryProviderRegistry().IsValid())
	{
		TestTrue(
			TEXT("exact query provider binding is addressable"),
			Authorities.GetQueryProviderRegistry()->FindExact(
				TEXT("fixture-query-provider")).IsValid());
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUeAgentDomainCapabilityExistingAuthorityProjectionTest,
	"UeAgentInterface.Core.DomainCapability.ExistingAuthorityProjection",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FUeAgentDomainCapabilityExistingAuthorityProjectionTest::RunTest(
	const FString& Parameters)
{
	Contract::FDiagnostic Error;
	TSharedPtr<const FFrozenDomainCapabilitySet> Frozen;
	const bool bProcessFrozen = TryFreezeProcessDomainCapabilities(
		Frozen,
		Error);
	TestTrue(
		TEXT("freeze self-registered representative domains without a central list"),
		bProcessFrozen);
	if (!bProcessFrozen)
	{
		AddError(TEXT("process domain capability freeze failed: ") + Error.ToPublicDetail());
	}
	TestTrue(TEXT("representative frozen set returned"), Frozen.IsValid());
	if (!Frozen.IsValid())
	{
		return false;
	}
	TSet<FString> DomainIds;
	int32 DeclaredCommandCount = 0;
	int32 DeclaredHjsonProviderCount = 0;
	for (const TSharedRef<const FDomainCapabilityContribution>& Contribution :
		Frozen->GetContributions())
	{
		DomainIds.Add(Contribution->GetDomainId());
		for (const TSharedRef<Module::CommandRegistration::Active::
			IUeAgentActiveCommandContribution>& CommandContribution :
			Contribution->GetDeclaration().Command.Contributions)
		{
			TArray<CommandContract::FUeAgentCommandDeclaration> Declarations;
			CommandContribution->CollectDeclarations(Declarations);
			DeclaredCommandCount += Declarations.Num();
		}
		DeclaredHjsonProviderCount +=
			Contribution->GetDeclaration().Hjson.Providers.Num();
	}
	TestTrue(TEXT("T13 self-registers"), DomainIds.Contains(TEXT("uobject-dataasset")));
	TestTrue(TEXT("T27 self-registers"), DomainIds.Contains(TEXT("datatable-curvetable")));
	TestTrue(TEXT("T30 self-registers"), DomainIds.Contains(TEXT("localization-string-table")));
	TestTrue(
		TEXT("a command-only domain is owned by the frozen domain catalog"),
		DomainIds.Contains(TEXT("query")));
	TestTrue(
		TEXT("a paired command and HJSON domain is owned atomically"),
		DomainIds.Contains(TEXT("niagara-domain-candidates")));
	const TSharedRef<const FDomainCapabilityContribution>* Niagara =
		Frozen->GetContributions().FindByPredicate(
			[](const TSharedRef<const FDomainCapabilityContribution>& Item)
			{
				return Item->GetDomainId()
					== TEXT("niagara-domain-candidates");
			});
	TestNotNull(TEXT("the paired Niagara domain is addressable"), Niagara);
	if (Niagara)
	{
		TestEqual(
			TEXT("the paired Niagara domain owns its command contribution"),
			(*Niagara)->GetDeclaration().Command.Contributions.Num(),
			1);
		TestEqual(
			TEXT("the paired Niagara domain owns its HJSON provider"),
			(*Niagara)->GetDeclaration().Hjson.Providers.Num(),
			1);
	}
	const TSharedRef<const FDomainCapabilityContribution>* T13 =
		Frozen->GetContributions().FindByPredicate(
			[](const TSharedRef<const FDomainCapabilityContribution>& Item)
			{
				return Item->GetDomainId() == TEXT("uobject-dataasset");
			});
	TestNotNull(TEXT("T13 frozen contribution is addressable"), T13);
	if (T13)
	{
		TestEqual(
			TEXT("T13 declares its real HJSON provider"),
			(*T13)->GetDeclaration().Hjson.Providers.Num(),
			1);
		TestEqual(
			TEXT("T13 declares its exact public HJSON route"),
			(*T13)->GetDeclaration().Hjson.Routes[0].CommandId,
			FString(TEXT("content_graph.import_workspace")));
		TestEqual(
			TEXT("T13 declares its root initialization contract"),
			(*T13)->GetDeclaration().Hjson.Routes[0].InitializationContractId,
			FString(TEXT("uai.init.uobject_dataasset.root/1")));
	}

	FDomainCapabilityAuthorities Authorities;
	TestTrue(
		TEXT("build one process authority from the frozen domain catalog"),
		TryGetOrBuildProcessDomainCapabilityAuthorities(Authorities, Error));
	TestTrue(
		TEXT("T13 projected to immutable command registry"),
		Authorities.GetCommandRegistry().IsValid());
	TestTrue(
		TEXT("T13 projected to exact command dispatch"),
		Authorities.GetCommandDispatch().IsValid());
	TestTrue(
		TEXT("T27 and T30 projected to HJSON provider registry"),
		Authorities.GetHjsonRegistry().IsValid());
	TestTrue(
		TEXT("T35 projected unified content discovery authority"),
		Authorities.GetContentDiscoveryService().IsValid());
	if (Authorities.GetContentDiscoveryService().IsValid())
	{
		TestEqual(
			TEXT("T35 publishes the two current content providers"),
			Authorities.GetContentDiscoveryService()->GetProviderIds().Num(),
			2);
	}
	TSharedPtr<const CommandRegistry::FUeAgentImmutableCommandRegistry>
		ProcessCommandRegistry;
	TSharedPtr<const CommandEdge::FUeAgentExactCommandDispatch>
		ProcessCommandDispatch;
	TestTrue(
		TEXT("compose the process command authority for projection comparison"),
		Module::CommandRegistration::Active::
			TryComposeProcessActiveCommandComposition(
				ProcessCommandRegistry,
				ProcessCommandDispatch,
				Error));
	TestTrue(
		TEXT("domain activation returns the identical process command authority"),
		ProcessCommandRegistry.IsValid()
			&& Authorities.GetCommandRegistry().IsValid()
			&& ProcessCommandRegistry.Get()
				== Authorities.GetCommandRegistry().Get());
	TestEqual(
		TEXT("the command authority contains exactly the domain-declared commands"),
		Authorities.GetCommandRegistry()->GetContracts().Num(),
		DeclaredCommandCount);
	TSharedPtr<const HjsonWorkspace::
		FImportInitializationDomainProviderRegistry> ProcessHjsonRegistry;
	HjsonWorkspace::FWorkspaceError WorkspaceError;
	TestTrue(
		TEXT("freeze the process HJSON authority for projection comparison"),
		HjsonWorkspace::TryFreezeProcessImportInitializationDomainProviders(
			ProcessHjsonRegistry,
			WorkspaceError));
	TestTrue(
		TEXT("domain activation returns the identical process HJSON authority"),
		ProcessHjsonRegistry.IsValid()
			&& Authorities.GetHjsonRegistry().IsValid()
			&& ProcessHjsonRegistry.Get()
				== Authorities.GetHjsonRegistry().Get());
	if (Authorities.GetHjsonRegistry().IsValid())
	{
		TestEqual(
			TEXT("the HJSON authority contains exactly the domain-declared providers"),
			Authorities.GetHjsonRegistry()->Num(),
			DeclaredHjsonProviderCount);
	}
	TSharedPtr<Module::CommandRegistration::Active::
		IUeAgentActiveCommandContribution> ExactContribution;
	TestTrue(
		TEXT("create one exact contribution for the no-fallback oracle"),
		Module::CommandRegistration::UObjectDataAsset::
			TryCreateProductionUObjectDataAssetContribution(
				ExactContribution,
				Error));
	if (!ExactContribution.IsValid())
	{
		return false;
	}
	TArray<CommandContract::FUeAgentCommandDeclaration> ExactDeclarations;
	ExactContribution->CollectDeclarations(ExactDeclarations);
	TSharedPtr<const CommandRegistry::FUeAgentImmutableCommandRegistry>
		ExactRegistry;
	TSharedPtr<const CommandEdge::FUeAgentExactCommandDispatch> ExactDispatch;
	TestTrue(
		TEXT("the pure composer accepts only the exact supplied contribution"),
		Module::CommandRegistration::Active::
			TryComposeActiveCommandContributions(
				{ExactContribution.ToSharedRef()},
				ExactRegistry,
				ExactDispatch,
				Error));
	TestEqual(
		TEXT("the pure composer cannot refill omitted process contributions"),
		ExactRegistry->GetContracts().Num(),
		ExactDeclarations.Num());
	FDomainCapabilityDeclaration DeclaredRoute =
		MakeExplicitlyUnsupported(TEXT("fixture-declared-route"));
	DeclaredRoute.Composition.Support = EDomainCapabilitySupport::Supported;
	HjsonWorkspace::FTargetCompositionContract RouteContract;
	RouteContract.SchemaId =
		HjsonWorkspace::GetTargetCompositionContractSchemaId();
	RouteContract.TargetKind = TEXT("ue.fixture.declared_route");
	RouteContract.Surfaces = {
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
	RouteContract.SemanticRoutes = {
		{
			HjsonWorkspace::ECompositionSemanticRouteMatch::Suffix,
			TEXT(".node"),
			TEXT("graph_collection"),
			HjsonWorkspace::ESemanticUnitKind::ItemState,
			HjsonWorkspace::ECompositionSemanticRouteRole::Item,
			100,
		},
		{
			HjsonWorkspace::ECompositionSemanticRouteMatch::Default,
			FString(),
			TEXT("target_settings"),
			HjsonWorkspace::ESemanticUnitKind::PropertySet,
			HjsonWorkspace::ECompositionSemanticRouteRole::Property,
			0,
		},
	};
	DeclaredRoute.Composition.Contracts.Add(MoveTemp(RouteContract));
	TSharedPtr<const FDomainCapabilityContribution> RouteContribution;
	TestTrue(
		TEXT("a domain-owned semantic route contribution is locally valid"),
		FDomainCapabilityContribution::TryCreate(
			MoveTemp(DeclaredRoute), RouteContribution, Error));
	FDomainCapabilityComposition RouteComposition;
	TestTrue(
		TEXT("add the domain-owned semantic route"),
		RouteComposition.TryAdd(RouteContribution.ToSharedRef(), Error));
	TSharedPtr<const FFrozenDomainCapabilitySet> RouteFrozen;
	TestTrue(
		TEXT("freeze the domain-owned semantic route"),
		RouteComposition.TryFreeze(RouteFrozen, Error));
	FDomainCapabilityAuthorities RouteAuthorities;
	TestTrue(
		TEXT("project the route into the existing target composition authority"),
		RouteFrozen->TryBuildAuthorities(RouteAuthorities, Error));
	HjsonWorkspace::FCompositionSemanticRouteContract ResolvedRoute;
	TestTrue(
		TEXT("the exact target resolves its specific suffix route"),
		HjsonWorkspace::TryResolveTargetCompositionSemanticRoute(
			TEXT("ue.fixture.declared_route"),
			TEXT("uai.fixture.node"),
			ResolvedRoute));
	TestEqual(
		TEXT("the specific route targets the declared graph surface"),
		ResolvedRoute.SurfaceId,
		FString(TEXT("graph_collection")));
	TestTrue(
		TEXT("an unmatched semantic ID resolves the declared default"),
		HjsonWorkspace::TryResolveTargetCompositionSemanticRoute(
			TEXT("ue.fixture.declared_route"),
			TEXT("uai.fixture.other"),
			ResolvedRoute));
	TestEqual(
		TEXT("the default targets the declared property surface"),
		ResolvedRoute.SurfaceId,
		FString(TEXT("target_settings")));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUeAgentDomainCapabilityRequestScopedOperationFactoryTest,
	"UeAgentInterface.Core.DomainCapability.RequestScopedOperationFactory",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FUeAgentDomainCapabilityRequestScopedOperationFactoryTest::RunTest(
	const FString& Parameters)
{
	const TSharedRef<const FInvocationFactory> Factory =
		MakeShared<const FInvocationFactory>();
	FDomainCapabilityDeclaration Declaration =
		MakeExplicitlyUnsupported(TEXT("fixture-operation-domain"));
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
	Entry.InvocationFactory = Factory;
	Declaration.Operation.Operations.Add(MoveTemp(Entry));

	Contract::FDiagnostic Error;
	TSharedPtr<const FDomainCapabilityContribution> Contribution;
	TestTrue(
		TEXT("a stateless request-scoped operation factory is accepted"),
		FDomainCapabilityContribution::TryCreate(
			MoveTemp(Declaration), Contribution, Error));
	TestTrue(TEXT("operation contribution is available"), Contribution.IsValid());
	if (!Contribution.IsValid())
	{
		return false;
	}

	FDomainCapabilityComposition Composition;
	TestTrue(
		TEXT("add request-scoped operation contribution"),
		Composition.TryAdd(Contribution.ToSharedRef(), Error));
	TSharedPtr<const FFrozenDomainCapabilitySet> Frozen;
	TestTrue(TEXT("freeze operation contribution"),
		Composition.TryFreeze(Frozen, Error));
	const TSharedPtr<const IDomainOperationInvocationFactory> Projected =
		Frozen->FindOperationFactory(TEXT("fixture.operation"));
	TestTrue(TEXT("the exact operation factory is retained"), Projected.IsValid());
	if (!Projected.IsValid())
	{
		return false;
	}
	TestTrue(
		TEXT("the frozen contribution preserves the same factory owner"),
		Projected.Get() == &Factory.Get());

	TOptional<CommandEdge::FUeAgentCommandRequest> RequestA;
	TOptional<CommandEdge::FUeAgentCommandRequest> RequestB;
	TestTrue(TEXT("decode first request"),
		DecodeInvocationRequest(TEXT("request-a"), RequestA));
	TestTrue(TEXT("decode second request"),
		DecodeInvocationRequest(TEXT("request-b"), RequestB));
	if (!RequestA.IsSet() || !RequestB.IsSet())
	{
		return false;
	}
	const Contract::FUeAgentCallerContext Caller(
		TEXT("t65-caller"), {TEXT("uai:operate")}, TEXT("corr-t65"));
	const TSharedRef<Contract::IUeAgentCancellationSignal> Cancellation =
		MakeShared<FNeverCancelled>();
	FDomainOperationInvocation InvocationA;
	FDomainOperationInvocation InvocationB;
	FDomainOperationInvocationError InvocationError;
	TestTrue(TEXT("create first request-scoped invocation"),
		Projected->TryCreate(
			RequestA.GetValue(), Caller, Cancellation,
			InvocationA, InvocationError));
	TestTrue(TEXT("create second request-scoped invocation"),
		Projected->TryCreate(
			RequestB.GetValue(), Caller, Cancellation,
			InvocationB, InvocationError));
	TestTrue(TEXT("both invocations are valid"),
		InvocationA.IsValid() && InvocationB.IsValid());
	TestNotEqual(
		TEXT("each request receives an independent lifecycle service"),
		&InvocationA.GetService().Get(),
		&InvocationB.GetService().Get());
	TestNotEqual(
		TEXT("each request receives an independent result context"),
		&InvocationA.GetContext().Get(),
		&InvocationB.GetContext().Get());
	TestNotEqual(
		TEXT("each request receives an independent plan"),
		InvocationA.GetPlan()->GetPlanId(),
		InvocationB.GetPlan()->GetPlanId());
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUeAgentDomainCapabilityOperationDurabilityPolicyTest,
	"UeAgentInterface.Core.DomainCapability.OperationDurabilityPolicy",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FUeAgentDomainCapabilityOperationDurabilityPolicyTest::RunTest(
	const FString& Parameters)
{
	auto MakeOperationDeclaration = [](
		const FString& DomainId,
		const FString& OperationId,
		const EDomainTransientStatePolicy TransientStatePolicy,
		const bool bPersists,
		const bool bIndependentReadback)
	{
		FDomainCapabilityDeclaration Declaration =
			MakeExplicitlyUnsupported(DomainId);
		Declaration.Operation.Support = EDomainCapabilitySupport::Supported;
		FDomainOperationEntry Entry;
		Entry.OperationId = OperationId;
		Entry.Kind = EDomainOperationKind::Write;
		Entry.TerminalPolicy = EDomainOperationTerminalPolicy::
			SucceededRejectedRolledBackOrStateUnknown;
		Entry.ExecutionPolicy = EDomainOperationExecutionPolicy::Immediate;
		Entry.TransientStatePolicy = TransientStatePolicy;
		Entry.bPersists = bPersists;
		Entry.bIndependentReadback = bIndependentReadback;
		Entry.InvocationFactory = MakeShared<const FInvocationFactory>(OperationId);
		Declaration.Operation.Operations.Add(MoveTemp(Entry));
		return Declaration;
	};

	Contract::FDiagnostic Error;
	TSharedPtr<const FDomainCapabilityContribution> Contribution;
	TestTrue(
		TEXT("a recoverable session operation is valid without durable persistence"),
		FDomainCapabilityContribution::TryCreate(
			MakeOperationDeclaration(
				TEXT("fixture-session-domain"),
				TEXT("fixture.session.probe"),
				EDomainTransientStatePolicy::RecoverableSession,
				false,
				true),
			Contribution,
			Error));

	Contribution.Reset();
	Error.Reset();
	TestFalse(
		TEXT("a persistent operation cannot omit persistence"),
		FDomainCapabilityContribution::TryCreate(
			MakeOperationDeclaration(
				TEXT("fixture-persistent-domain"),
				TEXT("fixture.asset.update"),
				EDomainTransientStatePolicy::None,
				false,
				true),
			Contribution,
			Error));
	TestTrue(
		TEXT("persistent omission has a stable diagnostic"),
		Error.Code == TEXT("domain_capability_write_persistence_missing")
			&& Error.SubjectId == TEXT("fixture.asset.update"));

	Contribution.Reset();
	Error.Reset();
	TestFalse(
		TEXT("a recoverable session cannot claim durable persistence"),
		FDomainCapabilityContribution::TryCreate(
			MakeOperationDeclaration(
				TEXT("fixture-conflict-domain"),
				TEXT("fixture.session.conflict"),
				EDomainTransientStatePolicy::RecoverableSession,
				true,
				true),
			Contribution,
			Error));
	TestTrue(
		TEXT("session persistence conflicts have a stable diagnostic"),
		Error.Code == TEXT("domain_capability_transient_persistence_conflict")
			&& Error.SubjectId == TEXT("fixture.session.conflict"));

	Contribution.Reset();
	Error.Reset();
	TestFalse(
		TEXT("every operation still requires independent readback or observation"),
		FDomainCapabilityContribution::TryCreate(
			MakeOperationDeclaration(
				TEXT("fixture-observation-domain"),
				TEXT("fixture.session.unobserved"),
				EDomainTransientStatePolicy::RecoverableSession,
				false,
				false),
			Contribution,
			Error));
	TestTrue(
		TEXT("missing independent observation keeps the existing diagnostic"),
		Error.Code == TEXT("domain_capability_write_readback_missing")
			&& Error.SubjectId == TEXT("fixture.session.unobserved"));
	return true;
}
