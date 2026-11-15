// Copyright Epic Games, Inc. All Rights Reserved.

#include "Core/ContentDiscovery/PaletteSearch/UeAgentPaletteSearchTypes.h"
#include "Core/ContentDiscovery/UeAgentNativeNodeAction.h"

#include "Core/ContentDiscovery/PaletteSearch/UeAgentPaletteSearchNaming.h"
#include "Core/ContentDiscovery/UeAgentNameDiscoveryProjection.h"
#include "Core/Contract/UeAgentCommandJson.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Delegates/Delegate.h"
#include "DomainAdapters/BlueprintCommands/UeAgentBlueprintCommandMechanicsBoundary.h"
#include "Domains/Blueprint/Authoring/UeAgentBlueprintAssetLifecycle.h"
#include "Utilities/AuthoringCore/UeAgentAuthoringDiagnostics.h"
#include "Utilities/AuthoringCore/UeAgentAuthoringAssetIdentity.h"
#include "Utilities/UeAgentBlueprintInputBindings.h"
#include "AssetRegistry/AssetRegistryModule.h"
#include "AssetRegistry/AssetData.h"
#include "AssetRegistry/IAssetRegistry.h"
#include "BlueprintNamespaceHelper.h"
#include "BlueprintActionDatabase.h"
#include "BlueprintActionFilter.h"
#include "BlueprintActionMenuBuilder.h"
#include "BlueprintActionMenuItem.h"
#include "BlueprintActionMenuUtils.h"
#include "BlueprintAssetNodeSpawner.h"
#include "BlueprintEventNodeSpawner.h"
#include "BlueprintFieldNodeSpawner.h"
#include "BlueprintFunctionNodeSpawner.h"
#include "BlueprintNodeSpawner.h"
#include "BlueprintVariableNodeSpawner.h"
#include "EdGraph/EdGraph.h"
#include "EdGraph/EdGraphNode.h"
#include "EdGraph/EdGraphPin.h"
#include "EdGraphSchema_K2.h"
#include "EdGraphSchema_K2_Actions.h"
#include "Engine/Blueprint.h"
#include "Engine/BlueprintGeneratedClass.h"
#include "GameFramework/Actor.h"
#include "HAL/IConsoleManager.h"
#include "K2Node.h"
#include "K2Node_MacroInstance.h"
#include "K2Node_PromotableOperator.h"
#include "Kismet2/BlueprintEditorUtils.h"
#include "Kismet2/KismetEditorUtilities.h"
#include "ScopedTransaction.h"
#include "Misc/SecureHash.h"
#include "Misc/PackageName.h"
#include "Misc/Paths.h"
#include "Misc/ScopeExit.h"
#include "Modules/ModuleManager.h"
#include "UObject/StrongObjectPtr.h"
#include "UObject/SoftObjectPath.h"
#include "UObject/Package.h"
#include "UObject/UnrealType.h"
#include "UObject/WeakObjectPtrTemplates.h"

namespace UeAgentPaletteSearch
{
namespace
{
	static FString NormalizeObjectPath(const FString& InPath)
	{
		FString ObjectPath = InPath.TrimStartAndEnd();
		if (ObjectPath.IsEmpty() || ObjectPath.Contains(TEXT(".")))
		{
			return ObjectPath;
		}

		const FString AssetName = FPackageName::GetLongPackageAssetName(ObjectPath);
		if (AssetName.IsEmpty())
		{
			return FString();
		}
		return FString::Printf(TEXT("%s.%s"), *ObjectPath, *AssetName);
	}

	static UObject* LoadObjectFromPath(const FString& InPath)
	{
		const FString ObjectPath = NormalizeObjectPath(InPath);
		if (ObjectPath.IsEmpty())
		{
			return nullptr;
		}
		if (UObject* Existing = FindObject<UObject>(nullptr, *ObjectPath))
		{
			return Existing;
		}
		return LoadObject<UObject>(nullptr, *ObjectPath);
	}

	static UBlueprint* LoadBlueprint(const FString& AssetPath)
	{
		return Cast<UBlueprint>(LoadObjectFromPath(AssetPath));
	}

	static void EnsureBlueprintActionRegistration(UBlueprint& Blueprint)
	{
		// Loading a resident object does not emit OnAssetLoaded. Native unload
		// notifications can therefore leave a valid query context unregistered.
		// Keep PIE/diff and scratch contexts outside persistent database tracking.
		if (Blueprint.GetOutermost()->HasAnyPackageFlags(PKG_PlayInEditor | PKG_ForDiffing)) return;
		const UObject* AssetOwner = &Blueprint;
		while (AssetOwner && !AssetOwner->IsAsset()) AssetOwner = AssetOwner->GetOuter();
		if (!AssetOwner) return;

		FBlueprintActionDatabase& Database = FBlueprintActionDatabase::Get();
		if (Database.GetAllActions().Contains(FObjectKey(&Blueprint))
			&& Blueprint.OnChanged().IsBoundToObject(&Database)
			&& Blueprint.OnCompiled().IsBoundToObject(&Database)) return;
		Database.RefreshAssetActions(&Blueprint);
	}

	class FWorkingBlueprintActionScope final
	{
	public:
		explicit FWorkingBlueprintActionScope(UBlueprint& InBlueprint)
			: Blueprint(&InBlueprint)
		{
			// UE only builds asset-owned member actions for asset-eligible objects.
			// Borrow that eligibility solely during synchronous menu registration;
			// the validation copy stays transient during mutation, dependency scans
			// and persistence. Never publish it to the asset registry.
			const EObjectFlags EligibilityFlags = RF_Public | RF_Transient;
			const EObjectFlags OriginalFlags = InBlueprint.GetFlags() & EligibilityFlags;
            UPackage* WorkingPackage = Cast<UPackage>(InBlueprint.GetOuter());
            const bool bRestoreTransientPackage = WorkingPackage && WorkingPackage != GetTransientPackage()
                && WorkingPackage->HasAnyFlags(RF_Transient);
            ON_SCOPE_EXIT
            {
                InBlueprint.ClearFlags(EligibilityFlags);
                InBlueprint.SetFlags(OriginalFlags);
                if (bRestoreTransientPackage) WorkingPackage->SetFlags(RF_Transient);
            };
			InBlueprint.ClearFlags(RF_Transient);
			InBlueprint.SetFlags(RF_Public);
            // UObject::IsAsset also rejects a transient outer package. A private
            // validation package must borrow eligibility for this synchronous
            // registration only; never make the global transient package eligible.
            if (bRestoreTransientPackage) WorkingPackage->ClearFlags(RF_Transient);
			FBlueprintActionDatabase::Get().RefreshAssetActions(&InBlueprint);
		}

		~FWorkingBlueprintActionScope()
		{
			FBlueprintActionDatabase& Database = FBlueprintActionDatabase::Get();
			Blueprint->OnChanged().RemoveAll(&Database);
			Blueprint->OnCompiled().RemoveAll(&Database);
			Database.ClearAssetActions(Blueprint.Get());
		}

	private:
		TStrongObjectPtr<UBlueprint> Blueprint;
	};

	bool IsAuthoringSelfClass(const UBlueprint* Blueprint, const UClass* Class)
	{
		if (!Blueprint || !Class) return false;
		const FString Owner = UeAgentBlueprintAssetLifecycle::GetAuthoringClassPath(*Blueprint, *Class);
		for (const UClass* SelfClass : {Blueprint->SkeletonGeneratedClass, Blueprint->GeneratedClass})
		{
			if (SelfClass && Owner == UeAgentBlueprintAssetLifecycle::GetAuthoringClassPath(*Blueprint, *SelfClass))
				return true;
		}
		return false;
	}

	FString GetAuthoringOwnerPath(const UBlueprint* Blueprint, const UClass& Class)
	{
		return Blueprint ? UeAgentBlueprintAssetLifecycle::GetAuthoringClassPath(*Blueprint, Class) : Class.GetPathName();
	}

	const UClass* GetActionOwnerClass(const TSharedPtr<FEdGraphSchemaAction>& Action)
	{
		if (!Action.IsValid() || Action->GetTypeId() != FBlueprintActionMenuItem::StaticGetTypeId()) return nullptr;
		const UBlueprintNodeSpawner* Spawner = StaticCastSharedPtr<FBlueprintActionMenuItem>(Action)->GetRawAction();
		if (const auto* Function = Cast<UBlueprintFunctionNodeSpawner>(Spawner))
			return Function->GetFunction() ? Function->GetFunction()->GetOwnerClass() : nullptr;
		if (const auto* Event = Cast<UBlueprintEventNodeSpawner>(Spawner))
			return Event->GetEventFunction() ? Event->GetEventFunction()->GetOwnerClass() : nullptr;
		if (const auto* Variable = Cast<UBlueprintVariableNodeSpawner>(Spawner))
			return Variable->GetVarProperty() ? Variable->GetVarProperty()->GetOwnerClass() : nullptr;
		if (const auto* Field = Cast<UBlueprintFieldNodeSpawner>(Spawner))
			return Field->GetField().GetOwnerClass();
		return nullptr;
	}

	UBlueprint* GetActionOwnerBlueprint(const TSharedPtr<FEdGraphSchemaAction>& Action, UEdGraph* Graph)
	{
		if (const UClass* OwnerClass = GetActionOwnerClass(Action))
			return Cast<UBlueprint>(OwnerClass->ClassGeneratedBy);
		if (!Action.IsValid() || Action->GetTypeId() != FBlueprintActionMenuItem::StaticGetTypeId()) return nullptr;
		const auto* Spawner = StaticCastSharedPtr<FBlueprintActionMenuItem>(Action)->GetRawAction();
		if (Spawner && Spawner->NodeClass && Spawner->NodeClass->IsChildOf(UK2Node_MacroInstance::StaticClass()))
			if (const auto* Macro = Cast<UK2Node_MacroInstance>(Spawner->GetTemplateNode(Graph)))
				if (const UEdGraph* MacroGraph = Macro->GetMacroGraph()) return MacroGraph->GetTypedOuter<UBlueprint>();
		return nullptr;
	}

	static UEdGraph* ResolveGraph(UBlueprint* Blueprint, const FString& GraphName)
	{
		if (!Blueprint)
		{
			return nullptr;
		}

		const FString CleanName = GraphName.TrimStartAndEnd();
		auto MatchesGraph = [&CleanName](UEdGraph* Graph)
		{
			if (!Graph)
			{
				return false;
			}
			const FString GuidIdentity = Graph->GraphGuid.IsValid()
				? TEXT("graph:") + Graph->GraphGuid.ToString(
					EGuidFormats::DigitsWithHyphensLower)
				: FString();
			return CleanName.IsEmpty()
				|| Graph->GetName().Equals(CleanName, ESearchCase::IgnoreCase)
				|| Graph->GetPathName().Equals(CleanName, ESearchCase::IgnoreCase)
				|| (!GuidIdentity.IsEmpty()
					&& GuidIdentity.Equals(CleanName, ESearchCase::IgnoreCase))
				|| (TEXT("graph:name:") + Graph->GetName()).Equals(
					CleanName, ESearchCase::IgnoreCase);
		};

		auto FindFromList = [&MatchesGraph](const TArray<UEdGraph*>& Graphs) -> UEdGraph*
		{
			for (UEdGraph* Graph : Graphs)
			{
				if (MatchesGraph(Graph))
				{
					return Graph;
				}
			}
			return nullptr;
		};

		if (UEdGraph* Graph = FindFromList(Blueprint->UbergraphPages))
		{
			return Graph;
		}
		if (UEdGraph* Graph = FindFromList(Blueprint->FunctionGraphs))
		{
			return Graph;
		}
		if (UEdGraph* Graph = FindFromList(Blueprint->MacroGraphs))
		{
			return Graph;
		}
		if (UEdGraph* Graph = FindFromList(Blueprint->DelegateSignatureGraphs))
		{
			return Graph;
		}

		TArray<UEdGraph*> AllGraphs;
		Blueprint->GetAllGraphs(AllGraphs);
		return FindFromList(AllGraphs);
	}

