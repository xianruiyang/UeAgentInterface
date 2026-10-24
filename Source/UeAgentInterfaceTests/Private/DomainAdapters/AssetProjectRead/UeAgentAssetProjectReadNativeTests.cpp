#include "Misc/AutomationTest.h"

// RE-005 production must satisfy this RE-004 frozen native boundary.
#include "DomainAdapters/AssetProjectRead/UeAgentAssetProjectReadNative.h"
#include "DomainAdapters/AssetProjectRead/UeAgentAssetProjectReadDescriptors.h"


#include "AssetRegistry/AssetData.h"
#include "Core/ContextResource/UeAgentOperationContext.h"
#include "Core/Contract/UeAgentContract.h"
#include "Core/Discovery/UeAgentDiscovery.h"
#include "Core/StateSemantic/UeAgentCanonicalValueJson.h"
#include "Core/StateSemantic/UeAgentStateSemantic.h"
#include "Curves/CurveFloat.h"
#include "EdGraph/EdGraph.h"
#include "EdGraph/EdGraphNode.h"
#include "Engine/DataTable.h"
#include "Engine/StaticMesh.h"
#include "HAL/FileManager.h"
#include "MeshDescription.h"
#include "Misc/ConfigCacheIni.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "StaticMeshAttributes.h"
#include "UObject/Package.h"
#include "Utilities/AuthoringCore/UeAgentAuthoringIdentity.h"

namespace
{
	using namespace UE::UAI::ContextResource;
	using namespace UE::UAI::Contract;
	using namespace UE::UAI::Discovery;
	using namespace UE::UAI::DomainAdapters::AssetProjectRead;
	using namespace UE::UAI::StateSemantic;

	class FOwnedAssetProjectRead final
	{
	public:
		FOwnedAssetProjectRead()
		{
			const FString PackageName = FString::Printf(
				TEXT("/Temp/UAI/AssetProjectRead/%s"),
				*FGuid::NewGuid().ToString(EGuidFormats::Digits));
			Package = CreatePackage(*PackageName);
			Asset = NewObject<UCurveFloat>(
				Package,
				TEXT("OwnedAsset"),
				RF_Public | RF_Standalone);
			ProjectObject = NewObject<UCurveFloat>(
				Package,
				TEXT("OwnedProjectSettings"),
				RF_Transient);
			Graph = NewObject<UEdGraph>(
				Package,
				TEXT("OwnedGraph"),
				RF_Transient);
			GraphNode = NewObject<UEdGraphNode>(
				Graph,
				TEXT("OwnedGraphNode"),
				RF_Transient);
			if (Graph && GraphNode)
			{
				GraphNode->CreateNewGuid();
				Graph->AddNode(GraphNode, false, false);
			}
			Asset->FloatCurve.AddKey(0.0f, 37.0f);
			ProjectObject->FloatCurve.AddKey(0.0f, 91.0f);

			Table = NewObject<UDataTable>(
				Package,
				TEXT("OwnedTable"),
				RF_Public | RF_Standalone);
			if (Table)
			{
				Table->RowStruct = FTableRowBase::StaticStruct();
				Table->AddRow(TEXT("Alpha"), FTableRowBase());
			}

			Mesh = NewObject<UStaticMesh>(
				Package,
				TEXT("OwnedMesh"),
				RF_Public | RF_Standalone);
			if (Mesh)
			{
				FMeshDescription Description;
				FStaticMeshAttributes Attributes(Description);
				Attributes.Register();
				TVertexAttributesRef<FVector3f> Positions =
					Attributes.GetVertexPositions();
				const FVertexID V0 = Description.CreateVertex();
				const FVertexID V1 = Description.CreateVertex();
				const FVertexID V2 = Description.CreateVertex();
				Positions[V0] = FVector3f(-10.0f, -20.0f, 0.0f);
				Positions[V1] = FVector3f(30.0f, -20.0f, 0.0f);
				Positions[V2] = FVector3f(-10.0f, 40.0f, 50.0f);
				const FVertexInstanceID I0 =
					Description.CreateVertexInstance(V0);
				const FVertexInstanceID I1 =
					Description.CreateVertexInstance(V1);
				const FVertexInstanceID I2 =
					Description.CreateVertexInstance(V2);
				const FPolygonGroupID Group =
					Description.CreatePolygonGroup();
				Description.CreatePolygon(Group, {I0, I1, I2});
				Mesh->BuildFromMeshDescriptions({&Description});
			}

			DirtyPackage = CreatePackage(*(PackageName + TEXT("_Dirty")));
			DirtyAsset = NewObject<UCurveFloat>(
				DirtyPackage,
				TEXT("OwnedDirtyAsset"),
				RF_Public | RF_Standalone);
			MapPackage = CreatePackage(*(PackageName + TEXT("_Map")));
			MapPackage->SetPackageFlags(PKG_ContainsMap);
			RegistryRecord = FAssetData(Asset);
			Config.SetString(
				TEXT("UAI.AssetProjectRead"),
				TEXT("KnownValue"),
				TEXT("fixture-value"));
			Package->SetDirtyFlag(false);
			DirtyPackage->SetDirtyFlag(true);
			MapPackage->SetDirtyFlag(true);
		}

		~FOwnedAssetProjectRead()
		{
			if (DirtyPackage)
			{
				DirtyPackage->SetDirtyFlag(false);
				DirtyPackage->MarkAsGarbage();
			}
			if (MapPackage)
			{
				MapPackage->SetDirtyFlag(false);
				MapPackage->MarkAsGarbage();
			}
			if (Package)
			{
				Package->SetDirtyFlag(false);
				Package->MarkAsGarbage();
			}
		}

		UPackage* Package = nullptr;
		UCurveFloat* Asset = nullptr;
		UCurveFloat* ProjectObject = nullptr;
		UDataTable* Table = nullptr;
		UStaticMesh* Mesh = nullptr;
		UPackage* DirtyPackage = nullptr;
		UCurveFloat* DirtyAsset = nullptr;
		UPackage* MapPackage = nullptr;
		UEdGraph* Graph = nullptr;
		UEdGraphNode* GraphNode = nullptr;
		FAssetData RegistryRecord;
		FConfigFile Config;
	};

	FUeAgentDescriptorIdentity Identity()
	{
		TSharedPtr<const FUeAgentDescriptorRegistry> Registry;
		FString Error;
		if (TryCreateAssetProjectReadDescriptorRegistry(Registry, Error)
			&& Registry.IsValid())
		{
			const FUeAgentOperationDescriptor* Descriptor =
				Registry->FindByDescriptorId(TEXT("uai.query.asset.summary"));
			if (Descriptor)
			{
				return Descriptor->GetIdentity();
			}
		}
		return FUeAgentDescriptorIdentity(FString(), 0, FString());
	}

	FUeAgentResourceTrackerOwner Resources()
	{
		return MakeShared<FUeAgentResourceTracker, ESPMode::ThreadSafe>();
	}

