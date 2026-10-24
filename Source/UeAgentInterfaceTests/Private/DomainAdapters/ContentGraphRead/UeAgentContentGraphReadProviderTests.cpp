#include "Misc/AutomationTest.h"

// RE-007 production must satisfy exactly the seam frozen by RE-006.
#include "DomainAdapters/ContentGraphRead/UeAgentContentGraphReadProvider.h"

#include "Core/ContextResource/UeAgentOperationContext.h"
#include "Core/Contract/UeAgentDescriptorRegistry.h"
#include "Core/Discovery/UeAgentDiscovery.h"
#include "Core/Projection/UeAgentProjection.h"
#include "Core/StateSemantic/UeAgentStateSemantic.h"
#include "DomainAdapters/ContentGraphRead/UeAgentContentGraphReadDescriptors.h"

namespace
{
	using namespace UE::UAI::BindingGateway::Query;
	using namespace UE::UAI::ContextResource;
	using namespace UE::UAI::Contract;
	using namespace UE::UAI::Discovery;
	using namespace UE::UAI::DomainAdapters::ContentGraphRead;
	using namespace UE::UAI::Projection;
	using namespace UE::UAI::StateSemantic;

	const TCHAR* CompoundDescriptorId()
	{
		return TEXT("uai.query.content_graph.compound");
	}

	FUeAgentResourceTrackerOwner Resources()
	{
		return MakeShared<FUeAgentResourceTracker, ESPMode::ThreadSafe>();
	}

	TSharedPtr<FUeAgentOperationContext> OperationContext(
		FAutomationTestBase* Test,
		const FUeAgentQueryDescriptor& Descriptor,
		const FUeAgentResourceTrackerOwner& ResourceTracker)
	{
		FUeAgentExecutionContextInput Input;
		Input.ProjectInstanceId = TEXT("content-graph-fixture");
		Input.EditorInstanceId = TEXT("content-graph-editor");
		Input.StatePlane = EUeAgentStatePlane::AuthoringMemory;
		Input.WorldType = EUeAgentExecutionWorldType::Editor;
		Input.ContextRevision = TEXT("content-graph-revision-1");

		FUeAgentExecutionContextRefPtr ExecutionContext;
		FUeAgentExecutionContextError ContextError;
		Test->TestTrue(
			TEXT("freeze ContentGraphRead execution context"),
			FUeAgentExecutionContextRef::TryCreate(
				Input,
				Descriptor.GetIdentity(),
				EUeAgentRequestKind::Query,
				Descriptor.GetExecutionContextPolicy(),
				ExecutionContext,
				ContextError));
		if (!ExecutionContext.IsValid())
		{
			return nullptr;
		}

		const FUeAgentQueryRequest Request(
			Descriptor.GetIdentity(),
			TEXT("request-content-graph-read-provider"),
			FUeAgentContractValue(
				TEXT("{\"sections\":[\"items\",\"relations\"]}")));
		const FUeAgentCallerContext Caller(
			TEXT("fixture-agent"),
			{TEXT("content_graph:read")},
			TEXT("corr-content-graph-read-provider"));
		const FUeAgentOperationDeadline Deadline =
			FUeAgentOperationDeadline::FromTimeout(
				30.0,
				[]() { return 100.0; });
		const FUeAgentCancellationSource Cancellation(
			[]() { return 100.0; });
		TSharedPtr<FUeAgentOperationContext> Context;
		FUeAgentOperationContextError OperationError;
		Test->TestTrue(
			TEXT("create ContentGraphRead operation context"),
			FUeAgentOperationContext::TryCreate(
				Request,
				Caller,
				ExecutionContext.ToSharedRef(),
				ResourceTracker,
				Deadline,
				Cancellation.GetToken(),
				Context,
				OperationError));
		return Context;
	}

	FUeAgentCandidateBinding Target(
		const FUeAgentDescriptorIdentity& DescriptorIdentity)
	{
		return FUeAgentCandidateBinding(
			TEXT("content-graph-read-target"),
			FUeAgentCandidateSummary(
				FUeAgentCanonicalIdentity(
					TEXT("asset:/Game/UAI/Fixture"),
					TEXT("asset"),
					TEXT("/Game/UAI/Fixture"),
					TEXT("Fixture"),
					TEXT("target"),
					TEXT("revision-1")),
				DescriptorIdentity,
				TEXT("blueprint"),
				TEXT("BP_Fixture"),
				TEXT("owned ContentGraphRead Blueprint fixture"),
				{TEXT("ContentGraphRead"), TEXT("Blueprint")}),
			FUeAgentCatalogRevision(
				TEXT("content-graph-read-fixture"),
				TEXT("revision-1"),
				1),
			TEXT("uai://asset/Game/UAI/Fixture"),
			TEXT("content-graph-read"),
			{EUeAgentCandidateOperation::Read},
			TEXT("content-graph-read-provider-test"));
	}