	static UEdGraphNode* FindNodeByGuid(UEdGraph* Graph, const FString& NodeGuidText)
	{
		if (!Graph || NodeGuidText.TrimStartAndEnd().IsEmpty())
		{
			return nullptr;
		}

		FGuid NodeGuid;
		if (!FGuid::Parse(NodeGuidText, NodeGuid))
		{
			return nullptr;
		}

		for (UEdGraphNode* Node : Graph->Nodes)
		{
			if (Node && Node->NodeGuid == NodeGuid)
			{
				return Node;
			}
		}
		return nullptr;
	}

	static bool PinNameMatches(const UEdGraphPin* Pin, const FString& PinName)
	{
		if (!Pin || PinName.TrimStartAndEnd().IsEmpty())
		{
			return false;
		}

		return Pin->PinName.ToString().Equals(PinName, ESearchCase::IgnoreCase)
			|| Pin->GetDisplayName().ToString().Equals(PinName, ESearchCase::IgnoreCase);
	}

	static bool TryResolvePin(
		UEdGraph* Graph,
		UEdGraphNode* Node,
		const FString& PinGuidText,
		const FString& PinName,
		UEdGraphPin*& OutPin,
		FDiagnostic& OutError)
	{
		OutPin = nullptr;
		if (!Graph)
		{
			OutError = FDiagnostic::Failure(TEXT("graph_not_found"), TEXT("A Blueprint graph is required to resolve the requested pin."), TEXT("candidate_resolve"));
			OutError.CandidateName = PinName; OutError.PropertyPath = TEXT("graph");
			return false;
		}

		FGuid PinGuid;
		const FString CleanPinGuid = PinGuidText.TrimStartAndEnd();
		const FString CleanPinName = PinName.TrimStartAndEnd();
		const bool bHasPinGuid = !CleanPinGuid.IsEmpty();
		if (bHasPinGuid && !FGuid::Parse(CleanPinGuid, PinGuid))
		{
			OutError = FDiagnostic::Failure(TEXT("pin_guid_invalid"), TEXT("The supplied pin identity is not a valid GUID."), TEXT("candidate_resolve"));
			OutError.PropertyPath = TEXT("pin_guid"); OutError.ActualEvidence = MakeShared<FJsonValueString>(PinGuidText); OutError.ExpectedValue = MakeShared<FJsonValueString>(TEXT("GUID"));
			return false;
		}
		if (!bHasPinGuid && CleanPinName.IsEmpty())
		{
			OutError = FDiagnostic::Failure(TEXT("pin_selector_required"), TEXT("A non-empty pin name or pin GUID is required to select a Blueprint pin."), TEXT("candidate_resolve"));
			OutError.PropertyPath = TEXT("pin_name"); OutError.ExpectedValuePresent = true; OutError.ActualValuePresent = false;
			return false;
		}
		auto MatchesPin = [bHasPinGuid, PinGuid, &PinName](UEdGraphPin* Pin)
		{
			return Pin
				&& (!bHasPinGuid || Pin->PinId == PinGuid)
				&& (PinName.TrimStartAndEnd().IsEmpty()
					|| PinNameMatches(Pin, PinName));
		};

		TArray<UEdGraphPin*> Matches;
		auto AppendMatches = [&Matches, &MatchesPin](UEdGraphNode* CandidateNode)
		{
			if (!CandidateNode)
			{
				return;
			}
			for (UEdGraphPin* Pin : CandidateNode->Pins)
			{
				if (MatchesPin(Pin))
				{
					Matches.Add(Pin);
				}
			}
		};
		if (Node)
		{
			AppendMatches(Node);
		}
		else
		{
			for (UEdGraphNode* GraphNode : Graph->Nodes)
			{
				AppendMatches(GraphNode);
			}
		}
		if (Matches.IsEmpty())
		{
			OutError = FDiagnostic::Failure(TEXT("pin_not_found"), TEXT("No pin in the selected Blueprint node or graph matches the supplied pin selectors."), TEXT("candidate_resolve"));
			OutError.CandidateName = PinName; OutError.PropertyPath = bHasPinGuid ? TEXT("pin_guid") : TEXT("pin_name"); OutError.ActualEvidence = MakeShared<FJsonValueString>(PinGuidText); OutError.ActualValue = MakeShared<FJsonValueNumber>(0);
			return false;
		}
		if (Matches.Num() != 1)
		{
			OutError = FDiagnostic::Failure(TEXT("pin_ambiguous"), FString::Printf(TEXT("pin ambiguous: %d"), Matches.Num()), TEXT("candidate_resolve"));
			OutError.CandidateName = PinName; OutError.ExpectedValue = MakeShared<FJsonValueNumber>(1); OutError.ActualValue = MakeShared<FJsonValueNumber>(Matches.Num());
			return false;
		}
		OutPin = Matches[0];
		OutError.Reset();
		return true;
	}

	static FString BuildNativeActionSignature(
		const FEdGraphSchemaAction& Action,
		const UBlueprintNodeSpawner& Spawner)
	{
		// Menu category, tooltip, keywords, grouping and section describe where
		// one native action is presented, not what it performs. Keeping those
		// fields in the identity made equivalent menu rows look like distinct
		// authoring candidates and forced artificial public disambiguators.
		const FString Material = FString::Printf(
			TEXT("type=%s\nspawner_class=%s\nspawner=%s"),
			*Action.GetTypeId().ToString(),
			*Spawner.GetClass()->GetPathName(),
			*Spawner.GetSpawnerSignature().ToString());
		return TEXT("native-action:") + FMD5::HashAnsiString(*Material).ToLower();
	}

	static FString NormalizeCategoryPath(const FEdGraphSchemaAction& Action)
	{
		FString Category = Action.GetCategory().ToString();
		Category.ReplaceInline(TEXT("|"), TEXT("/"));
		Category.TrimStartAndEndInline();
		return Category.IsEmpty() ? TEXT("Blueprint") : Category;
	}

	static FString ClassSourceOrigin(const UClass* Class)
	{
		if (!Class)
		{
			return TEXT("unknown");
		}
		const FString PackageName = Class->GetOutermost()->GetName();
		if (PackageName.StartsWith(TEXT("/Script/")) || PackageName.StartsWith(TEXT("/Engine/")))
		{
			return TEXT("engine");
		}
		if (PackageName.StartsWith(TEXT("/Game/")))
		{
			return TEXT("project");
		}
		return TEXT("plugins");
	}

	using FBlueprintLibraries = TArray<TStrongObjectPtr<UBlueprint>>;

	struct FBlueprintActionEntry final
	{
		FCandidate Candidate;
		TSharedPtr<FEdGraphSchemaAction> Action;
		FBlueprintActionContext ActionContext;
		UEdGraph* Graph = nullptr;
		UEdGraphPin* Pin = nullptr;
		bool bComponentBoundEvent = false;
		UBlueprint* Blueprint = nullptr;
		bool bCandidatePrepared = false;
		bool bCandidateAvailable = false;
		FDiagnostic CandidateError;
		FString KnownNamePrefix;
		bool bNamePrefixPrepared = false;
		TSharedPtr<FBlueprintLibraries> Libraries;
		TSharedPtr<FWorkingBlueprintActionScope> WorkingActions;
	};

	struct FBatchProjection
	{
		TMap<FString, FString> RequiredSelectors;
		FString QueryName;
		TArray<UE::UAI::ContentDiscovery::FNameSelectionEntry> Names;
		TArray<int32> ScopedToEntry;
	};
	struct FBatchCatalog
	{
		FSearchParams Params;
		TArray<FBlueprintActionEntry> Entries;
		TArray<TSharedPtr<FBatchProjection>> Projections;
	};
	struct FBatchState
	{
		struct FBlueprintNotifications
		{
			FDelegateHandle Changed;
			FDelegateHandle Compiled;
		};
		~FBatchState()
		{
			for (const auto& Entry : Notifications)
				if (UBlueprint* Blueprint = Entry.Key.Get())
				{
					Blueprint->OnChanged().Remove(Entry.Value.Changed);
					Blueprint->OnCompiled().Remove(Entry.Value.Compiled);
				}
		}
		FBatchState* Previous = nullptr;
		TArray<TSharedPtr<FBatchCatalog>> Catalogs;
		TMap<const UBlueprint*, TSharedPtr<FBlueprintLibraries>> Libraries;
		TMap<TWeakObjectPtr<UBlueprint>, FBlueprintNotifications> Notifications;
	};
	thread_local FBatchState* ActiveBatch = nullptr;

	void ObserveBlueprintActionChanges(UBlueprint& Blueprint)
	{
		if (!ActiveBatch) return;
		for (FBatchState* Batch = ActiveBatch; Batch; Batch = Batch->Previous)
			if (Batch->Notifications.Contains(&Blueprint)) return;
		auto& Notifications = ActiveBatch->Notifications.Add(&Blueprint);
		// Native actions can regenerate the skeleton while applying a node.
		// Skeleton-only compilation broadcasts Changed, not Compiled. Retire all
		// cached spawners/projections before another action consumes their fields.
		const auto Invalidate = [](UBlueprint*) { InvalidateBlueprintActionBatch(); };
		Notifications.Changed = Blueprint.OnChanged().AddLambda(Invalidate);
		Notifications.Compiled = Blueprint.OnCompiled().AddLambda(Invalidate);
	}

	TSharedPtr<FBlueprintLibraries> PrepareBlueprintLibraries(const UBlueprint& Blueprint)
	{
		if (ActiveBatch)
			if (const auto* Existing = ActiveBatch->Libraries.Find(&Blueprint)) return *Existing;
		auto Libraries = MakeShared<FBlueprintLibraries>();
		const IConsoleVariable* AutoLoad = IConsoleManager::Get().FindConsoleVariable(
			TEXT("bp.EnableAutomaticLibraryAssetLoading"));
		if (Blueprint.BlueprintType != BPTYPE_Interface && (!AutoLoad || AutoLoad->GetInt() != 0))
		{
			// Match Blueprint Editor library preparation without opening a toolkit.
			// Namespace membership remains owned by UE, not a parallel UAI filter.
			FBlueprintNamespaceHelper Namespaces;
			Namespaces.AddBlueprint(&Blueprint);
			const FString DeveloperPath = FPackageName::FilenameToLongPackageName(FPaths::GameDevelopersDir());
			const FString UserDeveloperPath = FPackageName::FilenameToLongPackageName(FPaths::GameUserDeveloperDir());
			FAssetRegistryModule& Registry = FModuleManager::LoadModuleChecked<FAssetRegistryModule>(TEXT("AssetRegistry"));
			TArray<FAssetData> Assets;
			Registry.Get().GetAssetsByClass(UBlueprint::StaticClass()->GetClassPathName(), Assets);
			const FName BlueprintTypeName(GET_MEMBER_NAME_STRING_CHECKED(UBlueprint, BlueprintType));
			for (const FAssetData& Asset : Assets)
			{
				const FString Type = Asset.GetTagValueRef<FString>(BlueprintTypeName);
				if (Type != TEXT("BPTYPE_MacroLibrary") && Type != TEXT("BPTYPE_FunctionLibrary")) continue;
				if (!Namespaces.IsImportedAsset(Asset)) continue;
				const FString Path = Asset.GetSoftObjectPath().ToString();
				if (Path.StartsWith(DeveloperPath) && !Path.StartsWith(UserDeveloperPath)) continue;
				if (UBlueprint* Library = LoadBlueprint(Path))
					Libraries->Emplace(Library);
			}
		}
		if (ActiveBatch) ActiveBatch->Libraries.Add(&Blueprint, Libraries);
		return Libraries;
	}

	static FString ReadableBlueprintEventName(FString Identifier)
	{
		Identifier.RemoveFromStart(TEXT("Receive"), ESearchCase::CaseSensitive);
		return ReadableSemanticName(MoveTemp(Identifier));
	}

