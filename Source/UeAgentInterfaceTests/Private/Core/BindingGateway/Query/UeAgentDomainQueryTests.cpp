#include "Misc/AutomationTest.h"

#include "Core/BindingGateway/Query/UeAgentDomainQuery.h"
#include "Core/BindingGateway/Query/UeAgentQueryBinding.h"
#include "Core/ContextResource/UeAgentOperationContext.h"
#include "Core/Contract/UeAgentContractSchema.h"
#include "Core/Contract/UeAgentDescriptorRegistry.h"
#include "Core/Discovery/UeAgentDiscovery.h"
#include "Core/Projection/UeAgentProjection.h"
#include "Core/StateSemantic/UeAgentStateSemantic.h"

namespace
{
	using namespace UE::UAI::BindingGateway::Query;
	using namespace UE::UAI::ContextResource;
	using namespace UE::UAI::Contract;
	using namespace UE::UAI::Discovery;
	using namespace UE::UAI::Projection;
	using namespace UE::UAI::StateSemantic;

	FUeAgentDescriptorIdentity DomainDescriptorIdentity()
	{
		return FUeAgentDescriptorIdentity(
			TEXT("fixture.domain_query.world_read"),
			1,
			TEXT(
				"sha256:bce3a8d3db40ad79ddba82c3e8a21244"
				"e71478751016f8c88112d15b87c621e1"));
	}

	FUeAgentOperationDescriptor DomainDescriptor()
	{
		return FUeAgentOperationDescriptor(
			DomainDescriptorIdentity(),
			TEXT("world.read"),
			TEXT("world"),
			EUeAgentDescriptorOperationKind::Query,
			FUeAgentExecutionContextPolicy(
				{EUeAgentStatePlane::AuthoringMemory},
				{
					EUeAgentExecutionContextFacet::World,
					EUeAgentExecutionContextFacet::Viewport,
					EUeAgentExecutionContextFacet::Selection,
				},
				EUeAgentContextResolutionPolicy::ExplicitOnly,
				EUeAgentTransientContextPolicy::ReadOnly),
			FUeAgentContractValue(TEXT(
				"{\"additionalProperties\":false,"
				"\"properties\":{"
					"\"filter\":{\"items\":{\"type\":\"string\"},"
						"\"maxItems\":4,\"type\":\"array\"},"
					"\"mode\":{\"enum\":[\"path\",\"ray\"],"
						"\"type\":\"string\"},"
					"\"path\":{\"minLength\":1,\"type\":\"string\"}},"
				"\"required\":[\"mode\",\"path\"],"
				"\"type\":\"object\"}")),
			FUeAgentContractValue(TEXT("{\"type\":\"object\"}")),
			UE::UAI::Contract::FUeAgentQueryBinding(
				TEXT("world.query.provider"),
				EUeAgentQueryKind::RuntimeProbe,
				EUeAgentQueryMaterializationPolicy::RuntimeObserve,
				EUeAgentQuerySideEffectPolicy::NoPersistentMutation));
	}

	TSharedPtr<const FUeAgentDescriptorRegistry> BuildDescriptorRegistry(
		FAutomationTestBase* Test)
	{
		TSharedPtr<const FUeAgentDescriptorRegistry> Registry;
		FString Error;
		Test->TestTrue(
			TEXT("create C0 descriptor registry"),
			FUeAgentDescriptorRegistry::TryCreate(
				{DomainDescriptor()},
				Registry,
				Error));
		return Registry;
	}

	FUeAgentQuerySessionIdentity DomainSession(
		const FUeAgentDescriptorRegistry* Registry)
	{
		check(Registry);
		return FUeAgentQuerySessionIdentity(
			TEXT("fixture-server-session"),
			7,
			Registry->GetRegistryHash());
	}

	FUeAgentCallerContext DomainCaller(
		FString CallerId = TEXT("fixture-agent"))
	{
		return FUeAgentCallerContext(
			MoveTemp(CallerId),
			{TEXT("world:read")},
			TEXT("corr-domain-query"));
	}