	TSharedPtr<const FUeAgentCanonicalSnapshot> Snapshot(
		FAutomationTestBase* Test,
		FUeAgentResourceTracker& ResourceTracker)
	{
		const FUeAgentStateRevision Revision(
			TEXT("content-graph-read"),
			TEXT("revision-1"),
			1);
		FUeAgentCanonicalValuePtr Value;
		FUeAgentStateSemanticError Error;
		Test->TestTrue(
			TEXT("create ContentGraphRead C4 value"),
			FUeAgentCanonicalValue::TryString(
				TEXT("/Game/UAI/Fixture"),
				ResourceTracker,
				Value,
				Error));
		if (!Value.IsValid())
		{
			return nullptr;
		}
		const FUeAgentStateEntry Entry(
			FUeAgentCanonicalIdentity(
				TEXT("content_graph.item:owner"),
				TEXT("blueprint"),
				TEXT("/Game/UAI/Fixture"),
				TEXT("path"),
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
				TEXT("content-graph-native"),
				TEXT("independent-readback"),
				Value.ToSharedRef()),
			FUeAgentStateDetails(
				TEXT("Graph Owner"),
				TEXT("owned fixture graph owner"),
				{TEXT("ContentGraphRead"), TEXT("Blueprint")},
				false,
				TEXT("available"),
				{}),
			Revision);
		TSharedPtr<const FUeAgentCanonicalSnapshot> Result;
		Test->TestTrue(
			TEXT("create ContentGraphRead C4 snapshot"),
			FUeAgentCanonicalSnapshot::TryCreate(
				TEXT("content-graph-read-compound"),
				EUeAgentStatePlane::AuthoringMemory,
				Revision,
				FUeAgentTemporalPoint(TEXT("fixture-clock"), TEXT("1")),
				FUeAgentStateProvenance(
					TEXT("independent_readback"),
					TEXT("content-graph-read"),
					TEXT("fixture-content-graph-read")),
				{Entry},
				ResourceTracker,
				Result,
				Error));
		return Result;
	}

	class FRecordingHook final : public IUeAgentContentGraphReadHook
	{
	public:
		explicit FRecordingHook(
			TSharedRef<const FUeAgentCanonicalSnapshot> InSnapshot)
			: StoredSnapshot(MoveTemp(InSnapshot))
		{
		}

		bool TryCapture(
			const FUeAgentContractValue& CanonicalInput,
			const TOptional<FUeAgentCandidateBinding>& InTarget,
			const FUeAgentOperationContext& Context,
			TSharedPtr<const FUeAgentCanonicalSnapshot>& OutSnapshot,
			FUeAgentContentGraphReadError& OutError) override
		{
			++Calls;
			LastInput = CanonicalInput.GetJsonValue();
			LastTarget = InTarget;
			LastContext = &Context;
			if (bFail)
			{
				OutError.Code =
					TEXT("fixture_content_graph_read_failed");
				OutError.SubjectId = TEXT("fixture-graph-owner");
				OutError.Detail = TEXT("fixture hook failure");
				return false;
			}
			OutSnapshot = StoredSnapshot;
			return true;
		}

		bool bFail = false;
		int32 Calls = 0;
		FString LastInput;
		TOptional<FUeAgentCandidateBinding> LastTarget;
		const FUeAgentOperationContext* LastContext = nullptr;

	private:
		TSharedRef<const FUeAgentCanonicalSnapshot> StoredSnapshot;
	};
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUeAgentContentGraphReadProviderBoundaryTest,
	"UeAgentInterface.DomainAdapters.ContentGraphRead.E1.Provider."
		"ExactHookAndContext",
	EAutomationTestFlags::EditorContext
		| EAutomationTestFlags::EngineFilter)

