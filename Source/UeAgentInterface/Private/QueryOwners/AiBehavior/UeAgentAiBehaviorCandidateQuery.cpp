#include "QueryOwners/AiBehavior/UeAgentAiBehaviorCandidateQuery.h"

#include "AIGraphTypes.h"
#include "Algo/Sort.h"
#include "BehaviorTree/BTCompositeNode.h"
#include "BehaviorTree/BTNode.h"
#include "BehaviorTree/BTTaskNode.h"
#include "BehaviorTree/BehaviorTree.h"
#include "BehaviorTree/Blackboard/BlackboardKeyType.h"
#include "BehaviorTree/BlackboardData.h"
#include "BehaviorTree/BTDecorator.h"
#include "BehaviorTree/BTService.h"
#include "BehaviorTree/Decorators/BTDecorator_BlueprintBase.h"
#include "BehaviorTree/Services/BTService_BlueprintBase.h"
#include "BehaviorTree/Tasks/BTTask_BlueprintBase.h"
#include "Blueprint/StateTreeNodeBlueprintBase.h"
#include "Commands/PaletteSearch/UeAgentPaletteSearchTypes.h"
#include "DomainAdapters/ContentGraphRead/UeAgentContentGraphReadProductionEnvironment.h"
#include "EdGraph/EdGraphSchema.h"
#include "EdGraphSchema_BehaviorTree.h"
#include "EnvironmentQuery/EnvQuery.h"
#include "EnvironmentQuery/EnvQueryGenerator.h"
#include "BehaviorTreeGraph.h"
#include "BehaviorTreeGraphNode.h"
#include "SmartObjectDefinition.h"
#include "StateTree.h"
#include "StateTreeConditionBase.h"
#include "StateTreeEvaluatorBase.h"
#include "StateTreeTaskBase.h"
#include "QueryOwners/AiBehavior/UeAgentBehaviorTreeHeadlessSchema.h"
#include "UObject/StrongObjectPtr.h"
#include "UObject/UObjectIterator.h"

#include <openssl/sha.h>

void UUeAgentBehaviorTreeHeadlessSchema::InitializeHeadlessClassCache()
{
	if (HeadlessClassCache.IsValid())
	{
		return;
	}
	HeadlessClassCache = MakeShared<FGraphNodeClassHelper>(
		UBTNode::StaticClass());
	FGraphNodeClassHelper::AddObservedBlueprintClasses(
		UBTTask_BlueprintBase::StaticClass());
	FGraphNodeClassHelper::AddObservedBlueprintClasses(
		UBTDecorator_BlueprintBase::StaticClass());
	FGraphNodeClassHelper::AddObservedBlueprintClasses(
		UBTService_BlueprintBase::StaticClass());
	HeadlessClassCache->UpdateAvailableBlueprintClasses();
}

FGraphNodeClassHelper&
UUeAgentBehaviorTreeHeadlessSchema::GetClassCache() const
{
	check(HeadlessClassCache.IsValid());
	return *HeadlessClassCache;
}

FGraphNodeClassHelper&
UUeAgentBehaviorTreeHeadlessSchema::GetHeadlessClassCache() const
{
	return GetClassCache();
}

namespace UE::UAI::QueryOwners::AiBehavior
{
	using namespace DomainAdapters::ContentGraphRead;

	namespace
	{
		bool Fail(
			FAiBehaviorCandidateQueryError& OutError,
			FString Code,
			FString SubjectId = FString(),
			FString Detail = FString())
		{
			OutError.Code = MoveTemp(Code);
			OutError.SubjectId = MoveTemp(SubjectId);
			OutError.Detail = MoveTemp(Detail);
			return false;
		}

		FString Sha256(const FString& Text)
		{
			const FTCHARToUTF8 Utf8(*Text);
			uint8 Digest[SHA256_DIGEST_LENGTH];
			SHA256(reinterpret_cast<const uint8*>(Utf8.Get()), Utf8.Length(), Digest);
			FString Hex;
			Hex.Reserve(SHA256_DIGEST_LENGTH * 2);
			for (const uint8 Byte : Digest)
			{
				Hex += FString::Printf(TEXT("%02x"), Byte);
			}
			return TEXT("sha256:") + Hex;
		}

		FString Normalize(const FString& Value)
		{
			FString Result;
			for (const TCHAR Character : Value.ToLower())
			{
				if (FChar::IsAlnum(Character)) Result.AppendChar(Character);
			}
			return Result;
		}

		const FString BehaviorTreeCoverageScope =
			TEXT("behavior_tree_blank_background");

		struct FNativeBehaviorTreeActionEntry final
		{
			TSharedPtr<FEdGraphSchemaAction> Action;
			FAiBehaviorCandidate Candidate;
			UClass* PayloadClass = nullptr;
		};