	static bool TryBuildComponentEventCandidate(
		const FSearchParams& Params,
		const UE::UAI::DomainAdapters::BlueprintCommands::FBlueprintComponentEventRecord& Event,
		const FString& ComponentName,
		const FString& ComponentClass,
		int32 UiOrder,
		FCandidate& OutCandidate,
		FDiagnostic& OutError)
	{
		OutCandidate = {};
		OutError.Reset();
		const FString EventName = Event.NativeFieldName.IsEmpty()
			? Event.DisplayName : Event.NativeFieldName;
		if (EventName.IsEmpty() || ComponentName.IsEmpty())
		{
			return false;
		}
		FCandidate Candidate;
		Candidate.Kind = TEXT("blueprint_component_event");
		Candidate.Type = Candidate.Kind;
		Candidate.Name = ReadableBlueprintEventName(EventName);
		Candidate.DisplayName = Candidate.Name;
		Candidate.FullName = Candidate.Name;
		Candidate.CategoryPath = Event.Category;
		Candidate.Section = TEXT("Component Events");
		Candidate.SourceOrigin = TEXT("blueprint");
		Candidate.SourceLabel = ComponentName;
		Candidate.WriteSupport = TEXT("blueprint.graphs[].items[].candidate");
		Candidate.SupportReason = TEXT("Use candidate.name; add select.component_name only when the event name is ambiguous.");
		Candidate.AuthoringSelectors.Add(TEXT("component_name"), ComponentName);
		Candidate.AuthoringSelectors.Add(TEXT("event_name"), EventName);
		Candidate.AuthoringSelectors.Add(TEXT("event_class"), ComponentClass);
		Candidate.AuthoringSelectors.Add(TEXT("node_type"), TEXT("component_bound_event"));
		Candidate.UiOrder = UiOrder;
		FDiagnostic IdentityError;
		if (!SetCandidateIdentity(Params.CatalogType, Event.EventRef,
			ComponentName + TEXT("/") + EventName, Candidate, IdentityError))
		{
			OutError = MoveTemp(IdentityError);
			return false;
		}
		Candidate.AuthoringSelectors.Add(TEXT("delegate_property_name"), EventName);
		Candidate.AuthoringSelectors.Add(TEXT("delegate_owner_class"), ComponentClass);
		OutCandidate = MoveTemp(Candidate);
		return true;
	}

	static FString BuildCandidateBaseName(const FString& Type, FString CanonicalKey,
		const FString& StableSemanticName)
	{
		if (!StableSemanticName.IsEmpty()) return StableSemanticName;
		CanonicalKey.RemoveFromStart(TEXT("K2Node_"), ESearchCase::CaseSensitive);
		return Type == TEXT("blueprint_event")
			? ReadableBlueprintEventName(CanonicalKey) : ReadableSemanticName(CanonicalKey);
	}

	static bool TryBuildBlueprintActionCandidate(
		const FSearchParams& Params,
		const UBlueprint* ContextBlueprint,
		UEdGraph* ContextGraph,
		const TSharedPtr<FEdGraphSchemaAction>& Action,
		int32 UiOrder,
		FCandidate& OutCandidate,
		FDiagnostic& OutError)
	{
		OutError.Reset();
		if (!Action.IsValid())
		{
			return false;
		}

		const FBlueprintActionMenuItem* MenuItem = nullptr;
		const UBlueprintNodeSpawner* Spawner = nullptr;
		if (Action->GetTypeId() == FBlueprintActionMenuItem::StaticGetTypeId())
		{
			const TSharedPtr<FBlueprintActionMenuItem> TypedAction = StaticCastSharedPtr<FBlueprintActionMenuItem>(Action);
			MenuItem = TypedAction.Get();
			Spawner = MenuItem ? MenuItem->GetRawAction() : nullptr;
		}

		const UK2Node* TemplateNode = Spawner
			? Cast<UK2Node>(Spawner->GetTemplateNode(ContextGraph))
			: FBlueprintActionMenuUtils::ExtractNodeTemplateFromAction(Action);
		const UClass* NodeClass = Spawner && Spawner->NodeClass ? Spawner->NodeClass.Get() : nullptr;
		if (!NodeClass && TemplateNode)
		{
			NodeClass = TemplateNode->GetClass();
		}
		if (!NodeClass)
		{
			return false;
		}

		FString Type = TEXT("blueprint_node");
		FString StableId = NodeClass->GetPathName();
		FString CanonicalKey = NodeClass->GetName();
		FString WriteSupport = TEXT("blueprint.graphs[].items[].candidate");
		FString SupportReason = TEXT("Use node_class as the item candidate in the Blueprint graph authoring surface; specialized candidates may require additional fields.");
		FString AuthoritativeDisplayName;
		FString StableSemanticName;
		TMap<FString, FString> AuthoringSelectors;
		AuthoringSelectors.Add(TEXT("node_class"), NodeClass->GetPathName());
		const FString NativeActionSignature = Spawner
			? BuildNativeActionSignature(*Action, *Spawner)
			: FString();
		if (NativeActionSignature.IsEmpty())
		{
			return false;
		}
		AuthoringSelectors.Add(
			TEXT("native_action_signature"), NativeActionSignature);
		FString BoundAssetName;
		FString BoundClassName;
		if (const auto* AssetSpawner = Cast<UBlueprintAssetNodeSpawner>(Spawner))
		{
			const FAssetData& Asset = AssetSpawner->GetAssetData();
			if (Asset.IsValid())
			{
				BoundAssetName = Asset.AssetName.ToString();
				AuthoringSelectors.Add(TEXT("asset_override"), Asset.GetObjectPathString());
			}
		}
		else if (TemplateNode)
		{
			TSet<const UObject*> BoundAssets;
			TSet<const UClass*> BoundClasses;
			for (TFieldIterator<FObjectPropertyBase> Property(TemplateNode->GetClass()); Property; ++Property)
			{
				const UObject* Value = Property->GetObjectPropertyValue_InContainer(TemplateNode);
				if (const UClass* Class = Cast<UClass>(Value))
				{
					BoundClasses.Add(Class);
				}
				else if (Value && Value->IsAsset())
				{
					BoundAssets.Add(Value);
				}
			}
			if (BoundAssets.Num() == 1)
			{
				const UObject* Asset = *BoundAssets.CreateConstIterator();
				BoundAssetName = Asset->GetName();
				AuthoringSelectors.Add(TEXT("asset_override"), Asset->GetPathName());
			}
			if (BoundClasses.Num() == 1)
			{
				const UClass* Class = *BoundClasses.CreateConstIterator();
				BoundClassName = ReadableSemanticName(Class->GetName());
				AuthoringSelectors.Add(TEXT("class_binding"), Class->GetPathName());
			}
		}

		if (TemplateNode) UeAgentBlueprintInputBindings::Capture(*TemplateNode, AuthoringSelectors);

		if (const UK2Node_MacroInstance* Macro = Cast<UK2Node_MacroInstance>(TemplateNode))
		{
			const UEdGraph* MacroGraph = Macro->GetMacroGraph();
			if (!MacroGraph) return false;
			CanonicalKey = MacroGraph->GetName();
			StableId = MacroGraph->GetPathName();
			StableSemanticName = FName::NameToDisplayString(CanonicalKey, false);
			AuthoritativeDisplayName = StableSemanticName;
			AuthoringSelectors.Add(TEXT("macro_graph"), CanonicalKey);
			if (const UBlueprint* Owner = MacroGraph->GetTypedOuter<UBlueprint>())
				AuthoringSelectors.Add(TEXT("asset_override"), NormalizeObjectPath(
					UeAgentBlueprintAssetLifecycle::GetAuthoringTargetPath(*Owner)));
		}
		else if (const UK2Node_PromotableOperator* PromotableOperator =
			Cast<UK2Node_PromotableOperator>(TemplateNode))
		{
			NodeClass = PromotableOperator->GetClass();
			const FString OperationName =
				PromotableOperator->GetOperationName().ToString();
			if (OperationName.IsEmpty())
			{
				return false;
			}
			Type = TEXT("blueprint_node");
			StableId = NodeClass->GetPathName();
			CanonicalKey = OperationName;
			AuthoritativeDisplayName = FName::NameToDisplayString(
				OperationName, false);
			StableSemanticName = AuthoritativeDisplayName;
			WriteSupport = TEXT("blueprint.graphs[].items[].candidate");
			SupportReason = TEXT(
				"Use the returned readable candidate name in the Blueprint graph "
				"authoring surface; operation_name is an optional exact "
				"assertion for this promotable operator.");
			AuthoringSelectors.Add(
				TEXT("operation_name"), OperationName);
		}
		else if (const UBlueprintFunctionNodeSpawner* FunctionSpawner =
			Cast<const UBlueprintFunctionNodeSpawner>(Spawner))
		{
			if (const UFunction* Function = FunctionSpawner->GetFunction())
			{
				Type = TEXT("blueprint_call_function");
				StableId = Function->GetPathName();
				CanonicalKey = Function->GetName();
				AuthoringSelectors.Add(TEXT("function_name"), Function->GetName());
				if (Function->GetOwnerClass()) AuthoringSelectors.Add(TEXT("function_owner_class"),
					GetAuthoringOwnerPath(ContextBlueprint, *Function->GetOwnerClass()));
				const FString NativeDisplayName = Function->HasMetaData(
					FBlueprintMetadata::MD_DisplayName)
					? Function->GetMetaData(FBlueprintMetadata::MD_DisplayName)
					: FString();
				StableSemanticName = NativeDisplayName.IsEmpty()
					? FName::NameToDisplayString(Function->GetName(), false)
					: NativeDisplayName;
				AuthoritativeDisplayName = NativeDisplayName.IsEmpty()
					? Function->GetDisplayNameText().ToString()
					: NativeDisplayName;
				WriteSupport = TEXT("blueprint.graphs[].items[].candidate");
				SupportReason = TEXT("Use function_owner_class and function_name as the call-function item candidate fields.");
			}
		}
		else if (const UBlueprintEventNodeSpawner* EventSpawner = Cast<const UBlueprintEventNodeSpawner>(Spawner))
		{
			if (const UFunction* Function = EventSpawner->GetEventFunction())
			{
				Type = TEXT("blueprint_event");
				StableId = Function->GetPathName();
				CanonicalKey = Function->GetName();
				WriteSupport = TEXT("blueprint.graphs[].items[].candidate");
				SupportReason = TEXT("Use event_class and event_name as the event item candidate fields.");
				if (Function->GetOwnerClass()) AuthoringSelectors.Add(TEXT("event_class"), GetAuthoringOwnerPath(ContextBlueprint, *Function->GetOwnerClass()));
				AuthoringSelectors.Add(TEXT("event_name"), Function->GetName());
			}
		}
		else if (const UBlueprintVariableNodeSpawner* VariableSpawner = Cast<const UBlueprintVariableNodeSpawner>(Spawner))
		{
			const FProperty* VarProperty = VariableSpawner->GetVarProperty();
			if (VarProperty)
			{
				Type = TEXT("blueprint_variable_node");
				StableId = VarProperty->GetPathName();
				CanonicalKey = VarProperty->GetName();
				const UClass* OwnerClass = VarProperty->GetOwnerClass();
				if (IsAuthoringSelfClass(ContextBlueprint, OwnerClass))
				{
					// Validation copies have different generated class names. Self
					// members keep their authored meaning across those copies.
					StableSemanticName = ReadableSemanticName(CanonicalKey) + TEXT(" (Self)");
				}
				WriteSupport = TEXT("blueprint.graphs[].items[].candidate");
				SupportReason = TEXT("Use variable_name and node_type as the variable item candidate fields.");
				AuthoringSelectors.Add(TEXT("variable_name"), VarProperty->GetName());
				const FString NodeClassName = VariableSpawner->NodeClass ? VariableSpawner->NodeClass->GetName() : FString();
				AuthoringSelectors.Add(TEXT("node_type"), NodeClassName.Contains(TEXT("Set")) ? TEXT("set") : TEXT("get"));
				if (VarProperty->GetOwnerClass()) AuthoringSelectors.Add(TEXT("member_parent_class"), GetAuthoringOwnerPath(ContextBlueprint, *VarProperty->GetOwnerClass()));
			}
		}
		else if (const UBlueprintFieldNodeSpawner* FieldSpawner = Cast<const UBlueprintFieldNodeSpawner>(Spawner))
		{
			const FFieldVariant Field = FieldSpawner->GetField();
			if (const FField* RawField = Field.ToField())
			{
				StableId = RawField->GetPathName();
				CanonicalKey = RawField->GetName();
			}
		}

		// Specialization adds to the shared node form and bound context. A
		// promotable operator is identified by its operation, not the arbitrary
		// function used to register its spawner before pin type promotion.
		AuthoringSelectors.Add(TEXT("node_class"), NodeClass->GetPathName());

		FCandidate Candidate;
		Candidate.Kind = Type;
		// Native events already have a stable readable semantic name. Menu text is
		// localized UI state and may be unavailable or malformed in commandlets,
		// so it must not replace the name used for discovery and re-resolution.
		Candidate.Name = BuildCandidateBaseName(Type, CanonicalKey, StableSemanticName);
		if (!BoundAssetName.IsEmpty()) Candidate.Name += TEXT(" [") + BoundAssetName + TEXT("]");
		if (!BoundClassName.IsEmpty()) Candidate.Name += TEXT(" [") + BoundClassName + TEXT("]");
		Candidate.FullName = StableId;
		// Name is the stable semantic lookup term. DisplayName is deliberately
		// kept as the editor-facing menu text so localization cannot change
		// resolution while the UI remains human-friendly.
		Candidate.DisplayName = AuthoritativeDisplayName.TrimStartAndEnd().IsEmpty()
			? MenuNameFromText(Action->GetMenuDescription(), Candidate.Name)
			: AuthoritativeDisplayName.TrimStartAndEnd();
		Candidate.Tooltip = Action->GetTooltipDescription().ToString();
		Candidate.Keywords = Action->GetKeywords().ToString();
		Candidate.CategoryPath = NormalizeCategoryPath(*Action);
		Candidate.Section = Params.PinGuid.IsEmpty() && Params.PinName.IsEmpty() ? TEXT("Context") : TEXT("Pin Context");
		Candidate.Type = Type;
		Candidate.SourceOrigin = ClassSourceOrigin(NodeClass);
		Candidate.SourceLabel = NodeClass->GetOutermost()->GetName();
		Candidate.ObjectPath = NodeClass->GetPathName();
		Candidate.AuthoringSelectors = MoveTemp(AuthoringSelectors);
		Candidate.WriteSupport = WriteSupport;
		Candidate.SupportReason = SupportReason;
		Candidate.bInLibrary = true;
		Candidate.bSupportedInContext = true;
		if (Action->GetTypeId() == FEdGraphSchemaAction_K2ViewNode::StaticGetTypeId())
		{
			Candidate.bSupportedInContext = false;
			Candidate.WriteSupport = TEXT("search_only");
			Candidate.SupportDiagnostic = FDiagnostic::Failure(
				TEXT("native_view_action_is_navigation_not_creation"),
				TEXT("This native Blueprint action navigates to an existing node and does not create a node."),
				TEXT("candidate_resolve"), NativeActionSignature);
			Candidate.SupportDiagnostic.CandidateName = Candidate.Name;
			Candidate.SupportDiagnostic.TargetPath = Params.AssetPath;
			Candidate.SupportDiagnostic.SourceKind = TEXT("native_result");
			Candidate.SupportDiagnostic.PropertyPath = TEXT("action_operation");
			Candidate.SupportDiagnostic.ActualValue = MakeShared<FJsonValueString>(TEXT("navigation"));
			Candidate.SupportDiagnostic.ExpectedValue = MakeShared<FJsonValueString>(TEXT("creation"));
			Candidate.SupportReason = Candidate.SupportDiagnostic.PublicMessage;
		}
		Candidate.UiOrder = UiOrder;
		FDiagnostic IdentityError;
		if (!SetCandidateIdentity(
				Params.CatalogType,
				NativeActionSignature,
				CanonicalKey,
				Candidate,
				IdentityError))
		{
			OutError = MoveTemp(IdentityError);
			return false;
		}
		OutCandidate = MoveTemp(Candidate);
		return true;
	}

}

bool UeAgentPaletteSearch::IsPublicBlueprintCandidateSemanticField(
	const FString& Field)
{
	return Field == TEXT("component_name")
		|| Field == TEXT("function_name")
		|| Field == TEXT("function_owner_class")
		|| Field == TEXT("event_name")
		|| Field == TEXT("event_class")
		|| Field == TEXT("variable_name")
		|| Field == TEXT("node_type")
		|| Field == TEXT("member_parent_class")
		|| Field == TEXT("node_class")
		|| Field == TEXT("asset_override")
		|| Field == TEXT("class_binding")
		|| Field == TEXT("operation_name")
		|| Field == TEXT("input_key")
		|| Field == TEXT("input_action")
		|| Field == TEXT("input_axis")
		|| Field == TEXT("macro_graph");
}

FString UeAgentPaletteSearch::GetBlueprintActionPublicName(
	const FCandidate& Candidate)
{
	// Public discovery is a stable authoring protocol, not the editor's
	// localized presentation surface. Candidate.Name is derived from the
	// authoritative native semantic identifier and therefore remains readable
	// across editor cultures. Variable actions need their operation prefix to
	// distinguish the two actions that share one property name.
	FString PublicName = Candidate.Name.TrimStartAndEnd();
	if (Candidate.Type == TEXT("blueprint_variable_node"))
	{
		const FString Operation =
			Candidate.AuthoringSelectors.FindRef(TEXT("node_type"));
		if (Operation.Equals(TEXT("get"), ESearchCase::IgnoreCase))
		{
			PublicName = TEXT("Get ") + PublicName;
		}
		else if (Operation.Equals(TEXT("set"), ESearchCase::IgnoreCase))
		{
			PublicName = TEXT("Set ") + PublicName;
		}
	}
	return PublicName;
}

bool UeAgentPaletteSearch::TryBuildBlueprintActionScopedDiscoveryEntry(
	const FCandidate& Candidate,
	UE::UAI::ContentDiscovery::FScopedNameDiscoveryEntry& OutEntry)
{
	OutEntry = {};
	OutEntry.Name = GetBlueprintActionPublicName(Candidate);
	OutEntry.Identity = Candidate.Identity.CanonicalKey;
	if (!UE::UAI::ContentDiscovery::IsReadableDiscoveryName(OutEntry.Name))
	{
		return false;
	}

	auto CompactSemantic = [](const FString& Value)
	{
		FString Result;
		for (const TCHAR Character : Value.ToLower())
		{
			if (FChar::IsAlnum(Character))
			{
				Result.AppendChar(Character);
			}
		}
		return Result;
	};
	auto ReadableObjectScope = [](FString Value)
	{
		int32 Separator = INDEX_NONE;
		if (Value.FindLastChar(TEXT('.'), Separator)
			|| Value.FindLastChar(TEXT('/'), Separator))
		{
			Value.RightChopInline(Separator + 1);
		}
		Value.RemoveFromEnd(TEXT("_C"), ESearchCase::CaseSensitive);
		return ReadableSemanticName(MoveTemp(Value));
	};
	const FString NameKey = CompactSemantic(OutEntry.Name);
	auto AddScope = [&OutEntry, &CompactSemantic, &NameKey](FString Scope)
	{
		Scope.TrimStartAndEndInline();
		if (Scope.IsEmpty()
			|| CompactSemantic(Scope) == NameKey
			|| !UE::UAI::ContentDiscovery::IsReadableDiscoveryName(Scope)
			|| OutEntry.Scopes.ContainsByPredicate(
				[&Scope](const FString& Existing)
				{
					return Existing.Equals(Scope, ESearchCase::IgnoreCase);
				}))
		{
			return;
		}
		OutEntry.Scopes.Add(MoveTemp(Scope));
	};

	AddScope(Candidate.AuthoringSelectors.FindRef(TEXT("component_name")));
	const FString BoundAssetPath = Candidate.AuthoringSelectors.FindRef(TEXT("asset_override"));
	if (!BoundAssetPath.IsEmpty())
	{
		UE::UAI::ContentDiscovery::AppendReadableDiscoveryPathScopes(
			FPackageName::GetLongPackagePath(FPackageName::ObjectPathToPackageName(BoundAssetPath)), OutEntry.Scopes);
	}
	for (const TCHAR* OwnerField : {
		TEXT("function_owner_class"), TEXT("event_class"),
		TEXT("member_parent_class"), TEXT("node_class"), TEXT("class_binding")})
	{
		const FString Owner = Candidate.AuthoringSelectors.FindRef(OwnerField);
		if (!Owner.IsEmpty())
		{
			AddScope(ReadableObjectScope(Owner));
			UE::UAI::ContentDiscovery::AppendReadableDiscoveryPathScopes(
				FPackageName::ObjectPathToPackageName(Owner), OutEntry.Scopes);
		}
	}
	for (const TCHAR* SemanticField : {
		TEXT("node_type"), TEXT("operation_name"), TEXT("function_name"),
		TEXT("event_name"), TEXT("variable_name")})
	{
		AddScope(ReadableSemanticName(
			Candidate.AuthoringSelectors.FindRef(SemanticField)));
	}
	AddScope(ReadableSemanticName(Candidate.Type));
	return true;
}

namespace
{
	bool IsBlueprintCatalogType(const FString& CatalogType)
	{
		return CatalogType.Equals(TEXT("blueprint_node"))
			|| CatalogType.Equals(TEXT("blueprint_call_function"))
			|| CatalogType.Equals(TEXT("blueprint_event"))
			|| CatalogType.Equals(TEXT("blueprint_variable_node"));
	}

