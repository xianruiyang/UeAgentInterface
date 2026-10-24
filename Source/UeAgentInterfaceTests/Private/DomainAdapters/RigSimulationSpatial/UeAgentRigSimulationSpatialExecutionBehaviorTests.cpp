#include "Misc/AutomationTest.h"

#include "DomainAdapters/RigSimulationSpatial/UeAgentRigSimulationSpatialDescriptors.h"
#include "DomainAdapters/RigSimulationSpatial/UeAgentRigSimulationSpatialExecutionChains.h"


#include "Core/BindingGateway/Operation/UeAgentOperationBinding.h"
#include "Core/BindingGateway/Query/UeAgentDomainQuery.h"
#include "Core/BindingGateway/Query/UeAgentQueryBindingError.h"
#include "Core/ContextResource/UeAgentOperationContext.h"
#include "Core/Contract/UeAgentContract.h"
#include "Core/Contract/UeAgentDescriptorRegistry.h"
#include "Core/Discovery/UeAgentDiscovery.h"
#include "Core/ExecutionMutation/UeAgentExecutionMutation.h"
#include "Core/PlanningPreflight/UeAgentPlanningPreflight.h"
#include "Core/Projection/UeAgentProjection.h"
#include "Core/StateSemantic/UeAgentStateSemantic.h"
#include "Core/Verification/UeAgentVerification.h"

namespace
{
	using namespace UE::UAI::BindingGateway::Operation;
	using namespace UE::UAI::BindingGateway::Query;
	using namespace UE::UAI::BindingGateway::Ticket;
	using namespace UE::UAI::ContextResource;
	using namespace UE::UAI::Contract;
	using namespace UE::UAI::Discovery;
	using namespace UE::UAI::DomainAdapters::RigSimulationSpatial;
	using namespace UE::UAI::ExecutionMutation;
	using namespace UE::UAI::PlanningPreflight;
	using namespace UE::UAI::Projection;
	using namespace UE::UAI::StateSemantic;
	using namespace UE::UAI::Verification;

	FUeAgentResourceTrackerOwner MakeResources()
	{
		return MakeShared<FUeAgentResourceTracker, ESPMode::ThreadSafe>();
	}

	FUeAgentExecutionContextInput MakeExecutionInput(
		EUeAgentStatePlane StatePlane)
	{
		FUeAgentExecutionContextInput Input;
		Input.ProjectInstanceId = TEXT("rig-simulation-spatial-project");
		Input.EditorInstanceId = TEXT("rig-simulation-spatial-editor");
		Input.StatePlane = StatePlane;
		Input.WorldType = EUeAgentExecutionWorldType::Editor;
		Input.WorldIdentity = TEXT("/Game/Maps/RigSimulationSpatialFixture");
		Input.LevelIdentity = TEXT("PersistentLevel");
		Input.DataLayerIdentities = {TEXT("RigSimulationSpatialFixtureLayer")};
		Input.WorldPartitionIdentity = TEXT("world-partition:fixture");
		Input.PieInstanceId = 0;
		Input.NetMode = TEXT("standalone");
		Input.NetRole = TEXT("authority");
		Input.AssetEditorIdentity = TEXT("asset-editor:fixture");
		Input.ViewportIdentity = TEXT("viewport:fixture");
		Input.SelectionIdentities = {TEXT("actor:fixture")};
		Input.ContextRevision = TEXT("rig-simulation-spatial-context-revision-1");
		Input.bResolvedFromCurrent = false;
		return Input;
	}

	TSharedPtr<FUeAgentOperationContext> MakeContext(
		FAutomationTestBase* Test,
		const FUeAgentOperationDescriptor& Descriptor,
		EUeAgentRequestKind RequestKind,
		const FString& RequestId,
		const FUeAgentResourceTrackerOwner& Resources)
	{
		const TArray<EUeAgentStatePlane>& Planes =
			Descriptor.GetExecutionContextPolicy().GetAllowedStatePlanes();
		if (!Test->TestTrue(TEXT("descriptor has a state plane"), !Planes.IsEmpty()))
		{
			return nullptr;
		}

		FUeAgentExecutionContextRefPtr ExecutionContext;
		FUeAgentExecutionContextError ContextError;
		if (!Test->TestTrue(
				TEXT("freeze explicit RigSimulationSpatial context"),
				FUeAgentExecutionContextRef::TryCreate(
					MakeExecutionInput(Planes[0]),
					Descriptor.GetIdentity(),
					RequestKind,
					Descriptor.GetExecutionContextPolicy(),
					ExecutionContext,
					ContextError)))
		{
			return nullptr;
		}

		const FUeAgentCallerContext Caller(
			TEXT("rig-simulation-spatial-fixture-agent"),
			{TEXT("rig-simulation-spatial:read"), TEXT("rig-simulation-spatial:write")},
			TEXT("corr-") + RequestId);
		const FUeAgentOperationDeadline Deadline =
			FUeAgentOperationDeadline::FromTimeout(30.0, []() { return 100.0; });
		const FUeAgentCancellationSource Cancellation([]() { return 100.0; });
		TSharedPtr<FUeAgentOperationContext> Context;
		FUeAgentOperationContextError OperationError;
		if (RequestKind == EUeAgentRequestKind::Query)
		{
			const FUeAgentQueryRequest Request(
				Descriptor.GetIdentity(),
				RequestId,
				FUeAgentContractValue(TEXT("{}")));
			Test->TestTrue(
				TEXT("create query OperationContext"),
				FUeAgentOperationContext::TryCreate(
					Request,
					Caller,
					ExecutionContext.ToSharedRef(),
					Resources,
					Deadline,
					Cancellation.GetToken(),
					Context,
					OperationError));
		}
		else
		{
			const FUeAgentOperationRequest Request(
				Descriptor.GetIdentity(),
				RequestId,
				TEXT("idem-") + RequestId,
				FUeAgentContractValue(TEXT("{}")));
			Test->TestTrue(
				TEXT("create operation OperationContext"),
				FUeAgentOperationContext::TryCreate(
					Request,
					Caller,
					ExecutionContext.ToSharedRef(),
					Resources,
					Deadline,
					Cancellation.GetToken(),
					Context,
					OperationError));
		}
		return Context;
	}

