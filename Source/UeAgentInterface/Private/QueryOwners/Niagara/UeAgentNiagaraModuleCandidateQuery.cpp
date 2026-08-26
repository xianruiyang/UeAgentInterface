#include "QueryOwners/Niagara/UeAgentNiagaraModuleCandidateQuery.h"

#include "Algo/Sort.h"
#include "AssetRegistry/AssetRegistryModule.h"
#include "Commands/PaletteSearch/UeAgentPaletteSearchTypes.h"
#include "DomainAdapters/ContentGraphRead/UeAgentContentGraphReadProductionEnvironment.h"
#include "Domains/Niagara/Authoring/UeAgentNiagaraStageModuleAuthoring.h"
#include "NiagaraEmitter.h"
#include "ViewModels/NiagaraEmitterHandleViewModel.h"
#include "NiagaraNodeFunctionCall.h"
#include "NiagaraSystem.h"
#include "ViewModels/NiagaraSystemViewModel.h"
#include "ViewModels/Stack/INiagaraStackItemGroupAddUtilities.h"
#include "ViewModels/Stack/NiagaraStackEntry.h"
#include "ViewModels/Stack/NiagaraStackScriptItemGroup.h"
#include "ViewModels/Stack/NiagaraStackViewModel.h"

#include <openssl/sha.h>

namespace UE::UAI::QueryOwners::Niagara
{
	using namespace DomainAdapters::ContentGraphRead;
	using namespace UeAgentNiagaraStageModuleAuthoring;

	namespace
	{
		const FString CandidateKind = TEXT("niagara_module");
		const FString AssetModuleActionKind = TEXT("asset_module");
		const FString CoverageScope =
			TEXT("native_niagara_stack_add_module.actions");

		bool Fail(
			FNiagaraModuleCandidateQueryError& OutError,
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

		FString NormalizeSearch(const FString& Value)
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
			return Target.IsA<UNiagaraSystem>();
		}

		bool ReadGuid(
			const FString& Text,
			const TCHAR* ErrorCode,
			FGuid& OutGuid,
			bool& bOutExplicit,
			FNiagaraModuleCandidateQueryError& OutError)
		{
			const FString Trimmed = Text.TrimStartAndEnd();
			bOutExplicit = !Trimmed.IsEmpty();
			OutGuid.Invalidate();
			if (!bOutExplicit)
			{
				return true;
			}
			if (!FGuid::Parse(Trimmed, OutGuid))
			{
				return Fail(OutError, ErrorCode, Trimmed);
			}
			return true;
		}

