#include "Misc/AutomationTest.h"

// The fixture and expected hashes are hand-authored test truth, not exports
// from the production registry.
#include "Core/Contract/UeAgentDescriptorRegistry.h"

#include "Core/Contract/UeAgentExecutionContextPolicy.h"


using namespace UE::UAI::Contract;

namespace
{
	const FString QueryDescriptorId =
		TEXT("uai.reference.asset_query");
	const FString QueryCapabilityId =
		TEXT("reference.asset_query");
	const FString QueryDescriptorHash =
		TEXT("sha256:"
			"c358b0002058aa1ef83f427e74f8fde7"
			"16bed4e699d82effcdd322f5ad108245");
	const FString OperationDescriptorId =
		TEXT("uai.fixture.asset_apply");
	const FString OperationCapabilityId =
		TEXT("fixture.asset_apply");
	const FString OperationDescriptorHash =
		TEXT("sha256:"
			"d5d77b007fce21ae3743d655c3bc324e"
			"f69d3f08f899faa0be224b7bea6695fe");
	const FString ExpectedRegistryHash =
		TEXT("sha256:"
			"9ac9d35fd98fac85ef734e5064b1c648"
			"4ed17a0d1e46507c0539c30e81d136e6");

	FUeAgentExecutionContextPolicy MakeQueryExecutionPolicy()
	{
		return FUeAgentExecutionContextPolicy(
			{EUeAgentStatePlane::RegistryIndex},
			{},
			EUeAgentContextResolutionPolicy::ExplicitOnly,
			EUeAgentTransientContextPolicy::ReadOnly);
	}

	FUeAgentExecutionContextPolicy MakeOperationExecutionPolicy(
		const bool bReverseStatePlaneOrder = false)
	{
		TArray<EUeAgentStatePlane> StatePlanes;
		if (bReverseStatePlaneOrder)
		{
			StatePlanes = {
				EUeAgentStatePlane::Persisted,
				EUeAgentStatePlane::AuthoringMemory};
		}
		else
		{
			StatePlanes = {
				EUeAgentStatePlane::AuthoringMemory,
				EUeAgentStatePlane::Persisted};
		}
		return FUeAgentExecutionContextPolicy(
			MoveTemp(StatePlanes),
			{},
			EUeAgentContextResolutionPolicy::ExplicitOnly,
			EUeAgentTransientContextPolicy::ReadOnly);
	}

	FUeAgentQueryBinding MakeQueryBinding()
	{
		return FUeAgentQueryBinding(
			TEXT("reference.asset_query.provider"),
			EUeAgentQueryKind::Catalog,
			EUeAgentQueryMaterializationPolicy::IndexOnly,
			EUeAgentQuerySideEffectPolicy::NoPersistentMutation);
	}

	FUeAgentOperationDescriptor MakeQueryDescriptor(
		const FString& DescriptorId = QueryDescriptorId,
		const FString& CapabilityId = QueryCapabilityId,
		const FString& DescriptorHash = QueryDescriptorHash,
		TOptional<FUeAgentQueryBinding> QueryBinding =
			MakeQueryBinding())
	{
		return FUeAgentOperationDescriptor(
			FUeAgentDescriptorIdentity(
				DescriptorId,
				1,
				DescriptorHash),
			CapabilityId,
			TEXT("reference"),
			EUeAgentDescriptorOperationKind::Query,
			MakeQueryExecutionPolicy(),
			FUeAgentContractValue(
				TEXT("{\"required\":[\"query\"],"
					"\"type\":\"object\"}")),
			FUeAgentContractValue(TEXT("{\"type\":\"object\"}")),
			MoveTemp(QueryBinding));
	}

	FUeAgentOperationDescriptor MakeOperationDescriptor(
		const FString& DescriptorId = OperationDescriptorId,
		const FString& CapabilityId = OperationCapabilityId,
		const FString& DescriptorHash = OperationDescriptorHash,
		TOptional<FUeAgentQueryBinding> QueryBinding = {},
		const bool bReverseStatePlaneOrder = false)
	{
		return FUeAgentOperationDescriptor(
			FUeAgentDescriptorIdentity(
				DescriptorId,
				1,
				DescriptorHash),
			CapabilityId,
			TEXT("reference"),
			EUeAgentDescriptorOperationKind::Operation,
			MakeOperationExecutionPolicy(
				bReverseStatePlaneOrder),
			FUeAgentContractValue(
				TEXT("{\"required\":[\"object_path\"],"
					"\"type\":\"object\"}")),
			FUeAgentContractValue(TEXT("{\"type\":\"object\"}")),
			MoveTemp(QueryBinding));
	}