	bool CollectBlueprintActionEntries(
		const FSearchParams& Params,
		TArray<FBlueprintActionEntry>& OutEntries,
		TArray<FString>& OutWarnings,
		FDiagnostic& OutError)
	{
		OutEntries.Reset();
		if (!IsBlueprintCatalogType(Params.CatalogType))
		{
			OutError = FDiagnostic::Failure(TEXT("catalog_type_unsupported"), FString::Printf(TEXT("unsupported Blueprint catalog_type '%s'."), *Params.CatalogType), TEXT("candidate_resolve"));
			return false;
		}
		if (!Params.NodeGuid.TrimStartAndEnd().IsEmpty()
			&& Params.PinGuid.TrimStartAndEnd().IsEmpty()
			&& Params.PinName.TrimStartAndEnd().IsEmpty())
		{
			OutError = FDiagnostic::Failure(TEXT("pin_selector_required"), TEXT("A node-specific Blueprint candidate query also requires a pin name or pin GUID."), TEXT("candidate_resolve"));
			OutError.TargetPath = Params.AssetPath; OutError.PropertyPath = TEXT("pin_name"); OutError.ExpectedValuePresent = true; OutError.ActualValuePresent = false;
			return false;
		}

		UBlueprint* Blueprint = nullptr;
		UEdGraph* Graph = nullptr;
		UEdGraphPin* Pin = nullptr;
		if (!Params.AssetPath.IsEmpty())
		{
			Blueprint = LoadBlueprint(Params.AssetPath);
			if (!Blueprint)
			{
				OutError = FDiagnostic::Failure(TEXT("blueprint_not_found"), TEXT("The requested Blueprint asset could not be loaded."), TEXT("candidate_resolve"));
				OutError.TargetPath = Params.AssetPath; OutError.PropertyPath = TEXT("asset_path");
				return false;
			}
			Graph = ResolveGraph(Blueprint, Params.GraphName);
			if (!Graph && (!Params.GraphName.IsEmpty()
				|| !Params.PinGuid.IsEmpty()
				|| !Params.PinName.IsEmpty()))
			{
				OutError = FDiagnostic::Failure(TEXT("graph_not_found"), TEXT("The Blueprint has no graph matching the requested graph selector and pin context."), TEXT("candidate_resolve"));
				OutError.TargetPath = Params.AssetPath; OutError.PropertyPath = TEXT("graph_name"); OutError.ActualValue = MakeShared<FJsonValueString>(Params.GraphName);
				return false;
			}
			if (Graph && (!Params.PinGuid.IsEmpty() || !Params.PinName.IsEmpty()))
			{
				UEdGraphNode* Node = nullptr;
				if (!Params.NodeGuid.TrimStartAndEnd().IsEmpty())
				{
					FGuid ParsedNodeGuid;
					if (!FGuid::Parse(Params.NodeGuid, ParsedNodeGuid))
					{
						OutError = FDiagnostic::Failure(TEXT("node_guid_invalid"), TEXT("The supplied Blueprint node identity is not a valid GUID."), TEXT("candidate_resolve"));
						OutError.TargetPath = Params.AssetPath; OutError.PropertyPath = TEXT("node_guid"); OutError.ExpectedValue = MakeShared<FJsonValueString>(TEXT("GUID")); OutError.ActualEvidence = MakeShared<FJsonValueString>(Params.NodeGuid);
						return false;
					}
					Node = FindNodeByGuid(Graph, Params.NodeGuid);
					if (!Node)
					{
						OutError = FDiagnostic::Failure(TEXT("node_not_found"), TEXT("The selected Blueprint graph has no node with the supplied identity."), TEXT("candidate_resolve"));
						OutError.TargetPath = Params.AssetPath; OutError.PropertyPath = TEXT("node_guid"); OutError.CandidateName = Params.GraphName; OutError.ActualEvidence = MakeShared<FJsonValueString>(Params.NodeGuid);
						return false;
					}
				}
				if (!TryResolvePin(
						Graph,
						Node,
						Params.PinGuid,
						Params.PinName,
						Pin,
						OutError))
				{
					return false;
				}
			}
		}
		else
		{
			const FName TempBlueprintName = MakeUniqueObjectName(
				GetTransientPackage(),
				UBlueprint::StaticClass(),
				TEXT("UAI_NodeCatalog_TempBlueprint"));
			Blueprint = FKismetEditorUtilities::CreateBlueprint(
				AActor::StaticClass(),
				GetTransientPackage(),
				TempBlueprintName,
				BPTYPE_Normal,
				UBlueprint::StaticClass(),
				UBlueprintGeneratedClass::StaticClass());
			Graph = ResolveGraph(Blueprint, TEXT(""));
			OutWarnings.Add(TEXT(
				"Blueprint search ran without asset_path; a transient Actor "
				"Blueprint context was used, so project variables, components, "
				"selected actors, and exact pin context are omitted."));
		}

		ObserveBlueprintActionChanges(*Blueprint);
		const TSharedPtr<FBlueprintLibraries> Libraries = PrepareBlueprintLibraries(*Blueprint);
		TSharedPtr<FWorkingBlueprintActionScope> WorkingActions;
		if (Blueprint->HasAnyFlags(RF_Transient)
			&& UeAgentBlueprintAssetLifecycle::GetAuthoringTargetPath(*Blueprint) != Blueprint->GetPathName())
			WorkingActions = MakeShared<FWorkingBlueprintActionScope>(*Blueprint);
		EnsureBlueprintActionRegistration(*Blueprint);
		FBlueprintActionContext Context;
		if (Blueprint)
		{
			Context.Blueprints.Add(Blueprint);
		}
		if (Graph)
		{
			Context.Graphs.Add(Graph);
		}
		if (Pin)
		{
			Context.Pins.Add(Pin);
		}

		auto AppendBuilder = [
			&Params, &OutEntries, &OutError, Blueprint, Graph, Pin, Libraries, WorkingActions, &Context](
				FBlueprintActionMenuBuilder& Builder, bool bUnboundPalette) -> bool
		{
			while (Builder.ProcessPendingActions())
			{
			}
			for (int32 Index = 0; Index < Builder.GetNumActions(); ++Index)
			{
				const TSharedPtr<FEdGraphSchemaAction> Action =
					Builder.GetSchemaAction(Index);
				UBlueprint* Owner = ActiveBatch || WorkingActions ? GetActionOwnerBlueprint(Action, Graph) : nullptr;
				if (Owner) ObserveBlueprintActionChanges(*Owner);
				if (bUnboundPalette && WorkingActions)
				{
					// A validation copy owns the proposed member definitions for this
					// asset. UE's global palette also contains the published definitions;
					// their physical signatures are distinct, but they are superseded in
					// this authoring view. Keep native context/bound menus unchanged.
					if (Owner && Owner != Blueprint
						&& UeAgentAuthoringAssetIdentity::NormalizePackagePath(
							UeAgentBlueprintAssetLifecycle::GetAuthoringTargetPath(*Owner))
							== UeAgentAuthoringAssetIdentity::NormalizePackagePath(
								UeAgentBlueprintAssetLifecycle::GetAuthoringTargetPath(*Blueprint))) continue;
				}
				FBlueprintActionEntry& Entry = OutEntries.AddDefaulted_GetRef();
				Entry.Blueprint = Blueprint;
				Entry.Libraries = Libraries;
				Entry.WorkingActions = WorkingActions;
				Entry.Action = Action;
				Entry.ActionContext = Context;
				Entry.Graph = Graph;
				Entry.Pin = Pin;
			}
			return true;
		};

		if (Pin)
		{
			FBlueprintActionMenuBuilder ContextBuilder;
			const uint32 TargetMask =
				EContextTargetFlags::TARGET_Blueprint |
				EContextTargetFlags::TARGET_SubComponents |
				EContextTargetFlags::TARGET_NodeTarget |
				EContextTargetFlags::TARGET_PinObject |
				EContextTargetFlags::TARGET_SiblingPinObjects |
				EContextTargetFlags::TARGET_BlueprintLibraries;
			FBlueprintActionMenuUtils::MakeContextMenu(
				Context, Params.bContextSensitive, TargetMask, ContextBuilder);
			if (!AppendBuilder(ContextBuilder, false))
			{
				return false;
			}
		}
		else
		{
			FBlueprintActionMenuBuilder PaletteBuilder;
			FBlueprintActionMenuUtils::MakePaletteMenu(
				Context, nullptr, PaletteBuilder);
			if (!AppendBuilder(PaletteBuilder, true))
			{
				return false;
			}

			FBlueprintActionMenuBuilder VariableBuilder;
			const uint32 VariableTargetMask =
				EContextTargetFlags::TARGET_Blueprint |
				EContextTargetFlags::TARGET_SubComponents |
				EContextTargetFlags::TARGET_NodeTarget |
				EContextTargetFlags::TARGET_PinObject |
				EContextTargetFlags::TARGET_SiblingPinObjects |
				EContextTargetFlags::TARGET_BlueprintLibraries;
			FBlueprintActionMenuUtils::MakeContextMenu(
				Context, Params.bContextSensitive,
				VariableTargetMask, VariableBuilder);
			if (!AppendBuilder(VariableBuilder, false))
			{
				return false;
			}
		}

		// Component-bound events are owned by the same reflection-based discovery
		// used by blueprint.component.events.discover. Keep them out of the native
		// menu's duplicate/opaque identity space.
		if (Params.CatalogType.Equals(TEXT("blueprint_node")) && Blueprint)
		{
			TArray<FString> ComponentNames;
			if (!UE::UAI::DomainAdapters::BlueprintCommands::TryGetBlueprintComponentNames(
					*Blueprint, ComponentNames, OutError))
			{
				return false;
			}
			for (const FString& ComponentName : ComponentNames)
			{
				UE::UAI::DomainAdapters::BlueprintCommands::FBlueprintComponentEventDiscoveryRequest Request;
				Request.ComponentName = ComponentName;
				// Palette queries also match the component scope. Collect the full
				// event set and let the shared readable-name matcher filter it;
				// delegate-only filtering would discard names such as Event [Component].
				Request.bCompileBeforeQuery = false;
				UE::UAI::DomainAdapters::BlueprintCommands::FBlueprintComponentEventDiscoveryResult Result;
				FDiagnostic DiscoveryError;
				if (!UE::UAI::DomainAdapters::BlueprintCommands::TryDiscoverComponentEvents(
						*Blueprint, Request, Result, DiscoveryError))
				{
					continue;
				}
				for (const UE::UAI::DomainAdapters::BlueprintCommands::FBlueprintComponentEventRecord& Event
					: Result.Events)
				{
					FCandidate Candidate;
					FDiagnostic CandidateError;
					if (!TryBuildComponentEventCandidate(
							Params, Event, ComponentName, Result.TargetClassRef,
							OutEntries.Num(), Candidate, CandidateError))
					{
						if (!CandidateError.IsEmpty())
						{
							OutError = MoveTemp(CandidateError);
							return false;
						}
						continue;
					}
					FBlueprintActionEntry& Entry = OutEntries.AddDefaulted_GetRef();
					Entry.Candidate = MoveTemp(Candidate);
					Entry.bComponentBoundEvent = true;
					Entry.bCandidatePrepared = true;
					Entry.bCandidateAvailable = true;
				}
			}
		}
		return true;
	}

