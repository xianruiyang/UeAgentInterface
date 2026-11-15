#include "Misc/AutomationTest.h"
#include "Core/HjsonWorkspace/UeAgentHjsonWorkspace.h"

namespace {
using namespace UE::UAI::ContextResource;
using namespace UE::UAI::HjsonWorkspace;
static FUeAgentResourceTracker MakeResources(int64, int64) { return FUeAgentResourceTracker(); }
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUeAgentStrictV2T07DepthResourceTest,
	"UAI.StrictV2.T07.HJSON.MODEL.DeepParsing",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FUeAgentStrictV2T07DepthResourceTest::RunTest(
	const FString& Parameters)
{
	using namespace UE::UAI::ContextResource;
	using namespace UE::UAI::HjsonWorkspace;

	const FString DeepDocument =
		FString::ChrN(512, TEXT('['))
		+ TEXT("0")
		+ FString::ChrN(512, TEXT(']'));
	FUeAgentResourceTracker FullResources = MakeResources(4096, 262144);
	FHjsonDocumentPtr Document;
	FWorkspaceError Error;
	TestTrue(
		TEXT("valid HJSON is not rejected by an artificial nesting limit"),
		TryParseHjsonDocument(
			DeepDocument,
			TEXT("strict_v2:t07:deep"),
			FullResources,
			Document,
			Error));
	TestTrue(TEXT("deep document is available"), Document.IsValid());

	return true;
}