		struct FNativeBehaviorTreeActionCatalog final
		{
			UBehaviorTreeGraph* EditorGraph = nullptr;
			TStrongObjectPtr<UBehaviorTreeGraph> OwnedGraph;
			TStrongObjectPtr<UUeAgentBehaviorTreeHeadlessSchema> Schema;
			TStrongObjectPtr<UEdGraph> ActionOwner;
			TArray<FNativeBehaviorTreeActionEntry> Entries;
			int32 CompositeClassCount = 0;
			int32 TaskClassCount = 0;
		};

		bool SetNativeCandidateIdentity(
			const FString& Signature,
			FAiBehaviorCandidate& Candidate,
			FAiBehaviorCandidateQueryError& OutError)
		{
			UeAgentPaletteSearch::FCandidate Identity;
			Identity.Kind = TEXT("behavior_tree_action");
			Identity.Name = Candidate.Name;
			Identity.DisplayName = Candidate.Label;
			Identity.CategoryPath = Candidate.Category;
			Identity.Tooltip = Candidate.Summary;
			Identity.Type = Candidate.ActionKind;
			Identity.FullName = Signature;
			FString IdentityError;
			if (Identity.Name.IsEmpty()
				|| !UeAgentPaletteSearch::SetCandidateIdentity(
					TEXT("behavior_tree_action"),
					Signature,
					Identity.Name,
					Identity,
					IdentityError))
			{
				return Fail(
					OutError,
					TEXT("ai_behavior_candidate_identity_failed"),
					Candidate.Name,
					IdentityError);
			}
			Candidate.Id = Identity.GetPublicId();
			return true;
		}

		bool BuildNativeNodeCandidate(
			const TSharedPtr<FEdGraphSchemaAction>& Action,
			FGraphNodeClassData& ClassData,
			const FString& CandidateKind,
			FAiBehaviorCandidate& OutCandidate,
			UClass*& OutPayloadClass,
			FAiBehaviorCandidateQueryError& OutError)
		{
			OutCandidate = {};
			OutPayloadClass = ClassData.GetClass(true);
			if (!Action.IsValid() || !OutPayloadClass)
			{
				return Fail(
					OutError,
					TEXT("ai_behavior_candidate_native_class_unresolved"),
					ClassData.GetClassName(),
					ClassData.GetPackageName());
			}
			OutCandidate.Name = Action->GetMenuDescription().ToString();
			OutCandidate.Label = OutCandidate.Name;
			OutCandidate.Kind = CandidateKind;
			OutCandidate.ActionKind = TEXT("native_behavior_tree_new_node");
			OutCandidate.Category = Action->GetCategory().ToString();
			OutCandidate.Summary = Action->GetTooltipDescription().ToString();
			OutCandidate.PayloadType = OutPayloadClass->GetPathName();
			OutCandidate.bSupported = true;
			OutCandidate.WriteSupport = TEXT("supported");
			OutCandidate.SupportReason =
				TEXT("native_behavior_tree_schema_action");
			OutCandidate.SupportedOperations = {TEXT("create")};
			return SetNativeCandidateIdentity(
				BehaviorTreeCoverageScope + TEXT("|") + CandidateKind
					+ TEXT("|") + OutCandidate.PayloadType,
				OutCandidate,
				OutError);
		}

		bool BuildNativeMenuCandidate(
			const TSharedPtr<FEdGraphSchemaAction>& Action,
			const FString& Slot,
			FAiBehaviorCandidate& OutCandidate,
			FAiBehaviorCandidateQueryError& OutError)
		{
			OutCandidate = {};
			if (!Action.IsValid())
			{
				return Fail(
					OutError,
					TEXT("ai_behavior_candidate_native_action_invalid"),
					Slot);
			}
			OutCandidate.Name = Action->GetMenuDescription().ToString();
			OutCandidate.Label = OutCandidate.Name;
			OutCandidate.Kind = TEXT("behavior_menu_action");
			OutCandidate.ActionKind = TEXT("native_behavior_tree_") + Slot;
			OutCandidate.Category = Action->GetCategory().ToString();
			OutCandidate.Summary = Action->GetTooltipDescription().ToString();
			OutCandidate.bSupported = false;
			OutCandidate.WriteSupport = TEXT("search_only");
			OutCandidate.SupportReason =
				TEXT("native_menu_action_has_no_hjson_create_contract");
			return SetNativeCandidateIdentity(
				BehaviorTreeCoverageScope + TEXT("|menu_slot=") + Slot,
				OutCandidate,
				OutError);
		}

