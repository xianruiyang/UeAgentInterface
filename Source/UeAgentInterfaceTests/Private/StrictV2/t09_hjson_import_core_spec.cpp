#include "Misc/AutomationTest.h"

#include "Core/ContextResource/UeAgentContextResource.h"
#include "Core/HjsonWorkspace/UeAgentAuthoringComposition.h"
#include "Core/HjsonWorkspace/UeAgentHjsonWorkspace.h"
#include "Core/HjsonWorkspace/SemanticModel/UeAgentUnifiedHjsonSemanticModel.h"
#include "HAL/FileManager.h"
#include "Misc/FileHelper.h"
#include "Misc/Guid.h"
#include "Misc/Paths.h"
#include "Policies/CondensedJsonPrintPolicy.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"
#include "Serialization/JsonWriter.h"

#if __has_include("Core/HjsonWorkspace/ImportInitialization/UeAgentHjsonDomainProviderRegistry.h")
#define UAI_T09_HJSON_DOMAIN_PROVIDER_REGISTRY_AVAILABLE 1
#include "Core/HjsonWorkspace/ImportInitialization/UeAgentHjsonDomainProviderRegistry.h"
#else
#define UAI_T09_HJSON_DOMAIN_PROVIDER_REGISTRY_AVAILABLE 0
#endif

#if __has_include("Core/HjsonWorkspace/ImportInitialization/UeAgentHjsonImportInitialization.h")
#define UAI_T09_HJSON_IMPORT_CORE_AVAILABLE 1
#include "Core/HjsonWorkspace/ImportInitialization/UeAgentHjsonImportInitialization.h"
#else
#define UAI_T09_HJSON_IMPORT_CORE_AVAILABLE 0
#endif

#if __has_include("Core/HjsonWorkspace/SourceLoader/UeAgentHjsonSourceLoader.h")
#define UAI_T09_HJSON_SOURCE_LOADER_AVAILABLE 1
#include "Core/HjsonWorkspace/SourceLoader/UeAgentHjsonSourceLoader.h"
#else
#define UAI_T09_HJSON_SOURCE_LOADER_AVAILABLE 0
#endif

#if UAI_T09_HJSON_IMPORT_CORE_AVAILABLE
namespace
{
	using namespace UE::UAI::ContextResource;
	using namespace UE::UAI::HjsonWorkspace;
	using namespace UE::UAI::StateSemantic;

	static FUeAgentResourceTracker MakeResources(
		const int64 Nodes = 32768,
		const int64 WorkUnits = 262144,
		const int64 Files = 4096)
	{
		return FUeAgentResourceTracker(FUeAgentResourceBudget({
			{EUeAgentResourceDimension::InputBytes, 8 * 1024 * 1024},
			{EUeAgentResourceDimension::OutputBytes, 8 * 1024 * 1024},
			{EUeAgentResourceDimension::StringBytes, 8 * 1024 * 1024},
			{EUeAgentResourceDimension::Nodes, Nodes},
			{EUeAgentResourceDimension::Items, 65536},
			{EUeAgentResourceDimension::Files, Files},
			{EUeAgentResourceDimension::ReferenceEdges, 65536},
			{EUeAgentResourceDimension::WorkUnits, WorkUnits},
			{EUeAgentResourceDimension::EstimatedMemoryBytes, 64 * 1024 * 1024},
		}));
	}

	static FSemanticUnit Unit(
		FString UnitId,
		const ESemanticUnitKind Kind,
		FString Owner,
		FString Role)
	{
		return FSemanticUnit(
			MoveTemp(UnitId),
			Kind,
			MoveTemp(Owner),
			TEXT("uai.strict_v2.t09.semantic/1"),
			MoveTemp(Role),
			FString(),
			{},
			{},
			{},
			FString(),
			FString(),
			{},
			{TEXT("state"), TEXT("minimal_create"), TEXT("local_edit")},
			TEXT("strict_v2:t09:current-design"));
	}

	static FUnifiedHjsonSemanticModelPtr MakeModel(FAutomationTestBase* Test)
	{
		const TArray<FString> Ids{
			TEXT("target"), TEXT("property"), TEXT("member"),
			TEXT("structure"), TEXT("item"), TEXT("endpoint"),
			TEXT("relation"), TEXT("order"), TEXT("layout"),
			TEXT("temporal"), TEXT("spatial"), TEXT("external"),
		};
		TArray<FSemanticUnit> Units{
			Unit(Ids[0], ESemanticUnitKind::TargetState, FString(), TEXT("target")),
			Unit(Ids[1], ESemanticUnitKind::PropertySet, Ids[0], TEXT("property_set")),
			Unit(Ids[2], ESemanticUnitKind::EntityState, Ids[0], TEXT("member")),
			Unit(Ids[3], ESemanticUnitKind::StructureState, Ids[0], TEXT("structure")),
			Unit(Ids[4], ESemanticUnitKind::ItemState, Ids[3], TEXT("item")),
			Unit(Ids[5], ESemanticUnitKind::EntityState, Ids[4], TEXT("endpoint")),
			Unit(Ids[6], ESemanticUnitKind::RelationSet, Ids[3], TEXT("relation")),
			Unit(Ids[7], ESemanticUnitKind::OrderingState, Ids[3], TEXT("order")),
			Unit(Ids[8], ESemanticUnitKind::LayoutState, Ids[3], TEXT("layout")),
			Unit(Ids[9], ESemanticUnitKind::PropertySet, Ids[0], TEXT("temporal_contract")),
			Unit(Ids[10], ESemanticUnitKind::PropertySet, Ids[0], TEXT("spatial_contract")),
			Unit(Ids[11], ESemanticUnitKind::ExternalPayloadState, Ids[0], TEXT("external_input")),
		};
		FUeAgentResourceTracker CoreResources = MakeResources();
		FWorkspaceError Error;
		FAuthoringModelPtr Core;
		Test->TestTrue(
			TEXT("T09 fixture compiles the canonical authoring model"),
			TryCompileAuthoringModel(
				TEXT("uai.strict_v2.t09.hjson"),
				1,
				TEXT("unified"),
				TEXT("fixture-r1"),
				MoveTemp(Units),
				{Ids[0]},
				CoreResources,
				Core,
				Error));
		if (!Core.IsValid())
		{
			return nullptr;
		}
		TMap<EUnifiedHjsonCompositionKind, TArray<FString>> Compositions{
			{EUnifiedHjsonCompositionKind::Target, {Ids[0]}},
			{EUnifiedHjsonCompositionKind::PropertySet, {Ids[1]}},
			{EUnifiedHjsonCompositionKind::MemberSet, {Ids[2]}},
			{EUnifiedHjsonCompositionKind::Structure, {Ids[3]}},
			{EUnifiedHjsonCompositionKind::Item, {Ids[4]}},
			{EUnifiedHjsonCompositionKind::Endpoint, {Ids[5]}},
			{EUnifiedHjsonCompositionKind::Relation, {Ids[6]}},
			{EUnifiedHjsonCompositionKind::Order, {Ids[7]}},
			{EUnifiedHjsonCompositionKind::Layout, {Ids[8]}},
			{EUnifiedHjsonCompositionKind::TemporalContract, {Ids[9]}},
			{EUnifiedHjsonCompositionKind::SpatialContract, {Ids[10]}},
			{EUnifiedHjsonCompositionKind::ExternalInput, {Ids[11]}},
		};
		FUeAgentResourceTracker ModelResources = MakeResources();
		FUnifiedHjsonSemanticModelPtr Model;
		Test->TestTrue(
			TEXT("T09 fixture compiles the unified local-edit model"),
			TryCreateUnifiedHjsonSemanticModel(
				Core.ToSharedRef(),
				EUnifiedHjsonAuthoringIntent::LocalEdit,
				MoveTemp(Compositions),
				ModelResources,
				Model,
				Error));
		return Model;
	}

