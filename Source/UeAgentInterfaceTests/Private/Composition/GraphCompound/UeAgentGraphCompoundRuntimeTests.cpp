#include "Misc/AutomationTest.h"

#include "Composition/GraphCompound/UeAgentGraphCompoundRuntime.h"
#include "Composition/GraphCompound/UeAgentGraphCompoundBlueprintMaterialRoutes.h"


static_assert(
	UAI_GRAPH_COMPOUND_RUNTIME_V1 == 1,
	"GC018-RT-002 requires the exact-binding Graph runtime contract.");

namespace
{
	using namespace UE::UAI::Composition::GraphCompound;
	using namespace UE::UAI::ContextResource;
	using namespace UE::UAI::Contract;

	const FString QueryCapability =
		TEXT("blueprint.component.events.discover");
	const FString OperationCapability = TEXT("blueprint.asset.create");

	class FTestCancellation final : public IUeAgentCancellationSignal
	{
	public:
		explicit FTestCancellation(const bool bInCancelled = false)
			: bCancelled(bInCancelled)
		{
		}

		bool IsCancelled() const override { return bCancelled; }

	private:
		bool bCancelled = false;
	};

	class FTestQueryBinding final : public IGraphCompoundQueryBinding
	{
	public:
		FTestQueryBinding(
			FUeAgentDescriptorIdentity InIdentity,
			FString InOwnerId,
			FUeAgentContractValue InResult,
			bool bInSucceeds = true)
			: Identity(MoveTemp(InIdentity))
			, OwnerId(MoveTemp(InOwnerId))
			, Result(MoveTemp(InResult))
			, bSucceeds(bInSucceeds)
		{
		}

		const FUeAgentDescriptorIdentity& GetDescriptorIdentity() const override
		{
			return Identity;
		}

		const FString& GetOwnerId() const override { return OwnerId; }

		bool TryExecute(
			const FUeAgentQueryRequest& Request,
			const FUeAgentCallerContext& CallerContext,
			TSharedRef<FUeAgentResourceTracker, ESPMode::ThreadSafe> Resources,
			const TSharedRef<IUeAgentCancellationSignal>& Cancellation,
			FUeAgentContractValue& OutResult,
			FGraphCompoundRuntimeError& OutError) override
		{
			(void)CallerContext;
			(void)Resources;
			(void)Cancellation;
			++InvocationCount;
			LastPayload = Request.GetPayload().GetJsonValue();
			if (!bSucceeds)
			{
				OutError = {
					TEXT("test_query_provider_failed"),
					OwnerId,
					TEXT("The exact query provider rejected the request.")};
				return false;
			}
			OutResult = Result;
			return true;
		}

		int32 InvocationCount = 0;
		FString LastPayload;

	private:
		FUeAgentDescriptorIdentity Identity;
		FString OwnerId;
		FUeAgentContractValue Result;
		bool bSucceeds = true;
	};

	class FTestOperationBinding final : public IGraphCompoundOperationBinding
	{
	public:
		FTestOperationBinding(
			FUeAgentDescriptorIdentity InIdentity,
			FString InOwnerId,
			FUeAgentContractValue InResult,
			bool bInSucceeds = true)
			: Identity(MoveTemp(InIdentity))
			, OwnerId(MoveTemp(InOwnerId))
			, Result(MoveTemp(InResult))
			, bSucceeds(bInSucceeds)
		{
		}

		const FUeAgentDescriptorIdentity& GetDescriptorIdentity() const override
		{
			return Identity;
		}

		const FString& GetOwnerId() const override { return OwnerId; }