	TSharedPtr<FUeAgentOperationContext> Context(
		FAutomationTestBase* Test,
		const FUeAgentResourceTrackerOwner& ResourceTracker)
	{
		FUeAgentExecutionContextInput Input;
		Input.ProjectInstanceId = TEXT("owned-project");
		Input.EditorInstanceId = TEXT("owned-editor");
		Input.StatePlane = EUeAgentStatePlane::AuthoringMemory;
		Input.WorldType = EUeAgentExecutionWorldType::Editor;
		Input.ContextRevision = TEXT("owned-project-revision");
		const FUeAgentExecutionContextPolicy Policy(
			{EUeAgentStatePlane::AuthoringMemory},
			{},
			EUeAgentContextResolutionPolicy::ResolveCurrentThenFreeze,
			EUeAgentTransientContextPolicy::ReadOnly);
		FUeAgentExecutionContextRefPtr ExecutionContext;
		FUeAgentExecutionContextError ContextError;
		Test->TestTrue(
			TEXT("freeze owned asset/project context"),
			FUeAgentExecutionContextRef::TryCreate(
				Input,
				Identity(),
				EUeAgentRequestKind::Query,
				Policy,
				ExecutionContext,
				ContextError));
		if (!ExecutionContext.IsValid())
		{
			return nullptr;
		}
		const FUeAgentQueryRequest Request(
			Identity(),
			TEXT("request-asset-project-read-native"),
			FUeAgentContractValue(TEXT("{}")));
		const FUeAgentCallerContext Caller(
			TEXT("fixture-agent"),
			{TEXT("asset_project:read")},
			TEXT("corr-asset-project-read-native"));
		const FUeAgentOperationDeadline Deadline =
			FUeAgentOperationDeadline::FromTimeout(
				30.0,
				[]() { return 100.0; });
		const FUeAgentCancellationSource Cancellation(
			[]() { return 100.0; });
		TSharedPtr<FUeAgentOperationContext> Result;
		FUeAgentOperationContextError Error;
		Test->TestTrue(
			TEXT("create native operation context"),
			FUeAgentOperationContext::TryCreate(
				Request,
				Caller,
				ExecutionContext.ToSharedRef(),
				ResourceTracker,
				Deadline,
				Cancellation.GetToken(),
				Result,
				Error));
		return Result;
	}

	FUeAgentCandidateBinding Target(
		const FString& BindingId,
		const FString& PublicRef)
	{
		return FUeAgentCandidateBinding(
			BindingId,
			FUeAgentCandidateSummary(
				FUeAgentCanonicalIdentity(
					TEXT("asset.target:") + BindingId,
					TEXT("asset"),
					TEXT("owned-project"),
					BindingId,
					TEXT("target"),
					TEXT("owned-project-revision")),
				Identity(),
				TEXT("asset"),
				BindingId,
				TEXT("owned AssetProjectRead target"),
				{TEXT("AssetProjectRead"), TEXT("Fixture")}),
			FUeAgentCatalogRevision(
				TEXT("owned-asset-catalog"),
				TEXT("owned-project-revision"),
				1),
			PublicRef,
			TEXT("asset-project-read"),
			{EUeAgentCandidateOperation::Read},
			TEXT("asset-project-read-native-test"));
	}

	class FOwnedEnvironment final
		: public IUeAgentAssetProjectReadEnvironment
	{
	public:
		explicit FOwnedEnvironment(FOwnedAssetProjectRead& InOwner)
			: Owner(InOwner)
		{
		}

		bool TryResolveAsset(
			const FUeAgentCandidateBinding& InTarget,
			UObject*& OutAsset,
			FUeAgentAssetProjectReadError& OutError) override
		{
			++AssetCalls;
			if (InTarget.GetBindingId() == TEXT("owned-asset"))
			{
				OutAsset = Owner.Asset;
				return true;
			}
			if (InTarget.GetBindingId() == TEXT("owned-table"))
			{
				OutAsset = Owner.Table;
				return true;
			}
			if (InTarget.GetBindingId() == TEXT("owned-mesh"))
			{
				OutAsset = Owner.Mesh;
				return true;
			}
			if (InTarget.GetBindingId() == TEXT("owned-graph"))
			{
				OutAsset = Owner.Graph;
				return true;
			}
			OutAsset = nullptr;
			OutError.Code = TEXT("asset_project_read_asset_unresolved");
			OutError.SubjectId = InTarget.GetBindingId();
			return false;
		}

		bool TryResolveRegistryRecord(
			const FUeAgentCandidateBinding& InTarget,
			FAssetData& OutRecord,
			FUeAgentAssetProjectReadError& OutError) override
		{
			++RegistryCalls;
			OutRecord = Owner.RegistryRecord;
			return InTarget.GetBindingId() == TEXT("owned-asset");
		}

		bool TryResolveRegistryRelations(
			const FAssetData& Record,
			const FUeAgentAssetRelationGraphRequest& Request,
			FUeAgentAssetRelationGraphResult& OutGraph,
			FUeAgentAssetProjectReadError& OutError) override
		{
			++RelationCalls;
			if (!Record.IsValid()
				|| Request.Direction != EUeAgentAssetRelationDirection::Both
				|| !Request.RelationKinds.Contains(
					EUeAgentAssetRelationKind::Hard))
			{
				OutError.Code = TEXT("fixture_relation_input_mismatch");
				return false;
			}
			OutGraph = {};
			OutGraph.bComplete = true;
			OutGraph.Root.StableIdentifier = Record.PackageName.ToString();
			OutGraph.Root.PackageName = Record.PackageName.ToString();
			OutGraph.Root.ObjectPath = Record.GetObjectPathString();
			OutGraph.Root.bResolved = true;
			FUeAgentAssetRelationNode Dependency;
			Dependency.StableIdentifier = TEXT("/Game/Dependency");
			Dependency.PackageName = TEXT("/Game/Dependency");
			Dependency.bResolved = true;
			FUeAgentAssetRelationNode Referencer;
			Referencer.StableIdentifier = TEXT("/Game/Referencer");
			Referencer.PackageName = TEXT("/Game/Referencer");
			Referencer.bResolved = true;
			OutGraph.Nodes = {OutGraph.Root, Dependency, Referencer};
			FUeAgentAssetRelationEdge Outgoing;
			Outgoing.SourceIdentifier = OutGraph.Root.StableIdentifier;
			Outgoing.SourcePackageName = OutGraph.Root.PackageName;
			Outgoing.SourceObjectPath = OutGraph.Root.ObjectPath;
			Outgoing.TargetIdentifier = Dependency.StableIdentifier;
			Outgoing.TargetPackageName = Dependency.PackageName;
			Outgoing.TraversalDirection = TEXT("dependencies");
			Outgoing.Depth = 1;
			Outgoing.TraversalPath = {
				OutGraph.Root.StableIdentifier, Dependency.StableIdentifier};
			Outgoing.Category = TEXT("package");
			Outgoing.bHard = true;
			Outgoing.bGame = true;
			Outgoing.bSourceResolved = true;
			Outgoing.bTargetResolved = true;
			FUeAgentAssetRelationEdge Incoming;
			Incoming.SourceIdentifier = Referencer.StableIdentifier;
			Incoming.SourcePackageName = Referencer.PackageName;
			Incoming.TargetIdentifier = OutGraph.Root.StableIdentifier;
			Incoming.TargetPackageName = OutGraph.Root.PackageName;
			Incoming.TargetObjectPath = OutGraph.Root.ObjectPath;
			Incoming.TraversalDirection = TEXT("referencers");
			Incoming.Depth = 1;
			Incoming.TraversalPath = {
				OutGraph.Root.StableIdentifier, Referencer.StableIdentifier};
			Incoming.Category = TEXT("package");
			Incoming.bHard = true;
			Incoming.bGame = true;
			Incoming.bSourceResolved = true;
			Incoming.bTargetResolved = true;
			OutGraph.Edges = {MoveTemp(Outgoing), MoveTemp(Incoming)};
			return true;
		}

		bool TryResolveOpenAssets(
			const FUeAgentExecutionContextRef& InContext,
			const bool bIncludeEditorIdentity,
			TArray<FUeAgentEditorResourceObservation>& OutResources,
			FUeAgentAssetProjectReadError& OutError) override
		{
			++EditorCalls;
			FUeAgentEditorResourceObservation Observation;
			Observation.ResourceId = Owner.Package->GetName();
			Observation.ResourceKind = TEXT("asset");
			Observation.PackageName = Owner.Package->GetName();
			Observation.ObjectPath = Owner.Asset->GetPathName();
			Observation.ClassPath = Owner.Asset->GetClass()->GetPathName();
			Observation.EditorIdentity = bIncludeEditorIdentity
				? InContext.GetEditorInstanceId() : FString();
			Observation.bDirty = Owner.Package->IsDirty();
			OutResources = {MoveTemp(Observation)};
			return InContext.GetProjectInstanceId() == TEXT("owned-project");
		}

		bool TryResolveDirtyResources(
			const FUeAgentExecutionContextRef& InContext,
			const TArray<FString>& ResourceKinds,
			TArray<FUeAgentEditorResourceObservation>& OutResources,
			FUeAgentAssetProjectReadError& OutError) override
		{
			++EditorCalls;
			OutResources.Reset();
			auto Add = [&](UPackage* Package, const FString& Kind)
			{
				if (!ResourceKinds.IsEmpty() && !ResourceKinds.Contains(Kind))
				{
					return;
				}
				FUeAgentEditorResourceObservation Observation;
				Observation.ResourceId = Package->GetName();
				Observation.ResourceKind = Kind;
				Observation.PackageName = Package->GetName();
				Observation.bDirty = Package->IsDirty();
				OutResources.Add(MoveTemp(Observation));
			};
			Add(Owner.DirtyPackage, TEXT("asset"));
			Add(Owner.MapPackage, TEXT("map"));
			return InContext.GetProjectInstanceId() == TEXT("owned-project");
		}

		bool TryResolveProjectPlatforms(
			const FUeAgentExecutionContextRef& InContext,
			const TArray<FString>& PlatformIds,
			TArray<FUeAgentProjectPlatformObservation>& OutPlatforms,
			FUeAgentAssetProjectReadError& OutError) override
		{
			++PlatformCalls;
			if (!PlatformIds.IsEmpty()
				&& !PlatformIds.Contains(TEXT("Windows")))
			{
				OutError.Code = TEXT("asset_project_read_platform_unresolved");
				return false;
			}
			FUeAgentProjectPlatformObservation Observation;
			Observation.PlatformName = TEXT("Windows");
			Observation.IniPlatformName = TEXT("Windows");
			Observation.DisplayName = TEXT("Windows");
			Observation.bRunningPlatform = true;
			Observation.bSdkInstalled = true;
			OutPlatforms = {MoveTemp(Observation)};
			return InContext.GetProjectInstanceId() == TEXT("owned-project");
		}

		bool TryResolveProjectObject(
			const FUeAgentExecutionContextRef& InContext,
			const FString& SourceId,
			UObject*& OutObject,
			FUeAgentAssetProjectReadError& OutError) override
		{
			++ProjectCalls;
			OutObject = Owner.ProjectObject;
			return InContext.GetProjectInstanceId() == TEXT("owned-project")
				&& SourceId == TEXT("project");
		}

		bool TryResolveProjectSettingsObject(
			const FUeAgentExecutionContextRef& InContext,
			const FString& SectionId,
			UObject*& OutObject,
			FUeAgentAssetProjectReadError& OutError) override
		{
			++ProjectCalls;
			OutObject = Owner.ProjectObject;
			return InContext.GetProjectInstanceId() == TEXT("owned-project")
				&& SectionId == TEXT("project");
		}

		bool TryResolveConfig(
			const FUeAgentExecutionContextRef& InContext,
			const FString& ConfigScope,
			const FConfigFile*& OutConfig,
			FUeAgentAssetProjectReadError& OutError) override
		{
			++ConfigCalls;
			OutConfig = &Owner.Config;
			return InContext.GetProjectInstanceId() == TEXT("owned-project")
				&& ConfigScope == TEXT("game");
		}

		int32 AssetCalls = 0;
		int32 RegistryCalls = 0;
		int32 RelationCalls = 0;
		int32 EditorCalls = 0;
		int32 PlatformCalls = 0;
		int32 ProjectCalls = 0;
		int32 ConfigCalls = 0;

	private:
		FOwnedAssetProjectRead& Owner;
	};