	class FFixturePreflight final : public IImportInitializationPreflight
	{
	public:
		virtual bool TryReadCurrentTargetRevision(
			const FString& TargetIdentity,
			FUeAgentStateRevision& OutRevision,
			FWorkspaceError& OutError) override
		{
			Calls.Add(TEXT("version"));
			OutRevision = bStaleRevision
				? FUeAgentStateRevision(TEXT("t09-target"), TEXT("g1"), 8)
				: FUeAgentStateRevision(TEXT("t09-target"), TEXT("g1"), 7);
			return true;
		}

		virtual bool TryResolveCandidate(
			const FString& CandidateName,
			const FString& BindingRevision,
			FString& OutResolvedName,
			FWorkspaceError& OutError) override
		{
			Calls.Add(TEXT("candidate"));
			OutResolvedName = bRemapCandidate
				? TEXT("fixture.remapped")
				: CandidateName;
			return true;
		}

		virtual bool SupportsChange(
			const FImportChange& Change,
			FWorkspaceError& OutError) override
		{
			Calls.Add(TEXT("support"));
			if (bUnsupported)
			{
				OutError.Set(TEXT("fixture_change_unsupported"), Change.GetChangeId());
				return false;
			}
			return true;
		}

		virtual bool TryAuthorizeImpact(
			const TArray<FString>& ImpactScopes,
			const FString& PlanFingerprint,
			const FString& AuthorizationId,
			FWorkspaceError& OutError) override
		{
			Calls.Add(TEXT("authorize"));
			if (bUnauthorized)
			{
				OutError.Set(TEXT("fixture_unauthorized"));
				return false;
			}
			return !ImpactScopes.IsEmpty()
				&& PlanFingerprint.StartsWith(TEXT("sha256:"))
				&& AuthorizationId == TEXT("auth:t09:fixture");
		}

		TArray<FString> Calls;
		bool bStaleRevision = false;
		bool bRemapCandidate = false;
		bool bUnsupported = false;
		bool bUnauthorized = false;
	};

	static FImportInitializationPlanningRequest Request(
		const EImportSourceCarrier Carrier,
		FUnifiedHjsonSemanticModelRef Model,
		TArray<FImportChange> Changes = {
			FImportChange(
				TEXT("change:create-child"),
				EImportChangeKind::Create,
				TEXT("node:child"),
				TEXT("candidate.fixture.child"),
				TEXT("binding-r1")),
			FImportChange(
				TEXT("change:set-value"),
				EImportChangeKind::SetValue,
				TEXT("node:child")),
		},
		TArray<FInitializationNode> Nodes = {
			FInitializationNode(
				TEXT("node:root"),
				EInitializationNodeRole::RootTarget,
				TEXT("type.fixture.root"),
				TEXT("init.fixture.root")),
			FInitializationNode(
				TEXT("node:child"),
				EInitializationNodeRole::OwnedObject,
				TEXT("type.fixture.child"),
				TEXT("init.fixture.child")),
		},
		TArray<FInitializationEdge> Edges = {
			FInitializationEdge(
				TEXT("node:root"),
				TEXT("node:child"),
				EInitializationEdgeKind::Ownership),
		})
	{
		return FImportInitializationPlanningRequest(
			Carrier,
			MoveTemp(Model),
			TEXT("/Game/StrictV2/T09/Fixture"),
			FUeAgentStateRevision(TEXT("t09-target"), TEXT("g1"), 7),
			TEXT("sha256:41536f1320056b89c09fbd26aa8038876a023f49a382f2c57de33e5405389379"),
			TEXT("sha256:ac432558d81aade34196ea0ca5afc2547f324541ace1f3c403cbd4f6bbecf1d2"),
			MoveTemp(Changes),
			MoveTemp(Nodes),
			MoveTemp(Edges),
			{TEXT("asset:/Game/StrictV2/T09/Fixture")},
			TEXT("auth:t09:fixture"));
	}

	static bool Plan(
		const FImportInitializationPlanningRequest& InRequest,
		const TSharedRef<FFixturePreflight>& Preflight,
		FUeAgentResourceTracker& Resources,
		FImportInitializationPlanPtr& OutPlan,
		FWorkspaceError& OutError)
	{
		return TryPlanHjsonImportInitialization(
			InRequest,
			Preflight,
			Resources,
			OutPlan,
			OutError);
	}

	static FString Utf8Text(const TArray<uint8>& Bytes)
	{
		const FUTF8ToTCHAR Converted(
			reinterpret_cast<const ANSICHAR*>(Bytes.GetData()),
			Bytes.Num());
		return FString(Converted.Length(), Converted.Get());
	}

	static bool ParseJsonObject(
		const FString& Text,
		TSharedPtr<FJsonObject>& OutObject)
	{
		OutObject.Reset();
		const TSharedRef<TJsonReader<>> Reader =
			TJsonReaderFactory<>::Create(Text);
		return FJsonSerializer::Deserialize(Reader, OutObject)
			&& OutObject.IsValid();
	}

	static FString SerializeJsonObject(
		const TSharedRef<FJsonObject>& Object)
	{
		FString Text;
		const TSharedRef<
			TJsonWriter<TCHAR, TCondensedJsonPrintPolicy<TCHAR>>> Writer =
				TJsonWriterFactory<
					TCHAR,
					TCondensedJsonPrintPolicy<TCHAR>>::Create(&Text);
		FJsonSerializer::Serialize(Object, Writer);
		return Text;
	}

