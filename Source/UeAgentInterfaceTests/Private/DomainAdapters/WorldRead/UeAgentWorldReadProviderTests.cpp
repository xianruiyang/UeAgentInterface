#include "Misc/AutomationTest.h"

// RE-003 supplies the optional production seam frozen by the RE-002 test.
#include "DomainAdapters/WorldRead/UeAgentWorldReadProvider.h"


#include "Core/ContextResource/UeAgentOperationContext.h"
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
	using namespace UE::UAI::DomainAdapters::WorldRead;
	using namespace UE::UAI::Projection;
	using namespace UE::UAI::StateSemantic;

	FUeAgentDescriptorIdentity WorldSummaryIdentity()
	{
		return FUeAgentDescriptorIdentity(
			TEXT("uai.query.world.summary"),
			1,
			TEXT(
				"sha256:0c8c315c7e3f6d714fd0ee10c9fe1434"
				"2917408f095d6d0da55c72fd4741234f"));
	}

	FUeAgentOperationDescriptor WorldSummaryOperation()
	{
		return FUeAgentOperationDescriptor(
			WorldSummaryIdentity(),
			TEXT("world.summary.read"),
			TEXT("world"),
			EUeAgentDescriptorOperationKind::Query,
			FUeAgentExecutionContextPolicy(
				{EUeAgentStatePlane::AuthoringMemory},
				{EUeAgentExecutionContextFacet::World},
				EUeAgentContextResolutionPolicy::
					ResolveCurrentThenFreeze,
				EUeAgentTransientContextPolicy::ReadOnly),
			FUeAgentContractValue(TEXT(
				"{\"additionalProperties\":false,"
				"\"properties\":{},\"type\":\"object\"}")),
			FUeAgentContractValue(TEXT("{\"type\":\"object\"}")),
			FUeAgentQueryBinding(
				TEXT("world.read.summary"),
				EUeAgentQueryKind::State,
				EUeAgentQueryMaterializationPolicy::RuntimeObserve,
				EUeAgentQuerySideEffectPolicy::
					NoPersistentMutation));
	}

	TSharedPtr<const FUeAgentDescriptorRegistry> WorldSummaryRegistry(
		FAutomationTestBase* Test)
	{
		TSharedPtr<const FUeAgentDescriptorRegistry> Registry;
		FString Error;
		Test->TestTrue(
			TEXT("create frozen WorldRead summary descriptor"),
			FUeAgentDescriptorRegistry::TryCreate(
				{WorldSummaryOperation()},
				Registry,
				Error));
		if (!Registry.IsValid())
		{
			Test->AddError(FString::Printf(
				TEXT("world_read_descriptor_registry_failed:%s"),
				*Error));
		}
		return Registry;
	}

	FUeAgentResourceTrackerOwner WorldReadResources()
	{
		return MakeShared<FUeAgentResourceTracker, ESPMode::ThreadSafe>();
	}

	FUeAgentCandidateBinding WorldTarget()
	{
		return FUeAgentCandidateBinding(
			TEXT("world-read-target"),
			FUeAgentCandidateSummary(
				FUeAgentCanonicalIdentity(
					TEXT("world.actor:FixtureActor"),
					TEXT("actor"),
					TEXT("/Game/Maps/WorldReadFixture"),
					TEXT("FixtureActor"),
					TEXT("target"),
					TEXT("revision-1")),
				WorldSummaryIdentity(),
				TEXT("world_actor"),
				TEXT("Fixture Actor"),
				TEXT("owned WorldRead provider fixture"),
				{TEXT("WorldRead"), TEXT("Fixture")}),
			FUeAgentCatalogRevision(
				TEXT("world-read-fixture-catalog"),
				TEXT("revision-1"),
				1),
			TEXT("uai://world/actor/FixtureActor"),
			TEXT("world-read"),
			{EUeAgentCandidateOperation::Read},
			TEXT("world-read-provider-test"));
	}

	TSharedPtr<FUeAgentOperationContext> WorldOperationContext(
		FAutomationTestBase* Test,
		const FUeAgentResourceTrackerOwner& Resources)
	{
		FUeAgentExecutionContextInput Input;
		Input.ProjectInstanceId = TEXT("fixture-project");
		Input.EditorInstanceId = TEXT("fixture-editor");
		Input.StatePlane = EUeAgentStatePlane::AuthoringMemory;
		Input.WorldType = EUeAgentExecutionWorldType::Editor;
		Input.WorldIdentity =
			TEXT("/Game/Maps/WorldReadFixture.WorldReadFixture");
		Input.ContextRevision = TEXT("world-revision-1");

		FUeAgentExecutionContextRefPtr ExecutionContext;
		FUeAgentExecutionContextError ContextError;
		Test->TestTrue(
			TEXT("freeze explicit WorldRead execution context"),
			FUeAgentExecutionContextRef::TryCreate(
				Input,
				WorldSummaryIdentity(),
				EUeAgentRequestKind::Query,
				WorldSummaryOperation().GetExecutionContextPolicy(),
				ExecutionContext,
				ContextError));
		if (!ExecutionContext.IsValid())
		{
			return nullptr;
		}

		const FUeAgentQueryRequest Request(
			WorldSummaryIdentity(),
			TEXT("request-world-read-provider"),
			FUeAgentContractValue(TEXT("{}")));
		const FUeAgentCallerContext Caller(
			TEXT("fixture-agent"),
			{TEXT("world:read")},
			TEXT("corr-world-read-provider"));
		const FUeAgentOperationDeadline Deadline =
			FUeAgentOperationDeadline::FromTimeout(
				30.0,
				[]() { return 100.0; });
		const FUeAgentCancellationSource Cancellation(
			[]() { return 100.0; });

		TSharedPtr<FUeAgentOperationContext> Context;
		FUeAgentOperationContextError OperationError;
		Test->TestTrue(
			TEXT("create one WorldRead operation context"),
			FUeAgentOperationContext::TryCreate(
				Request,
				Caller,
				ExecutionContext.ToSharedRef(),
				Resources,
				Deadline,
				Cancellation.GetToken(),
				Context,
				OperationError));
		return Context;
	}

	TSharedPtr<const FUeAgentCanonicalSnapshot> WorldSnapshot(
		FAutomationTestBase* Test,
		FUeAgentResourceTracker& Resources)
	{
		const FUeAgentStateRevision Revision(
			TEXT("world-read"),
			TEXT("revision-1"),
			1);
		FUeAgentCanonicalValuePtr Value;
		FUeAgentStateSemanticError Error;
		Test->TestTrue(
			TEXT("create C4 WorldRead value"),
			FUeAgentCanonicalValue::TryString(
				TEXT("/Game/Maps/WorldReadFixture"),
				Resources,
				Value,
				Error));
		if (!Value.IsValid())
		{
			return nullptr;
		}

		const FUeAgentStateEntry Entry(
			FUeAgentCanonicalIdentity(
				TEXT("world.summary:map"),
				TEXT("world"),
				TEXT("/Game/Maps/WorldReadFixture"),
				TEXT("package_name"),
				TEXT("state"),
				TEXT("revision-1")),
			FUeAgentCanonicalType(
				TEXT("core.string"),
				EUeAgentValueKind::String,
				false,
				TEXT("scalar")),
			Value.ToSharedRef(),
			FUeAgentDefaultValue(
				EUeAgentDefaultSourceKind::DomainNative,
				TEXT("world-native"),
				TEXT("independent-world-read"),
				Value.ToSharedRef()),
			FUeAgentStateDetails(
				TEXT("World Package"),
				TEXT("owned fixture world package"),
				{TEXT("WorldRead"), TEXT("Object")},
				false,
				TEXT("available"),
				{}),
			Revision);
		TSharedPtr<const FUeAgentCanonicalSnapshot> Snapshot;
		Test->TestTrue(
			TEXT("create C4 WorldRead snapshot"),
			FUeAgentCanonicalSnapshot::TryCreate(
				TEXT("world-read-summary-snapshot"),
				EUeAgentStatePlane::AuthoringMemory,
				Revision,
				FUeAgentTemporalPoint(
					TEXT("fixture-clock"),
					TEXT("1")),
				FUeAgentStateProvenance(
					TEXT("independent_readback"),
					TEXT("world-read"),
					TEXT("fixture-world-read")),
				{Entry},
				Resources,
				Snapshot,
				Error));
		return Snapshot;
	}

	class FRecordingWorldReadHook final
		: public IUeAgentWorldReadHook
	{
	public:
		explicit FRecordingWorldReadHook(
			TSharedRef<const FUeAgentCanonicalSnapshot> InSnapshot)
			: Snapshot(MoveTemp(InSnapshot))
		{
		}

		bool TryCapture(
			const FUeAgentContractValue& CanonicalInput,
			const TOptional<FUeAgentCandidateBinding>& Target,
			const FUeAgentOperationContext& Context,
			TSharedPtr<const FUeAgentCanonicalSnapshot>& OutSnapshot,
			FUeAgentWorldReadError& OutError) override
		{
			++Calls;
			LastInput = CanonicalInput.GetJsonValue();
			LastTarget = Target;
			LastContext = &Context;
			LastRequestId = Context.GetRequestId();
			LastWorld =
				Context.GetExecutionContext().GetWorldIdentity();
			if (bFail)
			{
				OutError.Code = TEXT("fixture_world_read_failed");
				OutError.SubjectId = TEXT("fixture-world");
				OutError.Detail = TEXT("fixture hook failure");
				return false;
			}
			OutSnapshot = Snapshot;
			return true;
		}

		bool bFail = false;
		int32 Calls = 0;
		FString LastInput;
		TOptional<FUeAgentCandidateBinding> LastTarget;
		const FUeAgentOperationContext* LastContext = nullptr;
		FString LastRequestId;
		FString LastWorld;

	private:
		TSharedRef<const FUeAgentCanonicalSnapshot> Snapshot;
	};
}


IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUeAgentWorldReadProviderBoundaryTest,
	"UeAgentInterface.DomainAdapters.WorldRead.E1.Provider."
		"ExactHookAndContext",
	EAutomationTestFlags::EditorContext
		| EAutomationTestFlags::EngineFilter)

bool FUeAgentWorldReadProviderBoundaryTest::RunTest(
	const FString& Parameters)
{
	const TSharedPtr<const FUeAgentDescriptorRegistry> Registry =
		WorldSummaryRegistry(this);
	const FUeAgentResourceTrackerOwner Resources =
		WorldReadResources();
	const TSharedPtr<FUeAgentOperationContext> Context =
		WorldOperationContext(this, Resources);
	const TSharedPtr<const FUeAgentCanonicalSnapshot> Snapshot =
		WorldSnapshot(this, Resources.Get());
	if (!Registry.IsValid()
		|| !Context.IsValid()
		|| !Snapshot.IsValid())
	{
		return false;
	}
	const TOptional<FUeAgentQueryDescriptor> Descriptor =
		Registry->FindQueryExact(WorldSummaryIdentity());
	if (!TestTrue(
			TEXT("typed WorldRead query descriptor"),
			Descriptor.IsSet()))
	{
		return false;
	}

	const TSharedRef<FRecordingWorldReadHook> Hook =
		MakeShared<FRecordingWorldReadHook>(
			Snapshot.ToSharedRef());
	FUeAgentWorldReadProvider Provider(
		TEXT("world.read.summary"),
		Hook);
	TSharedPtr<const FUeAgentProjectionSourceValue> Source;
	FUeAgentQueryBindingError Error;
	const FUeAgentCandidateBinding Target = WorldTarget();
	TestTrue(
		TEXT("provider forwards to one exact hook"),
		Provider.TryQuery(
			Descriptor.GetValue(),
			FUeAgentContractValue(TEXT("{}")),
			Target,
			*Context,
			Source,
			Error));
	TestEqual(TEXT("hook called exactly once"), Hook->Calls, 1);
	TestEqual(
		TEXT("canonical input forwarded unchanged"),
		Hook->LastInput,
		FString(TEXT("{}")));
	TestTrue(
		TEXT("Ticket-resolved target forwarded unchanged"),
		Hook->LastTarget.IsSet()
			&& Hook->LastTarget->GetBindingId()
				== Target.GetBindingId());
	TestTrue(
		TEXT("same OperationContext instance is forwarded"),
		Hook->LastContext == Context.Get());
	TestEqual(
		TEXT("frozen world comes only from OperationContext"),
		Hook->LastWorld,
		Context->GetExecutionContext().GetWorldIdentity());
	TestTrue(
		TEXT("successful provider exposes only the hook C4 snapshot"),
		Source.IsValid()
			&& Source->GetSnapshot().Get() == Snapshot.Get()
			&& !Source->GetCatalog().IsValid()
			&& !Source->GetValue().IsValid()
			&& !Source->GetComparison().IsValid());
	Hook->bFail = true;
	Source.Reset();
	TestFalse(
		TEXT("hook failure is returned without fallback"),
		Provider.TryQuery(
			Descriptor.GetValue(),
			FUeAgentContractValue(TEXT("{}")),
			{},
			*Context,
			Source,
			Error));
	TestEqual(
		TEXT("failed hook is not retried"),
		Hook->Calls,
		2);
	TestFalse(
		TEXT("failed hook cannot leak a projection source"),
		Source.IsValid());
	TestEqual(
		TEXT("typed hook failure code is preserved"),
		Error.Code,
		FString(TEXT("fixture_world_read_failed")));

	const TSharedRef<FRecordingWorldReadHook> MismatchHook =
		MakeShared<FRecordingWorldReadHook>(
			Snapshot.ToSharedRef());
	FUeAgentWorldReadProvider MismatchedProvider(
		TEXT("world.read.actors"),
		MismatchHook);
	TestFalse(
		TEXT("provider binding mismatch is rejected"),
		MismatchedProvider.TryQuery(
			Descriptor.GetValue(),
			FUeAgentContractValue(TEXT("{}")),
			{},
			*Context,
			Source,
			Error));
	TestEqual(
		TEXT("mismatched provider never calls a hook"),
		MismatchHook->Calls,
		0);
	TestEqual(
		TEXT("mismatch has a stable failure code"),
		Error.Code,
		FString(TEXT("world_read_provider_binding_mismatch")));
	return true;
}
