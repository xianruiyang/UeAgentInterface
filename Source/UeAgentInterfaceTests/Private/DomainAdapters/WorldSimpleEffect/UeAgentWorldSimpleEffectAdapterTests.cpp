#include "Misc/AutomationTest.h"
#include <type_traits>

#include "DomainAdapters/WorldSimpleEffect/UeAgentWorldSimpleEffectAdapter.h"

#include "DomainAdapters/WorldSimpleEffect/UeAgentWorldSimpleEffectProduction.h"


#include "Core/ContextResource/UeAgentContextResource.h"
#include "Core/Contract/UeAgentContract.h"
#include "Core/Discovery/UeAgentDiscovery.h"
#include "Core/ExecutionMutation/UeAgentExecutionMutation.h"
#include "Core/PlanningPreflight/UeAgentPlanningPreflight.h"
#include "Core/StateSemantic/UeAgentStateSemantic.h"
#include "DomainAdapters/GenericEffect/UeAgentGenericEffectAdapter.h"
#include "Engine/World.h"
#include "UObject/Package.h"

namespace
{
	using namespace UE::UAI::ContextResource;
	using namespace UE::UAI::Contract;
	using namespace UE::UAI::Discovery;
	using namespace UE::UAI::DomainAdapters::GenericEffect;
	using namespace UE::UAI::DomainAdapters::WorldSimpleEffect;
	using namespace UE::UAI::ExecutionMutation;
	using namespace UE::UAI::PlanningPreflight;
	using namespace UE::UAI::StateSemantic;

	static_assert(std::is_base_of_v<
		IUeAgentGenericEffectUeHook,
		FUeAgentWorldSimpleEffectUeHook>);

	static TSharedRef<FUeAgentResourceTracker, ESPMode::ThreadSafe>
	MakeTracker()
	{
		return MakeShared<
			FUeAgentResourceTracker,
			ESPMode::ThreadSafe>();
	}

	static FUeAgentCanonicalIdentity MakeIdentity(
		const FString& SemanticKind,
		const FString& LocalId)
	{
		return FUeAgentCanonicalIdentity(
			TEXT("uai.world.simple.") + LocalId.ToLower(),
			TEXT("world_simple_effect"),
			TEXT("world:sa-002"),
			LocalId,
			SemanticKind,
			TEXT("editor-session:sa-002"));
	}

	static FUeAgentCanonicalValueRef MakeStringValue(
		const FString& Text,
		FUeAgentResourceTracker& Resources)
	{
		FUeAgentCanonicalValuePtr Value;
		FUeAgentStateSemanticError Error;
		check(FUeAgentCanonicalValue::TryString(
			Text,
			Resources,
			Value,
			Error));
		return Value.ToSharedRef();
	}

	static FUeAgentCanonicalValueRef MakeNullValue(
		FUeAgentResourceTracker& Resources)
	{
		FUeAgentCanonicalValuePtr Value;
		FUeAgentStateSemanticError Error;
		check(FUeAgentCanonicalValue::TryNull(
			Resources,
			Value,
			Error));
		return Value.ToSharedRef();
	}

	static FUeAgentDescriptorIdentity MakeDescriptorIdentity()
	{
		return FUeAgentDescriptorIdentity(
			TEXT("uai.world.simple.effect/v1"),
			1,
			TEXT(
				"sha256:77777777777777777777777777777777"
				"77777777777777777777777777777777"));
	}

	static FUeAgentCandidateBinding MakeCandidateBinding(
		const FUeAgentCanonicalIdentity& Identity)
	{
		return FUeAgentCandidateBinding(
			TEXT("binding:sa-002:") + Identity.GetCanonicalKey(),
			FUeAgentCandidateSummary(
				Identity,
				MakeDescriptorIdentity(),
				TEXT("world.simple"),
				Identity.GetSelector(),
				TEXT("SA-002 WorldSimpleEffect target"),
				{TEXT("World"), TEXT("SimpleAuthoring")}),
			FUeAgentCatalogRevision(
				TEXT("world-simple-catalog"),
				TEXT("catalog-generation:sa-002"),
				1),
			TEXT("uai://world-target/") + Identity.GetSelector(),
			TEXT("world-read"),
			{
				EUeAgentCandidateOperation::Read,
				EUeAgentCandidateOperation::Create,
				EUeAgentCandidateOperation::Update,
				EUeAgentCandidateOperation::Replace,
				EUeAgentCandidateOperation::Delete,
			},
			TEXT("fixture:sa-002:candidate-binding"));
	}

	static FUeAgentGenericEffectBinding MakeEffectBinding(
		const FUeAgentCanonicalIdentity& Identity)
	{
		return FUeAgentGenericEffectBinding(
			MakeCandidateBinding(Identity),
			TEXT("UE.WorldSimple:FString"),
			FUeAgentCanonicalType(
				TEXT("nullable_string"),
				EUeAgentValueKind::String,
				true,
				TEXT("scalar")),
			TEXT("fixture:sa-002:type-binding"));
	}

