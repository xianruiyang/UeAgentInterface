#include "QueryOwners/AnimationBlueprint/UeAgentAnimationBlueprintQueryOwner.h"

#include "Algo/Sort.h"
#include "AnimationStateMachineGraph.h"
#include "AnimationStateMachineSchema.h"
#include "AnimGraphNode_StateMachine.h"
#include "AnimGraphNode_StateMachineBase.h"
#include "AnimStateAliasNode.h"
#include "AnimStateConduitNode.h"
#include "AnimStateEntryNode.h"
#include "AnimStateNode.h"
#include "Commands/PaletteSearch/UeAgentPaletteSearchTypes.h"
#include "Domains/Animation/Authoring/UeAgentAnimationAuthoringDocuments.h"
#include "Domains/Animation/UeAgentAnimBlueprintDomainBackend.h"
#include "Dom/JsonObject.h"
#include "EdGraph/EdGraph.h"
#include "EdGraph/EdGraphSchema.h"
#include "Misc/PackageName.h"
#include "UObject/StrongObjectPtr.h"
#include "UObject/UObjectGlobals.h"

namespace UE::UAI::QueryOwners::AnimationBlueprint
{
	namespace
	{
		const FString AnimationGraphSurface = TEXT("animation_graph");
		const FString StateMachineGraphSurface = TEXT("state_machine_graph");
		const FString AnimationGraphCoverage =
			TEXT("anim_graph_native_state_machine_node_family");
		const FString StateMachineGraphCoverage =
			TEXT("new_state_machine_blank_graph_native_menu");

		struct FStateMachineActionEntry final
		{
			FAnimationBlueprintCandidate Candidate;
			TSharedPtr<FEdGraphSchemaAction> Action;
		};

		struct FStateMachineActionCatalog final
		{
			TStrongObjectPtr<UEdGraph> ActionOwner;
			TArray<FStateMachineActionEntry> Entries;
		};

		bool Fail(
			FAnimationBlueprintCandidateQueryError& OutError,
			FString Code,
			FString SubjectId = FString(),
			FString Detail = FString())
		{
			OutError.Code = MoveTemp(Code);
			OutError.SubjectId = MoveTemp(SubjectId);
			OutError.Detail = MoveTemp(Detail);
			return false;
		}

		FString Normalize(const FString& Value)
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
		}

		FString ObjectPath(const FString& Target)
		{
			if (Target.Contains(TEXT(".")))
			{
				return Target;
			}
			const FString Name = FPackageName::GetLongPackageAssetName(Target);
			return Name.IsEmpty() ? Target : Target + TEXT(".") + Name;
		}

		bool MatchesQuery(
			const FAnimationBlueprintCandidate& Candidate,
			const FString& Query)
		{
			const FString Needle = Normalize(Query);
			return Needle.IsEmpty()
				|| Normalize(Candidate.Name + TEXT("|")
					+ Candidate.Label + TEXT("|")
					+ Candidate.Category + TEXT("|")
					+ Candidate.NodeClassPath).Contains(Needle);
		}

		bool SetCandidateIdentity(
			const FString& CatalogType,
			const FString& Signature,
			FAnimationBlueprintCandidate& Candidate,
			FAnimationBlueprintCandidateQueryError& OutError)
		{
			UeAgentPaletteSearch::FCandidate Identity;
			Identity.Kind = Candidate.Kind;
			Identity.Name = Candidate.Name;
			Identity.DisplayName = Candidate.Label;
			Identity.CategoryPath = Candidate.Category;
			Identity.Tooltip = Candidate.Summary;
			Identity.Type = Candidate.ActionKind;
			Identity.FullName = Candidate.NodeClassPath;
			Identity.ObjectPath = Candidate.NodeClassPath;
			FString IdentityError;
			if (Candidate.Name.IsEmpty()
				|| !UeAgentPaletteSearch::SetCandidateIdentity(
					CatalogType,
					Signature,
					Candidate.NodeClassPath.IsEmpty()
						? Candidate.Name
						: Candidate.NodeClassPath,
					Identity,
					IdentityError))
			{
				return Fail(
					OutError,
					TEXT("animation_blueprint_candidate_identity_failed"),
					Candidate.Name,
					IdentityError);
			}
			Candidate.Id = Identity.GetPublicId();
			return true;
		}