		bool ResolveContext(
			UObject& Target,
			const FNiagaraModuleCandidateContext& Requested,
			FNiagaraModuleCandidateContext& OutResolved,
			FNiagaraModuleCandidateQueryError& OutError)
		{
			OutResolved = {};
			if (!IsSupportedTarget(Target))
			{
				return Fail(
					OutError,
					TEXT("niagara_candidate_target_type_unsupported"),
					Target.GetClass()->GetPathName());
			}

			FString CanonicalUsage;
			ENiagaraScriptUsage Usage = ENiagaraScriptUsage::Function;
			if (!UeAgentPaletteSearch::TryCanonicalizeNiagaraTargetUsage(
					Requested.TargetUsage, CanonicalUsage)
				|| !TryParseScriptUsage(CanonicalUsage, Usage))
			{
				return Fail(
					OutError,
					TEXT("niagara_candidate_target_usage_invalid"),
					Requested.TargetUsage);
			}

			FGuid RequestedUsageId;
			FGuid RequestedEmitterHandleId;
			bool bUsageIdExplicit = false;
			bool bEmitterHandleIdExplicit = false;
			if (!ReadGuid(
					Requested.TargetUsageId,
					TEXT("niagara_candidate_target_usage_id_invalid"),
					RequestedUsageId,
					bUsageIdExplicit,
					OutError)
				|| !ReadGuid(
					Requested.EmitterHandleId,
					TEXT("niagara_candidate_emitter_handle_id_invalid"),
					RequestedEmitterHandleId,
					bEmitterHandleIdExplicit,
					OutError))
			{
				return false;
			}

			TArray<FResolvedStage> Stages;
			FString StageError;
			if (!EnumerateStages(Target, Stages, StageError))
			{
				return Fail(
					OutError,
					TEXT("niagara_candidate_stage_enumeration_failed"),
					Target.GetPathName(),
					StageError);
			}

			TArray<const FResolvedStage*> Matches;
			for (const FResolvedStage& Stage : Stages)
			{
				if (Stage.Usage != Usage)
				{
					continue;
				}
				if (bUsageIdExplicit && Stage.UsageId != RequestedUsageId)
				{
					continue;
				}
				if (bEmitterHandleIdExplicit
					&& Stage.EmitterHandleId != RequestedEmitterHandleId)
				{
					continue;
				}
				Matches.Add(&Stage);
			}
			if (Matches.IsEmpty())
			{
				return Fail(
					OutError,
					TEXT("niagara_candidate_stage_not_found"),
					Target.GetPathName(),
					CanonicalUsage);
			}
			if (Matches.Num() != 1)
			{
				return Fail(
					OutError,
					TEXT("niagara_candidate_stage_ambiguous"),
					Target.GetPathName(),
					FString::FromInt(Matches.Num()));
			}

			const FResolvedStage& Stage = *Matches[0];
			OutResolved.TargetUsage = ScriptUsageToString(Stage.Usage);
			OutResolved.TargetUsageId = Stage.UsageId.ToString(
				EGuidFormats::DigitsWithHyphensLower);
			if (Stage.EmitterHandleId.IsValid())
			{
				OutResolved.EmitterHandleId = Stage.EmitterHandleId.ToString(
					EGuidFormats::DigitsWithHyphensLower);
			}
			OutResolved.StageKey = Stage.StageKey;
			return true;
		}

		bool ReadTargetRevision(
			UObject& Target,
			FString& OutRevision,
			FNiagaraModuleCandidateQueryError& OutError)
		{
			if (Target.GetOutermost() == GetTransientPackage())
			{
				OutRevision = Sha256(
					TEXT("transient|") + Target.GetClass()->GetPathName());
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
						? FString(TEXT("niagara_candidate_target_readback_failed"))
						: ReadError.Code,
					ReadError.SubjectId,
					ReadError.Detail);
			}
			OutRevision = Capture.Revision;
			return true;
		}

		struct FNativeActionRecord
		{
			FNiagaraModuleCandidate Candidate;
			TSharedPtr<INiagaraStackItemGroupAddAction> Action;
		};

		struct FNativeActionSet
		{
			TSharedPtr<FNiagaraSystemViewModel> SystemViewModel;
			UNiagaraStackScriptItemGroup* Group = nullptr;
			INiagaraStackItemGroupAddUtilities* Utilities = nullptr;
			TArray<FNativeActionRecord> Records;
		};

		FString ActionSignature(
			const INiagaraStackItemGroupAddAction& Action,
			const FString& NativeActionKind,
			const FString& AssetPath)
		{
			TArray<FString> Parts;
			Parts.Add(TEXT("action_kind=") + NativeActionKind);
			if (NativeActionKind == TEXT("new_scratch_pad_module")
				|| NativeActionKind == TEXT("assignment_or_set_specific"))
			{
				return Parts[0];
			}
			if (!AssetPath.IsEmpty())
			{
				Parts.Add(TEXT("asset_path=") + AssetPath);
			}
			if (const TOptional<FNiagaraFavoritesActionData> Favorites =
					Action.GetFavoritesData();
				Favorites.IsSet())
			{
				for (const FName Name : Favorites->ActionIdentifier.Names)
				{
					Parts.Add(TEXT("favorite_name=") + Name.ToString());
				}
				for (const FGuid& Guid : Favorites->ActionIdentifier.Guids)
				{
					Parts.Add(TEXT("favorite_guid=")
						+ Guid.ToString(EGuidFormats::DigitsWithHyphensLower));
				}
			}
			if (Parts.Num() == 1)
			{
				// Last-resort identity for a native action that exposes no favorite id.
				Parts.Add(TEXT("display=") + Action.GetDisplayName().ToString());
				Parts.Add(TEXT("categories=")
					+ FString::Join(Action.GetCategories(), TEXT("/")));
				Parts.Add(TEXT("source=")
					+ Action.GetSourceData().SourceText.ToString());
			}
			return FString::Join(Parts, TEXT("|"));
		}