	static FPlanningPreflightPlanRef MakePlan(
		TArray<FPlannedChange> Changes)
	{
		TArray<FImpact> Impacts;
		for (const FPlannedChange& Change : Changes)
		{
			Impacts.Emplace(
				Change.GetIdentity(),
				Change.GetIdentity(),
				TEXT("direct"),
				TEXT("fixture:sa-002:direct-impact"));
		}
		return MakeShared<FPlanningPreflightPlan>(
			MakeDescriptorIdentity(),
			TEXT("request:sa-002"),
			TEXT("idempotency:sa-002"),
			TEXT("agent:sa-002"),
			TArray<FString>{TEXT("uai:world:write")},
			TEXT("snapshot:sa-002:before"),
			FUeAgentStateRevision(
				TEXT("world-read"),
				TEXT("editor-session:sa-002"),
				17),
			TEXT("fixture:sa-002:independent-before"),
			FChangeSet(MoveTemp(Changes)),
			FImpactSet(MoveTemp(Impacts)),
			TArray<FPrecondition>(),
			TEXT(
				"sha256:88888888888888888888888888888888"
				"88888888888888888888888888888888"));
	}

	static FUeAgentExecutionContextRefOwner MakeExecutionContext(
		const FString& WorldIdentity)
	{
		FUeAgentExecutionContextInput Input;
		Input.ProjectInstanceId = TEXT("project:sa-002");
		Input.EditorInstanceId = TEXT("editor:sa-002");
		Input.StatePlane = EUeAgentStatePlane::AuthoringMemory;
		Input.WorldType = EUeAgentExecutionWorldType::Pie;
		Input.WorldIdentity = WorldIdentity;
		Input.LevelIdentity =
			WorldIdentity + TEXT(":PersistentLevel");
		Input.PieInstanceId = 2;
		Input.ContextRevision = TEXT("context:sa-002:r5");
		Input.bResolvedFromCurrent = false;

		FUeAgentExecutionContextRefPtr Context;
		FUeAgentExecutionContextError Error;
		check(FUeAgentExecutionContextRef::TryCreate(
			Input,
			MakeDescriptorIdentity(),
			EUeAgentRequestKind::Operation,
			FUeAgentExecutionContextPolicy(
				{EUeAgentStatePlane::AuthoringMemory},
				{
					EUeAgentExecutionContextFacet::World,
					EUeAgentExecutionContextFacet::Level,
					EUeAgentExecutionContextFacet::PieInstance,
				},
				EUeAgentContextResolutionPolicy::ExplicitOnly,
				EUeAgentTransientContextPolicy::
					TemporaryMutationAllowed),
			Context,
			Error));
		return Context.ToSharedRef();
	}

	struct FRequestFixture
	{
		FRequestFixture(
			TArray<FPlannedChange> InChanges,
			const FString& WorldIdentity)
			: Resources(MakeTracker())
			, Cancellation([this]() { return Now; })
			, Plan(MakePlan(MoveTemp(InChanges)))
			, ExecutionContext(
				MakeExecutionContext(WorldIdentity))
		{
		}

		FExecutionMutationRequest MakeRequest()
		{
			return FExecutionMutationRequest(
				TEXT("operation:sa-002"),
				TEXT("owner:sa-002"),
				Plan,
				ExecutionContext,
				Resources,
				FUeAgentOperationDeadline::FromTimeout(
					30.0,
					[this]() { return Now; }),
				Cancellation.GetToken(),
				EExecutionThreadAffinity::GameThread,
				EExecutionReentrancyPolicy::RejectSameTarget);
		}

		double Now = 100.0;
		TSharedRef<FUeAgentResourceTracker, ESPMode::ThreadSafe>
			Resources;
		FUeAgentCancellationSource Cancellation;
		FPlanningPreflightPlanRef Plan;
		FUeAgentExecutionContextRefOwner ExecutionContext;
	};

	class FExactWorldResolver final
		: public IUeAgentWorldSimpleEffectWorldResolver
	{
	public:
		bool TryResolveWorld(
			const FUeAgentExecutionContextRef& Context,
			UWorld*& OutWorld,
			FExecutionMutationError& OutError) override
		{
			++ResolveCalls;
			LastWorldIdentity = Context.GetWorldIdentity();
			OutWorld = Worlds.FindRef(LastWorldIdentity);
			if (!IsValid(OutWorld))
			{
				OutError = FExecutionMutationError(
					TEXT("world_simple_context_unresolved"),
					LastWorldIdentity);
				return false;
			}
			return true;
		}

		TMap<FString, UWorld*> Worlds;
		int32 ResolveCalls = 0;
		FString LastWorldIdentity;
	};

