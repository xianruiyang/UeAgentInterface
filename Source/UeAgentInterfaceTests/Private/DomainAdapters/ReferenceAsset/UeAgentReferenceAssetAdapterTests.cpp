#include "Misc/AutomationTest.h"

#include "DomainAdapters/ReferenceAsset/UeAgentReferenceAssetAdapter.h"


#include "Core/ContextResource/UeAgentContextResource.h"
#include "Core/Contract/UeAgentContract.h"
#include "Core/Discovery/UeAgentDiscovery.h"
#include "Core/StateSemantic/UeAgentStateSemantic.h"

namespace
{
	using namespace UE::UAI::ContextResource;
	using namespace UE::UAI::Contract;
	using namespace UE::UAI::Discovery;
	using namespace UE::UAI::DomainAdapters::ReferenceAsset;
	using namespace UE::UAI::StateSemantic;

	TSharedRef<FUeAgentResourceTracker, ESPMode::ThreadSafe> MakeTracker()
	{
		return MakeShared<
			FUeAgentResourceTracker,
			ESPMode::ThreadSafe>(
			FUeAgentResourceBudget({
				{EUeAgentResourceDimension::InputBytes, 1048576},
				{EUeAgentResourceDimension::OutputBytes, 1048576},
				{EUeAgentResourceDimension::StringBytes, 1048576},
				{EUeAgentResourceDimension::Nodes, 4096},
				{EUeAgentResourceDimension::Items, 4096},
				{EUeAgentResourceDimension::Files, 64},
				{EUeAgentResourceDimension::ReferenceEdges, 4096},
				{EUeAgentResourceDimension::WorkUnits, 65536},
				{
					EUeAgentResourceDimension::EstimatedMemoryBytes,
					16777216,
				},
			}));
	}

	FUeAgentDescriptorIdentity OperationDescriptor()
	{
		return FUeAgentDescriptorIdentity(
			TEXT("uai.operation.reference.crud"),
			1,
			TEXT(
				"sha256:9f8882546a7f47a2f80b43b038c09eb4e5ea20bf4f52ed65cdc8432d43cfdd12"));
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUeAgentReferenceAssetAdapterContractTest,
	"UeAgentInterface.DomainAdapters.ReferenceAsset.Contract",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FUeAgentReferenceAssetAdapterContractTest::RunTest(
	const FString& Parameters)
{
	const FString ObjectPath =
		TEXT("/Game/UAI/Reference/P4_013_AdapterFixture.")
		TEXT("P4_013_AdapterFixture");
	const TSharedRef<FUeAgentReferenceAssetCatalogSource> Source =
		MakeShared<FUeAgentReferenceAssetCatalogSource>(
			OperationDescriptor(),
			TEXT("p4-013-adapter-session"));
	TestTrue(
		TEXT("reference CRUD owns a separate C2 catalog kind"),
		Source->GetCatalogKinds()
			== TArray<FString>{TEXT("uai_reference_asset")});

	const TSharedRef<
		FUeAgentResourceTracker,
		ESPMode::ThreadSafe> Resources = MakeTracker();
	const auto Clock = []() { return 100.0; };
	const FUeAgentOperationDeadline Deadline =
		FUeAgentOperationDeadline::FromTimeout(30.0, Clock);
	FUeAgentCancellationSource Cancellation(Clock);
	TSharedPtr<const FUeAgentCatalogSnapshot> Catalog;
	FUeAgentDiscoveryError Error;
	TestTrue(
		TEXT("an allowed absent asset path is still a creatable candidate"),
		Source->TrySearch(
			FUeAgentDiscoveryQuery(
				TEXT("uai_reference_asset"),
				ObjectPath,
				EUeAgentDiscoveryMatchMode::Exact,
				EUeAgentMaterializationPolicy::IndexOnly),
			*Resources,
			Deadline,
			Cancellation.GetToken(),
			Catalog,
			Error));
	if (!Catalog.IsValid()
		|| Catalog->GetCandidates().Num() != 1)
	{
		AddError(
			TEXT("reference candidate missing: ")
			+ Error.Code
			+ TEXT(":")
			+ Error.Detail);
		return false;
	}
	TestTrue(
		TEXT("candidate advertises the canonical operation descriptor"),
		Catalog->GetCandidates()[0].GetDescriptorIdentity()
			== OperationDescriptor());

	TSharedPtr<const FUeAgentCandidateBinding> Binding;
	TestTrue(
		TEXT("exact resolve reconstructs the same candidate"),
		Source->TryResolveExact(
			Catalog->GetCandidates()[0]
				.GetIdentity()
				.GetCanonicalKey(),
			Catalog->GetRevision(),
			*Resources,
			Deadline,
			Cancellation.GetToken(),
			Binding,
			Error));
	if (!Binding.IsValid())
	{
		return false;
	}
	TestEqual(
		TEXT("binding public ref remains the UE object path"),
		Binding->GetPublicRef(),
		ObjectPath);
	TestTrue(
		TEXT("binding exposes all typed CRUD operations"),
		Binding->Supports(EUeAgentCandidateOperation::Create)
			&& Binding->Supports(EUeAgentCandidateOperation::Update)
			&& Binding->Supports(EUeAgentCandidateOperation::Replace)
			&& Binding->Supports(EUeAgentCandidateOperation::Delete));

	const TSharedRef<FUeAgentReferenceAssetReadHook> ReadHook =
		MakeShared<FUeAgentReferenceAssetReadHook>(
			TEXT("p4-013-adapter-session"));
	TSharedPtr<const
		UE::UAI::DomainAdapters::GenericRead::FUeAgentUeReadCapture>
		Capture;
	UE::UAI::DomainAdapters::GenericRead::FUeAgentGenericReadError
		ReadError;
	TestTrue(
		TEXT("persisted absence produces an independent empty snapshot"),
		ReadHook->TryCapture(
			FUeAgentStateReadRequest(
				*Binding,
				EUeAgentStatePlane::Persisted),
			*Resources,
			Deadline,
			Cancellation.GetToken(),
			Capture,
			ReadError));
	TestTrue(
		TEXT("absence is not echoed as a null value"),
		Capture.IsValid() && Capture->GetEntries().IsEmpty());
	return true;
}