	bool PrepareCandidate(const FSearchParams& Params, FBlueprintActionEntry& Entry,
		int32 UiOrder, FDiagnostic& OutError)
	{
		if (!Entry.bCandidatePrepared)
		{
			Entry.bCandidateAvailable = TryBuildBlueprintActionCandidate(
				Params, Entry.Blueprint, Entry.Graph, Entry.Action, UiOrder, Entry.Candidate, Entry.CandidateError);
			Entry.bCandidatePrepared = true;
			if (Entry.bCandidateAvailable && Params.CatalogType != TEXT("blueprint_node")
				&& Entry.Candidate.Type != Params.CatalogType)
			{
				Entry.bCandidateAvailable = false;
			}
		}
		OutError = Entry.CandidateError;
		return Entry.bCandidateAvailable;
	}

	bool MayMatchNativeName(FBlueprintActionEntry& Entry, const FString& Name)
	{
		if (!Entry.bNamePrefixPrepared)
		{
			Entry.bNamePrefixPrepared = true;
			if (Entry.Action.IsValid()
				&& Entry.Action->GetTypeId() == FBlueprintActionMenuItem::StaticGetTypeId())
			{
				const auto Item = StaticCastSharedPtr<FBlueprintActionMenuItem>(Entry.Action);
				const UBlueprintNodeSpawner* Spawner = Item->GetRawAction();
				// Promotable operators take precedence over function spawners in
				// candidate construction. Their name comes from the template.
				if (Spawner && Spawner->NodeClass
					&& (Spawner->GetClass() == UBlueprintFunctionNodeSpawner::StaticClass()
						|| Spawner->GetClass() == UBlueprintEventNodeSpawner::StaticClass()
						|| Spawner->GetClass() == UBlueprintVariableNodeSpawner::StaticClass())
					&& !Spawner->NodeClass->IsChildOf(UK2Node_PromotableOperator::StaticClass()))
				{
					if (const auto* FunctionSpawner = Cast<UBlueprintFunctionNodeSpawner>(Spawner))
					{
						if (const UFunction* Function = FunctionSpawner->GetFunction())
						{
							Entry.KnownNamePrefix = Function->GetMetaData(FBlueprintMetadata::MD_DisplayName);
							if (Entry.KnownNamePrefix.IsEmpty())
								Entry.KnownNamePrefix = FName::NameToDisplayString(Function->GetName(), false);
						}
					}
					else if (const auto* EventSpawner = Cast<UBlueprintEventNodeSpawner>(Spawner))
					{
						if (const UFunction* Function = EventSpawner->GetEventFunction())
							Entry.KnownNamePrefix = BuildCandidateBaseName(TEXT("blueprint_event"), Function->GetName(), FString());
					}
					else if (const auto* VariableSpawner = Cast<UBlueprintVariableNodeSpawner>(Spawner))
					{
						if (const FProperty* Property = VariableSpawner->GetVarProperty())
						{
							FString StableName;
							const UClass* Owner = Property->GetOwnerClass();
							if (IsAuthoringSelfClass(Entry.Blueprint, Owner))
								StableName = ReadableSemanticName(Property->GetName()) + TEXT(" (Self)");
							const FString Base = BuildCandidateBaseName(TEXT("blueprint_variable_node"), Property->GetName(), StableName);
							Entry.KnownNamePrefix = (Spawner->NodeClass->GetName().Contains(TEXT("Set"))
								? FString(TEXT("Set ")) : FString(TEXT("Get "))) + Base;
						}
					}
				}
			}
			Entry.KnownNamePrefix.TrimStartInline();
		}
		// Asset/class bindings and disambiguating scopes append bracket suffixes.
		// Retain every possible same-name group; unknown spawners expand normally.
		return Entry.KnownNamePrefix.IsEmpty()
			|| Name.Equals(Entry.KnownNamePrefix.TrimEnd(), ESearchCase::IgnoreCase)
			|| Name.StartsWith(Entry.KnownNamePrefix + TEXT(" ["), ESearchCase::IgnoreCase);
	}