	class FRecordingWorldNativeHook final
		: public IUeAgentWorldSimpleEffectNativeHook
	{
	public:
		bool TryApply(
			UWorld& World,
			const FExecutionMutationRequest& Request,
			const FLockFenceLease& Lease,
			const TArray<FUeAgentGenericEffectBinding>& Bindings,
			FUeAgentResourceTracker& Resources,
			FUeAgentGenericEffectApplyResult& OutResult,
			FExecutionMutationError& OutError) override
		{
			++ApplyCalls;
			LastWorld = &World;
			LastRequest = &Request;
			LastBindings = Bindings;
			if (bFailApply)
			{
				OutError = FExecutionMutationError(
					TEXT("world_simple_native_failed"),
					TEXT("injected native failure"));
				return false;
			}
			OutResult = FUeAgentGenericEffectApplyResult(
				18,
				{TEXT("uai.world.simple.changed")},
				{TEXT("world_state_settled")},
				true,
				TEXT("rollback:sa-002"),
				true);
			return true;
		}

		bool TryRollback(
			UWorld& World,
			const FExecutionMutationRequest& Request,
			const FUeAgentGenericEffectApplyResult& Applied,
			FUeAgentResourceTracker& Resources,
			bool& bOutConfirmed,
			FExecutionMutationError& OutError) override
		{
			++RollbackCalls;
			LastWorld = &World;
			bOutConfirmed = true;
			return true;
		}

		void ReleaseVerifiedOperation(
			const FString& OperationId) override
		{
			++ReleaseCalls;
		}

		bool TryWaitForSettle(
			const FBarrierRequest& Request,
			FUeAgentResourceTracker& Resources,
			FBarrierResult& OutResult,
			FExecutionMutationError& OutError) override
		{
			++WaitCalls;
			OutResult = FBarrierResult(
				Request.GetSubjectGeneration(),
				Request.GetRequiredSignals(),
				TEXT("world-revision:sa-002:18"),
				true);
			return true;
		}

		bool bFailApply = false;
		int32 ApplyCalls = 0;
		int32 RollbackCalls = 0;
		int32 ReleaseCalls = 0;
		int32 WaitCalls = 0;
		UWorld* LastWorld = nullptr;
		const FExecutionMutationRequest* LastRequest = nullptr;
		TArray<FUeAgentGenericEffectBinding> LastBindings;
	};

	static TSharedRef<FUeAgentWorldSimpleEffectUeHook, ESPMode::ThreadSafe>
	MakeWorldHook(
		const TSharedRef<
			FExactWorldResolver,
			ESPMode::ThreadSafe>& Resolver,
		const TSharedRef<
			FRecordingWorldNativeHook,
			ESPMode::ThreadSafe>& NativeHook)
	{
		return MakeShared<
			FUeAgentWorldSimpleEffectUeHook,
			ESPMode::ThreadSafe>(
			Resolver,
			NativeHook);
	}


	static_assert(std::is_base_of_v<
		IUeAgentWorldSimpleEffectWorldResolver,
		FUeAgentWorldSimpleEffectExactWorldResolver>);
	static_assert(std::is_base_of_v<
		IUeAgentWorldSimpleEffectWorldCatalog,
		FUeAgentWorldSimpleEffectEngineWorldCatalog>);

	class FControlledWorldCatalog final
		: public IUeAgentWorldSimpleEffectWorldCatalog
	{
	public:
		bool TryEnumerate(
			TArray<FUeAgentWorldSimpleEffectWorldCandidate>&
				OutCandidates,
			FExecutionMutationError& OutError) override
		{
			++EnumerateCalls;
			OutCandidates.Reset();
			if (bFail)
			{
				OutError = FExecutionMutationError(
					TEXT(
						"world_simple_context_catalog_unavailable"),
					TEXT("injected catalog failure"));
				return false;
			}
			OutCandidates = Candidates;
			return true;
		}

		TArray<FUeAgentWorldSimpleEffectWorldCandidate> Candidates;
		int32 EnumerateCalls = 0;
		bool bFail = false;
	};

	static FUeAgentWorldSimpleEffectWorldCandidate
	MakeWorldCandidate(
		UWorld* World,
		const FUeAgentExecutionContextRef& Context,
		const FString& ContextRevision,
		const TOptional<int32>& PieInstanceId)
	{
		return FUeAgentWorldSimpleEffectWorldCandidate(
			World,
			Context.GetWorldIdentity(),
			Context.GetLevelIdentity(),
			Context.GetWorldType(),
			PieInstanceId,
			ContextRevision);
	}

	static FUeAgentWorldSimpleEffectWorldCandidate
	MakeExactWorldCandidate(
		UWorld* World,
		const FUeAgentExecutionContextRef& Context)
	{
		return MakeWorldCandidate(
			World,
			Context,
			Context.GetContextRevision(),
			Context.GetPieInstanceId());
	}

	static FUeAgentExecutionContextRefOwner
	MakeExecutionContextFromWorldCandidate(
		const FUeAgentWorldSimpleEffectWorldCandidate& Candidate,
		FString WorldIdentity,
		FString ContextRevision,
		TOptional<int32> PieInstanceId)
	{
		FUeAgentExecutionContextInput Input;
		Input.ProjectInstanceId = TEXT("project:sa-003");
		Input.EditorInstanceId = TEXT("editor:sa-003");
		Input.StatePlane = EUeAgentStatePlane::AuthoringMemory;
		Input.WorldType = Candidate.GetWorldType();
		Input.WorldIdentity = MoveTemp(WorldIdentity);
		Input.LevelIdentity = Candidate.GetLevelIdentity();
		Input.PieInstanceId = MoveTemp(PieInstanceId);
		Input.ContextRevision = MoveTemp(ContextRevision);
		Input.bResolvedFromCurrent = false;

		TArray<EUeAgentExecutionContextFacet> RequiredFacets{
			EUeAgentExecutionContextFacet::World,
			EUeAgentExecutionContextFacet::Level,
		};
		if (Candidate.GetWorldType()
			== EUeAgentExecutionWorldType::Pie)
		{
			RequiredFacets.Add(
				EUeAgentExecutionContextFacet::PieInstance);
		}

		FUeAgentExecutionContextRefPtr Context;
		FUeAgentExecutionContextError Error;
		check(FUeAgentExecutionContextRef::TryCreate(
			Input,
			MakeDescriptorIdentity(),
			EUeAgentRequestKind::Operation,
			FUeAgentExecutionContextPolicy(
				{EUeAgentStatePlane::AuthoringMemory},
				MoveTemp(RequiredFacets),
				EUeAgentContextResolutionPolicy::ExplicitOnly,
				EUeAgentTransientContextPolicy::
					TemporaryMutationAllowed),
			Context,
			Error));
		return Context.ToSharedRef();
	}

}


IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUeAgentWorldSimpleEffectModuleAvailableTest,
	"UeAgentInterface.DomainAdapters.WorldSimpleEffect.Adapter.ModuleAvailable",
	EAutomationTestFlags::EditorContext
		| EAutomationTestFlags::EngineFilter)

bool FUeAgentWorldSimpleEffectModuleAvailableTest::RunTest(
	const FString& Parameters)
{
	return true;
}


IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUeAgentWorldSimpleEffectTypedSemanticsTest,
	"UeAgentInterface.DomainAdapters.WorldSimpleEffect.Adapter.TypedSemantics",
	EAutomationTestFlags::EditorContext
		| EAutomationTestFlags::EngineFilter)

bool FUeAgentWorldSimpleEffectTypedSemanticsTest::RunTest(
	const FString& Parameters)
{
	const FUeAgentCanonicalIdentity Omitted =
		MakeIdentity(TEXT("property"), TEXT("OmittedLabel"));
	const FUeAgentCanonicalIdentity ExplicitNull =
		MakeIdentity(TEXT("property"), TEXT("NullableLabel"));
	const FUeAgentCanonicalIdentity ClearedField =
		MakeIdentity(TEXT("property"), TEXT("ClearedTag"));
	const FUeAgentCanonicalIdentity DeletedComponent =
		MakeIdentity(TEXT("component"), TEXT("DeletedComponent"));
	const TSharedRef<FUeAgentResourceTracker, ESPMode::ThreadSafe>
		ValueResources = MakeTracker();

	TArray<FPlannedChange> Changes;
	Changes.Emplace(
		ExplicitNull,
		EChangeIntent::Update,
		MakeStringValue(TEXT("before"), *ValueResources),
		MakeNullValue(*ValueResources),
		TArray<FString>{TEXT("uai:world:write")},
		TEXT("fixture:sa-002:direct-impact"));
	Changes.Emplace(
		ClearedField,
		EChangeIntent::Delete,
		MakeStringValue(TEXT("tag"), *ValueResources),
		FUeAgentCanonicalValuePtr(),
		TArray<FString>{TEXT("uai:world:write")},
		TEXT("fixture:sa-002:direct-impact"));
	Changes.Emplace(
		DeletedComponent,
		EChangeIntent::Delete,
		MakeStringValue(TEXT("component-state"), *ValueResources),
		FUeAgentCanonicalValuePtr(),
		TArray<FString>{TEXT("uai:world:write")},
		TEXT("fixture:sa-002:direct-impact"));

	const FString WorldIdentity = TEXT("/Game/UAI/SA_002_PIE_2");
	FRequestFixture Fixture(MoveTemp(Changes), WorldIdentity);
	FExecutionMutationRequest Request = Fixture.MakeRequest();
	UWorld* SelectedWorld = NewObject<UWorld>(
		GetTransientPackage(),
		NAME_None,
		RF_Transient);
	const TSharedRef<FExactWorldResolver, ESPMode::ThreadSafe>
		Resolver = MakeShared<
			FExactWorldResolver,
			ESPMode::ThreadSafe>();
	Resolver->Worlds.Add(WorldIdentity, SelectedWorld);
	const TSharedRef<
		FRecordingWorldNativeHook,
		ESPMode::ThreadSafe> NativeHook = MakeShared<
			FRecordingWorldNativeHook,
			ESPMode::ThreadSafe>();
	const TSharedRef<
		FUeAgentWorldSimpleEffectUeHook,
		ESPMode::ThreadSafe> Hook =
		MakeWorldHook(Resolver, NativeHook);

	FUeAgentGenericEffectApplyResult Result;
	FExecutionMutationError Error;
	const bool bApplied = Hook->TryApply(
		Request,
		FLockFenceLease(
			TEXT("owner:sa-002"),
			{TEXT("world:sa-002")},
			9),
		{
			MakeEffectBinding(ExplicitNull),
			MakeEffectBinding(ClearedField),
			MakeEffectBinding(DeletedComponent),
		},
		*Fixture.Resources,
		Result,
		Error);

	TestTrue(TEXT("typed world mutation applies"), bApplied);
	TestEqual(
		TEXT("resolver consumes exact request world identity"),
		Resolver->LastWorldIdentity,
		WorldIdentity);
	TestEqual(
		TEXT("native hook receives selected world"),
		NativeHook->LastWorld,
		SelectedWorld);
	TestEqual(
		TEXT("adapter neither invents omitted change nor merges actions"),
		NativeHook->LastRequest->GetPlan()
			->GetChangeSet().GetChanges().Num(),
		3);

	const TArray<FPlannedChange>& Observed =
		NativeHook->LastRequest->GetPlan()
			->GetChangeSet().GetChanges();
	TestEqual(
		TEXT("explicit null remains update"),
		Observed[0].GetIntent(),
		EChangeIntent::Update);
	TestEqual(
		TEXT("explicit null remains canonical null"),
		Observed[0].GetAfter()->GetKind(),
		EUeAgentValueKind::Null);
	TestEqual(
		TEXT("field clear remains delete at property identity"),
		Observed[1].GetIdentity().GetObjectRole(),
		FString(TEXT("property")));
	TestEqual(
		TEXT("field clear intent"),
		Observed[1].GetIntent(),
		EChangeIntent::Delete);
	TestFalse(
		TEXT("field clear has no replacement value"),
		Observed[1].GetAfter().IsValid());
	TestEqual(
		TEXT("object delete remains delete at component identity"),
		Observed[2].GetIdentity().GetObjectRole(),
		FString(TEXT("component")));
	TestEqual(
		TEXT("object delete intent"),
		Observed[2].GetIntent(),
		EChangeIntent::Delete);
	TestFalse(
		TEXT("omitted identity is absent from the change set"),
		Observed.ContainsByPredicate(
			[&Omitted](const FPlannedChange& Change)
			{
				return Change.GetIdentity().GetCanonicalKey()
					== Omitted.GetCanonicalKey();
			}));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUeAgentWorldSimpleEffectResolveFailureTest,
	"UeAgentInterface.DomainAdapters.WorldSimpleEffect.Adapter.ResolveFailureStopsMutation",
	EAutomationTestFlags::EditorContext
		| EAutomationTestFlags::EngineFilter)