	TSharedPtr<const FUeAgentCanonicalSnapshot> MakeSnapshot(
		FAutomationTestBase* Test,
		const FString& CapabilityId,
		FUeAgentResourceTracker& Resources,
		int64 Sequence)
	{
		const FUeAgentStateRevision Revision(
			TEXT("rig-simulation-spatial-readback"),
			TEXT("generation-1"),
			Sequence);
		FUeAgentCanonicalValuePtr Value;
		FUeAgentStateSemanticError Error;
		if (!Test->TestTrue(
				TEXT("create independent readback value"),
				FUeAgentCanonicalValue::TryString(
					CapabilityId + TEXT(":observed"),
					Resources,
					Value,
					Error)))
		{
			return nullptr;
		}

		const FUeAgentStateEntry Entry(
			FUeAgentCanonicalIdentity(
				TEXT("rig-simulation-spatial:") + CapabilityId,
				TEXT("rig_simulation_spatial_observation"),
				TEXT("/Game/RigSimulationSpatialFixture"),
				CapabilityId,
				TEXT("readback"),
				TEXT("generation-1")),
			FUeAgentCanonicalType(
				TEXT("core.string"),
				EUeAgentValueKind::String,
				false,
				TEXT("scalar")),
			Value.ToSharedRef(),
			FUeAgentDefaultValue(
				EUeAgentDefaultSourceKind::DomainNative,
				TEXT("rig-simulation-spatial-native"),
				TEXT("independent-readback"),
				Value.ToSharedRef()),
			FUeAgentStateDetails(
				TEXT("RigSimulationSpatial readback"),
				TEXT("independent fake-port observation"),
				{TEXT("RigSimulationSpatial"), TEXT("Readback")},
				false,
				TEXT("available"),
				{}),
			Revision);

		TSharedPtr<const FUeAgentCanonicalSnapshot> Snapshot;
		Test->TestTrue(
			TEXT("create independent readback snapshot"),
			FUeAgentCanonicalSnapshot::TryCreate(
				TEXT("rig-simulation-spatial-snapshot-") + CapabilityId,
				EUeAgentStatePlane::AuthoringMemory,
				Revision,
				FUeAgentTemporalPoint(TEXT("fixture-clock"), TEXT("1")),
				FUeAgentStateProvenance(
					TEXT("independent_readback"),
					TEXT("rig-simulation-spatial-fake-port"),
					TEXT("fixture-evidence")),
				{Entry},
				Resources,
				Snapshot,
				Error));
		return Snapshot;
	}

	EUeAgentRigSimulationSpatialPortFamily PortFamilyFor(
		const FString& FamilyId)
	{
		if (FamilyId == TEXT("skeletal_attachment"))
		{
			return EUeAgentRigSimulationSpatialPortFamily::SkeletalAttachment;
		}
		if (FamilyId == TEXT("rig_bake"))
		{
			return EUeAgentRigSimulationSpatialPortFamily::RigBake;
		}
		if (FamilyId == TEXT("ik_retarget"))
		{
			return EUeAgentRigSimulationSpatialPortFamily::IkRetarget;
		}
		if (FamilyId == TEXT("simulation_definition"))
		{
			return EUeAgentRigSimulationSpatialPortFamily::SimulationDefinition;
		}
		return EUeAgentRigSimulationSpatialPortFamily::SimulationObservation;
	}