		FString ScriptAssetPath(
			const INiagaraStackItemGroupAddAction& Action)
		{
			const TOptional<FNiagaraFavoritesActionData> Favorites =
				Action.GetFavoritesData();
			if (!Favorites.IsSet())
			{
				return FString();
			}
			FAssetRegistryModule& AssetRegistry =
				FModuleManager::LoadModuleChecked<FAssetRegistryModule>(
					TEXT("AssetRegistry"));
			for (const FName Name : Favorites->ActionIdentifier.Names)
			{
				const FString ObjectPath = Name.ToString();
				const FAssetData Asset = AssetRegistry.Get().GetAssetByObjectPath(
					FSoftObjectPath(ObjectPath));
				if (Asset.IsValid()
					&& Asset.IsInstanceOf(UNiagaraScript::StaticClass()))
				{
					return Asset.GetSoftObjectPath().ToString();
				}
			}
			return FString();
		}

		FString ActionKind(
			const INiagaraStackItemGroupAddAction& Action,
			const FString& AssetPath,
			int32& InOutUnboundActionOrdinal)
		{
			if (!AssetPath.IsEmpty())
			{
				return AssetModuleActionKind;
			}
			const FString Source = Action.GetSourceData().SourceText.ToString();
			if (Source.Equals(TEXT("Scratch Pad"), ESearchCase::IgnoreCase))
			{
				return TEXT("scratch_pad_module");
			}

			// UE 5.6 FScriptItemGroupAddUtilities appends these two semantic
			// actions, in this order, after asset and scratch-pad modules and
			// before module wizards.  Display text is localized and cannot own
			// action identity.
			const int32 UnboundActionOrdinal = InOutUnboundActionOrdinal++;
			if (UnboundActionOrdinal == 0)
			{
				return TEXT("new_scratch_pad_module");
			}
			if (UnboundActionOrdinal == 1)
			{
				return TEXT("assignment_or_set_specific");
			}
			return TEXT("module_wizard");
		}