	bool MayMatchNativeSelectors(const FBlueprintActionEntry& Entry,
		const TMap<FString, FString>& Selectors)
	{
		// Only reject with an authoritative identity available before template
		// expansion. Semantic names and scopes still use the complete resolver.
		const FString* Signature = Selectors.Find(TEXT("native_action_signature"));
		if (!Signature || Entry.bComponentBoundEvent) return true;
		if (!Entry.Action.IsValid()
			|| Entry.Action->GetTypeId() != FBlueprintActionMenuItem::StaticGetTypeId()) return true;
		const auto Item = StaticCastSharedPtr<FBlueprintActionMenuItem>(Entry.Action);
		const UBlueprintNodeSpawner* Spawner = Item->GetRawAction();
		return !Spawner || BuildNativeActionSignature(*Entry.Action, *Spawner)
			.Equals(*Signature, ESearchCase::CaseSensitive);
	}

	bool MatchesExactName(const FCandidate& Candidate, const FString& Name)
	{
		if (Name.TrimStartAndEnd().IsEmpty())
		{
			return true;
		}
		const auto Matches = [&Name](const FString& Value)
		{
			return !Value.IsEmpty()
				&& Value.Equals(Name, ESearchCase::IgnoreCase);
		};
		if (Matches(Candidate.Name)
			|| Matches(Candidate.DisplayName)
			|| Matches(Candidate.FullName)
			|| Matches(Candidate.SearchKey)
			|| Matches(Candidate.GetStableId())
			|| Matches(Candidate.ObjectPath))
		{
			return true;
		}
		for (const TPair<FString, FString>& Selector
			: Candidate.AuthoringSelectors)
		{
			if (Matches(Selector.Value))
			{
				return true;
			}
		}
		return false;
	}

	bool CollectForBatch(const FSearchParams& Params,
		TSharedPtr<FBatchCatalog>& OutCatalog, FDiagnostic& OutError)
	{
		// Pin types can evolve during node creation and linking. Those requests
		// keep their own fresh catalog; they never consume a no-pin snapshot.
		const bool Cacheable = ActiveBatch && !Params.AssetPath.IsEmpty()
			&& Params.NodeGuid.IsEmpty() && Params.PinGuid.IsEmpty() && Params.PinName.IsEmpty();
		if (Cacheable)
		{
			for (const TSharedPtr<FBatchCatalog>& Catalog : ActiveBatch->Catalogs)
			{
				const FSearchParams& P = Catalog->Params;
				if (P.AssetPath == Params.AssetPath && P.GraphName == Params.GraphName
					&& P.CatalogType == Params.CatalogType && P.SearchText == Params.SearchText
					&& P.bContextSensitive == Params.bContextSensitive)
				{
					OutCatalog = Catalog;
					return true;
				}
			}
		}
		auto Catalog = MakeShared<FBatchCatalog>();
		Catalog->Params = Params;
		TArray<FString> Warnings;
		if (!CollectBlueprintActionEntries(Params, Catalog->Entries, Warnings, OutError)) return false;
		if (Cacheable) ActiveBatch->Catalogs.Add(Catalog);
		OutCatalog = MoveTemp(Catalog);
		return true;
	}

	bool SameSelectors(const TMap<FString, FString>& A, const TMap<FString, FString>& B)
	{
		if (A.Num() != B.Num()) return false;
		for (const TPair<FString, FString>& Pair : A)
		{
			const FString* Value = B.Find(Pair.Key);
			if (!Value || !Value->Equals(Pair.Value, ESearchCase::CaseSensitive)) return false;
		}
		return true;
	}

	FString BlueprintCandidateScopeMessage(const FSearchParams& Params)
	{
		const UEdGraph* Graph = ResolveGraph(LoadBlueprint(Params.AssetPath), Params.GraphName);
		return FString::Printf(TEXT("graph '%s' (%s)"), Graph ? *Graph->GetName() : TEXT("current"),
			Params.PinGuid.IsEmpty() ? TEXT("background menu") : TEXT("pin menu"));
	}

	void SetBlueprintCandidateResolutionFailure(
		const FSearchParams& Params, const FString& Name, const TCHAR* Code, FDiagnostic& OutError)
	{
		const UBlueprint* Blueprint = LoadBlueprint(Params.AssetPath);
		OutError = UeAgentAuthoringDiagnostics::CandidateFailure(Code, Name,
			Blueprint ? UeAgentBlueprintAssetLifecycle::GetAuthoringTargetPath(*Blueprint) : Params.AssetPath,
			BlueprintCandidateScopeMessage(Params), TEXT("blueprint.node.candidates"));
	}

    void ReportMissingBlueprintMember(const FSearchParams& Params, const FString& Name,
        TArray<FBlueprintActionEntry>& Entries)
    {
        UBlueprint* Blueprint = LoadBlueprint(Params.AssetPath);
        if (!Blueprint) return;
        FString Member = Params.RequiredAuthoringSelectors.FindRef(TEXT("variable_name"));
        // Public inputs contain name/select only. This mapping is diagnostic
        // evidence from this Blueprint's declarations, never a creation fallback.
        if (Member.IsEmpty())
            for (const FBPVariableDescription& Variable : Blueprint->NewVariables)
            {
                const FString VariableName = Variable.VarName.ToString();
                const FString Base = BuildCandidateBaseName(TEXT("blueprint_variable_node"), VariableName,
                    ReadableSemanticName(VariableName) + TEXT(" (Self)"));
                if (Name.Equals(TEXT("Get ") + Base, ESearchCase::IgnoreCase)
                    || Name.Equals(TEXT("Set ") + Base, ESearchCase::IgnoreCase))
                {
                    Member = VariableName;
                    break;
                }
            }
        const FName MemberName(*Member);
        const FProperty* SkeletonField = Blueprint->SkeletonGeneratedClass
            ? FindFProperty<FProperty>(Blueprint->SkeletonGeneratedClass, MemberName) : nullptr;
        const FProperty* GeneratedField = Blueprint->GeneratedClass
            ? FindFProperty<FProperty>(Blueprint->GeneratedClass, MemberName) : nullptr;
        int32 RegisteredMembers = 0, MenuMembers = 0, PreparedMembers = 0;
        const auto* Registered = FBlueprintActionDatabase::Get().GetAllActions().Find(FObjectKey(Blueprint));
        if (Registered)
            for (const auto& Action : *Registered)
                if (const auto* Variable = Cast<UBlueprintVariableNodeSpawner>(Action))
                    if (const FProperty* Property = Variable->GetVarProperty(); Property && Property->GetFName() == MemberName)
                        ++RegisteredMembers;
        for (FBlueprintActionEntry& Entry : Entries)
        {
            if (!Entry.Action.IsValid() || Entry.Action->GetTypeId() != FBlueprintActionMenuItem::StaticGetTypeId()) continue;
            const auto* Variable = Cast<UBlueprintVariableNodeSpawner>(
                StaticCastSharedPtr<FBlueprintActionMenuItem>(Entry.Action)->GetRawAction());
            const FProperty* Property = Variable ? Variable->GetVarProperty() : nullptr;
            if (!Property || Property->GetFName() != MemberName) continue;
            ++MenuMembers;
            FDiagnostic Error;
            if (PrepareCandidate(Params, Entry, &Entry - Entries.GetData(), Error))
            {
                ++PreparedMembers;
                if (PreparedMembers <= 4)
                    UE_LOG(LogTemp, Display, TEXT("UAI Blueprint missing member projection: requested=%s current=%s owner=%s self=%s"),
                        *Name, *GetBlueprintActionPublicName(Entry.Candidate), *GetPathNameSafe(Property->GetOwnerClass()),
                        IsAuthoringSelfClass(Blueprint, Property->GetOwnerClass()) ? TEXT("true") : TEXT("false"));
            }
        }
        UE_LOG(LogTemp, Display, TEXT("UAI Blueprint missing member state: name=%s member=%s context=%s authoring=%s transient=%s skeleton=%s generated=%s skeleton_field=%s skeleton_flags=%llu generated_field=%s registered_actions=%d registered=%d menu=%d prepared=%d"),
            *Name, *Member, *Blueprint->GetPathName(), *UeAgentBlueprintAssetLifecycle::GetAuthoringTargetPath(*Blueprint),
            Blueprint->HasAnyFlags(RF_Transient) ? TEXT("true") : TEXT("false"),
            *GetPathNameSafe(Blueprint->SkeletonGeneratedClass), *GetPathNameSafe(Blueprint->GeneratedClass),
            SkeletonField ? TEXT("true") : TEXT("false"), SkeletonField ? static_cast<uint64>(SkeletonField->GetPropertyFlags()) : 0ull,
            GeneratedField ? TEXT("true") : TEXT("false"), Registered ? Registered->Num() : 0,
            RegisteredMembers, MenuMembers, PreparedMembers);
    }

	TMap<FString, FString> BlueprintPublicSelectionFields(const FCandidate& Candidate)
	{
		TMap<FString, FString> Fields;
		for (const auto& Field : Candidate.AuthoringSelectors)
		{
			if (!IsPublicBlueprintCandidateSemanticField(Field.Key)) continue;
			FString Value = Field.Value;
			if (Field.Key == TEXT("node_class") || Field.Key == TEXT("function_owner_class")
				|| Field.Key == TEXT("event_class") || Field.Key == TEXT("member_parent_class")
				|| Field.Key == TEXT("asset_override") || Field.Key == TEXT("class_binding"))
			{
				if (Value.StartsWith(TEXT("/")))
				{
					const FString Source = UE::UAI::ContentDiscovery::MakeReadableDiscoveryPath(Value);
					if (!Source.IsEmpty()) Fields.Add(Field.Key + TEXT("_source"), Source);
					Value = FPackageName::ObjectPathToObjectName(Value);
				}
			}
			if (UE::UAI::ContentDiscovery::IsReadableDiscoveryName(Value)) Fields.Add(Field.Key, MoveTemp(Value));
		}
		return Fields;
	}

	bool TryBuildBlueprintSelectionFailure(
		const FSearchParams& Params, TArray<FBlueprintActionEntry>& Entries,
		const FString& RequestedName, FDiagnostic& OutError)
	{
		TArray<UE::UAI::ContentDiscovery::FNameSelectionEntry> Choices;
		FString BaseName;
		for (FBlueprintActionEntry& Entry : Entries)
		{
			if (!MayMatchNativeName(Entry, RequestedName)) continue;
			if (!PrepareCandidate(Params, Entry, &Entry - Entries.GetData(), OutError))
			{
				if (!OutError.IsEmpty()) return true;
				continue;
			}
			const FString Name = GetBlueprintActionPublicName(Entry.Candidate);
			if (Name.IsEmpty() || !RequestedName.Equals(Name, ESearchCase::IgnoreCase)) continue;
			if (BaseName.IsEmpty()) BaseName = Name;
			if (!Name.Equals(BaseName, ESearchCase::IgnoreCase)) continue;
			UE::UAI::ContentDiscovery::FNameSelectionEntry Choice;
			Choice.Name = BaseName;
			Choice.Identity = Entry.Candidate.Identity.CanonicalKey;
			Choice.Select = BlueprintPublicSelectionFields(Entry.Candidate);
			Choices.Add(MoveTemp(Choice));
		}
		if (Choices.IsEmpty()) return false;
		int32 Index = INDEX_NONE;
		FDiagnostic SelectionFailure;
		if (UE::UAI::ContentDiscovery::TryResolveNameSelection(
			Choices, BaseName, Params.CandidateSelect, Index, SelectionFailure)) return false;
		const UBlueprint* Blueprint = LoadBlueprint(Params.AssetPath);
		OutError = UeAgentAuthoringDiagnostics::CandidateFailure(SelectionFailure.Code, RequestedName,
			Blueprint ? UeAgentBlueprintAssetLifecycle::GetAuthoringTargetPath(*Blueprint) : Params.AssetPath,
			BlueprintCandidateScopeMessage(Params), TEXT("blueprint.node.candidates"), SelectionFailure);
		return true;
	}