	FUeAgentExecutionContextInput DomainExecutionContext(
		const bool bIncludeViewport = true)
	{
		FUeAgentExecutionContextInput Input;
		Input.ProjectInstanceId = TEXT("fixture-project");
		Input.EditorInstanceId = TEXT("fixture-editor");
		Input.StatePlane = EUeAgentStatePlane::AuthoringMemory;
		Input.WorldType = EUeAgentExecutionWorldType::Editor;
		Input.WorldIdentity = TEXT("/Game/Maps/TestMap.TestMap");
		Input.ViewportIdentity =
			bIncludeViewport ? TEXT("LevelEditorViewport0") : TEXT("");
		Input.SelectionIdentities = {
			TEXT("/Game/Maps/TestMap.TestMap:PersistentLevel.Actor_1"),
		};
		Input.ContextRevision = TEXT("world-revision-17");
		return Input;
	}

	FUeAgentProjectionRequest DomainProjection()
	{
		return FUeAgentProjectionRequest(
			FUeAgentProjectionProfile(
				TEXT("compact"),
				false,
				false,
				false),
			FUeAgentPageRequest(),
			{},
			{},
			{});
	}

	FUeAgentResourceTrackerOwner DomainResources()
	{
		return MakeShared<
			FUeAgentResourceTracker,
			ESPMode::ThreadSafe>(
			FUeAgentResourceBudget({
				{EUeAgentResourceDimension::InputBytes, 1024 * 1024},
				{EUeAgentResourceDimension::OutputBytes, 1024 * 1024},
				{EUeAgentResourceDimension::Nodes, 100000},
				{EUeAgentResourceDimension::Items, 100000},
				{EUeAgentResourceDimension::StringBytes, 1024 * 1024},
				{EUeAgentResourceDimension::WorkUnits, 100000},
				{
					EUeAgentResourceDimension::EstimatedMemoryBytes,
					8 * 1024 * 1024,
				},
			}));
	}

	FUeAgentOperationDeadline DomainDeadline()
	{
		return FUeAgentOperationDeadline::FromTimeout(
			30.0,
			[]() { return 100.0; });
	}

	FUeAgentCandidateBinding DomainTarget()
	{
		return FUeAgentCandidateBinding(
			TEXT("fixture-domain-target"),
			FUeAgentCandidateSummary(
				FUeAgentCanonicalIdentity(
					TEXT("world.actor:/Game/Maps/TestMap.Actor_1"),
					TEXT("actor"),
					TEXT("/Game/Maps/TestMap"),
					TEXT("Actor_1"),
					TEXT("target"),
					TEXT("17")),
				DomainDescriptorIdentity(),
				TEXT("world_actor"),
				TEXT("Actor_1"),
				TEXT("Fixture domain target"),
				{TEXT("World"), TEXT("Actor")}),
			FUeAgentCatalogRevision(
				TEXT("fixture-world-catalog"),
				TEXT("17"),
				17),
			TEXT("uai://world/actor/Actor_1"),
			TEXT("world-read"),
			{EUeAgentCandidateOperation::Read},
			TEXT("fixture-domain-query-test"));
	}

	class FDomainTokenEstimator final : public IUeAgentTokenEstimator
	{
	public:
		virtual bool TryEstimate(
			const FUeAgentProjectedPayload& Payload,
			const TArray<FUeAgentArtifactDescriptor>& Artifacts,
			const TArray<FUeAgentProjectionDiagnostic>& Diagnostics,
			FUeAgentResourceTracker& Resources,
			const FUeAgentOperationDeadline& Deadline,
			const FUeAgentCancellationToken& Cancellation,
			FUeAgentTokenEstimate& OutEstimate,
			FUeAgentProjectionError& OutError) override
		{
			++Calls;
			LastResources = &Resources;
			OutEstimate = FUeAgentTokenEstimate(
				EUeAgentTokenEstimateKind::Exact,
				8,
				16);
			return true;
		}

		int32 Calls = 0;
		FUeAgentResourceTracker* LastResources = nullptr;
	};