		bool BuildNativeActionSet(
			UObject& Target,
			const FNiagaraModuleCandidateContext& ResolvedContext,
			FNativeActionSet& OutSet,
			FNiagaraModuleCandidateQueryError& OutError)
		{
			OutSet = {};
			UNiagaraSystem* const System = Cast<UNiagaraSystem>(&Target);
			if (!System)
			{
				return Fail(
					OutError,
					TEXT("niagara_candidate_target_type_unsupported"),
					Target.GetClass()->GetPathName());
			}
			const FGuid MessageLogGuid = System->GetAssetGuid();
			if (!MessageLogGuid.IsValid())
			{
				return Fail(
					OutError,
					TEXT("niagara_candidate_asset_guid_missing"),
					Target.GetPathName());
			}

			FNiagaraSystemViewModelOptions Options;
			Options.bCanModifyEmittersFromTimeline = false;
			Options.bCanAutoCompile = false;
			Options.bCanSimulate = false;
			Options.bIsForDataProcessingOnly = true;
			Options.bCompileForEdit = false;
			Options.MessageLogGuid = MessageLogGuid;
			OutSet.SystemViewModel = MakeShared<FNiagaraSystemViewModel>();
			OutSet.SystemViewModel->Initialize(*System, Options);

			UNiagaraStackViewModel* StackViewModel = nullptr;
			if (!ResolvedContext.EmitterHandleId.IsEmpty())
			{
				FGuid EmitterHandleId;
				FGuid::Parse(
					ResolvedContext.EmitterHandleId, EmitterHandleId);
				const TSharedPtr<FNiagaraEmitterHandleViewModel> EmitterViewModel =
					OutSet.SystemViewModel->GetEmitterHandleViewModelById(
						EmitterHandleId);
				StackViewModel = EmitterViewModel.IsValid()
					? EmitterViewModel->GetEmitterStackViewModel()
					: nullptr;
			}
			else
			{
				StackViewModel = OutSet.SystemViewModel->GetSystemStackViewModel();
			}
			UNiagaraStackEntry* const Root = StackViewModel
				? StackViewModel->GetRootEntry()
				: nullptr;
			if (!Root)
			{
				return Fail(
					OutError,
					TEXT("niagara_candidate_native_stack_unavailable"),
					Target.GetPathName());
			}

			ENiagaraScriptUsage Usage = ENiagaraScriptUsage::Function;
			FGuid UsageId;
			if (!TryParseScriptUsage(ResolvedContext.TargetUsage, Usage)
				|| !FGuid::Parse(ResolvedContext.TargetUsageId, UsageId))
			{
				return Fail(
					OutError,
					TEXT("niagara_candidate_resolved_context_invalid"),
					ResolvedContext.StageKey);
			}
			TArray<UNiagaraStackScriptItemGroup*> Groups;
			Root->GetUnfilteredChildrenOfType<UNiagaraStackScriptItemGroup>(
				Groups, true);
			TArray<UNiagaraStackScriptItemGroup*> Matches;
			for (UNiagaraStackScriptItemGroup* Group : Groups)
			{
				if (Group
					&& Group->GetScriptUsage() == Usage
					&& Group->GetScriptUsageId() == UsageId)
				{
					Matches.Add(Group);
				}
			}
			if (Matches.Num() != 1)
			{
				return Fail(
					OutError,
					Matches.IsEmpty()
						? TEXT("niagara_candidate_native_stack_not_found")
						: TEXT("niagara_candidate_native_stack_ambiguous"),
					ResolvedContext.StageKey,
					FString::FromInt(Matches.Num()));
			}
			OutSet.Group = Matches[0];
			OutSet.Utilities = OutSet.Group->GetAddUtilities();
			if (!OutSet.Utilities)
			{
				return Fail(
					OutError,
					TEXT("niagara_candidate_native_add_utilities_missing"),
					ResolvedContext.StageKey);
			}

			FNiagaraStackItemGroupAddOptions AddOptions;
			AddOptions.bIncludeDeprecated = false;
			AddOptions.bIncludeNonLibrary = true;
			TArray<TSharedRef<INiagaraStackItemGroupAddAction>> Actions;
			OutSet.Utilities->GenerateAddActions(Actions, AddOptions);
			int32 UnboundActionOrdinal = 0;
			for (const TSharedRef<INiagaraStackItemGroupAddAction>& Action
				: Actions)
			{
				const FString AssetPath = ScriptAssetPath(*Action);
				const FString NativeActionKind = ActionKind(
					*Action, AssetPath, UnboundActionOrdinal);
				const FString Signature = ActionSignature(
					*Action, NativeActionKind, AssetPath);
				UeAgentPaletteSearch::FCandidate IdentityCandidate;
				IdentityCandidate.Kind = CandidateKind;
				IdentityCandidate.Name = Action->GetDisplayName().ToString();
				IdentityCandidate.DisplayName = IdentityCandidate.Name;
				IdentityCandidate.FullName = Signature;
				IdentityCandidate.AuthoringSelectors.Add(
					TEXT("native_action_signature"), Signature);
				FString IdentityError;
				if (!UeAgentPaletteSearch::SetCandidateIdentity(
						CandidateKind,
						Signature,
						Signature,
						IdentityCandidate,
						IdentityError))
				{
					return Fail(
						OutError,
						TEXT("niagara_candidate_identity_failed"),
						IdentityCandidate.Name,
						IdentityError);
				}

				FNativeActionRecord& Record = OutSet.Records.AddDefaulted_GetRef();
				Record.Action = Action;
				Record.Candidate.Id = IdentityCandidate.GetPublicId();
				Record.Candidate.Name = IdentityCandidate.Name;
				Record.Candidate.Label = IdentityCandidate.DisplayName;
				Record.Candidate.Kind = CandidateKind;
				Record.Candidate.ActionKind = NativeActionKind;
				Record.Candidate.Category = FString::Join(
					Action->GetCategories(), TEXT("/"));
				Record.Candidate.ScriptAssetPath = AssetPath;
				Record.Candidate.bSupported =
					NativeActionKind == AssetModuleActionKind;
				Record.Candidate.SupportReason =
					Record.Candidate.bSupported
						? TEXT("Replays the exact data-processing Stack action without opening an editor window.")
						: TEXT("Search-equivalent native action; creation requires a dedicated noninteractive HJSON contract before it can be executed.");
				Record.Candidate.SupportedOperations =
					Record.Candidate.bSupported
						? TArray<FString>{TEXT("create")}
						: TArray<FString>{};
			}
			return true;
		}