	bool TryRegisterSelectedBlueprintCast(
		const FSearchParams& Params, const FString& Name, FDiagnostic& OutError)
	{
		// Registry metadata identifies a registration prerequisite only. The
		// selected action still comes from the native, context-filtered catalog.
		const TCHAR* const NodeClasses[] = {
			TEXT("K2Node_DynamicCast"), TEXT("K2Node_ClassDynamicCast")};
		bool bCastName = false;
		for (const TCHAR* NodeClass : NodeClasses)
			bCastName |= Name.StartsWith(
				BuildCandidateBaseName(TEXT("blueprint_node"), NodeClass, FString()) + TEXT(" ["),
				ESearchCase::IgnoreCase);
		if (!bCastName) return false;

		FAssetRegistryModule& Registry = FModuleManager::LoadModuleChecked<FAssetRegistryModule>(TEXT("AssetRegistry"));
		TArray<FAssetData> Assets;
		Registry.Get().GetAssetsByClass(UBlueprint::StaticClass()->GetClassPathName(), Assets, true);
		TArray<UE::UAI::ContentDiscovery::FScopedNameDiscoveryEntry> ScopedEntries;
		TArray<FSoftObjectPath> BlueprintPaths;
		for (const FAssetData& Asset : Assets)
		{
			const FString ClassPath = FPackageName::ExportTextPathToObjectPath(
				Asset.GetTagValueRef<FString>(FBlueprintTags::GeneratedClassPath));
			if (ClassPath.IsEmpty()) continue;
			for (const TCHAR* NodeClass : NodeClasses)
			{
				FCandidate Metadata;
				Metadata.Type = TEXT("blueprint_node");
				Metadata.Name = BuildCandidateBaseName(Metadata.Type, NodeClass, FString())
					+ TEXT(" [") + ReadableSemanticName(FPackageName::ObjectPathToObjectName(ClassPath)) + TEXT("]");
				if (!Name.Equals(Metadata.Name, ESearchCase::IgnoreCase)
					&& !Name.StartsWith(Metadata.Name + TEXT(" ["), ESearchCase::IgnoreCase)) continue;
				Metadata.AuthoringSelectors.Add(TEXT("node_class"), FString(TEXT("/Script/BlueprintGraph.")) + NodeClass);
				Metadata.AuthoringSelectors.Add(TEXT("class_binding"), ClassPath);
				bool bSelectorsMatch = true;
				for (const auto& Required : Params.RequiredAuthoringSelectors)
					if (const FString* Value = Metadata.AuthoringSelectors.Find(Required.Key))
						bSelectorsMatch &= *Value == Required.Value;
				if (!bSelectorsMatch) continue;
				UE::UAI::ContentDiscovery::FScopedNameDiscoveryEntry Scoped;
				if (!TryBuildBlueprintActionScopedDiscoveryEntry(Metadata, Scoped)) continue;
                // This recovery lookup selects a Blueprint to load, not an
                // executable action. The asset path is its native identity.
                Scoped.Identity = Asset.GetSoftObjectPath().ToString();
				ScopedEntries.Add(MoveTemp(Scoped));
				BlueprintPaths.Add(Asset.GetSoftObjectPath());
			}
		}
		int32 Index = INDEX_NONE;
		FDiagnostic Error;
		UE::UAI::ContentDiscovery::FScopedNameDiscoveryIndex Names(
			ScopedEntries, UE::UAI::ContentDiscovery::ENameComparison::IgnoreCase);
		if (!Names.TryResolve(Name, Index, Error))
		{
			if (Error.Code == TEXT("name_discovery_semantic_scope_ambiguous"))
				SetBlueprintCandidateResolutionFailure(Params, Name, TEXT("blueprint_action_candidate_ambiguous"), OutError);
			return false;
		}
		if (!BlueprintPaths.IsValidIndex(Index)) return false;
		UBlueprint* Blueprint = LoadBlueprint(BlueprintPaths[Index].ToString());
		if (!Blueprint) return false;
		EnsureBlueprintActionRegistration(*Blueprint);
		// Loading adds class actions to the shared native database. All cached
		// menus in this apply batch must be recollected against that database.
		if (ActiveBatch) ActiveBatch->Catalogs.Reset();
		return true;
	}

