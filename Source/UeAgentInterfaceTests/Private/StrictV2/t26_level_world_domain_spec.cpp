#include "Misc/AutomationTest.h"
#include "Core/Contract/UeAgentDiagnostic.h"

#include "AssetRegistry/AssetRegistryModule.h"
#include "AssetRegistry/IAssetRegistry.h"
#include "Core/Contract/UeAgentCommandJson.h"
#include "Core/HjsonWorkspace/ImportInitialization/UeAgentHjsonDomainProviderRegistry.h"
#include "Core/HjsonWorkspace/UeAgentAuthoringComposition.h"
#include "Core/ContentDiscovery/PaletteSearch/UeAgentPaletteSearchTypes.h"
#include "DomainAdapters/WorldSpatial/UeAgentLevelWorldDomainMechanics.h"
#include "Domains/Level/Authoring/UeAgentLevelAuthoringRegistration.h"
#include "Domains/Level/Authoring/UeAgentLevelAuthoringState.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Engine/Blueprint.h"
#include "Engine/BlueprintGeneratedClass.h"
#include "EngineUtils.h"
#include "GameFramework/Actor.h"
#include "HAL/FileManager.h"
#include "Kismet2/KismetEditorUtilities.h"
#include "Misc/PackageName.h"
#include "Misc/Paths.h"
#include "Misc/ScopeExit.h"
#include "Module/CommandRegistration/Active/UeAgentActiveCommandComposition.h"
#include "ObjectTools.h"
#include "Utilities/AuthoringOperationApply/UeAgentApplyInputContract.h"
#include "UObject/SavePackage.h"
#include "UObject/AssetRegistryTagsContext.h"

