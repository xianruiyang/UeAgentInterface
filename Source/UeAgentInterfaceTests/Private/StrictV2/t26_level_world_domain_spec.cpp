#include "Misc/AutomationTest.h"

#include "AssetRegistry/AssetRegistryModule.h"
#include "Core/Contract/UeAgentCommandJson.h"
#include "Core/HjsonWorkspace/ImportInitialization/UeAgentHjsonDomainProviderRegistry.h"
#include "Core/HjsonWorkspace/UeAgentAuthoringComposition.h"
#include "Commands/PaletteSearch/UeAgentPaletteSearchTypes.h"
#include "DomainAdapters/WorldSpatial/UeAgentLevelWorldDomainMechanics.h"
#include "Domains/Level/Authoring/UeAgentLevelAuthoringRegistration.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Engine/Blueprint.h"
#include "Engine/BlueprintGeneratedClass.h"
#include "GameFramework/Actor.h"
#include "HAL/FileManager.h"
#include "Kismet2/KismetEditorUtilities.h"
#include "Misc/Paths.h"
#include "Misc/ScopeExit.h"
#include "Module/CommandRegistration/Active/UeAgentActiveCommandComposition.h"
#include "ObjectTools.h"
#include "Utilities/AuthoringOperationApply/UeAgentApplyInputContract.h"

#if __has_include("OperationOwners/WorldSpatial/UeAgentLevelWorldDomainService.h")
#define UAI_T26_LEVEL_WORLD_DOMAIN_AVAILABLE 1
#include "OperationOwners/WorldSpatial/UeAgentLevelWorldDomainService.h"
#else
#define UAI_T26_LEVEL_WORLD_DOMAIN_AVAILABLE 0
#endif

#if UAI_T26_LEVEL_WORLD_DOMAIN_AVAILABLE
namespace
{
	void DeleteAssetIfPresent(const FString& ObjectPath)
	{
		if (UObject* Existing = StaticLoadObject(
			UObject::StaticClass(), nullptr, *ObjectPath))
		{
			ObjectTools::DeleteObjectsUnchecked({Existing});
		}
	}

	bool TryBuildResolvedWorldImportParams(
		const FString& Target,
		const FString& Document,
		FString& OutParams,
		FString& OutError)
	{
		const TSharedRef<FJsonObject> Params = MakeShared<FJsonObject>();
		Params->SetStringField(TEXT("target"), Target);
		Params->SetStringField(TEXT("expected_revision"), TEXT("missing"));
		const TSharedRef<FJsonObject> Source = MakeShared<FJsonObject>();
		Source->SetStringField(TEXT("kind"), TEXT("resolved"));
		Source->SetStringField(TEXT("target_identity"), Target);
		Source->SetStringField(TEXT("entry_file"), TEXT("index.hjson"));
		const TSharedRef<FJsonObject> Entry = MakeShared<FJsonObject>();
		Entry->SetStringField(TEXT("path"), TEXT("index.hjson"));
		Entry->SetStringField(TEXT("document"), Document);
		Source->SetArrayField(
			TEXT("documents"), {MakeShared<FJsonValueObject>(Entry)});
		Params->SetObjectField(TEXT("source"), Source);
		return UE::UAI::CommandContract::Json::TryCanonicalize(
			Params, OutParams, OutError);
	}