	bool Capture(
		FAutomationTestBase* Test,
		const FString& Binding,
		const FString& Input,
		const TOptional<FUeAgentCandidateBinding>& InTarget,
		const TSharedRef<IUeAgentAssetProjectReadEnvironment>& Environment,
		const FUeAgentOperationContext& OperationContext,
		TSharedPtr<const FUeAgentCanonicalSnapshot>& OutSnapshot,
		FUeAgentAssetProjectReadError& OutError)
	{
		TSharedPtr<IUeAgentAssetProjectReadHook> Hook;
		if (!Test->TestTrue(
				*Binding,
				FUeAgentAssetProjectReadNativeHookFactory::TryCreate(
					Binding,
					Environment,
					Hook,
					OutError)))
		{
			return false;
		}
		return Hook.IsValid()
			&& Hook->TryCapture(
				FUeAgentContractValue(Input),
				InTarget,
				OperationContext,
				OutSnapshot,
				OutError);
	}

	bool HasOwner(
		const TSharedPtr<const FUeAgentCanonicalSnapshot>& Snapshot,
		const FString& OwnerId)
	{
		return Snapshot.IsValid()
			&& Snapshot->GetEntries().ContainsByPredicate(
				[&OwnerId](const FUeAgentStateEntry& Entry)
				{
					return Entry.GetIdentity().GetOwnerId().Contains(OwnerId);
				});
	}

	bool HasSelector(
		const TSharedPtr<const FUeAgentCanonicalSnapshot>& Snapshot,
		const FString& Selector)
	{
		return Snapshot.IsValid()
			&& Snapshot->GetEntries().ContainsByPredicate(
				[&Selector](const FUeAgentStateEntry& Entry)
				{
					return Entry.GetIdentity().GetSelector() == Selector;
				});
	}

	TSharedPtr<FJsonObject> FindObjectSelector(
		const TSharedPtr<const FUeAgentCanonicalSnapshot>& Snapshot,
		const FString& Selector)
	{
		if (!Snapshot.IsValid())
		{
			return nullptr;
		}
		const FUeAgentStateEntry* Entry =
			Snapshot->GetEntries().FindByPredicate(
				[&Selector](const FUeAgentStateEntry& Candidate)
				{
					return Candidate.GetIdentity().GetSelector() == Selector;
				});
		if (!Entry)
		{
			return nullptr;
		}
		TSharedPtr<FJsonValue> Json;
		FString Error;
		return CanonicalJson::TryToJsonValue(*Entry->GetValue(), Json, Error)
			&& Json.IsValid()
			? Json->AsObject()
			: nullptr;
	}
}


IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUeAgentAssetProjectReadNativeOwnedFactsTest,
	"UeAgentInterface.DomainAdapters.AssetProjectRead.E2.Native."
		"SixFamiliesOwnedFacts",
	EAutomationTestFlags::EditorContext
		| EAutomationTestFlags::EngineFilter)

