#include "QueryOwners/AudioGraph/UeAgentAudioGraphCandidateQuery.h"

#include "Algo/Sort.h"
#include "Commands/PaletteSearch/UeAgentPaletteSearchTypes.h"
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
		const FString SoundCueCandidateKind = TEXT("sound_cue_node");
		const FString SoundCueInputContract =
			TEXT("uai.audio.sound_cue_node.create/1");
		const FString SoundCuePlacementContract =
			TEXT("uai.audio.sound-cue-node-identity/1");
		const FString SoundCueRequiredContext = TEXT("target_ref:object_path");
		const FString MetaSoundCandidateKind = TEXT("metasound_node");
		const FString MetaSoundInputContract =
			TEXT("uai.audio.metasound_node.create/1");
		const FString MetaSoundPlacementContract =
			TEXT("uai.audio.metasound-node-identity/1");
		const FString MetaSoundRequiredContext = TEXT("target_ref:node_guid");
		const FString SoundCueCoverageScope =
			TEXT("sound_cue_loaded_class_approximation");
		const FString MetaSoundCoverageScope =
			TEXT("metasound_frontend_registry_loaded_classes");

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

		FString CandidateRevision(
			const FString& TargetRef,
			const FString& TargetRevision,
			const FString& Name,
			const FString& Kind,
			const FString& InputContract,
			const FString& PlacementContract,
			const FString& RequiredContext,
			const int32 MajorVersion,
			const int32 MinorVersion)
		{
			return Sha256(FString::Printf(
				TEXT("%s|%s|%s|%s|%d|%d|%s|%s|%s"),
				*TargetRef,
				*TargetRevision,
				*Name,
				*Kind,
				MajorVersion,
				MinorVersion,
				*InputContract,
				*PlacementContract,
				*RequiredContext));
		}

		bool SetMetaSoundCandidateIdentity(
			const FMetasoundFrontendClass& Class,
			FAudioGraphCandidate& Candidate,
			FAudioGraphCandidateQueryError& OutError)
		{
			const FMetasoundFrontendClassMetadata& Metadata = Class.Metadata;
			const FMetasoundFrontendVersionNumber& Version =
				Metadata.GetVersion();
			const FString Signature = FString::Printf(
				TEXT("metasound_frontend_class|%s|%d|%d|%d"),
				*Candidate.Name,
				Version.Major,
				Version.Minor,
				static_cast<int32>(Metadata.GetType()));
			UeAgentPaletteSearch::FCandidate Identity;
			Identity.Kind = TEXT("metasound_frontend_class");
			Identity.Name = Candidate.Name;
			Identity.DisplayName = Candidate.Label;
			Identity.CategoryPath = Candidate.Category;
			Identity.Tooltip = Candidate.Summary;
			Identity.Type = Candidate.ActionKind;
			Identity.FullName = Signature;
			FString IdentityError;
			if (!UeAgentPaletteSearch::SetCandidateIdentity(
					TEXT("metasound_frontend_class"),
					Signature,
					Candidate.Name,
					Identity,
					IdentityError))
			{
				return Fail(
					OutError,
					TEXT("audio_graph_candidate_identity_failed"),
					Candidate.Name,
					IdentityError);
			}
			Candidate.Id = Identity.GetPublicId();
			return true;
		}

		bool BuildMetaSoundCandidates(
			const FString& TargetRef,
			const FString& TargetRevision,
			TArray<FAudioGraphCandidate>& OutCandidates,
			FAudioGraphCandidateQueryError& OutError)
		{
			OutCandidates.Reset();
#if WITH_EDITORONLY_DATA
			const TArray<FMetasoundFrontendClass> Classes =
				Metasound::Frontend::ISearchEngine::Get().FindAllClasses(false);
			TSet<FString> SeenIdentities;
			for (const FMetasoundFrontendClass& Class : Classes)
			{
				const FMetasoundFrontendClassName& ClassName =
					Class.Metadata.GetClassName();
				const FMetasoundFrontendVersionNumber& Version =
					Class.Metadata.GetVersion();
				if (!ClassName.IsValid() || !Version.IsValid()
					|| Class.Metadata.GetType() == EMetasoundFrontendClassType::Graph
					|| Class.Metadata.GetType() == EMetasoundFrontendClassType::Input
					|| Class.Metadata.GetType() == EMetasoundFrontendClassType::Output)
				{
					continue;
				}
				const FString Identity = FString::Printf(
					TEXT("%s|%d|%d|%d"),
					*ClassName.ToString(),
					Version.Major,
					Version.Minor,
					static_cast<int32>(Class.Metadata.GetType()));
				if (SeenIdentities.Contains(Identity))
				{
					continue;
				}
				SeenIdentities.Add(Identity);

				FAudioGraphCandidate Candidate;
				Candidate.TargetRef = TargetRef;
				Candidate.TargetRevision = TargetRevision;
				Candidate.Name = ClassName.ToString();
				Candidate.Label = Class.Metadata.GetDisplayName().ToString();
				Candidate.Kind = MetaSoundCandidateKind;
				Candidate.ActionKind = TEXT("metasound_frontend_class");
				Candidate.Category = TEXT("MetaSound");
				Candidate.Summary = Candidate.Label;
				Candidate.WriteSupport = TEXT("frontend_builder_create");
				Candidate.SupportReason =
					TEXT("native_blank_menu_unavailable_headless");
				Candidate.bSupported = true;
				Candidate.SupportedOperations = {TEXT("create")};
				Candidate.InputContract = MetaSoundInputContract;
				Candidate.PlacementContract = MetaSoundPlacementContract;
				Candidate.RequiredContext = MetaSoundRequiredContext;
				Candidate.MajorVersion = Version.Major;
				Candidate.MinorVersion = Version.Minor;
				Candidate.BindingRevision = CandidateRevision(
					Candidate.TargetRef,
					Candidate.TargetRevision,
					Candidate.Name,
					Candidate.Kind,
					Candidate.InputContract,
					Candidate.PlacementContract,
					Candidate.RequiredContext,
					Candidate.MajorVersion,
					Candidate.MinorVersion);
				if (!SetMetaSoundCandidateIdentity(
						Class, Candidate, OutError))
				{
					return false;
				}
				OutCandidates.Add(MoveTemp(Candidate));
			}
#endif
			return true;
		}

		bool TrySelectMetaSoundCandidate(
			TArray<FAudioGraphCandidate>& Candidates,
			const FString& Name,
			const FString& Id,
			FAudioGraphCandidate*& OutCandidate,
			FAudioGraphCandidateQueryError& OutError)
		{
			OutCandidate = nullptr;
			const FString CleanName = Name.TrimStartAndEnd();
			const FString CleanId = Id.TrimStartAndEnd();
			if (CleanName.IsEmpty() && CleanId.IsEmpty())
			{
				return Fail(
					OutError, TEXT("audio_graph_candidate_selector_required"));
			}
			TArray<FAudioGraphCandidate*> Matches;
			for (FAudioGraphCandidate& Candidate : Candidates)
			{
				if (!CleanId.IsEmpty()
					&& !Candidate.Id.Equals(CleanId, ESearchCase::IgnoreCase))
				{
					continue;
				}
				if (!CleanName.IsEmpty()
					&& !Candidate.Name.Equals(
						CleanName, ESearchCase::CaseSensitive))
				{
					continue;
				}
				Matches.Add(&Candidate);
			}
			if (Matches.IsEmpty())
			{
				return Fail(
					OutError,
					TEXT("audio_graph_candidate_not_found"),
					CleanId.IsEmpty() ? CleanName : CleanId);
			}
			if (Matches.Num() != 1)
			{
				return Fail(
					OutError,
					TEXT("audio_graph_candidate_ambiguous"),
					CleanName,
					FString::FromInt(Matches.Num()));
			}
			OutCandidate = Matches[0];
			return true;
		}

		bool IsCurrentMetaSoundClass(
			const FString& Name,
			const int32 MajorVersion,
			const int32 MinorVersion)
		{
#if WITH_EDITORONLY_DATA
			const TArray<FMetasoundFrontendClass> Classes =
				Metasound::Frontend::ISearchEngine::Get().FindAllClasses(false);
			return Classes.ContainsByPredicate(
				[&Name, MajorVersion, MinorVersion](
					const FMetasoundFrontendClass& Class)
				{
					const FMetasoundFrontendClassName& ClassName =
						Class.Metadata.GetClassName();
					const FMetasoundFrontendVersionNumber& Version =
						Class.Metadata.GetVersion();
					return ClassName.IsValid()
						&& Version.IsValid()
						&& Class.Metadata.GetType()
							!= EMetasoundFrontendClassType::Graph
						&& Class.Metadata.GetType()
							!= EMetasoundFrontendClassType::Input
						&& Class.Metadata.GetType()
							!= EMetasoundFrontendClassType::Output
						&& ClassName.ToString() == Name
						&& Version.Major == MajorVersion
						&& Version.Minor == MinorVersion;
				});
#else
			return false;
#endif
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
		const int32 Cursor,
		const FString& CandidateId)
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
		const FString CleanCandidateId = CandidateId.TrimStartAndEnd();
		TArray<FAudioGraphCandidate> Matches;

		if (Target.GetClass() == USoundCue::StaticClass())
		{
			OutResult.CoverageScope = SoundCueCoverageScope;
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
				Candidate.TargetRef = OutResult.Target;
				Candidate.TargetRevision = OutResult.TargetRevision;
				Candidate.Name = Identity;
				Candidate.Label = Class->GetDisplayNameText().ToString();
				Candidate.Kind = SoundCueCandidateKind;
				Candidate.ActionKind = TEXT("loaded_sound_node_class");
				Candidate.WriteSupport = TEXT("legacy_binding_only");
				Candidate.SupportReason =
					TEXT("native_sound_cue_palette_unavailable_headless");
				Candidate.SupportedOperations = {TEXT("create")};
				Candidate.InputContract = SoundCueInputContract;
				Candidate.PlacementContract = SoundCuePlacementContract;
				Candidate.RequiredContext = SoundCueRequiredContext;
				Candidate.BindingRevision = CandidateRevision(
					Candidate.TargetRef,
					Candidate.TargetRevision,
					Candidate.Name,
					Candidate.Kind,
					Candidate.InputContract,
					Candidate.PlacementContract,
					Candidate.RequiredContext,
					Candidate.MajorVersion,
					Candidate.MinorVersion);
				if (!CleanCandidateId.IsEmpty())
				{
					continue;
				}
				Matches.Add(MoveTemp(Candidate));
			}
		}
		else
		{
			OutResult.CoverageScope = MetaSoundCoverageScope;
			TArray<FAudioGraphCandidate> Catalog;
			if (!BuildMetaSoundCandidates(
					OutResult.Target,
					OutResult.TargetRevision,
					Catalog,
					OutError))
			{
				return false;
			}
			for (FAudioGraphCandidate& Candidate : Catalog)
			{
				if ((!CleanCandidateId.IsEmpty()
						&& !Candidate.Id.Equals(
							CleanCandidateId, ESearchCase::IgnoreCase))
					|| (!Needle.IsEmpty()
						&& !Normalize(
							Candidate.Name + TEXT("|") + Candidate.Label)
							.Contains(Needle)))
				{
					continue;
				}
				Matches.Add(MoveTemp(Candidate));
			}
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

	bool TryResolveAudioGraphCandidate(
		UObject& Target,
		const FString& Name,
		const FString& Id,
		FAudioGraphCandidate& OutCandidate,
		FAudioGraphCandidateQueryError& OutError)
	{
		OutCandidate = {};
		OutError = {};
		if (!IsInGameThread())
		{
			return Fail(
				OutError,
				TEXT("audio_graph_candidate_game_thread_required"),
				Target.GetPathName());
		}
		if (Target.GetClass() != UMetaSoundSource::StaticClass())
		{
			return Fail(
				OutError,
				TEXT("audio_graph_candidate_compact_selector_unsupported"),
				Target.GetClass()->GetPathName());
		}
		FString TargetRevision;
		if (!ReadTargetRevision(Target, TargetRevision, OutError))
		{
			return false;
		}
		TArray<FAudioGraphCandidate> Candidates;
		if (!BuildMetaSoundCandidates(
				Target.GetPathName(),
				TargetRevision,
				Candidates,
				OutError))
		{
			return false;
		}
		FAudioGraphCandidate* Selected = nullptr;
		if (!TrySelectMetaSoundCandidate(
				Candidates, Name, Id, Selected, OutError))
		{
			return false;
		}
		OutCandidate = *Selected;
		return true;
	}

	bool IsCurrentAudioGraphCandidateBinding(
		UObject& Target,
		const FString& CandidateTargetRef,
		const FString& CandidateTargetRevision,
		const FString& CandidateName,
		const FString& CandidateKind,
		const TArray<FString>& SupportedOperations,
		const FString& InputContract,
		const FString& PlacementContract,
		const FString& RequiredContext,
		const int32 MajorVersion,
		const int32 MinorVersion,
		const FString& BindingRevision,
		FAudioGraphCandidateQueryError& OutError)
	{
		OutError = {};
		if (!IsInGameThread())
		{
			return Fail(
				OutError,
				TEXT("audio_graph_candidate_game_thread_required"),
				Target.GetPathName());
		}
		if (!IsSupportedTarget(Target))
		{
			return Fail(
				OutError,
				TEXT("audio_graph_candidate_target_type_unsupported"),
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
				TEXT("audio_graph_candidate_target_stale"),
				CandidateTargetRef,
				CurrentTargetRevision);
		}

		const bool bSoundCue = Target.GetClass() == USoundCue::StaticClass();
		const FString& ExpectedKind = bSoundCue
			? SoundCueCandidateKind : MetaSoundCandidateKind;
		const FString& ExpectedInput = bSoundCue
			? SoundCueInputContract : MetaSoundInputContract;
		const FString& ExpectedPlacement = bSoundCue
			? SoundCuePlacementContract : MetaSoundPlacementContract;
		const FString& ExpectedContext = bSoundCue
			? SoundCueRequiredContext : MetaSoundRequiredContext;
		const TArray<FString> ExpectedOperations = {TEXT("create")};
		if (CandidateKind != ExpectedKind
			|| SupportedOperations != ExpectedOperations
			|| InputContract != ExpectedInput
			|| PlacementContract != ExpectedPlacement
			|| RequiredContext != ExpectedContext)
		{
			return Fail(
				OutError,
				TEXT("audio_graph_candidate_contract_mismatch"),
				CandidateName);
		}

		if (bSoundCue)
		{
			UClass* const CandidateClass = LoadObject<UClass>(nullptr, *CandidateName);
			if (!CandidateClass
				|| CandidateClass == USoundNode::StaticClass()
				|| !CandidateClass->IsChildOf(USoundNode::StaticClass())
				|| CandidateClass->HasAnyClassFlags(
					CLASS_Abstract | CLASS_Deprecated | CLASS_NewerVersionExists)
				|| !CandidateClass->GetPathName().StartsWith(TEXT("/Script/"))
				|| MajorVersion != 0
				|| MinorVersion != 0)
			{
				return Fail(
					OutError,
					TEXT("audio_graph_candidate_name_unresolved"),
					CandidateName);
			}
		}
		else if (!IsCurrentMetaSoundClass(
			CandidateName, MajorVersion, MinorVersion))
		{
			return Fail(
				OutError,
				TEXT("audio_graph_candidate_name_unresolved"),
				CandidateName);
		}

		const FString CurrentRevision = CandidateRevision(
			CandidateTargetRef,
			CandidateTargetRevision,
			CandidateName,
			CandidateKind,
			InputContract,
			PlacementContract,
			RequiredContext,
			MajorVersion,
			MinorVersion);
		if (BindingRevision != CurrentRevision)
		{
			return Fail(
				OutError,
				TEXT("audio_graph_candidate_binding_stale"),
				CandidateName,
				CurrentRevision);
		}
		return true;
	}
}
