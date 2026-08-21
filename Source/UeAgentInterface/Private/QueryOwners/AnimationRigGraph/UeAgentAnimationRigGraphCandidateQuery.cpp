#include "QueryOwners/AnimationRigGraph/UeAgentAnimationRigGraphCandidateQuery.h"

#include "Algo/Sort.h"
#include "ControlRigBlueprint.h"
#include "DomainAdapters/ContentGraphRead/UeAgentContentGraphReadProductionEnvironment.h"
#include "Units/RigUnit.h"
#include "UObject/UObjectIterator.h"

#include <openssl/sha.h>

namespace UE::UAI::QueryOwners::AnimationRigGraph
{
	using namespace DomainAdapters::ContentGraphRead;

	namespace
	{
		const FString CandidateKind = TEXT("rig_unit_struct");
		const FString InputContract = TEXT("uai.rig.unit.create/1");
		const FString PlacementContract = TEXT("uai.rig.graph-placement/1");
		const FString RequiredContext = TEXT("target_ref:node_id:position");

		bool Fail(
			FAnimationRigGraphCandidateQueryError& OutError,
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
			static const TSet<FString> Supported = {
				TEXT("/Script/ControlRigDeveloper.ControlRigBlueprint"),
				TEXT("/Script/IKRig.IKRigDefinition"),
				TEXT("/Script/IKRig.IKRetargeter"),
				TEXT("/Script/Engine.Skeleton"),
			};
			return Supported.Contains(Target.GetClass()->GetPathName());
		}

		FString CandidateRevision(
			const FString& TargetRef,
			const FString& TargetRevision,
			const FString& Name)
		{
			return Sha256(
				TargetRef + TEXT("|") + TargetRevision + TEXT("|") + Name
					+ TEXT("|") + CandidateKind + TEXT("|") + InputContract
					+ TEXT("|") + PlacementContract + TEXT("|")
					+ RequiredContext);
		}

		bool ReadTargetRevision(
			UObject& Target,
			FString& OutRevision,
			FAnimationRigGraphCandidateQueryError& OutError)
		{
			if (Target.GetOutermost() == GetTransientPackage())
			{
				OutRevision = Sha256(TEXT("transient|")
					+ Target.GetClass()->GetPathName());
				return true;
			}
			FUeAgentContentGraphCapture Capture;
			FUeAgentContentGraphReadError ReadError;
			if (FUeAgentContentGraphReadProductionEnvironment::TryCaptureOwner(
					Target, Capture, ReadError)
				&& !Capture.Revision.IsEmpty())
			{
				OutRevision = Capture.Revision;
				return true;
			}
			const UPackage* Package = Target.GetOutermost();
			if (Package && Package != GetTransientPackage())
			{
				OutRevision = FString::Printf(TEXT("%s:%s:%s"),
					*Package->GetPersistentGuid().ToString(EGuidFormats::Digits),
					Package->IsDirty() ? TEXT("dirty") : TEXT("clean"),
					*Sha256(Target.GetClass()->GetPathName()
						+ TEXT("|") + Target.GetPathName()));
				return true;
			}
			return Fail(OutError,
				TEXT("animation_rig_graph_candidate_target_readback_failed"),
				Target.GetPathName(), ReadError.Code);
		}
	}

