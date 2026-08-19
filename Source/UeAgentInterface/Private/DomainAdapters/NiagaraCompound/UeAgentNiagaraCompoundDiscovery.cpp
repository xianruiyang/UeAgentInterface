#include "DomainAdapters/NiagaraCompound/UeAgentNiagaraCompoundDiscovery.h"

#include "DomainAdapters/NiagaraCompound/UeAgentNiagaraCompoundNativeHook.h"
#include "Misc/PackageName.h"
#include "NiagaraNodeFunctionCall.h"
#include "UObject/Package.h"

namespace UE::UAI::DomainAdapters::NiagaraCompound
{
	namespace
	{
		using namespace ContextResource;
		using namespace Contract;
		using namespace Discovery;
		using namespace UeAgentNiagaraStageModuleAuthoring;

		bool Checkpoint(
			const FUeAgentOperationDeadline& Deadline,
			const FUeAgentCancellationToken& Cancellation,
			const FString& Stage,
			FUeAgentDiscoveryError& OutError)
		{
			FUeAgentCancellationRequested Cancelled;
			if (!Cancellation.TryCheckpoint(Cancelled))
			{
				OutError = {
					Cancelled.Code,
					Stage,
					Cancelled.Reason,
				};
				return false;
			}
			FUeAgentOperationDeadlineExceeded Expired;
			if (!Deadline.TryCheckpoint(Stage, Expired))
			{
				OutError = {
					Expired.Code,
					Stage,
					Expired.Stage,
				};
				return false;
			}
			return true;
		}

		void AppendLengthPrefixed(
			FString& Out,
			const FString& Value)
		{
			Out += FString::FromInt(Value.Len());
			Out += TEXT(":");
			Out += Value;
		}

		bool TryReadLengthPrefixed(
			const FString& Text,
			int32& Offset,
			FString& OutValue)
		{
			int32 Colon = INDEX_NONE;
			Colon = Text.Find(
				TEXT(":"),
				ESearchCase::CaseSensitive,
				ESearchDir::FromStart,
				Offset);
			if (Colon == INDEX_NONE)
			{
				return false;
			}
			const FString LengthText =
				Text.Mid(Offset, Colon - Offset);
			int32 Length = 0;
			if (LengthText.IsEmpty()
				|| !LexTryParseString(Length, *LengthText)
				|| Length < 0)
			{
				return false;
			}
			const int32 Start = Colon + 1;
			if (Start > Text.Len()
				|| Length > Text.Len() - Start)
			{
				return false;
			}
			OutValue = Text.Mid(Start, Length);
			Offset = Start + Length;
			return true;
		}

		FUeAgentCanonicalIdentity MakeIdentity(
			const FString& AssetPath,
			const FString& Selector,
			const FString& Generation)
		{
			return FUeAgentCanonicalIdentity(
				TEXT("uai.semantic.niagara.compound_input"),
				TEXT("niagara_compound_input"),
				AssetPath,
				Selector,
				TEXT("niagara_input"),
				Generation);
		}

		FUeAgentCandidateBinding MakeBinding(
			const FString& AssetPath,
			const FUeAgentNiagaraCompoundSelector& Selector,
			const FString& Generation,
			const FUeAgentDescriptorIdentity& OperationDescriptor,
			const int64 Sequence)
		{
			const FString SelectorText =
				MakeNiagaraCompoundSelector(Selector);
			const FUeAgentCanonicalIdentity Identity =
				MakeIdentity(AssetPath, SelectorText, Generation);
			return FUeAgentCandidateBinding(
				TEXT("uai-niagara-compound-binding:")
					+ Identity.GetCanonicalKey(),
				FUeAgentCandidateSummary(
					Identity,
					OperationDescriptor,
					GetNiagaraCompoundCatalogKind(),
					Selector.InputName,
					TEXT("Native Niagara graph input"),
					{
						TEXT("Niagara"),
						Selector.StageKey,
						Selector.ModuleNodeGuid,
					}),
				FUeAgentCatalogRevision(
					GetNiagaraCompoundCatalogSourceId(),
					Generation,
					Sequence),
				AssetPath + TEXT("#") + SelectorText,
				GetNiagaraCompoundReadPortId(),
				{
					EUeAgentCandidateOperation::Read,
					EUeAgentCandidateOperation::Update,
					EUeAgentCandidateOperation::Replace,
				},
				TEXT("niagara-native-graph-enumeration"));
		}