		bool TryBuildNativeBehaviorTreeCatalog(
			UBehaviorTree& Target,
			const bool bRequireTargetGraph,
			FNativeBehaviorTreeActionCatalog& OutCatalog,
			FAiBehaviorCandidateQueryError& OutError)
		{
			OutCatalog.EditorGraph = nullptr;
			OutCatalog.OwnedGraph.Reset();
			OutCatalog.Schema.Reset();
			OutCatalog.ActionOwner.Reset();
			OutCatalog.Entries.Reset();
			OutCatalog.CompositeClassCount = 0;
			OutCatalog.TaskClassCount = 0;

#if WITH_EDITORONLY_DATA
			UBehaviorTreeGraph* EditorGraph =
				Cast<UBehaviorTreeGraph>(Target.BTGraph);
#else
			UBehaviorTreeGraph* EditorGraph = nullptr;
#endif
			if (!EditorGraph && bRequireTargetGraph)
			{
				return Fail(
					OutError,
					TEXT("ai_behavior_candidate_context_unavailable"),
					Target.GetPathName(),
					TEXT("the target BehaviorTree has no editor graph"));
			}
			if (!EditorGraph)
			{
				EditorGraph = NewObject<UBehaviorTreeGraph>(
					GetTransientPackage(),
					NAME_None,
					RF_Transient);
				if (!EditorGraph)
				{
					return Fail(
						OutError,
						TEXT("ai_behavior_candidate_context_unavailable"),
						Target.GetPathName(),
						TEXT("a transient BehaviorTree graph could not be created"));
				}
				OutCatalog.OwnedGraph.Reset(EditorGraph);
			}

			UUeAgentBehaviorTreeHeadlessSchema* const HeadlessSchema =
				NewObject<UUeAgentBehaviorTreeHeadlessSchema>(
					GetTransientPackage());
			if (!HeadlessSchema)
			{
				return Fail(
					OutError,
					TEXT("ai_behavior_candidate_context_unavailable"),
					Target.GetPathName(),
					TEXT("the headless BehaviorTree schema could not be created"));
			}
			HeadlessSchema->InitializeHeadlessClassCache();
			OutCatalog.Schema.Reset(HeadlessSchema);
			OutCatalog.EditorGraph = EditorGraph;

			TArray<FGraphNodeClassData> CompositeClasses;
			TArray<FGraphNodeClassData> TaskClasses;
			HeadlessSchema->GetHeadlessClassCache().GatherClasses(
				UBTCompositeNode::StaticClass(), CompositeClasses);
			HeadlessSchema->GetHeadlessClassCache().GatherClasses(
				UBTTaskNode::StaticClass(), TaskClasses);
			OutCatalog.CompositeClassCount = CompositeClasses.Num();
			OutCatalog.TaskClassCount = TaskClasses.Num();

			FGraphContextMenuBuilder MenuBuilder(EditorGraph);
			MenuBuilder.FromPin = nullptr;
			HeadlessSchema->GetGraphContextActions(MenuBuilder);
			OutCatalog.ActionOwner.Reset(MenuBuilder.OwnerOfTemporaries);
			const int32 ExpectedActionCount =
				CompositeClasses.Num() + TaskClasses.Num() + 2;
			if (MenuBuilder.GetNumActions() != ExpectedActionCount)
			{
				return Fail(
					OutError,
					TEXT("ai_behavior_candidate_native_menu_shape_mismatch"),
					Target.GetPathName(),
					FString::Printf(
						TEXT("expected=%d actual=%d composites=%d tasks=%d"),
						ExpectedActionCount,
						MenuBuilder.GetNumActions(),
						CompositeClasses.Num(),
						TaskClasses.Num()));
			}

			TSet<FString> SeenIds;
			auto AddNodeSegment = [&](TArray<FGraphNodeClassData>& Classes,
				const FString& Kind, const int32 Offset) -> bool
			{
				for (int32 Index = 0; Index < Classes.Num(); ++Index)
				{
					const TSharedPtr<FEdGraphSchemaAction>& Action =
						MenuBuilder.GetSchemaAction(Offset + Index);
					const FString ExpectedName = FName::NameToDisplayString(
						Classes[Index].ToString(), false);
					if (!Action.IsValid()
						|| Action->GetMenuDescription().ToString() != ExpectedName)
					{
						return Fail(
							OutError,
							TEXT("ai_behavior_candidate_native_menu_shape_mismatch"),
							FString::FromInt(Offset + Index),
							ExpectedName);
					}
					FNativeBehaviorTreeActionEntry& Entry =
						OutCatalog.Entries.AddDefaulted_GetRef();
					Entry.Action = Action;
					if (!BuildNativeNodeCandidate(
							Action,
							Classes[Index],
							Kind,
							Entry.Candidate,
							Entry.PayloadClass,
							OutError))
					{
						return false;
					}
					if (SeenIds.Contains(Entry.Candidate.Id))
					{
						return Fail(
							OutError,
							TEXT("ai_behavior_candidate_identity_ambiguous"),
							Entry.Candidate.Id,
							Entry.Candidate.Name);
					}
					SeenIds.Add(Entry.Candidate.Id);
				}
				return true;
			};

			if (!AddNodeSegment(
					CompositeClasses, TEXT("behavior_composite"), 0)
				|| !AddNodeSegment(
					TaskClasses,
					TEXT("behavior_task"),
					CompositeClasses.Num()))
			{
				return false;
			}

			const int32 MenuOffset =
				CompositeClasses.Num() + TaskClasses.Num();
			const TCHAR* const Slots[] = {TEXT("auto_arrange"), TEXT("add_comment")};
			for (int32 Index = 0; Index < 2; ++Index)
			{
				FNativeBehaviorTreeActionEntry& Entry =
					OutCatalog.Entries.AddDefaulted_GetRef();
				Entry.Action = MenuBuilder.GetSchemaAction(MenuOffset + Index);
				if (!BuildNativeMenuCandidate(
						Entry.Action, Slots[Index], Entry.Candidate, OutError))
				{
					return false;
				}
				if (SeenIds.Contains(Entry.Candidate.Id))
				{
					return Fail(
						OutError,
						TEXT("ai_behavior_candidate_identity_ambiguous"),
						Entry.Candidate.Id,
						Entry.Candidate.Name);
				}
				SeenIds.Add(Entry.Candidate.Id);
			}
			return true;
		}