	bool TryCreateFixtureRegistry(
		TSharedPtr<const FUeAgentDescriptorRegistry>& OutRegistry,
		FString& OutError,
		const bool bReverseInputOrder = false,
		const bool bReverseStatePlaneOrder = false)
	{
		TArray<FUeAgentOperationDescriptor> Descriptors;
		if (bReverseInputOrder)
		{
			Descriptors.Add(MakeQueryDescriptor());
			Descriptors.Add(MakeOperationDescriptor(
				OperationDescriptorId,
				OperationCapabilityId,
				OperationDescriptorHash,
				{},
				bReverseStatePlaneOrder));
		}
		else
		{
			Descriptors.Add(MakeOperationDescriptor(
				OperationDescriptorId,
				OperationCapabilityId,
				OperationDescriptorHash,
				{},
				bReverseStatePlaneOrder));
			Descriptors.Add(MakeQueryDescriptor());
		}
		return FUeAgentDescriptorRegistry::TryCreate(
			MoveTemp(Descriptors),
			OutRegistry,
			OutError);
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUeAgentDescriptorRegistrySnapshotTest,
	"UeAgentInterface.Core.Contract.DescriptorRegistry.Snapshot",
	EAutomationTestFlags::EditorContext
		| EAutomationTestFlags::EngineFilter)

bool FUeAgentDescriptorRegistrySnapshotTest::RunTest(
	const FString& Parameters)
{
	TSharedPtr<const FUeAgentDescriptorRegistry> Registry;
	FString Error;
	TestTrue(
		TEXT("hand-authored descriptor snapshot is accepted"),
		TryCreateFixtureRegistry(Registry, Error, true, true));
	TestTrue(TEXT("registry is returned"), Registry.IsValid());
	if (!Registry.IsValid())
	{
		AddError(Error);
		return false;
	}

	TestEqual(
		TEXT("registry exposes exactly the fixture descriptors"),
		Registry->GetDescriptors().Num(),
		2);
	TestEqual(
		TEXT("snapshot order is descriptor ID order"),
		Registry->GetDescriptors()[0].GetIdentity().GetDescriptorId(),
		OperationDescriptorId);
	TestEqual(
		TEXT("registry hash is independent of insertion order"),
		Registry->GetRegistryHash(),
		ExpectedRegistryHash);

	TSharedPtr<const FUeAgentDescriptorRegistry> SecondRegistry;
	TestTrue(
		TEXT("canonical input order is also accepted"),
		TryCreateFixtureRegistry(SecondRegistry, Error));
	TestTrue(TEXT("second registry is returned"), SecondRegistry.IsValid());
	if (SecondRegistry.IsValid())
	{
		TestEqual(
			TEXT("same snapshot produces the same registry hash"),
			SecondRegistry->GetRegistryHash(),
			Registry->GetRegistryHash());
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUeAgentDescriptorRegistryQueryProjectionTest,
	"UeAgentInterface.Core.Contract.DescriptorRegistry.QueryProjection",
	EAutomationTestFlags::EditorContext
		| EAutomationTestFlags::EngineFilter)

bool FUeAgentDescriptorRegistryQueryProjectionTest::RunTest(
	const FString& Parameters)
{
	TSharedPtr<const FUeAgentDescriptorRegistry> Registry;
	FString Error;
	if (!TryCreateFixtureRegistry(Registry, Error)
		|| !Registry.IsValid())
	{
		AddError(Error);
		return false;
	}

	const FUeAgentOperationDescriptor* OperationDescriptor =
		Registry->FindByDescriptorId(QueryDescriptorId);
	const TOptional<FUeAgentQueryDescriptor> QueryDescriptor =
		Registry->FindQueryExact(
			FUeAgentDescriptorIdentity(
				QueryDescriptorId,
				1,
				QueryDescriptorHash));
	TestNotNull(
		TEXT("operation descriptor is found by canonical ID"),
		OperationDescriptor);
	TestTrue(
		TEXT("query descriptor is projected from the same registry"),
		QueryDescriptor.IsSet());
	if (!OperationDescriptor || !QueryDescriptor.IsSet())
	{
		return false;
	}

	TestTrue(
		TEXT("query projection points to the same operation descriptor"),
		&QueryDescriptor->GetOperationDescriptor()
			== OperationDescriptor);
	TestEqual(
		TEXT("query input schema remains business contract truth"),
		QueryDescriptor->GetInputSchema().GetJsonValue(),
		FString(TEXT(
			"{\"required\":[\"query\"],\"type\":\"object\"}")));
	TestEqual(
		TEXT("query result schema remains business contract truth"),
		QueryDescriptor->GetResultSchema().GetJsonValue(),
		FString(TEXT("{\"type\":\"object\"}")));
	TestEqual(
		TEXT("provider binding is explicit and domain-neutral"),
		QueryDescriptor->GetProviderBinding(),
		FString(TEXT("reference.asset_query.provider")));
	TestEqual(
		TEXT("query kind is explicit"),
		QueryDescriptor->GetQueryKind(),
		EUeAgentQueryKind::Catalog);
	TestEqual(
		TEXT("materialization policy is explicit"),
		QueryDescriptor->GetMaterializationPolicy(),
		EUeAgentQueryMaterializationPolicy::IndexOnly);
	TestEqual(
		TEXT("query side-effect policy is explicit"),
		QueryDescriptor->GetSideEffectPolicy(),
		EUeAgentQuerySideEffectPolicy::NoPersistentMutation);

	const FUeAgentExecutionContextPolicy& ExecutionPolicy =
		QueryDescriptor->GetExecutionContextPolicy();
	TestTrue(
		TEXT("query projection reuses the operation descriptor policy"),
		&ExecutionPolicy
			== &OperationDescriptor->GetExecutionContextPolicy());
	TestTrue(
		TEXT("registry-index is the explicit query state plane"),
		ExecutionPolicy.GetAllowedStatePlanes().Contains(
			EUeAgentStatePlane::RegistryIndex));
	TestEqual(
		TEXT("query has exactly one allowed state plane"),
		ExecutionPolicy.GetAllowedStatePlanes().Num(),
		1);
	TestEqual(
		TEXT("context resolution never defaults silently"),
		ExecutionPolicy.GetResolutionPolicy(),
		EUeAgentContextResolutionPolicy::ExplicitOnly);
	TestEqual(
		TEXT("transient contexts remain read-only"),
		ExecutionPolicy.GetTransientContextPolicy(),
		EUeAgentTransientContextPolicy::ReadOnly);

	TestFalse(
		TEXT("effect descriptor cannot be projected as a query"),
		Registry->FindQueryExact(
			FUeAgentDescriptorIdentity(
				OperationDescriptorId,
				1,
				OperationDescriptorHash))
			.IsSet());
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUeAgentDescriptorRegistryValidationTest,
	"UeAgentInterface.Core.Contract.DescriptorRegistry.Validation",
	EAutomationTestFlags::EditorContext
		| EAutomationTestFlags::EngineFilter)

bool FUeAgentDescriptorRegistryValidationTest::RunTest(
	const FString& Parameters)
{
	TSharedPtr<const FUeAgentDescriptorRegistry> Rejected;
	FString Error;

	TArray<FUeAgentOperationDescriptor> DuplicateIds{
		MakeQueryDescriptor(),
		MakeQueryDescriptor(
			QueryDescriptorId,
			TEXT("reference.asset_query.duplicate"))};
	TestFalse(
		TEXT("duplicate descriptor ID is rejected"),
		FUeAgentDescriptorRegistry::TryCreate(
			MoveTemp(DuplicateIds),
			Rejected,
			Error));
	TestEqual(
		TEXT("duplicate descriptor ID error is stable"),
		Error,
		FString(TEXT("descriptor_registry_descriptor_id_duplicate")));

	TArray<FUeAgentOperationDescriptor> DuplicateCapabilities{
		MakeQueryDescriptor(),
		MakeQueryDescriptor(
			TEXT("uai.reference.asset_query.duplicate"),
			QueryCapabilityId)};
	TestFalse(
		TEXT("duplicate capability ID is rejected"),
		FUeAgentDescriptorRegistry::TryCreate(
			MoveTemp(DuplicateCapabilities),
			Rejected,
			Error));
	TestEqual(
		TEXT("duplicate capability error is stable"),
		Error,
		FString(TEXT("descriptor_registry_capability_id_duplicate")));

	TArray<FUeAgentOperationDescriptor> InvalidHash{
		MakeQueryDescriptor(
			QueryDescriptorId,
			QueryCapabilityId,
			TEXT("sha256:"
				"00000000000000000000000000000000"
				"00000000000000000000000000000000"))};
	TestFalse(
		TEXT("descriptor hash must match canonical payload"),
		FUeAgentDescriptorRegistry::TryCreate(
			MoveTemp(InvalidHash),
			Rejected,
			Error));
	TestEqual(
		TEXT("hash mismatch error is stable"),
		Error,
		FString(TEXT("descriptor_hash_mismatch")));

	TArray<FUeAgentOperationDescriptor> MissingQueryBinding{
		MakeQueryDescriptor(
			QueryDescriptorId,
			QueryCapabilityId,
			QueryDescriptorHash,
			TOptional<FUeAgentQueryBinding>())};
	TestFalse(
		TEXT("query requires query binding"),
		FUeAgentDescriptorRegistry::TryCreate(
			MoveTemp(MissingQueryBinding),
			Rejected,
			Error));
	TestEqual(
		TEXT("missing query binding error is stable"),
		Error,
		FString(TEXT("descriptor_query_binding_required")));

	TArray<FUeAgentOperationDescriptor> EffectWithQueryBinding{
		MakeOperationDescriptor(
			OperationDescriptorId,
			OperationCapabilityId,
			OperationDescriptorHash,
			MakeQueryBinding())};
	TestFalse(
		TEXT("effect descriptor cannot own query binding"),
		FUeAgentDescriptorRegistry::TryCreate(
			MoveTemp(EffectWithQueryBinding),
			Rejected,
			Error));
	TestEqual(
		TEXT("effect query binding error is stable"),
		Error,
		FString(TEXT("descriptor_query_binding_forbidden")));

	const FUeAgentExecutionContextPolicy InvalidExecutionPolicy(
		{EUeAgentStatePlane::Invalid},
		{},
		EUeAgentContextResolutionPolicy::ExplicitOnly,
		EUeAgentTransientContextPolicy::ReadOnly);
	TArray<FUeAgentOperationDescriptor> InvalidExecutionPolicies{
		FUeAgentOperationDescriptor(
			FUeAgentDescriptorIdentity(
				QueryDescriptorId,
				1,
				QueryDescriptorHash),
			QueryCapabilityId,
			TEXT("reference"),
			EUeAgentDescriptorOperationKind::Query,
			InvalidExecutionPolicy,
			FUeAgentContractValue(
				TEXT("{\"required\":[\"query\"],"
					"\"type\":\"object\"}")),
			FUeAgentContractValue(TEXT("{\"type\":\"object\"}")),
			MakeQueryBinding())};
	TestFalse(
		TEXT("descriptor rejects an invalid execution context policy"),
		FUeAgentDescriptorRegistry::TryCreate(
			MoveTemp(InvalidExecutionPolicies),
			Rejected,
			Error));
	TestEqual(
		TEXT("invalid execution context policy error is stable"),
		Error,
		FString(TEXT(
			"descriptor_execution_context_policy_invalid")));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUeAgentDescriptorRegistryCompatibilityTest,
	"UeAgentInterface.Core.Contract.DescriptorRegistry.Compatibility",
	EAutomationTestFlags::EditorContext
		| EAutomationTestFlags::EngineFilter)

bool FUeAgentDescriptorRegistryCompatibilityTest::RunTest(
	const FString& Parameters)
{
	TSharedPtr<const FUeAgentDescriptorRegistry> Registry;
	FString Error;
	if (!TryCreateFixtureRegistry(Registry, Error)
		|| !Registry.IsValid())
	{
		AddError(Error);
		return false;
	}

	const FUeAgentDescriptorIdentity ExactIdentity(
		QueryDescriptorId,
		1,
		QueryDescriptorHash);
	TestNotNull(
		TEXT("exact identity resolves"),
		Registry->FindExact(ExactIdentity));
	TestEqual(
		TEXT("exact compatibility is explicit"),
		Registry->CheckCompatibility(ExactIdentity),
		EUeAgentDescriptorCompatibility::Exact);

	const FUeAgentDescriptorIdentity VersionMismatch(
		QueryDescriptorId,
		2,
		QueryDescriptorHash);
	TestNull(
		TEXT("version mismatch never falls back"),
		Registry->FindExact(VersionMismatch));
	TestEqual(
		TEXT("version mismatch is explicit"),
		Registry->CheckCompatibility(VersionMismatch),
		EUeAgentDescriptorCompatibility::VersionIncompatible);

	const FUeAgentDescriptorIdentity HashMismatch(
		QueryDescriptorId,
		1,
		TEXT("sha256:"
			"ffffffffffffffffffffffffffffffff"
			"ffffffffffffffffffffffffffffffff"));
	TestNull(
		TEXT("hash mismatch never falls back"),
		Registry->FindExact(HashMismatch));
	TestEqual(
		TEXT("hash mismatch is explicit"),
		Registry->CheckCompatibility(HashMismatch),
		EUeAgentDescriptorCompatibility::HashIncompatible);

	const FUeAgentDescriptorIdentity Missing(
		TEXT("uai.reference.missing"),
		1,
		QueryDescriptorHash);
	TestEqual(
		TEXT("missing descriptor is explicit"),
		Registry->CheckCompatibility(Missing),
		EUeAgentDescriptorCompatibility::NotFound);
	return true;
}


IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUeAgentExecutionContextPolicyContractTest,
	"UeAgentInterface.Core.Contract.DescriptorRegistry.ExecutionContextPolicy",
	EAutomationTestFlags::EditorContext
		| EAutomationTestFlags::EngineFilter)

bool FUeAgentExecutionContextPolicyContractTest::RunTest(
	const FString& Parameters)
{
	const FUeAgentExecutionContextPolicy CompletePolicy(
		{
			EUeAgentStatePlane::AuthoringMemory,
			EUeAgentStatePlane::Persisted,
			EUeAgentStatePlane::CompiledGenerated,
			EUeAgentStatePlane::RuntimeInstance,
			EUeAgentStatePlane::RenderedObservation,
			EUeAgentStatePlane::RegistryIndex,
		},
		{
			EUeAgentExecutionContextFacet::World,
			EUeAgentExecutionContextFacet::Level,
			EUeAgentExecutionContextFacet::DataLayer,
			EUeAgentExecutionContextFacet::WorldPartition,
			EUeAgentExecutionContextFacet::PieInstance,
			EUeAgentExecutionContextFacet::NetMode,
			EUeAgentExecutionContextFacet::NetRole,
			EUeAgentExecutionContextFacet::AssetEditor,
			EUeAgentExecutionContextFacet::Viewport,
			EUeAgentExecutionContextFacet::Selection,
		},
		EUeAgentContextResolutionPolicy::ResolveCurrentThenFreeze,
		EUeAgentTransientContextPolicy::TemporaryMutationAllowed);
	TestTrue(
		TEXT("all designed state planes and context facets are typed"),
		CompletePolicy.IsValid());
	TestEqual(
		TEXT("the six target state planes remain distinct"),
		CompletePolicy.GetAllowedStatePlanes().Num(),
		6);
	TestEqual(
		TEXT("all optional execution context facets are expressible"),
		CompletePolicy.GetRequiredFacets().Num(),
		10);
	TestEqual(
		TEXT("resolve-current is only represented as resolve then freeze"),
		CompletePolicy.GetResolutionPolicy(),
		EUeAgentContextResolutionPolicy::ResolveCurrentThenFreeze);
	TestEqual(
		TEXT("temporary mutation requires an explicit descriptor policy"),
		CompletePolicy.GetTransientContextPolicy(),
		EUeAgentTransientContextPolicy::TemporaryMutationAllowed);

	const FUeAgentExecutionContextPolicy EmptyStatePlanes(
		{},
		{},
		EUeAgentContextResolutionPolicy::ExplicitOnly,
		EUeAgentTransientContextPolicy::ReadOnly);
	TestFalse(
		TEXT("a descriptor cannot omit its allowed state planes"),
		EmptyStatePlanes.IsValid());

	const FUeAgentExecutionContextPolicy DuplicateStatePlane(
		{
			EUeAgentStatePlane::Persisted,
			EUeAgentStatePlane::Persisted,
		},
		{},
		EUeAgentContextResolutionPolicy::ExplicitOnly,
		EUeAgentTransientContextPolicy::ReadOnly);
	TestFalse(
		TEXT("duplicate state planes are rejected, not silently merged"),
		DuplicateStatePlane.IsValid());

	const FUeAgentExecutionContextPolicy DuplicateFacet(
		{EUeAgentStatePlane::AuthoringMemory},
		{
			EUeAgentExecutionContextFacet::World,
			EUeAgentExecutionContextFacet::World,
		},
		EUeAgentContextResolutionPolicy::ExplicitOnly,
		EUeAgentTransientContextPolicy::ReadOnly);
	TestFalse(
		TEXT("duplicate context facets are rejected"),
		DuplicateFacet.IsValid());

	const FUeAgentExecutionContextPolicy InvalidResolution(
		{EUeAgentStatePlane::AuthoringMemory},
		{},
		EUeAgentContextResolutionPolicy::Invalid,
		EUeAgentTransientContextPolicy::ReadOnly);
	TestFalse(
		TEXT("context resolution policy is mandatory"),
		InvalidResolution.IsValid());

	const FUeAgentExecutionContextPolicy InvalidTransientPolicy(
		{EUeAgentStatePlane::AuthoringMemory},
		{},
		EUeAgentContextResolutionPolicy::ExplicitOnly,
		EUeAgentTransientContextPolicy::Invalid);
	TestFalse(
		TEXT("transient context access policy is mandatory"),
		InvalidTransientPolicy.IsValid());
	return true;
}