	class FFixtureTypedRequest final
		: public IUeAgentRigSimulationSpatialTypedRequest
	{
	public:
		explicit FFixtureTypedRequest(
			const FUeAgentRigSimulationSpatialExecutionChainContract& Contract)
			: TypeId(Contract.GetTypedRequestId())
			, FamilyId(Contract.GetFamilyId())
			, PortRequest(
				PortFamilyFor(Contract.GetFamilyId()),
				{},
				{},
				{},
				{},
				{TEXT("fixture_explicit_space_contract")})
		{
		}

		const FString& GetTypeId() const override { return TypeId; }
		const FString& GetFamilyId() const override { return FamilyId; }
		const FString& GetRequestKindId() const override { return TypeId; }
		const FUeAgentRigSimulationSpatialPortRequest&
		GetFormalPortRequest() const override
		{
			return PortRequest;
		}

		bool TryBuildChangeDraft(
			const FUeAgentCanonicalSnapshot& Snapshot,
			const FUeAgentOperationContext& Context,
			TOptional<FChangeDraft>& OutDraft,
			FUeAgentRigSimulationSpatialExecutionError& OutError) const override
		{
			OutDraft.Reset();
			if (Snapshot.GetEntries().IsEmpty())
			{
				OutError.Code = TEXT("fixture_rig_simulation_spatial_preflight_empty");
				return false;
			}
			const FUeAgentStateEntry& Entry = Snapshot.GetEntries()[0];
			FUeAgentCanonicalValuePtr Proposed;
			FUeAgentStateSemanticError ValueError;
			if (!FUeAgentCanonicalValue::TryString(
					TEXT("desired:") + TypeId,
					Context.GetResourceTracker(),
					Proposed,
					ValueError))
			{
				OutError.Code = TEXT("fixture_rig_simulation_spatial_proposed_value_failed");
				return false;
			}
			OutDraft.Emplace(
				Entry.GetIdentity(),
				EChangeIntent::Update,
				Entry.GetValue(),
				Proposed,
				TArray<FString>(),
				TEXT("fixture-rig-simulation-spatial-impact"),
				TArray<FUeAgentCanonicalIdentity>{Entry.GetIdentity()});
			return OutDraft->IsValid();
		}

	private:
		FString TypeId;
		FString FamilyId;
		FUeAgentRigSimulationSpatialPortRequest PortRequest;
	};

	class FRecordingDecoder final
		: public IUeAgentRigSimulationSpatialRequestDecoder
	{
	public:
		explicit FRecordingDecoder(TArray<FString>* InTrace)
			: Trace(InTrace)
		{
		}

		bool TryDecode(
			const FUeAgentRigSimulationSpatialExecutionChainContract& Contract,
			const FUeAgentContractValue& CanonicalInput,
			FUeAgentResourceTracker& Resources,
			FUeAgentRigSimulationSpatialTypedRequestPtr& OutRequest,
			FUeAgentRigSimulationSpatialExecutionError& OutError) override
		{
			++Calls;
			if (Trace)
			{
				Trace->Add(TEXT("decode"));
			}
			LastCapability = Contract.GetCapabilityId();
			LastCanonicalInput = CanonicalInput.GetJsonValue();
			if (bFail)
			{
				OutError.Code = TEXT("fixture_rig_simulation_spatial_decode_failed");
				return false;
			}
			OutRequest = MakeShared<FFixtureTypedRequest>(Contract);
			LastRequest = OutRequest;
			return true;
		}

		bool bFail = false;
		int32 Calls = 0;
		FString LastCapability;
		FString LastCanonicalInput;
		FUeAgentRigSimulationSpatialTypedRequestPtr LastRequest;
		TArray<FString>* Trace = nullptr;
	};

	class FRecordingReadbackPort final
		: public IUeAgentRigSimulationSpatialReadbackPort
	{
	public:
		FRecordingReadbackPort(
			FAutomationTestBase* InTest,
			TArray<FString>* InTrace)
			: Test(InTest)
			, Trace(InTrace)
		{
		}

		bool TryRead(
			const FUeAgentRigSimulationSpatialExecutionChainContract& Contract,
			const FUeAgentRigSimulationSpatialTypedRequestRef& Request,
			const TOptional<FUeAgentCandidateBinding>& Target,
			EUeAgentRigSimulationSpatialReadPhase Phase,
			const FUeAgentOperationContext& Context,
			TOptional<FUeAgentRigSimulationSpatialReadbackResult>& OutReadback,
			FUeAgentRigSimulationSpatialExecutionError& OutError) override
		{
			OutReadback.Reset();
			++Calls;
			PhaseCalls.FindOrAdd(Phase) += 1;
			if (Trace)
			{
				Trace->Add(
					Phase == EUeAgentRigSimulationSpatialReadPhase::Query
						? TEXT("query_read")
						: Phase == EUeAgentRigSimulationSpatialReadPhase::Preflight
							? TEXT("preflight_read")
							: TEXT("verification_read"));
			}
			LastContext = &Context;
			LastResources = &Context.GetResourceTracker();
			LastRequest = &Request.Get();
			if (FailPhase.IsSet() && FailPhase.GetValue() == Phase)
			{
				OutError.Code = TEXT("fixture_rig_simulation_spatial_readback_failed");
				OutError.SubjectId = Contract.GetReadbackId();
				return false;
			}
			const auto Snapshot = MakeSnapshot(
				Test,
				Contract.GetCapabilityId(),
				Context.GetResourceTracker(),
				Calls);
			if (!Snapshot.IsValid()) return false;
			OutReadback.Emplace(Snapshot.ToSharedRef());
			return true;
		}

		void ResetEvents()
		{
			PhaseCalls.Reset();
			FailPhase.Reset();
		}

		FAutomationTestBase* Test = nullptr;
		int32 Calls = 0;
		TMap<EUeAgentRigSimulationSpatialReadPhase, int32> PhaseCalls;
		TOptional<EUeAgentRigSimulationSpatialReadPhase> FailPhase;
		const FUeAgentOperationContext* LastContext = nullptr;
		FUeAgentResourceTracker* LastResources = nullptr;
		const IUeAgentRigSimulationSpatialTypedRequest* LastRequest = nullptr;
		TArray<FString>* Trace = nullptr;
	};