		bool MatchesNativeSearch(
			const FAiBehaviorCandidate& Candidate,
			const FString& Query)
		{
			const FString Needle = Normalize(Query);
			return Needle.IsEmpty()
				|| Normalize(
					Candidate.Name + TEXT("|") + Candidate.Category
					+ TEXT("|") + Candidate.Summary + TEXT("|")
					+ Candidate.ActionKind + TEXT("|")
					+ Candidate.PayloadType).Contains(Needle);
		}

		bool TrySelectNativeBehaviorTreeAction(
			FNativeBehaviorTreeActionCatalog& Catalog,
			const FString& Name,
			const FString& Id,
			FNativeBehaviorTreeActionEntry*& OutEntry,
			FAiBehaviorCandidateQueryError& OutError)
		{
			OutEntry = nullptr;
			const FString CleanName = Name.TrimStartAndEnd();
			const FString CleanId = Id.TrimStartAndEnd();
			if (CleanName.IsEmpty() && CleanId.IsEmpty())
			{
				return Fail(
					OutError,
					TEXT("ai_behavior_candidate_selector_required"));
			}
			TArray<FNativeBehaviorTreeActionEntry*> Matches;
			for (FNativeBehaviorTreeActionEntry& Entry : Catalog.Entries)
			{
				if (!CleanId.IsEmpty()
					&& !Entry.Candidate.Id.Equals(
						CleanId, ESearchCase::IgnoreCase))
				{
					continue;
				}
				if (!CleanName.IsEmpty()
					&& !Entry.Candidate.Name.Equals(
						CleanName, ESearchCase::CaseSensitive))
				{
					continue;
				}
				Matches.Add(&Entry);
			}
			if (Matches.IsEmpty())
			{
				return Fail(
					OutError,
					TEXT("ai_behavior_candidate_not_found"),
					CleanId.IsEmpty() ? CleanName : CleanId);
			}
			if (Matches.Num() != 1)
			{
				return Fail(
					OutError,
					TEXT("ai_behavior_candidate_ambiguous"),
					CleanName,
					FString::FromInt(Matches.Num()));
			}
			OutEntry = Matches[0];
			return true;
		}

		bool IsSupportedTarget(const UObject& Target)
		{
			return Target.GetClass() == UBehaviorTree::StaticClass()
				|| Target.GetClass() == UBlackboardData::StaticClass()
				|| Target.GetClass() == UStateTree::StaticClass()
				|| Target.GetClass() == UEnvQuery::StaticClass()
				|| Target.GetClass() == USmartObjectDefinition::StaticClass();
		}

		FString KindForClass(const UClass& Class)
		{
			if (Class.IsChildOf(UBTService::StaticClass())) return TEXT("behavior_service");
			if (Class.IsChildOf(UBTDecorator::StaticClass())) return TEXT("behavior_decorator");
			if (Class.IsChildOf(UBTCompositeNode::StaticClass())) return TEXT("behavior_composite");
			if (Class.IsChildOf(UBTTaskNode::StaticClass())) return TEXT("behavior_task");
			if (Class.IsChildOf(UStateTreeNodeBlueprintBase::StaticClass())) return TEXT("state_tree_node");
			if (Class.IsChildOf(UBlackboardKeyType::StaticClass())) return TEXT("blackboard_key");
			if (Class.IsChildOf(UEnvQueryGenerator::StaticClass())) return TEXT("eqs_generator");
			if (Class.IsChildOf(USmartObjectBehaviorDefinition::StaticClass())) return TEXT("smart_object_behavior");
			return FString();
		}

