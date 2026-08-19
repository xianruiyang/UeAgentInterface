#include "Misc/AutomationTest.h"

#include "Core/HjsonWorkspace/DomainAuthoring/UeAgentDomainHjsonAuthoringKit.h"

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
		const FString& ExtraDocument = FString())
	{
		return FString::Printf(
			TEXT("{\"target\":\"%s\",\"expected_revision\":\"rev-1\",")
			TEXT("\"source\":{\"kind\":\"resolved\",")
			TEXT("\"target_identity\":\"%s\",\"entry_file\":\"%s\",")
			TEXT("\"documents\":[{\"path\":\"%s\",\"document\":")
			TEXT("\"{\\\"schema\\\":\\\"%s\\\",\\\"root\\\":{")
			TEXT("\\\"type\\\":\\\"%s\\\",\\\"properties\\\":[]}}\"}%s]}}"),
			*Target, *TargetIdentity, *Entry, *Entry, *Schema, *RootType,
			*ExtraDocument);
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUeAgentDomainHjsonAuthoringDecodeTest,
	"UAI.Core.HjsonWorkspace.DomainAuthoring.DecodeResolvedDocument",
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
	return !HasAnyErrors();
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUeAgentDomainHjsonAuthoringPrewriteFailureTest,
	"UAI.Core.HjsonWorkspace.DomainAuthoring.PrewriteFailures",
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
			TEXT(",{\"path\":\"root.hjson\",\"document\":\"{}\"}")),
		TEXT("domain_hjson_entry_document_ambiguous"));
	return !HasAnyErrors();
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUeAgentDomainHjsonAuthoringCaptureResultTest,
	"UAI.Core.HjsonWorkspace.DomainAuthoring.CaptureAndResult",
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
	TestTrue(TEXT("an independent readback result builds"),
		TryBuildImportSuccessPayload(
			TEXT("fixture-provider"), TEXT("/Game/UAI/Fixture"),
			Capture.Revision, Snapshot, nullptr, Payload, Error));
	TestTrue(TEXT("the payload records independent readback"),
		Payload.Contains(TEXT("independent_readback")));
	TestTrue(TEXT("the payload records reload"),
		Payload.Contains(TEXT("\"reloaded\":true")));
	return !HasAnyErrors();
}