bool FUeAgentContentGraphReadProviderBoundaryTest::RunTest(
	const FString& Parameters)
{
	TSharedPtr<const FUeAgentDescriptorRegistry> Registry;
	FString RegistryError;
	TestTrue(
		TEXT("create production ContentGraphRead descriptors"),
		TryCreateContentGraphReadDescriptorRegistry(
			Registry,
			RegistryError));
	if (!Registry.IsValid())
	{
		AddError(TEXT("content_graph_read_registry_failed:")
			+ RegistryError);
		return false;
	}
	const FUeAgentOperationDescriptor* RegisteredDescriptor =
		Registry->FindByDescriptorId(CompoundDescriptorId());
	if (!TestNotNull(
			TEXT("registered compound descriptor"),
			RegisteredDescriptor))
	{
		return false;
	}
	const TOptional<FUeAgentQueryDescriptor> Descriptor =
		Registry->FindQueryExact(RegisteredDescriptor->GetIdentity());
	if (!TestTrue(TEXT("typed query descriptor"), Descriptor.IsSet()))
	{
		return false;
	}
	const FUeAgentResourceTrackerOwner ResourceTracker =
		Resources();
	const TSharedPtr<FUeAgentOperationContext> Context =
		OperationContext(
			this,
			Descriptor.GetValue(),
			ResourceTracker);
	const TSharedPtr<const FUeAgentCanonicalSnapshot> ExpectedSnapshot =
		Snapshot(this, ResourceTracker.Get());
	if (!Registry.IsValid()
		|| !Context.IsValid()
		|| !ExpectedSnapshot.IsValid())
	{
		return false;
	}
	const TSharedRef<FRecordingHook> Hook =
		MakeShared<FRecordingHook>(ExpectedSnapshot.ToSharedRef());
	FUeAgentContentGraphReadProvider Provider(
		TEXT("content_graph.read.compound"),
		Hook);
	TSharedPtr<const FUeAgentProjectionSourceValue> Source;
	FUeAgentQueryBindingError Error;
	const FUeAgentCandidateBinding ResolvedTarget =
		Target(Descriptor->GetIdentity());
	TestTrue(
		TEXT("provider forwards to one exact hook"),
		Provider.TryQuery(
			Descriptor.GetValue(),
			FUeAgentContractValue(
				TEXT("{\"sections\":[\"items\",\"relations\"]}")),
			ResolvedTarget,
			*Context,
			Source,
			Error));
	TestEqual(TEXT("hook called once"), Hook->Calls, 1);
	TestEqual(
		TEXT("canonical input unchanged"),
		Hook->LastInput,
		FString(TEXT("{\"sections\":[\"items\",\"relations\"]}")));
	TestTrue(
		TEXT("Ticket target unchanged"),
		Hook->LastTarget.IsSet()
			&& Hook->LastTarget->GetBindingId()
				== ResolvedTarget.GetBindingId());
	TestTrue(TEXT("same context instance"), Hook->LastContext == Context.Get());
	TestTrue(
		TEXT("success exposes only the hook C4 snapshot"),
		Source.IsValid()
			&& Source->GetSnapshot().Get() == ExpectedSnapshot.Get()
			&& !Source->GetCatalog().IsValid()
			&& !Source->GetValue().IsValid()
			&& !Source->GetComparison().IsValid());

	Hook->bFail = true;
	Source.Reset();
	TestFalse(
		TEXT("failure has no fallback"),
		Provider.TryQuery(
			Descriptor.GetValue(),
			FUeAgentContractValue(
				TEXT("{\"sections\":[\"items\",\"relations\"]}")),
			{},
			*Context,
			Source,
			Error));
	TestEqual(TEXT("failed hook is not retried"), Hook->Calls, 2);
	TestFalse(TEXT("failure leaks no projection source"), Source.IsValid());
	TestEqual(
		TEXT("typed hook error preserved"),
		Error.Code,
		FString(TEXT("fixture_content_graph_read_failed")));

	const TSharedRef<FRecordingHook> MismatchHook =
		MakeShared<FRecordingHook>(ExpectedSnapshot.ToSharedRef());
	FUeAgentContentGraphReadProvider Mismatch(
		TEXT("content_graph.read.properties"),
		MismatchHook);
	TestFalse(
		TEXT("binding mismatch rejected"),
		Mismatch.TryQuery(
			Descriptor.GetValue(),
			FUeAgentContractValue(
				TEXT("{\"sections\":[\"items\",\"relations\"]}")),
			{},
			*Context,
			Source,
			Error));
	TestEqual(TEXT("mismatch never calls hook"), MismatchHook->Calls, 0);
	TestEqual(
		TEXT("stable mismatch error"),
		Error.Code,
		FString(TEXT(
			"content_graph_read_provider_binding_mismatch")));
	return true;
}