		FString KindForStruct(const UScriptStruct& Struct)
		{
			if (Struct.IsChildOf(FStateTreeTaskBase::StaticStruct()))
			{
				return TEXT("state_tree_task");
			}
			if (Struct.IsChildOf(FStateTreeConditionBase::StaticStruct()))
			{
				return TEXT("state_tree_condition");
			}
			if (Struct.IsChildOf(FStateTreeEvaluatorBase::StaticStruct()))
			{
				return TEXT("state_tree_evaluator");
			}
			return FString();
		}

		bool MatchesTarget(const UObject& Target, const UClass& Class)
		{
			if (Target.GetClass() == UBehaviorTree::StaticClass())
			{
				return Class.IsChildOf(UBTNode::StaticClass());
			}
			if (Target.GetClass() == UStateTree::StaticClass())
			{
				return Class.IsChildOf(UStateTreeNodeBlueprintBase::StaticClass());
			}
			if (Target.GetClass() == UEnvQuery::StaticClass())
			{
				return Class.IsChildOf(UEnvQueryGenerator::StaticClass());
			}
			if (Target.GetClass() == USmartObjectDefinition::StaticClass())
			{
				return Class.IsChildOf(
					USmartObjectBehaviorDefinition::StaticClass());
			}
			return Class.IsChildOf(UBlackboardKeyType::StaticClass());
		}

		FString InputContract(const FString& Kind)
		{
			if (Kind == TEXT("behavior_composite")) return TEXT("uai.ai.behavior_composite.create/1");
			if (Kind == TEXT("behavior_task")) return TEXT("uai.ai.behavior_task.create/1");
			if (Kind == TEXT("behavior_service")) return TEXT("uai.ai.behavior_service.create/1");
			if (Kind == TEXT("behavior_decorator")) return TEXT("uai.ai.behavior_decorator.create/1");
			if (Kind.StartsWith(TEXT("state_tree_"))) return TEXT("uai.ai.state_tree_node.create/1");
			if (Kind == TEXT("blackboard_key")) return TEXT("uai.ai.blackboard_entry.create/1");
			if (Kind == TEXT("eqs_generator")) return TEXT("uai.ai.eqs_option.create/1");
			if (Kind == TEXT("smart_object_behavior")) return TEXT("uai.ai.smart_object_slot.create/1");
			return FString();
		}

		FString PlacementContract(const FString& Kind)
		{
			if (Kind == TEXT("blackboard_key")) return TEXT("uai.ai.blackboard-entry-placement/1");
			if (Kind.StartsWith(TEXT("state_tree_"))) return TEXT("uai.ai.state-tree-placement/1");
			if (Kind == TEXT("eqs_generator")) return TEXT("uai.ai.eqs-graph-placement/1");
			if (Kind == TEXT("smart_object_behavior")) return TEXT("uai.ai.smart-object-slot-placement/1");
			return TEXT("uai.ai.behavior-tree-placement/1");
		}

		FString RequiredContext(const FString& Kind)
		{
			if (Kind == TEXT("blackboard_key")) return TEXT("target_ref:entry_name:key_type_ref");
			if (Kind.StartsWith(TEXT("state_tree_"))) return TEXT("target_ref:owner_state:node_type_ref:placement");
			if (Kind == TEXT("eqs_generator")) return TEXT("target_ref:option_guid:generator_type_ref");
			if (Kind == TEXT("smart_object_behavior")) return TEXT("target_ref:slot_guid:behavior_type_ref");
			return TEXT("target_ref:owner_node:node_class:placement");
		}

		FString CandidateRevision(
			const UObject& Target,
			const FString& CandidateName,
			const FString& CandidateKind)
		{
			const FString CandidateInput = InputContract(CandidateKind);
			const FString CandidatePlacement = PlacementContract(CandidateKind);
			const FString CandidateContext = RequiredContext(CandidateKind);
			return Sha256(
				CandidateName + TEXT("|") + CandidateKind + TEXT("|")
					+ CandidateInput + TEXT("|") + CandidatePlacement
					+ TEXT("|") + CandidateContext + TEXT("|")
					+ Target.GetClass()->GetPathName());
		}

		bool ReadTargetRevision(
			UObject& Target,
			FString& OutRevision,
			FAiBehaviorCandidateQueryError& OutError)
		{
			if (Target.GetOutermost() == GetTransientPackage())
			{
				OutRevision = Sha256(TEXT("transient|") + Target.GetClass()->GetPathName());
				return true;
			}
			FUeAgentContentGraphCapture Capture;
			FUeAgentContentGraphReadError ReadError;
			if (!FUeAgentContentGraphReadProductionEnvironment::TryCaptureOwner(
					Target, Capture, ReadError)
				|| Capture.Revision.IsEmpty())
			{
				return Fail(
					OutError,
					ReadError.Code.IsEmpty()
						? FString(TEXT("ai_behavior_candidate_target_readback_failed"))
						: ReadError.Code,
					ReadError.SubjectId,
					ReadError.Detail);
			}
			OutRevision = Capture.Revision;
			return true;
		}
	}

