#include "Misc/AutomationTest.h"

#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Materials/Material.h"
#include "Materials/MaterialExpressionConstant.h"
#include "Domains/Material/Authoring/UeAgentMaterialGraphAuthoring.h"

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUeAgentMaterialCommonGraphAdapterTest,
	"UeAgentInterface.DomainAdapters.Material.CommonGraphAdapter",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FUeAgentMaterialCommonGraphAdapterTest::RunTest(const FString& Parameters)
{
	(void)Parameters;
	UMaterial* Material = NewObject<UMaterial>(GetTransientPackage());
	TestNotNull(TEXT("transient Material exists"), Material);
	if (!Material)
	{
		return false;
	}

	TSharedRef<FJsonObject> Node = MakeShared<FJsonObject>();
	Node->SetStringField(TEXT("id"), TEXT("Constant1"));
	Node->SetStringField(TEXT("name"), TEXT("Constant"));
	Node->SetNumberField(TEXT("x"), 120.0);
	Node->SetNumberField(TEXT("y"), 80.0);
	TSharedRef<FJsonObject> CandidateContext = MakeShared<FJsonObject>();
	CandidateContext->SetStringField(
		TEXT("expression_class"),
		UMaterialExpressionConstant::StaticClass()->GetPathName());
	Node->SetObjectField(TEXT("candidate_context"), CandidateContext);
	TSharedRef<FJsonObject> Nodes = MakeShared<FJsonObject>();
	Nodes->SetObjectField(TEXT("Constant1"), Node);
	TSharedRef<FJsonObject> CommonGraph = MakeShared<FJsonObject>();
	CommonGraph->SetObjectField(TEXT("nodes"), Nodes);
	CommonGraph->SetArrayField(
		TEXT("connections"), TArray<TSharedPtr<FJsonValue>>{});

	UeAgentMaterialAuthoring::FPreparedGraph Prepared;
	FString Error, ErrorDetail;
	TestTrue(
		TEXT("Material typed boundary accepts the resolved common graph"),
		Prepared.Prepare(
			*Material,
			StaticCastSharedRef<const FJsonObject>(CommonGraph),
			TEXT("/Game/UAI/MaterialCommonGraphAdapter"),
			TEXT("MaterialGraph"),
			Error, ErrorDetail));
	if (!Error.IsEmpty())
	{
		AddError(Error);
	}
	TestTrue(TEXT("prepared graph is ready"), Prepared.IsReady());
	TestEqual(TEXT("one common node becomes one native expression"),
		Prepared.GetExpressionCount(), 1);
	TestEqual(TEXT("empty common connections remain empty"),
		Prepared.GetEdgeCount(), 0);
	return !HasAnyErrors();
}
