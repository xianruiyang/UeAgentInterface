#include "Misc/AutomationTest.h"

#include "Containers/StringConv.h"
#include "Core/ArtifactPublisher/UeAgentArtifactPublisher.h"
#include "Core/HjsonWorkspace/DomainAuthoring/UeAgentDomainHjsonAuthoringKit.h"
#include "Engine/Texture2D.h"
#include "HAL/FileManager.h"
#include "Misc/Guid.h"
#include "Misc/Paths.h"
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

	UE::UAI::ContextResource::FUeAgentResourceTracker MakeExportResources()
	{
		using namespace ContextResource;
		return FUeAgentResourceTracker(FUeAgentResourceBudget({
			{EUeAgentResourceDimension::InputBytes, 1024 * 1024},
			{EUeAgentResourceDimension::OutputBytes, 1024 * 1024},
			{EUeAgentResourceDimension::StringBytes, 1024 * 1024},
			{EUeAgentResourceDimension::Nodes, 4096},
			{EUeAgentResourceDimension::Items, 4096},
			{EUeAgentResourceDimension::Files, 32},
			{EUeAgentResourceDimension::ReferenceEdges, 4096},
			{EUeAgentResourceDimension::WorkUnits, 16384},
			{EUeAgentResourceDimension::EstimatedMemoryBytes, 16 * 1024 * 1024},
		}));
	}

	TSharedRef<FJsonObject> ExportDocument(
		const FString& Schema = TEXT("uai.fixture.edit/1"),
		const FString& RootType = TEXT("/Script/CoreUObject.Object"))
	{
		const TSharedRef<FJsonObject> Root = MakeShared<FJsonObject>();
		Root->SetStringField(TEXT("type"), RootType);
		Root->SetArrayField(
			TEXT("properties"), TArray<TSharedPtr<FJsonValue>>{});
		const TSharedRef<FJsonObject> Document = MakeShared<FJsonObject>();
		Document->SetStringField(TEXT("schema"), Schema);
		Document->SetObjectField(TEXT("root"), Root);
		return Document;
	}

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

	void CheckCandidateSelectorAccepted(
		FAutomationTestBase& Test,
		const TSharedPtr<FJsonObject>& Candidate,
		const FString& Pointer,
		const FString& ExpectedName,
		const FString& ExpectedId)
	{
		FDomainHjsonCandidateSelector Selector;
		FWorkspaceError Error;
		Test.TestTrue(TEXT("candidate selector is accepted"),
			TryReadCandidateSelector(Candidate, Pointer, Selector, Error));
		Test.TestEqual(TEXT("selector name is trimmed"),
			Selector.Name, ExpectedName);
		Test.TestEqual(TEXT("selector id is trimmed"),
			Selector.Id, ExpectedId);
		Test.TestTrue(TEXT("accepted selector is valid"), Selector.IsValid());
	}

	void CheckCandidateSelectorRejected(
		FAutomationTestBase& Test,
		const TSharedPtr<FJsonObject>& Candidate,
		const FString& Pointer,
		const FString& ExpectedCode)
	{
		FDomainHjsonCandidateSelector Selector;
		FWorkspaceError Error;
		Test.TestFalse(TEXT("invalid candidate selector fails closed"),
			TryReadCandidateSelector(Candidate, Pointer, Selector, Error));
		Test.TestEqual(TEXT("candidate selector error is typed"),
			Error.GetCode(), ExpectedCode);
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUeAgentDomainHjsonDirectoryExportPlanTest,
	"UAI.Root.T62.DomainAuthoring.DirectoryExportPlan",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FUeAgentDomainHjsonDirectoryExportPlanTest::RunTest(
	const FString& Parameters)
{
	using namespace UE::UAI::HjsonWorkspace;
	using namespace UE::UAI::HjsonWorkspace::DomainAuthoring;
	using namespace UE::UAI::Tests::DomainHjsonAuthoring;
	(void)Parameters;
	UE::UAI::ContextResource::FUeAgentResourceTracker Resources =
		MakeExportResources();
	FExportWorkspacePlanPtr Plan;
	FWorkspaceError Error;
	TestTrue(TEXT("a domain-owned directory export plan builds"),
		TryBuildDomainHjsonDirectoryExportPlan(
			TEXT("/Game/UAI/Fixture.Fixture"),
			FixtureContract,
			TEXT("rev-1"),
			TEXT("fixture-profile"),
			TEXT("fixture-provider"),
			TEXT("root.hjson"),
			ExportDocument(),
			Resources,
			Plan,
			Error));
	if (!Plan.IsValid())
	{
		return false;
	}
	TestEqual(TEXT("the plan is a directory workspace"),
		Plan->GetWorkspaceKind(), EWorkspaceKind::Directory);
	TestEqual(TEXT("the entry identity is exact"),
		Plan->GetEntryFile(), FString(TEXT("root.hjson")));
	TestEqual(TEXT("manifest plus one entry are planned"),
		Plan->GetFiles().Num(), 2);
	const FExportFilePlan* Manifest = Plan->GetFiles().FindByPredicate(
		[](const FExportFilePlan& File)
		{
			return File.GetRelativePath() == TEXT(".uai/workspace.hjson");
		});
	const FExportFilePlan* Entry = Plan->GetFiles().FindByPredicate(
		[](const FExportFilePlan& File)
		{
			return File.GetRelativePath() == TEXT("root.hjson");
		});
	TestNotNull(TEXT("the workspace manifest is planned"), Manifest);
	TestNotNull(TEXT("the unique entry document is planned"), Entry);
	if (Manifest)
	{
		const FUTF8ToTCHAR Text(
			reinterpret_cast<const ANSICHAR*>(Manifest->GetCanonicalUtf8().GetData()),
			Manifest->GetCanonicalUtf8().Num());
		const FString ManifestText(Text.Length(), Text.Get());
		TestTrue(TEXT("the manifest schema is exact"),
			ManifestText.Contains(TEXT("uai.content_graph.workspace/1")));
		TestTrue(TEXT("the manifest target is exact"),
			ManifestText.Contains(TEXT("/Game/UAI/Fixture.Fixture")));
		TestTrue(TEXT("the manifest base revision is exact"),
			ManifestText.Contains(TEXT("\"base_revision\":\"rev-1\"")));
		TestTrue(TEXT("the manifest entry is exact"),
			ManifestText.Contains(TEXT("\"entry_file\":\"root.hjson\"")));
		TestTrue(TEXT("the manifest declares an empty reference set"),
			ManifestText.Contains(TEXT("\"references\":[]")));
	}
	if (Entry)
	{
		const FUTF8ToTCHAR Text(
			reinterpret_cast<const ANSICHAR*>(Entry->GetCanonicalUtf8().GetData()),
			Entry->GetCanonicalUtf8().Num());
		const FString EntryText(Text.Length(), Text.Get());
		TestTrue(TEXT("the entry schema is exact"),
			EntryText.Contains(TEXT("uai.fixture.edit/1")));
		TestTrue(TEXT("the entry root type is exact"),
			EntryText.Contains(TEXT("/Script/CoreUObject.Object")));
	}
	TestTrue(TEXT("the plan fingerprint is SHA-256"),
		Plan->GetPlanFingerprint().StartsWith(TEXT("sha256:")));
	return !HasAnyErrors();
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUeAgentDomainHjsonDirectoryExportFailureTest,
	"UAI.Root.T62.DomainAuthoring.DirectoryExportFailures",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FUeAgentDomainHjsonDirectoryExportFailureTest::RunTest(
	const FString& Parameters)
{
	using namespace UE::UAI::HjsonWorkspace;
	using namespace UE::UAI::HjsonWorkspace::DomainAuthoring;
	using namespace UE::UAI::Tests::DomainHjsonAuthoring;
	(void)Parameters;
	auto Rejects = [this](
		const TSharedPtr<FJsonObject>& Document,
		const FString& Entry,
		const FString& ExpectedCode)
	{
		UE::UAI::ContextResource::FUeAgentResourceTracker Resources =
			MakeExportResources();
		FExportWorkspacePlanPtr Plan;
		FWorkspaceError Error;
		TestFalse(*ExpectedCode,
			TryBuildDomainHjsonDirectoryExportPlan(
				TEXT("/Game/UAI/Fixture.Fixture"),
				FixtureContract,
				TEXT("rev-1"),
				TEXT("fixture-profile"),
				TEXT("fixture-provider"),
				Entry,
				Document,
				Resources,
				Plan,
				Error));
		TestEqual(TEXT("the rejection is typed"),
			Error.GetCode(), ExpectedCode);
	};
	Rejects(ExportDocument(TEXT("uai.other/1")), TEXT("root.hjson"),
		TEXT("domain_hjson_export_schema_mismatch"));
	Rejects(ExportDocument(TEXT("uai.fixture.edit/1"), TEXT("/Script/Engine.Actor")),
		TEXT("root.hjson"), TEXT("domain_hjson_export_root_type_mismatch"));
	Rejects(ExportDocument(), TEXT("../root.hjson"),
		TEXT("domain_hjson_export_entry_invalid"));
	return !HasAnyErrors();
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUeAgentDomainHjsonDirectoryExportStagingVerifyTest,
	"UAI.Root.T62.DomainAuthoring.DirectoryExportStagingVerify",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FUeAgentDomainHjsonDirectoryExportStagingVerifyTest::RunTest(
	const FString& Parameters)
{
	using namespace UE::UAI::ArtifactPublisher;
	using namespace UE::UAI::HjsonWorkspace;
	using namespace UE::UAI::HjsonWorkspace::DomainAuthoring;
	using namespace UE::UAI::Tests::DomainHjsonAuthoring;
	(void)Parameters;
	const FString Output = FPaths::Combine(
		FPaths::ProjectSavedDir(),
		TEXT("UAI/Tests/T62DomainExport_"),
		FGuid::NewGuid().ToString(EGuidFormats::Digits));
	IFileManager::Get().DeleteDirectory(*Output, false, true);
	UE::UAI::ContextResource::FUeAgentResourceTracker BuildResources =
		MakeExportResources();
	FExportWorkspacePlanPtr Plan;
	FWorkspaceError Error;
	if (!TestTrue(TEXT("the publish fixture plan builds"),
		TryBuildDomainHjsonDirectoryExportPlan(
			TEXT("/Game/UAI/Fixture.Fixture"), FixtureContract,
			TEXT("rev-1"), TEXT("fixture-profile"),
			TEXT("fixture-provider"), TEXT("root.hjson"),
			ExportDocument(), BuildResources, Plan, Error))
		|| !Plan.IsValid())
	{
		return false;
	}
	UE::UAI::ContextResource::FUeAgentResourceTracker PublishResources =
		MakeExportResources();
	FArtifactPublishReceipt Receipt;
	FArtifactPublishError PublishError;
	const FArtifactPublishRequest Request(
		Plan.ToSharedRef(), Output, TEXT("fixture-domain-export"));
	TestTrue(TEXT("ArtifactPublisher publishes the owned workspace"),
		TryPublishExportPlan(
			Request, PublishResources, Receipt, PublishError));
	UE::UAI::ContextResource::FUeAgentResourceTracker VerifyResources =
		MakeExportResources();
	TestTrue(TEXT("active import parser verifies the staged workspace"),
		TryVerifyDomainHjsonStagedWorkspace(
			Output,
			TEXT("/Game/UAI/Fixture.Fixture"),
			FixtureContract,
			TEXT("rev-1"),
			TEXT("root.hjson"),
			VerifyResources,
			Error));
	TestTrue(TEXT("the owned workspace cleanup succeeds"),
		IFileManager::Get().DeleteDirectory(*Output, false, true));
	return !HasAnyErrors();
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

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUeAgentDomainHjsonCandidateSelectorTest,
	"UAI.Root.T85.DomainAuthoring.CandidateSelector",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FUeAgentDomainHjsonCandidateSelectorTest::RunTest(
	const FString& Parameters)
{
	using namespace UE::UAI::HjsonWorkspace;
	using namespace UE::UAI::HjsonWorkspace::DomainAuthoring;
	using namespace UE::UAI::Tests::DomainHjsonAuthoring;
	(void)Parameters;
	const FString Pointer = TEXT("/edits/0/value/candidate");

	TSharedPtr<FJsonObject> Candidate = MakeShared<FJsonObject>();
	Candidate->SetStringField(TEXT("name"), TEXT("  Native Cube  "));
	Candidate->SetStringField(TEXT("ignored_assertion"), TEXT("preserved"));
	CheckCandidateSelectorAccepted(
		*this, Candidate, Pointer, TEXT("Native Cube"), TEXT(""));

	Candidate = MakeShared<FJsonObject>();
	Candidate->SetStringField(TEXT("id"), TEXT("  candidate:cube  "));
	CheckCandidateSelectorAccepted(
		*this, Candidate, Pointer, TEXT(""), TEXT("candidate:cube"));

	Candidate = MakeShared<FJsonObject>();
	Candidate->SetStringField(TEXT("name"), TEXT("Native Cube"));
	Candidate->SetStringField(TEXT("id"), TEXT("candidate:cube"));
	CheckCandidateSelectorAccepted(
		*this, Candidate, Pointer, TEXT("Native Cube"), TEXT("candidate:cube"));

	Candidate = MakeShared<FJsonObject>();
	Candidate->SetStringField(TEXT("candidate_id"), TEXT("  candidate:cube  "));
	CheckCandidateSelectorRejected(*this, Candidate, Pointer,
		TEXT("domain_hjson_candidate_selector_field_unsupported"));

	Candidate = MakeShared<FJsonObject>();
	Candidate->SetStringField(TEXT("id"), TEXT("candidate:cube"));
	Candidate->SetStringField(TEXT("candidate_id"), TEXT(" candidate:cube "));
	CheckCandidateSelectorRejected(*this, Candidate, Pointer,
		TEXT("domain_hjson_candidate_selector_field_unsupported"));

	Candidate = MakeShared<FJsonObject>();
	Candidate->SetStringField(TEXT("id"), TEXT("candidate:one"));
	Candidate->SetStringField(TEXT("candidate_id"), TEXT("candidate:two"));
	CheckCandidateSelectorRejected(*this, Candidate, Pointer,
		TEXT("domain_hjson_candidate_selector_field_unsupported"));

	Candidate = MakeShared<FJsonObject>();
	Candidate->SetField(TEXT("name"), MakeShared<FJsonValueNumber>(7.0));
	CheckCandidateSelectorRejected(*this, Candidate, Pointer,
		TEXT("domain_hjson_candidate_selector_invalid"));

	Candidate = MakeShared<FJsonObject>();
	Candidate->SetField(TEXT("id"), MakeShared<FJsonValueBoolean>(true));
	CheckCandidateSelectorRejected(*this, Candidate, Pointer,
		TEXT("domain_hjson_candidate_selector_invalid"));

	Candidate = MakeShared<FJsonObject>();
	Candidate->SetStringField(TEXT("candidate_id"), TEXT("alias"));
	Candidate->SetField(TEXT("name"), MakeShared<FJsonValueNull>());
	CheckCandidateSelectorRejected(*this, Candidate, Pointer,
		TEXT("domain_hjson_candidate_selector_field_unsupported"));

	Candidate = MakeShared<FJsonObject>();
	Candidate->SetStringField(TEXT("name"), TEXT("  \t  "));
	Candidate->SetStringField(TEXT("id"), TEXT(" \r\n "));
	CheckCandidateSelectorRejected(*this, Candidate, Pointer,
		TEXT("domain_hjson_candidate_selector_required"));

	FDomainHjsonCandidateSelector Selector;
	FWorkspaceError Error;
	TestFalse(TEXT("null candidate fails closed"),
		TryReadCandidateSelector(nullptr, Pointer, Selector, Error));
	TestEqual(TEXT("null candidate error is typed"), Error.GetCode(),
		FString(TEXT("domain_hjson_candidate_selector_invalid")));
	return !HasAnyErrors();
}
