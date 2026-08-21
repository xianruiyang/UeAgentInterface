#include "QueryOwners/AiBehavior/UeAgentAiBehaviorCandidateQuery.h"

#include "Algo/Sort.h"
#include "BehaviorTree/BehaviorTree.h"
#include "BehaviorTree/Blackboard/BlackboardKeyType.h"
#include "BehaviorTree/BlackboardData.h"
#include "BehaviorTree/BTDecorator.h"
#include "BehaviorTree/BTCompositeNode.h"
#include "BehaviorTree/BTNode.h"
#include "BehaviorTree/BTService.h"
#include "BehaviorTree/BTTaskNode.h"
#include "Blueprint/StateTreeNodeBlueprintBase.h"
#include "DomainAdapters/ContentGraphRead/UeAgentContentGraphReadProductionEnvironment.h"
#include "EnvironmentQuery/EnvQuery.h"
#include "EnvironmentQuery/EnvQueryGenerator.h"
#include "SmartObjectDefinition.h"
#include "StateTree.h"
#include "StateTreeConditionBase.h"
#include "StateTreeEvaluatorBase.h"
#include "StateTreeTaskBase.h"
#include "UObject/UObjectIterator.h"

#include <openssl/sha.h>

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
		const int32 Cursor)
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
