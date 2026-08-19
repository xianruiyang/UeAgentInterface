#include "DomainAdapters/WorldSpatial/UeAgentWorldSpatialNativeTestSupport.h"

#include "Editor.h"
#include "Kismet2/BlueprintEditorUtils.h"
#include "EdGraph/EdGraph.h"
#include "EdGraph/EdGraphNode.h"
#include "EdGraphSchema_K2.h"
#include "Engine/Blueprint.h"
#include "GraphEditor.h"
#include "UObject/Package.h"
#include "Widgets/SBoxPanel.h"

namespace
{
	using namespace UE::UAI::DomainAdapters::WorldSpatial;
	using namespace UE::UAI::Tests::WorldSpatial;

	class FOwnedGraphFixture final : public FOwnedNativeEnvironmentBase
	{
	public:
		FOwnedGraphFixture()
		{
			const FString PackageName = FString::Printf(
				TEXT("/Temp/UAI/WorldSpatial/%s"),
				*FGuid::NewGuid().ToString(EGuidFormats::Digits));
			Package = CreatePackage(*PackageName);
			Blueprint = NewObject<UBlueprint>(
				Package,
				TEXT("BP_WorldSpatialOwned"),
				RF_Transient);
			if (!Blueprint)
			{
				return;
			}
			Graph = FBlueprintEditorUtils::CreateNewGraph(
				Blueprint,
				TEXT("EventGraph"),
				UEdGraph::StaticClass(),
				UEdGraphSchema_K2::StaticClass());
			if (!Graph)
			{
				return;
			}
			Blueprint->UbergraphPages.Add(Graph);
			Alpha = NewObject<UEdGraphNode>(Graph, TEXT("Alpha"));
			Beta = NewObject<UEdGraphNode>(Graph, TEXT("Beta"));
			if (!Alpha || !Beta)
			{
				return;
			}
			Graph->AddNode(Alpha, false, false);
			Graph->AddNode(Beta, false, false);
			Alpha->NodePosX = -10;
			Alpha->NodePosY = -20;
			Beta->NodePosX = 30;
			Beta->NodePosY = 40;
			GraphEditor = SNew(SGraphEditor).GraphToEdit(Graph);
			GraphEditor->SetViewLocation(FVector2f(12.0f, 24.0f), 0.75f);
			Package->SetDirtyFlag(false);

			Objects.Add(GraphViewRef, Graph);
			Objects.Add(GraphLayoutRef, Graph);
			Objects.Add(TEXT("node:alpha"), Alpha);
			Objects.Add(TEXT("node:beta"), Beta);
		}

		~FOwnedGraphFixture()
		{
			GraphEditor.Reset();
			if (Package)
			{
				Package->SetDirtyFlag(false);
				Package->MarkAsGarbage();
			}
		}

		bool IsValid() const
		{
			return Package && Blueprint && Graph && Alpha && Beta
				&& GraphEditor.IsValid();
		}

		bool TryResolveObject(
			const FString& ExplicitRef,
			UObject*& OutObject,
			FUeAgentWorldSpatialExecutionError& OutError) override
		{
			OutObject = Objects.FindRef(ExplicitRef);
			if (!OutObject)
			{
				OutError.Code = TEXT("owned_graph_object_unresolved");
				OutError.SubjectId = ExplicitRef;
				return false;
			}
			return true;
		}

		bool TryResolveGraphEditor(
			const FString& AssetEditorIdentity,
			TSharedPtr<SGraphEditor>& OutGraphEditor,
			FUeAgentWorldSpatialExecutionError& OutError) override
		{
			if (AssetEditorIdentity != EditorIdentity
				|| !GraphEditor.IsValid())
			{
				OutGraphEditor.Reset();
				OutError.Code = TEXT("owned_graph_editor_identity_mismatch");
				OutError.SubjectId = AssetEditorIdentity;
				return false;
			}
			OutGraphEditor = GraphEditor;
			return true;
		}

		bool TryReadRevision(
			const FString& ExplicitRef,
			FWorldSpatialRevision& OutRevision,
			FUeAgentWorldSpatialExecutionError& OutError) override
		{
			OutRevision = {};
			OutError = {};
			if (ExplicitRef == GraphViewRef)
			{
				FVector2f Location = FVector2f::ZeroVector;
				float Zoom = 0.0f;
				GraphEditor->GetViewLocation(Location, Zoom);
				OutRevision = {
					TEXT("graph-view"),
					TEXT("gen-1"),
					Location.Equals(FVector2f(12.0f, 24.0f), 0.01f)
						&& FMath::IsNearlyEqual(Zoom, 0.75f)
						? 1
						: 2,
				};
				return true;
			}
			if (ExplicitRef == GraphLayoutRef)
			{
				OutRevision = {
					TEXT("graph-layout"),
					TEXT("gen-1"),
					Alpha->NodePosX == -10 && Alpha->NodePosY == -20
						? 2
						: 3,
				};
				return true;
			}
			OutError.Code = TEXT("owned_graph_revision_unresolved");
			OutError.SubjectId = ExplicitRef;
			return false;
		}

