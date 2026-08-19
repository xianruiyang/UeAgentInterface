#include "Misc/AutomationTest.h"

// RE-005 production must satisfy this RE-004 frozen native boundary.
#include "DomainAdapters/AssetProjectRead/UeAgentAssetProjectReadNative.h"


#include "AssetRegistry/AssetData.h"
#include "Core/ContextResource/UeAgentOperationContext.h"
#include "Core/Contract/UeAgentContract.h"
#include "Core/Discovery/UeAgentDiscovery.h"
#include "Core/StateSemantic/UeAgentStateSemantic.h"
#include "Curves/CurveFloat.h"
#include "EdGraph/EdGraph.h"
#include "EdGraph/EdGraphNode.h"
#include "HAL/FileManager.h"
#include "Misc/ConfigCacheIni.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
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
			RegistryRecord = FAssetData(Asset);
			Config.SetString(
				TEXT("UAI.AssetProjectRead"),
				TEXT("KnownValue"),
				TEXT("fixture-value"));
			Package->SetDirtyFlag(false);
		}

		~FOwnedAssetProjectRead()
		{
			if (Package)
			{
				Package->SetDirtyFlag(false);
				Package->MarkAsGarbage();
			}
		}

		UPackage* Package = nullptr;
		UCurveFloat* Asset = nullptr;
		UCurveFloat* ProjectObject = nullptr;
		UEdGraph* Graph = nullptr;
		UEdGraphNode* GraphNode = nullptr;
		FAssetData RegistryRecord;
		FConfigFile Config;
	};

	FUeAgentDescriptorIdentity Identity()
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
			if (InTarget.GetBindingId() != TEXT("owned-asset"))
			{
				OutError.Code = TEXT("asset_project_read_asset_unresolved");
				OutError.SubjectId = InTarget.GetBindingId();
				return false;
			}
			OutAsset = Owner.Asset;
			return true;
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
			const FString& Direction,
			const TArray<FString>& RelationKinds,
			TArray<FString>& OutDependencies,
			TArray<FString>& OutReferencers,
			FUeAgentAssetProjectReadError& OutError) override
		{
			++RelationCalls;
			if (!Record.IsValid()
				|| Direction != TEXT("both")
				|| !RelationKinds.Contains(TEXT("hard")))
			{
				OutError.Code = TEXT("fixture_relation_input_mismatch");
				return false;
			}
			OutDependencies = {TEXT("/Game/Dependency")};
			OutReferencers = {TEXT("/Game/Referencer")};
			return true;
		}

		bool TryResolveEditorResources(
			const FUeAgentExecutionContextRef& InContext,
			TArray<UPackage*>& OutPackages,
			FUeAgentAssetProjectReadError& OutError) override
		{
			++EditorCalls;
			OutPackages = {Owner.Package};
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
		TEXT("registry relation family reads real graph edges through its environment owner"),
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
	TestTrue(TEXT("editor snapshot owns package"), HasOwner(Snapshot, Owner.Package->GetName()));

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
