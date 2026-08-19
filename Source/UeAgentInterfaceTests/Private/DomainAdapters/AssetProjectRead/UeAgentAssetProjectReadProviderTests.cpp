#include "Misc/AutomationTest.h"

// RE-005 production must satisfy exactly this frozen seam.
#include "DomainAdapters/AssetProjectRead/UeAgentAssetProjectReadProvider.h"


#include "Core/ContextResource/UeAgentOperationContext.h"
#include "Core/Contract/UeAgentDescriptorRegistry.h"
#include "Core/Discovery/UeAgentDiscovery.h"
#include "Core/Projection/UeAgentProjection.h"
#include "Core/StateSemantic/UeAgentStateSemantic.h"
#include "DomainAdapters/AssetProjectRead/UeAgentAssetProjectReadDescriptors.h"

namespace
{
	using namespace UE::UAI::BindingGateway::Query;
	using namespace UE::UAI::ContextResource;
	using namespace UE::UAI::Contract;
	using namespace UE::UAI::Discovery;
	using namespace UE::UAI::DomainAdapters::AssetProjectRead;
	using namespace UE::UAI::Projection;
	using namespace UE::UAI::StateSemantic;

	FUeAgentDescriptorIdentity SummaryIdentity()
	{
		return FUeAgentDescriptorIdentity(
			TEXT("uai.query.asset.summary"),
			1,
			TEXT(
				"sha256:72a69072c32e4e0e096549473f5607ad"
				"3974a0987d557bfd5eea883f2a70e421"));
	}