		bool BuildAnimationGraphCatalog(
			UAnimBlueprint& Blueprint,
			UEdGraph& Graph,
			TArray<FAnimationBlueprintCandidate>& OutCandidates,
			FAnimationBlueprintCandidateQueryError& OutError)
		{
			UeAgentPaletteSearch::FSearchParams Params;
			Params.CatalogType = TEXT("blueprint_node");
			Params.AssetPath = Blueprint.GetPathName();
			Params.GraphName = Graph.GetName();
			Params.bContextSensitive = true;
			TArray<UeAgentPaletteSearch::FCandidate> RawCandidates;
			TArray<FString> Warnings;
			FString SearchError;
			if (!UeAgentPaletteSearch::CollectBlueprintCandidates(
					Params, RawCandidates, Warnings, SearchError))
			{
				return Fail(
					OutError,
					TEXT("animation_blueprint_candidate_catalog_failed"),
					Graph.GetPathName(),
					SearchError);
			}

			OutCandidates.Reset();
			TSet<FString> SeenIds;
			for (const UeAgentPaletteSearch::FCandidate& Raw : RawCandidates)
			{
				UClass* const NodeClass = FindObject<UClass>(
					nullptr, *Raw.ObjectPath);
				if (!NodeClass
					|| !NodeClass->IsChildOf(
						UAnimGraphNode_StateMachineBase::StaticClass()))
				{
					continue;
				}

				FAnimationBlueprintCandidate Candidate;
				Candidate.Id = Raw.GetPublicId();
				Candidate.Name = Raw.Name;
				Candidate.Label = Raw.DisplayName.IsEmpty()
					? Raw.Name
					: Raw.DisplayName;
				Candidate.Surface = AnimationGraphSurface;
				Candidate.Kind = TEXT("anim_graph_state_machine_node");
				Candidate.ActionKind = TEXT("blueprint_node_spawner");
				Candidate.Category = Raw.CategoryPath;
				Candidate.Summary = Raw.Tooltip;
				Candidate.NodeClassPath = NodeClass->GetPathName();
				Candidate.SourceOrigin = Raw.SourceOrigin;
				Candidate.bSupported =
					NodeClass == UAnimGraphNode_StateMachine::StaticClass();
				Candidate.WriteSupport = Candidate.bSupported
					? FString(TEXT("state_machine_create"))
					: FString(TEXT("search_only"));
				Candidate.SupportReason = Candidate.bSupported
					? FString(TEXT("native_blueprint_node_spawner"))
					: FString(TEXT("state_machine_node_variant_not_yet_supported"));
				if (Candidate.Id.IsEmpty() || SeenIds.Contains(Candidate.Id))
				{
					return Fail(
						OutError,
						TEXT("animation_blueprint_candidate_identity_ambiguous"),
						Candidate.Id,
						Candidate.NodeClassPath);
				}
				SeenIds.Add(Candidate.Id);
				OutCandidates.Add(MoveTemp(Candidate));
			}
			Algo::Sort(OutCandidates,
				[](const FAnimationBlueprintCandidate& A,
					const FAnimationBlueprintCandidate& B)
				{
					const int32 NameOrder = A.Label.Compare(B.Label);
					return NameOrder != 0
						? NameOrder < 0
						: A.NodeClassPath < B.NodeClassPath;
				});
			return true;
		}