	class FDomainSessionSource final : public IUeAgentQuerySessionSource
	{
	public:
		explicit FDomainSessionSource(
			FUeAgentQuerySessionIdentity InSession)
			: Session(MoveTemp(InSession))
		{
		}

		virtual FUeAgentQuerySessionIdentity GetCurrent() const override
		{
			return Session;
		}

	private:
		FUeAgentQuerySessionIdentity Session;
	};

	class FDomainProvider final : public IUeAgentDomainQueryProvider
	{
	public:
		explicit FDomainProvider(
			TSharedRef<const FUeAgentProjectionSourceValue> InSource)
			: Source(MoveTemp(InSource))
		{
		}

		virtual bool TryQuery(
			const FUeAgentQueryDescriptor& Descriptor,
			const FUeAgentContractValue& CanonicalInput,
			const TOptional<FUeAgentCandidateBinding>& Target,
			const FUeAgentOperationContext& Context,
			TSharedPtr<const FUeAgentProjectionSourceValue>& OutSource,
			FUeAgentQueryBindingError& OutError) override
		{
			++Calls;
			LastDescriptorId = Descriptor.GetIdentity().GetDescriptorId();
			LastInput = CanonicalInput.GetJsonValue();
			LastTarget = Target;
			LastRequestId = Context.GetRequestId();
			LastCallerId = Context.GetCallerContext().GetCallerId();
			LastWorld = Context.GetExecutionContext().GetWorldIdentity();
			LastViewport =
				Context.GetExecutionContext().GetViewportIdentity();
			LastResources = &Context.GetResourceTracker();
			LastDeadlineAt = Context.GetDeadline().GetDeadlineAt();
			bObservedCancellation =
				Context.GetCancellationToken().IsCancelled();
			if (bFail)
			{
				OutError.Code = FailureCode;
				OutError.SubjectId = Descriptor.GetProviderBinding();
				OutError.Detail = TEXT("fixture provider failure");
				return false;
			}
			OutSource = Source;
			return true;
		}

		bool bFail = false;
		FString FailureCode = TEXT("fixture_provider_failed");
		int32 Calls = 0;
		FString LastDescriptorId;
		FString LastInput;
		TOptional<FUeAgentCandidateBinding> LastTarget;
		FString LastRequestId;
		FString LastCallerId;
		FString LastWorld;
		FString LastViewport;
		FUeAgentResourceTracker* LastResources = nullptr;
		double LastDeadlineAt = -1.0;
		bool bObservedCancellation = false;

	private:
		TSharedRef<const FUeAgentProjectionSourceValue> Source;
	};

	TSharedPtr<const FUeAgentProjectionSourceValue> BuildProjectionSource(
		FAutomationTestBase* Test,
		FUeAgentResourceTracker& Resources)
	{
		FUeAgentCanonicalValuePtr Value;
		FUeAgentStateSemanticError Error;
		Test->TestTrue(
			TEXT("create typed C4 provider value"),
			FUeAgentCanonicalValue::TryString(
				TEXT("domain-result"),
				Resources,
				Value,
				Error));
		if (!Value.IsValid())
		{
			return nullptr;
		}
		return MakeShared<FUeAgentProjectionSourceValue>(
			Value.ToSharedRef());
	}

	struct FDomainBindingGraph
	{
		TSharedPtr<const FUeAgentDescriptorRegistry> DescriptorRegistry;
		TSharedPtr<const FUeAgentDomainQueryProviderRegistry>
			ProviderRegistry;
		TSharedRef<FDomainProvider> Primary;
		TSharedRef<FDomainProvider> Fallback;
		TSharedRef<FDomainTokenEstimator> Estimator;
		TSharedPtr<const FUeAgentQueryTicketService> Tickets;
		TSharedPtr<
			const UE::UAI::BindingGateway::Query::
				FUeAgentQueryBinding> Binding;
	};