	FUeAgentResourceTrackerOwner Resources()
	{
		return MakeShared<FUeAgentResourceTracker, ESPMode::ThreadSafe>(
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

	TSharedPtr<FUeAgentOperationContext> OperationContext(
		FAutomationTestBase* Test,
		const FUeAgentQueryDescriptor& Descriptor,
		const FUeAgentResourceTrackerOwner& ResourceTracker)
	{
		FUeAgentExecutionContextInput Input;
		Input.ProjectInstanceId = TEXT("asset-project-fixture");
		Input.EditorInstanceId = TEXT("asset-project-editor");
		Input.StatePlane = EUeAgentStatePlane::AuthoringMemory;
		Input.WorldType = EUeAgentExecutionWorldType::Editor;
		Input.ContextRevision = TEXT("asset-project-revision-1");

		FUeAgentExecutionContextRefPtr ExecutionContext;
		FUeAgentExecutionContextError ContextError;
		Test->TestTrue(
			TEXT("freeze AssetProjectRead execution context"),
			FUeAgentExecutionContextRef::TryCreate(
				Input,
				SummaryIdentity(),
				EUeAgentRequestKind::Query,
				Descriptor.GetExecutionContextPolicy(),
				ExecutionContext,
				ContextError));
		if (!ExecutionContext.IsValid())
		{
			return nullptr;
		}

		const FUeAgentQueryRequest Request(
			SummaryIdentity(),
			TEXT("request-asset-project-read-provider"),
			FUeAgentContractValue(TEXT("{}")));
		const FUeAgentCallerContext Caller(
			TEXT("fixture-agent"),
			{TEXT("asset_project:read")},
			TEXT("corr-asset-project-read-provider"));
		const FUeAgentOperationDeadline Deadline =
			FUeAgentOperationDeadline::FromTimeout(
				30.0,
				[]() { return 100.0; });
		const FUeAgentCancellationSource Cancellation(
			[]() { return 100.0; });
		TSharedPtr<FUeAgentOperationContext> Context;
		FUeAgentOperationContextError OperationError;
		Test->TestTrue(
			TEXT("create AssetProjectRead operation context"),
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

	FUeAgentCandidateBinding Target()
	{
		return FUeAgentCandidateBinding(
			TEXT("asset-project-read-target"),
			FUeAgentCandidateSummary(
				FUeAgentCanonicalIdentity(
					TEXT("asset:/Game/UAI/Fixture"),
					TEXT("asset"),
					TEXT("/Game/UAI/Fixture"),
					TEXT("Fixture"),
					TEXT("target"),
					TEXT("revision-1")),
				SummaryIdentity(),
				TEXT("asset"),
				TEXT("Fixture"),
				TEXT("owned AssetProjectRead fixture"),
				{TEXT("AssetProjectRead"), TEXT("Fixture")}),
			FUeAgentCatalogRevision(
				TEXT("asset-project-read-fixture"),
				TEXT("revision-1"),
				1),
			TEXT("uai://asset/Game/UAI/Fixture"),
			TEXT("asset-project-read"),
			{EUeAgentCandidateOperation::Read},
			TEXT("asset-project-read-provider-test"));
	}

	TSharedPtr<const FUeAgentCanonicalSnapshot> Snapshot(
		FAutomationTestBase* Test,
		FUeAgentResourceTracker& ResourceTracker)
	{
		const FUeAgentStateRevision Revision(
			TEXT("asset-project-read"),
			TEXT("revision-1"),
			1);
		FUeAgentCanonicalValuePtr Value;
		FUeAgentStateSemanticError Error;
		Test->TestTrue(
			TEXT("create AssetProjectRead C4 value"),
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
				TEXT("asset.summary:path"),
				TEXT("asset"),
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
				TEXT("asset-project-native"),
				TEXT("independent-readback"),
				Value.ToSharedRef()),
			FUeAgentStateDetails(
				TEXT("Asset Path"),
				TEXT("owned fixture asset path"),
				{TEXT("AssetProjectRead"), TEXT("Asset")},
				false,
				TEXT("available"),
				{}),
			Revision);
		TSharedPtr<const FUeAgentCanonicalSnapshot> Result;
		Test->TestTrue(
			TEXT("create AssetProjectRead C4 snapshot"),
			FUeAgentCanonicalSnapshot::TryCreate(
				TEXT("asset-project-read-summary"),
				EUeAgentStatePlane::AuthoringMemory,
				Revision,
				FUeAgentTemporalPoint(TEXT("fixture-clock"), TEXT("1")),
				FUeAgentStateProvenance(
					TEXT("independent_readback"),
					TEXT("asset-project-read"),
					TEXT("fixture-asset-project-read")),
				{Entry},
				ResourceTracker,
				Result,
				Error));
		return Result;
	}

	class FRecordingHook final : public IUeAgentAssetProjectReadHook
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
			FUeAgentAssetProjectReadError& OutError) override
		{
			++Calls;
			LastInput = CanonicalInput.GetJsonValue();
			LastTarget = InTarget;
			LastContext = &Context;
			if (bFail)
			{
				OutError.Code =
					TEXT("fixture_asset_project_read_failed");
				OutError.SubjectId = TEXT("fixture-asset");
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
	FUeAgentAssetProjectReadProviderBoundaryTest,
	"UeAgentInterface.DomainAdapters.AssetProjectRead.E1.Provider."
		"ExactHookAndContext",
	EAutomationTestFlags::EditorContext
		| EAutomationTestFlags::EngineFilter)

bool FUeAgentAssetProjectReadProviderBoundaryTest::RunTest(
	const FString& Parameters)
{
	TSharedPtr<const FUeAgentDescriptorRegistry> Registry;
	FString RegistryError;
	TestTrue(
		TEXT("create production AssetProjectRead descriptors"),
		TryCreateAssetProjectReadDescriptorRegistry(
			Registry,
			RegistryError));
	if (!Registry.IsValid())
	{
		AddError(TEXT("asset_project_read_registry_failed:")
			+ RegistryError);
		return false;
	}
	const TOptional<FUeAgentQueryDescriptor> Descriptor =
		Registry->FindQueryExact(SummaryIdentity());
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
	FUeAgentAssetProjectReadProvider Provider(
		TEXT("asset_project.read.asset_summary"),
		Hook);
	TSharedPtr<const FUeAgentProjectionSourceValue> Source;
	FUeAgentQueryBindingError Error;
	const FUeAgentCandidateBinding ResolvedTarget = Target();
	TestTrue(
		TEXT("provider forwards to one exact hook"),
		Provider.TryQuery(
			Descriptor.GetValue(),
			FUeAgentContractValue(TEXT("{}")),
			ResolvedTarget,
			*Context,
			Source,
			Error));
	TestEqual(TEXT("hook called once"), Hook->Calls, 1);
	TestEqual(TEXT("canonical input unchanged"), Hook->LastInput, FString(TEXT("{}")));
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
			FUeAgentContractValue(TEXT("{}")),
			{},
			*Context,
			Source,
			Error));
	TestEqual(TEXT("failed hook is not retried"), Hook->Calls, 2);
	TestFalse(TEXT("failure leaks no projection source"), Source.IsValid());
	TestEqual(
		TEXT("typed hook error preserved"),
		Error.Code,
		FString(TEXT("fixture_asset_project_read_failed")));

	const TSharedRef<FRecordingHook> MismatchHook =
		MakeShared<FRecordingHook>(ExpectedSnapshot.ToSharedRef());
	FUeAgentAssetProjectReadProvider Mismatch(
		TEXT("asset_project.read.asset_properties"),
		MismatchHook);
	TestFalse(
		TEXT("binding mismatch rejected"),
		Mismatch.TryQuery(
			Descriptor.GetValue(),
			FUeAgentContractValue(TEXT("{}")),
			{},
			*Context,
			Source,
			Error));
	TestEqual(TEXT("mismatch never calls hook"), MismatchHook->Calls, 0);
	TestEqual(
		TEXT("stable mismatch error"),
		Error.Code,
		FString(TEXT(
			"asset_project_read_provider_binding_mismatch")));
	return true;
}