		bool BuildStateMachineGraphCatalog(
			UAnimationStateMachineGraph& Graph,
			FStateMachineActionCatalog& OutCatalog,
			FAnimationBlueprintCandidateQueryError& OutError)
		{
			OutCatalog.ActionOwner.Reset();
			OutCatalog.Entries.Reset();
			const UAnimationStateMachineSchema* const Schema =
				Cast<UAnimationStateMachineSchema>(Graph.GetSchema());
			if (!Schema)
			{
				return Fail(
					OutError,
					TEXT("animation_blueprint_candidate_context_invalid"),
					Graph.GetPathName(),
					TEXT("the graph does not use UAnimationStateMachineSchema"));
			}

			FGraphContextMenuBuilder MenuBuilder(&Graph);
			MenuBuilder.FromPin = nullptr;
			Schema->GetGraphContextActions(MenuBuilder);
			OutCatalog.ActionOwner.Reset(MenuBuilder.OwnerOfTemporaries);
			TSet<FString> SeenIds;
			for (int32 Index = 0; Index < MenuBuilder.GetNumActions(); ++Index)
			{
				const TSharedPtr<FEdGraphSchemaAction> Action =
					MenuBuilder.GetSchemaAction(Index);
				if (!Action.IsValid())
				{
					return Fail(
						OutError,
						TEXT("animation_blueprint_candidate_native_action_invalid"),
						FString::FromInt(Index));
				}

				FStateMachineActionEntry& Entry =
					OutCatalog.Entries.AddDefaulted_GetRef();
				Entry.Action = Action;
				FAnimationBlueprintCandidate& Candidate = Entry.Candidate;
				Candidate.Name = Action->GetMenuDescription().ToString();
				Candidate.Label = Candidate.Name;
				Candidate.Surface = StateMachineGraphSurface;
				Candidate.Category = Action->GetCategory().ToString();
				Candidate.Summary = Action->GetTooltipDescription().ToString();
				Candidate.SourceOrigin = TEXT("native_state_machine_schema");
				FString Signature = StateMachineGraphCoverage
					+ TEXT("|") + Action->GetTypeId().ToString();

				TArray<UObject*> ReferencedObjects;
				FReferenceFinder ReferenceFinder(
					ReferencedObjects,
					MenuBuilder.OwnerOfTemporaries,
					true,
					true,
					false,
					false);
				Action->AddReferencedObjects(ReferenceFinder);
				UEdGraphNode* NodeTemplate = nullptr;
				for (UObject* ReferencedObject : ReferencedObjects)
				{
					UEdGraphNode* const ReferencedNode =
						Cast<UEdGraphNode>(ReferencedObject);
					if (!ReferencedNode)
					{
						continue;
					}
					if (NodeTemplate && NodeTemplate != ReferencedNode)
					{
						return Fail(
							OutError,
							TEXT("animation_blueprint_candidate_native_template_ambiguous"),
							Candidate.Name,
							TEXT("the native action references more than one graph-node template"));
					}
					NodeTemplate = ReferencedNode;
				}

				if (NodeTemplate)
				{
					UClass* const NodeClass = NodeTemplate->GetClass();
					Candidate.NodeClassPath = NodeClass->GetPathName();
					Signature += TEXT("|") + Candidate.NodeClassPath;
					Candidate.ActionKind = TEXT("state_machine_schema_new_node");
					if (NodeClass == UAnimStateNode::StaticClass())
					{
						Candidate.Kind = TEXT("animation_state");
						Candidate.bSupported = true;
					}
					else if (NodeClass == UAnimStateAliasNode::StaticClass())
					{
						Candidate.Kind = TEXT("animation_state_alias");
					}
					else if (NodeClass == UAnimStateConduitNode::StaticClass())
					{
						Candidate.Kind = TEXT("animation_state_conduit");
					}
					else if (NodeClass == UAnimStateEntryNode::StaticClass())
					{
						Candidate.Kind = TEXT("animation_state_entry");
					}
					else
					{
						Candidate.Kind = TEXT("animation_state_machine_node");
					}
				}
				else
				{
					Candidate.Kind = TEXT("animation_state_machine_menu_action");
					Candidate.ActionKind = TEXT("state_machine_schema_comment");
					Signature += TEXT("|") + Candidate.Name;
				}
				Candidate.WriteSupport = Candidate.bSupported
					? FString(TEXT("state_create"))
					: FString(TEXT("search_only"));
				Candidate.SupportReason = Candidate.bSupported
					? FString(TEXT("native_state_machine_schema_action"))
					: FString(TEXT("native_action_has_no_current_hjson_create_contract"));
				if (!SetCandidateIdentity(
						TEXT("animation_state_machine_graph"),
						Signature,
						Candidate,
						OutError)
					|| SeenIds.Contains(Candidate.Id))
				{
					return Candidate.Id.IsEmpty()
						? false
						: Fail(
							OutError,
							TEXT("animation_blueprint_candidate_identity_ambiguous"),
							Candidate.Id,
							Candidate.Name);
				}
				SeenIds.Add(Candidate.Id);
			}
			return true;
		}

		bool MatchSelector(
			const FAnimationBlueprintCandidate& Candidate,
			const FString& Name,
			const FString& Id)
		{
			const FString CleanName = Name.TrimStartAndEnd();
			const FString CleanId = Id.TrimStartAndEnd();
			const bool bIdMatches = CleanId.IsEmpty()
				|| Candidate.Id.Equals(CleanId, ESearchCase::IgnoreCase);
			const bool bNameMatches = CleanName.IsEmpty()
				|| Candidate.Name.Equals(CleanName, ESearchCase::IgnoreCase)
				|| Candidate.Label.Equals(CleanName, ESearchCase::IgnoreCase)
				|| Candidate.NodeClassPath.Equals(
					CleanName, ESearchCase::IgnoreCase);
			return bIdMatches && bNameMatches;
		}