	TOptional<FDomainBindingGraph> BuildDomainBindingGraph(
		FAutomationTestBase* Test,
		const bool bRegisterPrimary = true)
	{
		const TSharedPtr<const FUeAgentDescriptorRegistry> Descriptors =
			BuildDescriptorRegistry(Test);
		if (!Descriptors.IsValid())
		{
			return {};
		}

		const FUeAgentResourceTrackerOwner BuildResources =
			DomainResources();
		const TSharedPtr<const FUeAgentProjectionSourceValue> Source =
			BuildProjectionSource(Test, BuildResources.Get());
		if (!Source.IsValid())
		{
			return {};
		}
		const TSharedRef<FDomainProvider> Primary =
			MakeShared<FDomainProvider>(Source.ToSharedRef());
		const TSharedRef<FDomainProvider> Fallback =
			MakeShared<FDomainProvider>(Source.ToSharedRef());

		TArray<FUeAgentDomainQueryProviderRegistration> Registrations;
		if (bRegisterPrimary)
		{
			Registrations.Emplace(
				TEXT("world.query.provider"),
				Primary);
		}
		Registrations.Emplace(
			TEXT("world.query.fallback"),
			Fallback);
		TSharedPtr<const FUeAgentDomainQueryProviderRegistry> Providers;
		FUeAgentQueryBindingError QueryError;
		Test->TestTrue(
			TEXT("create immutable C3Q provider registry"),
			FUeAgentDomainQueryProviderRegistry::TryCreate(
				MoveTemp(Registrations),
				Providers,
				QueryError));

		TSharedPtr<const FUeAgentDiscoveryService> Discovery;
		FUeAgentDiscoveryError DiscoveryError;
		Test->TestTrue(
			TEXT("create empty C2 service for domain-only binding"),
			FUeAgentDiscoveryService::TryCreate(
				{},
				{},
				Discovery,
				DiscoveryError));

		const TSharedRef<FDomainTokenEstimator> Estimator =
			MakeShared<FDomainTokenEstimator>();
		TSharedPtr<const FUeAgentResponseProjector> Projector;
		FUeAgentProjectionError ProjectionError;
		Test->TestTrue(
			TEXT("create C10 projector"),
			FUeAgentResponseProjector::TryCreate(
				Estimator,
				Projector,
				ProjectionError));

		const TSharedRef<FDomainSessionSource> Sessions =
			MakeShared<FDomainSessionSource>(
				DomainSession(Descriptors.Get()));
		TSharedPtr<const FUeAgentQueryTicketService> Tickets;
		Test->TestTrue(
			TEXT("create shared C3 ticket view"),
			FUeAgentQueryTicketService::TryCreate(
				TEXT("fixture-domain-ticket-key"),
				30.0,
				[]() { return 100.0; },
				[]() { return FString(TEXT("fixture-domain-ticket")); },
				Tickets,
				QueryError));

		TSharedPtr<
			const UE::UAI::BindingGateway::Query::
				FUeAgentQueryBinding> Binding;
		Test->TestTrue(
			TEXT("create C3Q with C0 descriptors and exact providers"),
			UE::UAI::BindingGateway::Query::
				FUeAgentQueryBinding::TryCreate(
				Descriptors.ToSharedRef(),
				Providers.ToSharedRef(),
				Discovery.ToSharedRef(),
				Projector.ToSharedRef(),
				Sessions,
				Tickets.ToSharedRef(),
				Binding,
				QueryError));
		if (!Providers.IsValid()
			|| !Discovery.IsValid()
			|| !Projector.IsValid()
			|| !Tickets.IsValid()
			|| !Binding.IsValid())
		{
			return {};
		}
		return FDomainBindingGraph{
			Descriptors,
			Providers,
			Primary,
			Fallback,
			Estimator,
			Tickets,
			Binding,
		};
	}

	FUeAgentDomainQuery DomainPayload(
		const FUeAgentDescriptorRegistry* Registry,
		FUeAgentContractValue Input,
		TOptional<FUeAgentDomainQueryTarget> Target = {},
		const bool bIncludeViewport = true)
	{
		return FUeAgentDomainQuery(
			DomainSession(Registry),
			DomainExecutionContext(bIncludeViewport),
			MoveTemp(Input),
			MoveTemp(Target),
			DomainProjection());
	}