		int64 GetSequence(const UObject& Asset)
		{
			const UPackage* Package = Asset.GetOutermost();
			return Package
				? FMath::Max<int64>(
					1,
					static_cast<int64>(
						GetTypeHash(Package->GetPersistentGuid())))
				: 1;
		}
	}

	bool FUeAgentNiagaraCompoundSelector::IsValid() const
	{
		return !StageKey.IsEmpty()
			&& !ModuleNodeGuid.IsEmpty()
			&& !InputName.IsEmpty();
	}

	FString MakeNiagaraCompoundSelector(
		const FUeAgentNiagaraCompoundSelector& Selector)
	{
		FString Result;
		AppendLengthPrefixed(Result, Selector.StageKey);
		AppendLengthPrefixed(Result, Selector.EmitterHandleId);
		AppendLengthPrefixed(Result, Selector.ModuleNodeGuid);
		AppendLengthPrefixed(Result, Selector.InputName);
		return Result;
	}

	bool TryParseNiagaraCompoundSelector(
		const FString& Text,
		FUeAgentNiagaraCompoundSelector& OutSelector,
		FString& OutErrorCode)
	{
		OutSelector = {};
		OutErrorCode.Reset();
		int32 Offset = 0;
		if (!TryReadLengthPrefixed(
				Text, Offset, OutSelector.StageKey)
			|| !TryReadLengthPrefixed(
				Text, Offset, OutSelector.EmitterHandleId)
			|| !TryReadLengthPrefixed(
				Text, Offset, OutSelector.ModuleNodeGuid)
			|| !TryReadLengthPrefixed(
				Text, Offset, OutSelector.InputName)
			|| Offset != Text.Len()
			|| !OutSelector.IsValid())
		{
			OutSelector = {};
			OutErrorCode =
				TEXT("niagara_compound_selector_invalid");
			return false;
		}
		return true;
	}

	FUeAgentNiagaraCompoundCatalogSource::
		FUeAgentNiagaraCompoundCatalogSource(
			FUeAgentDescriptorIdentity InOperationDescriptor,
			FString InGeneration)
		: OperationDescriptor(MoveTemp(InOperationDescriptor))
		, Generation(MoveTemp(InGeneration))
		, Kinds({GetNiagaraCompoundCatalogKind()})
	{
		Generation.TrimStartAndEndInline();
	}

	const FString&
	FUeAgentNiagaraCompoundCatalogSource::GetSourceId() const
	{
		return GetNiagaraCompoundCatalogSourceId();
	}

	const TArray<FString>&
	FUeAgentNiagaraCompoundCatalogSource::GetCatalogKinds() const
	{
		return Kinds;
	}

