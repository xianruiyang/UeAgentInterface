#include "QueryOwners/Pcg/UeAgentPcgNodeCandidateQuery.h"

#include "Algo/Sort.h"
#include "DomainAdapters/ContentGraphRead/UeAgentContentGraphReadProductionEnvironment.h"
#include "PCGGraph.h"
#include "PCGSettings.h"
#include "UObject/UObjectIterator.h"

#include <openssl/sha.h>

namespace UE::UAI::QueryOwners::Pcg
{
	using namespace DomainAdapters::ContentGraphRead;

	namespace
	{
		const FString CandidateKind = TEXT("pcg_node");
		const FString InputContract = TEXT("uai.pcg.node.create/1");
		const FString PlacementContract = TEXT("uai.pcg.graph-placement/1");
		const FString RequiredContext = TEXT("target_ref:settings_class:position");

		bool Fail(
			FPcgNodeCandidateQueryError& OutError,
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
				if (FChar::IsAlnum(Character))
				{
					Result.AppendChar(Character);
				}
			}
			return Result;
		}

		bool IsSupportedTarget(const UObject& Target)
		{
			return Target.GetClass() == UPCGGraph::StaticClass();
		}

		FString CandidateRevision(const UObject& Target, const FString& Name)
		{
			return Sha256(
				Name + TEXT("|") + CandidateKind + TEXT("|") + InputContract
					+ TEXT("|") + PlacementContract + TEXT("|") + RequiredContext
					+ TEXT("|") + Target.GetClass()->GetPathName());
		}

		bool ReadTargetRevision(
			UObject& Target,
			FString& OutRevision,
			FPcgNodeCandidateQueryError& OutError)
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
						? FString(TEXT("pcg_candidate_target_readback_failed"))
						: ReadError.Code,
					ReadError.SubjectId,
					ReadError.Detail);
			}
			OutRevision = Capture.Revision;
			return true;
		}
	}

	bool TryQueryPcgNodeCandidates(
		UObject& Target,
		const FString& Query,
		FPcgNodeCandidateQueryResult& OutResult,
		FPcgNodeCandidateQueryError& OutError,
		const int32 Limit,
		const int32 Cursor)
	{
		OutResult = {};
		OutError = {};
		if (!IsInGameThread())
		{
			return Fail(OutError, TEXT("pcg_candidate_game_thread_required"), Target.GetPathName());
		}
		if (!IsSupportedTarget(Target))
		{
			return Fail(OutError, TEXT("pcg_candidate_target_type_unsupported"), Target.GetClass()->GetPathName());
		}
		if (Limit < 1 || Limit > 64 || Cursor < 0)
		{
			return Fail(OutError, TEXT("pcg_candidate_page_invalid"), Target.GetPathName(), TEXT("limit must be 1..64 and cursor must be non-negative"));
		}

		OutResult.Target = Target.GetPathName();
		if (!ReadTargetRevision(Target, OutResult.TargetRevision, OutError))
		{
			return false;
		}

		const FString Needle = Normalize(Query);
		TArray<UClass*> Matches;
		for (TObjectIterator<UClass> It; It; ++It)
		{
			UClass* Class = *It;
			if (!Class || Class == UPCGSettings::StaticClass()
				|| !Class->IsChildOf(UPCGSettings::StaticClass())
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
		Algo::SortBy(Matches, [](const UClass* Class) { return Class->GetPathName(); });

		OutResult.TotalMatches = Matches.Num();
		const int32 Start = FMath::Min(Cursor, Matches.Num());
		const int32 End = FMath::Min(Start + Limit, Matches.Num());
		for (int32 Index = Start; Index < End; ++Index)
		{
			const UClass* Class = Matches[Index];
			FPcgNodeCandidate Candidate;
			Candidate.Name = Class->GetPathName();
			Candidate.Label = Class->GetName();
			Candidate.Kind = CandidateKind;
			Candidate.SupportedOperations = {TEXT("create")};
			Candidate.InputContract = InputContract;
			Candidate.PlacementContract = PlacementContract;
			Candidate.RequiredContext = RequiredContext;
			Candidate.BindingRevision = CandidateRevision(Target, Candidate.Name);
			OutResult.Candidates.Add(MoveTemp(Candidate));
		}
		OutResult.bHasMore = End < OutResult.TotalMatches;
		OutResult.bComplete = !OutResult.bHasMore;
		OutResult.NextCursor = OutResult.bHasMore ? End : INDEX_NONE;
		return true;
	}

	bool IsCurrentPcgNodeCandidateBinding(
		UObject& Target,
		const FString& CandidateName,
		const FString& CandidateKindValue,
		const TArray<FString>& SupportedOperations,
		const FString& InputContractValue,
		const FString& PlacementContractValue,
		const FString& RequiredContextValue,
		const FString& BindingRevision,
		FPcgNodeCandidateQueryError& OutError)
	{
		OutError = {};
		if (!IsInGameThread())
		{
			return Fail(
				OutError, TEXT("pcg_candidate_game_thread_required"),
				Target.GetPathName());
		}
		if (!IsSupportedTarget(Target))
		{
			return Fail(
				OutError, TEXT("pcg_candidate_target_type_unsupported"),
				Target.GetClass()->GetPathName());
		}
		UClass* const CandidateClass = LoadObject<UClass>(
			nullptr, *CandidateName);
		if (!CandidateClass
			|| CandidateClass == UPCGSettings::StaticClass()
			|| !CandidateClass->IsChildOf(UPCGSettings::StaticClass())
			|| CandidateClass->HasAnyClassFlags(
				CLASS_Abstract | CLASS_Deprecated | CLASS_NewerVersionExists)
			|| !CandidateClass->GetPathName().StartsWith(TEXT("/Script/")))
		{
			return Fail(
				OutError, TEXT("pcg_candidate_name_unresolved"),
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
				OutError, TEXT("pcg_candidate_contract_mismatch"),
				CandidateName);
		}
		const FString CurrentRevision = CandidateRevision(Target, CandidateName);
		if (BindingRevision != CurrentRevision)
		{
			return Fail(
				OutError, TEXT("pcg_candidate_binding_stale"),
				CandidateName, CurrentRevision);
		}
		return true;
	}
}