	bool TryQueryAnimationRigGraphCandidates(
		UObject& Target,
		const FString& Query,
		FAnimationRigGraphCandidateQueryResult& OutResult,
		FAnimationRigGraphCandidateQueryError& OutError,
		const int32 Limit,
		const int32 Cursor)
	{
		OutResult = {};
		OutError = {};
		if (!IsInGameThread())
		{
			return Fail(OutError,
				TEXT("animation_rig_graph_candidate_game_thread_required"),
				Target.GetPathName());
		}
		if (!IsSupportedTarget(Target))
		{
			return Fail(OutError,
				TEXT("animation_rig_graph_candidate_target_type_unsupported"),
				Target.GetClass()->GetPathName());
		}
		if (Limit < 1 || Limit > 64 || Cursor < 0)
		{
			return Fail(OutError,
				TEXT("animation_rig_graph_candidate_page_invalid"),
				Target.GetPathName());
		}

		OutResult.Target = Target.GetPathName();
		if (!ReadTargetRevision(Target, OutResult.TargetRevision, OutError))
		{
			return false;
		}
		const FString Needle = Normalize(Query);
		TArray<FAnimationRigGraphCandidate> Matches;
		for (TObjectIterator<UScriptStruct> It; It; ++It)
		{
			UScriptStruct* Struct = *It;
			if (!Struct || Struct == FRigUnit::StaticStruct()
				|| !Struct->IsChildOf(FRigUnit::StaticStruct())
				|| Struct->HasMetaData(TEXT("Abstract")))
			{
				continue;
			}
			const FString Identity = Struct->GetPathName();
			if (!Identity.StartsWith(TEXT("/Script/"))
				|| (!Needle.IsEmpty()
					&& !Normalize(Identity + TEXT("|")
						+ Struct->GetDisplayNameText().ToString()).Contains(Needle)))
			{
				continue;
			}
			FAnimationRigGraphCandidate Candidate;
			Candidate.TargetRef = OutResult.Target;
			Candidate.TargetRevision = OutResult.TargetRevision;
			Candidate.Name = Identity;
			Candidate.Label = Struct->GetDisplayNameText().ToString();
			Candidate.Kind = CandidateKind;
			Candidate.SupportedOperations = {TEXT("create")};
			Candidate.InputContract = InputContract;
			Candidate.PlacementContract = PlacementContract;
			Candidate.RequiredContext = RequiredContext;
			Candidate.BindingRevision = CandidateRevision(
				Candidate.TargetRef,
				Candidate.TargetRevision,
				Candidate.Name);
			Matches.Add(MoveTemp(Candidate));
		}

		Algo::SortBy(Matches,
			[](const FAnimationRigGraphCandidate& Candidate)
			{
				return Candidate.Name;
			});
		OutResult.TotalMatches = Matches.Num();
		const int32 Start = FMath::Min(Cursor, Matches.Num());
		const int32 End = FMath::Min(Start + Limit, Matches.Num());
		for (int32 Index = Start; Index < End; ++Index)
		{
			OutResult.Candidates.Add(MoveTemp(Matches[Index]));
		}
		OutResult.bHasMore = End < OutResult.TotalMatches;
		OutResult.bComplete = !OutResult.bHasMore;
		OutResult.NextCursor = OutResult.bHasMore ? End : INDEX_NONE;
		return true;
	}

	bool IsCurrentAnimationRigGraphCandidateBinding(
		UObject& Target,
		const FString& CandidateTargetRef,
		const FString& CandidateTargetRevision,
		const FString& CandidateName,
		const FString& CandidateKindValue,
		const TArray<FString>& SupportedOperations,
		const FString& InputContractValue,
		const FString& PlacementContractValue,
		const FString& RequiredContextValue,
		const FString& BindingRevision,
		FAnimationRigGraphCandidateQueryError& OutError)
	{
		OutError = {};
		if (!IsInGameThread())
		{
			return Fail(
				OutError,
				TEXT("animation_rig_graph_candidate_game_thread_required"),
				Target.GetPathName());
		}
		if (!IsSupportedTarget(Target))
		{
			return Fail(
				OutError,
				TEXT("animation_rig_graph_candidate_target_type_unsupported"),
				Target.GetClass()->GetPathName());
		}
		FString CurrentTargetRevision;
		if (CandidateTargetRef != Target.GetPathName()
			|| !ReadTargetRevision(Target, CurrentTargetRevision, OutError)
			|| CandidateTargetRevision != CurrentTargetRevision)
		{
			if (!OutError.Code.IsEmpty())
			{
				return false;
			}
			return Fail(
				OutError,
				TEXT("animation_rig_graph_candidate_target_stale"),
				CandidateTargetRef,
				CurrentTargetRevision);
		}
		UScriptStruct* const CandidateStruct =
			LoadObject<UScriptStruct>(nullptr, *CandidateName);
		if (!CandidateStruct
			|| CandidateStruct == FRigUnit::StaticStruct()
			|| !CandidateStruct->IsChildOf(FRigUnit::StaticStruct())
			|| CandidateStruct->HasMetaData(TEXT("Abstract"))
			|| !CandidateStruct->GetPathName().StartsWith(TEXT("/Script/")))
		{
			return Fail(
				OutError,
				TEXT("animation_rig_graph_candidate_name_unresolved"),
				CandidateName);
		}
		const TArray<FString> ExpectedOperations = {TEXT("create")};
		if (CandidateKindValue != CandidateKind
			|| SupportedOperations != ExpectedOperations
			|| InputContractValue != InputContract
			|| PlacementContractValue != PlacementContract
			|| RequiredContextValue != RequiredContext)
		{
			return Fail(
				OutError,
				TEXT("animation_rig_graph_candidate_contract_mismatch"),
				CandidateName);
		}
		const FString CurrentRevision = CandidateRevision(
			CandidateTargetRef, CandidateTargetRevision, CandidateName);
		if (BindingRevision != CurrentRevision)
		{
			return Fail(
				OutError,
				TEXT("animation_rig_graph_candidate_binding_stale"),
				CandidateName,
				CurrentRevision);
		}
		return true;
	}
}