		bool TryExecute(
			const FUeAgentOperationRequest& Request,
			const FUeAgentCallerContext& CallerContext,
			TSharedRef<FUeAgentResourceTracker, ESPMode::ThreadSafe> Resources,
			const TSharedRef<IUeAgentCancellationSignal>& Cancellation,
			FUeAgentContractValue& OutResult,
			FGraphCompoundRuntimeError& OutError) override
		{
			(void)CallerContext;
			(void)Resources;
			(void)Cancellation;
			++InvocationCount;
			LastPayload = Request.GetPayload().GetJsonValue();
			LastIdempotencyKey = Request.GetIdempotencyKey();
			if (!bSucceeds)
			{
				OutError = {
					TEXT("test_operation_provider_failed"),
					OwnerId,
					TEXT("The exact operation provider rejected the request.")};
				return false;
			}
			OutResult = Result;
			return true;
		}

		int32 InvocationCount = 0;
		FString LastPayload;
		FString LastIdempotencyKey;

	private:
		FUeAgentDescriptorIdentity Identity;
		FString OwnerId;
		FUeAgentContractValue Result;
		bool bSucceeds = true;
	};

	struct FRuntimeFixture
	{
		TSharedPtr<const FUeAgentDescriptorRegistry> Registry;
		FUeAgentDescriptorIdentity QueryIdentity = FUeAgentDescriptorIdentity(
			TEXT("invalid"), 1, TEXT("sha256:invalid"));
		FUeAgentDescriptorIdentity OperationIdentity = FUeAgentDescriptorIdentity(
			TEXT("invalid"), 1, TEXT("sha256:invalid"));
	};

	bool TryCreateFixture(FRuntimeFixture& OutFixture, FString& OutError)
	{
		TSharedPtr<const FUeAgentDescriptorRegistry> DomainRegistry;
		if (!TryCreateBlueprintMaterialGraphDescriptorRegistry(
				DomainRegistry, OutError)
			|| !DomainRegistry.IsValid())
		{
			return false;
		}

		const FGraphCompoundDomainRouteProfile* QueryProfile =
			FindBlueprintMaterialGraphRouteProfile(QueryCapability);
		const FGraphCompoundDomainRouteProfile* OperationProfile =
			FindBlueprintMaterialGraphRouteProfile(OperationCapability);
		if (!QueryProfile || !OperationProfile)
		{
			OutError = TEXT("graph_compound_runtime_fixture_profile_missing");
			return false;
		}

		OutFixture.QueryIdentity =
			QueryProfile->GetExecutionContract().GetDescriptorIdentity();
		OutFixture.OperationIdentity =
			OperationProfile->GetExecutionContract().GetDescriptorIdentity();
		const FUeAgentOperationDescriptor* QueryDescriptor =
			DomainRegistry->FindExact(OutFixture.QueryIdentity);
		const FUeAgentOperationDescriptor* OperationDescriptor =
			DomainRegistry->FindExact(OutFixture.OperationIdentity);
		if (!QueryDescriptor || !OperationDescriptor)
		{
			OutError = TEXT("graph_compound_runtime_fixture_descriptor_missing");
			return false;
		}

		TArray<FUeAgentOperationDescriptor> Descriptors;
		Descriptors.Add(*QueryDescriptor);
		Descriptors.Add(*OperationDescriptor);
		return FUeAgentDescriptorRegistry::TryCreate(
			MoveTemp(Descriptors), OutFixture.Registry, OutError);
	}

	FGraphCompoundRuntimeConfig MakeConfig(const int64 MaxFrameBytes = 65536)
	{
		FGraphCompoundRuntimeConfig Config;
		Config.MaxFrameBytes = MaxFrameBytes;
		return Config;
	}

	TSharedRef<FTestQueryBinding> MakeQueryBinding(
		const FRuntimeFixture& Fixture,
		const FString& OwnerId = TEXT("owner.blueprint.query"),
		const FString& Result = TEXT(
			"{\"asset_path\":\"/Game/UAI/BP_Test\","
			"\"can_author_component_bound_event\":true,"
			"\"compiled_before_query\":true,"
			"\"component_name\":\"Root\","
			"\"component_property_found\":true,\"events\":[],"
			"\"matched_event_count\":0,\"returned_event_count\":0,"
			"\"target_class\":\"/Script/Engine.SceneComponent\","
			"\"total_event_count\":0,\"truncated\":false}"),
		const bool bSucceeds = true)
	{
		return MakeShared<FTestQueryBinding>(
			Fixture.QueryIdentity,
			OwnerId,
			FUeAgentContractValue(Result),
			bSucceeds);
	}