	bool TryResolveBlueprintActionEntry(
		const FSearchParams& Params,
		const FString& Name,
		const FString& Id,
		FBlueprintActionEntry& OutEntry,
		FDiagnostic& OutError,
        bool bAllowRegistrationRecovery = true)
	{
		OutEntry = {};
		if (Name.TrimStartAndEnd().IsEmpty()
			&& Id.TrimStartAndEnd().IsEmpty())
		{
			OutError = FDiagnostic::Failure(TEXT("blueprint_action_candidate_selector_required"), TEXT("A Blueprint action candidate requires a non-empty name or identity selector."), TEXT("candidate_resolve"));
			OutError.TargetPath = Params.AssetPath; OutError.PropertyPath = TEXT("candidate"); OutError.ExpectedValuePresent = true; OutError.ActualValuePresent = false;
			return false;
		}
		TSharedPtr<FBatchCatalog> Catalog;
		if (!CollectForBatch(Params, Catalog, OutError)) return false;
		TArray<FBlueprintActionEntry>& Entries = Catalog->Entries;
		if (!Name.TrimStartAndEnd().IsEmpty())
		{
			const FString QueryName = Name.TrimStartAndEnd();
			TSharedPtr<FBatchProjection> Projection;
			for (const TSharedPtr<FBatchProjection>& Existing : Catalog->Projections)
			{
				if (Existing->QueryName.Equals(QueryName, ESearchCase::IgnoreCase)
					&& SameSelectors(Existing->RequiredSelectors, Params.RequiredAuthoringSelectors))
				{
					Projection = Existing;
					break;
				}
			}
			if (!Projection)
			{
				Projection = MakeShared<FBatchProjection>();
				Projection->RequiredSelectors = Params.RequiredAuthoringSelectors;
				Projection->QueryName = QueryName;
				auto& ScopedToEntry = Projection->ScopedToEntry;
				Projection->Names.Reserve(Entries.Num());
				for (FBlueprintActionEntry& Entry : Entries)
				{
                    if (!MayMatchNativeName(Entry, QueryName)) continue;
					if (!MayMatchNativeSelectors(Entry, Params.RequiredAuthoringSelectors)) continue;
					if (!PrepareCandidate(Params, Entry, &Entry - Entries.GetData(), OutError))
					{
						if (!OutError.IsEmpty()) return false;
						continue;
					}
					const FString PublicName =
						GetBlueprintActionPublicName(Entry.Candidate);
					if (!UE::UAI::ContentDiscovery::IsReadableDiscoveryName(PublicName))
					{
						continue;
					}
					if (!QueryName.Equals(PublicName, ESearchCase::IgnoreCase))
					{
						continue;
					}
					bool bSelectorsMatch = true;
					for (const TPair<FString, FString>& Required
						: Params.RequiredAuthoringSelectors)
					{
						const FString* Actual = Entry.Candidate.AuthoringSelectors.Find(
							Required.Key);
						if (!Actual || !Actual->Equals(
								Required.Value, ESearchCase::CaseSensitive))
						{
							bSelectorsMatch = false;
							break;
						}
					}
					if (!bSelectorsMatch)
					{
						continue;
					}
					const FString& SemanticIdentity = Entry.Candidate.Identity.CanonicalKey;
					UE::UAI::ContentDiscovery::FNameSelectionEntry Choice;
					Choice.Name = PublicName;
					Choice.Identity = SemanticIdentity;
					Choice.Select = BlueprintPublicSelectionFields(Entry.Candidate);
					Projection->Names.Add(MoveTemp(Choice));
					ScopedToEntry.Add(&Entry - Entries.GetData());
				}
				Catalog->Projections.Add(Projection);
			}
			const auto& ScopedToEntry = Projection->ScopedToEntry;
			int32 ScopedIndex = INDEX_NONE;
			FDiagnostic ScopedError;
			if (!UE::UAI::ContentDiscovery::TryResolveNameSelection(Projection->Names,
				QueryName, Params.CandidateSelect, ScopedIndex, ScopedError, UE::UAI::ContentDiscovery::ENameComparison::IgnoreCase))
			{
				if (ScopedError.IsEmpty()
					|| ScopedError.Code == TEXT("candidate_name_not_found"))
				{
					FDiagnostic RegistrationError;
					if (bAllowRegistrationRecovery
						&& TryRegisterSelectedBlueprintCast(Params, QueryName, RegistrationError))
                        return TryResolveBlueprintActionEntry(Params, Name, Id, OutEntry, OutError, false);
					if (!RegistrationError.IsEmpty())
					{
						OutError = MoveTemp(RegistrationError);
						return false;
					}
					if (Params.RequiredAuthoringSelectors.IsEmpty()
						|| !TryBuildBlueprintSelectionFailure(Params, Entries, QueryName, OutError))
                    {
                        ReportMissingBlueprintMember(Params, Name, Entries);
                        SetBlueprintCandidateResolutionFailure(Params, Name, TEXT("blueprint_action_candidate_not_found"), OutError);
                    }
				}
				else
				{
					OutError = MoveTemp(ScopedError);
				}
				return false;
			}
			if (!ScopedToEntry.IsValidIndex(ScopedIndex)
				|| !Entries.IsValidIndex(ScopedToEntry[ScopedIndex]))
			{
				OutError = FDiagnostic::Failure(TEXT("blueprint_action_candidate_resolved_index_invalid"), !ScopedToEntry.IsValidIndex(ScopedIndex) ? TEXT("The resolved candidate index is outside the scoped Blueprint catalog.") : TEXT("The scoped Blueprint candidate maps to an entry outside the action catalog."), TEXT("candidate_resolve"));
				OutError.CandidateName = Name; OutError.TargetPath = Params.AssetPath; OutError.SourceKind = TEXT("native_result");
				OutError.ActualValue = MakeShared<FJsonValueNumber>(ScopedToEntry.IsValidIndex(ScopedIndex) ? ScopedToEntry[ScopedIndex] : ScopedIndex);
				OutError.ExpectedValue = MakeShared<FJsonValueString>(FString::Printf(TEXT("0 <= index < %d"), ScopedToEntry.IsValidIndex(ScopedIndex) ? Entries.Num() : ScopedToEntry.Num()));
				return false;
			}
			OutEntry = Entries[ScopedToEntry[ScopedIndex]];
			OutError.Reset();
			return true;
		}

		TMap<FString, int32> MatchingCanonicalKeys;
		TMap<FString, int32> MatchingRebindKeys;
		for (int32 Index = 0; Index < Entries.Num(); ++Index)
		{
			if (!PrepareCandidate(Params, Entries[Index], Index, OutError))
			{
				if (!OutError.IsEmpty()) return false;
				continue;
			}
			const FCandidate& Candidate = Entries[Index].Candidate;
			const bool bNameMatches = MatchesExactName(Candidate, Name);
			const bool bIdMatches = Id.TrimStartAndEnd().IsEmpty()
				|| Candidate.GetPublicId().Equals(Id, ESearchCase::IgnoreCase)
				|| Candidate.GetCandidateId().Equals(Id, ESearchCase::CaseSensitive);
			if (bIdMatches && bNameMatches)
			{
				MatchingCanonicalKeys.FindOrAdd(
					Candidate.GetCandidateId(), Index);
			}
			if (!bNameMatches || Params.RequiredAuthoringSelectors.IsEmpty())
			{
				continue;
			}
			bool bSelectorsMatch = true;
			for (const TPair<FString, FString>& Required
				: Params.RequiredAuthoringSelectors)
			{
				const FString* Actual =
					Candidate.AuthoringSelectors.Find(Required.Key);
				if (!Actual || !Actual->Equals(
						Required.Value, ESearchCase::CaseSensitive))
				{
					bSelectorsMatch = false;
					break;
				}
			}
			if (bSelectorsMatch)
			{
				MatchingRebindKeys.FindOrAdd(
					Candidate.GetCandidateId(), Index);
			}
		}
		if (MatchingCanonicalKeys.IsEmpty()
			&& !MatchingRebindKeys.IsEmpty())
		{
			MatchingCanonicalKeys = MoveTemp(MatchingRebindKeys);
		}
		if (MatchingCanonicalKeys.IsEmpty())
		{
			SetBlueprintCandidateResolutionFailure(Params, Name, TEXT("blueprint_action_candidate_not_found"), OutError);
			return false;
		}
		if (MatchingCanonicalKeys.Num() != 1)
		{
			if (!Name.IsEmpty() && TryBuildBlueprintSelectionFailure(Params, Entries, Name, OutError)) return false;
			SetBlueprintCandidateResolutionFailure(Params, Name, TEXT("blueprint_action_candidate_ambiguous"), OutError);
			return false;
		}
		for (const TPair<FString, int32>& Match : MatchingCanonicalKeys)
		{
			OutEntry = Entries[Match.Value];
			break;
		}
		OutError.Reset();
		return true;
	}
}

FBlueprintActionBatchScope::FBlueprintActionBatchScope()
{
	auto* Batch = new FBatchState();
	Batch->Previous = ActiveBatch;
	ActiveBatch = Batch;
	State = Batch;
}

FBlueprintActionBatchScope::~FBlueprintActionBatchScope()
{
	auto* Batch = static_cast<FBatchState*>(State);
	ActiveBatch = Batch->Previous;

	delete Batch;
}

void InvalidateBlueprintActionBatch()
{
	// A nested import/compile also invalidates any suspended outer phase.
	for (FBatchState* Batch = ActiveBatch; Batch; Batch = Batch->Previous)
	{
		Batch->Catalogs.Reset();
	}
}

bool TryResolveBlueprintGraphIdentity(
	const FString& AssetPath,
	const FString& GraphSelector,
	FString& OutGraphId,
	FString& OutGraphName,
	FDiagnostic& OutError)
{
	OutGraphId.Reset();
	OutGraphName.Reset();
	UBlueprint* Blueprint = LoadBlueprint(AssetPath);
	if (!Blueprint)
	{
		OutError = FDiagnostic::Failure(TEXT("blueprint_not_found"), TEXT("The requested Blueprint asset could not be loaded for graph identity resolution."), TEXT("candidate_resolve")); OutError.TargetPath = AssetPath;
		return false;
	}
	UEdGraph* Graph = ResolveGraph(Blueprint, GraphSelector);
	if (!Graph)
	{
		OutError = FDiagnostic::Failure(TEXT("graph_not_found"), TEXT("The Blueprint has no graph matching the requested selector."), TEXT("candidate_resolve")); OutError.TargetPath = AssetPath; OutError.CandidateName = GraphSelector;
		return false;
	}
	if (!Graph->GraphGuid.IsValid())
	{
		OutError = FDiagnostic::Failure(TEXT("graph_identity_invalid"), TEXT("The selected Blueprint graph has no valid native GUID identity."), TEXT("candidate_resolve")); OutError.TargetPath = AssetPath; OutError.CandidateName = Graph->GetName(); OutError.SourceKind = TEXT("native_result");
		return false;
	}
	OutGraphId = TEXT("graph:") + Graph->GraphGuid.ToString(
		EGuidFormats::DigitsWithHyphensLower);
	OutGraphName = Graph->GetName();
	OutError.Reset();
	return true;
}

bool CollectBlueprintCandidates(
	const FSearchParams& Params,
	TArray<FCandidate>& OutCandidates,
	TArray<FString>& OutWarnings,
	FDiagnostic& OutError)
{
	OutCandidates.Reset();
	TArray<FBlueprintActionEntry> Entries;
	if (!CollectBlueprintActionEntries(
			Params, Entries, OutWarnings, OutError))
	{
		return false;
	}
	for (FBlueprintActionEntry& Entry : Entries)
	{
		if (!PrepareCandidate(Params, Entry, OutCandidates.Num(), OutError))
		{
			if (!OutError.IsEmpty()) return false;
			continue;
		}
		Entry.Candidate.UiOrder = OutCandidates.Num();
		OutCandidates.Add(MoveTemp(Entry.Candidate));
	}

	if (OutCandidates.IsEmpty())
	{
		OutWarnings.Add(TEXT(
			"Blueprint action menu returned no supported candidates for the "
			"requested context."));
	}
	return true;
}

bool TryResolveBlueprintActionCandidate(
	const FSearchParams& Params,
	const FString& Name,
	const FString& Id,
	FCandidate& OutCandidate,
	FDiagnostic& OutError)
{
	FBlueprintActionEntry Entry;
	if (!TryResolveBlueprintActionEntry(
			Params, Name, Id, Entry, OutError))
	{
		OutCandidate = {};
		return false;
	}
	OutCandidate = MoveTemp(Entry.Candidate);
	return true;
}

bool TryPerformBlueprintActionCandidate(
	const FSearchParams& Params,
	const FString& Name,
	const FString& Id,
	const FVector2f& Location,
	UEdGraphNode*& OutNode,
	FCandidate& OutCandidate,
	FDiagnostic& OutError)
{
	OutNode = nullptr;
	FBlueprintActionEntry Entry;
	if (!TryResolveBlueprintActionEntry(
			Params, Name, Id, Entry, OutError))
	{
		OutCandidate = {};
		return false;
	}
	const FString TargetPath = Entry.Blueprint
		? UeAgentBlueprintAssetLifecycle::GetAuthoringTargetPath(*Entry.Blueprint) : Params.AssetPath;
	if (!Entry.Candidate.bSupportedInContext)
	{
		OutError = FDiagnostic::WrapFailure(TEXT("blueprint_action_candidate_not_executable"),
			Entry.Candidate.SupportDiagnostic);
		return false;
	}
	if (!Entry.Action.IsValid() || !Entry.Graph)
	{
		OutError = FDiagnostic::Failure(TEXT("blueprint_action_candidate_not_executable"),
			!Entry.Action.IsValid() ? TEXT("The selected Blueprint candidate has no native action to execute.")
				: TEXT("The selected Blueprint action has no target graph."), TEXT("candidate_resolve"));
		OutError.CandidateName = Name;
		OutError.TargetPath = TargetPath;
		OutError.SourceKind = TEXT("native_result");
		OutError.PropertyPath = !Entry.Action.IsValid() ? TEXT("native_action_available") : TEXT("target_graph_available");
		OutError.ActualValue = MakeShared<FJsonValueBoolean>(false);
		OutError.ActualValuePresent = true;
		OutError.ExpectedValue = MakeShared<FJsonValueBoolean>(true);
		return false;
	}
	TSet<UEdGraphNode*> ExistingNodes;
	for (UEdGraphNode* ExistingNode : Entry.Graph->Nodes)
	{
		if (ExistingNode)
		{
			ExistingNodes.Add(ExistingNode);
		}
	}
	const UBlueprintNodeSpawner* Spawner = nullptr;
	IBlueprintNodeBinder::FBindingSet Bindings;
	if (Entry.Action->GetTypeId() == FBlueprintActionMenuItem::StaticGetTypeId())
	{
		const auto Item = StaticCastSharedPtr<FBlueprintActionMenuItem>(Entry.Action);
		Spawner = Item->GetRawAction();
		if (!UE::UAI::ContentDiscovery::NativeNodeAction::ReadBindings(*Item, Entry.ActionContext, Bindings, OutError))
		{
			if (OutError.TargetPath.IsEmpty()) OutError.TargetPath = TargetPath;
			if (OutError.CandidateName.IsEmpty()) OutError.CandidateName = Name;
			return false;
		}
	}
	const FScopedTransaction Transaction(NSLOCTEXT("UeAgentInterface", "CreateBlueprintCandidate", "Create Blueprint Node"));
	OutNode = Spawner
		? UE::UAI::ContentDiscovery::NativeNodeAction::Invoke(*Spawner, *Entry.Graph, Entry.Pin, Location, Bindings)
		: UE::UAI::ContentDiscovery::NativeNodeAction::InvokeSchemaAction(*Entry.Action, *Entry.Graph, Entry.Pin, Location);
	if (!OutNode)
	{
		OutError = FDiagnostic::Failure(TEXT("blueprint_action_candidate_create_failed"), TEXT("Executing the native Blueprint action returned no node."), TEXT("candidate_apply"));
		OutError.CandidateName = Name; OutError.TargetPath = TargetPath; OutError.SourceKind = TEXT("native_result");
		OutError.PropertyPath = TEXT("node_created"); OutError.ActualValue = MakeShared<FJsonValueBoolean>(false);
		OutError.ActualValuePresent = true; OutError.ExpectedValue = MakeShared<FJsonValueBoolean>(true);
		return false;
	}
	TArray<UEdGraphNode*> CreatedNodes;
	for (UEdGraphNode* CurrentNode : Entry.Graph->Nodes)
	{
		if (CurrentNode && !ExistingNodes.Contains(CurrentNode))
		{
			CreatedNodes.Add(CurrentNode);
		}
	}
	if (CreatedNodes.IsEmpty())
	{
		OutNode = nullptr;
		OutError = FDiagnostic::Failure(TEXT("blueprint_action_candidate_no_new_node"), TEXT("The native Blueprint action returned a node but added no new node to the target graph."), TEXT("candidate_apply")); OutError.CandidateName = Name; OutError.TargetPath = TargetPath; OutError.SourceKind = TEXT("native_result"); OutError.ActualValue = MakeShared<FJsonValueNumber>(0);
		OutError.PropertyPath = TEXT("created_node_count"); OutError.ActualValuePresent = true;
		OutError.ExpectedValue = MakeShared<FJsonValueString>(TEXT("at least one newly created node"));
		return false;
	}
	if (ExistingNodes.Contains(OutNode) || !CreatedNodes.Contains(OutNode))
	{
		OutError = FDiagnostic::Failure(TEXT("blueprint_action_candidate_primary_not_new"), ExistingNodes.Contains(OutNode) ? TEXT("The Blueprint action returned an existing node instead of a newly created node.") : TEXT("The Blueprint action returned a primary node that is absent from the newly created graph nodes."), TEXT("candidate_apply"));
		OutError.CandidateName = Name; OutError.TargetPath = TargetPath; OutError.SourceKind = TEXT("native_result"); OutError.InternalContext = GetPathNameSafe(OutNode);
		OutError.PropertyPath = TEXT("primary_node_is_new"); OutError.ActualValue = MakeShared<FJsonValueBoolean>(false);
		OutError.ActualValuePresent = true; OutError.ExpectedValue = MakeShared<FJsonValueBoolean>(true);
		OutNode = nullptr;
		return false;
	}
	if (OutNode->GetGraph() != Entry.Graph
		|| CreatedNodes.ContainsByPredicate(
			[Graph = Entry.Graph](const UEdGraphNode* CreatedNode)
			{
				return !CreatedNode || CreatedNode->GetGraph() != Graph;
			}))
	{
		const UEdGraphNode* WrongNode = OutNode;
		if (OutNode->GetGraph() == Entry.Graph) for (const UEdGraphNode* CreatedNode : CreatedNodes) if (!CreatedNode || CreatedNode->GetGraph() != Entry.Graph) { WrongNode = CreatedNode; break; }
		OutError = FDiagnostic::Failure(TEXT("blueprint_action_candidate_graph_mismatch"), TEXT("A node created by the Blueprint action does not belong to the requested target graph."), TEXT("candidate_apply"));
		OutError.CandidateName = Name; OutError.TargetPath = TargetPath; OutError.SourceKind = TEXT("native_result");
		OutError.ExpectedValue = MakeShared<FJsonValueString>(Entry.Graph->GetName()); OutError.ActualValue = MakeShared<FJsonValueString>(WrongNode ? GetNameSafe(WrongNode->GetGraph()) : TEXT("missing node"));
		OutError.PropertyPath = TEXT("node_graph_name"); OutError.ActualValuePresent = true;
		OutNode = nullptr;
		return false;
	}
	OutCandidate = MoveTemp(Entry.Candidate);
	OutError.Reset();
	return true;
}
}