	static bool WriteExportPlan(
		const FExportWorkspacePlan& Plan,
		const FString& Root)
	{
		for (const FExportFilePlan& File : Plan.GetFiles())
		{
			const FString FullPath =
				FPaths::Combine(Root, File.GetRelativePath());
			if (!IFileManager::Get().MakeDirectory(
					*FPaths::GetPath(FullPath),
					true)
				|| !FFileHelper::SaveArrayToFile(
					File.GetCanonicalUtf8(),
					*FullPath))
			{
				return false;
			}
		}
		return true;
	}

	static bool BuildEmbeddedWorkspace(
		const FExportWorkspacePlan& Plan,
		FString& OutText)
	{
		OutText.Reset();
		const FExportFilePlan* ManifestFile =
			Plan.GetFiles().FindByPredicate(
				[](const FExportFilePlan& File)
				{
					return File.GetRelativePath()
						== TEXT(".uai/workspace.hjson");
				});
		if (ManifestFile == nullptr)
		{
			return false;
		}
		TSharedPtr<FJsonObject> Manifest;
		if (!ParseJsonObject(
				Utf8Text(ManifestFile->GetCanonicalUtf8()),
				Manifest))
		{
			return false;
		}
		const TArray<TSharedPtr<FJsonValue>>* Inventory = nullptr;
		if (!Manifest->TryGetArrayField(TEXT("unit_inventory"), Inventory)
			|| Inventory == nullptr)
		{
			return false;
		}
		TArray<TSharedPtr<FJsonValue>> Units;
		for (int32 Index = 0; Index < Inventory->Num(); ++Index)
		{
			const TSharedPtr<FJsonObject> InventoryRow =
				(*Inventory)[Index]->AsObject();
			FString LogicalPath;
			if (!InventoryRow.IsValid()
				|| !InventoryRow->TryGetStringField(
					TEXT("logical_path"),
					LogicalPath))
			{
				return false;
			}
			const FExportFilePlan* UnitFile =
				Plan.GetFiles().FindByPredicate(
					[&LogicalPath](const FExportFilePlan& File)
					{
						return File.GetRelativePath() == LogicalPath;
					});
			TSharedPtr<FJsonObject> UnitDocument;
			if (UnitFile == nullptr
				|| !ParseJsonObject(
					Utf8Text(UnitFile->GetCanonicalUtf8()),
					UnitDocument))
			{
				return false;
			}
			const TSharedPtr<FJsonObject>* Unit = nullptr;
			if (!UnitDocument->TryGetObjectField(TEXT("unit"), Unit)
				|| Unit == nullptr
				|| !Unit->IsValid())
			{
				return false;
			}
			InventoryRow->SetStringField(
				TEXT("logical_path"),
				FString::Printf(TEXT("#/units/%d"), Index));
			Units.Add(MakeShared<FJsonValueObject>(*Unit));
		}
		const TSharedRef<FJsonObject> Envelope = MakeShared<FJsonObject>();
		Envelope->SetObjectField(TEXT("manifest"), Manifest);
		Envelope->SetArrayField(TEXT("units"), MoveTemp(Units));
		OutText = SerializeJsonObject(Envelope);
		return !OutText.IsEmpty();
	}

	static bool LoadManifestObject(
		const FString& Root,
		TSharedPtr<FJsonObject>& OutManifest)
	{
		FString Text;
		return FFileHelper::LoadFileToString(
				Text,
				*FPaths::Combine(
					Root,
					TEXT(".uai/workspace.hjson")))
			&& ParseJsonObject(Text, OutManifest);
	}

	static bool SaveManifestObject(
		const FString& Root,
		const TSharedRef<FJsonObject>& Manifest)
	{
		return FFileHelper::SaveStringToFile(
			SerializeJsonObject(Manifest),
			*FPaths::Combine(Root, TEXT(".uai/workspace.hjson")),
			FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM);
	}

	static bool GetInventoryRows(
		const TSharedPtr<FJsonObject>& Manifest,
		const TArray<TSharedPtr<FJsonValue>>*& OutRows)
	{
		OutRows = nullptr;
		return Manifest.IsValid()
			&& Manifest->TryGetArrayField(TEXT("unit_inventory"), OutRows)
			&& OutRows != nullptr
			&& !OutRows->IsEmpty();
	}

	static bool BuildSourcePlan(
		FAutomationTestBase* Test,
		FUnifiedHjsonSemanticModelPtr& OutModel,
		FExportWorkspacePlanPtr& OutPlan,
		FString& OutEmbedded)
	{
		OutModel = MakeModel(Test);
		if (!OutModel.IsValid())
		{
			return false;
		}
		FUeAgentResourceTracker Resources = MakeResources();
		FWorkspaceError Error;
		if (!TryBuildCompositionExportPlan(
				OutModel->GetCoreModel(),
				{},
				Resources,
				OutPlan,
				Error)
			|| !OutPlan.IsValid())
		{
			Test->AddError(
				TEXT("T09_SOURCE_FIXTURE_EXPORT_FAILED:")
				+ Error.GetCode());
			return false;
		}
		if (!BuildEmbeddedWorkspace(*OutPlan, OutEmbedded))
		{
			Test->AddError(TEXT("T09_SOURCE_FIXTURE_ENVELOPE_FAILED"));
			return false;
		}
		return true;
	}

	class FScopedT09SourceWorkspace final
	{
	public:
		FScopedT09SourceWorkspace()
			: Root(FPaths::Combine(
				FPaths::ProjectIntermediateDir(),
				TEXT("UAI/Tests/StrictV2/T09/SourceLoader"),
				FGuid::NewGuid().ToString(EGuidFormats::Digits)))
		{
		}

		~FScopedT09SourceWorkspace()
		{
			IFileManager::Get().DeleteDirectory(*Root, false, true);
		}

		FString Child(const FString& Name) const
		{
			return FPaths::Combine(Root, Name);
		}

		FString Root;
	};
}
#endif

#if UAI_T09_HJSON_DOMAIN_PROVIDER_REGISTRY_AVAILABLE
namespace
{
	using namespace UE::UAI::HjsonWorkspace;