bool FUeAgentWorldSimpleEffectResolveFailureTest::RunTest(
	const FString& Parameters)
{
	const FUeAgentCanonicalIdentity Identity =
		MakeIdentity(TEXT("property"), TEXT("Label"));
	const TSharedRef<FUeAgentResourceTracker, ESPMode::ThreadSafe>
		ValueResources = MakeTracker();
	TArray<FPlannedChange> Changes;
	Changes.Emplace(
		Identity,
		EChangeIntent::Update,
		MakeStringValue(TEXT("before"), *ValueResources),
		MakeStringValue(TEXT("after"), *ValueResources),
		TArray<FString>{TEXT("uai:world:write")},
		TEXT("fixture:sa-002:direct-impact"));
	FRequestFixture Fixture(
		MoveTemp(Changes),
		TEXT("/Game/UAI/MissingWorld_PIE_2"));
	FExecutionMutationRequest Request = Fixture.MakeRequest();
	const TSharedRef<FExactWorldResolver, ESPMode::ThreadSafe>
		Resolver = MakeShared<
			FExactWorldResolver,
			ESPMode::ThreadSafe>();
	const TSharedRef<
		FRecordingWorldNativeHook,
		ESPMode::ThreadSafe> NativeHook = MakeShared<
			FRecordingWorldNativeHook,
			ESPMode::ThreadSafe>();
	const TSharedRef<
		FUeAgentWorldSimpleEffectUeHook,
		ESPMode::ThreadSafe> Hook =
		MakeWorldHook(Resolver, NativeHook);

	FUeAgentGenericEffectApplyResult Result;
	FExecutionMutationError Error;
	const bool bApplied = Hook->TryApply(
		Request,
		FLockFenceLease(
			TEXT("owner:sa-002"),
			{TEXT("world:sa-002")},
			10),
		{MakeEffectBinding(Identity)},
		*Fixture.Resources,
		Result,
		Error);

	TestFalse(TEXT("unknown explicit world fails"), bApplied);
	TestEqual(
		TEXT("world is resolved exactly once"),
		Resolver->ResolveCalls,
		1);
	TestEqual(
		TEXT("native mutation is not entered"),
		NativeHook->ApplyCalls,
		0);
	TestFalse(
		TEXT("failed resolution does not report mutation"),
		Result.DidMutationStart());
	TestEqual(
		TEXT("failure is precise"),
		Error.GetCode(),
		FString(TEXT("world_simple_context_unresolved")));
	return true;
}


IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUeAgentWorldSimpleEffectExactProductionResolverTest,
	"UeAgentInterface.DomainAdapters.WorldSimpleEffect.Production.ExactResolution",
	EAutomationTestFlags::EditorContext
		| EAutomationTestFlags::EngineFilter)