bool FUeAgentAssetProjectReadNativeOwnedFactsTest::RunTest(
	const FString& Parameters)
{
	FOwnedAssetProjectRead Owner;
	if (!TestNotNull(TEXT("owned asset"), Owner.Asset)
		|| !TestNotNull(TEXT("owned package"), Owner.Package)
		|| !TestNotNull(TEXT("owned graph"), Owner.Graph)
		|| !TestNotNull(TEXT("owned graph node"), Owner.GraphNode))
	{
		return false;
	}
	const float OriginalAssetValue =
		Owner.Asset->FloatCurve.Eval(0.0f);
	const float OriginalProjectValue =
		Owner.ProjectObject->FloatCurve.Eval(0.0f);
	FString OriginalConfigValue;
	Owner.Config.GetString(
		TEXT("UAI.AssetProjectRead"),
		TEXT("KnownValue"),
		OriginalConfigValue);
	const FUeAgentResourceTrackerOwner ResourceTracker =
		Resources();
	const TSharedPtr<FUeAgentOperationContext> OperationContext =
		Context(this, ResourceTracker);
	if (!OperationContext.IsValid())
	{
		return false;
	}
	const TSharedRef<FOwnedEnvironment> Environment =
		MakeShared<FOwnedEnvironment>(Owner);
	const FUeAgentCandidateBinding AssetTarget =
		Target(TEXT("owned-asset"), Owner.Asset->GetPathName());
	TSharedPtr<const FUeAgentCanonicalSnapshot> Snapshot;
	FUeAgentAssetProjectReadError Error;

	TestTrue(
		TEXT("asset family reads Ticket-resolved UObject"),
		Capture(
			this,
			TEXT("asset_project.read.asset_summary"),
			TEXT("{}"),
			AssetTarget,
			Environment,
			*OperationContext,
			Snapshot,
			Error));
	TestTrue(TEXT("asset snapshot owns UObject"), HasOwner(Snapshot, Owner.Asset->GetPathName()));

	Snapshot.Reset();
	TestTrue(
		TEXT("registry family reads explicit FAssetData"),
		Capture(
			this,
			TEXT("asset_project.read.registry_record"),
			TEXT("{}"),
			AssetTarget,
			Environment,
			*OperationContext,
			Snapshot,
			Error));
	TestTrue(TEXT("registry snapshot owns asset"), HasOwner(Snapshot, Owner.Asset->GetPathName()));

	Snapshot.Reset();
	TestTrue(
		TEXT("asset relation family projects one typed graph entry"),
		Capture(
			this,
			TEXT("asset_project.read.asset_relations"),
			TEXT("{\"direction\":\"both\",\"relation_kinds\":[\"hard\"],\"package_scope\":\"game\"}"),
			AssetTarget,
			Environment,
			*OperationContext,
			Snapshot,
			Error));
	TestTrue(
		TEXT("normal relation projection exposes the structured graph selector"),
		HasSelector(Snapshot, TEXT("relation_graph")));
	const TSharedPtr<FJsonObject> RelationGraph =
		FindObjectSelector(Snapshot, TEXT("relation_graph"));
	TestTrue(
		TEXT("relation graph publishes a normalized business object"),
		RelationGraph.IsValid());
	if (RelationGraph.IsValid())
	{
		TestFalse(
			TEXT("relation graph omits the request echo"),
			RelationGraph->HasField(TEXT("query")));
		TestFalse(
			TEXT("relation graph uses coverage instead of the legacy completeness envelope"),
			RelationGraph->HasField(TEXT("completeness")));
		TestEqual(
			TEXT("relation root is the exact reusable asset reference"),
			RelationGraph->GetStringField(TEXT("root")),
			Owner.Asset->GetPathName());

		const TArray<TSharedPtr<FJsonValue>>* Nodes = nullptr;
		TestTrue(
			TEXT("relation graph exposes normalized nodes"),
			RelationGraph->TryGetArrayField(TEXT("nodes"), Nodes)
				&& Nodes && Nodes->Num() == 3);
		if (Nodes)
		{
			for (const TSharedPtr<FJsonValue>& NodeValue : *Nodes)
			{
				const TSharedPtr<FJsonObject> Node =
					NodeValue.IsValid() ? NodeValue->AsObject() : nullptr;
				TestTrue(
					TEXT("relation node has an exact reusable reference"),
					Node.IsValid()
						&& Node->GetStringField(TEXT("ref")).StartsWith(TEXT("/")));
				TestTrue(
					TEXT("relation node preserves package or object identity"),
					Node.IsValid()
						&& (Node->HasField(TEXT("package_name"))
							|| Node->HasField(TEXT("object_path"))));
				TestFalse(
					TEXT("relation node omits aliases and internal identifiers"),
					Node.IsValid()
						&& (Node->HasField(TEXT("key"))
							|| Node->HasField(TEXT("id"))
							|| Node->HasField(TEXT("package"))));
				TestFalse(
					TEXT("false redirector state is not repeated"),
					Node.IsValid() && Node->HasField(TEXT("redirector")));
			}
		}

		const TArray<TSharedPtr<FJsonValue>>* Edges = nullptr;
		TestTrue(
			TEXT("relation graph exposes normalized edges"),
			RelationGraph->TryGetArrayField(TEXT("edges"), Edges)
				&& Edges && Edges->Num() == 2);
		if (Edges)
		{
			for (const TSharedPtr<FJsonValue>& EdgeValue : *Edges)
			{
				const TSharedPtr<FJsonObject> Edge =
					EdgeValue.IsValid() ? EdgeValue->AsObject() : nullptr;
				TestTrue(
					TEXT("relation edge uses exact source and target references"),
					Edge.IsValid()
						&& Edge->GetStringField(TEXT("source")).StartsWith(TEXT("/"))
						&& Edge->GetStringField(TEXT("target")).StartsWith(TEXT("/")));
				TestTrue(
					TEXT("relation edge preserves interpreted business semantics"),
					Edge.IsValid()
						&& Edge->HasField(TEXT("direction"))
						&& Edge->HasField(TEXT("depth"))
						&& Edge->HasField(TEXT("relation"))
						&& Edge->HasField(TEXT("properties"))
						&& Edge->HasField(TEXT("path")));
				TestFalse(
					TEXT("relation edge omits the legacy traversal label"),
					Edge.IsValid() && Edge->HasField(TEXT("traversal")));
				TestFalse(
					TEXT("false cycle state is not repeated"),
					Edge.IsValid() && Edge->HasField(TEXT("cycle")));
				const TArray<TSharedPtr<FJsonValue>>* Path = nullptr;
				TestTrue(
					TEXT("relation path contains only reusable UE references"),
					Edge.IsValid()
						&& Edge->TryGetArrayField(TEXT("path"), Path)
						&& Path
						&& !Path->IsEmpty()
						&& !Path->ContainsByPredicate(
							[](const TSharedPtr<FJsonValue>& Value)
							{
								return !Value.IsValid()
									|| Value->Type != EJson::String
									|| !Value->AsString().StartsWith(TEXT("/"));
							}));
			}
		}

		const TSharedPtr<FJsonObject> Coverage =
			RelationGraph->GetObjectField(TEXT("coverage"));
		TestTrue(
			TEXT("relation coverage keeps the result scope explicit"),
			Coverage.IsValid()
				&& Coverage->GetBoolField(TEXT("complete"))
				&& !Coverage->GetBoolField(TEXT("registry_loading"))
				&& Coverage->GetStringField(TEXT("scope"))
					== TEXT("on_disk_asset_registry")
				&& !Coverage->GetBoolField(TEXT("runtime_references_included"))
				&& !Coverage->HasField(TEXT("source")));
	}

	Snapshot.Reset();
	TestTrue(
		TEXT("dormant registry relation compatibility projects depth-one strings from the same graph owner"),
		Capture(
			this,
			TEXT("asset_project.read.registry_relations"),
			TEXT("{\"direction\":\"both\",\"relation_kinds\":[\"hard\"]}"),
			AssetTarget,
			Environment,
			*OperationContext,
			Snapshot,
			Error));
	TestTrue(TEXT("relation snapshot contains dependency edge"), HasOwner(Snapshot, Owner.Asset->GetPathName()));

	Snapshot.Reset();
	TestTrue(
		TEXT("editor family reads explicit package collection"),
		Capture(
			this,
			TEXT("asset_project.read.editor_dirty_resources"),
			TEXT("{}"),
			{},
			Environment,
			*OperationContext,
			Snapshot,
			Error));
	TestTrue(TEXT("editor snapshot owns dirty package"), HasOwner(Snapshot, Owner.DirtyPackage->GetName()));

	Snapshot.Reset();
	TestTrue(
		TEXT("project family reads context-resolved project object"),
		Capture(
			this,
			TEXT("asset_project.read.project_summary"),
			TEXT("{}"),
			{},
			Environment,
			*OperationContext,
			Snapshot,
			Error));
	TestTrue(TEXT("project snapshot owns project object"), HasOwner(Snapshot, Owner.ProjectObject->GetPathName()));

	Snapshot.Reset();
	const bool bOriginResolved = TestTrue(
		TEXT("reference family resolves explicit UE object origin without mutation"),
		Capture(
			this,
			TEXT("asset_project.read.reference_origin"),
			FString::Printf(
				TEXT("{\"target\":{\"source\":\"ue_object\",\"full_name\":\"%s\"}}"),
				*Owner.Asset->GetPathName()),
			{},
			Environment,
			*OperationContext,
			Snapshot,
			Error));
	if (!bOriginResolved)
	{
		AddError(FString::Printf(
			TEXT("reference_origin_error:%s:%s:%s"),
			*Error.Code,
			*Error.SubjectId,
			*Error.Detail));
	}
	TestTrue(TEXT("reference origin snapshot is canonical"), HasOwner(Snapshot, TEXT("reference-origin:ue_object")));

	auto EscapeJson = [](FString Value)
	{
		Value.ReplaceInline(TEXT("\\"), TEXT("\\\\"));
		Value.ReplaceInline(TEXT("\""), TEXT("\\\""));
		return Value;
	};
	auto CaptureOrigin = [&](const FString& Source, const FString& TargetFields)
	{
		Snapshot.Reset();
		Error = {};
		const bool bCaptured = Capture(
			this,
			TEXT("asset_project.read.reference_origin"),
			FString::Printf(
				TEXT("{\"target\":{\"source\":\"%s\"%s}}"),
				*Source,
				*TargetFields),
			{},
			Environment,
			*OperationContext,
			Snapshot,
			Error);
		if (!bCaptured)
		{
			AddError(FString::Printf(
				TEXT("reference_origin_source_failed:%s:%s:%s:%s"),
				*Source,
				*Error.Code,
				*Error.SubjectId,
				*Error.Detail));
		}
		return bCaptured
			&& HasOwner(Snapshot, TEXT("reference-origin:") + Source);
	};

	TestTrue(
		TEXT("name_reference resolves one exact loaded object identity"),
		CaptureOrigin(
			TEXT("name_reference"),
			FString::Printf(
				TEXT(",\"kind\":\"curve\",\"name\":\"OwnedAsset\",\"full_name\":\"%s\""),
				*EscapeJson(Owner.Asset->GetPathName()))));

	TestTrue(
		TEXT("graph_node resolves one real graph node identity"),
		CaptureOrigin(
			TEXT("graph_node"),
			FString::Printf(
				TEXT(",\"kind\":\"graph_node\",\"name\":\"%s\",\"full_name\":\"%s\",\"graph_name\":\"%s\",\"node_guid\":\"%s\""),
				*EscapeJson(Owner.GraphNode->GetName()),
				*EscapeJson(Owner.GraphNode->GetPathName()),
				*EscapeJson(Owner.Graph->GetName()),
				*Owner.GraphNode->NodeGuid.ToString(EGuidFormats::DigitsWithHyphensLower))));

	const FString JsonReferencePath = FPaths::ConvertRelativePathToFull(
		FPaths::ProjectSavedDir()
		/ TEXT("UAI/T46/origin-reference-")
		+ FGuid::NewGuid().ToString(EGuidFormats::Digits)
		+ TEXT(".json"));
	IFileManager::Get().MakeDirectory(*FPaths::GetPath(JsonReferencePath), true);
	const FString JsonReferenceText = FString::Printf(
		TEXT("{\"target\":{\"kind\":\"curve\",\"name\":\"OwnedAsset\",\"full_name\":\"%s\"}}"),
		*EscapeJson(Owner.Asset->GetPathName()));
	TestTrue(
		TEXT("json_reference fixture is written"),
		FFileHelper::SaveStringToFile(JsonReferenceText, *JsonReferencePath));
	TestTrue(
		TEXT("json_reference resolves one exact JSON pointer identity"),
		CaptureOrigin(
			TEXT("json_reference"),
			FString::Printf(
				TEXT(",\"json_file\":\"%s\",\"json_pointer\":\"/target\""),
				*EscapeJson(JsonReferencePath))));
	IFileManager::Get().Delete(*JsonReferencePath, false, true, true);

	UeAgentAuthoringIdentity::FCanonicalIdentity CatalogIdentity;
	FString CatalogIdentityError;
	const bool bCatalogIdentityBuilt =
		UeAgentAuthoringIdentity::BuildCanonicalIdentity(
			TEXT("catalog_candidate_identity"),
			TEXT("placeable_actor"),
			{
				{TEXT("catalog_type"), TEXT("placeable_actor")},
				{TEXT("full_name"), TEXT("/Script/Engine.StaticMeshActor")},
			},
			CatalogIdentity,
			CatalogIdentityError);
	TestTrue(TEXT("catalog_candidate identity uses the registered model"),
		bCatalogIdentityBuilt && CatalogIdentity.IsValid());
	if (bCatalogIdentityBuilt && CatalogIdentity.IsValid())
	{
		TestTrue(
			TEXT("catalog_candidate resolves one exact catalog identity"),
			CaptureOrigin(
				TEXT("catalog_candidate"),
				FString::Printf(
					TEXT(",\"candidate_id\":\"%s\""),
					*EscapeJson(CatalogIdentity.CanonicalKey))));
	}

	Snapshot.Reset();
	Error = {};
	TestFalse(
		TEXT("reference family rejects a missing explicit source"),
		Capture(
			this,
			TEXT("asset_project.read.reference_origin"),
			TEXT("{\"target\":{\"full_name\":\"/Game/Missing\"}}"),
			{},
			Environment,
			*OperationContext,
			Snapshot,
			Error));
	TestEqual(TEXT("missing source remains typed"), Error.Code, FString(TEXT("reference_origin_target_invalid")));

	Snapshot.Reset();
	TestTrue(
		TEXT("config family reads explicit in-memory config"),
		Capture(
			this,
			TEXT("asset_project.read.config_section"),
			TEXT("{\"config_scope\":\"game\","
				"\"section_id\":\"UAI.AssetProjectRead\"}"),
			{},
			Environment,
			*OperationContext,
			Snapshot,
			Error));
	TestTrue(TEXT("config snapshot is real"), Snapshot.IsValid());

	FString FinalConfigValue;
	Owner.Config.GetString(
		TEXT("UAI.AssetProjectRead"),
		TEXT("KnownValue"),
		FinalConfigValue);
	TestFalse(TEXT("package remains clean"), Owner.Package->IsDirty());
	TestEqual(TEXT("asset property unchanged"), Owner.Asset->FloatCurve.Eval(0.0f), OriginalAssetValue);
	TestEqual(TEXT("project property unchanged"), Owner.ProjectObject->FloatCurve.Eval(0.0f), OriginalProjectValue);
	TestEqual(TEXT("config unchanged"), FinalConfigValue, OriginalConfigValue);
	TestEqual(TEXT("asset resolver called once"), Environment->AssetCalls, 1);
	TestEqual(TEXT("registry resolver called twice"), Environment->RegistryCalls, 2);
	TestEqual(TEXT("relation resolver called once"), Environment->RelationCalls, 1);
	TestEqual(TEXT("editor resolver called once"), Environment->EditorCalls, 1);
	TestEqual(TEXT("project resolver called once"), Environment->ProjectCalls, 1);
	TestEqual(TEXT("config resolver called once"), Environment->ConfigCalls, 1);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUeAgentAssetSamplePublicResultTest,
	"UeAgentInterface.DomainAdapters.AssetProjectRead.E2.Native."
		"AssetSamplePublicResult",
	EAutomationTestFlags::EditorContext
		| EAutomationTestFlags::EngineFilter)