	TSharedPtr<FJsonObject> FindActorByManagedId(
		const TSharedPtr<FJsonObject>& Snapshot,
		const FString& ManagedId)
	{
		const TArray<TSharedPtr<FJsonValue>>* Actors = nullptr;
		if (!Snapshot.IsValid()
			|| !Snapshot->TryGetArrayField(TEXT("actors"), Actors)
			|| !Actors)
		{
			return nullptr;
		}
		for (const TSharedPtr<FJsonValue>& Value : *Actors)
		{
			const TSharedPtr<FJsonObject> Actor = Value.IsValid()
				? Value->AsObject()
				: nullptr;
			FString ActualManagedId;
			if (Actor.IsValid()
				&& Actor->TryGetStringField(
					TEXT("managed_id"), ActualManagedId)
				&& ActualManagedId == ManagedId)
			{
				return Actor;
			}
		}
		return nullptr;
	}
}
#endif

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUeAgentStrictV2T26LevelWorldDomainTest,
	"UAI.StrictV2.T26.LEVEL.WORLD.DOMAIN",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FUeAgentStrictV2T26LevelWorldDomainTest::RunTest(
	const FString& Parameters)
{
	(void)Parameters;
#if !UAI_T26_LEVEL_WORLD_DOMAIN_AVAILABLE
	TestTrue(TEXT("T26 Level/World domain authority is available"), false);
	return false;
#else
	using namespace UE::UAI;
	using namespace UE::UAI::HjsonWorkspace;
	using namespace UE::UAI::OperationOwners::WorldSpatial;

	TSharedPtr<IImportInitializationDomainProvider> Provider;
	FWorkspaceError WorkspaceError;
	if (!TestTrue(TEXT("the Level/World provider is created"),
		TryCreateLevelWorldDomainProvider(Provider, WorkspaceError))
		|| !Provider.IsValid())
	{
		AddError(WorkspaceError.GetCode());
		return false;
	}
	TestTrue(TEXT("the World provider owns authoring-schema discovery"),
		Provider->SupportsExplicitCommand(
			TEXT("content_graph.authoring_schema")));
	TestTrue(TEXT("the World provider routes export to the verified Level publisher"),
		Provider->SupportsExplicitCommand(
			TEXT("content_graph.export_workspace")));

	struct FContractCase
	{
		const TCHAR* Type;
		EInitializationNodeRole Role;
		const TCHAR* Contract;
	};
	const TArray<FContractCase> ContractCases = {
		{TEXT("/Script/Engine.World"), EInitializationNodeRole::RootTarget,
			TEXT("uai.init.world.world/1")},
		{TEXT("/Script/Engine.Actor"), EInitializationNodeRole::ContainerItem,
			TEXT("uai.init.world.actor/1")},
		{TEXT("/Script/Engine.StaticMeshActor"), EInitializationNodeRole::ContainerItem,
			TEXT("uai.init.world.actor/1")},
		{TEXT("/Script/Engine.SceneComponent"), EInitializationNodeRole::OwnedObject,
			TEXT("uai.init.world.component/1")},
		{TEXT("/Script/Engine.PointLightComponent"), EInitializationNodeRole::OwnedObject,
			TEXT("uai.init.world.component/1")},
		{TEXT("uai.world.data_layer"), EInitializationNodeRole::ContainerItem,
			TEXT("uai.init.world.data_layer/1")},
	};
	for (const FContractCase& ContractCase : ContractCases)
	{
		const FImportInitializationDomainRequest Request(
			TEXT("content_graph.import_workspace"), TEXT("uai.world.edit/1"),
			TEXT("/Game/UAI/StrictV2/T26/LV_T26.LV_T26"),
			ContractCase.Type, ContractCase.Role, TEXT("{}"));
		FString Contract;
		TestTrue(*FString::Printf(TEXT("world surface resolves: %s"),
			ContractCase.Type), Provider->Matches(Request)
				&& Provider->TryResolveInitializationContract(
					Request, Contract, WorkspaceError));
		TestEqual(TEXT("the initialization contract is exact"), Contract,
			FString(ContractCase.Contract));
	}

	const FImportInitializationDomainRequest Unsupported(
		TEXT("content_graph.import_workspace"), TEXT("uai.world.edit/1"),
		TEXT("/Game/UAI/StrictV2/T26/Unsupported.Unsupported"),
		TEXT("/Script/Engine.StaticMesh"), EInitializationNodeRole::RootTarget,
		TEXT("{}"));
	TestFalse(TEXT("an unregistered World root is rejected before write"),
		Provider->Matches(Unsupported));

	TSharedPtr<const CommandRegistry::FUeAgentImmutableCommandRegistry> Registry;
	TSharedPtr<const CommandEdge::FUeAgentExactCommandDispatch> Dispatch;
	FString Error;
	if (!TestTrue(TEXT("the process active composition freezes"),
		Module::CommandRegistration::Active::
			TryComposeProcessActiveCommandComposition(Registry, Dispatch, Error))
		|| !Registry.IsValid())
	{
		AddError(Error);
		return false;
	}
	const auto* ImportContract = Registry->FindExact(
		TEXT("content_graph.import_workspace"));
	TestNotNull(TEXT("the shared recursive HJSON ingress remains active"),
		ImportContract);
	if (ImportContract)
	{
		TestEqual(TEXT("T13 remains the shared recursive authority"),
			ImportContract->GetPublicOwner(),
			FString(TEXT("uobject-dataasset-recursive-initialization-owner")));
	}
	for (const FString& RetiredIngress : {
		FString(TEXT("level_apply_folder")),
		FString(TEXT("level.apply_folder")),
		FString(TEXT("level.apply_file")),
		FString(TEXT("world.authoring.apply"))})
	{
		TestNull(*FString::Printf(TEXT("%s remains absent"), *RetiredIngress),
			Registry->FindExact(RetiredIngress));
	}
	for (const FString& CommandId : {
		FString(TEXT("world.data_layer_instance.create")),
		FString(TEXT("world.data_layer_relation.update")),
		FString(TEXT("world.wall.create_with_opening"))})
	{
		const auto* WorldContract = Registry->FindExact(CommandId);
		TestNotNull(*FString::Printf(TEXT("%s remains active"), *CommandId),
			WorldContract);
		if (WorldContract)
		{
			TestEqual(TEXT("T59 remains the non-HJSON World owner"),
				WorldContract->GetPublicOwner(),
				FString(TEXT("world-command-owner")));
		}
	}
	const auto* ActorCandidates = Registry->FindExact(
		TEXT("world.actor.candidates"));
	TestNotNull(TEXT("the native Place Actors query is active"),
		ActorCandidates);
	if (ActorCandidates)
	{
		TestEqual(TEXT("the Level/World domain owns Actor candidates"),
			ActorCandidates->GetPublicOwner(),
			FString(TEXT("level-world-domain-owner")));
	}
	if (!TestTrue(TEXT("Level authoring registration succeeds"),
		UeAgentLevelAuthoring::RegisterAuthoring(Error)))
	{
		AddError(Error);
		return false;
	}
	UeAgentApplyInputContract::FContract ActorInputContract;
	if (TestTrue(TEXT("the Level Actor input contract is registered"),
		UeAgentApplyInputContract::FRegistry::Get().FindContract(
			TEXT("uai.apply_input.level.actor"), ActorInputContract)))
	{
		for (const FString& OptionalFieldName : {
			FString(TEXT("candidate")),
			FString(TEXT("placement_context"))})
		{
			const UeAgentApplyInputContract::FInputFieldDescriptor* OptionalField =
				ActorInputContract.FindInputField(OptionalFieldName);
			if (TestNotNull(*FString::Printf(TEXT("%s is declared"),
				*OptionalFieldName), OptionalField))
			{
				TestEqual(*FString::Printf(TEXT("%s remains optional for every operation"),
					*OptionalFieldName), OptionalField->RequiredFor.Num(), 0);
			}
		}
	}

	auto FindNativePlaceable = [this](
		const FString& SearchText,
		UeAgentPaletteSearch::FCandidate& OutCandidate) -> bool
	{
		UeAgentPaletteSearch::FSearchParams Params;
		Params.CatalogType = TEXT("placeable_actor");
		Params.SearchText = SearchText;
		Params.Limit = 20;
		UeAgentPaletteSearch::FSearchResult Result;
		FString SearchError;
		if (!UeAgentPaletteSearch::ExecuteSearch(Params, Result, SearchError))
		{
			AddError(SearchError);
			return false;
		}
		TArray<const UeAgentPaletteSearch::FCandidate*> Matches;
		for (const UeAgentPaletteSearch::FCandidate& Candidate : Result.MatchingCandidates)
		{
			if (Candidate.Name.Equals(SearchText, ESearchCase::IgnoreCase))
			{
				Matches.Add(&Candidate);
			}
		}
		if (Matches.Num() != 1)
		{
			AddError(FString::Printf(
				TEXT("native Place Actors candidate count invalid for %s: %d"),
				*SearchText,
				Matches.Num()));
			return false;
		}
		OutCandidate = *Matches[0];
		return true;
	};

	UeAgentPaletteSearch::FCandidate Cube;
	UeAgentPaletteSearch::FCandidate Sphere;
	{
		const FString BlueprintName = TEXT("BP_PlaceableActorCandidate_")
			+ FGuid::NewGuid().ToString(EGuidFormats::Digits);
		const FString PackageName = TEXT("/Game/UAI/StrictV2/T26/") + BlueprintName;
		const FString ObjectPath = PackageName + TEXT(".") + BlueprintName;
		DeleteAssetIfPresent(ObjectPath);
		UPackage* const Package = CreatePackage(*PackageName);
		UBlueprint* const Blueprint = Package
			? FKismetEditorUtilities::CreateBlueprint(
				AActor::StaticClass(), Package, *BlueprintName,
				BPTYPE_Normal, UBlueprint::StaticClass(),
				UBlueprintGeneratedClass::StaticClass(), NAME_None)
			: nullptr;
		if (!TestNotNull(TEXT("placeable Blueprint fixture exists"), Blueprint))
		{
			return false;
		}
		FAssetRegistryModule::AssetCreated(Blueprint);
		ON_SCOPE_EXIT
		{
			DeleteAssetIfPresent(ObjectPath);
		};

		UeAgentPaletteSearch::FCandidate BlueprintCandidate;
		if (TestTrue(TEXT("native Actor candidates expose a Content Browser Blueprint asset"),
			FindNativePlaceable(BlueprintName, BlueprintCandidate)))
		{
			TestEqual(TEXT("the Blueprint candidate retains executable asset identity"),
				BlueprintCandidate.AuthoringSelectors.FindRef(TEXT("asset_override")),
				ObjectPath);
			TestTrue(TEXT("the Blueprint candidate retains an Actor factory"),
				!BlueprintCandidate.AuthoringSelectors.FindRef(TEXT("actor_factory")).IsEmpty());
			TestTrue(TEXT("the Blueprint candidate resolves an Actor class"),
				!BlueprintCandidate.AuthoringSelectors.FindRef(TEXT("actor_class")).IsEmpty());
			TestTrue(TEXT("the Blueprint candidate is headlessly executable"),
				BlueprintCandidate.bSupportedInContext);

			UeAgentPaletteSearch::FCandidate ResolvedBlueprint;
			FString ResolveError;
			TestTrue(TEXT("the compact Blueprint id re-resolves through the same native catalog"),
				UeAgentPaletteSearch::TryResolvePlaceableActorCandidate(
					TEXT(""), BlueprintCandidate.GetPublicId(), TEXT("AllClasses"),
					ResolvedBlueprint, ResolveError));
			TestEqual(TEXT("id resolution preserves the Blueprint asset"),
				ResolvedBlueprint.AuthoringSelectors.FindRef(TEXT("asset_override")),
				ObjectPath);
		}
	}
	{
		UeAgentPaletteSearch::FSearchParams NoMatchParams;
		NoMatchParams.CatalogType = TEXT("placeable_actor");
		NoMatchParams.SearchText = TEXT("__UAI_T26_EXPECTED_NO_NATIVE_MATCH__");
		UeAgentPaletteSearch::FSearchResult NoMatchResult;
		FString NoMatchError;
		TestTrue(TEXT("a native search with no matches succeeds"),
			UeAgentPaletteSearch::ExecuteSearch(
				NoMatchParams, NoMatchResult, NoMatchError));
		TestEqual(TEXT("a native search with no matches returns an empty page"),
			NoMatchResult.MatchingCandidates.Num(), 0);
	}
	if (TestTrue(TEXT("native Place Actors exposes Cube"),
			FindNativePlaceable(TEXT("Cube"), Cube))
		&& TestTrue(TEXT("native Place Actors exposes Sphere"),
			FindNativePlaceable(TEXT("Sphere"), Sphere)))
	{
		TestEqual(TEXT("Basic shapes use the same native factory"),
			Cube.AuthoringSelectors.FindRef(TEXT("actor_factory")),
			Sphere.AuthoringSelectors.FindRef(TEXT("actor_factory")));
		TestEqual(TEXT("Basic shapes resolve the same Actor class"),
			Cube.AuthoringSelectors.FindRef(TEXT("actor_class")),
			Sphere.AuthoringSelectors.FindRef(TEXT("actor_class")));
		TestNotEqual(TEXT("Cube and Sphere retain distinct asset identity"),
			Cube.AuthoringSelectors.FindRef(TEXT("asset_override")),
			Sphere.AuthoringSelectors.FindRef(TEXT("asset_override")));
		TestNotEqual(TEXT("Cube and Sphere retain distinct compact ids"),
			Cube.GetPublicId(), Sphere.GetPublicId());
		UeAgentPaletteSearch::FCandidate ResolvedCube;
		FString ResolveError;
		TestTrue(TEXT("a minimal id re-resolves the same native Cube item"),
			UeAgentPaletteSearch::TryResolvePlaceableActorCandidate(
				TEXT(""), Cube.GetPublicId(), TEXT("AllClasses"),
				ResolvedCube, ResolveError));
		TestEqual(TEXT("id resolution preserves the Cube asset"),
			ResolvedCube.AuthoringSelectors.FindRef(TEXT("asset_override")),
			Cube.AuthoringSelectors.FindRef(TEXT("asset_override")));

		const FString PackageName = TEXT("/Game/UAI/StrictV2/T26/LV_Transform_")
			+ FGuid::NewGuid().ToString(EGuidFormats::Digits);
		FString AssetName;
		PackageName.Split(
			TEXT("/"), nullptr, &AssetName,
			ESearchCase::CaseSensitive, ESearchDir::FromEnd);
		const FString Target = PackageName + TEXT(".") + AssetName;
		DeleteAssetIfPresent(Target);
		ON_SCOPE_EXIT
		{
			DeleteAssetIfPresent(Target);
		};
		const FString ManagedId = TEXT("T85DocumentedTransform");
		const FString SecondManagedId = TEXT("T85DocumentedTransformSibling");
		const FString Document = FString::Printf(
			TEXT("{\"schema\":\"uai.world.edit/1\",\"root\":{\"type\":")
			TEXT("\"/Script/Engine.World\",\"document\":{\"operation\":\"create\",")
			TEXT("\"target\":{\"asset_path\":\"%s\"},\"provider_extension\":{")
			TEXT("\"frame\":{\"dimension\":3,\"semantic_frame\":\"ue_world\",")
			TEXT("\"write_frame\":\"ue_world\",\"read_frame\":\"ue_world\",")
			TEXT("\"unit\":\"centimeter\"},\"actors\":[{\"managed_id\":\"%s\",")
			TEXT("\"operation\":\"upsert\",\"candidate\":{\"id\":\"%s\"},")
			TEXT("\"transform\":{\"location\":{\"x\":125,\"y\":25,\"z\":50},")
			TEXT("\"rotation\":{\"pitch\":10,\"yaw\":20,\"roll\":30},")
			TEXT("\"scale\":{\"x\":1,\"y\":1,\"z\":1}}},{\"managed_id\":\"%s\",")
			TEXT("\"operation\":\"upsert\",\"candidate\":{\"id\":\"%s\"},")
			TEXT("\"transform\":{\"location\":{\"x\":-125,\"y\":25,\"z\":50},")
			TEXT("\"rotation\":{\"pitch\":0,\"yaw\":0,\"roll\":0},")
			TEXT("\"scale\":{\"x\":1,\"y\":1,\"z\":1}}}]}}}}"),
			*Target,
			*ManagedId,
			*Cube.GetPublicId(),
			*SecondManagedId,
			*Cube.GetPublicId());
		FString CanonicalParams;
		FString ParamsError;
		if (!TestTrue(TEXT("documented World transform fixture canonicalizes"),
			TryBuildResolvedWorldImportParams(
				Target, Document, CanonicalParams, ParamsError)))
		{
			AddError(ParamsError);
			return false;
		}
		FString ImportPayloadJson;
		bool bReadOnly = true;
		FWorkspaceError ImportError;
		if (!TestTrue(TEXT("documented World transform reaches native placement"),
			DomainAdapters::WorldSpatial::TryExecuteContentGraph(
				TEXT("content_graph.import_workspace"),
				Target,
				CanonicalParams,
				ImportPayloadJson,
				bReadOnly,
				ImportError)))
		{
			AddError(ImportError.GetCode() + TEXT(":")
				+ ImportError.GetSourceUnit());
			return false;
		}
		TestFalse(TEXT("World import is a mutation"), bReadOnly);
		TSharedPtr<FJsonObject> ImportPayload;
		FString PayloadError;
		if (!TestTrue(TEXT("World import payload is canonical JSON"),
			CommandContract::Json::TryParseObject(
				ImportPayloadJson, ImportPayload, PayloadError)))
		{
			AddError(PayloadError);
			return false;
		}
		FString TerminalStatus;
		TestTrue(TEXT("World import returns terminal status"),
			ImportPayload->TryGetStringField(
				TEXT("terminal_status"), TerminalStatus));
		TestEqual(TEXT("World import persists successfully"),
			TerminalStatus, FString(TEXT("succeeded")));
		const TSharedPtr<FJsonObject>* Readback = nullptr;
		const TSharedPtr<FJsonObject>* Snapshot = nullptr;
		if (!TestTrue(TEXT("World import returns independent readback"),
			ImportPayload->TryGetObjectField(TEXT("independent_readback"), Readback)
				&& Readback && Readback->IsValid()
				&& (*Readback)->TryGetObjectField(TEXT("query_result"), Snapshot)
				&& Snapshot && Snapshot->IsValid()))
		{
			return false;
		}
		const TSharedPtr<FJsonObject> CreatedActor =
			FindActorByManagedId(*Snapshot, ManagedId);
		const TSharedPtr<FJsonObject> SecondCreatedActor =
			FindActorByManagedId(*Snapshot, SecondManagedId);
		if (!TestNotNull(TEXT("native placement survives save and reload"),
			CreatedActor.Get()))
		{
			return false;
		}
		TestNotNull(TEXT("a sibling Actor with the same field schema survives"),
			SecondCreatedActor.Get());
		TestEqual(TEXT("native placement creates a StaticMeshActor"),
			CreatedActor->GetStringField(TEXT("type")),
			FString(TEXT("/Script/Engine.StaticMeshActor")));
		const TSharedPtr<FJsonObject>* Transform = nullptr;
		const TSharedPtr<FJsonObject>* Translation = nullptr;
		const TArray<TSharedPtr<FJsonValue>>* Components = nullptr;
		if (TestTrue(TEXT("documented location is normalized and read back"),
			CreatedActor->TryGetObjectField(TEXT("transform"), Transform)
				&& Transform && Transform->IsValid()
				&& (*Transform)->TryGetObjectField(TEXT("translation"), Translation)
				&& Translation && Translation->IsValid()
				&& (*Translation)->TryGetArrayField(TEXT("components"), Components)
				&& Components && Components->Num() == 3))
		{
			TestEqual(TEXT("location X survives"),
				(*Components)[0]->AsNumber(), 125.0);
			TestEqual(TEXT("location Y survives"),
				(*Components)[1]->AsNumber(), 25.0);
			TestEqual(TEXT("location Z survives"),
				(*Components)[2]->AsNumber(), 50.0);
		}

		const TSharedRef<FJsonObject> SchemaParams = MakeShared<FJsonObject>();
		SchemaParams->SetStringField(TEXT("target"), Target);
		FString SchemaParamsJson;
		if (!TestTrue(TEXT("World authoring-schema params canonicalize"),
			CommandContract::Json::TryCanonicalize(
				SchemaParams, SchemaParamsJson, PayloadError)))
		{
			AddError(PayloadError);
			return false;
		}
		FString SchemaPayloadJson;
		FWorkspaceError SchemaError;
		bReadOnly = false;
		if (!TestTrue(TEXT("World authoring schema is directly available"),
			DomainAdapters::WorldSpatial::TryExecuteContentGraph(
				TEXT("content_graph.authoring_schema"), Target,
				SchemaParamsJson, SchemaPayloadJson, bReadOnly, SchemaError)))
		{
			AddError(SchemaError.GetCode() + TEXT(":")
				+ SchemaError.GetSourceUnit());
			return false;
		}
		TestTrue(TEXT("World authoring schema is read-only"), bReadOnly);
		TSharedPtr<FJsonObject> SchemaPayload;
		const TSharedPtr<FJsonObject>* SchemaResult = nullptr;
		const TSharedPtr<FJsonObject>* Template = nullptr;
		const TSharedPtr<FJsonObject>* TemplateRoot = nullptr;
		const TSharedPtr<FJsonObject>* TemplateDocument = nullptr;
		const TSharedPtr<FJsonObject>* TemplateExtension = nullptr;
		const TArray<TSharedPtr<FJsonValue>>* TemplateActors = nullptr;
		if (TestTrue(TEXT("World authoring schema payload is canonical JSON"),
			CommandContract::Json::TryParseObject(
				SchemaPayloadJson, SchemaPayload, PayloadError))
			&& TestTrue(TEXT("World schema exposes a reusable query result"),
				SchemaPayload->TryGetObjectField(TEXT("query_result"), SchemaResult)
					&& SchemaResult && SchemaResult->IsValid()))
		{
			TestEqual(TEXT("World schema id is exact"),
				(*SchemaResult)->GetStringField(TEXT("schema_id")),
				FString(TEXT("uai.world.edit/1")));
			TestEqual(TEXT("World schema root type is exact"),
				(*SchemaResult)->GetStringField(TEXT("root_type")),
				FString(TEXT("/Script/Engine.World")));
			TestFalse(TEXT("World schema carries the current revision"),
				(*SchemaResult)->GetStringField(TEXT("revision")).IsEmpty());
			if (TestTrue(TEXT("World schema template has the import root"),
				(*SchemaResult)->TryGetObjectField(TEXT("template"), Template)
					&& Template && Template->IsValid()
					&& (*Template)->TryGetObjectField(TEXT("root"), TemplateRoot)
					&& TemplateRoot && TemplateRoot->IsValid()
					&& (*TemplateRoot)->TryGetObjectField(
						TEXT("document"), TemplateDocument)
					&& TemplateDocument && TemplateDocument->IsValid()
					&& (*TemplateDocument)->TryGetObjectField(
						TEXT("provider_extension"), TemplateExtension)
					&& TemplateExtension && TemplateExtension->IsValid()
					&& (*TemplateExtension)->TryGetArrayField(
						TEXT("actors"), TemplateActors)
					&& TemplateActors && TemplateActors->Num() == 1))
			{
				const TSharedPtr<FJsonObject> TemplateActor =
					(*TemplateActors)[0]->AsObject();
				const TSharedPtr<FJsonObject>* TemplateCandidate = nullptr;
				TestTrue(TEXT("minimal Actor upsert has managed_id plus candidate name"),
					TemplateActor.IsValid()
						&& TemplateActor->HasTypedField<EJson::String>(TEXT("managed_id"))
						&& TemplateActor->TryGetObjectField(
							TEXT("candidate"), TemplateCandidate)
						&& TemplateCandidate && TemplateCandidate->IsValid()
						&& (*TemplateCandidate)->HasTypedField<EJson::String>(TEXT("name")));
			}
		}

		const FString WorkspacePath = FPaths::ConvertRelativePathToFull(
			FPaths::ProjectSavedDir() / TEXT("Automation/UAI/T26/WorldExport_")
				+ FGuid::NewGuid().ToString(EGuidFormats::Digits));
		ON_SCOPE_EXIT
		{
			IFileManager::Get().DeleteDirectory(
				*WorkspacePath, false, true);
		};
		const TSharedRef<FJsonObject> ExportParams = MakeShared<FJsonObject>();
		ExportParams->SetStringField(TEXT("target"), Target);
		ExportParams->SetStringField(TEXT("workspace_path"), WorkspacePath);
		ExportParams->SetStringField(TEXT("profile_id"), TEXT("llm_authoring"));
		FString ExportParamsJson;
		if (!TestTrue(TEXT("World export params canonicalize"),
			CommandContract::Json::TryCanonicalize(
				ExportParams, ExportParamsJson, PayloadError)))
		{
			AddError(PayloadError);
			return false;
		}
		FString ExportPayloadJson;
		FWorkspaceError ExportError;
		bReadOnly = false;
		if (!TestTrue(TEXT("World export publishes its declared workspace"),
			DomainAdapters::WorldSpatial::TryExecuteContentGraph(
				TEXT("content_graph.export_workspace"), Target,
				ExportParamsJson, ExportPayloadJson, bReadOnly, ExportError)))
		{
			AddError(ExportError.GetCode() + TEXT(":")
				+ ExportError.GetSourceUnit() + TEXT(":")
				+ ExportError.GetJsonPointer());
			return false;
		}
		TestTrue(TEXT("World export is a read-only publication"), bReadOnly);
		HjsonWorkspace::FTargetCompositionContract LevelComposition;
		TestTrue(TEXT("the shared exporter owns the Level composition prerequisite"),
			HjsonWorkspace::FTargetCompositionRegistry::Get().FindContract(
				TEXT("ue.level.asset"), LevelComposition));
		TSharedPtr<FJsonObject> ExportPayload;
		if (TestTrue(TEXT("World export receipt is canonical JSON"),
			CommandContract::Json::TryParseObject(
				ExportPayloadJson, ExportPayload, PayloadError)))
		{
			TestEqual(TEXT("World export reports published"),
				ExportPayload->GetStringField(TEXT("status")),
				FString(TEXT("published")));
			const FString PublishedWorkspace =
				ExportPayload->GetStringField(TEXT("workspace_path"));
			TestTrue(TEXT("World export reports the declared workspace"),
				FPaths::IsSamePath(PublishedWorkspace, WorkspacePath));
			TestTrue(TEXT("World export entry file exists"),
				IFileManager::Get().FileExists(
					*(PublishedWorkspace
						/ ExportPayload->GetStringField(TEXT("entry_file")))));
		}
	}
	return !HasAnyErrors();
#endif
}