	TSharedRef<FTestOperationBinding> MakeOperationBinding(
		const FRuntimeFixture& Fixture,
		const FString& OwnerId = TEXT("owner.blueprint.operation"),
		const FString& Result = TEXT(
			"{\"revision\":\"revision:test\","
			"\"target_ref\":\"/Game/UAI/BP_Test\"}"),
		const bool bSucceeds = true)
	{
		return MakeShared<FTestOperationBinding>(
			Fixture.OperationIdentity,
			OwnerId,
			FUeAgentContractValue(Result),
			bSucceeds);
	}

	bool TryCreateRuntime(
		const FRuntimeFixture& Fixture,
		const TSharedRef<FTestQueryBinding>& QueryBinding,
		const TSharedRef<FTestOperationBinding>& OperationBinding,
		TSharedPtr<FGraphCompoundRuntime>& OutRuntime,
		FGraphCompoundRuntimeError& OutError,
		const int64 MaxFrameBytes = 65536)
	{
		TArray<TSharedRef<IGraphCompoundQueryBinding>> QueryBindings;
		QueryBindings.Add(QueryBinding);
		TArray<TSharedRef<IGraphCompoundOperationBinding>> OperationBindings;
		OperationBindings.Add(OperationBinding);
		return FGraphCompoundRuntime::TryCreate(
			MakeConfig(MaxFrameBytes),
			Fixture.Registry.ToSharedRef(),
			MoveTemp(QueryBindings),
			MoveTemp(OperationBindings),
			OutRuntime,
			OutError);
	}

	FUeAgentCallerContext MakeCaller()
	{
		return FUeAgentCallerContext(
			TEXT("test.agent"),
			{TEXT("uai.graph.read"), TEXT("uai.graph.write")},
			TEXT("correlation:runtime"));
	}

	FUeAgentQueryRequest MakeQueryRequest(
		const FRuntimeFixture& Fixture,
		const FString& Payload = TEXT(
			"{\"asset_path\":\"/Game/UAI/BP_Test\"," 
			"\"component_name\":\"Root\"}"))
	{
		return FUeAgentQueryRequest(
			Fixture.QueryIdentity,
			TEXT("request:query"),
		FUeAgentContractValue(Payload));
	}

	const FString ExpectedCanonicalQueryPayload = TEXT(
		"{\"asset_path\":\"/Game/UAI/BP_Test\"," 
		"\"component_name\":\"Root\"}");

	const FString ExpectedCanonicalQueryResult = TEXT(
		"{\"asset_path\":\"/Game/UAI/BP_Test\","
		"\"can_author_component_bound_event\":true,"
		"\"compiled_before_query\":true,"
		"\"component_name\":\"Root\","
		"\"component_property_found\":true,\"events\":[],"
		"\"matched_event_count\":0,\"returned_event_count\":0,"
		"\"target_class\":\"/Script/Engine.SceneComponent\","
		"\"total_event_count\":0,\"truncated\":false}");