bool FUeAgentWorldSimpleEffectExactProductionResolverTest::RunTest(
	const FString& Parameters)
{
	const FString WorldIdentity =
		TEXT("/Game/UAI/SA_002C_PIE_2");
	const FUeAgentExecutionContextRefOwner Context =
		MakeExecutionContext(WorldIdentity);
	UWorld* SelectedWorld = NewObject<UWorld>(
		GetTransientPackage(),
		NAME_None,
		RF_Transient);
	UWorld* OtherPieWorld = NewObject<UWorld>(
		GetTransientPackage(),
		NAME_None,
		RF_Transient);
	const TSharedRef<
		FControlledWorldCatalog,
		ESPMode::ThreadSafe> Catalog = MakeShared<
			FControlledWorldCatalog,
			ESPMode::ThreadSafe>();
	Catalog->Candidates.Add(
		MakeWorldCandidate(
			OtherPieWorld,
			*Context,
			Context->GetContextRevision(),
			3));
	Catalog->Candidates.Add(
		MakeExactWorldCandidate(
			SelectedWorld,
			*Context));
	const TSharedRef<
		FUeAgentWorldSimpleEffectExactWorldResolver,
		ESPMode::ThreadSafe> Resolver = MakeShared<
			FUeAgentWorldSimpleEffectExactWorldResolver,
			ESPMode::ThreadSafe>(Catalog);

	UWorld* ResolvedWorld = nullptr;
	FExecutionMutationError Error;
	const bool bResolved = Resolver->TryResolveWorld(
		*Context,
		ResolvedWorld,
		Error);

	TestTrue(
		TEXT("exact frozen context resolves"),
		bResolved);
	TestEqual(
		TEXT("PIE instance is part of exact identity"),
		ResolvedWorld,
		SelectedWorld);
	TestEqual(
		TEXT("resolver enumerates the production catalog once"),
		Catalog->EnumerateCalls,
		1);
	TestTrue(
		TEXT("success has no error"),
		Error.GetCode().IsEmpty());
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUeAgentWorldSimpleEffectEngineCatalogRoundTripTest,
	"UeAgentInterface.DomainAdapters.WorldSimpleEffect.Production.EngineCatalogRoundTrip",
	EAutomationTestFlags::EditorContext
		| EAutomationTestFlags::EngineFilter)

bool FUeAgentWorldSimpleEffectEngineCatalogRoundTripTest::RunTest(
	const FString& Parameters)
{
	if (!TestTrue(
		TEXT("engine catalog test owns the game thread"),
		IsInGameThread()))
	{
		return false;
	}

	const TSharedRef<
		FUeAgentWorldSimpleEffectEngineWorldCatalog,
		ESPMode::ThreadSafe> Catalog = MakeShared<
			FUeAgentWorldSimpleEffectEngineWorldCatalog,
			ESPMode::ThreadSafe>();
	TArray<FUeAgentWorldSimpleEffectWorldCandidate> Candidates;
	FExecutionMutationError CatalogError;
	if (!TestTrue(
		TEXT("real engine world catalog enumerates"),
		Catalog->TryEnumerate(Candidates, CatalogError)))
	{
		AddError(
			TEXT("catalog error: ") + CatalogError.GetCode());
		return false;
	}

	const FUeAgentWorldSimpleEffectWorldCandidate* Selected = nullptr;
	for (const FUeAgentWorldSimpleEffectWorldCandidate& Candidate
		: Candidates)
	{
		if (Candidate.IsValid())
		{
			Selected = &Candidate;
			break;
		}
	}
	if (!TestNotNull(
		TEXT("owned automation worker exposes a valid world candidate"),
		Selected))
	{
		return false;
	}

	const FUeAgentExecutionContextRefOwner ExactContext =
		MakeExecutionContextFromWorldCandidate(
			*Selected,
			Selected->GetWorldIdentity(),
			Selected->GetContextRevision(),
			Selected->GetPieInstanceId());
	const TSharedRef<
		FUeAgentWorldSimpleEffectExactWorldResolver,
		ESPMode::ThreadSafe> Resolver = MakeShared<
			FUeAgentWorldSimpleEffectExactWorldResolver,
			ESPMode::ThreadSafe>(Catalog);

	UWorld* ResolvedWorld = nullptr;
	FExecutionMutationError ResolveError;
	TestTrue(
		TEXT("catalog candidate round-trips through frozen context"),
		Resolver->TryResolveWorld(
			*ExactContext,
			ResolvedWorld,
			ResolveError));
	TestEqual(
		TEXT("round-trip resolves the same live world"),
		ResolvedWorld,
		Selected->GetWorld());

	const FUeAgentExecutionContextRefOwner StaleContext =
		MakeExecutionContextFromWorldCandidate(
			*Selected,
			Selected->GetWorldIdentity(),
			Selected->GetContextRevision() + TEXT(":stale"),
			Selected->GetPieInstanceId());
	ResolvedWorld = Selected->GetWorld();
	ResolveError = FExecutionMutationError();
	TestFalse(
		TEXT("changed catalog revision is rejected"),
		Resolver->TryResolveWorld(
			*StaleContext,
			ResolvedWorld,
			ResolveError));
	TestNull(
		TEXT("stale resolution clears output"),
		ResolvedWorld);
	TestEqual(
		TEXT("changed catalog revision is precisely stale"),
		ResolveError.GetCode(),
		FString(TEXT("world_simple_context_stale")));

	const FUeAgentExecutionContextRefOwner UnknownContext =
		MakeExecutionContextFromWorldCandidate(
			*Selected,
			Selected->GetWorldIdentity() + TEXT(":missing"),
			Selected->GetContextRevision(),
			Selected->GetPieInstanceId());
	ResolvedWorld = Selected->GetWorld();
	ResolveError = FExecutionMutationError();
	TestFalse(
		TEXT("changed identity has no current-world fallback"),
		Resolver->TryResolveWorld(
			*UnknownContext,
			ResolvedWorld,
			ResolveError));
	TestNull(
		TEXT("unresolved identity clears output"),
		ResolvedWorld);
	TestEqual(
		TEXT("changed identity is precisely unresolved"),
		ResolveError.GetCode(),
		FString(TEXT("world_simple_context_unresolved")));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUeAgentWorldSimpleEffectProductionResolverFailureTest,
	"UeAgentInterface.DomainAdapters.WorldSimpleEffect.Production.FailureSemantics",
	EAutomationTestFlags::EditorContext
		| EAutomationTestFlags::EngineFilter)