	FUeAgentQueryBindingRequest DomainRequest(
		const FUeAgentDescriptorRegistry* Registry,
		FUeAgentContractValue Input,
		TOptional<FUeAgentDomainQueryTarget> Target = {},
		const bool bIncludeViewport = true,
		FUeAgentDescriptorIdentity Identity = DomainDescriptorIdentity())
	{
		return FUeAgentQueryBindingRequest(
			MoveTemp(Identity),
			TEXT("request-domain-query"),
			FUeAgentQueryBindingPayload(
				DomainPayload(
					Registry,
					MoveTemp(Input),
					MoveTemp(Target),
					bIncludeViewport)));
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUeAgentDomainQueryPayloadTest,
	"UeAgentInterface.Core.BindingGateway.Query.DomainQuery.PayloadCodecAndBoundary",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FUeAgentDomainQueryPayloadTest::RunTest(const FString& Parameters)
{
	const TSharedPtr<const FUeAgentDescriptorRegistry> Registry =
		BuildDescriptorRegistry(this);
	if (!Registry.IsValid())
	{
		return false;
	}
	const FUeAgentQueryBindingPayload Payload(
		DomainPayload(
			Registry.Get(),
			FUeAgentContractValue(TEXT(
				"{\"path\":\"/Game/Maps/TestMap\",\"mode\":\"path\"}"))));
	TestEqual(
		TEXT("domain query is the fourth typed C3Q payload"),
		Payload.GetKind(),
		EUeAgentQueryBindingPayloadKind::DomainQuery);

	const FUeAgentResourceTrackerOwner Resources = DomainResources();
	FUeAgentContractValue Encoded(TEXT("{}"));
	FUeAgentQueryBindingError Error;
	TestTrue(
		TEXT("encode domain query through the existing C3Q codec"),
		FUeAgentQueryPayloadCodec::TryEncode(
			Payload,
			Resources.Get(),
			Encoded,
			Error));
	TestTrue(
		TEXT("wire owns only user input and explicit execution context"),
		Encoded.GetJsonValue().Contains(
			TEXT("\"query_kind\":\"domain_query\""))
			&& Encoded.GetJsonValue().Contains(
				TEXT("\"world_identity\":"))
			&& Encoded.GetJsonValue().Contains(
				TEXT("\"input\":")));
	for (const FString& Forbidden : {
			TEXT("provider_binding"),
			TEXT("command_id"),
			TEXT("\"route\""),
			TEXT("\"selector_params\""),
		})
	{
		TestFalse(
			*FString::Printf(
				TEXT("wire does not expose internal routing field %s"),
				*Forbidden),
			Encoded.GetJsonValue().Contains(Forbidden));
	}

	TOptional<FUeAgentQueryBindingPayload> Decoded;
	TestTrue(
		TEXT("decode domain query through the same C3Q codec"),
		FUeAgentQueryPayloadCodec::TryDecode(
			Encoded,
			Resources.Get(),
			Decoded,
			Error));
	TestTrue(TEXT("typed domain query returned"), Decoded.IsSet());
	if (!Decoded.IsSet())
	{
		AddError(FString::Printf(
			TEXT("domain_query_decode_failed:%s:%s:%s"),
			*Error.Code,
			*Error.SubjectId,
			*Error.Detail));
		return false;
	}
	TestEqual(
		TEXT("domain input survives wire round trip"),
		Decoded->GetDomainQuery()->GetInput().GetJsonValue(),
		Payload.GetDomainQuery()->GetInput().GetJsonValue());
	TestEqual(
		TEXT("explicit world survives wire round trip"),
		Decoded->GetDomainQuery()
			->GetExecutionContext()
			.WorldIdentity,
		DomainExecutionContext().WorldIdentity);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUeAgentDomainQueryDispatchTest,
	"UeAgentInterface.Core.BindingGateway.Query.DomainQuery.ExactProviderAndOperationContext",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FUeAgentDomainQueryDispatchTest::RunTest(const FString& Parameters)
{
	const TOptional<FDomainBindingGraph> Graph =
		BuildDomainBindingGraph(this);
	if (!Graph.IsSet())
	{
		return false;
	}
	const FUeAgentResourceTrackerOwner Resources = DomainResources();
	const FUeAgentOperationDeadline Deadline = DomainDeadline();
	const FUeAgentCancellationSource Cancellation(
		[]() { return 100.0; });
	TSharedPtr<const FUeAgentQueryBindingResult> Result;
	FUeAgentQueryBindingError Error;
	TestTrue(
		TEXT("submit parameterized query through one C3Q owner"),
		Graph->Binding->TrySubmit(
			DomainRequest(
				Graph->DescriptorRegistry.Get(),
				FUeAgentContractValue(TEXT(
					"{\"path\":\"/Game/Maps/TestMap\","
					"\"mode\":\"path\",\"filter\":[\"actor\"]}"))),
			DomainCaller(),
			Resources,
			Deadline,
			Cancellation.GetToken(),
			Result,
			Error));
	TestTrue(TEXT("typed query result returned"), Result.IsValid());
	TestEqual(TEXT("exact provider called once"), Graph->Primary->Calls, 1);
	TestEqual(TEXT("unselected provider never called"), Graph->Fallback->Calls, 0);
	TestEqual(
		TEXT("provider receives canonical C0 input"),
		Graph->Primary->LastInput,
		FString(TEXT(
			"{\"filter\":[\"actor\"],\"mode\":\"path\","
			"\"path\":\"/Game/Maps/TestMap\"}")));
	TestEqual(
		TEXT("provider sees request id"),
		Graph->Primary->LastRequestId,
		FString(TEXT("request-domain-query")));
	TestEqual(
		TEXT("provider sees caller"),
		Graph->Primary->LastCallerId,
		DomainCaller().GetCallerId());
	TestEqual(
		TEXT("provider sees frozen world"),
		Graph->Primary->LastWorld,
		DomainExecutionContext().WorldIdentity);
	TestEqual(
		TEXT("provider sees frozen viewport"),
		Graph->Primary->LastViewport,
		DomainExecutionContext().ViewportIdentity);
	TestTrue(
		TEXT("provider and projection share one resource tracker"),
		Graph->Primary->LastResources == &Resources.Get()
			&& Graph->Estimator->LastResources == &Resources.Get());
	TestEqual(
		TEXT("provider sees the same deadline"),
		Graph->Primary->LastDeadlineAt,
		Deadline.GetDeadlineAt());
	TestFalse(
		TEXT("provider sees the same live cancellation token"),
		Graph->Primary->bObservedCancellation);
	TestTrue(
		TEXT("C10 projects the C4 source returned by the provider"),
		Result->GetValue()
			.GetProjection()
			->GetPayload()
			.GetValue()
			.IsValid());
	TestFalse(
		TEXT("untargeted domain query does not invent a ticket"),
		Result->GetValue().GetTicket().IsSet());
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUeAgentDomainQueryValidationTest,
	"UeAgentInterface.Core.BindingGateway.Query.DomainQuery.ValidationBeforeDispatch",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FUeAgentDomainQueryValidationTest::RunTest(const FString& Parameters)
{
	const TOptional<FDomainBindingGraph> Graph =
		BuildDomainBindingGraph(this);
	if (!Graph.IsSet())
	{
		return false;
	}
	const FUeAgentResourceTrackerOwner Resources = DomainResources();
	const FUeAgentCancellationSource Cancellation(
		[]() { return 100.0; });
	TSharedPtr<const FUeAgentQueryBindingResult> Result;
	FUeAgentQueryBindingError Error;

	TestFalse(
		TEXT("C0 schema mismatch rejects before provider dispatch"),
		Graph->Binding->TrySubmit(
			DomainRequest(
				Graph->DescriptorRegistry.Get(),
				FUeAgentContractValue(TEXT("{\"mode\":\"path\"}"))),
			DomainCaller(),
			Resources,
			DomainDeadline(),
			Cancellation.GetToken(),
			Result,
			Error));
	TestEqual(
		TEXT("stable input schema mismatch"),
		Error.Code,
		FString(TEXT("contract_input_schema_mismatch")));
	TestEqual(TEXT("provider not called for bad input"), Graph->Primary->Calls, 0);

	TestFalse(
		TEXT("C1 missing required facet rejects before provider dispatch"),
		Graph->Binding->TrySubmit(
			DomainRequest(
				Graph->DescriptorRegistry.Get(),
				FUeAgentContractValue(TEXT(
					"{\"mode\":\"path\",\"path\":\"target\"}")),
				{},
				false),
			DomainCaller(),
			Resources,
			DomainDeadline(),
			Cancellation.GetToken(),
			Result,
			Error));
	TestEqual(
		TEXT("stable C1 facet mismatch"),
		Error.Code,
		FString(TEXT("execution_context_required_facet_missing")));
	TestEqual(
		TEXT("provider not called for bad execution context"),
		Graph->Primary->Calls,
		0);

	TestFalse(
		TEXT("C0 hash mismatch rejects before provider dispatch"),
		Graph->Binding->TrySubmit(
			DomainRequest(
				Graph->DescriptorRegistry.Get(),
				FUeAgentContractValue(TEXT(
					"{\"mode\":\"path\",\"path\":\"target\"}")),
				{},
				true,
				FUeAgentDescriptorIdentity(
					DomainDescriptorIdentity().GetDescriptorId(),
					1,
					TEXT(
						"sha256:ffffffffffffffffffffffffffffffff"
						"ffffffffffffffffffffffffffffffff"))),
			DomainCaller(),
			Resources,
			DomainDeadline(),
			Cancellation.GetToken(),
			Result,
			Error));
	TestEqual(
		TEXT("stable descriptor hash mismatch"),
		Error.Code,
		FString(TEXT("descriptor_hash_mismatch")));
	TestEqual(
		TEXT("provider not called for bad descriptor identity"),
		Graph->Primary->Calls,
		0);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUeAgentDomainQueryTicketTest,
	"UeAgentInterface.Core.BindingGateway.Query.DomainQuery.TicketAndNoFallback",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FUeAgentDomainQueryTicketTest::RunTest(const FString& Parameters)
{
	const TOptional<FDomainBindingGraph> Graph =
		BuildDomainBindingGraph(this);
	if (!Graph.IsSet())
	{
		return false;
	}
	const FUeAgentCandidateBinding Target = DomainTarget();
	TOptional<FUeAgentQueryTicket> Ticket;
	FUeAgentQueryBindingError Error;
	TestTrue(
		TEXT("issue candidate-bound ticket from the shared C3 owner"),
		Graph->Tickets->TryIssue(
			Target,
			DomainDescriptorIdentity(),
			DomainSession(Graph->DescriptorRegistry.Get()),
			DomainCaller(),
			{},
			Ticket,
			Error));
	const FUeAgentDomainQueryTarget QueryTarget(
		Ticket.GetValue(),
		Target);

	const FUeAgentResourceTrackerOwner Resources = DomainResources();
	const FUeAgentCancellationSource Cancellation(
		[]() { return 100.0; });
	TSharedPtr<const FUeAgentQueryBindingResult> Result;
	TestTrue(
		TEXT("valid ticket reaches the exact provider once"),
		Graph->Binding->TrySubmit(
			DomainRequest(
				Graph->DescriptorRegistry.Get(),
				FUeAgentContractValue(TEXT(
					"{\"mode\":\"path\",\"path\":\"target\"}")),
				QueryTarget),
			DomainCaller(),
			Resources,
			DomainDeadline(),
			Cancellation.GetToken(),
			Result,
			Error));
	TestTrue(
		TEXT("provider receives the ticket-bound target"),
		Graph->Primary->LastTarget.IsSet()
			&& Graph->Primary->LastTarget->GetBindingId()
				== Target.GetBindingId());
	TestEqual(TEXT("primary provider called once"), Graph->Primary->Calls, 1);
	TestEqual(TEXT("fallback provider never called"), Graph->Fallback->Calls, 0);

	TestFalse(
		TEXT("caller mismatch rejects the same ticket before provider"),
		Graph->Binding->TrySubmit(
			DomainRequest(
				Graph->DescriptorRegistry.Get(),
				FUeAgentContractValue(TEXT(
					"{\"mode\":\"path\",\"path\":\"target\"}")),
				QueryTarget),
			DomainCaller(TEXT("different-agent")),
			Resources,
			DomainDeadline(),
			Cancellation.GetToken(),
			Result,
			Error));
	TestEqual(
		TEXT("stable ticket context mismatch"),
		Error.Code,
		FString(TEXT("ticket_context_mismatch")));
	TestEqual(
		TEXT("provider count unchanged after ticket rejection"),
		Graph->Primary->Calls,
		1);

	Graph->Primary->bFail = true;
	TestFalse(
		TEXT("provider failure is returned without retry or fallback"),
		Graph->Binding->TrySubmit(
			DomainRequest(
				Graph->DescriptorRegistry.Get(),
				FUeAgentContractValue(TEXT(
					"{\"mode\":\"path\",\"path\":\"target\"}"))),
			DomainCaller(),
			Resources,
			DomainDeadline(),
			Cancellation.GetToken(),
			Result,
			Error));
	TestEqual(
		TEXT("provider error identity is preserved"),
		Error.Code,
		Graph->Primary->FailureCode);
	TestEqual(TEXT("primary invoked only once for failed submit"), Graph->Primary->Calls, 2);
	TestEqual(TEXT("fallback remains unused"), Graph->Fallback->Calls, 0);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUeAgentDomainQueryProviderRegistryTest,
	"UeAgentInterface.Core.BindingGateway.Query.DomainQuery.ProviderRegistry",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FUeAgentDomainQueryProviderRegistryTest::RunTest(
	const FString& Parameters)
{
	const FUeAgentResourceTrackerOwner Resources = DomainResources();
	const TSharedPtr<const FUeAgentProjectionSourceValue> Source =
		BuildProjectionSource(this, Resources.Get());
	if (!Source.IsValid())
	{
		return false;
	}
	const TSharedRef<FDomainProvider> Provider =
		MakeShared<FDomainProvider>(Source.ToSharedRef());
	TSharedPtr<const FUeAgentDomainQueryProviderRegistry> Registry;
	FUeAgentQueryBindingError Error;
	TestFalse(
		TEXT("duplicate provider binding is rejected"),
		FUeAgentDomainQueryProviderRegistry::TryCreate(
			{
				FUeAgentDomainQueryProviderRegistration(
					TEXT("world.query.provider"),
					Provider),
				FUeAgentDomainQueryProviderRegistration(
					TEXT("world.query.provider"),
					Provider),
			},
			Registry,
			Error));
	TestEqual(
		TEXT("stable duplicate provider code"),
		Error.Code,
		FString(TEXT("query_provider_binding_duplicate")));

	const TOptional<FDomainBindingGraph> Missing =
		BuildDomainBindingGraph(this, false);
	if (!Missing.IsSet())
	{
		return false;
	}
	const FUeAgentCancellationSource Cancellation(
		[]() { return 100.0; });
	TSharedPtr<const FUeAgentQueryBindingResult> Result;
	TestFalse(
		TEXT("unknown exact provider binding fails closed"),
		Missing->Binding->TrySubmit(
			DomainRequest(
				Missing->DescriptorRegistry.Get(),
				FUeAgentContractValue(TEXT(
					"{\"mode\":\"path\",\"path\":\"target\"}"))),
			DomainCaller(),
			Resources,
			DomainDeadline(),
			Cancellation.GetToken(),
			Result,
			Error));
	TestEqual(
		TEXT("stable missing provider code"),
		Error.Code,
		FString(TEXT("query_provider_not_found")));
	TestEqual(TEXT("unselected provider never called"), Missing->Fallback->Calls, 0);
	return true;
}