	FUeAgentOperationRequest MakeOperationRequest(
		const FRuntimeFixture& Fixture,
		const FString& Payload = TEXT(
			"{\"asset_name\":\"BP_Test\",\"package_path\":\"/Game/UAI\","
			"\"parent_class_ref\":\"/Script/Engine.Actor\"}"))
	{
		return FUeAgentOperationRequest(
			Fixture.OperationIdentity,
			TEXT("request:operation"),
			TEXT("idempotency:operation"),
			FUeAgentContractValue(Payload));
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUeAgentGraphCompoundRuntimeRegistryContractTest,
	"UeAgentInterface.Composition.GraphCompound.Runtime.RegistryContract",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FUeAgentGraphCompoundRuntimeRegistryContractTest::RunTest(
	const FString& Parameters)
{
	(void)Parameters;
	FRuntimeFixture Fixture;
	FString FixtureError;
	if (!TryCreateFixture(Fixture, FixtureError))
	{
		AddError(FixtureError);
		return false;
	}

	TestNotNull(
		TEXT("compiled result schema is addressable by exact identity"),
		Fixture.Registry->FindCompiledResultSchemaExact(Fixture.QueryIdentity));

	TSharedPtr<FGraphCompoundRuntime> Runtime;
	FGraphCompoundRuntimeError Error;
	TArray<TSharedRef<IGraphCompoundQueryBinding>> QueryBindings;
	QueryBindings.Add(MakeQueryBinding(Fixture));
	TArray<TSharedRef<IGraphCompoundOperationBinding>> NoOperations;
	TestFalse(
		TEXT("construction rejects a descriptor without its exact owner"),
		FGraphCompoundRuntime::TryCreate(
			MakeConfig(), Fixture.Registry.ToSharedRef(),
			MoveTemp(QueryBindings), MoveTemp(NoOperations), Runtime, Error));
	TestEqual(TEXT("missing owner has a stable error"), Error.Code,
		FString(TEXT("graph_compound_operation_binding_missing")));

	QueryBindings.Reset();
	QueryBindings.Add(MakeQueryBinding(Fixture));
	QueryBindings.Add(MakeQueryBinding(Fixture));
	TArray<TSharedRef<IGraphCompoundOperationBinding>> OperationBindings;
	OperationBindings.Add(MakeOperationBinding(Fixture));
	TestFalse(
		TEXT("construction rejects duplicate exact bindings"),
		FGraphCompoundRuntime::TryCreate(
			MakeConfig(), Fixture.Registry.ToSharedRef(),
			MoveTemp(QueryBindings), MoveTemp(OperationBindings), Runtime, Error));
	TestEqual(TEXT("duplicate binding has a stable error"), Error.Code,
		FString(TEXT("graph_compound_binding_duplicate")));

	QueryBindings.Reset();
	QueryBindings.Add(MakeShared<FTestQueryBinding>(
		Fixture.OperationIdentity,
		TEXT("owner.wrong.lane"),
		FUeAgentContractValue(TEXT("{\"events\":[]}"))));
	OperationBindings.Reset();
	OperationBindings.Add(MakeOperationBinding(Fixture));
	TestFalse(
		TEXT("construction rejects a query binding for an operation descriptor"),
		FGraphCompoundRuntime::TryCreate(
			MakeConfig(), Fixture.Registry.ToSharedRef(),
			MoveTemp(QueryBindings), MoveTemp(OperationBindings), Runtime, Error));
	TestEqual(TEXT("wrong lane has a stable error"), Error.Code,
		FString(TEXT("graph_compound_binding_lane_mismatch")));

	QueryBindings.Reset();
	QueryBindings.Add(MakeQueryBinding(Fixture, TEXT("")));
	OperationBindings.Reset();
	OperationBindings.Add(MakeOperationBinding(Fixture));
	TestFalse(
		TEXT("construction rejects a binding without an explicit owner"),
		FGraphCompoundRuntime::TryCreate(
			MakeConfig(), Fixture.Registry.ToSharedRef(),
			MoveTemp(QueryBindings), MoveTemp(OperationBindings), Runtime, Error));
	TestEqual(TEXT("empty owner has a stable error"), Error.Code,
		FString(TEXT("graph_compound_binding_owner_missing")));
	return !HasAnyErrors();
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUeAgentGraphCompoundRuntimeApplicationPortsTest,
	"UeAgentInterface.Composition.GraphCompound.Runtime.ApplicationPorts",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FUeAgentGraphCompoundRuntimeApplicationPortsTest::RunTest(
	const FString& Parameters)
{
	(void)Parameters;
	FRuntimeFixture Fixture;
	FString FixtureError;
	if (!TryCreateFixture(Fixture, FixtureError))
	{
		AddError(FixtureError);
		return false;
	}
	const TSharedRef<FTestQueryBinding> QueryBinding = MakeQueryBinding(Fixture);
	const TSharedRef<FTestOperationBinding> OperationBinding =
		MakeOperationBinding(Fixture);
	TSharedPtr<FGraphCompoundRuntime> Runtime;
	FGraphCompoundRuntimeError Error;
	if (!TryCreateRuntime(
			Fixture, QueryBinding, OperationBinding, Runtime, Error))
	{
		AddError(Error.Code + TEXT(":") + Error.Detail);
		return false;
	}

	const TSharedRef<FTestCancellation> Active =
		MakeShared<FTestCancellation>();
	const FUeAgentQueryResult QueryResult = Runtime->GetQueryPort()->Submit(
		MakeQueryRequest(Fixture), MakeCaller(), Active);
	TestTrue(TEXT("query result is valid"), QueryResult.IsValid());
	TestEqual(TEXT("query invokes one exact binding"),
		QueryBinding->InvocationCount, 1);
	TestEqual(TEXT("query payload is canonical"), QueryBinding->LastPayload,
		ExpectedCanonicalQueryPayload);
	TestEqual(TEXT("query descriptor identity is preserved"),
		QueryResult.GetDescriptorIdentity(), Fixture.QueryIdentity);
	TestEqual(TEXT("query request identity is preserved"),
		QueryResult.GetRequestId(), FString(TEXT("request:query")));
	TestEqual(TEXT("query result is schema-valid canonical data"),
		QueryResult.GetValue().GetJsonValue(), ExpectedCanonicalQueryResult);

	const FUeAgentOperationResult OperationResult =
		Runtime->GetOperationPort()->Submit(
			MakeOperationRequest(Fixture), MakeCaller(), Active);
	TestTrue(TEXT("operation result is valid"), OperationResult.IsValid());
	TestEqual(TEXT("operation invokes one exact binding"),
		OperationBinding->InvocationCount, 1);
	TestEqual(TEXT("idempotency remains visible to the exact owner"),
		OperationBinding->LastIdempotencyKey,
		FString(TEXT("idempotency:operation")));
	TestEqual(TEXT("operation descriptor identity is preserved"),
		OperationResult.GetDescriptorIdentity(), Fixture.OperationIdentity);
	TestEqual(TEXT("operation request identity is preserved"),
		OperationResult.GetRequestId(), FString(TEXT("request:operation")));
	return !HasAnyErrors();
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUeAgentGraphCompoundRuntimeFailureSemanticsTest,
	"UeAgentInterface.Composition.GraphCompound.Runtime.FailureSemantics",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FUeAgentGraphCompoundRuntimeFailureSemanticsTest::RunTest(
	const FString& Parameters)
{
	(void)Parameters;
	FRuntimeFixture Fixture;
	FString FixtureError;
	if (!TryCreateFixture(Fixture, FixtureError))
	{
		AddError(FixtureError);
		return false;
	}

	const TSharedRef<FTestQueryBinding> QueryBinding = MakeQueryBinding(Fixture);
	const TSharedRef<FTestOperationBinding> OperationBinding =
		MakeOperationBinding(Fixture);
	TSharedPtr<FGraphCompoundRuntime> Runtime;
	FGraphCompoundRuntimeError Error;
	if (!TryCreateRuntime(
			Fixture, QueryBinding, OperationBinding, Runtime, Error))
	{
		AddError(Error.Code + TEXT(":") + Error.Detail);
		return false;
	}

	const TSharedRef<FTestCancellation> Active =
		MakeShared<FTestCancellation>();
	const FUeAgentQueryResult WrongSchema = Runtime->GetQueryPort()->Submit(
		MakeQueryRequest(Fixture, TEXT("{\"unexpected\":true}")),
		MakeCaller(), Active);
	TestTrue(TEXT("input schema failure is explicit"),
		WrongSchema.GetValue().GetJsonValue().Contains(
			TEXT("contract_input_schema_mismatch")));
	TestEqual(TEXT("invalid input never reaches a provider"),
		QueryBinding->InvocationCount, 0);

	const TSharedRef<FTestCancellation> Cancelled =
		MakeShared<FTestCancellation>(true);
	const FUeAgentOperationResult CancelledResult =
		Runtime->GetOperationPort()->Submit(
			MakeOperationRequest(Fixture), MakeCaller(), Cancelled);
	TestTrue(TEXT("cancellation is explicit"),
		CancelledResult.GetValue().GetJsonValue().Contains(
			TEXT("graph_compound_cancelled")));
	TestEqual(TEXT("cancelled input never reaches a provider"),
		OperationBinding->InvocationCount, 0);

	TSharedPtr<FGraphCompoundRuntime> SmallRuntime;
	const TSharedRef<FTestQueryBinding> SmallQuery = MakeQueryBinding(Fixture);
	const TSharedRef<FTestOperationBinding> SmallOperation =
		MakeOperationBinding(Fixture);
	if (!TryCreateRuntime(
			Fixture, SmallQuery, SmallOperation, SmallRuntime, Error, 16))
	{
		AddError(Error.Code + TEXT(":") + Error.Detail);
		return false;
	}
	const FUeAgentQueryResult ResourceResult =
		SmallRuntime->GetQueryPort()->Submit(
			MakeQueryRequest(Fixture), MakeCaller(), Active);
	TestTrue(TEXT("resource exhaustion is explicit"),
		ResourceResult.GetValue().GetJsonValue().Contains(
			TEXT("graph_compound_resource_exceeded")));
	TestEqual(TEXT("resource-rejected input never reaches a provider"),
		SmallQuery->InvocationCount, 0);

	const TSharedRef<FTestQueryBinding> FailedProvider =
		MakeQueryBinding(Fixture, TEXT("owner.failed"), TEXT("{}"), false);
	TSharedPtr<FGraphCompoundRuntime> FailedRuntime;
	if (!TryCreateRuntime(
			Fixture, FailedProvider, MakeOperationBinding(Fixture),
			FailedRuntime, Error))
	{
		AddError(Error.Code + TEXT(":") + Error.Detail);
		return false;
	}
	const FUeAgentQueryResult ProviderFailure =
		FailedRuntime->GetQueryPort()->Submit(
			MakeQueryRequest(Fixture), MakeCaller(), Active);
	TestTrue(TEXT("provider failure remains explicit"),
		ProviderFailure.GetValue().GetJsonValue().Contains(
			TEXT("test_query_provider_failed")));

	const TSharedRef<FTestQueryBinding> InvalidResult =
		MakeQueryBinding(Fixture, TEXT("owner.invalid-result"), TEXT("{}"));
	TSharedPtr<FGraphCompoundRuntime> InvalidResultRuntime;
	if (!TryCreateRuntime(
			Fixture, InvalidResult, MakeOperationBinding(Fixture),
			InvalidResultRuntime, Error))
	{
		AddError(Error.Code + TEXT(":") + Error.Detail);
		return false;
	}
	const FUeAgentQueryResult InvalidProviderResult =
		InvalidResultRuntime->GetQueryPort()->Submit(
			MakeQueryRequest(Fixture), MakeCaller(), Active);
	TestTrue(TEXT("provider output must satisfy the exact result schema"),
		InvalidProviderResult.GetValue().GetJsonValue().Contains(
			TEXT("graph_compound_result_schema_invalid")));
	return !HasAnyErrors();
}
