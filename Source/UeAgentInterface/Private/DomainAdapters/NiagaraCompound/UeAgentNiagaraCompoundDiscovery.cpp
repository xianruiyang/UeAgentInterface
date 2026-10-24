#include "DomainAdapters/NiagaraCompound/UeAgentNiagaraCompoundDiscovery.h"

#include "Core/Contract/UeAgentDiagnostic.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"

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

		FDiagnostic DiscoveryFact(const TCHAR* Code, const TCHAR* Reason, const FString& Path,
			TSharedPtr<FJsonValue> Actual, TSharedPtr<FJsonValue> Expected,
			const FString& Target = FString(), const FString& Stage = TEXT("niagara_discovery"))
		{
			auto Error = FDiagnostic::Failure(Code, Reason, Stage);
			Error.SourceKind = TEXT("native_result");
			Error.PropertyPath = Path;
			Error.ActualValuePresent = Actual.IsValid();
			Error.ExpectedValuePresent = Expected.IsValid();
			Error.ActualValue = MoveTemp(Actual);
			Error.ExpectedValue = MoveTemp(Expected);
			Error.TargetPath = Target;
			return Error;
		}

		bool SelectorReadFailure(FDiagnostic& OutError, const TCHAR* Reason, const FString& Path,
			TSharedPtr<FJsonValue> Actual, TSharedPtr<FJsonValue> Expected, const FString& Text)
		{
			OutError = DiscoveryFact(TEXT("niagara_compound_selector_invalid"), Reason, Path,
				MoveTemp(Actual), MoveTemp(Expected), FString(), TEXT("candidate_resolve"));
			OutError.ActualEvidence = MakeShared<FJsonValueString>(Text);
			return false;
		}

		bool Checkpoint(
			const FUeAgentOperationDeadline& Deadline,
			const FUeAgentCancellationToken& Cancellation,
			const FString& Stage,
			FUeAgentDiscoveryError& OutError)
		{
			FUeAgentCancellationRequested Cancelled;
			if (!Cancellation.TryCheckpoint(Cancelled))
			{
				OutError = MoveTemp(Cancelled);
				if (OutError.Stage.IsEmpty()) OutError.Stage = Stage;
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
			FString& OutValue,
			const TCHAR* Part,
			FDiagnostic& OutError)
		{
			const FString Path = FString(TEXT("/selector/")) + Part;
			int32 Colon = INDEX_NONE;
			Colon = Text.Find(
				TEXT(":"),
				ESearchCase::CaseSensitive,
				ESearchDir::FromStart,
				Offset);
			if (Colon == INDEX_NONE)
			{
				return SelectorReadFailure(OutError, TEXT("The Niagara selector segment has no length separator."),
					Path + TEXT("/length_separator_present"), MakeShared<FJsonValueBoolean>(false),
					MakeShared<FJsonValueBoolean>(true), Text);
			}
			const FString LengthText =
				Text.Mid(Offset, Colon - Offset);
			int32 Length = 0;
			if (LengthText.IsEmpty())
				return SelectorReadFailure(OutError, TEXT("The Niagara selector segment has an empty length."),
					Path + TEXT("/length"), nullptr, MakeShared<FJsonValueString>(TEXT("non-negative int32 length")), Text);
			if (!LexTryParseString(Length, *LengthText))
				return SelectorReadFailure(OutError, TEXT("The Niagara selector segment length is not an int32."),
					Path + TEXT("/length_is_int32"), MakeShared<FJsonValueBoolean>(false), MakeShared<FJsonValueBoolean>(true), Text);
			if (Length < 0)
				return SelectorReadFailure(OutError, TEXT("The Niagara selector segment length cannot be negative."),
					Path + TEXT("/length"), MakeShared<FJsonValueNumber>(Length),
					MakeShared<FJsonValueString>(TEXT("non-negative length")), Text);
			const int32 Start = Colon + 1;
			if (Start > Text.Len()
				|| Length > Text.Len() - Start)
			{
				return SelectorReadFailure(OutError, TEXT("The Niagara selector segment is shorter than its declared length."),
					Path + TEXT("/available_characters"), MakeShared<FJsonValueNumber>(Text.Len() - Start),
					MakeShared<FJsonValueNumber>(Length), Text);
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
		UE::UAI::Contract::FDiagnostic& OutError)
	{
		OutSelector = {};
		OutError.Reset();
		int32 Offset = 0;
		if (!TryReadLengthPrefixed(Text, Offset, OutSelector.StageKey, TEXT("stage_key"), OutError)
			|| !TryReadLengthPrefixed(Text, Offset, OutSelector.EmitterHandleId, TEXT("emitter_handle_id"), OutError)
			|| !TryReadLengthPrefixed(Text, Offset, OutSelector.ModuleNodeGuid, TEXT("module_node_guid"), OutError)
			|| !TryReadLengthPrefixed(Text, Offset, OutSelector.InputName, TEXT("input_name"), OutError))
		{
			OutSelector = {};
			return false;
		}
		if (Offset != Text.Len())
		{
			OutSelector = {};
			return SelectorReadFailure(OutError, TEXT("The Niagara selector has trailing characters after its four segments."),
				TEXT("/selector/trailing_characters"), MakeShared<FJsonValueNumber>(Text.Len() - Offset),
				MakeShared<FJsonValueNumber>(0), Text);
		}
		if (!OutSelector.IsValid())
		{
			const FString Path = OutSelector.StageKey.IsEmpty() ? TEXT("/selector/stage_key/non_empty")
				: (OutSelector.ModuleNodeGuid.IsEmpty() ? TEXT("/selector/module_node_guid/non_empty") : TEXT("/selector/input_name/non_empty"));
			OutSelector = {};
			return SelectorReadFailure(OutError, TEXT("The Niagara selector requires non-empty stage, module and input segments."),
				Path, MakeShared<FJsonValueBoolean>(false), MakeShared<FJsonValueBoolean>(true), Text);
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
			if (!OperationDescriptor.IsValid())
			{
				OutError = OperationDescriptor.GetValidationDiagnostic();
				if (!OutError.IsValid()) OutError = DiscoveryFact(TEXT("niagara_compound_catalog_invalid"),
					TEXT("The Niagara input catalog has no valid operation descriptor."), TEXT("/operation_descriptor/valid"),
					MakeShared<FJsonValueBoolean>(false), MakeShared<FJsonValueBoolean>(true));
			}
			else if (Generation.IsEmpty())
				OutError = DiscoveryFact(TEXT("niagara_compound_catalog_invalid"),
					TEXT("The Niagara input catalog has no generation binding."), TEXT("/generation/present"),
					MakeShared<FJsonValueBoolean>(false), MakeShared<FJsonValueBoolean>(true));
			else OutError = DiscoveryFact(TEXT("niagara_compound_catalog_invalid"),
				TEXT("Niagara input discovery requires an asset search path."), TEXT("/search"),
				MakeShared<FJsonValueString>(Search), MakeShared<FJsonValueString>(TEXT("non-empty asset path")));
			if (OutError.SubjectId.IsEmpty()) OutError.SubjectId = Search;
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
			OutError = DiscoveryFact(TEXT("niagara_compound_asset_path_missing"),
				TEXT("The Niagara input reference has no asset path before its selector."), TEXT("/asset_path"),
				MakeShared<FJsonValueString>(AssetPath), MakeShared<FJsonValueString>(TEXT("non-empty asset path")));
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
			OutError = DiscoveryFact(TEXT("niagara_compound_candidate_not_found"),
				TEXT("No readable Niagara input matches the selected public reference."), TEXT("/candidates/matched_count"),
				MakeShared<FJsonValueNumber>(0), MakeShared<FJsonValueNumber>(1), AssetPath);
			OutError.SubjectId = ExactPublicRef;
			OutError.ActualEvidence = MakeShared<FJsonValueString>(ExactPublicRef);
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
			OutError = DiscoveryFact(TEXT("niagara_compound_candidate_not_found"),
				PublicRef.IsEmpty() ? TEXT("The selected Niagara input has no cached public reference.")
					: TEXT("The cached Niagara input reference has no usable asset and selector boundary."),
				PublicRef.IsEmpty() ? TEXT("/candidate/cached") : TEXT("/candidate/reference_valid"),
				MakeShared<FJsonValueBoolean>(false), MakeShared<FJsonValueBoolean>(true));
			OutError.SubjectId = CandidateId;
			OutError.ActualEvidence = MakeShared<FJsonValueString>(PublicRef);
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
			OutError = DiscoveryFact(TEXT("niagara_compound_candidate_not_found"),
				TEXT("Fresh Niagara input discovery no longer contains the selected candidate."), TEXT("/candidates/matched_count"),
				MakeShared<FJsonValueNumber>(0), MakeShared<FJsonValueNumber>(1), AssetPath);
			OutError.SubjectId = CandidateId;
			OutError.ActualEvidence = MakeShared<FJsonValueString>(PublicRef);
			return false;
		}
		if (Matched->GetCatalogRevision() != ExpectedRevision)
		{
			OutError = DiscoveryFact(TEXT("catalog_revision_stale"),
				TEXT("The Niagara input catalog changed after the candidate was selected."), TEXT("/catalog/revision_matches_expected"),
				MakeShared<FJsonValueBoolean>(false), MakeShared<FJsonValueBoolean>(true), AssetPath);
			OutError.SubjectId = CandidateId;
			const auto RevisionEvidence = [](const FUeAgentCatalogRevision& Revision) -> TSharedPtr<FJsonValue>
			{
				const TSharedRef<FJsonObject> Fields = MakeShared<FJsonObject>();
				Fields->SetStringField(TEXT("source"), Revision.GetSourceId());
				Fields->SetStringField(TEXT("generation"), Revision.GetGeneration());
				Fields->SetStringField(TEXT("sequence"), FString::Printf(TEXT("%lld"), Revision.GetSequence()));
				return MakeShared<FJsonValueObject>(Fields);
			};
			OutError.ActualEvidence = RevisionEvidence(Matched->GetCatalogRevision());
			OutError.ExpectedEvidence = RevisionEvidence(ExpectedRevision);
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
			if (AssetPath.IsEmpty())
				OutError = DiscoveryFact(TEXT("niagara_compound_discovery_invalid"),
					TEXT("Niagara input discovery requires an asset path."), TEXT("/asset_path"),
					MakeShared<FJsonValueString>(AssetPath), MakeShared<FJsonValueString>(TEXT("non-empty asset path")));
			else if (Generation.IsEmpty())
				OutError = DiscoveryFact(TEXT("niagara_compound_discovery_invalid"),
					TEXT("Niagara input discovery has no generation binding."), TEXT("/generation/present"),
					MakeShared<FJsonValueBoolean>(false), MakeShared<FJsonValueBoolean>(true), AssetPath);
			else
			{
				OutError = OperationDescriptor.GetValidationDiagnostic();
				if (!OutError.IsValid()) OutError = DiscoveryFact(TEXT("niagara_compound_discovery_invalid"),
					TEXT("Niagara input discovery has no valid operation descriptor."), TEXT("/operation_descriptor/valid"),
					MakeShared<FJsonValueBoolean>(false), MakeShared<FJsonValueBoolean>(true), AssetPath);
				if (OutError.TargetPath.IsEmpty()) OutError.TargetPath = AssetPath;
			}
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
			OutError = DiscoveryFact(TEXT("niagara_compound_asset_not_found"),
				TEXT("The requested Niagara asset could not be loaded."), TEXT("/asset/available"),
				MakeShared<FJsonValueBoolean>(false), MakeShared<FJsonValueBoolean>(true), AssetPath);
			return false;
		}
		TArray<FResolvedStage> Stages;
		UE::UAI::Contract::FDiagnostic NativeError;
		if (!EnumerateStages(*Asset, Stages, NativeError))
		{
			OutError = MoveTemp(NativeError);
			if (OutError.TargetPath.IsEmpty()) OutError.TargetPath = AssetPath;
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
			if (!GetModules(*Stage.OutputNode, Modules, OutError))
			{
				if (OutError.TargetPath.IsEmpty()) OutError.TargetPath = AssetPath;
				return false;
			}
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
					FString DynamicInputName;
                    UNiagaraNodeFunctionCall* Dynamic =
						FindDynamicInputForModuleInput(*Module, Input.FullInputName, DynamicInputName, OutError);
					if (OutError.IsValid())
					{
						if (OutError.TargetPath.IsEmpty()) OutError.TargetPath = AssetPath;
						if (OutError.CandidateName.IsEmpty()) OutError.CandidateName = Input.FullInputName;
						return false;
					}
					UEdGraphPin* const LiteralPin = Input.VisiblePin
						? Input.VisiblePin
						: FindResolvedInputValuePin(*Module, Input);
					if (!Dynamic
						&& (!LiteralPin || !LiteralPin->LinkedTo.IsEmpty()))
					{
						continue;
					}
					FUeAgentResourceChargeError ChargeError;
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
							ChargeError))
					{
						OutError = MoveTemp(ChargeError);
						if (OutError.TargetPath.IsEmpty()) OutError.TargetPath = AssetPath;
						if (OutError.CandidateName.IsEmpty()) OutError.CandidateName = Input.FullInputName;
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
			OutError = DiscoveryFact(TEXT("niagara_compound_input_not_found"),
				TEXT("No readable literal or dynamic input was discovered in the Niagara asset."), TEXT("/readable_inputs/count"),
				MakeShared<FJsonValueNumber>(0), MakeShared<FJsonValueString>(TEXT("at least one readable input")), AssetPath);
			return false;
		}
		return true;
	}

	bool TryResolveNiagaraCompoundInput(
		const FUeAgentCandidateBinding& Binding,
		const FString& ExpectedGeneration,
		FUeAgentResolvedNiagaraCompoundInput& OutResolved,
		UE::UAI::Contract::FDiagnostic& OutError)
	{
		OutResolved = {};
		OutError.Reset();
		const FUeAgentCanonicalIdentity& Identity =
			Binding.GetSummary().GetIdentity();
		auto ResolveFact = [&](const TCHAR* Code, const TCHAR* Reason, const FString& Path,
			TSharedPtr<FJsonValue> Actual, TSharedPtr<FJsonValue> Expected)
		{
			auto Error = DiscoveryFact(Code, Reason, Path, MoveTemp(Actual), MoveTemp(Expected),
				Identity.GetOwnerId(), TEXT("candidate_resolve"));
			Error.CandidateName = OutResolved.Selector.InputName;
			Error.SubjectId = Binding.GetBindingId();
			return Error;
		};
		if (Binding.GetReadPortId() != GetNiagaraCompoundReadPortId())
		{
			OutError = ResolveFact(TEXT("niagara_compound_binding_invalid"),
				TEXT("The selected Niagara input binding uses a different read port."), TEXT("/binding/read_port_matches_expected"),
				MakeShared<FJsonValueBoolean>(false), MakeShared<FJsonValueBoolean>(true));
			OutError.ActualEvidence = MakeShared<FJsonValueString>(Binding.GetReadPortId());
			OutError.ExpectedEvidence = MakeShared<FJsonValueString>(GetNiagaraCompoundReadPortId());
			return false;
		}
		if (Identity.GetGeneration() != ExpectedGeneration)
		{
			OutError = ResolveFact(TEXT("niagara_compound_binding_invalid"),
				TEXT("The selected Niagara input binding belongs to a different generation."), TEXT("/binding/generation_matches_expected"),
				MakeShared<FJsonValueBoolean>(false), MakeShared<FJsonValueBoolean>(true));
			OutError.ActualEvidence = MakeShared<FJsonValueString>(Identity.GetGeneration());
			OutError.ExpectedEvidence = MakeShared<FJsonValueString>(ExpectedGeneration);
			return false;
		}
		if (Identity.GetOwnerId().IsEmpty())
		{
			OutError = ResolveFact(TEXT("niagara_compound_binding_invalid"),
				TEXT("The selected Niagara input binding has no owning asset path."), TEXT("/binding/asset_path"),
				MakeShared<FJsonValueString>(Identity.GetOwnerId()), MakeShared<FJsonValueString>(TEXT("non-empty asset path")));
			return false;
		}
		if (!TryParseNiagaraCompoundSelector(
				Identity.GetSelector(),
				OutResolved.Selector,
				OutError))
		{
			if (OutError.TargetPath.IsEmpty()) OutError.TargetPath = Identity.GetOwnerId();
			if (OutError.SubjectId.IsEmpty()) OutError.SubjectId = Binding.GetBindingId();
			return false;
		}
		UObject* Asset =
			LoadObject<UObject>(nullptr, *Identity.GetOwnerId());
		if (!IsValid(Asset))
		{
			OutError = ResolveFact(TEXT("niagara_compound_asset_not_found"),
				TEXT("The selected Niagara input's owning asset could not be loaded."), TEXT("/asset/available"),
				MakeShared<FJsonValueBoolean>(false), MakeShared<FJsonValueBoolean>(true));
			return false;
		}
		TArray<FResolvedStage> Stages;
		UE::UAI::Contract::FDiagnostic NativeError;
		if (!EnumerateStages(*Asset, Stages, NativeError))
		{
			OutError = MoveTemp(NativeError);
			if (OutError.TargetPath.IsEmpty()) OutError.TargetPath = Identity.GetOwnerId();
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
					OutError = ResolveFact(TEXT("niagara_compound_stage_ambiguous"),
						TEXT("More than one native Niagara stage matches the selected stage and emitter."), TEXT("/stage/observed_match_count"),
						MakeShared<FJsonValueNumber>(2), MakeShared<FJsonValueNumber>(1));
					OutError.ActualEvidence = MakeShared<FJsonValueString>(Identity.GetSelector());
					return false;
				}
				MatchedStage = &Stage;
			}
		}
		if (!MatchedStage || !MatchedStage->OutputNode)
		{
			OutError = ResolveFact(TEXT("niagara_compound_stage_not_found"),
				!MatchedStage ? TEXT("No native Niagara stage matches the selected stage and emitter.")
					: TEXT("The selected native Niagara stage has no output node."),
				!MatchedStage ? TEXT("/stage/match_count") : TEXT("/stage/output_node_available"),
				!MatchedStage ? TSharedPtr<FJsonValue>(MakeShared<FJsonValueNumber>(0)) : TSharedPtr<FJsonValue>(MakeShared<FJsonValueBoolean>(false)),
				!MatchedStage ? TSharedPtr<FJsonValue>(MakeShared<FJsonValueNumber>(1)) : TSharedPtr<FJsonValue>(MakeShared<FJsonValueBoolean>(true)));
			OutError.ActualEvidence = MakeShared<FJsonValueString>(Identity.GetSelector());
			return false;
		}
		FGuid ModuleGuid;
		if (!FGuid::Parse(
				OutResolved.Selector.ModuleNodeGuid,
				ModuleGuid))
		{
			OutError = ResolveFact(TEXT("niagara_compound_module_guid_invalid"),
				TEXT("The selected Niagara module identity is not a valid GUID."), TEXT("/module_selector/guid_valid"),
				MakeShared<FJsonValueBoolean>(false), MakeShared<FJsonValueBoolean>(true));
			OutError.ActualEvidence = MakeShared<FJsonValueString>(OutResolved.Selector.ModuleNodeGuid);
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
				NativeError))
		{
			OutError = MoveTemp(NativeError);
			if (OutError.TargetPath.IsEmpty()) OutError.TargetPath = Identity.GetOwnerId();
			if (OutError.CandidateName.IsEmpty()) OutError.CandidateName = OutResolved.Selector.InputName;
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
			OutError = ResolveFact(TEXT("niagara_compound_input_not_found"),
				TEXT("The selected input name is absent from the resolved Niagara module's inputs."), TEXT("/module/input_name"),
				MakeShared<FJsonValueString>(OutResolved.Selector.InputName),
				MakeShared<FJsonValueString>(TEXT("existing module input name")));
			return false;
		}
		OutResolved.Asset = Asset;
		OutResolved.Input = *Input;
		return true;
	}
}
