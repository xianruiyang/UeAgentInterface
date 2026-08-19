#include "QueryOwners/AudioGraph/UeAgentAudioGraphCandidateQuery.h"

#include "Algo/Sort.h"
#include "DomainAdapters/ContentGraphRead/UeAgentContentGraphReadProductionEnvironment.h"
#include "MetasoundFrontendDocument.h"
#include "MetasoundFrontendSearchEngine.h"
#include "MetasoundSource.h"
#include "Sound/SoundCue.h"
#include "Sound/SoundNode.h"
#include "UObject/UObjectIterator.h"

#include <openssl/sha.h>

namespace UE::UAI::QueryOwners::AudioGraph
{
	using namespace DomainAdapters::ContentGraphRead;

	namespace
	{
		bool Fail(
			FAudioGraphCandidateQueryError& OutError,
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
			return Target.GetClass() == USoundCue::StaticClass()
				|| Target.GetClass() == UMetaSoundSource::StaticClass();
		}

		bool ReadTargetRevision(
			UObject& Target,
			FString& OutRevision,
			FAudioGraphCandidateQueryError& OutError)
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
						? FString(TEXT("audio_graph_candidate_target_readback_failed"))
						: ReadError.Code,
					ReadError.SubjectId,
					ReadError.Detail);
			}
			OutRevision = Capture.Revision;
			return true;
		}
	}

	bool TryQueryAudioGraphCandidates(
		UObject& Target,
		const FString& Query,
		FAudioGraphCandidateQueryResult& OutResult,
		FAudioGraphCandidateQueryError& OutError,
		const int32 Limit,
		const int32 Cursor)
	{
		OutResult = {};
		OutError = {};
		if (!IsInGameThread())
		{
			return Fail(OutError, TEXT("audio_graph_candidate_game_thread_required"), Target.GetPathName());
		}
		if (!IsSupportedTarget(Target))
		{
			return Fail(OutError, TEXT("audio_graph_candidate_target_type_unsupported"), Target.GetClass()->GetPathName());
		}
		if (Limit < 1 || Limit > 64 || Cursor < 0)
		{
			return Fail(OutError, TEXT("audio_graph_candidate_page_invalid"), Target.GetPathName(), TEXT("limit must be 1..64 and cursor must be non-negative"));
		}

		OutResult.Target = Target.GetPathName();
		if (!ReadTargetRevision(Target, OutResult.TargetRevision, OutError)) return false;
		const FString Needle = Normalize(Query);
		TArray<FAudioGraphCandidate> Matches;

		if (Target.GetClass() == USoundCue::StaticClass())
		{
			for (TObjectIterator<UClass> It; It; ++It)
			{
				UClass* Class = *It;
				if (!Class || Class == USoundNode::StaticClass()
					|| !Class->IsChildOf(USoundNode::StaticClass())
					|| Class->HasAnyClassFlags(CLASS_Abstract | CLASS_Deprecated | CLASS_NewerVersionExists))
				{
					continue;
				}
				const FString Identity = Class->GetPathName();
				if (!Identity.StartsWith(TEXT("/Script/"))
					|| (!Needle.IsEmpty()
						&& !Normalize(Identity + TEXT("|") + Class->GetName()).Contains(Needle)))
				{
					continue;
				}
				FAudioGraphCandidate Candidate;
				Candidate.Name = Identity;
				Candidate.Label = Class->GetDisplayNameText().ToString();
				Candidate.Kind = TEXT("sound_cue_node");
				Candidate.SupportedOperations = {TEXT("create")};
				Candidate.InputContract = TEXT("uai.audio.sound_cue_node.create/1");
				Candidate.PlacementContract = TEXT("uai.audio.sound-cue-node-placement/1");
				Candidate.RequiredContext = TEXT("target_ref:node_id:class_path:position");
				Candidate.BindingRevision = Sha256(
					Candidate.Name + TEXT("|") + Candidate.InputContract + TEXT("|")
					+ Candidate.PlacementContract + TEXT("|") + Candidate.RequiredContext);
				Matches.Add(MoveTemp(Candidate));
			}
		}
		else
		{
#if WITH_EDITORONLY_DATA
			TArray<FMetasoundFrontendClass> Classes =
				Metasound::Frontend::ISearchEngine::Get().FindAllClasses(false);
			for (const FMetasoundFrontendClass& Class : Classes)
			{
				const FMetasoundFrontendClassName& ClassName = Class.Metadata.GetClassName();
				const FMetasoundFrontendVersionNumber& Version = Class.Metadata.GetVersion();
				if (!ClassName.IsValid() || !Version.IsValid()
					|| Class.Metadata.GetType() == EMetasoundFrontendClassType::Graph
					|| Class.Metadata.GetType() == EMetasoundFrontendClassType::Input
					|| Class.Metadata.GetType() == EMetasoundFrontendClassType::Output)
				{
					continue;
				}
				const FString Identity = ClassName.ToString();
				if (!Needle.IsEmpty()
					&& !Normalize(Identity + TEXT("|") + Class.Metadata.GetDisplayName().ToString()).Contains(Needle))
				{
					continue;
				}
				FAudioGraphCandidate Candidate;
				Candidate.Name = Identity;
				Candidate.Label = Class.Metadata.GetDisplayName().ToString();
				Candidate.Kind = TEXT("metasound_node");
				Candidate.SupportedOperations = {TEXT("create")};
				Candidate.InputContract = TEXT("uai.audio.metasound_node.create/1");
				Candidate.PlacementContract = TEXT("uai.audio.metasound-node-placement/1");
				Candidate.RequiredContext = TEXT("target_ref:class_name:major_version:minor_version:node_id");
				Candidate.MajorVersion = Version.Major;
				Candidate.MinorVersion = Version.Minor;
				Candidate.BindingRevision = Sha256(FString::Printf(
					TEXT("%s|%d|%d|%s|%s|%s"),
					*Candidate.Name,
					Candidate.MajorVersion,
					Candidate.MinorVersion,
					*Candidate.InputContract,
					*Candidate.PlacementContract,
					*Candidate.RequiredContext));
				Matches.Add(MoveTemp(Candidate));
			}
#endif
		}

		Algo::SortBy(Matches, [](const FAudioGraphCandidate& Candidate)
		{
			return Candidate.Name + FString::Printf(TEXT("|%08d|%08d"), Candidate.MajorVersion, Candidate.MinorVersion);
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
}