	class FRecordingMutationPort final
		: public IUeAgentRigSimulationSpatialMutationPort
	{
	public:
		explicit FRecordingMutationPort(TArray<FString>* InTrace)
			: Trace(InTrace)
		{
		}

		bool TryMutate(
			const FUeAgentRigSimulationSpatialExecutionChainContract& Contract,
			const FUeAgentRigSimulationSpatialTypedRequestRef& Request,
			const TArray<FUeAgentOperationTargetBinding>& Targets,
			const FUeAgentOperationContext& Context,
			FUeAgentRigSimulationSpatialMutationResult& OutResult,
			FUeAgentRigSimulationSpatialExecutionError& OutError) override
		{
			++Calls;
			if (Trace)
			{
				Trace->Add(TEXT("mutate"));
			}
			LastContext = &Context;
			LastResources = &Context.GetResourceTracker();
			LastRequest = &Request.Get();
			if (bFail)
			{
				OutError.Code = TEXT("fixture_rig_simulation_spatial_mutation_failed");
				return false;
			}
			OutResult = FUeAgentRigSimulationSpatialMutationResult(
				TEXT("rig-simulation-spatial-operation:") + Contract.GetCapabilityId(),
				TEXT("fixture-mutation-fact"));
			return true;
		}

		bool bFail = false;
		int32 Calls = 0;
		const FUeAgentOperationContext* LastContext = nullptr;
		FUeAgentResourceTracker* LastResources = nullptr;
		const IUeAgentRigSimulationSpatialTypedRequest* LastRequest = nullptr;
		TArray<FString>* Trace = nullptr;
	};

	class FRecordingSettlePort final
		: public IUeAgentRigSimulationSpatialSettlePort
	{
	public:
		explicit FRecordingSettlePort(TArray<FString>* InTrace)
			: Trace(InTrace)
		{
		}

		bool TrySettle(
			const FUeAgentRigSimulationSpatialExecutionChainContract& Contract,
			const FUeAgentRigSimulationSpatialMutationResult& Mutation,
			const FUeAgentOperationContext& Context,
			FUeAgentRigSimulationSpatialExecutionError& OutError) override
		{
			++Calls;
			if (Trace)
			{
				Trace->Add(TEXT("settle"));
			}
			LastContext = &Context;
			LastResources = &Context.GetResourceTracker();
			return !Mutation.GetOperationId().IsEmpty();
		}

		int32 Calls = 0;
		const FUeAgentOperationContext* LastContext = nullptr;
		FUeAgentResourceTracker* LastResources = nullptr;
		TArray<FString>* Trace = nullptr;
	};

	class FRecordingComparator final
		: public IUeAgentRigSimulationSpatialResultComparator
	{
	public:
		explicit FRecordingComparator(TArray<FString>* InTrace)
			: Trace(InTrace)
		{
		}

		bool TryCompare(
			const FUeAgentRigSimulationSpatialExecutionChainContract& Contract,
			const FUeAgentRigSimulationSpatialTypedRequestRef& Request,
			const FUeAgentCanonicalSnapshot& Before,
			const FUeAgentRigSimulationSpatialReadbackResult& After,
			const FUeAgentOperationContext& Context,
			FVerificationOutcome& OutOutcome,
			FUeAgentRigSimulationSpatialExecutionError& OutError) override
		{
			++Calls;
			if (Trace)
			{
				Trace->Add(TEXT("compare"));
			}
			LastContext = &Context;
			LastResources = &Context.GetResourceTracker();
			LastRequest = &Request.Get();
			if (bFail)
			{
				OutError.Code = TEXT("fixture_rig_simulation_spatial_compare_failed");
				return false;
			}
			OutOutcome = FVerificationOutcome(
				TEXT("verify:") + Contract.GetCapabilityId(),
				EVerificationStatus::Verified,
				EEffectOutcome::Complete,
				FString(),
				false,
				{},
				{TEXT("fixture-independent-readback")},
				{});
			return true;
		}

		bool bFail = false;
		int32 Calls = 0;
		const FUeAgentOperationContext* LastContext = nullptr;
		FUeAgentResourceTracker* LastResources = nullptr;
		const IUeAgentRigSimulationSpatialTypedRequest* LastRequest = nullptr;
		TArray<FString>* Trace = nullptr;
	};