	bool FUeAgentNiagaraCompoundCatalogSource::TrySearch(
		const FUeAgentDiscoveryQuery& Query,
		FUeAgentResourceTracker& Resources,
		const FUeAgentOperationDeadline& Deadline,
		const FUeAgentCancellationToken& Cancellation,
		TSharedPtr<const FUeAgentCatalogSnapshot>& OutCatalog,
		FUeAgentDiscoveryError& OutError)
	{
		OutCatalog.Reset();
		OutError = {};
		FString Search = Query.GetSearchText().TrimStartAndEnd();
		if (!OperationDescriptor.IsValid()
			|| Generation.IsEmpty()
			|| Search.IsEmpty())
		{
			OutError.Code =
				TEXT("niagara_compound_catalog_invalid");
			OutError.SubjectId = Search;
			return false;
		}

		FString AssetPath = Search;
		FString ExactPublicRef;
		int32 SelectorSeparator = INDEX_NONE;
		if (Search.FindChar(TEXT('#'), SelectorSeparator))
		{
			AssetPath = Search.Left(SelectorSeparator);
			ExactPublicRef = Search;
		}
		AssetPath.TrimStartAndEndInline();
		if (AssetPath.IsEmpty())
		{
			OutError.Code =
				TEXT("niagara_compound_asset_path_missing");
			return false;
		}

		TArray<FUeAgentCandidateBinding> Bindings;
		if (!TryDiscoverNiagaraCompoundBindings(
				AssetPath,
				OperationDescriptor,
				Generation,
				Resources,
				Deadline,
				Cancellation,
				Bindings,
				OutError))
		{
			return false;
		}

		TArray<FUeAgentCandidateSummary> Candidates;
		TArray<FUeAgentCandidateBinding> MatchedBindings;
		for (const FUeAgentCandidateBinding& Binding : Bindings)
		{
			if (!ExactPublicRef.IsEmpty()
				&& Binding.GetPublicRef() != ExactPublicRef)
			{
				continue;
			}
			Candidates.Add(Binding.GetSummary());
			MatchedBindings.Add(Binding);
		}
		if (!ExactPublicRef.IsEmpty() && MatchedBindings.IsEmpty())
		{
			OutError.Code =
				TEXT("niagara_compound_candidate_not_found");
			OutError.SubjectId = ExactPublicRef;
			return false;
		}
		{
			FScopeLock Lock(&CacheMutex);
			for (const FUeAgentCandidateBinding& Binding :
				MatchedBindings)
			{
				PublicRefByCandidateId.Add(
					Binding.GetSummary()
						.GetIdentity()
						.GetCanonicalKey(),
					Binding.GetPublicRef());
			}
		}
		const FUeAgentCatalogRevision Revision =
			MatchedBindings.IsEmpty()
				? FUeAgentCatalogRevision(
					GetNiagaraCompoundCatalogSourceId(),
					Generation,
					1)
				: MatchedBindings[0].GetCatalogRevision();
		return FUeAgentCatalogSnapshot::TryCreate(
			TEXT("uai-niagara-compound-catalog:")
				+ Generation
				+ TEXT(":")
				+ FString::FromInt(GetTypeHash(AssetPath)),
			GetNiagaraCompoundCatalogKind(),
			Revision,
			EUeAgentCatalogCompleteness::Complete,
			Candidates,
			{},
			Resources,
			OutCatalog,
			OutError);
	}

	bool FUeAgentNiagaraCompoundCatalogSource::TryResolveExact(
		const FString& CandidateId,
		const FUeAgentCatalogRevision& ExpectedRevision,
		FUeAgentResourceTracker& Resources,
		const FUeAgentOperationDeadline& Deadline,
		const FUeAgentCancellationToken& Cancellation,
		TSharedPtr<const FUeAgentCandidateBinding>& OutBinding,
		FUeAgentDiscoveryError& OutError)
	{
		OutBinding.Reset();
		OutError = {};
		FString PublicRef;
		{
			FScopeLock Lock(&CacheMutex);
			if (const FString* Found =
					PublicRefByCandidateId.Find(CandidateId))
			{
				PublicRef = *Found;
			}
		}
		int32 SelectorSeparator = INDEX_NONE;
		if (PublicRef.IsEmpty()
			|| !PublicRef.FindChar(TEXT('#'), SelectorSeparator)
			|| SelectorSeparator <= 0)
		{
			OutError.Code =
				TEXT("niagara_compound_candidate_not_found");
			OutError.SubjectId = CandidateId;
			return false;
		}
		const FString AssetPath =
			PublicRef.Left(SelectorSeparator);
		TArray<FUeAgentCandidateBinding> Bindings;
		if (!TryDiscoverNiagaraCompoundBindings(
				AssetPath,
				OperationDescriptor,
				Generation,
				Resources,
				Deadline,
				Cancellation,
				Bindings,
				OutError))
		{
			return false;
		}
		const FUeAgentCandidateBinding* Matched =
			Bindings.FindByPredicate(
				[&CandidateId, &PublicRef](
					const FUeAgentCandidateBinding& Binding)
				{
					return Binding.GetPublicRef() == PublicRef
						&& Binding.GetSummary()
							.GetIdentity()
							.GetCanonicalKey()
							== CandidateId;
				});
		if (!Matched)
		{
			OutError.Code =
				TEXT("niagara_compound_candidate_not_found");
			OutError.SubjectId = CandidateId;
			return false;
		}
		if (Matched->GetCatalogRevision() != ExpectedRevision)
		{
			OutError.Code = TEXT("catalog_revision_stale");
			OutError.SubjectId = CandidateId;
			return false;
		}
		OutBinding =
			MakeShared<const FUeAgentCandidateBinding>(*Matched);
		return true;
	}