	bool TryQueryAiBehaviorCandidates(
		UObject& Target,
		const FString& Query,
		FAiBehaviorCandidateQueryResult& OutResult,
		FAiBehaviorCandidateQueryError& OutError,
		const int32 Limit,
		const int32 Cursor,
		const FString& CandidateId)
	{
		OutResult = {};
		OutError = {};
		if (!IsInGameThread())
		{
			return Fail(OutError, TEXT("ai_behavior_candidate_game_thread_required"), Target.GetPathName());
		}
		if (!IsSupportedTarget(Target))
		{
			return Fail(OutError, TEXT("ai_behavior_candidate_target_type_unsupported"), Target.GetClass()->GetPathName());
		}
		if (Limit < 1 || Limit > 64 || Cursor < 0)
		{
			return Fail(OutError, TEXT("ai_behavior_candidate_page_invalid"), Target.GetPathName(), TEXT("limit must be 1..64 and cursor must be non-negative"));
		}

		OutResult.Target = Target.GetPathName();
		if (!ReadTargetRevision(Target, OutResult.TargetRevision, OutError)) return false;

		if (UBehaviorTree* const BehaviorTree = Cast<UBehaviorTree>(&Target))
		{
			FNativeBehaviorTreeActionCatalog Catalog;
			if (!TryBuildNativeBehaviorTreeCatalog(
					*BehaviorTree, false, Catalog, OutError))
			{
				return false;
			}
			OutResult.CoverageScope = BehaviorTreeCoverageScope;
			OutResult.GraphName = Catalog.EditorGraph
				? Catalog.EditorGraph->GetName() : FString();
			OutResult.NativeActionCount = Catalog.Entries.Num();
			OutResult.bCoverageComplete = true;
			for (const FNativeBehaviorTreeActionEntry& Entry : Catalog.Entries)
			{
				if (Entry.Candidate.bSupported)
				{
					++OutResult.ExecutableActionCount;
				}
			}
			OutResult.bCreationCoverageComplete =
				OutResult.NativeActionCount == OutResult.ExecutableActionCount;

			TArray<const FAiBehaviorCandidate*> Matches;
			const FString CleanCandidateId = CandidateId.TrimStartAndEnd();
			for (const FNativeBehaviorTreeActionEntry& Entry : Catalog.Entries)
			{
				if (!CleanCandidateId.IsEmpty()
					&& !Entry.Candidate.Id.Equals(
						CleanCandidateId, ESearchCase::IgnoreCase))
				{
					continue;
				}
				if (MatchesNativeSearch(Entry.Candidate, Query))
				{
					Matches.Add(&Entry.Candidate);
				}
			}
			OutResult.TotalMatches = Matches.Num();
			const int32 Start = FMath::Min(Cursor, Matches.Num());
			const int32 End = FMath::Min(Start + Limit, Matches.Num());
			for (int32 Index = Start; Index < End; ++Index)
			{
				OutResult.Candidates.Add(*Matches[Index]);
			}
			OutResult.bHasMore = End < OutResult.TotalMatches;
			OutResult.bComplete = !OutResult.bHasMore;
			OutResult.NextCursor = OutResult.bHasMore ? End : INDEX_NONE;
			return true;
		}

		if (!CandidateId.TrimStartAndEnd().IsEmpty())
		{
			return Fail(
				OutError,
				TEXT("ai_behavior_candidate_id_context_unsupported"),
				Target.GetPathName(),
				TEXT("compact ids are currently available for BehaviorTree blank-background actions"));
		}
		OutResult.CoverageScope = TEXT("legacy_ai_class_identity_compatibility");

		const FString Needle = Normalize(Query);
		TArray<const UObject*> Matches;
		for (TObjectIterator<UClass> It; It; ++It)
		{
			UClass* Class = *It;
			const FString Kind = Class ? KindForClass(*Class) : FString();
			if (!Class || Kind.IsEmpty() || !MatchesTarget(Target, *Class)
				|| Class->HasAnyClassFlags(CLASS_Abstract | CLASS_Deprecated | CLASS_NewerVersionExists))
			{
				continue;
			}
			const FString Identity = Class->GetPathName();
			const FString Searchable = Normalize(Identity + TEXT("|") + Class->GetName());
			if (Identity.StartsWith(TEXT("/Script/"))
				&& (Needle.IsEmpty() || Searchable.Contains(Needle)))
			{
				Matches.Add(Class);
			}
		}
		if (Target.GetClass() == UStateTree::StaticClass())
		{
			for (TObjectIterator<UScriptStruct> It; It; ++It)
			{
				UScriptStruct* Struct = *It;
				const FString Kind = Struct ? KindForStruct(*Struct) : FString();
				if (!Struct || Kind.IsEmpty()
					|| Struct == FStateTreeTaskBase::StaticStruct()
					|| Struct == FStateTreeConditionBase::StaticStruct()
					|| Struct == FStateTreeEvaluatorBase::StaticStruct()
					|| Struct->GetName().Contains(TEXT("Blueprint"))
					|| Struct->HasMetaData(TEXT("Hidden"))
					|| Struct->HasMetaData(TEXT("Deprecated")))
				{
					continue;
				}
				const FString Identity = Struct->GetPathName();
				const FString Searchable = Normalize(
					Identity + TEXT("|") + Struct->GetName());
				if (Identity.StartsWith(TEXT("/Script/"))
					&& (Needle.IsEmpty() || Searchable.Contains(Needle)))
				{
					Matches.Add(Struct);
				}
			}
		}
		Algo::SortBy(Matches, [](const UObject* Object)
		{
			return Object->GetPathName();
		});

		OutResult.TotalMatches = Matches.Num();
		const int32 Start = FMath::Min(Cursor, Matches.Num());
		const int32 End = FMath::Min(Start + Limit, Matches.Num());
		for (int32 Index = Start; Index < End; ++Index)
		{
			const UObject* CandidateType = Matches[Index];
			const UClass* Class = Cast<UClass>(CandidateType);
			const UScriptStruct* Struct = Cast<UScriptStruct>(CandidateType);
			FAiBehaviorCandidate Candidate;
			Candidate.Name = CandidateType->GetPathName();
			Candidate.Label = CandidateType->GetName();
			Candidate.Kind = Class
				? KindForClass(*Class)
				: (Struct ? KindForStruct(*Struct) : FString());
			Candidate.bSupported = true;
			Candidate.WriteSupport = TEXT("legacy_binding");
			Candidate.SupportReason = TEXT("native_context_migration_pending");
			Candidate.SupportedOperations = {TEXT("create")};
			Candidate.InputContract = InputContract(Candidate.Kind);
			Candidate.PlacementContract = PlacementContract(Candidate.Kind);
			Candidate.RequiredContext = RequiredContext(Candidate.Kind);
			Candidate.BindingRevision = CandidateRevision(
				Target, Candidate.Name, Candidate.Kind);
			OutResult.Candidates.Add(MoveTemp(Candidate));
		}
		OutResult.bHasMore = End < OutResult.TotalMatches;
		OutResult.bComplete = !OutResult.bHasMore;
		OutResult.NextCursor = OutResult.bHasMore ? End : INDEX_NONE;
		return true;
	}