bool FUeAgentAssetSamplePublicResultTest::RunTest(
	const FString& Parameters)
{
	FOwnedAssetProjectRead Owner;
	const FUeAgentResourceTrackerOwner ResourceTracker = Resources();
	const TSharedPtr<FUeAgentOperationContext> OperationContext =
		Context(this, ResourceTracker);
	if (!OperationContext.IsValid())
	{
		return false;
	}
	const TSharedRef<FOwnedEnvironment> Environment =
		MakeShared<FOwnedEnvironment>(Owner);
	const FUeAgentCandidateBinding MeshTarget =
		Target(TEXT("owned-mesh"), Owner.Mesh->GetPathName());
	TSharedPtr<const FUeAgentCanonicalSnapshot> Snapshot;
	FUeAgentAssetProjectReadError Error;

	TestTrue(
		TEXT("bounds sample captures one normalized object"),
		Capture(
			this,
			TEXT("asset_project.read.asset_sample"),
			TEXT("{\"sample_kind\":\"bounds\"}"),
			MeshTarget,
			Environment,
			*OperationContext,
			Snapshot,
			Error));
	const TSharedPtr<FJsonObject> Bounds =
		FindObjectSelector(Snapshot, TEXT("sample"));
	TestTrue(
		TEXT("bounds sample keeps only direct geometric values"),
		Bounds.IsValid()
			&& Bounds->GetStringField(TEXT("kind")) == TEXT("bounds")
			&& Bounds->HasField(TEXT("origin_cm"))
			&& Bounds->HasField(TEXT("extent_cm"))
			&& Bounds->HasField(TEXT("sphere_radius_cm"))
			&& !Bounds->HasField(TEXT("target"))
			&& !Bounds->HasField(TEXT("query")));

	Snapshot.Reset();
	Error = {};
	TestTrue(
		TEXT("vertex sample captures one normalized object"),
		Capture(
			this,
			TEXT("asset_project.read.asset_sample"),
			TEXT("{\"sample_kind\":\"vertex\",\"index\":0}"),
			MeshTarget,
			Environment,
			*OperationContext,
			Snapshot,
			Error));
	const TSharedPtr<FJsonObject> Vertex =
		FindObjectSelector(Snapshot, TEXT("sample"));
	TestTrue(
		TEXT("vertex sample keeps the requested index and position"),
		Vertex.IsValid()
			&& Vertex->GetStringField(TEXT("kind")) == TEXT("vertex")
			&& Vertex->GetNumberField(TEXT("index")) == 0.0
			&& Vertex->HasField(TEXT("position_cm")));

	Snapshot.Reset();
	Error = {};
	TestFalse(
		TEXT("unsupported legacy sample kind fails closed"),
		Capture(
			this,
			TEXT("asset_project.read.asset_sample"),
			TEXT("{\"sample_kind\":\"pixel\"}"),
			MeshTarget,
			Environment,
			*OperationContext,
			Snapshot,
			Error));
	TestEqual(
		TEXT("unsupported legacy sample kind remains typed"),
		Error.Code,
		FString(TEXT("asset_project_read_sample_kind_unsupported")));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUeAgentAssetCollectionPublicResultTest,
	"UeAgentInterface.DomainAdapters.AssetProjectRead.E2.Native."
		"AssetCollectionPublicResult",
	EAutomationTestFlags::EditorContext
		| EAutomationTestFlags::EngineFilter)