		static inline const FString GraphViewRef =
			TEXT("graph:/Game/UAI/Tests/WorldSpatial/"
				"BP_GraphViewFixture.EventGraph");
		static inline const FString EditorIdentity = GraphViewRef;
		static inline const FString GraphLayoutRef =
			TEXT("graph:/Game/UAI/Tests/WorldSpatial/"
				"BP_GraphLayoutFixture.EventGraph");

		UPackage* Package = nullptr;
		UBlueprint* Blueprint = nullptr;
		UEdGraph* Graph = nullptr;
		UEdGraphNode* Alpha = nullptr;
		UEdGraphNode* Beta = nullptr;
		TSharedPtr<SGraphEditor> GraphEditor;
		TMap<FString, UObject*> Objects;
	};
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUeAgentWorldSpatialProductionGraphViewC3Test,
	"UeAgentInterface.DomainAdapters.WorldSpatial.E2.Production."
		"GraphViewC3",
	EAutomationTestFlags::EditorContext
		| EAutomationTestFlags::EngineFilter)

bool FUeAgentWorldSpatialProductionGraphViewC3Test::RunTest(
	const FString& Parameters)
{
	using namespace UE::UAI::Tests::WorldSpatial;
	FOwnedGraphFixture Fixture;
	UWorld* ExplicitWorld = GEditor
		? GEditor->GetEditorWorldContext().World()
		: nullptr;
	if (!TestTrue(TEXT("production graph fixture is valid"), Fixture.IsValid())
		|| !TestNotNull(TEXT("production graph world exists"), ExplicitWorld))
	{
		return false;
	}
	TSharedPtr<UE::UAI::DomainAdapters::WorldSpatial::
		FUeAgentWorldSpatialProductionRuntime> Runtime;
	if (!TryCreateProductionRuntimeForTest(*this, *ExplicitWorld, Runtime)
		|| !Runtime.IsValid())
	{
		return false;
	}
	FProductionScenarioBindings Bindings;
	Bindings.ExplicitRefs.Add(FOwnedGraphFixture::GraphViewRef,
		Fixture.Graph->GetPathName());
	const FOwnedScenarioContext Context{
		ExplicitWorld->GetPathName(),
		Fixture.Graph->GetPathName(),
		FString(),
	};
	return RunProductionScenarioThroughC0C3(
		*this,
		TEXT("graph_view_owned_blueprint_editor"),
		Runtime.ToSharedRef(),
		Context,
		{},
		[&](FAutomationTestBase& Test, const FString& CapabilityId,
			const TSharedPtr<FJsonObject>& RawInput)
		{
			return PrepareProductionCapabilityInput(
				Test, CapabilityId, RawInput, *Runtime, Bindings);
		},
		[&Fixture](FAutomationTestBase& Test, const FString& CapabilityId)
		{
			if (CapabilityId == TEXT("graph.view.update"))
			{
				FVector2f Location = FVector2f::ZeroVector;
				float Zoom = 0.0f;
				Fixture.GraphEditor->GetViewLocation(Location, Zoom);
				Test.TestTrue(TEXT("production C3 writes the exact graph view"),
					Location.Equals(FVector2f(320.0f, 180.0f), 0.5f)
						&& FMath::IsNearlyEqual(Zoom, 1.25f, 0.01f));
			}
		});
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUeAgentWorldSpatialProductionGraphLayoutC3Test,
	"UeAgentInterface.DomainAdapters.WorldSpatial.E2.Production."
		"GraphLayoutC3",
	EAutomationTestFlags::EditorContext
		| EAutomationTestFlags::EngineFilter)