	const FUeAgentOperationDescriptor* FindCapability(
		const FUeAgentDescriptorRegistry& Registry,
		const FString& CapabilityId)
	{
		for (const FUeAgentOperationDescriptor& Descriptor
			: Registry.GetDescriptors())
		{
			if (Descriptor.GetCapabilityId() == CapabilityId)
			{
				return &Descriptor;
			}
		}
		return nullptr;
	}
}


IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUeAgentRigSimulationSpatialC3QueryProviderBehaviorTest,
	"UeAgentInterface.DomainAdapters.RigSimulationSpatial.E1.C3QProviderBehavior",
	EAutomationTestFlags::EditorContext
		| EAutomationTestFlags::EngineFilter)

bool FUeAgentRigSimulationSpatialC3QueryProviderBehaviorTest::RunTest(
	const FString& Parameters)
{
	TSharedPtr<const FUeAgentDescriptorRegistry> DescriptorRegistry;
	FString RegistryError;
	if (!TestTrue(
			TEXT("create exact RigSimulationSpatial descriptors"),
			TryCreateRigSimulationSpatialDescriptorRegistry(
				DescriptorRegistry,
				RegistryError))
		|| !DescriptorRegistry.IsValid())
	{
		return false;
	}

	TArray<FString> Trace;
	const TSharedRef<FRecordingDecoder> Decoder =
		MakeShared<FRecordingDecoder>(&Trace);
	const TSharedRef<FRecordingReadbackPort> Readback =
		MakeShared<FRecordingReadbackPort>(this, &Trace);
	TSharedPtr<const FUeAgentDomainQueryProviderRegistry> Providers;
	FUeAgentRigSimulationSpatialExecutionError FactoryError;
	if (!TestTrue(
			TEXT("register five exact C3Q providers"),
			TryCreateRigSimulationSpatialQueryProviderRegistry(
				Decoder,
				Readback,
				Providers,
				FactoryError))
		|| !Providers.IsValid())
	{
		return false;
	}

	TArray<FUeAgentQueryDescriptor> QueryDescriptors;
	for (const FUeAgentOperationDescriptor& Descriptor
		: DescriptorRegistry->GetDescriptors())
	{
		const TOptional<FUeAgentQueryDescriptor> Query =
			DescriptorRegistry->FindQueryExact(Descriptor.GetIdentity());
		if (Query.IsSet())
		{
			QueryDescriptors.Add(Query.GetValue());
		}
	}

	const FUeAgentResourceTrackerOwner Resources = MakeResources();
	for (int32 Index = 0; Index < QueryDescriptors.Num(); ++Index)
	{
		const FUeAgentQueryDescriptor& Descriptor = QueryDescriptors[Index];
		const FString CapabilityId = Descriptor.GetCapabilityId();
		const TSharedPtr<IUeAgentDomainQueryProvider> Provider =
			Providers->FindExact(Descriptor.GetProviderBinding());
		if (!TestTrue(
				TEXT("exact provider registered: ") + CapabilityId,
				Provider.IsValid()))
		{
			continue;
		}
		const TSharedPtr<FUeAgentOperationContext> Context = MakeContext(
			this,
			Descriptor.GetOperationDescriptor(),
			EUeAgentRequestKind::Query,
			TEXT("query-") + FString::FromInt(Index),
			Resources);
		if (!Context.IsValid())
		{
			continue;
		}

		const int32 DecodeBefore = Decoder->Calls;
		const int32 ReadBefore = Readback->Calls;
		Trace.Reset();
		TSharedPtr<const FUeAgentProjectionSourceValue> Source;
		FUeAgentQueryBindingError Error;
		TestTrue(
			TEXT("query decodes once and reads once: ") + CapabilityId,
			Provider->TryQuery(
				Descriptor,
				FUeAgentContractValue(TEXT("{}")),
				{},
				*Context,
				Source,
				Error));
		TestEqual(TEXT("one typed decode"), Decoder->Calls - DecodeBefore, 1);
		TestEqual(TEXT("one independent readback"), Readback->Calls - ReadBefore, 1);
		TestEqual(
			TEXT("query order is decode then independent readback"),
			FString::Join(Trace, TEXT("|")),
			FString(TEXT("decode|query_read")));
		TestTrue(
			TEXT("same OperationContext reaches query readback"),
			Readback->LastContext == Context.Get());
		TestTrue(
			TEXT("same resource owner reaches query readback"),
			Readback->LastResources == &Context->GetResourceTracker());
		TestTrue(
			TEXT("decoded typed request reaches query readback"),
			Decoder->LastRequest.IsValid()
				&& Readback->LastRequest == Decoder->LastRequest.Get());
		TestEqual(
			TEXT("query request carries the formal typed-port family"),
			Decoder->LastRequest->GetFormalPortRequest().GetFamily(),
			PortFamilyFor(Decoder->LastRequest->GetFamilyId()));
		TestTrue(
			TEXT("query result is independent C4 readback"),
			Source.IsValid()
				&& Source->GetSnapshot().IsValid()
				&& !Source->GetCatalog().IsValid()
				&& !Source->GetValue().IsValid()
				&& !Source->GetComparison().IsValid());

		const FUeAgentQueryDescriptor& Other =
			QueryDescriptors[(Index + 1) % QueryDescriptors.Num()];
		const int32 DecodeBeforeMismatch = Decoder->Calls;
		const int32 ReadBeforeMismatch = Readback->Calls;
		Source.Reset();
		TestFalse(
			TEXT("provider binding mismatch fails closed: ") + CapabilityId,
			Provider->TryQuery(
				Other,
				FUeAgentContractValue(TEXT("{}")),
				{},
				*Context,
				Source,
				Error));
		TestEqual(
			TEXT("mismatch never decodes"),
			Decoder->Calls,
			DecodeBeforeMismatch);
		TestEqual(
			TEXT("mismatch never reads"),
			Readback->Calls,
			ReadBeforeMismatch);

		Readback->FailPhase = EUeAgentRigSimulationSpatialReadPhase::Query;
		const int32 ReadBeforeFailure = Readback->Calls;
		Trace.Reset();
		Source.Reset();
		TestFalse(
			TEXT("readback failure propagates: ") + CapabilityId,
			Provider->TryQuery(
				Descriptor,
				FUeAgentContractValue(TEXT("{}")),
				{},
				*Context,
				Source,
				Error));
		TestEqual(
			TEXT("failed readback is not retried"),
			Readback->Calls - ReadBeforeFailure,
			1);
		TestEqual(
			TEXT("stable readback failure code"),
			Error.Code,
			FString(TEXT("fixture_rig_simulation_spatial_readback_failed")));
		TestFalse(TEXT("failure cannot leak a source"), Source.IsValid());
		TestEqual(
			TEXT("failure path remains decode then one read"),
			FString::Join(Trace, TEXT("|")),
			FString(TEXT("decode|query_read")));
		Readback->FailPhase.Reset();
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUeAgentRigSimulationSpatialC3OperationBehaviorTest,
	"UeAgentInterface.DomainAdapters.RigSimulationSpatial.E1.C3OOperationBehavior",
	EAutomationTestFlags::EditorContext
		| EAutomationTestFlags::EngineFilter)

bool FUeAgentRigSimulationSpatialC3OperationBehaviorTest::RunTest(
	const FString& Parameters)
{
	TSharedPtr<const FUeAgentDescriptorRegistry> DescriptorRegistry;
	FString RegistryError;
	if (!TestTrue(
			TEXT("create exact RigSimulationSpatial descriptors"),
			TryCreateRigSimulationSpatialDescriptorRegistry(
				DescriptorRegistry,
				RegistryError))
		|| !DescriptorRegistry.IsValid())
	{
		return false;
	}

	TArray<FString> Trace;
	const TSharedRef<FRecordingDecoder> Decoder =
		MakeShared<FRecordingDecoder>(&Trace);
	const TSharedRef<FRecordingReadbackPort> Readback =
		MakeShared<FRecordingReadbackPort>(this, &Trace);
	const TSharedRef<FRecordingMutationPort> Mutation =
		MakeShared<FRecordingMutationPort>(&Trace);
	const TSharedRef<FRecordingSettlePort> Settle =
		MakeShared<FRecordingSettlePort>(&Trace);
	const TSharedRef<FRecordingComparator> Comparator =
		MakeShared<FRecordingComparator>(&Trace);
	TArray<TSharedRef<FUeAgentRigSimulationSpatialOperationChain>> Chains;
	FUeAgentRigSimulationSpatialExecutionError FactoryError;
	if (!TestTrue(
			TEXT("create thirteen exact C3O chains"),
			TryCreateRigSimulationSpatialOperationChains(
				Decoder,
				Readback,
				Mutation,
				Settle,
				Comparator,
				Chains,
				FactoryError)))
	{
		return false;
	}

	const FUeAgentResourceTrackerOwner Resources = MakeResources();
	for (int32 Index = 0; Index < Chains.Num(); ++Index)
	{
		const TSharedRef<FUeAgentRigSimulationSpatialOperationChain>& Chain =
			Chains[Index];
		const FUeAgentRigSimulationSpatialExecutionChainContract& ChainContract =
			Chain->GetContract();
		const FUeAgentOperationDescriptor* Descriptor = FindCapability(
			*DescriptorRegistry,
			ChainContract.GetCapabilityId());
		if (!TestNotNull(
				TEXT("operation descriptor exists"),
				Descriptor))
		{
			continue;
		}
		const TSharedPtr<FUeAgentOperationContext> Context = MakeContext(
			this,
			*Descriptor,
			EUeAgentRequestKind::Operation,
			TEXT("operation-") + FString::FromInt(Index),
			Resources);
		if (!Context.IsValid())
		{
			continue;
		}

		Trace.Reset();
		Readback->ResetEvents();
		const int32 MutationBefore = Mutation->Calls;
		const int32 SettleBefore = Settle->Calls;
		const int32 CompareBefore = Comparator->Calls;
		TOptional<FPlanningRequest> PlanningRequest;
		FUeAgentOperationBindingError Error;
		TestTrue(
			TEXT("typed request and preflight are built before mutation"),
			Chain->TryBuildPlanningRequest(
				FUeAgentContractValue(TEXT("{}")),
				{},
				*Context,
				PlanningRequest,
				Error));
		if (!PlanningRequest.IsSet())
		{
			continue;
		}
		TestEqual(
			TEXT("preflight does not mutate"),
			Mutation->Calls,
			MutationBefore);
		TestEqual(
			TEXT("preflight performs one independent read"),
			Readback->PhaseCalls.FindRef(
				EUeAgentRigSimulationSpatialReadPhase::Preflight),
			1);

		FPlanningResult Planning;
		TestTrue(
			TEXT("C3O planner accepts the typed draft"),
			Chain->GetPlanner().TryPlan(
				PlanningRequest.GetValue(),
				*Context,
				Planning,
				Error));
		FExecutionReceipt Receipt;
		TestTrue(
			TEXT("C3O executor performs one native mutation"),
			Chain->GetExecutor().TryExecute(
				Planning,
				{},
				*Context,
				Receipt,
				Error));
		TestEqual(
			TEXT("native mutation occurs exactly once"),
			Mutation->Calls - MutationBefore,
			1);

		FVerificationOutcome Outcome;
		TestTrue(
			TEXT("C3O verifier settles then independently reads"),
			Chain->GetVerifier().TryVerify(
				Receipt,
				{},
				*Context,
				Outcome,
				Error));
		TestEqual(TEXT("settle exactly once"), Settle->Calls - SettleBefore, 1);
		TestEqual(
			TEXT("post-mutation readback exactly once"),
			Readback->PhaseCalls.FindRef(
				EUeAgentRigSimulationSpatialReadPhase::Verification),
			1);
		TestEqual(
			TEXT("independent comparator exactly once"),
			Comparator->Calls - CompareBefore,
			1);
		TestEqual(
			TEXT("C3O physical order is preflight then mutation settle readback compare"),
			FString::Join(Trace, TEXT("|")),
			FString(TEXT(
				"decode|preflight_read|mutate|settle|verification_read|compare")));
		TestEqual(
			TEXT("verification is complete"),
			Outcome.GetEffectOutcome(),
			EEffectOutcome::Complete);
		TestTrue(
			TEXT("same typed request reaches mutation"),
			Decoder->LastRequest.IsValid()
				&& Mutation->LastRequest == Decoder->LastRequest.Get());
		TestEqual(
			TEXT("operation request carries the formal typed-port family"),
			Decoder->LastRequest->GetFormalPortRequest().GetFamily(),
			PortFamilyFor(Decoder->LastRequest->GetFamilyId()));
		TestTrue(
			TEXT("same typed request reaches independent compare"),
			Comparator->LastRequest == Decoder->LastRequest.Get());
		TestTrue(
			TEXT("same OperationContext reaches mutation/settle/readback/compare"),
			Mutation->LastContext == Context.Get()
				&& Settle->LastContext == Context.Get()
				&& Readback->LastContext == Context.Get()
				&& Comparator->LastContext == Context.Get());
		TestTrue(
			TEXT("same resource owner reaches every physical port"),
			Mutation->LastResources == &Context->GetResourceTracker()
				&& Settle->LastResources == &Context->GetResourceTracker()
				&& Readback->LastResources == &Context->GetResourceTracker()
				&& Comparator->LastResources == &Context->GetResourceTracker());
		TestEqual(
			TEXT("comparator receives the same typed request, not a receipt-derived payload"),
			Comparator->LastRequest,
			Mutation->LastRequest);
	}

	if (!Chains.IsEmpty())
	{
		const TSharedRef<FUeAgentRigSimulationSpatialOperationChain>& Chain = Chains[0];
		const FUeAgentOperationDescriptor* Descriptor = FindCapability(
			*DescriptorRegistry,
			Chain->GetContract().GetCapabilityId());
		const TSharedPtr<FUeAgentOperationContext> Context = Descriptor
			? MakeContext(
				this,
				*Descriptor,
				EUeAgentRequestKind::Operation,
				TEXT("operation-readback-failure"),
				Resources)
			: nullptr;
		if (Context.IsValid())
		{
			if (Chains.Num() > 1)
			{
				const FUeAgentOperationDescriptor* OtherDescriptor = FindCapability(
					*DescriptorRegistry,
					Chains[1]->GetContract().GetCapabilityId());
				const TSharedPtr<FUeAgentOperationContext> OtherContext =
					OtherDescriptor
						? MakeContext(
							this,
							*OtherDescriptor,
							EUeAgentRequestKind::Operation,
							TEXT("operation-context-mismatch"),
							Resources)
						: nullptr;
				if (OtherContext.IsValid())
				{
					const int32 DecodeBefore = Decoder->Calls;
					const int32 ReadBefore = Readback->Calls;
					const int32 MutationBefore = Mutation->Calls;
					TOptional<FPlanningRequest> InvalidPlanningRequest;
					FUeAgentOperationBindingError InvalidError;
					TestFalse(
						TEXT("descriptor/context mismatch fails before preflight"),
						Chain->TryBuildPlanningRequest(
							FUeAgentContractValue(TEXT("{}")),
							{},
							*OtherContext,
							InvalidPlanningRequest,
							InvalidError));
					TestEqual(TEXT("mismatch does not decode"), Decoder->Calls, DecodeBefore);
					TestEqual(TEXT("mismatch does not read"), Readback->Calls, ReadBefore);
					TestEqual(TEXT("mismatch does not mutate"), Mutation->Calls, MutationBefore);
					TestEqual(
						TEXT("mismatch has stable failure code"),
						InvalidError.Code,
						FString(TEXT("rig_simulation_spatial_context_descriptor_mismatch")));
				}
			}

			Readback->ResetEvents();
			Trace.Reset();
			Readback->FailPhase = EUeAgentRigSimulationSpatialReadPhase::Preflight;
			const int32 MutationBeforePreflightFailure = Mutation->Calls;
			TOptional<FPlanningRequest> MissingRevisionPlanningRequest;
			FUeAgentOperationBindingError MissingRevisionError;
			TestFalse(
				TEXT("missing preflight observation fails before native mutation"),
				Chain->TryBuildPlanningRequest(
					FUeAgentContractValue(TEXT("{}")),
					{},
					*Context,
					MissingRevisionPlanningRequest,
					MissingRevisionError));
			TestEqual(
				TEXT("preflight failure never mutates"),
				Mutation->Calls,
				MutationBeforePreflightFailure);
			TestEqual(
				TEXT("preflight read failure is not retried"),
				Readback->PhaseCalls.FindRef(
					EUeAgentRigSimulationSpatialReadPhase::Preflight),
				1);
			TestEqual(
				TEXT("preflight failure is stable"),
				MissingRevisionError.Code,
				FString(TEXT("fixture_rig_simulation_spatial_readback_failed")));

			Readback->ResetEvents();
			Trace.Reset();
			Readback->FailPhase =
				EUeAgentRigSimulationSpatialReadPhase::Verification;
			const int32 MutationBefore = Mutation->Calls;
			const int32 CompareBefore = Comparator->Calls;
			TOptional<FPlanningRequest> PlanningRequest;
			FUeAgentOperationBindingError Error;
			if (Chain->TryBuildPlanningRequest(
					FUeAgentContractValue(TEXT("{}")),
					{},
					*Context,
					PlanningRequest,
					Error))
			{
				FPlanningResult Planning;
				FExecutionReceipt Receipt;
				TestTrue(
					TEXT("failure scenario plans"),
					Chain->GetPlanner().TryPlan(
						PlanningRequest.GetValue(),
						*Context,
						Planning,
						Error));
				TestTrue(
					TEXT("failure scenario mutates once"),
					Chain->GetExecutor().TryExecute(
						Planning,
						{},
						*Context,
						Receipt,
						Error));
				FVerificationOutcome Outcome;
				TestFalse(
					TEXT("verification readback failure is not hidden"),
					Chain->GetVerifier().TryVerify(
						Receipt,
						{},
						*Context,
						Outcome,
						Error));
				TestEqual(
					TEXT("failed readback does not repeat mutation"),
					Mutation->Calls - MutationBefore,
					1);
				TestEqual(
					TEXT("failed verification readback is not retried"),
					Readback->PhaseCalls.FindRef(
						EUeAgentRigSimulationSpatialReadPhase::Verification),
					1);
				TestEqual(
					TEXT("stable verification readback failure"),
					Error.Code,
					FString(TEXT("fixture_rig_simulation_spatial_readback_failed")));
				TestEqual(
					TEXT("failed readback never reaches comparator"),
					Comparator->Calls,
					CompareBefore);
				TestEqual(
					TEXT("failure order stops after one verification read"),
					FString::Join(Trace, TEXT("|")),
					FString(TEXT(
						"decode|preflight_read|mutate|settle|verification_read")));
			}
		}
	}
	return true;
}