bool FUeAgentAssetCollectionPublicResultTest::RunTest(
	const FString& Parameters)
{
	FOwnedAssetProjectRead Owner;
	const FUeAgentResourceTrackerOwner ResourceTracker = Resources();
	const TSharedPtr<FUeAgentOperationContext> OperationContext =
		Context(this, ResourceTracker);
	if (!OperationContext.IsValid())
	{
		return false;
	}
	const TSharedRef<FOwnedEnvironment> Environment =
		MakeShared<FOwnedEnvironment>(Owner);
	TSharedPtr<const FUeAgentCanonicalSnapshot> Snapshot;
	FUeAgentAssetProjectReadError Error;
	TestTrue(
		TEXT("rows collection captures one canonical business object"),
		Capture(
			this,
			TEXT("asset_project.read.asset_collection"),
			TEXT("{\"collection_kind\":\"rows\",\"keys\":[\"Alpha\"]}"),
			Target(TEXT("owned-table"), Owner.Table->GetPathName()),
			Environment,
			*OperationContext,
			Snapshot,
			Error));
	const TSharedPtr<FJsonObject> Collection =
		FindObjectSelector(Snapshot, TEXT("collection"));
	if (!TestTrue(
			TEXT("collection selector is a canonical object"),
			Collection.IsValid()))
	{
		return false;
	}
	TestEqual(
		TEXT("collection kind is rows"),
		Collection->GetStringField(TEXT("kind")),
		FString(TEXT("rows")));
	TestEqual(
		TEXT("collection count matches the public rows"),
		Collection->GetNumberField(TEXT("count")),
		1.0);
	const TArray<TSharedPtr<FJsonValue>>* Rows = nullptr;
	TestTrue(
		TEXT("default row discovery returns one row"),
		Collection->TryGetArrayField(TEXT("rows"), Rows)
			&& Rows && Rows->Num() == 1);
	if (Rows && Rows->Num() == 1)
	{
		const TSharedPtr<FJsonObject> Row = (*Rows)[0]->AsObject();
		TestTrue(
			TEXT("default row discovery returns only its key"),
			Row.IsValid()
				&& Row->GetStringField(TEXT("key")) == TEXT("Alpha")
				&& !Row->HasField(TEXT("fields")));
	}
	TestFalse(
		TEXT("legacy collection kind selector is absent"),
		HasSelector(Snapshot, TEXT("collection_kind")));
	TestFalse(
		TEXT("legacy row key selector is absent"),
		HasSelector(Snapshot, TEXT("row_key")));

	Snapshot.Reset();
	Error = {};
	const FUeAgentCandidateBinding GraphTarget =
		Target(TEXT("owned-graph"), Owner.Graph->GetPathName());
	TestTrue(
		TEXT("reflected item discovery captures collection field names only"),
		Capture(
			this,
			TEXT("asset_project.read.asset_collection"),
			TEXT("{\"collection_kind\":\"items\"}"),
			GraphTarget,
			Environment,
			*OperationContext,
			Snapshot,
			Error));
	const TSharedPtr<FJsonObject> ItemDiscovery =
		FindObjectSelector(Snapshot, TEXT("collection"));
	const TArray<TSharedPtr<FJsonValue>>* CollectionFields = nullptr;
	TestTrue(
		TEXT("default reflected collection returns fields and counts"),
		ItemDiscovery.IsValid()
			&& ItemDiscovery->GetStringField(TEXT("kind")) == TEXT("items")
			&& ItemDiscovery->TryGetArrayField(
				TEXT("fields"), CollectionFields)
			&& CollectionFields
			&& CollectionFields->ContainsByPredicate(
				[](const TSharedPtr<FJsonValue>& Value)
				{
					const TSharedPtr<FJsonObject> Field =
						Value.IsValid() ? Value->AsObject() : nullptr;
					return Field.IsValid()
						&& Field->GetStringField(TEXT("field")) == TEXT("Nodes")
						&& Field->GetNumberField(TEXT("count")) == 1.0;
				})
			&& !ItemDiscovery->HasField(TEXT("items")));

	Snapshot.Reset();
	Error = {};
	TestTrue(
		TEXT("explicit reflected field returns its item values"),
		Capture(
			this,
			TEXT("asset_project.read.asset_collection"),
			TEXT("{\"collection_kind\":\"items\",\"field_ids\":[\"Nodes\"]}"),
			GraphTarget,
			Environment,
			*OperationContext,
			Snapshot,
			Error));
	const TSharedPtr<FJsonObject> ItemValues =
		FindObjectSelector(Snapshot, TEXT("collection"));
	const TArray<TSharedPtr<FJsonValue>>* Items = nullptr;
	TestTrue(
		TEXT("explicit reflected field returns only requested values"),
		ItemValues.IsValid()
			&& !ItemValues->HasField(TEXT("fields"))
			&& ItemValues->TryGetArrayField(TEXT("items"), Items)
			&& Items && Items->Num() == 1
			&& (*Items)[0]->AsObject()->GetStringField(TEXT("field"))
				== TEXT("Nodes")
			&& (*Items)[0]->AsObject()->GetNumberField(TEXT("index")) == 0.0
			&& (*Items)[0]->AsObject()->HasField(TEXT("value")));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUeAgentAssetRelationsPublicGraphTest,
	"UeAgentInterface.DomainAdapters.AssetProjectRead.E2.Native."
		"AssetRelationsPublicGraph",
	EAutomationTestFlags::EditorContext
		| EAutomationTestFlags::EngineFilter)