bool FUeAgentWorldSpatialProductionGraphLayoutC3Test::RunTest(
	const FString& Parameters)
{
	using namespace UE::UAI::Tests::WorldSpatial;
	FOwnedGraphFixture Fixture;
	UWorld* ExplicitWorld = GEditor
		? GEditor->GetEditorWorldContext().World()
		: nullptr;
	if (!TestTrue(TEXT("production layout fixture is valid"), Fixture.IsValid())
		|| !TestNotNull(TEXT("production layout world exists"), ExplicitWorld))
	{
		return false;
	}
	TSharedPtr<UE::UAI::DomainAdapters::WorldSpatial::
		FUeAgentWorldSpatialProductionRuntime> Runtime;
	if (!TryCreateProductionRuntimeForTest(*this, *ExplicitWorld, Runtime)
		|| !Runtime.IsValid())
	{
		return false;
	}
	FProductionScenarioBindings Bindings;
	Bindings.ExplicitRefs = {
		{FOwnedGraphFixture::GraphLayoutRef, Fixture.Graph->GetPathName()},
		{TEXT("node:alpha"), Fixture.Alpha->GetPathName()},
		{TEXT("node:beta"), Fixture.Beta->GetPathName()},
	};
	const FOwnedScenarioContext Context{
		ExplicitWorld->GetPathName(),
		Fixture.Graph->GetPathName(),
		FString(),
	};
	return RunProductionScenarioThroughC0C3(
		*this,
		TEXT("graph_layout_owned_nodes"),
		Runtime.ToSharedRef(),
		Context,
		{},
		[&](FAutomationTestBase& Test, const FString& CapabilityId,
			const TSharedPtr<FJsonObject>& RawInput)
		{
			return PrepareProductionCapabilityInput(
				Test, CapabilityId, RawInput, *Runtime, Bindings);
		},
		[&Fixture](FAutomationTestBase& Test, const FString& CapabilityId)
		{
			if (CapabilityId == TEXT("graph.layout.apply"))
			{
				Test.TestTrue(TEXT("production C3 writes exact graph nodes"),
					Fixture.Alpha->NodePosX == 100
						&& Fixture.Alpha->NodePosY == 200
						&& Fixture.Beta->NodePosX == 500
						&& Fixture.Beta->NodePosY == 200);
			}
		});
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUeAgentWorldSpatialGraphViewOwnedBlueprintEditorTest,
	"UeAgentInterface.DomainAdapters.WorldSpatial.E2.Native."
		"GraphViewOwnedBlueprintEditor",
	EAutomationTestFlags::EditorContext
		| EAutomationTestFlags::EngineFilter)

bool FUeAgentWorldSpatialGraphViewOwnedBlueprintEditorTest::RunTest(
	const FString& Parameters)
{
	using namespace UE::UAI::Tests::WorldSpatial;
	if (!RequireNativeExecution(*this, TEXT("graph_view_owned_blueprint_editor")))
	{
		return false;
	}
	FOwnedGraphFixture Fixture;
	if (!TestTrue(TEXT("owned Blueprint graph view is valid"), Fixture.IsValid()))
	{
		return false;
	}
	const FOwnedScenarioContext Context{
		TEXT("owned-world:none"),
		FOwnedGraphFixture::EditorIdentity,
		FString(),
	};
	const bool bResult = RunOwnedScenarioThroughC0C3(
		*this,
		TEXT("graph_view_owned_blueprint_editor"),
		MakeShareable(&Fixture, [](FOwnedGraphFixture*) {}),
		Context,
		{},
		[&Fixture](FAutomationTestBase& Test, const FString& CapabilityId)
		{
			if (CapabilityId != TEXT("graph.view.update"))
			{
				return;
			}
			FVector2f Location = FVector2f::ZeroVector;
			float Zoom = 0.0f;
			Fixture.GraphEditor->GetViewLocation(Location, Zoom);
			Test.TestTrue(
				TEXT("same graph editor reads the authored location"),
				Location.Equals(FVector2f(320.0f, 180.0f), 0.5f));
			Test.TestTrue(
				TEXT("same graph editor reads the authored zoom"),
				FMath::IsNearlyEqual(Zoom, 1.25f, 0.01f));
		});
	TestFalse(TEXT("owned graph package remains clean"), Fixture.Package->IsDirty());
	return bResult;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUeAgentWorldSpatialGraphLayoutOwnedNodesTest,
	"UeAgentInterface.DomainAdapters.WorldSpatial.E2.Native."
		"GraphLayoutOwnedNodes",
	EAutomationTestFlags::EditorContext
		| EAutomationTestFlags::EngineFilter)

bool FUeAgentWorldSpatialGraphLayoutOwnedNodesTest::RunTest(
	const FString& Parameters)
{
	using namespace UE::UAI::Tests::WorldSpatial;
	if (!RequireNativeExecution(*this, TEXT("graph_layout_owned_nodes")))
	{
		return false;
	}
	FOwnedGraphFixture Fixture;
	if (!TestTrue(TEXT("owned Blueprint nodes are valid"), Fixture.IsValid()))
	{
		return false;
	}
	const FOwnedScenarioContext Context{
		TEXT("owned-world:none"),
		FOwnedGraphFixture::EditorIdentity,
		FString(),
	};
	const bool bResult = RunOwnedScenarioThroughC0C3(
		*this,
		TEXT("graph_layout_owned_nodes"),
		MakeShareable(&Fixture, [](FOwnedGraphFixture*) {}),
		Context,
		{},
		[&Fixture](FAutomationTestBase& Test, const FString& CapabilityId)
		{
			if (CapabilityId == TEXT("graph.layout.apply"))
			{
				Test.TestEqual(TEXT("alpha X readback"), Fixture.Alpha->NodePosX, 100);
				Test.TestEqual(TEXT("alpha Y readback"), Fixture.Alpha->NodePosY, 200);
				Test.TestEqual(TEXT("beta X readback"), Fixture.Beta->NodePosX, 500);
				Test.TestEqual(TEXT("beta Y readback"), Fixture.Beta->NodePosY, 200);
			}
		});
	TestFalse(TEXT("owned layout package remains clean"), Fixture.Package->IsDirty());
	return bResult;
}