		template <typename EntryType>
		bool ResolveEntry(
			const TArray<EntryType>& Entries,
			const FString& Name,
			const FString& Id,
			const EntryType*& OutEntry,
			FAnimationBlueprintCandidateQueryError& OutError)
		{
			OutEntry = nullptr;
			TArray<const EntryType*> Matches;
			for (const EntryType& Entry : Entries)
			{
				if (MatchSelector(Entry.Candidate, Name, Id)
					&& (!Name.TrimStartAndEnd().IsEmpty()
						|| !Id.TrimStartAndEnd().IsEmpty()
						|| Entry.Candidate.bSupported))
				{
					Matches.Add(&Entry);
				}
			}
			if (Matches.IsEmpty())
			{
				return Fail(
					OutError,
					TEXT("animation_blueprint_candidate_not_found"),
					Id.IsEmpty() ? Name : Id);
			}
			if (Matches.Num() != 1)
			{
				return Fail(
					OutError,
					TEXT("animation_blueprint_candidate_ambiguous"),
					Name,
					TEXT("use exact candidate id"));
			}
			if (!Matches[0]->Candidate.bSupported)
			{
				return Fail(
					OutError,
					TEXT("animation_blueprint_candidate_not_writable"),
					Matches[0]->Candidate.Id,
					Matches[0]->Candidate.SupportReason);
			}
			OutEntry = Matches[0];
			return true;
		}

		void RetireScratch(TStrongObjectPtr<UAnimBlueprint>& Scratch)
		{
			if (!Scratch.IsValid())
			{
				return;
			}
			Scratch->ClearFlags(RF_Public | RF_Standalone);
			Scratch->MarkAsGarbage();
			Scratch.Reset();
		}

		bool CreateScratch(
			const TSharedPtr<FJsonObject>& ClassSettings,
			TStrongObjectPtr<UAnimBlueprint>& OutScratch,
			FAnimationBlueprintCandidateQueryError& OutError)
		{
			const TSharedPtr<FJsonObject> SeedDocument = MakeShared<FJsonObject>();
			SeedDocument->SetObjectField(
				TEXT("settings"),
				ClassSettings.IsValid()
					? ClassSettings
					: MakeShared<FJsonObject>());
			FString NativeError;
			UObject* const Object = UeAgentAnimationAuthoring::CreateFactorySeed(
				UeAgentAnimationAuthoring::EAssetKind::AnimBlueprint,
				GetTransientPackage(),
				*FString(TEXT("ABP_UAI_Candidate_")
					+ FGuid::NewGuid().ToString(EGuidFormats::Digits)),
				RF_Transient,
				SeedDocument,
				NativeError);
			OutScratch.Reset(Cast<UAnimBlueprint>(Object));
			if (!OutScratch.IsValid())
			{
				return Fail(
					OutError,
					TEXT("animation_blueprint_candidate_context_create_failed"),
					TEXT("AnimBlueprint"),
					NativeError);
			}
			return true;
		}