bool FUeAgentAssetRelationsPublicGraphTest::RunTest(
	const FString& Parameters)
{
	FOwnedAssetProjectRead Owner;
	const FUeAgentResourceTrackerOwner ResourceTracker = Resources();
	const TSharedPtr<FUeAgentOperationContext> OperationContext =
		Context(this, ResourceTracker);
	if (!OperationContext.IsValid())
	{
		return false;
	}
	const TSharedRef<FOwnedEnvironment> Environment =
		MakeShared<FOwnedEnvironment>(Owner);
	TSharedPtr<const FUeAgentCanonicalSnapshot> Snapshot;
	FUeAgentAssetProjectReadError Error;
	TestTrue(
		TEXT("asset.relations captures one normalized graph"),
		Capture(
			this,
			TEXT("asset_project.read.asset_relations"),
			TEXT("{\"direction\":\"both\",\"relation_kinds\":[\"hard\"],\"package_scope\":\"game\"}"),
			Target(TEXT("owned-asset"), Owner.Asset->GetPathName()),
			Environment,
			*OperationContext,
			Snapshot,
			Error));
	const TSharedPtr<FJsonObject> Graph =
		FindObjectSelector(Snapshot, TEXT("relation_graph"));
	if (!TestTrue(TEXT("relation graph is a canonical object"), Graph.IsValid()))
	{
		return false;
	}
	TestEqual(
		TEXT("root is the exact reusable object path"),
		Graph->GetStringField(TEXT("root")),
		Owner.Asset->GetPathName());
	TestFalse(TEXT("request echo is absent"), Graph->HasField(TEXT("query")));
	TestFalse(
		TEXT("legacy completeness envelope is absent"),
		Graph->HasField(TEXT("completeness")));

	const TArray<TSharedPtr<FJsonValue>>* Nodes = nullptr;
	TestTrue(
		TEXT("all resolved relation nodes are public"),
		Graph->TryGetArrayField(TEXT("nodes"), Nodes)
			&& Nodes && Nodes->Num() == 3);
	if (Nodes)
	{
		for (const TSharedPtr<FJsonValue>& Value : *Nodes)
		{
			const TSharedPtr<FJsonObject> Node =
				Value.IsValid() ? Value->AsObject() : nullptr;
			TestTrue(
				TEXT("node reference is directly reusable"),
				Node.IsValid()
					&& Node->GetStringField(TEXT("ref")).StartsWith(TEXT("/")));
			TestFalse(
				TEXT("node has no alias or internal identifier"),
				Node.IsValid()
					&& (Node->HasField(TEXT("key"))
						|| Node->HasField(TEXT("id"))));
		}
	}

	const TArray<TSharedPtr<FJsonValue>>* Edges = nullptr;
	TestTrue(
		TEXT("both relation directions are public"),
		Graph->TryGetArrayField(TEXT("edges"), Edges)
			&& Edges && Edges->Num() == 2);
	if (Edges)
	{
		for (const TSharedPtr<FJsonValue>& Value : *Edges)
		{
			const TSharedPtr<FJsonObject> Edge =
				Value.IsValid() ? Value->AsObject() : nullptr;
			TestTrue(
				TEXT("edge keeps the business relation and exact endpoints"),
				Edge.IsValid()
					&& Edge->GetStringField(TEXT("source")).StartsWith(TEXT("/"))
					&& Edge->GetStringField(TEXT("target")).StartsWith(TEXT("/"))
					&& Edge->HasField(TEXT("direction"))
					&& Edge->HasField(TEXT("depth"))
					&& Edge->HasField(TEXT("relation"))
					&& Edge->HasField(TEXT("properties"))
					&& Edge->HasField(TEXT("path")));
			TestFalse(
				TEXT("legacy traversal label is absent"),
				Edge.IsValid() && Edge->HasField(TEXT("traversal")));
		}
	}

	const TSharedPtr<FJsonObject> Coverage =
		Graph->GetObjectField(TEXT("coverage"));
	TestTrue(
		TEXT("coverage states the Asset Registry boundary"),
		Coverage.IsValid()
			&& Coverage->GetBoolField(TEXT("complete"))
			&& Coverage->GetStringField(TEXT("scope"))
				== TEXT("on_disk_asset_registry")
			&& !Coverage->GetBoolField(TEXT("runtime_references_included")));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUeAgentAssetProjectReadReferenceAbsencePolicyTest,
	"UeAgentInterface.DomainAdapters.AssetProjectRead.E2.Native."
		"ReferenceAbsenceIsValidOnlyForReferenceReadback",
	EAutomationTestFlags::EditorContext
		| EAutomationTestFlags::EngineFilter)