	class FFixtureDomainProvider final :
		public IImportInitializationDomainProvider
	{
	public:
		FFixtureDomainProvider(
			FString InProviderId,
			FString InSchemaId,
			FString InContractId)
			: ProviderId(MoveTemp(InProviderId))
			, SchemaId(MoveTemp(InSchemaId))
			, ContractId(MoveTemp(InContractId))
		{
		}

		virtual const FString& GetProviderId() const override
		{
			return ProviderId;
		}

		virtual bool Matches(
			const FImportInitializationDomainRequest& Request) const override
		{
			return Request.GetSchemaId() == SchemaId;
		}

#if defined(UAI_HJSON_DOMAIN_PROVIDER_RESOLVES_CONTRACT)
		virtual bool TryResolveInitializationContract(
			const FImportInitializationDomainRequest& Request,
			FString& OutContractId,
			FWorkspaceError& OutError) const override
		{
			OutContractId.Reset();
			OutError.Reset();
			if (Request.GetRootTypeIdentity() != TEXT("type.fixture.child")
				|| Request.GetRole() != EInitializationNodeRole::OwnedObject)
			{
				return false;
			}
			OutContractId = ContractId;
			return true;
		}

		virtual bool TryExecute(
			const FImportInitializationDomainRequest& Request,
			const FString& InitializationContractId,
			FImportInitializationDomainResponse& OutResponse,
			FWorkspaceError& OutError) const override
		{
			OutError.Reset();
			OutResponse = FImportInitializationDomainResponse(
				TEXT("{\"provider\":\"fixture\"}"), false);
			return Request.IsValid()
				&& InitializationContractId == ContractId
				&& OutResponse.IsValid();
		}
#else
		virtual bool SupportsInitializationContract(
			const FString& InContractId,
			const EInitializationNodeRole Role) const override
		{
			return InContractId == ContractId
				&& Role == EInitializationNodeRole::OwnedObject;
		}

		virtual bool TryExecute(
			const FImportInitializationDomainRequest& Request,
			FImportInitializationDomainResponse& OutResponse,
			FWorkspaceError& OutError) const override
		{
			OutError.Reset();
			OutResponse = FImportInitializationDomainResponse(
				TEXT("{\"provider\":\"fixture\"}"), false);
			return Request.IsValid() && OutResponse.IsValid();
		}
#endif

	private:
		FString ProviderId;
		FString SchemaId;
		FString ContractId;
	};