#include "OperationOwners/WorldSpatial/UeAgentLevelWorldDomainService.h"

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
		UE::UAI::Contract::FDiagnostic& OutError)
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

}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUeAgentStrictV2T26LevelWorldDomainTest,
	"UAI.StrictV2.T26.LEVEL.WORLD.DOMAIN",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FUeAgentStrictV2T26LevelWorldDomainTest::RunTest(
	const FString& Parameters)
{
	(void)Parameters;
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
	UE::UAI::Contract::FDiagnostic Error;
	if (!TestTrue(TEXT("the process active composition freezes"),
		Module::CommandRegistration::Active::
			TryComposeProcessActiveCommandComposition(Registry, Dispatch, Error))
		|| !Registry.IsValid())
	{
		AddError(Error.ToPublicDetail());
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
			FString(TEXT("content-graph-workspace-owner")));
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
		TestFalse(TEXT("the public tutorial does not revive nested candidate syntax"),
			ActorCandidates->GetPurpose().ParameterGuidance.Contains(
				TEXT("actors[].candidate")));
	}
	if (!TestTrue(TEXT("Level authoring registration succeeds"),
		UeAgentLevelAuthoring::RegisterAuthoring(Error)))
	{
		AddError(Error.ToPublicDetail());
		return false;
	}
	UeAgentApplyInputContract::FContract ActorInputContract;
	if (TestTrue(TEXT("the Level Actor input contract is registered"),
		UeAgentApplyInputContract::FRegistry::Get().FindContract(
			TEXT("uai.apply_input.level.actor"), ActorInputContract)))
	{
		for (const FString& OptionalFieldName : {
			FString(TEXT("actor_class")),
			FString(TEXT("candidate")),
			FString(TEXT("placement_context")),
			FString(TEXT("tags"))})
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

		const TSharedRef<FJsonObject> Candidate = MakeShared<FJsonObject>();
		Candidate->SetStringField(TEXT("name"), TEXT("TargetPoint"));
		const TSharedRef<FJsonObject> ProviderExtension = MakeShared<FJsonObject>();
		ProviderExtension->SetObjectField(TEXT("candidate"), Candidate);
		const TSharedRef<FJsonObject> Target = MakeShared<FJsonObject>();
		Target->SetStringField(
			TEXT("world_path"), TEXT("/Game/UAI/StrictV2/T26/LV_T26.LV_T26"));
		Target->SetStringField(TEXT("managed_id"), TEXT("candidate-target-point"));
		const TSharedRef<FJsonObject> Document = MakeShared<FJsonObject>();
		Document->SetStringField(TEXT("operation"), TEXT("create"));
		Document->SetObjectField(TEXT("target"), Target);
		Document->SetObjectField(TEXT("provider_extension"), ProviderExtension);

		UeAgentAuthoringWorkspaceRouter::FWorkspaceUnit CandidateCreateUnit;
		CandidateCreateUnit.ApplyContractRef = ActorInputContract.ContractId;
		CandidateCreateUnit.Action =
			UeAgentAuthoringWorkspaceRouter::EUnitAction::Apply;
		CandidateCreateUnit.Document = Document;
		UeAgentApplyInputContract::FOperationInput CandidateCreateInput;
		UeAgentApplyInputContract::FParseFailure CandidateCreateFailure;
		TestTrue(TEXT("candidate-backed Actor create passes without actor_class"),
			UeAgentApplyInputContract::ParseOperationInput(
				ActorInputContract, CandidateCreateUnit, CandidateCreateInput,
				CandidateCreateFailure));
		TestTrue(TEXT("candidate-backed create preserves the candidate extension"),
			CandidateCreateInput.ProviderExtension.IsValid()
				&& CandidateCreateInput.ProviderExtension->HasField(TEXT("candidate")));
		TestTrue(TEXT("candidate-backed create does not synthesize actor_class"),
			CandidateCreateInput.Target.IsValid()
				&& !CandidateCreateInput.Target->HasField(TEXT("actor_class")));
	}

	auto FindNativePlaceable = [this](
		const FString& SearchText,
		UeAgentPaletteSearch::FCandidate& OutCandidate) -> bool
	{
		UeAgentPaletteSearch::FSearchParams Params;
		Params.CatalogType = TEXT("placeable_actor");
		Params.SearchText = SearchText;
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
	auto FindPublicPlaceableAlias = [this](
		const FString& SearchText,
		FString& OutAlias) -> bool
	{
		OutAlias.Reset();
		const FString QueryTarget = GWorld
			? GWorld->GetOutermost()->GetName() + TEXT(".") + GWorld->GetName()
			: FString(TEXT("/Engine/Transient.World"));
		UE::UAI::DomainAdapters::WorldSpatial::FUeAgentWorldActorCandidateQuery Query;
		Query.Target = QueryTarget;
		Query.Query = SearchText;
		FString Payload;
		UE::UAI::HjsonWorkspace::FWorkspaceError QueryError;
		if (!UE::UAI::DomainAdapters::WorldSpatial::TryQueryWorldActorCandidates(
				Query, Payload, QueryError))
		{
			AddError(QueryError.GetCode() + TEXT(":") + QueryError.GetJsonPointer());
			return false;
		}
		TSharedPtr<FJsonObject> Object;
		FString JsonError;
		if (!UE::UAI::CommandContract::Json::TryParseObject(Payload, Object, JsonError)
			|| !Object.IsValid())
		{
			AddError(JsonError);
			return false;
		}
		const TArray<TSharedPtr<FJsonValue>>* Names = nullptr;
		if (!Object->TryGetArrayField(TEXT("names"), Names) || !Names)
		{
			AddError(TEXT("world.actor.candidates did not return names"));
			return false;
		}
		for (const TSharedPtr<FJsonValue>& Value : *Names)
		{
			if (Value.IsValid() && Value->Type == EJson::String
				&& Value->AsString().Equals(SearchText, ESearchCase::IgnoreCase))
			{
				OutAlias = Value->AsString();
				return true;
			}
		}
		for (const TSharedPtr<FJsonValue>& Value : *Names)
		{
			if (Value.IsValid() && Value->Type == EJson::String
				&& Value->AsString().StartsWith(
					SearchText + TEXT(" ["), ESearchCase::IgnoreCase))
			{
				OutAlias = Value->AsString();
				return true;
			}
		}
		AddError(TEXT("world.actor.candidates returned no matching public alias"));
		return false;
	};
	auto PersistBlueprintFixture = [this](
		UPackage* Package,
		UBlueprint* Blueprint,
		const FString& PackageName) -> bool
	{
		FKismetEditorUtilities::CompileBlueprint(Blueprint);
		if (!TestTrue(TEXT("placeable Blueprint fixture compiles"),
			Blueprint->Status != BS_Error))
		{
			return false;
		}
		FAssetRegistryModule::AssetCreated(Blueprint);
		IAssetRegistry::GetChecked().AssetUpdateTags(
			Blueprint, EAssetRegistryTagsCaller::FullUpdate);
		Package->MarkPackageDirty();
		FSavePackageArgs SaveArgs;
		SaveArgs.TopLevelFlags = RF_Public | RF_Standalone;
		const FString Filename = FPackageName::LongPackageNameToFilename(
			PackageName, FPackageName::GetAssetPackageExtension());
		const bool bSaved = UPackage::SavePackage(
			Package, Blueprint, *Filename, SaveArgs);
		TestTrue(TEXT("placeable Blueprint fixture is persisted for discovery"), bSaved);
		if (bSaved)
		{
			Package->SetDirtyFlag(false);
		}
		return bSaved;
	};

	UeAgentPaletteSearch::FCandidate Cube;
	UeAgentPaletteSearch::FCandidate Sphere;
	{
		const FString BlueprintName = TEXT("BP_PlaceableActorCandidateFixture");
		const FString BlueprintPublicName =
			TEXT("BP Placeable Actor Candidate Fixture");
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
		if (!PersistBlueprintFixture(Package, Blueprint, PackageName))
		{
			DeleteAssetIfPresent(ObjectPath);
			return false;
		}
		ON_SCOPE_EXIT
		{
			DeleteAssetIfPresent(ObjectPath);
		};

		UeAgentPaletteSearch::FCandidate BlueprintCandidate;
		FString BlueprintAlias;
		if (TestTrue(TEXT("native Actor candidates expose a Content Browser Blueprint asset"),
			FindNativePlaceable(BlueprintPublicName, BlueprintCandidate)))
		{
			TestTrue(TEXT("world.actor.candidates returns the Blueprint public alias"),
				FindPublicPlaceableAlias(BlueprintPublicName, BlueprintAlias));
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
			TestTrue(TEXT("the Blueprint name re-resolves through the same native catalog"),
				UeAgentPaletteSearch::TryResolvePlaceableActorCandidate(
					BlueprintAlias, TEXT(""), TEXT("AllClasses"),
					ResolvedBlueprint, ResolveError));
			TestEqual(TEXT("name resolution preserves the Blueprint asset"),
				ResolvedBlueprint.AuthoringSelectors.FindRef(TEXT("asset_override")),
				ObjectPath);
		}
	}
	{
		// Two assets with the same leaf name must remain distinguishable through
		// the compact public world query and its shared resolver.
		const FString BlueprintName = TEXT("BP_PlaceableActorScopeFixture");
		const FString BlueprintPublicName =
			TEXT("BP Placeable Actor Scope Fixture");
		const FString PackageA = TEXT("/Game/UAI/StrictV2/T26/ScopeA/") + BlueprintName;
		const FString PackageB = TEXT("/Game/UAI/StrictV2/T26/ScopeB/") + BlueprintName;
		const FString ObjectPathA = PackageA + TEXT(".") + BlueprintName;
		const FString ObjectPathB = PackageB + TEXT(".") + BlueprintName;
		DeleteAssetIfPresent(ObjectPathA);
		DeleteAssetIfPresent(ObjectPathB);
		UPackage* const AssetPackageA = CreatePackage(*PackageA);
		UPackage* const AssetPackageB = CreatePackage(*PackageB);
		UBlueprint* const BlueprintA = AssetPackageA
			? FKismetEditorUtilities::CreateBlueprint(
				AActor::StaticClass(), AssetPackageA, *BlueprintName,
				BPTYPE_Normal, UBlueprint::StaticClass(),
				UBlueprintGeneratedClass::StaticClass(), NAME_None)
			: nullptr;
		UBlueprint* const BlueprintB = AssetPackageB
			? FKismetEditorUtilities::CreateBlueprint(
				AActor::StaticClass(), AssetPackageB, *BlueprintName,
				BPTYPE_Normal, UBlueprint::StaticClass(),
				UBlueprintGeneratedClass::StaticClass(), NAME_None)
			: nullptr;
		if (!TestNotNull(TEXT("first scoped Blueprint fixture exists"), BlueprintA)
			|| !TestNotNull(TEXT("second scoped Blueprint fixture exists"), BlueprintB))
		{
			DeleteAssetIfPresent(ObjectPathA);
			DeleteAssetIfPresent(ObjectPathB);
			return false;
		}
		if (!PersistBlueprintFixture(AssetPackageA, BlueprintA, PackageA)
			|| !PersistBlueprintFixture(AssetPackageB, BlueprintB, PackageB))
		{
			DeleteAssetIfPresent(ObjectPathA);
			DeleteAssetIfPresent(ObjectPathB);
			return false;
		}
		ON_SCOPE_EXIT
		{
			DeleteAssetIfPresent(ObjectPathA);
			DeleteAssetIfPresent(ObjectPathB);
		};

		const FString QueryTarget = GWorld
			? GWorld->GetOutermost()->GetName() + TEXT(".") + GWorld->GetName()
			: FString(TEXT("/Engine/Transient.World"));
		UE::UAI::DomainAdapters::WorldSpatial::FUeAgentWorldActorCandidateQuery Query;
		Query.Target = QueryTarget;
		Query.Query = BlueprintPublicName;
		FString Payload;
		UE::UAI::HjsonWorkspace::FWorkspaceError QueryError;
		if (!TestTrue(TEXT("scoped duplicate actor query succeeds"),
			UE::UAI::DomainAdapters::WorldSpatial::TryQueryWorldActorCandidates(
				Query, Payload, QueryError)))
		{
			AddError(QueryError.GetCode() + TEXT(":") + QueryError.GetJsonPointer());
			return false;
		}
		TSharedPtr<FJsonObject> Object;
		FString JsonError;
		if (!TestTrue(TEXT("scoped duplicate actor query is valid JSON"),
			UE::UAI::CommandContract::Json::TryParseObject(Payload, Object, JsonError))
			|| !Object.IsValid())
		{
			AddError(JsonError);
			return false;
		}
		const TArray<TSharedPtr<FJsonValue>>* Names = nullptr;
		if (!TestTrue(TEXT("scoped duplicate actor query returns names"),
			Object->TryGetArrayField(TEXT("names"), Names)) || !Names)
		{
			return false;
		}
		TArray<FString> ScopedAliases;
		for (const TSharedPtr<FJsonValue>& Value : *Names)
		{
			if (Value.IsValid() && Value->Type == EJson::String
				&& Value->AsString().StartsWith(
					BlueprintPublicName + TEXT(" ["), ESearchCase::IgnoreCase))
			{
				ScopedAliases.Add(Value->AsString());
			}
		}
		TestEqual(TEXT("same-name assets return two scoped aliases"), ScopedAliases.Num(), 2);
		if (ScopedAliases.Num() == 2)
		{
			TestNotEqual(TEXT("scoped aliases are distinct"), ScopedAliases[0], ScopedAliases[1]);
			TArray<FString> ResolvedAssets;
			for (const FString& Alias : ScopedAliases)
			{
				UeAgentPaletteSearch::FCandidate Resolved;
				FString ResolveError;
				TestTrue(TEXT("each scoped alias resolves"),
					UeAgentPaletteSearch::TryResolvePlaceableActorCandidate(
						Alias, TEXT(""), TEXT("AllClasses"), Resolved, ResolveError));
				const FString ResolvedAsset =
					Resolved.AuthoringSelectors.FindRef(TEXT("asset_override"));
				TestTrue(TEXT("scoped alias resolves to one fixture asset"),
					ResolvedAsset.Equals(ObjectPathA, ESearchCase::IgnoreCase)
						|| ResolvedAsset.Equals(ObjectPathB, ESearchCase::IgnoreCase));
				ResolvedAssets.Add(ResolvedAsset);
			}
			if (ResolvedAssets.Num() == 2)
			{
				TestTrue(TEXT("scoped aliases resolve to different fixture assets"),
					!ResolvedAssets[0].Equals(ResolvedAssets[1], ESearchCase::IgnoreCase));
				TestTrue(TEXT("both scoped fixture assets are addressable"),
					(ResolvedAssets[0].Equals(ObjectPathA, ESearchCase::IgnoreCase)
						&& ResolvedAssets[1].Equals(ObjectPathB, ESearchCase::IgnoreCase))
						|| (ResolvedAssets[0].Equals(ObjectPathB, ESearchCase::IgnoreCase)
							&& ResolvedAssets[1].Equals(ObjectPathA, ESearchCase::IgnoreCase)));
			}
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
		FString CubeAlias;
		FString SphereAlias;
		TestTrue(TEXT("world.actor.candidates returns the Cube public alias"),
			FindPublicPlaceableAlias(TEXT("Cube"), CubeAlias));
		TestTrue(TEXT("world.actor.candidates returns the Sphere public alias"),
			FindPublicPlaceableAlias(TEXT("Sphere"), SphereAlias));
		TestEqual(TEXT("Basic shapes use the same native factory"),
			Cube.AuthoringSelectors.FindRef(TEXT("actor_factory")),
			Sphere.AuthoringSelectors.FindRef(TEXT("actor_factory")));
		TestEqual(TEXT("Basic shapes resolve the same Actor class"),
			Cube.AuthoringSelectors.FindRef(TEXT("actor_class")),
			Sphere.AuthoringSelectors.FindRef(TEXT("actor_class")));
		TestNotEqual(TEXT("Cube and Sphere retain distinct asset identity"),
			Cube.AuthoringSelectors.FindRef(TEXT("asset_override")),
			Sphere.AuthoringSelectors.FindRef(TEXT("asset_override")));
		UeAgentPaletteSearch::FCandidate ResolvedCube;
		FString ResolveError;
		TestTrue(TEXT("a minimal name re-resolves the same native Cube item"),
			UeAgentPaletteSearch::TryResolvePlaceableActorCandidate(
				CubeAlias, TEXT(""), TEXT("AllClasses"),
				ResolvedCube, ResolveError));
		TestEqual(TEXT("name resolution preserves the Cube asset"),
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
			TEXT("\"operation\":\"upsert\",\"candidate\":{\"name\":\"%s\"},")
			TEXT("\"transform\":{\"location\":{\"x\":125,\"y\":25,\"z\":50},")
			TEXT("\"rotation\":{\"pitch\":10,\"yaw\":20,\"roll\":30},")
			TEXT("\"scale\":{\"x\":1,\"y\":1,\"z\":1}}},{\"managed_id\":\"%s\",")
			TEXT("\"operation\":\"upsert\",\"candidate\":{\"name\":\"%s\"},")
			TEXT("\"transform\":{\"location\":{\"x\":-125,\"y\":25,\"z\":50},")
			TEXT("\"rotation\":{\"pitch\":0,\"yaw\":0,\"roll\":0},")
			TEXT("\"scale\":{\"x\":1,\"y\":1,\"z\":1}}}]}}}}"),
			*Target,
			*ManagedId,
			*CubeAlias,
			*SecondManagedId,
			*CubeAlias);
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
				ImportError,
				TEXT("req-t26-world-transform-import"))))
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
		TestEqual(TEXT("World import returns one public status field"),
			ImportPayload->Values.Num(), 1);
		TestEqual(TEXT("World import persists successfully"),
			ImportPayload->GetStringField(TEXT("status")),
			FString(TEXT("succeeded")));
		TestFalse(TEXT("World import omits provider identity"),
			ImportPayload->HasField(TEXT("provider_id")));
		TestFalse(TEXT("World import keeps its target in the request and state"),
			ImportPayload->HasField(TEXT("target")));
		TestFalse(TEXT("World import keeps its root type in the state"),
			ImportPayload->HasField(TEXT("root_type")));
		TestFalse(TEXT("World import keeps save state internal"),
			ImportPayload->HasField(TEXT("saved")));
		TestFalse(TEXT("World import keeps reload state internal"),
			ImportPayload->HasField(TEXT("reloaded")));
		TestFalse(TEXT("World import keeps the space contract out of success"),
			ImportPayload->HasField(TEXT("space_contract")));
		TestFalse(TEXT("World import omits apply diagnostics"),
			ImportPayload->HasField(TEXT("apply_result")));
		TestFalse(TEXT("World import omits independent readback payload"),
			ImportPayload->HasField(TEXT("independent_readback")));

		UWorld* const ReloadedWorld = LoadObject<UWorld>(nullptr, *Target);
		if (!TestNotNull(TEXT("persisted World independently reloads"), ReloadedWorld))
		{
			return false;
		}
		AActor* CreatedActor = nullptr;
		AActor* SecondCreatedActor = nullptr;
		for (TActorIterator<AActor> It(ReloadedWorld); It; ++It)
		{
			AActor* Actor = *It;
			if (!Actor) continue;
			const FString ActualId = UeAgentLevelAuthoring::GetManagedActorId(*Actor);
			if (ActualId == ManagedId) CreatedActor = Actor;
			if (ActualId == SecondManagedId) SecondCreatedActor = Actor;
		}
		if (!TestNotNull(TEXT("native placement survives save and reload"), CreatedActor))
		{
			return false;
		}
		TestNotNull(TEXT("a sibling Actor with the same field schema survives"),
			SecondCreatedActor);
		TestEqual(TEXT("native placement creates a StaticMeshActor"),
			CreatedActor->GetClass()->GetPathName(),
			FString(TEXT("/Script/Engine.StaticMeshActor")));
		const FVector Location = CreatedActor->GetActorLocation();
		TestEqual(TEXT("location X survives"), Location.X, 125.0);
		TestEqual(TEXT("location Y survives"), Location.Y, 25.0);
		TestEqual(TEXT("location Z survives"), Location.Z, 50.0);

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
			TestEqual(TEXT("World export reports one public field"),
				ExportPayload->Values.Num(), 1);
			TestEqual(TEXT("World export reports success"),
				ExportPayload->GetStringField(TEXT("status")),
				FString(TEXT("succeeded")));
			TestTrue(TEXT("World export publishes the declared workspace"),
				IFileManager::Get().DirectoryExists(*WorkspacePath));
			TArray<FString> PublishedHjsonFiles;
			IFileManager::Get().FindFilesRecursive(
				PublishedHjsonFiles,
				*WorkspacePath,
				TEXT("*.hjson"),
				true,
				false);
			TestTrue(TEXT("World export publishes visible HJSON files"),
				!PublishedHjsonFiles.IsEmpty());
			TestFalse(TEXT("World export does not repeat the workspace path"),
				ExportPayload->HasField(TEXT("workspace_path")));
			TestFalse(TEXT("World export keeps file counts internal"),
				ExportPayload->HasField(TEXT("file_count")));
			TestFalse(TEXT("World export omits absolute written path lists"),
				ExportPayload->HasField(TEXT("written_files")));
			TestFalse(TEXT("World export omits relative written path lists"),
				ExportPayload->HasField(TEXT("written_relative_files")));
		}
	}
	return !HasAnyErrors();
}