bool FUeAgentAssetProjectReadReferenceAbsencePolicyTest::RunTest(
	const FString& Parameters)
{
	FOwnedAssetProjectRead Owner;
	const FUeAgentResourceTrackerOwner ResourceTracker = Resources();
	const TSharedPtr<FUeAgentOperationContext> OperationContext =
		Context(this, ResourceTracker);
	if (!OperationContext.IsValid())
	{
		return false;
	}
	const TSharedRef<FOwnedEnvironment> Environment =
		MakeShared<FOwnedEnvironment>(Owner);
	const FUeAgentCandidateBinding MissingTarget = Target(
		TEXT("missing-asset"),
		TEXT("/Game/UAI/Reference/Missing.Missing"));
	TSharedPtr<const FUeAgentCanonicalSnapshot> Snapshot;
	FUeAgentAssetProjectReadError Error;

	TestTrue(
		TEXT("reference readback treats an absent exact target as a valid observation"),
		Capture(
			this,
			TEXT("asset_project.read.reference_asset"),
			TEXT("{\"field_ids\":[\"BlueprintDisplayName\"]}"),
			MissingTarget,
			Environment,
			*OperationContext,
			Snapshot,
			Error));
	TestTrue(TEXT("reference absence returns a canonical snapshot"),
		Snapshot.IsValid());
	TestEqual(TEXT("reference absence snapshot has no entries"),
		Snapshot.IsValid() ? Snapshot->GetEntries().Num() : -1,
		0);
	TestTrue(TEXT("reference absence clears the resolver error"),
		Error.Code.IsEmpty());

	Snapshot.Reset();
	Error = {};
	TestFalse(
		TEXT("generic asset properties still reject an unresolved target"),
		Capture(
			this,
			TEXT("asset_project.read.asset_properties"),
			TEXT("{\"field_ids\":[\"BlueprintDisplayName\"]}"),
			MissingTarget,
			Environment,
			*OperationContext,
			Snapshot,
			Error));
	TestFalse(TEXT("generic failure does not publish a snapshot"),
		Snapshot.IsValid());
	TestEqual(TEXT("generic unresolved target remains typed"),
		Error.Code,
		FString(TEXT("asset_project_read_asset_unresolved")));
	TestEqual(TEXT("both policies use the same environment owner"),
		Environment->AssetCalls,
		2);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUeAgentAssetProjectReadSemanticSelectorsTest,
	"UeAgentInterface.DomainAdapters.AssetProjectRead.E2.Native."
		"SemanticSelectorsUseDistinctRealObservations",
	EAutomationTestFlags::EditorContext
		| EAutomationTestFlags::EngineFilter)

bool FUeAgentAssetProjectReadSemanticSelectorsTest::RunTest(
	const FString& Parameters)
{
	FOwnedAssetProjectRead Owner;
	if (!TestNotNull(TEXT("owned data table"), Owner.Table)
		|| !TestNotNull(TEXT("owned static mesh"), Owner.Mesh)
		|| !TestNotNull(TEXT("owned dirty package"), Owner.DirtyPackage)
		|| !TestNotNull(TEXT("owned map package"), Owner.MapPackage))
	{
		return false;
	}
	const FUeAgentResourceTrackerOwner ResourceTracker = Resources();
	const TSharedPtr<FUeAgentOperationContext> OperationContext =
		Context(this, ResourceTracker);
	if (!OperationContext.IsValid())
	{
		return false;
	}
	const TSharedRef<FOwnedEnvironment> Environment =
		MakeShared<FOwnedEnvironment>(Owner);
	const FUeAgentCandidateBinding AssetTarget =
		Target(TEXT("owned-asset"), Owner.Asset->GetPathName());
	const FUeAgentCandidateBinding TableTarget =
		Target(TEXT("owned-table"), Owner.Table->GetPathName());
	const FUeAgentCandidateBinding MeshTarget =
		Target(TEXT("owned-mesh"), Owner.Mesh->GetPathName());
	TSharedPtr<const FUeAgentCanonicalSnapshot> Snapshot;
	FUeAgentAssetProjectReadError Error;

	TestTrue(
		TEXT("asset relations use the registry relation owner"),
		Capture(
			this,
			TEXT("asset_project.read.asset_relations"),
			TEXT("{\"direction\":\"both\",\"relation_kinds\":[\"hard\"]}"),
			AssetTarget,
			Environment,
			*OperationContext,
			Snapshot,
			Error));
	TestTrue(TEXT("asset relation contains a dependency fact"),
		HasSelector(Snapshot, TEXT("dependency")));

	Snapshot.Reset();
	Error = {};
	TestTrue(
		TEXT("asset validation executes a named rule"),
		Capture(
			this,
			TEXT("asset_project.read.asset_validation"),
			TEXT("{\"rule_ids\":[\"object.valid\"],\"include_passed\":true}"),
			AssetTarget,
			Environment,
			*OperationContext,
			Snapshot,
			Error));
	TestTrue(TEXT("asset validation publishes the executed rule identity"),
		HasSelector(Snapshot, TEXT("validation_rule_id")));

	Snapshot.Reset();
	Error = {};
	TestTrue(
		TEXT("data table row collection enumerates real rows"),
		Capture(
			this,
			TEXT("asset_project.read.asset_collection"),
			TEXT("{\"collection_kind\":\"rows\",\"keys\":[\"Alpha\"]}"),
			TableTarget,
			Environment,
			*OperationContext,
			Snapshot,
			Error));
	TestTrue(TEXT("row collection publishes one canonical collection"),
		HasSelector(Snapshot, TEXT("collection")));

	Snapshot.Reset();
	Error = {};
	TestTrue(
		TEXT("static mesh bounds sample reads geometric bounds"),
		Capture(
			this,
			TEXT("asset_project.read.asset_sample"),
			TEXT("{\"sample_kind\":\"bounds\"}"),
			MeshTarget,
			Environment,
			*OperationContext,
			Snapshot,
			Error));
	TestTrue(TEXT("bounds sample publishes one canonical sample"),
		HasSelector(Snapshot, TEXT("sample")));

	Snapshot.Reset();
	Error = {};
	TestTrue(
		TEXT("open assets query succeeds"),
		Capture(
			this,
			TEXT("asset_project.read.editor_open_assets"),
			TEXT("{\"include_editor_identity\":true}"),
			{},
			Environment,
			*OperationContext,
			Snapshot,
			Error));
	TestTrue(TEXT("open assets contains the explicitly open fixture asset"),
		HasOwner(Snapshot, Owner.Package->GetName()));
	TestFalse(TEXT("open assets excludes a merely dirty package"),
		HasOwner(Snapshot, Owner.DirtyPackage->GetName()));
	TestTrue(TEXT("open asset observation carries editor identity when requested"),
		HasSelector(Snapshot, TEXT("editor_identity")));

	Snapshot.Reset();
	Error = {};
	TestTrue(
		TEXT("dirty resource kind filter succeeds"),
		Capture(
			this,
			TEXT("asset_project.read.editor_dirty_resources"),
			TEXT("{\"resource_kinds\":[\"map\"]}"),
			{},
			Environment,
			*OperationContext,
			Snapshot,
			Error));
	TestTrue(TEXT("map filter keeps the dirty map package"),
		HasOwner(Snapshot, Owner.MapPackage->GetName()));
	TestFalse(TEXT("map filter excludes a dirty asset package"),
		HasOwner(Snapshot, Owner.DirtyPackage->GetName()));
	TestTrue(TEXT("dirty resource publishes its classified kind"),
		HasSelector(Snapshot, TEXT("resource_kind")));

	Snapshot.Reset();
	Error = {};
	TestTrue(
		TEXT("project platform resolves exact platform facts"),
		Capture(
			this,
			TEXT("asset_project.read.project_platform"),
			TEXT("{\"platform_ids\":[\"Windows\"],\"field_ids\":[\"platform_name\",\"display_name\",\"running_platform\"],\"resolve_inheritance\":true}"),
			{},
			Environment,
			*OperationContext,
			Snapshot,
			Error));
	TestTrue(TEXT("platform query publishes the selected platform identity"),
		HasSelector(Snapshot, TEXT("platform_name")));

	Snapshot.Reset();
	Error = {};
	TestTrue(
		TEXT("project validation executes a named rule"),
		Capture(
			this,
			TEXT("asset_project.read.project_validation"),
			TEXT("{\"section_ids\":[\"project\"],\"rule_ids\":[\"project_object.valid\"],\"include_passed\":true}"),
			{},
			Environment,
			*OperationContext,
			Snapshot,
			Error));
	TestTrue(TEXT("project validation publishes the executed rule identity"),
		HasSelector(Snapshot, TEXT("validation_rule_id")));

	TestFalse(TEXT("semantic read fixtures leave the asset package clean"),
		Owner.Package->IsDirty());
	return true;
}