	FImportInitializationDomainRequest DomainRequest(
		FString SchemaId = TEXT("schema.fixture/1"),
		FString RootTypeId = TEXT("type.fixture.child"),
		FString CallerContractId = TEXT("init.fixture.child"))
	{
#if defined(UAI_HJSON_DOMAIN_PROVIDER_RESOLVES_CONTRACT)
		return FImportInitializationDomainRequest(
			TEXT("content_graph.import_workspace"),
			MoveTemp(SchemaId),
			TEXT("/Game/StrictV2/T09/Fixture.Fixture"),
			MoveTemp(RootTypeId),
			EInitializationNodeRole::OwnedObject,
			TEXT("{}"));
#else
		return FImportInitializationDomainRequest(
			TEXT("content_graph.import_workspace"),
			MoveTemp(SchemaId),
			TEXT("/Game/StrictV2/T09/Fixture.Fixture"),
			MoveTemp(RootTypeId),
			EInitializationNodeRole::OwnedObject,
			MoveTemp(CallerContractId),
			TEXT("{}"));
#endif
	}
}
#endif

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUeAgentStrictV2T09RealSourceCarrierTest,
	"UAI.StrictV2.T09.HJSON.IMPORT.CORE.RealSourceCarrierEquivalence",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FUeAgentStrictV2T09RealSourceCarrierTest::RunTest(
	const FString& Parameters)
{
#if !UAI_T09_HJSON_IMPORT_CORE_AVAILABLE
	TestTrue(TEXT("T09 HJSON import core is implemented"), false);
	return false;
#elif !UAI_T09_HJSON_SOURCE_LOADER_AVAILABLE
	AddError(TEXT("T09_SOURCE_LOADER_MISSING"));
	return false;
#else
	FUnifiedHjsonSemanticModelPtr Model;
	FExportWorkspacePlanPtr Plan;
	FString Embedded;
	if (!BuildSourcePlan(this, Model, Plan, Embedded))
	{
		return false;
	}
	FScopedT09SourceWorkspace Workspace;
	const FString DirectoryRoot = Workspace.Child(TEXT("directory"));
	const FString SingleFile = Workspace.Child(TEXT("single/workspace.hjson"));
	TestTrue(
		TEXT("directory fixture persists every declared file"),
		WriteExportPlan(*Plan, DirectoryRoot));
	TestTrue(
		TEXT("single-file fixture parent exists"),
		IFileManager::Get().MakeDirectory(
			*FPaths::GetPath(SingleFile),
			true));
	TestTrue(
		TEXT("single-file fixture persists the embedded envelope"),
		FFileHelper::SaveStringToFile(
			Embedded,
			*SingleFile,
			FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM));

	const TArray<FWorkspaceSource> Sources{
		FWorkspaceSource::Directory(DirectoryRoot),
		FWorkspaceSource::SingleFile(SingleFile),
		FWorkspaceSource::Inline(
			Embedded,
			TEXT("workspace.hjson"),
			TEXT("t09-inline-r1")),
	};
	TArray<FString> SemanticFingerprints;
	for (const FWorkspaceSource& Source : Sources)
	{
		FUeAgentResourceTracker Resources = MakeResources();
		FResolvedSemanticWorkspacePtr Loaded;
		FWorkspaceError Error;
		TestTrue(
			TEXT("every real carrier enters the public source loader"),
			TryLoadHjsonImportSource(
				Source,
				ESemanticWorkspaceMode::State,
				Resources,
				Loaded,
				Error));
		TestTrue(TEXT("every real carrier returns one resolved model"), Loaded.IsValid());
		if (Loaded.IsValid())
		{
			SemanticFingerprints.Add(
				Loaded->GetModel()->GetSemanticFingerprint());
		}
	}
	TestEqual(
		TEXT("directory, single-file and inline all resolve"),
		SemanticFingerprints.Num(),
		3);
	if (SemanticFingerprints.Num() == 3)
	{
		TestTrue(
			TEXT("carrier shape cannot change the edit semantic model"),
			SemanticFingerprints[0] == SemanticFingerprints[1]
				&& SemanticFingerprints[0] == SemanticFingerprints[2]
				&& SemanticFingerprints[0]
					== Model->GetCoreModel()->GetSemanticFingerprint());
	}
	return !HasAnyErrors();
#endif
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUeAgentStrictV2T09SourceBoundaryTest,
	"UAI.StrictV2.T09.HJSON.IMPORT.CORE.SourceBoundaryDigestAndVersion",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FUeAgentStrictV2T09SourceBoundaryTest::RunTest(
	const FString& Parameters)
{
#if !UAI_T09_HJSON_IMPORT_CORE_AVAILABLE
	TestTrue(TEXT("T09 HJSON import core is implemented"), false);
	return false;
#elif !UAI_T09_HJSON_SOURCE_LOADER_AVAILABLE
	AddError(TEXT("T09_SOURCE_LOADER_BOUNDARY_MISSING"));
	return false;
#else
	FUnifiedHjsonSemanticModelPtr Model;
	FExportWorkspacePlanPtr Plan;
	FString Embedded;
	if (!BuildSourcePlan(this, Model, Plan, Embedded))
	{
		return false;
	}
	FScopedT09SourceWorkspace Workspace;
	FResolvedSemanticWorkspacePtr Loaded;
	FWorkspaceError Error;

	const FString MissingRoot = Workspace.Child(TEXT("missing_manifest"));
	TestTrue(
		TEXT("missing-manifest fixture root exists"),
		IFileManager::Get().MakeDirectory(*MissingRoot, true));
	FUeAgentResourceTracker MissingResources = MakeResources();
	TestFalse(
		TEXT("directory without its machine manifest fails before publication"),
		TryLoadHjsonImportSource(
			FWorkspaceSource::Directory(MissingRoot),
			ESemanticWorkspaceMode::State,
			MissingResources,
			Loaded,
			Error));
	TestEqual(
		TEXT("missing manifest has a typed failure"),
		Error.GetCode(),
		FString(TEXT("semantic_workspace_manifest_missing")));

	const FString ExtraRoot = Workspace.Child(TEXT("extra_file"));
	TestTrue(TEXT("extra-file fixture persists"), WriteExportPlan(*Plan, ExtraRoot));
	TestTrue(
		TEXT("undeclared business file fixture persists"),
		FFileHelper::SaveStringToFile(
			TEXT("{}"),
			*FPaths::Combine(ExtraRoot, TEXT("extra.hjson"))));
	FUeAgentResourceTracker ExtraResources = MakeResources();
	TestFalse(
		TEXT("undeclared HJSON cannot enter the business model"),
		TryLoadHjsonImportSource(
			FWorkspaceSource::Directory(ExtraRoot),
			ESemanticWorkspaceMode::State,
			ExtraResources,
			Loaded,
			Error));
	TestEqual(
		TEXT("undeclared HJSON failure is typed"),
		Error.GetCode(),
		FString(TEXT("semantic_workspace_unit_extra")));

	const FString EscapeRoot = Workspace.Child(TEXT("path_escape"));
	TestTrue(TEXT("path-escape fixture persists"), WriteExportPlan(*Plan, EscapeRoot));
	TSharedPtr<FJsonObject> EscapeManifest;
	const TArray<TSharedPtr<FJsonValue>>* EscapeRows = nullptr;
	TestTrue(
		TEXT("path-escape manifest loads"),
		LoadManifestObject(EscapeRoot, EscapeManifest)
			&& GetInventoryRows(EscapeManifest, EscapeRows));
	if (EscapeRows != nullptr && !EscapeRows->IsEmpty())
	{
		(*EscapeRows)[0]->AsObject()->SetStringField(
			TEXT("logical_path"),
			TEXT("../outside.hjson"));
		TestTrue(
			TEXT("path-escape mutation persists"),
			SaveManifestObject(EscapeRoot, EscapeManifest.ToSharedRef()));
		FUeAgentResourceTracker EscapeResources = MakeResources();
		TestFalse(
			TEXT("parent traversal fails before any source publication"),
			TryLoadHjsonImportSource(
				FWorkspaceSource::Directory(EscapeRoot),
				ESemanticWorkspaceMode::State,
				EscapeResources,
				Loaded,
				Error));
		TestEqual(
			TEXT("path traversal failure is typed"),
			Error.GetCode(),
			FString(TEXT("workspace_path_invalid")));
	}

	const FString DuplicateRoot = Workspace.Child(TEXT("duplicate_identity"));
	TestTrue(TEXT("duplicate fixture persists"), WriteExportPlan(*Plan, DuplicateRoot));
	TSharedPtr<FJsonObject> DuplicateManifest;
	const TArray<TSharedPtr<FJsonValue>>* DuplicateRows = nullptr;
	TestTrue(
		TEXT("duplicate manifest loads"),
		LoadManifestObject(DuplicateRoot, DuplicateManifest)
			&& GetInventoryRows(DuplicateManifest, DuplicateRows)
			&& DuplicateRows->Num() > 1);
	if (DuplicateRows != nullptr && DuplicateRows->Num() > 1)
	{
		FString FirstId;
		(*DuplicateRows)[0]->AsObject()->TryGetStringField(TEXT("unit_id"), FirstId);
		(*DuplicateRows)[1]->AsObject()->SetStringField(TEXT("unit_id"), FirstId);
		TestTrue(
			TEXT("duplicate identity mutation persists"),
			SaveManifestObject(DuplicateRoot, DuplicateManifest.ToSharedRef()));
		FUeAgentResourceTracker DuplicateResources = MakeResources();
		TestFalse(
			TEXT("duplicate logical identity fails before publication"),
			TryLoadHjsonImportSource(
				FWorkspaceSource::Directory(DuplicateRoot),
				ESemanticWorkspaceMode::State,
				DuplicateResources,
				Loaded,
				Error));
		TestEqual(
			TEXT("duplicate identity failure is typed"),
			Error.GetCode(),
			FString(TEXT("semantic_workspace_unit_duplicate")));
	}

	const FString DigestRoot = Workspace.Child(TEXT("digest_mismatch"));
	TestTrue(TEXT("digest fixture persists"), WriteExportPlan(*Plan, DigestRoot));
	TSharedPtr<FJsonObject> DigestManifest;
	const TArray<TSharedPtr<FJsonValue>>* DigestRows = nullptr;
	TestTrue(
		TEXT("digest manifest loads"),
		LoadManifestObject(DigestRoot, DigestManifest)
			&& GetInventoryRows(DigestManifest, DigestRows));
	if (DigestRows != nullptr && !DigestRows->IsEmpty())
	{
		(*DigestRows)[0]->AsObject()->SetStringField(
			TEXT("content_hash"),
			TEXT("sha256:invalid"));
		TestTrue(
			TEXT("digest mutation persists"),
			SaveManifestObject(DigestRoot, DigestManifest.ToSharedRef()));
		FUeAgentResourceTracker DigestResources = MakeResources();
		TestFalse(
			TEXT("manifest digest mismatch fails before publication"),
			TryLoadHjsonImportSource(
				FWorkspaceSource::Directory(DigestRoot),
				ESemanticWorkspaceMode::State,
				DigestResources,
				Loaded,
				Error));
		TestEqual(
			TEXT("digest mismatch failure is typed"),
			Error.GetCode(),
			FString(TEXT("semantic_workspace_unit_hash_mismatch")));
	}

	const FString VersionRoot = Workspace.Child(TEXT("version_mismatch"));
	TestTrue(TEXT("version fixture persists"), WriteExportPlan(*Plan, VersionRoot));
	TSharedPtr<FJsonObject> VersionManifest;
	TestTrue(
		TEXT("version manifest loads"),
		LoadManifestObject(VersionRoot, VersionManifest));
	if (VersionManifest.IsValid())
	{
		VersionManifest->SetStringField(
			TEXT("workspace_schema"),
			TEXT("uai.semantic.workspace/unsupported"));
		TestTrue(
			TEXT("version mutation persists"),
			SaveManifestObject(VersionRoot, VersionManifest.ToSharedRef()));
		FUeAgentResourceTracker VersionResources = MakeResources();
		TestFalse(
			TEXT("unsupported workspace version fails before publication"),
			TryLoadHjsonImportSource(
				FWorkspaceSource::Directory(VersionRoot),
				ESemanticWorkspaceMode::State,
				VersionResources,
				Loaded,
				Error));
		TestEqual(
			TEXT("unsupported version failure is typed"),
			Error.GetCode(),
			FString(TEXT("semantic_workspace_schema_unsupported")));
	}
	return !HasAnyErrors();
#endif
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUeAgentStrictV2T09ParserResourceTest,
	"UAI.StrictV2.T09.HJSON.IMPORT.CORE.DeepParserAndBoundedResourceFailure",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FUeAgentStrictV2T09ParserResourceTest::RunTest(
	const FString& Parameters)
{
#if !UAI_T09_HJSON_IMPORT_CORE_AVAILABLE
	TestTrue(TEXT("T09 HJSON import core is implemented"), false);
	return false;
#elif !UAI_T09_HJSON_SOURCE_LOADER_AVAILABLE
	AddError(TEXT("T09_SOURCE_LOADER_RESOURCE_MISSING"));
	return false;
#else
	constexpr int32 Depth = 2048;
	FString Deep = TEXT("{value:");
	Deep.Reserve(Depth * 2 + 32);
	for (int32 Index = 0; Index < Depth; ++Index)
	{
		Deep += TEXT("[");
	}
	Deep += TEXT("0");
	for (int32 Index = 0; Index < Depth; ++Index)
	{
		Deep += TEXT("]");
	}
	Deep += TEXT("}");
	FUeAgentResourceTracker DeepResources =
		MakeResources(Depth + 64, Depth * 32);
	FHjsonDocumentPtr Document;
	FWorkspaceError Error;
	TestTrue(
		TEXT("HJSON parser accepts deep valid input without an artificial depth gate"),
		TryParseHjsonDocument(
			Deep,
			TEXT("t09-deep-inline.hjson"),
			DeepResources,
			Document,
			Error));
	TestTrue(TEXT("deep parse returns one immutable document"), Document.IsValid());

	FUnifiedHjsonSemanticModelPtr Model;
	FExportWorkspacePlanPtr Plan;
	FString Embedded;
	if (!BuildSourcePlan(this, Model, Plan, Embedded))
	{
		return false;
	}
	FUeAgentResourceTracker TightResources = MakeResources(32768, 262144, 0);
	FResolvedSemanticWorkspacePtr Loaded;
	TestFalse(
		TEXT("source loading stops when its explicit file budget is exhausted"),
		TryLoadHjsonImportSource(
			FWorkspaceSource::Inline(
				Embedded,
				TEXT("workspace.hjson"),
				TEXT("t09-tight-resource")),
			ESemanticWorkspaceMode::State,
			TightResources,
			Loaded,
			Error));
	TestEqual(
		TEXT("resource failure uses the shared typed error"),
		Error.GetCode(),
		FString(TEXT("resource_budget_exceeded")));
	TestTrue(TEXT("resource failure reports the observed amount"), Error.GetObserved() > 0);
	TestTrue(TEXT("resource failure proves early stop"), Error.IsStoppedEarly());
	TestFalse(TEXT("resource failure publishes no partial workspace"), Loaded.IsValid());
	return !HasAnyErrors();
#endif
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUeAgentStrictV2T09CarrierEquivalenceTest,
	"UAI.StrictV2.T09.HJSON.IMPORT.CORE.CarrierEquivalenceAndExplicitChanges",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FUeAgentStrictV2T09CarrierEquivalenceTest::RunTest(const FString& Parameters)
{
#if !UAI_T09_HJSON_IMPORT_CORE_AVAILABLE
	TestTrue(TEXT("T09 unified HJSON import planning is implemented"), false);
	return false;
#else
	const FUnifiedHjsonSemanticModelPtr Model = MakeModel(this);
	if (!Model.IsValid())
	{
		return false;
	}
	TArray<FString> Fingerprints;
	for (const EImportSourceCarrier Carrier : {
		EImportSourceCarrier::Directory,
		EImportSourceCarrier::MultiFile,
		EImportSourceCarrier::SingleFile,
		EImportSourceCarrier::Inline})
	{
		FUeAgentResourceTracker Resources = MakeResources();
		FWorkspaceError Error;
		FImportInitializationPlanPtr Result;
		const TSharedRef<FFixturePreflight> Preflight = MakeShared<FFixturePreflight>();
		TestTrue(
			TEXT("every carrier enters the same public planner"),
			Plan(Request(Carrier, Model.ToSharedRef()), Preflight, Resources, Result, Error));
		TestTrue(TEXT("carrier produces one immutable plan"), Result.IsValid());
		if (Result.IsValid())
		{
			Fingerprints.Add(Result->GetPlanFingerprint());
			TestEqual(TEXT("only explicit changes enter the plan"), Result->GetChanges().Num(), 2);
			TestEqual(TEXT("omission does not synthesize deletion"),
				Result->CountChanges(EImportChangeKind::Delete), 0);
			TestEqual(TEXT("candidate identity is preserved exactly"),
				Result->GetResolvedCandidates().FindRef(TEXT("change:create-child")),
				FString(TEXT("candidate.fixture.child")));
		}
	}
	TestEqual(TEXT("all four carriers produce a plan"), Fingerprints.Num(), 4);
	if (Fingerprints.Num() == 4)
	{
		TestTrue(TEXT("carrier representation cannot change business planning"),
			Fingerprints[0] == Fingerprints[1]
			&& Fingerprints[0] == Fingerprints[2]
			&& Fingerprints[0] == Fingerprints[3]);
	}
	return true;
#endif
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUeAgentStrictV2T09PrewriteFailureTest,
	"UAI.StrictV2.T09.HJSON.IMPORT.CORE.PrewriteValidationAndCandidateFailure",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FUeAgentStrictV2T09PrewriteFailureTest::RunTest(const FString& Parameters)
{
#if !UAI_T09_HJSON_IMPORT_CORE_AVAILABLE
	TestTrue(TEXT("T09 prewrite failures are implemented"), false);
	return false;
#else
	const FUnifiedHjsonSemanticModelPtr Model = MakeModel(this);
	if (!Model.IsValid())
	{
		return false;
	}
	FWorkspaceError Error;
	FImportInitializationPlanPtr Result;

	const TSharedRef<FFixturePreflight> Stale = MakeShared<FFixturePreflight>();
	Stale->bStaleRevision = true;
	FUeAgentResourceTracker StaleResources = MakeResources();
	TestFalse(TEXT("stale target fails before plan publication"),
		Plan(Request(EImportSourceCarrier::Inline, Model.ToSharedRef()), Stale, StaleResources, Result, Error));
	TestEqual(TEXT("stale target has a typed conflict"), Error.GetCode(),
		FString(TEXT("hjson_import_target_version_conflict")));
	TestFalse(TEXT("stale failure publishes no plan"), Result.IsValid());
	TestFalse(TEXT("stale target is never authorized"), Stale->Calls.Contains(TEXT("authorize")));

	const TSharedRef<FFixturePreflight> Remapped = MakeShared<FFixturePreflight>();
	Remapped->bRemapCandidate = true;
	FUeAgentResourceTracker CandidateResources = MakeResources();
	TestFalse(TEXT("implicit candidate remapping is rejected"),
		Plan(Request(EImportSourceCarrier::Inline, Model.ToSharedRef()), Remapped, CandidateResources, Result, Error));
	TestEqual(TEXT("candidate remapping has a typed failure"), Error.GetCode(),
		FString(TEXT("hjson_import_candidate_identity_changed")));

	const TSharedRef<FFixturePreflight> Unsupported = MakeShared<FFixturePreflight>();
	Unsupported->bUnsupported = true;
	FUeAgentResourceTracker UnsupportedResources = MakeResources();
	TestFalse(TEXT("unsupported explicit change fails closed"),
		Plan(Request(EImportSourceCarrier::Inline, Model.ToSharedRef()), Unsupported, UnsupportedResources, Result, Error));
	TestEqual(TEXT("provider failure remains typed"), Error.GetCode(),
		FString(TEXT("fixture_change_unsupported")));

	const TSharedRef<FFixturePreflight> Unauthorized = MakeShared<FFixturePreflight>();
	Unauthorized->bUnauthorized = true;
	FUeAgentResourceTracker AuthorizationResources = MakeResources();
	TestFalse(TEXT("impact authorization is mandatory"),
		Plan(Request(EImportSourceCarrier::Inline, Model.ToSharedRef()), Unauthorized, AuthorizationResources, Result, Error));
	TestEqual(TEXT("authorization failure remains typed"), Error.GetCode(),
		FString(TEXT("fixture_unauthorized")));
	return true;
#endif
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUeAgentStrictV2T09InitializationGraphTest,
	"UAI.StrictV2.T09.HJSON.IMPORT.CORE.IterativeInitializationGraph",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FUeAgentStrictV2T09InitializationGraphTest::RunTest(const FString& Parameters)
{
#if !UAI_T09_HJSON_IMPORT_CORE_AVAILABLE
	TestTrue(TEXT("T09 iterative initialization graph is implemented"), false);
	return false;
#else
	const FUnifiedHjsonSemanticModelPtr Model = MakeModel(this);
	if (!Model.IsValid())
	{
		return false;
	}
	TArray<FInitializationNode> DeepNodes;
	TArray<FInitializationEdge> DeepEdges;
	constexpr int32 Depth = 2048;
	for (int32 Index = 0; Index < Depth; ++Index)
	{
		const FString Id = FString::Printf(TEXT("node:%04d"), Index);
		DeepNodes.Emplace(
			Id,
			Index == 0 ? EInitializationNodeRole::RootTarget : EInitializationNodeRole::OwnedObject,
			TEXT("type.fixture.deep"),
			TEXT("init.fixture.deep"));
		if (Index > 0)
		{
			DeepEdges.Emplace(
				FString::Printf(TEXT("node:%04d"), Index - 1),
				Id,
				EInitializationEdgeKind::Ownership);
		}
	}
	FUeAgentResourceTracker DeepResources = MakeResources(4096, 131072);
	FWorkspaceError Error;
	FImportInitializationPlanPtr Result;
	TestTrue(TEXT("deep ownership is planned with an explicit worklist"),
		Plan(
			Request(
				EImportSourceCarrier::Directory,
				Model.ToSharedRef(),
				{FImportChange(TEXT("change:deep"), EImportChangeKind::Create, TEXT("node:2047"))},
				MoveTemp(DeepNodes),
				MoveTemp(DeepEdges)),
			MakeShared<FFixturePreflight>(),
			DeepResources,
			Result,
			Error));
	if (!Result.IsValid())
	{
		return false;
	}
	TestEqual(TEXT("all deep nodes are ordered"), Result->GetCreationOrder().Num(), Depth);

	TArray<FInitializationNode> CycleNodes{
		FInitializationNode(TEXT("node:a"), EInitializationNodeRole::OwnedObject, TEXT("type.a"), TEXT("init.a")),
		FInitializationNode(TEXT("node:b"), EInitializationNodeRole::OwnedObject, TEXT("type.b"), TEXT("init.b")),
	};
	TArray<FInitializationEdge> OwnershipCycle{
		FInitializationEdge(TEXT("node:a"), TEXT("node:b"), EInitializationEdgeKind::Ownership),
		FInitializationEdge(TEXT("node:b"), TEXT("node:a"), EInitializationEdgeKind::Ownership),
	};
	FUeAgentResourceTracker CycleResources = MakeResources();
	TestFalse(TEXT("ownership cycles fail before any write"),
		Plan(Request(EImportSourceCarrier::SingleFile, Model.ToSharedRef(),
			{FImportChange(TEXT("change:cycle"), EImportChangeKind::Create, TEXT("node:a"))},
			CycleNodes, OwnershipCycle), MakeShared<FFixturePreflight>(), CycleResources, Result, Error));
	TestEqual(TEXT("ownership cycle is precise"), Error.GetCode(),
		FString(TEXT("hjson_import_ownership_cycle")));

	TArray<FInitializationEdge> ReferenceCycle{
		FInitializationEdge(TEXT("node:a"), TEXT("node:b"), EInitializationEdgeKind::Reference),
		FInitializationEdge(TEXT("node:b"), TEXT("node:a"), EInitializationEdgeKind::Reference),
	};
	FUeAgentResourceTracker ReferenceResources = MakeResources();
	TestTrue(TEXT("reference cycles are legal and deferred"),
		Plan(Request(EImportSourceCarrier::MultiFile, Model.ToSharedRef(),
			{FImportChange(TEXT("change:refs"), EImportChangeKind::Connect, TEXT("node:a"))},
			CycleNodes, ReferenceCycle), MakeShared<FFixturePreflight>(), ReferenceResources, Result, Error));
	TestEqual(TEXT("both reference edges are retained"), Result->GetReferenceEdges().Num(), 2);
	return true;
#endif
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUeAgentStrictV2T09ResourceFailureTest,
	"UAI.StrictV2.T09.HJSON.IMPORT.CORE.ResourceFailureAndCleanup",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FUeAgentStrictV2T09ResourceFailureTest::RunTest(const FString& Parameters)
{
#if !UAI_T09_HJSON_IMPORT_CORE_AVAILABLE
	TestTrue(TEXT("T09 resource failures are implemented"), false);
	return false;
#else
	const FUnifiedHjsonSemanticModelPtr Model = MakeModel(this);
	if (!Model.IsValid())
	{
		return false;
	}
	FUeAgentResourceTracker Resources = MakeResources(1, 8);
	FWorkspaceError Error;
	FImportInitializationPlanPtr Result;
	const TSharedRef<FFixturePreflight> Preflight = MakeShared<FFixturePreflight>();
	TestFalse(TEXT("real node budget is enforced before plan publication"),
		Plan(Request(EImportSourceCarrier::Inline, Model.ToSharedRef()), Preflight, Resources, Result, Error));
	TestEqual(TEXT("resource failure remains the shared explicit error"), Error.GetCode(),
		FString(TEXT("resource_budget_exceeded")));
	TestFalse(TEXT("resource failure publishes no partial plan"), Result.IsValid());
	TestFalse(TEXT("resource failure performs no authorization side effect"),
		Preflight->Calls.Contains(TEXT("authorize")));
	return true;
#endif
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUeAgentStrictV2T09DomainProviderRegistryTest,
	"UAI.StrictV2.T09.HJSON.IMPORT.CORE.DomainProviderUniqueResolution",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FUeAgentStrictV2T09DomainProviderRegistryTest::RunTest(
	const FString& Parameters)
{
#if !UAI_T09_HJSON_DOMAIN_PROVIDER_REGISTRY_AVAILABLE
	TestTrue(
		TEXT("T09 public HJSON domain provider registry is implemented"),
		false);
	return false;
#else
	FWorkspaceError Error;
	TSharedPtr<const FImportInitializationDomainProviderRegistry> Registry;
	const TSharedRef<FFixtureDomainProvider> First =
		MakeShared<FFixtureDomainProvider>(
			TEXT("fixture-provider"),
			TEXT("schema.fixture/1"),
			TEXT("init.fixture.child"));
	TestTrue(
		TEXT("one exact provider catalog freezes"),
		FImportInitializationDomainProviderRegistry::TryFreeze(
			{First}, Registry, Error));
	TestTrue(TEXT("frozen provider registry exists"), Registry.IsValid());

#if defined(UAI_HJSON_DOMAIN_PROVIDER_RESOLVES_CONTRACT)
	FImportInitializationDomainResolution Resolution;
	TestTrue(
		TEXT("schema, target type and role resolve the provider and its contract"),
		Registry.IsValid()
			&& Registry->TryResolve(DomainRequest(), Resolution, Error));
	TestTrue(
		TEXT("the exact provider is returned"),
		Resolution.GetProvider() == First);
	TestEqual(
		TEXT("the provider, not the caller, supplies the exact contract"),
		Resolution.GetInitializationContractId(),
		FString(TEXT("init.fixture.child")));

	FImportInitializationDomainResponse Response;
	TestTrue(
		TEXT("the resolved provider remains the sole domain execution port"),
		Resolution.IsValid()
			&& Resolution.GetProvider()->TryExecute(
				DomainRequest(),
				Resolution.GetInitializationContractId(),
				Response,
				Error));
	TestTrue(TEXT("provider response is explicit"), Response.IsValid());

	Resolution.Reset();
	TestFalse(
		TEXT("unknown target type fails closed without a caller contract map"),
		Registry.IsValid()
			&& Registry->TryResolve(
				DomainRequest(
					TEXT("schema.fixture/1"),
					TEXT("type.fixture.unknown")),
				Resolution,
				Error));
	TestEqual(
		TEXT("unknown provider contract is typed"),
		Error.GetCode(),
		FString(TEXT("hjson_domain_provider_unresolved")));
#else
	TSharedPtr<const IImportInitializationDomainProvider> Resolved;
	TestTrue(
		TEXT("caller-selected contract is ignored in favor of provider resolution"),
		Registry.IsValid()
			&& Registry->TryResolve(
				DomainRequest(
					TEXT("schema.fixture/1"),
					TEXT("type.fixture.child"),
					TEXT("init.caller.must-not-select")),
				Resolved,
				Error));
#endif

	const TSharedRef<FFixtureDomainProvider> DuplicateId =
		MakeShared<FFixtureDomainProvider>(
			TEXT("fixture-provider"),
			TEXT("schema.other/1"),
			TEXT("init.other"));
	TestFalse(
		TEXT("duplicate provider identity is rejected at freeze"),
		FImportInitializationDomainProviderRegistry::TryFreeze(
			{First, DuplicateId}, Registry, Error));
	TestEqual(
		TEXT("duplicate identity is typed"),
		Error.GetCode(),
		FString(TEXT("hjson_domain_provider_duplicate")));

	const TSharedRef<FFixtureDomainProvider> Ambiguous =
		MakeShared<FFixtureDomainProvider>(
			TEXT("fixture-provider-two"),
			TEXT("schema.fixture/1"),
			TEXT("init.fixture.child"));
	TestTrue(
		TEXT("distinct providers can coexist before a request is resolved"),
		FImportInitializationDomainProviderRegistry::TryFreeze(
			{First, Ambiguous}, Registry, Error));
#if defined(UAI_HJSON_DOMAIN_PROVIDER_RESOLVES_CONTRACT)
	Resolution.Reset();
	TestFalse(
		TEXT("ambiguous runtime ownership is rejected"),
		Registry.IsValid()
			&& Registry->TryResolve(DomainRequest(), Resolution, Error));
#else
	Resolved.Reset();
	TestFalse(
		TEXT("ambiguous runtime ownership is rejected"),
		Registry.IsValid()
			&& Registry->TryResolve(DomainRequest(), Resolved, Error));
#endif
	TestEqual(
		TEXT("ambiguous ownership is typed"),
		Error.GetCode(),
		FString(TEXT("hjson_domain_provider_ambiguous")));
	return true;
#endif
}