	bool TryDiscoverNiagaraCompoundBindings(
		const FString& AssetPath,
		const FUeAgentDescriptorIdentity& OperationDescriptor,
		const FString& Generation,
		FUeAgentResourceTracker& Resources,
		const FUeAgentOperationDeadline& Deadline,
		const FUeAgentCancellationToken& Cancellation,
		TArray<FUeAgentCandidateBinding>& OutBindings,
		FUeAgentDiscoveryError& OutError)
	{
		OutBindings.Reset();
		OutError = {};
		if (AssetPath.IsEmpty()
			|| Generation.IsEmpty()
			|| !OperationDescriptor.IsValid())
		{
			OutError.Code =
				TEXT("niagara_compound_discovery_invalid");
			return false;
		}
		if (!Checkpoint(
				Deadline,
				Cancellation,
				TEXT("niagara_compound.discovery.load"),
				OutError))
		{
			return false;
		}
		UObject* Asset = LoadObject<UObject>(nullptr, *AssetPath);
		if (!IsValid(Asset))
		{
			OutError = {
				TEXT("niagara_compound_asset_not_found"),
				AssetPath,
				TEXT("the requested Niagara asset could not be loaded"),
			};
			return false;
		}
		TArray<FResolvedStage> Stages;
		FString NativeError;
		if (!EnumerateStages(*Asset, Stages, NativeError))
		{
			OutError = {
				TEXT("niagara_compound_stage_enumeration_failed"),
				AssetPath,
				NativeError,
			};
			return false;
		}
		const int64 Sequence = GetSequence(*Asset);
		for (FResolvedStage& Stage : Stages)
		{
			if (!Checkpoint(
					Deadline,
					Cancellation,
					TEXT("niagara_compound.discovery.stage"),
					OutError))
			{
				return false;
			}
			if (!Stage.OutputNode)
			{
				continue;
			}
			TArray<UNiagaraNodeFunctionCall*> Modules;
			GetModules(*Stage.OutputNode, Modules);
			for (UNiagaraNodeFunctionCall* Module : Modules)
			{
				if (!Module)
				{
					continue;
				}
				TArray<FResolvedModuleInput> Inputs;
				BuildResolvedModuleInputs(*Module, Inputs);
				for (const FResolvedModuleInput& Input : Inputs)
				{
					if (!Input.InputVariable.IsValid())
					{
						continue;
					}
					UNiagaraNodeFunctionCall* Dynamic =
						FindDynamicInputForModuleInput(
							*Module,
							Input.FullInputName,
							NativeError);
					UEdGraphPin* const LiteralPin = Input.VisiblePin
						? Input.VisiblePin
						: FindResolvedInputValuePin(*Module, Input);
					if (!Dynamic
						&& (!LiteralPin || !LiteralPin->LinkedTo.IsEmpty()))
					{
						continue;
					}
					FUeAgentResourceBudgetExceeded BudgetError;
					if (!Resources.TryChargeMany(
							{
								{
									EUeAgentResourceDimension::Items,
									1,
								},
								{
									EUeAgentResourceDimension::WorkUnits,
									1,
								},
							},
							TEXT("/niagara_compound/candidates"),
							BudgetError))
					{
						OutError = {
							BudgetError.Code,
							AssetPath,
							BudgetError.Path,
						};
						return false;
					}
					FUeAgentNiagaraCompoundSelector Selector;
					Selector.StageKey = Stage.StageKey;
					Selector.EmitterHandleId =
						Stage.EmitterHandleId.IsValid()
							? Stage.EmitterHandleId.ToString(
								EGuidFormats::DigitsWithHyphensLower)
							: FString();
					Selector.ModuleNodeGuid =
						Module->NodeGuid.ToString(
							EGuidFormats::DigitsWithHyphensLower);
					Selector.InputName = Input.FullInputName;
					OutBindings.Add(
						MakeBinding(
							AssetPath,
							Selector,
							Generation,
							OperationDescriptor,
							Sequence));
				}
			}
		}
		if (OutBindings.IsEmpty())
		{
			OutError = {
				TEXT("niagara_compound_input_not_found"),
				AssetPath,
				TEXT(
					"no readable literal or dynamic Niagara input "
					"was discovered"),
			};
			return false;
		}
		return true;
	}