bool FUeAgentWorldSimpleEffectProductionResolverFailureTest::RunTest(
	const FString& Parameters)
{
	const FUeAgentExecutionContextRefOwner Context =
		MakeExecutionContext(
			TEXT("/Game/UAI/SA_002C_PIE_2"));

	auto ResolveWith = [this, &Context](
		const TSharedRef<
			FControlledWorldCatalog,
			ESPMode::ThreadSafe>& Catalog,
		const FString& ExpectedCode,
		const FString& Label)
	{
		const TSharedRef<
			FUeAgentWorldSimpleEffectExactWorldResolver,
			ESPMode::ThreadSafe> Resolver = MakeShared<
				FUeAgentWorldSimpleEffectExactWorldResolver,
				ESPMode::ThreadSafe>(Catalog);
		UWorld* ResolvedWorld = NewObject<UWorld>(
			GetTransientPackage(),
			NAME_None,
			RF_Transient);
		FExecutionMutationError Error;
		const bool bResolved = Resolver->TryResolveWorld(
			*Context,
			ResolvedWorld,
			Error);
		TestFalse(*Label, bResolved);
		TestNull(
			*(Label + TEXT(" clears output world")),
			ResolvedWorld);
		TestEqual(
			*(Label + TEXT(" returns precise error")),
			Error.GetCode(),
			ExpectedCode);
	};

	const TSharedRef<
		FControlledWorldCatalog,
		ESPMode::ThreadSafe> InvalidCatalog = MakeShared<
			FControlledWorldCatalog,
			ESPMode::ThreadSafe>();
	InvalidCatalog->Candidates.Add(
		MakeExactWorldCandidate(nullptr, *Context));
	ResolveWith(
		InvalidCatalog,
		TEXT("world_simple_context_invalid"),
		TEXT("invalid matching world is rejected"));

	UWorld* StaleWorld = NewObject<UWorld>(
		GetTransientPackage(),
		NAME_None,
		RF_Transient);
	const TSharedRef<
		FControlledWorldCatalog,
		ESPMode::ThreadSafe> StaleCatalog = MakeShared<
			FControlledWorldCatalog,
			ESPMode::ThreadSafe>();
	StaleCatalog->Candidates.Add(
		MakeWorldCandidate(
			StaleWorld,
			*Context,
			TEXT("context:sa-002:r4"),
			Context->GetPieInstanceId()));
	ResolveWith(
		StaleCatalog,
		TEXT("world_simple_context_stale"),
		TEXT("stale matching world is rejected"));

	UWorld* FirstWorld = NewObject<UWorld>(
		GetTransientPackage(),
		NAME_None,
		RF_Transient);
	UWorld* SecondWorld = NewObject<UWorld>(
		GetTransientPackage(),
		NAME_None,
		RF_Transient);
	const TSharedRef<
		FControlledWorldCatalog,
		ESPMode::ThreadSafe> AmbiguousCatalog = MakeShared<
			FControlledWorldCatalog,
			ESPMode::ThreadSafe>();
	AmbiguousCatalog->Candidates.Add(
		MakeExactWorldCandidate(FirstWorld, *Context));
	AmbiguousCatalog->Candidates.Add(
		MakeExactWorldCandidate(SecondWorld, *Context));
	ResolveWith(
		AmbiguousCatalog,
		TEXT("world_simple_context_ambiguous"),
		TEXT("duplicate exact worlds are rejected"));

	const FUeAgentExecutionContextRefOwner OtherContext =
		MakeExecutionContext(
			TEXT("/Game/UAI/OtherWorld_PIE_2"));
	UWorld* OtherWorld = NewObject<UWorld>(
		GetTransientPackage(),
		NAME_None,
		RF_Transient);
	const TSharedRef<
		FControlledWorldCatalog,
		ESPMode::ThreadSafe> UnresolvedCatalog = MakeShared<
			FControlledWorldCatalog,
			ESPMode::ThreadSafe>();
	UnresolvedCatalog->Candidates.Add(
		MakeExactWorldCandidate(OtherWorld, *OtherContext));
	ResolveWith(
		UnresolvedCatalog,
		TEXT("world_simple_context_unresolved"),
		TEXT("nonmatching available world is not a fallback"));

	const TSharedRef<
		FControlledWorldCatalog,
		ESPMode::ThreadSafe> UnavailableCatalog = MakeShared<
			FControlledWorldCatalog,
			ESPMode::ThreadSafe>();
	UnavailableCatalog->bFail = true;
	ResolveWith(
		UnavailableCatalog,
		TEXT("world_simple_context_catalog_unavailable"),
		TEXT("catalog failure is propagated"));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUeAgentWorldSimpleEffectProductionHostTest,
	"UeAgentInterface.DomainAdapters.WorldSimpleEffect.Production.HostBinding",
	EAutomationTestFlags::EditorContext
		| EAutomationTestFlags::EngineFilter)