	bool TryResolveAiBehaviorCandidate(
		UObject& Target,
		const FString& Name,
		const FString& Id,
		FAiBehaviorCandidate& OutCandidate,
		FAiBehaviorCandidateQueryError& OutError)
	{
		OutCandidate = {};
		OutError = {};
		if (!IsInGameThread())
		{
			return Fail(
				OutError,
				TEXT("ai_behavior_candidate_game_thread_required"),
				Target.GetPathName());
		}
		UBehaviorTree* const BehaviorTree = Cast<UBehaviorTree>(&Target);
		if (!BehaviorTree)
		{
			return Fail(
				OutError,
				TEXT("ai_behavior_candidate_selector_context_unsupported"),
				Target.GetClass()->GetPathName(),
				TEXT("compact candidate selectors currently require a BehaviorTree blank-background context"));
		}
		FNativeBehaviorTreeActionCatalog Catalog;
		FNativeBehaviorTreeActionEntry* Entry = nullptr;
		if (!TryBuildNativeBehaviorTreeCatalog(
				*BehaviorTree, false, Catalog, OutError)
			|| !TrySelectNativeBehaviorTreeAction(
				Catalog, Name, Id, Entry, OutError))
		{
			return false;
		}
		OutCandidate = Entry->Candidate;
		return true;
	}