	bool TryResolveNiagaraCompoundInput(
		const FUeAgentCandidateBinding& Binding,
		const FString& ExpectedGeneration,
		FUeAgentResolvedNiagaraCompoundInput& OutResolved,
		FString& OutErrorCode,
		FString& OutDetail)
	{
		OutResolved = {};
		OutErrorCode.Reset();
		OutDetail.Reset();
		const FUeAgentCanonicalIdentity& Identity =
			Binding.GetSummary().GetIdentity();
		if (Binding.GetReadPortId()
				!= GetNiagaraCompoundReadPortId()
			|| Identity.GetGeneration() != ExpectedGeneration
			|| Identity.GetOwnerId().IsEmpty())
		{
			OutErrorCode =
				TEXT("niagara_compound_binding_invalid");
			OutDetail = Binding.GetBindingId();
			return false;
		}
		if (!TryParseNiagaraCompoundSelector(
				Identity.GetSelector(),
				OutResolved.Selector,
				OutErrorCode))
		{
			OutDetail = Identity.GetSelector();
			return false;
		}
		UObject* Asset =
			LoadObject<UObject>(nullptr, *Identity.GetOwnerId());
		if (!IsValid(Asset))
		{
			OutErrorCode =
				TEXT("niagara_compound_asset_not_found");
			OutDetail = Identity.GetOwnerId();
			return false;
		}
		TArray<FResolvedStage> Stages;
		FString NativeError;
		if (!EnumerateStages(*Asset, Stages, NativeError))
		{
			OutErrorCode =
				TEXT("niagara_compound_stage_enumeration_failed");
			OutDetail = NativeError;
			return false;
		}
		FResolvedStage* MatchedStage = nullptr;
		for (FResolvedStage& Stage : Stages)
		{
			const FString EmitterId =
				Stage.EmitterHandleId.IsValid()
					? Stage.EmitterHandleId.ToString(
						EGuidFormats::DigitsWithHyphensLower)
					: FString();
			if (Stage.StageKey == OutResolved.Selector.StageKey
				&& EmitterId
					== OutResolved.Selector.EmitterHandleId)
			{
				if (MatchedStage)
				{
					OutErrorCode =
						TEXT("niagara_compound_stage_ambiguous");
					return false;
				}
				MatchedStage = &Stage;
			}
		}
		if (!MatchedStage || !MatchedStage->OutputNode)
		{
			OutErrorCode =
				TEXT("niagara_compound_stage_not_found");
			return false;
		}
		FGuid ModuleGuid;
		if (!FGuid::Parse(
				OutResolved.Selector.ModuleNodeGuid,
				ModuleGuid))
		{
			OutErrorCode =
				TEXT("niagara_compound_module_guid_invalid");
			return false;
		}
		FModuleSelector ModuleSelector;
		ModuleSelector.NodeGuid = ModuleGuid;
		int32 ModuleIndex = INDEX_NONE;
		if (!ResolveModule(
				*MatchedStage->OutputNode,
				ModuleSelector,
				OutResolved.Module,
				ModuleIndex,
				NativeError)
			|| !OutResolved.Module)
		{
			OutErrorCode =
				TEXT("niagara_compound_module_not_found");
			OutDetail = NativeError;
			return false;
		}
		TArray<FResolvedModuleInput> Inputs;
		BuildResolvedModuleInputs(*OutResolved.Module, Inputs);
		FString ResolvedName;
		const FResolvedModuleInput* Input =
			ResolveModuleInputByName(
				Inputs,
				OutResolved.Selector.InputName,
				ResolvedName);
		if (!Input)
		{
			OutErrorCode =
				TEXT("niagara_compound_input_not_found");
			return false;
		}
		OutResolved.Asset = Asset;
		OutResolved.Input = *Input;
		return true;
	}
}