bool FUeAgentWorldSimpleEffectProductionHostTest::RunTest(
	const FString& Parameters)
{
	const FString WorldIdentity =
		TEXT("/Game/UAI/SA_002C_PIE_2");
	const FUeAgentCanonicalIdentity Identity =
		MakeIdentity(TEXT("property"), TEXT("Label"));
	const TSharedRef<FUeAgentResourceTracker, ESPMode::ThreadSafe>
		ValueResources = MakeTracker();
	TArray<FPlannedChange> Changes;
	Changes.Emplace(
		Identity,
		EChangeIntent::Update,
		MakeStringValue(TEXT("before"), *ValueResources),
		MakeStringValue(TEXT("after"), *ValueResources),
		TArray<FString>{TEXT("uai:world:write")},
		TEXT("fixture:sa-002c:direct-impact"));
	FRequestFixture Fixture(MoveTemp(Changes), WorldIdentity);
	FExecutionMutationRequest Request = Fixture.MakeRequest();

	UWorld* SelectedWorld = NewObject<UWorld>(
		GetTransientPackage(),
		NAME_None,
		RF_Transient);
	const TSharedRef<
		FControlledWorldCatalog,
		ESPMode::ThreadSafe> Catalog = MakeShared<
			FControlledWorldCatalog,
			ESPMode::ThreadSafe>();
	Catalog->Candidates.Add(
		MakeExactWorldCandidate(
			SelectedWorld,
			Request.GetExecutionContext()));
	const TSharedRef<
		FRecordingWorldNativeHook,
		ESPMode::ThreadSafe> NativeHook = MakeShared<
			FRecordingWorldNativeHook,
			ESPMode::ThreadSafe>();
	const TSharedRef<
		FUeAgentWorldSimpleEffectUeHook,
		ESPMode::ThreadSafe> Hook =
			FUeAgentWorldSimpleEffectProductionHost::Create(
				Catalog,
				NativeHook);

	FUeAgentGenericEffectApplyResult Result;
	FExecutionMutationError Error;
	const bool bApplied = Hook->TryApply(
		Request,
		FLockFenceLease(
			TEXT("owner:sa-002"),
			{TEXT("world:sa-002")},
			11),
		{MakeEffectBinding(Identity)},
		*Fixture.Resources,
		Result,
		Error);

	TestTrue(
		TEXT("production host applies with exact context"),
		bApplied);
	TestEqual(
		TEXT("production host injects the exact selected world"),
		NativeHook->LastWorld,
		SelectedWorld);
	TestEqual(
		TEXT("production host enters native mutation once"),
		NativeHook->ApplyCalls,
		1);

	TArray<FPlannedChange> MissingChanges;
	MissingChanges.Emplace(
		Identity,
		EChangeIntent::Update,
		MakeStringValue(TEXT("before"), *ValueResources),
		MakeStringValue(TEXT("after"), *ValueResources),
		TArray<FString>{TEXT("uai:world:write")},
		TEXT("fixture:sa-002c:direct-impact"));
	FRequestFixture MissingFixture(
		MoveTemp(MissingChanges),
		TEXT("/Game/UAI/MissingWorld_PIE_2"));
	FExecutionMutationRequest MissingRequest =
		MissingFixture.MakeRequest();
	FUeAgentGenericEffectApplyResult MissingResult;
	FExecutionMutationError MissingError;
	const bool bMissingApplied = Hook->TryApply(
		MissingRequest,
		FLockFenceLease(
			TEXT("owner:sa-002"),
			{TEXT("world:sa-002")},
			12),
		{MakeEffectBinding(Identity)},
		*MissingFixture.Resources,
		MissingResult,
		MissingError);

	TestFalse(
		TEXT("production host rejects unknown context"),
		bMissingApplied);
	TestEqual(
		TEXT("failed exact resolution stops before native mutation"),
		NativeHook->ApplyCalls,
		1);
	TestFalse(
		TEXT("failed exact resolution reports no mutation"),
		MissingResult.DidMutationStart());
	TestEqual(
		TEXT("production host preserves resolver error"),
		MissingError.GetCode(),
		FString(TEXT("world_simple_context_unresolved")));
	return true;
}



IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUeAgentWorldSimpleEffectProductionModuleAvailableTest,
	"UeAgentInterface.DomainAdapters.WorldSimpleEffect.Production.ModuleAvailable",
	EAutomationTestFlags::EditorContext
		| EAutomationTestFlags::EngineFilter)

bool FUeAgentWorldSimpleEffectProductionModuleAvailableTest::RunTest(
	const FString& Parameters)
{
	return true;
}
