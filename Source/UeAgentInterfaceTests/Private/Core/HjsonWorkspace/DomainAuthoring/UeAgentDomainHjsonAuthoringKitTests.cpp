#include "Misc/AutomationTest.h"

#include "Core/HjsonWorkspace/DomainAuthoring/UeAgentDomainHjsonAuthoringKit.h"
#include "Engine/Texture2D.h"
#include "Misc/Guid.h"
#include "UObject/Package.h"
#include "UObject/UObjectGlobals.h"

namespace UE::UAI::Tests::DomainHjsonAuthoring
{
	using namespace HjsonWorkspace;
	using namespace HjsonWorkspace::DomainAuthoring;

	const FDomainHjsonDocumentContract FixtureContract(
		TEXT("fixture-domain"),
		TEXT("uai.fixture.edit/1"),
		TEXT("/Script/CoreUObject.Object"));

	FString Params(
		const FString& Target = TEXT("/Game/UAI/Fixture"),
		const FString& TargetIdentity = TEXT("/Game/UAI/Fixture"),
		const FString& Entry = TEXT("root.hjson"),
		const FString& Schema = TEXT("uai.fixture.edit/1"),
		const FString& RootType = TEXT("/Script/CoreUObject.Object"),
		const FString& ExtraDocument = FString(),
		const FString& Carrier = TEXT("inline"),
		const FString& BaseRevision = FString())
	{
		return FString::Printf(
			TEXT("{\"target\":\"%s\",\"expected_revision\":\"rev-1\",")
			TEXT("\"source\":{\"kind\":\"resolved\",")
			TEXT("\"carrier\":\"%s\",\"source_unit\":\"fixture:%s\",")
			TEXT("\"target_identity\":\"%s\",\"base_revision\":\"%s\",")
			TEXT("\"entry_file\":\"%s\",")
			TEXT("\"source_fingerprint\":\"sha256:1111111111111111111111111111111111111111111111111111111111111111\",")
			TEXT("\"semantic_fingerprint\":\"sha256:2222222222222222222222222222222222222222222222222222222222222222\",")
			TEXT("\"documents\":[{\"path\":\"%s\",\"document\":")
			TEXT("\"{\\\"schema\\\":\\\"%s\\\",\\\"root\\\":{")
			TEXT("\\\"type\\\":\\\"%s\\\",\\\"properties\\\":[]}}\",")
			TEXT("\"content_hash\":\"sha256:3333333333333333333333333333333333333333333333333333333333333333\"}%s]}}"),
			*Target, *Carrier, *Carrier, *TargetIdentity, *BaseRevision,
			*Entry, *Entry, *Schema, *RootType, *ExtraDocument);
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUeAgentDomainHjsonAuthoringDecodeTest,
	"UAI.Root.T36.DomainAuthoring.DecodeResolvedDocument",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FUeAgentDomainHjsonAuthoringDecodeTest::RunTest(const FString& Parameters)
{
	using namespace UE::UAI::HjsonWorkspace;
	using namespace UE::UAI::HjsonWorkspace::DomainAuthoring;
	using namespace UE::UAI::Tests::DomainHjsonAuthoring;
	(void)Parameters;
	FResolvedDomainHjsonDocument Decoded;
	FWorkspaceError Error;
	TestTrue(TEXT("a canonical resolved source decodes"),
		TryDecodeResolvedDocument(
			TEXT("/Game/UAI/Fixture"), Params(), FixtureContract, Decoded, Error));
	TestEqual(TEXT("the exact target is retained"),
		Decoded.TargetIdentity, FString(TEXT("/Game/UAI/Fixture")));
	TestEqual(TEXT("the expected revision is retained"),
		Decoded.ExpectedRevision, FString(TEXT("rev-1")));
	TestTrue(TEXT("the domain root is parsed"), Decoded.Root.IsValid());
	TestEqual(TEXT("the carrier is retained"),
		Decoded.Carrier, FString(TEXT("inline")));
	TestTrue(TEXT("the semantic fingerprint is retained"),
		Decoded.SemanticFingerprint.StartsWith(TEXT("sha256:")));
	return !HasAnyErrors();
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUeAgentDomainHjsonAuthoringPrewriteFailureTest,
	"UAI.Root.T36.DomainAuthoring.PrewriteFailures",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FUeAgentDomainHjsonAuthoringPrewriteFailureTest::RunTest(
	const FString& Parameters)
{
	using namespace UE::UAI::HjsonWorkspace;
	using namespace UE::UAI::HjsonWorkspace::DomainAuthoring;
	using namespace UE::UAI::Tests::DomainHjsonAuthoring;
	(void)Parameters;
	auto Rejects = [this](const FString& Input, const FString& Code)
	{
		FResolvedDomainHjsonDocument Decoded;
		FWorkspaceError Error;
		TestFalse(*Code,
			TryDecodeResolvedDocument(
				TEXT("/Game/UAI/Fixture"), Input, FixtureContract, Decoded, Error));
		TestEqual(*FString::Printf(TEXT("%s is exact"), *Code),
			Error.GetCode(), Code);
	};
	Rejects(
		Params(TEXT("/Game/UAI/Fixture"), TEXT("/Game/UAI/Other")),
		TEXT("domain_hjson_target_identity_mismatch"));
	Rejects(
		Params(TEXT("/Game/UAI/Fixture"), TEXT("/Game/UAI/Fixture"),
			TEXT("root.hjson"), TEXT("uai.other/1")),
		TEXT("domain_hjson_schema_mismatch"));
	Rejects(
		Params(TEXT("/Game/UAI/Fixture"), TEXT("/Game/UAI/Fixture"),
			TEXT("root.hjson"), TEXT("uai.fixture.edit/1"),
			TEXT("/Script/Engine.Actor")),
		TEXT("domain_hjson_root_type_mismatch"));
	Rejects(
		Params(TEXT("/Game/UAI/Fixture"), TEXT("/Game/UAI/Fixture"),
			TEXT("root.hjson"), TEXT("uai.fixture.edit/1"),
			TEXT("/Script/CoreUObject.Object"),
			FString(TEXT(",{\"path\":\"root.hjson\",\"document\":\"{}\","))
				+ TEXT("\"content_hash\":\"sha256:4444444444444444444444444444444444444444444444444444444444444444\"}")),
		TEXT("domain_hjson_entry_document_ambiguous"));
	Rejects(
		Params(TEXT("/Game/UAI/Fixture"), TEXT("/Game/UAI/Fixture"),
			TEXT("root.hjson"), TEXT("uai.fixture.edit/1"),
			TEXT("/Script/CoreUObject.Object"), FString(),
			TEXT("directory"), TEXT("rev-0")),
		TEXT("domain_hjson_base_revision_mismatch"));
	return !HasAnyErrors();
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUeAgentDomainHjsonAuthoringCaptureResultTest,
	"UAI.Root.T36.DomainAuthoring.LifecycleReceipt",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FUeAgentDomainHjsonAuthoringCaptureResultTest::RunTest(
	const FString& Parameters)
{
	using namespace UE::UAI::DomainAdapters::ContentGraphRead;
	using namespace UE::UAI::HjsonWorkspace;
	using namespace UE::UAI::HjsonWorkspace::DomainAuthoring;
	(void)Parameters;
	FUeAgentContentGraphCapture Capture;
	Capture.DomainId = TEXT("fixture-domain");
	Capture.Revision = TEXT("rev-2");
	FUeAgentContentGraphItem Item;
	Item.ItemId = TEXT("fixture:item");
	Item.ItemKind = TEXT("fixture");
	Item.ClassPath = TEXT("/Script/CoreUObject.Object");
	Capture.Items.Add(MoveTemp(Item));
	TSharedPtr<FJsonObject> Snapshot;
	FWorkspaceError Error;
	TestTrue(TEXT("a canonical capture snapshot builds"),
		TryBuildCaptureSnapshot(
			TEXT("/Game/UAI/Fixture"),
			TEXT("/Script/CoreUObject.Object"), Capture, Snapshot, Error));
	FString Payload;
	FDomainHjsonImportLifecycleReceipt LifecycleReceipt;
	TestFalse(TEXT("an incomplete lifecycle receipt is rejected"),
		TryBuildImportSuccessPayload(
			TEXT("fixture-provider"), TEXT("/Game/UAI/Fixture"),
			Capture.Revision, LifecycleReceipt, Snapshot, nullptr,
			Payload, Error));
	TestEqual(TEXT("the lifecycle failure is exact"),
		Error.GetCode(),
		FString(TEXT("domain_hjson_lifecycle_receipt_incomplete")));
	LifecycleReceipt.bMutationApplied = true;
	LifecycleReceipt.bCompileSettled = true;
	LifecycleReceipt.bPackageSaved = true;
	LifecycleReceipt.bUnloadedReloaded = true;
	LifecycleReceipt.PersistenceReceiptId = TEXT("reload:fixture:receipt");
	TestTrue(TEXT("an independent readback result builds"),
		TryBuildImportSuccessPayload(
			TEXT("fixture-provider"), TEXT("/Game/UAI/Fixture"),
			Capture.Revision, LifecycleReceipt, Snapshot, nullptr,
			Payload, Error));
	TestTrue(TEXT("the payload records independent readback"),
		Payload.Contains(TEXT("independent_readback")));
	TestTrue(TEXT("the payload records reload"),
		Payload.Contains(TEXT("\"reloaded\":true")));
	TestTrue(TEXT("the payload records the exact persistence receipt"),
		Payload.Contains(TEXT("reload:fixture:receipt")));
	return !HasAnyErrors();
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUeAgentDomainHjsonCandidateBindingTest,
	"UAI.Root.T39.DomainAuthoring.CandidateBinding",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FUeAgentDomainHjsonCandidateBindingTest::RunTest(
	const FString& Parameters)
{
	using namespace UE::UAI::HjsonWorkspace;
	using namespace UE::UAI::HjsonWorkspace::DomainAuthoring;
	(void)Parameters;
	TSharedPtr<FJsonObject> Candidate = MakeShared<FJsonObject>();
	Candidate->SetStringField(TEXT("name"),
		TEXT("/Script/PCG.PCGCreatePointsGridSettings"));
	Candidate->SetStringField(TEXT("kind"), TEXT("pcg_settings"));
	Candidate->SetArrayField(TEXT("supported_operations"), {
		MakeShared<FJsonValueString>(TEXT("create")),
		MakeShared<FJsonValueString>(TEXT("update")),
	});
	Candidate->SetStringField(TEXT("input_contract"),
		TEXT("uai.query.pcg_node_candidate.binding/1"));
	Candidate->SetStringField(TEXT("placement_contract"),
		TEXT("pcg_graph_node"));
	Candidate->SetStringField(TEXT("required_context"),
		TEXT("pcg_graph"));
	Candidate->SetStringField(TEXT("binding_revision"),
		TEXT("sha256:fixture"));

	FDomainHjsonCandidateBinding Binding;
	FWorkspaceError Error;
	TestTrue(TEXT("a complete public candidate is consumed without remapping"),
		TryReadCandidateBinding(
			Candidate, TEXT("/edits/0/value/candidate"), Binding, Error));
	TestEqual(TEXT("the exact candidate identity is retained"),
		Binding.Name,
		FString(TEXT("/Script/PCG.PCGCreatePointsGridSettings")));
	TestTrue(TEXT("the declared create operation is retained"),
		Binding.SupportsOperation(TEXT("create")));
	TestTrue(TEXT("the declared update operation is retained"),
		Binding.SupportsOperation(TEXT("update")));

	Candidate->RemoveField(TEXT("binding_revision"));
	TestFalse(TEXT("a partial candidate fails closed"),
		TryReadCandidateBinding(
			Candidate, TEXT("/edits/0/value/candidate"), Binding, Error));
	TestEqual(TEXT("the partial-candidate error is typed"),
		Error.GetCode(),
		FString(TEXT("domain_hjson_candidate_binding_invalid")));
	return !HasAnyErrors();
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUeAgentDomainHjsonTargetPresenceTest,
	"UAI.Root.T39.DomainAuthoring.TargetPresence",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FUeAgentDomainHjsonTargetPresenceTest::RunTest(
	const FString& Parameters)
{
	using namespace UE::UAI::HjsonWorkspace::DomainAuthoring;
	(void)Parameters;
	const FString PackageName = TEXT("/Game/UAI/Tests/T39Presence_")
		+ FGuid::NewGuid().ToString(EGuidFormats::Digits);
	const FString ObjectPath = PackageName + TEXT(".Target");
	UPackage* const EmptyPackage = CreatePackage(*PackageName);
	if (!TestNotNull(TEXT("an empty task-owned package is created"), EmptyPackage))
	{
		return false;
	}

	bool bPresent = true;
	TestTrue(TEXT("an empty loaded package has a valid target identity"),
		TryInspectObjectTargetPresence(ObjectPath, bPresent));
	TestFalse(TEXT("an empty loaded package is not an existing asset"), bPresent);

	UObject* const Target = NewObject<UTexture2D>(
		EmptyPackage, TEXT("Target"), RF_Transient);
	TestNotNull(TEXT("a task-owned target object is created"), Target);
	bPresent = false;
	TestTrue(TEXT("a loaded target has a valid target identity"),
		TryInspectObjectTargetPresence(ObjectPath, bPresent));
	TestTrue(TEXT("a loaded target is reported as present"), bPresent);

	return !HasAnyErrors();
}