		bool CollectCandidates(
			UObject& Target,
			const FNiagaraModuleCandidateContext& RequestedContext,
			FNiagaraModuleCandidateContext& OutResolvedContext,
			TArray<FNiagaraModuleCandidate>& OutCandidates,
			FNiagaraModuleCandidateQueryError& OutError)
		{
			OutCandidates.Reset();
			if (!ResolveContext(
					Target, RequestedContext, OutResolvedContext, OutError))
			{
				return false;
			}
			FNativeActionSet ActionSet;
			if (!BuildNativeActionSet(
					Target, OutResolvedContext, ActionSet, OutError))
			{
				return false;
			}
			for (const FNativeActionRecord& Record : ActionSet.Records)
			{
				OutCandidates.Add(Record.Candidate);
			}
			return true;
		}
	}

	bool TryQueryNiagaraModuleCandidates(
		UObject& Target,
		const FString& Query,
		const FNiagaraModuleCandidateContext& Context,
		FNiagaraModuleCandidateQueryResult& OutResult,
		FNiagaraModuleCandidateQueryError& OutError,
		const int32 Limit,
		const int32 Cursor)
	{
		OutResult = {};
		OutError = {};
		if (!IsInGameThread())
		{
			return Fail(
				OutError,
				TEXT("niagara_candidate_game_thread_required"),
				Target.GetPathName());
		}
		if (Limit < 1 || Limit > 20 || Cursor < 0)
		{
			return Fail(
				OutError,
				TEXT("niagara_candidate_page_invalid"),
				Target.GetPathName(),
				TEXT("limit must be 1..20 and cursor must be non-negative"));
		}

		OutResult.Target = Target.GetPathName();
		OutResult.CoverageScope = CoverageScope;
		OutResult.bCoverageComplete = true;
		OutResult.bCreationCoverageComplete = true;
		if (!ReadTargetRevision(Target, OutResult.TargetRevision, OutError))
		{
			return false;
		}

		TArray<FNiagaraModuleCandidate> Candidates;
		if (!CollectCandidates(
				Target, Context, OutResult.Context, Candidates, OutError))
		{
			return false;
		}
		for (const FNiagaraModuleCandidate& Candidate : Candidates)
		{
			if (!Candidate.bSupported)
			{
				OutResult.bCreationCoverageComplete = false;
				OutResult.UnsupportedActionKinds.AddUnique(
					Candidate.ActionKind);
			}
		}

		const FString NormalizedQuery = NormalizeSearch(Query);
		TArray<FNiagaraModuleCandidate> MatchingCandidates;
		for (FNiagaraModuleCandidate& Candidate : Candidates)
		{
			const FString Searchable = NormalizeSearch(
				Candidate.Name + TEXT("|") + Candidate.Label + TEXT("|")
					+ Candidate.Category + TEXT("|")
					+ Candidate.ScriptAssetPath);
			if (NormalizedQuery.IsEmpty()
				|| Searchable.Contains(NormalizedQuery))
			{
				MatchingCandidates.Add(MoveTemp(Candidate));
			}
		}
		Algo::Sort(MatchingCandidates,
			[](const FNiagaraModuleCandidate& Left,
				const FNiagaraModuleCandidate& Right)
			{
				const int32 NameCompare = Left.Name.Compare(
					Right.Name, ESearchCase::IgnoreCase);
				return NameCompare != 0
					? NameCompare < 0
					: Left.Id < Right.Id;
			});

		OutResult.TotalMatches = MatchingCandidates.Num();
		const int32 Start = FMath::Min(Cursor, MatchingCandidates.Num());
		const int32 End = FMath::Min(Start + Limit, MatchingCandidates.Num());
		for (int32 Index = Start; Index < End; ++Index)
		{
			OutResult.Candidates.Add(MoveTemp(MatchingCandidates[Index]));
		}
		OutResult.bHasMore = End < OutResult.TotalMatches;
		OutResult.bComplete = !OutResult.bHasMore;
		OutResult.NextCursor = OutResult.bHasMore ? End : INDEX_NONE;
		return true;
	}

	bool TryResolveNiagaraModuleCandidate(
		UObject& Target,
		const FString& CandidateName,
		const FString& CandidateId,
		const FNiagaraModuleCandidateContext& Context,
		FNiagaraModuleCandidate& OutCandidate,
		FNiagaraModuleCandidateContext& OutResolvedContext,
		FNiagaraModuleCandidateQueryError& OutError)
	{
		OutCandidate = {};
		OutResolvedContext = {};
		OutError = {};
		const FString Name = CandidateName.TrimStartAndEnd();
		const FString Id = CandidateId.TrimStartAndEnd();
		if (Name.IsEmpty() && Id.IsEmpty())
		{
			return Fail(
				OutError,
				TEXT("niagara_candidate_selector_required"),
				Target.GetPathName());
		}

		TArray<FNiagaraModuleCandidate> Candidates;
		if (!CollectCandidates(
				Target, Context, OutResolvedContext, Candidates, OutError))
		{
			return false;
		}

		TArray<const FNiagaraModuleCandidate*> Matches;
		for (const FNiagaraModuleCandidate& Candidate : Candidates)
		{
			const bool bNameMatches = Name.IsEmpty()
				|| Candidate.Name.Equals(Name, ESearchCase::CaseSensitive)
				// Read-only compatibility for the former path-as-name contract.
				|| Candidate.ScriptAssetPath.Equals(
					Name, ESearchCase::CaseSensitive);
			const bool bIdMatches = Id.IsEmpty()
				|| Candidate.Id.Equals(Id, ESearchCase::CaseSensitive);
			if (bNameMatches && bIdMatches)
			{
				Matches.Add(&Candidate);
			}
		}
		if (Matches.IsEmpty())
		{
			return Fail(
				OutError,
				TEXT("niagara_candidate_selector_unresolved"),
				Id.IsEmpty() ? Name : Id);
		}
		if (Matches.Num() != 1)
		{
			return Fail(
				OutError,
				TEXT("niagara_candidate_selector_ambiguous"),
				Id.IsEmpty() ? Name : Id,
				FString::FromInt(Matches.Num()));
		}
		if (!Matches[0]->bSupported)
		{
			return Fail(
				OutError,
				TEXT("niagara_candidate_action_unsupported"),
				Matches[0]->Id,
				Matches[0]->SupportReason);
		}
		OutCandidate = *Matches[0];
		return true;
	}

	bool TryPerformNiagaraModuleCandidate(
		UObject& Target,
		const FString& CandidateName,
		const FString& CandidateId,
		const FNiagaraModuleCandidateContext& Context,
		const int32 TargetIndex,
		FNiagaraModuleCandidate& OutCandidate,
		FNiagaraModuleCandidateContext& OutResolvedContext,
		UNiagaraNodeFunctionCall*& OutCreatedNode,
		FNiagaraModuleCandidateQueryError& OutError)
	{
		OutCandidate = {};
		OutResolvedContext = {};
		OutCreatedNode = nullptr;
		OutError = {};
		const FString Name = CandidateName.TrimStartAndEnd();
		const FString Id = CandidateId.TrimStartAndEnd();
		if (Name.IsEmpty() && Id.IsEmpty())
		{
			return Fail(
				OutError,
				TEXT("niagara_candidate_selector_required"),
				Target.GetPathName());
		}
		if (!ResolveContext(Target, Context, OutResolvedContext, OutError))
		{
			return false;
		}

		FNativeActionSet ActionSet;
		if (!BuildNativeActionSet(
				Target, OutResolvedContext, ActionSet, OutError))
		{
			return false;
		}
		TArray<const FNativeActionRecord*> Matches;
		for (const FNativeActionRecord& Record : ActionSet.Records)
		{
			const bool bNameMatches = Name.IsEmpty()
				|| Record.Candidate.Name.Equals(
					Name, ESearchCase::CaseSensitive)
				|| Record.Candidate.ScriptAssetPath.Equals(
					Name, ESearchCase::CaseSensitive);
			const bool bIdMatches = Id.IsEmpty()
				|| Record.Candidate.Id.Equals(Id, ESearchCase::CaseSensitive);
			if (bNameMatches && bIdMatches)
			{
				Matches.Add(&Record);
			}
		}
		if (Matches.IsEmpty())
		{
			return Fail(
				OutError,
				TEXT("niagara_candidate_selector_unresolved"),
				Id.IsEmpty() ? Name : Id);
		}
		if (Matches.Num() != 1)
		{
			return Fail(
				OutError,
				TEXT("niagara_candidate_selector_ambiguous"),
				Id.IsEmpty() ? Name : Id,
				FString::FromInt(Matches.Num()));
		}
		const FNativeActionRecord& Match = *Matches[0];
		if (!Match.Candidate.bSupported || !Match.Action.IsValid())
		{
			return Fail(
				OutError,
				TEXT("niagara_candidate_action_unsupported"),
				Match.Candidate.Id,
				Match.Candidate.SupportReason);
		}

		UNiagaraNodeOutput* const OutputNode =
			ActionSet.Group ? ActionSet.Group->GetScriptOutputNode() : nullptr;
		if (!OutputNode || !ActionSet.Utilities)
		{
			return Fail(
				OutError,
				TEXT("niagara_candidate_native_stack_unavailable"),
				OutResolvedContext.StageKey);
		}
		TArray<UNiagaraNodeFunctionCall*> BeforeModules;
		GetModules(*OutputNode, BeforeModules);
		TSet<FGuid> BeforeGuids;
		for (const UNiagaraNodeFunctionCall* Module : BeforeModules)
		{
			if (Module)
			{
				BeforeGuids.Add(Module->NodeGuid);
			}
		}

		ActionSet.Utilities->ExecuteAddAction(
			Match.Action.ToSharedRef(), TargetIndex);
		TArray<UNiagaraNodeFunctionCall*> AfterModules;
		GetModules(*OutputNode, AfterModules);
		TArray<UNiagaraNodeFunctionCall*> CreatedModules;
		for (UNiagaraNodeFunctionCall* Module : AfterModules)
		{
			if (Module && !BeforeGuids.Contains(Module->NodeGuid))
			{
				CreatedModules.Add(Module);
			}
		}
		if (CreatedModules.Num() != 1)
		{
			return Fail(
				OutError,
				TEXT("niagara_candidate_native_action_result_invalid"),
				Match.Candidate.Id,
				FString::FromInt(CreatedModules.Num()));
		}
		OutCandidate = Match.Candidate;
		OutCreatedNode = CreatedModules[0];
		return true;
	}
}