		void ProjectPage(
			TArray<FAnimationBlueprintCandidate>&& Catalog,
			const FString& Query,
			const FString& CandidateId,
			const int32 Limit,
			const int32 Cursor,
			FAnimationBlueprintCandidateQueryResult& OutResult,
			FAnimationBlueprintCandidateQueryError& OutError)
		{
			const FString CleanId = CandidateId.TrimStartAndEnd();
			TArray<FAnimationBlueprintCandidate> Matches;
			for (FAnimationBlueprintCandidate& Candidate : Catalog)
			{
				if ((!CleanId.IsEmpty()
						&& !Candidate.Id.Equals(
							CleanId, ESearchCase::IgnoreCase))
					|| !MatchesQuery(Candidate, Query))
				{
					continue;
				}
				Matches.Add(MoveTemp(Candidate));
			}
			if (!CleanId.IsEmpty() && Matches.IsEmpty())
			{
				Fail(
					OutError,
					TEXT("animation_blueprint_candidate_not_found"),
					CleanId);
				return;
			}
			OutResult.TotalMatches = Matches.Num();
			const int32 End = FMath::Min(Cursor + Limit, Matches.Num());
			for (int32 Index = Cursor; Index < End; ++Index)
			{
				OutResult.Candidates.Add(MoveTemp(Matches[Index]));
			}
			OutResult.bHasMore = End < Matches.Num();
			OutResult.NextCursor = OutResult.bHasMore ? End : INDEX_NONE;
			OutResult.bComplete = !OutResult.bHasMore;
		}
	}

	bool TryQueryAnimationBlueprintCandidates(
		const FString& Target,
		const FString& Surface,
		const TSharedPtr<FJsonObject>& ClassSettings,
		const FString& Query,
		FAnimationBlueprintCandidateQueryResult& OutResult,
		FAnimationBlueprintCandidateQueryError& OutError,
		const int32 Limit,
		const int32 Cursor,
		const FString& CandidateId)
	{
		OutResult = {};
		OutError = {};
		if (!IsInGameThread())
		{
			return Fail(
				OutError,
				TEXT("animation_blueprint_candidate_game_thread_required"),
				Target);
		}
		if (Target.TrimStartAndEnd().IsEmpty())
		{
			return Fail(
				OutError,
				TEXT("animation_blueprint_candidate_target_required"));
		}
		if (Limit < 1 || Limit > 64 || Cursor < 0)
		{
			return Fail(
				OutError,
				TEXT("animation_blueprint_candidate_page_invalid"),
				Target,
				TEXT("limit must be 1..64 and cursor must be non-negative"));
		}
		const FString CleanSurface = Surface.TrimStartAndEnd().ToLower();
		if (CleanSurface != AnimationGraphSurface
			&& CleanSurface != StateMachineGraphSurface)
		{
			return Fail(
				OutError,
				TEXT("animation_blueprint_candidate_surface_invalid"),
				Surface,
				TEXT("use animation_graph or state_machine_graph"));
		}
		if (StaticLoadObject(UObject::StaticClass(), nullptr, *ObjectPath(Target)))
		{
			return Fail(
				OutError,
				TEXT("animation_blueprint_candidate_target_already_exists"),
				Target,
				TEXT("T16 candidate query currently binds only a missing create target"));
		}

		TStrongObjectPtr<UAnimBlueprint> Scratch;
		if (!CreateScratch(ClassSettings, Scratch, OutError))
		{
			return false;
		}
		UEdGraph* const AnimGraph =
			UeAgentAnimBlueprintOps::ResolveAnyAnimBlueprintGraph(
				Scratch.Get(), TEXT("AnimGraph"));
		if (!AnimGraph)
		{
			RetireScratch(Scratch);
			return Fail(
				OutError,
				TEXT("animation_blueprint_candidate_context_invalid"),
				Target,
				TEXT("the factory seed did not create AnimGraph"));
		}

		TArray<FAnimationBlueprintCandidate> Catalog;
		if (CleanSurface == AnimationGraphSurface)
		{
			if (!BuildAnimationGraphCatalog(
					*Scratch, *AnimGraph, Catalog, OutError))
			{
				RetireScratch(Scratch);
				return false;
			}
			OutResult.CoverageScope = AnimationGraphCoverage;
			OutResult.bCoverageComplete = true;
			OutResult.bCreationCoverageComplete =
				!Catalog.ContainsByPredicate(
					[](const FAnimationBlueprintCandidate& Candidate)
					{
						return !Candidate.bSupported;
					});
		}
		else
		{
			UEdGraphNode* MachineNode = nullptr;
			FAnimationBlueprintCandidate MachineCandidate;
			if (!TryPerformAnimationGraphStateMachineCandidate(
					*Scratch,
					*AnimGraph,
					FString(),
					FString(),
					FVector2f::ZeroVector,
					MachineNode,
					MachineCandidate,
					OutError))
			{
				RetireScratch(Scratch);
				return false;
			}
			UAnimGraphNode_StateMachine* const StateMachineNode =
				Cast<UAnimGraphNode_StateMachine>(MachineNode);
			if (!StateMachineNode || !StateMachineNode->EditorStateMachineGraph)
			{
				RetireScratch(Scratch);
				return Fail(
					OutError,
					TEXT("animation_blueprint_candidate_context_invalid"),
					Target,
					TEXT("the native StateMachine spawner did not create its graph"));
			}
			FStateMachineActionCatalog NativeCatalog;
			if (!BuildStateMachineGraphCatalog(
					*StateMachineNode->EditorStateMachineGraph,
					NativeCatalog,
					OutError))
			{
				RetireScratch(Scratch);
				return false;
			}
			for (FStateMachineActionEntry& Entry : NativeCatalog.Entries)
			{
				Catalog.Add(MoveTemp(Entry.Candidate));
			}
			OutResult.CoverageScope = StateMachineGraphCoverage;
			OutResult.bCoverageComplete = true;
			OutResult.bCreationCoverageComplete = false;
		}

		OutResult.Target = Target.TrimStartAndEnd();
		OutResult.TargetRevision = TEXT("missing");
		OutResult.Surface = CleanSurface;
		ProjectPage(
			MoveTemp(Catalog),
			Query,
			CandidateId,
			Limit,
			Cursor,
			OutResult,
			OutError);
		RetireScratch(Scratch);
		return OutError.Code.IsEmpty();
	}

	bool TryPerformAnimationGraphStateMachineCandidate(
		UAnimBlueprint& Blueprint,
		UEdGraph& Graph,
		const FString& Name,
		const FString& Id,
		const FVector2f& Location,
		UEdGraphNode*& OutNode,
		FAnimationBlueprintCandidate& OutCandidate,
		FAnimationBlueprintCandidateQueryError& OutError)
	{
		OutNode = nullptr;
		OutCandidate = {};
		OutError = {};
		TArray<FAnimationBlueprintCandidate> Catalog;
		if (!BuildAnimationGraphCatalog(
				Blueprint, Graph, Catalog, OutError))
		{
			return false;
		}
		struct FProjectedEntry final
		{
			FAnimationBlueprintCandidate Candidate;
		};
		TArray<FProjectedEntry> Entries;
		for (FAnimationBlueprintCandidate& Candidate : Catalog)
		{
			Entries.Add({MoveTemp(Candidate)});
		}
		const FProjectedEntry* Selected = nullptr;
		if (!ResolveEntry(Entries, Name, Id, Selected, OutError))
		{
			return false;
		}

		UeAgentPaletteSearch::FSearchParams Params;
		Params.CatalogType = TEXT("blueprint_node");
		Params.AssetPath = Blueprint.GetPathName();
		Params.GraphName = Graph.GetName();
		Params.bContextSensitive = true;
		UeAgentPaletteSearch::FCandidate NativeCandidate;
		FString NativeError;
		if (!UeAgentPaletteSearch::TryPerformBlueprintActionCandidate(
				Params,
				Selected->Candidate.Name,
				Selected->Candidate.Id,
				Location,
				OutNode,
				NativeCandidate,
				NativeError))
		{
			return Fail(
				OutError,
				TEXT("animation_blueprint_candidate_create_failed"),
				Selected->Candidate.Id,
				NativeError);
		}
		OutCandidate = Selected->Candidate;
		return true;
	}

	bool TryPerformStateMachineGraphCandidate(
		UAnimationStateMachineGraph& Graph,
		const FString& Name,
		const FString& Id,
		const FVector2f& Location,
		UEdGraphNode*& OutNode,
		FAnimationBlueprintCandidate& OutCandidate,
		FAnimationBlueprintCandidateQueryError& OutError)
	{
		OutNode = nullptr;
		OutCandidate = {};
		OutError = {};
		FStateMachineActionCatalog Catalog;
		if (!BuildStateMachineGraphCatalog(Graph, Catalog, OutError))
		{
			return false;
		}
		const FStateMachineActionEntry* Selected = nullptr;
		if (!ResolveEntry(Catalog.Entries, Name, Id, Selected, OutError))
		{
			return false;
		}
		TSet<UEdGraphNode*> ExistingNodes;
		for (UEdGraphNode* Existing : Graph.Nodes)
		{
			if (Existing)
			{
				ExistingNodes.Add(Existing);
			}
		}
		OutNode = Selected->Action->PerformAction(
			&Graph, nullptr, Location, false);
		if (!OutNode)
		{
			return Fail(
				OutError,
				TEXT("animation_blueprint_candidate_create_failed"),
				Selected->Candidate.Id);
		}
		TArray<UEdGraphNode*> CreatedNodes;
		for (UEdGraphNode* Current : Graph.Nodes)
		{
			if (Current && !ExistingNodes.Contains(Current))
			{
				CreatedNodes.Add(Current);
			}
		}
		if (CreatedNodes.Num() != 1 || CreatedNodes[0] != OutNode)
		{
			OutNode = nullptr;
			return Fail(
				OutError,
				TEXT("animation_blueprint_candidate_result_shape_unsupported"),
				Selected->Candidate.Id,
				FString::FromInt(CreatedNodes.Num()));
		}
		OutCandidate = Selected->Candidate;
		return true;
	}

	bool IsReadOnlyContentGraphCommand(const FString& CommandId)
	{
		return CommandId.StartsWith(TEXT("content_graph."))
			&& CommandId != TEXT("content_graph.import_workspace");
	}
}