	bool TryPerformBehaviorTreeCandidate(
		UBehaviorTree& Target,
		const FString& Name,
		const FString& Id,
		const FVector2f& Location,
		UBehaviorTreeGraphNode*& OutNode,
		FAiBehaviorCandidate& OutCandidate,
		FAiBehaviorCandidateQueryError& OutError)
	{
		OutNode = nullptr;
		OutCandidate = {};
		OutError = {};
		if (!IsInGameThread())
		{
			return Fail(
				OutError,
				TEXT("ai_behavior_candidate_game_thread_required"),
				Target.GetPathName());
		}
		if (!FMath::IsFinite(Location.X) || !FMath::IsFinite(Location.Y))
		{
			return Fail(
				OutError,
				TEXT("ai_behavior_candidate_position_invalid"),
				Target.GetPathName());
		}

		FNativeBehaviorTreeActionCatalog Catalog;
		FNativeBehaviorTreeActionEntry* Entry = nullptr;
		if (!TryBuildNativeBehaviorTreeCatalog(
				Target, true, Catalog, OutError)
			|| !TrySelectNativeBehaviorTreeAction(
				Catalog, Name, Id, Entry, OutError))
		{
			return false;
		}
		if (!Entry->Candidate.bSupported || !Entry->PayloadClass)
		{
			return Fail(
				OutError,
				TEXT("ai_behavior_candidate_action_kind_unsupported"),
				Entry->Candidate.ActionKind,
				Entry->Candidate.SupportReason);
		}

		TSet<UEdGraphNode*> NodesBefore;
		for (UEdGraphNode* Node : Catalog.EditorGraph->Nodes)
		{
			if (Node)
			{
				NodesBefore.Add(Node);
			}
		}
		UEdGraphNode* const Returned = Entry->Action->PerformAction(
			Catalog.EditorGraph, nullptr, Location, false);
		UBehaviorTreeGraphNode* const Created =
			Cast<UBehaviorTreeGraphNode>(Returned);
		if (!Created
			|| NodesBefore.Contains(Created)
			|| !Catalog.EditorGraph->Nodes.Contains(Created)
			|| !Created->NodeInstance
			|| Created->NodeInstance->GetClass() != Entry->PayloadClass)
		{
			return Fail(
				OutError,
				TEXT("ai_behavior_candidate_native_action_create_failed"),
				Entry->Candidate.Id,
				FString::Printf(
					TEXT("returned=%s instance=%s"),
					Created ? TEXT("true") : TEXT("false"),
					Created && Created->NodeInstance
						? *Created->NodeInstance->GetClass()->GetPathName()
						: TEXT("none")));
		}
		OutNode = Created;
		OutCandidate = Entry->Candidate;
		return true;
	}

	bool IsCurrentAiBehaviorCandidateBinding(
		UObject& Target,
		const FString& CandidateName,
		const FString& CandidateKind,
		const TArray<FString>& SupportedOperations,
		const FString& InputContractValue,
		const FString& PlacementContractValue,
		const FString& RequiredContextValue,
		const FString& BindingRevision,
		FAiBehaviorCandidateQueryError& OutError)
	{
		OutError = {};
		if (!IsInGameThread())
		{
			return Fail(
				OutError, TEXT("ai_behavior_candidate_game_thread_required"),
				Target.GetPathName());
		}
		if (!IsSupportedTarget(Target))
		{
			return Fail(
				OutError, TEXT("ai_behavior_candidate_target_type_unsupported"),
				Target.GetClass()->GetPathName());
		}

		const UObject* CandidateType = LoadObject<UClass>(nullptr, *CandidateName);
		if (!CandidateType && Target.GetClass() == UStateTree::StaticClass())
		{
			CandidateType = LoadObject<UScriptStruct>(nullptr, *CandidateName);
		}
		const UClass* Class = Cast<UClass>(CandidateType);
		const UScriptStruct* Struct = Cast<UScriptStruct>(CandidateType);
		const FString CurrentKind = Class
			? KindForClass(*Class)
			: (Struct ? KindForStruct(*Struct) : FString());
		const bool bMatchesTarget = Class
			? MatchesTarget(Target, *Class)
			: (Struct && Target.GetClass() == UStateTree::StaticClass()
				&& !CurrentKind.IsEmpty());
		if (!CandidateType || CurrentKind.IsEmpty() || !bMatchesTarget
			|| (Class && Class->HasAnyClassFlags(
				CLASS_Abstract | CLASS_Deprecated | CLASS_NewerVersionExists)))
		{
			return Fail(
				OutError, TEXT("ai_behavior_candidate_name_unresolved"),
				CandidateName);
		}
		const TArray<FString> ExpectedOperations = {TEXT("create")};
		if (CandidateKind != CurrentKind
			|| SupportedOperations != ExpectedOperations
			|| InputContractValue != InputContract(CurrentKind)
			|| PlacementContractValue != PlacementContract(CurrentKind)
			|| RequiredContextValue != RequiredContext(CurrentKind))
		{
			return Fail(
				OutError, TEXT("ai_behavior_candidate_contract_mismatch"),
				CandidateName);
		}
		const FString CurrentRevision = CandidateRevision(
			Target, CandidateName, CurrentKind);
		if (BindingRevision != CurrentRevision)
		{
			return Fail(
				OutError, TEXT("ai_behavior_candidate_binding_stale"),
				CandidateName, CurrentRevision);
		}
		return true;
	}
}
