#include "Composition/GraphCompound/UeAgentGraphCompoundNiagaraTypedMutationProduction.h"

#include "Core/AssetDurable/UeAgentAssetDurable.h"
#include "Core/Contract/UeAgentContractJson.h"
#include "Core/Contract/UeAgentExecutionContextPolicy.h"
#include "Core/StateSemantic/UeAgentCanonicalValueJson.h"
#include "Core/StateSemantic/UeAgentStateSemantic.h"
#include "Core/Verification/UeAgentIndependentReadback.h"
#include "DomainAdapters/GenericEffect/UeAgentGenericEffectAdapter.h"
#include "DomainAdapters/GenericRead/UeAgentGenericReadAdapter.h"
#include "DomainAdapters/MaterialNiagaraGraph/UeAgentMaterialNiagaraGraphNativeHook.h"
#include "DomainAdapters/NiagaraCompound/UeAgentNiagaraCompoundAdapter.h"
#include "DomainAdapters/NiagaraCompound/UeAgentNiagaraCompoundDiscovery.h"
#include "DomainAdapters/NiagaraCompound/UeAgentNiagaraCompoundNativeHook.h"
#include "Domains/Niagara/Authoring/UeAgentNiagaraStageModuleAuthoring.h"
#include "Domains/Niagara/UeAgentNiagaraCompileValidation.h"
#include "Dom/JsonObject.h"
#include "HAL/PlatformTime.h"
#include "Misc/DateTime.h"
#include "Misc/Guid.h"
#include "Misc/PackageName.h"
#include "NiagaraEmitter.h"
#include "NiagaraEmitterHandle.h"
#include "NiagaraNodeFunctionCall.h"
#include "NiagaraRendererProperties.h"
#include "NiagaraSystem.h"
#include "PackageTools.h"
#include "UObject/Package.h"
#include "UObject/UObjectGlobals.h"
#include "UObject/UnrealType.h"
#include "ViewModels/NiagaraEmitterHandleViewModel.h"
#include "ViewModels/NiagaraSystemViewModel.h"
#include "ViewModels/Stack/NiagaraStackEntry.h"
#include "ViewModels/Stack/NiagaraStackModuleItem.h"
#include "ViewModels/Stack/NiagaraStackViewModel.h"

namespace UE::UAI::Composition::GraphCompound
{
	using namespace AssetDurable;
	using namespace ContextResource;
	using namespace Contract;
	using namespace Discovery;
	using namespace DomainAdapters::GenericEffect;
	using namespace DomainAdapters::GenericRead;
	using namespace DomainAdapters::MaterialNiagaraGraph;
	using namespace DomainAdapters::NiagaraCompound;
	using namespace ExecutionMutation;
	using namespace PlanningPreflight;
	using namespace StateSemantic;
	using namespace UeAgentNiagaraStageModuleAuthoring;
	using namespace Verification;

	namespace
	{
		const FString NiagaraTypedReadbackOracleId =
			TEXT("uai.readback.graph_compound.niagara_typed_mutation");
		const FString NiagaraCompoundGeneration =
			TEXT("graph-compound-niagara-typed-v1");

		void SetError(
			FGraphCompoundRuntimeError& OutError,
			FString Code,
			FString SubjectId = FString(),
			FString Detail = FString())
		{
			OutError = {};
			OutError.Code = MoveTemp(Code);
			OutError.SubjectId = MoveTemp(SubjectId);
			OutError.Detail = MoveTemp(Detail);
		}

		FString PackageNameFromRef(const FString& Ref)
		{
			return Ref.Contains(TEXT("."))
				? FPackageName::ObjectPathToPackageName(Ref)
				: Ref;
		}

		FString ObjectPathFromRef(const FString& Ref)
		{
			const FString PackageName = PackageNameFromRef(Ref);
			return PackageName + TEXT(".")
				+ FPackageName::GetLongPackageAssetName(PackageName);
		}

		UNiagaraSystem* TryResolveSystem(
			const FString& TargetRef,
			FGraphCompoundRuntimeError& OutError)
		{
			const FString PackageName = PackageNameFromRef(TargetRef);
			UNiagaraSystem* const System =
				FPackageName::IsValidLongPackageName(PackageName)
					? LoadObject<UNiagaraSystem>(
						nullptr, *ObjectPathFromRef(PackageName))
					: nullptr;
			if (!System
				|| System->GetOutermost()->GetName() != PackageName)
			{
				SetError(OutError,
					TEXT("graph_compound_niagara_system_invalid"),
					TargetRef);
				return nullptr;
			}
			return System;
		}

		bool TryUnloadPackage(
			const FString& TargetRef,
			FString& OutDetail)
		{
			OutDetail.Reset();
			const FString PackageName = PackageNameFromRef(TargetRef);
			UPackage* const Package = FindPackage(nullptr, *PackageName);
			if (!Package)
			{
				return true;
			}
			Package->SetDirtyFlag(false);
			FText Error;
			if (!UPackageTools::UnloadPackages({Package}, Error, true))
			{
				OutDetail = Error.ToString();
				return false;
			}
			CollectGarbage(RF_NoFlags, true);
			if (FindPackage(nullptr, *PackageName))
			{
				OutDetail = TEXT("package remains loaded after unload");
				return false;
			}
			return true;
		}

		FUeAgentCanonicalIdentity Identity(
			const FGraphCompoundDomainRouteProfile& Profile,
			const FGraphCompoundNiagaraMutationSelector& Selector,
			const FString& Generation)
		{
			const FString SemanticId = Selector.GetStableSemanticId();
			return FUeAgentCanonicalIdentity(
				SemanticId, Profile.GetTargetKind(), Selector.GetTargetRef(),
				SemanticId, TEXT("graph_compound"), Generation);
		}

		bool TryCanonicalText(
			const FUeAgentCanonicalValueRef& Value,
			FString& OutText,
			FGraphCompoundRuntimeError& OutError,
			const FString& Subject)
		{
			TSharedPtr<FJsonValue> JsonValue;
			FString Error;
			if (!CanonicalJson::TryToJsonValue(*Value, JsonValue, Error)
				|| !Json::TryCanonicalize(JsonValue, OutText, Error))
			{
				SetError(OutError,
					TEXT("graph_compound_niagara_value_canonicalize_failed"),
					Subject, Error);
				return false;
			}
			return true;
		}

		bool TryStringValue(
			const FString& Text,
			FUeAgentResourceTracker& Resources,
			FUeAgentCanonicalValuePtr& OutValue,
			FGraphCompoundRuntimeError& OutError,
			const FString& Subject)
		{
			FUeAgentStateSemanticError Error;
			if (!FUeAgentCanonicalValue::TryString(
					Text, Resources, OutValue, Error)
				|| !OutValue.IsValid())
			{
				SetError(OutError, Error.Code, Subject, Error.Path);
				return false;
			}
			return true;
		}

		bool TryBuildSnapshot(
			const FGraphCompoundDomainRouteProfile& Profile,
			const FGraphCompoundNiagaraMutationSelector& Selector,
			const FUeAgentCanonicalValueRef& Current,
			FUeAgentResourceTracker& Resources,
			FUeAgentCanonicalSnapshotPtr& OutSnapshot,
			FGraphCompoundRuntimeError& OutError)
		{
			FString Canonical;
			if (!TryCanonicalText(
					Current, Canonical, OutError,
					Selector.GetStableSemanticId()))
			{
				return false;
			}
			const FString Generation = Json::Sha256Utf8(
				Selector.GetTargetRef() + TEXT("\n")
				+ Selector.GetStableSemanticId() + TEXT("\n") + Canonical);
			const FUeAgentStateRevision Revision(
				TEXT("graph-compound:c4:niagara-typed"), Generation, 0);
			const FUeAgentCanonicalIdentity EntryIdentity =
				Identity(Profile, Selector, Generation);
			FUeAgentStateEntry Entry(
				EntryIdentity,
				FUeAgentCanonicalType(
					Current->GetKind() == EUeAgentValueKind::Object
						? TEXT("uai.niagara.input.v1")
						: TEXT("uai.graph_compound.state.v1"),
					Current->GetKind(), false,
					Current->GetKind() == EUeAgentValueKind::Object
						? TEXT("object") : TEXT("scalar")),
				Current,
				FUeAgentDefaultValue(
					EUeAgentDefaultSourceKind::DomainNative,
					Selector.GetTargetRef(),
					TEXT("independent exact Niagara selector readback"),
					Current),
				FUeAgentStateDetails(
					Selector.GetStableSemanticId(),
					TEXT("GraphCompound exact Niagara typed state"),
					{TEXT("GraphCompound"), TEXT("Niagara"), TEXT("Typed")},
					true, TEXT("available"), {}),
				Revision,
				EUeAgentAuthoredPresence::Present,
				EUeAgentOverrideState::Overridden);
			if (!Entry.IsValid())
			{
				SetError(OutError,
					TEXT("graph_compound_niagara_state_entry_invalid"),
					Selector.GetStableSemanticId());
				return false;
			}
			FUeAgentStateSemanticError Error;
			if (!FUeAgentCanonicalSnapshot::TryCreate(
					TEXT("graph-compound:c4:niagara-typed:")
						+ Json::Sha256Utf8(Selector.GetStableSemanticId()),
					EUeAgentStatePlane::Persisted,
					Revision,
					FUeAgentTemporalPoint(
						TEXT("utc-seconds"),
						FString::Printf(TEXT("%lld"),
							FDateTime::UtcNow().ToUnixTimestamp())),
					FUeAgentStateProvenance(
						TEXT("independent_readback"),
						Selector.GetTargetRef(),
						TEXT("niagara-typed:") + Generation),
					{MoveTemp(Entry)}, Resources, OutSnapshot, Error)
				|| !OutSnapshot.IsValid())
			{
				SetError(OutError, Error.Code,
					Selector.GetTargetRef(), Error.Path);
				return false;
			}
			return true;
		}

		bool TryParseModuleGuid(
			const FString& ModuleRef,
			FGuid& OutGuid)
		{
			return ModuleRef.StartsWith(TEXT("module:"))
				&& FGuid::Parse(ModuleRef.RightChop(7), OutGuid)
				&& OutGuid.IsValid();
		}

		bool TryParseEmitterGuid(
			const FGraphCompoundNiagaraMutationSelector& Selector,
			FGuid& OutGuid)
		{
			return FGuid::Parse(Selector.GetEmitterHandleId(), OutGuid)
				&& OutGuid.IsValid();
		}

		bool TryResolveStage(
			UNiagaraSystem& System,
			const FGraphCompoundNiagaraMutationSelector& Selector,
			FResolvedStage& OutStage,
			FGraphCompoundRuntimeError& OutError)
		{
			FGuid EmitterHandleId;
			if (!TryParseEmitterGuid(Selector, EmitterHandleId))
			{
				SetError(OutError,
					TEXT("graph_compound_niagara_emitter_handle_invalid"),
					Selector.GetEmitterHandleId());
				return false;
			}
			TArray<FResolvedStage> Stages;
			FString Error;
			if (!EnumerateStages(System, Stages, Error))
			{
				SetError(OutError,
					TEXT("graph_compound_niagara_stage_enumeration_failed"),
					Selector.GetTargetRef(), Error);
				return false;
			}
			const FResolvedStage* Match = nullptr;
			int32 MatchCount = 0;
			for (const FResolvedStage& Stage : Stages)
			{
				if (Stage.EmitterHandleId == EmitterHandleId
					&& Stage.StageKey == Selector.GetStageRef())
				{
					Match = &Stage;
					++MatchCount;
				}
			}
			if (MatchCount != 1 || !Match || !Match->OutputNode)
			{
				SetError(OutError,
					TEXT("graph_compound_niagara_stage_not_unique"),
					Selector.GetStageRef(),
					FString::FromInt(MatchCount));
				return false;
			}
			OutStage = *Match;
			return true;
		}

		bool TryCaptureReorder(
			UNiagaraSystem& System,
			const FGraphCompoundNiagaraMutationSelector& Selector,
			FMaterialNiagaraGraphNativeHook& NativeHook,
			FUeAgentResourceTracker& Resources,
			FUeAgentCanonicalValuePtr& OutValue,
			FGraphCompoundRuntimeError& OutError)
		{
			FResolvedStage Stage;
			if (!TryResolveStage(System, Selector, Stage, OutError))
			{
				return false;
			}
			FMaterialNiagaraGraphTopology Topology;
			FString Revision;
			FString Evidence;
			FMaterialNiagaraGraphAdapterError Error;
			if (!NativeHook.TryCaptureNiagaraStack(
					System, Stage.EmitterHandleId, Stage.Usage, Stage.UsageId,
					Topology, Revision, Evidence, Error))
			{
				SetError(OutError, Error.GetCode(),
					Selector.GetItemRef(), Error.GetDetail());
				return false;
			}
			const FMaterialNiagaraGraphOrderingIdentity* Match = nullptr;
			int32 MatchCount = 0;
			for (const FMaterialNiagaraGraphOrderingIdentity& Ordering
				: Topology.GetOrdering())
			{
				if (Ordering.GetItemKey() == Selector.GetItemRef())
				{
					Match = &Ordering;
					++MatchCount;
				}
			}
			if (MatchCount != 1 || !Match)
			{
				SetError(OutError,
					TEXT("graph_compound_niagara_order_item_not_unique"),
					Selector.GetItemRef(), FString::FromInt(MatchCount));
				return false;
			}
			return TryStringValue(
				Match->GetPredecessorItemKey(), Resources, OutValue,
				OutError, Selector.GetItemRef());
		}

		bool TryIssueState(
			UNiagaraSystem& System,
			const FGraphCompoundNiagaraMutationSelector& Selector,
			const bool bApply,
			FString& OutState,
			FGraphCompoundRuntimeError& OutError)
		{
			OutState.Reset();
			FGuid EmitterHandleId;
			if (!TryParseEmitterGuid(Selector, EmitterHandleId))
			{
				SetError(OutError,
					TEXT("graph_compound_niagara_emitter_handle_invalid"),
					Selector.GetEmitterHandleId());
				return false;
			}
			TSharedPtr<FNiagaraSystemViewModel> ViewModel =
				MakeShared<FNiagaraSystemViewModel>();
			FNiagaraSystemViewModelOptions Options;
			Options.bCanModifyEmittersFromTimeline = false;
			Options.bCanAutoCompile = false;
			Options.bCanSimulate = false;
			Options.bIsForDataProcessingOnly = false;
			Options.bCompileForEdit = false;
			Options.EditMode = ENiagaraSystemViewModelEditMode::SystemAsset;
			Options.MessageLogGuid = System.GetAssetGuid();
			ViewModel->Initialize(System, Options);

			UNiagaraStackViewModel* StackViewModel = nullptr;
			int32 EmitterCount = 0;
			for (const TSharedRef<FNiagaraEmitterHandleViewModel>& Handle
				: ViewModel->GetEmitterHandleViewModels())
			{
				if (Handle->GetId() == EmitterHandleId)
				{
					StackViewModel = Handle->GetEmitterStackViewModel();
					++EmitterCount;
				}
			}
			UNiagaraStackEntry* Root = StackViewModel
				? StackViewModel->GetRootEntry() : nullptr;
			if (EmitterCount != 1 || !Root)
			{
				ViewModel.Reset();
				SetError(OutError,
					TEXT("graph_compound_niagara_issue_emitter_not_unique"),
					Selector.GetEmitterHandleId(),
					FString::FromInt(EmitterCount));
				return false;
			}

			Root->RefreshChildren();
			Root->RefreshFilteredChildren();
			TArray<UNiagaraStackModuleItem*> ModuleItems;
			Root->GetUnfilteredChildrenOfType(ModuleItems, true);
			int32 IssueCount = 0;
			int32 FixCount = 0;
			UNiagaraStackEntry::FStackIssueFixDelegate FixDelegate;
			for (UNiagaraStackModuleItem* ModuleItem : ModuleItems)
			{
				if (!ModuleItem)
				{
					continue;
				}
				ModuleItem->RefreshChildren();
				ModuleItem->RefreshFilteredChildren();
				for (const UNiagaraStackEntry::FStackIssue& Issue
					: ModuleItem->GetIssues())
				{
					if (Issue.GetUniqueIdentifier() != Selector.GetIssueId())
					{
						continue;
					}
					++IssueCount;
					for (const UNiagaraStackEntry::FStackIssueFix& Fix
						: Issue.GetFixes())
					{
						if (Fix.GetUniqueIdentifier() == Selector.GetFixId()
							&& Fix.IsValid()
							&& Fix.GetFixDelegate().IsBound())
						{
							++FixCount;
							FixDelegate = Fix.GetFixDelegate();
						}
					}
				}
			}
			if (IssueCount == 0)
			{
				ViewModel.Reset();
				OutState = TEXT("resolved");
				return true;
			}
			if (IssueCount != 1 || FixCount != 1 || !FixDelegate.IsBound())
			{
				ViewModel.Reset();
				SetError(OutError,
					TEXT("graph_compound_niagara_issue_fix_not_unique"),
					Selector.GetIssueId(),
					FString::Printf(TEXT("issues=%d,fixes=%d"),
						IssueCount, FixCount));
				return false;
			}
			OutState = TEXT("unresolved");
			if (bApply)
			{
				FixDelegate.Execute();
			}
			ViewModel.Reset();
			return true;
		}

		bool TryFindCompoundBinding(
			const FGraphCompoundDomainRouteProfile& Profile,
			const FGraphCompoundNiagaraMutationSelector& Selector,
			FUeAgentResourceTracker& Resources,
			const FUeAgentOperationDeadline& Deadline,
			const FUeAgentCancellationToken& Cancellation,
			TSharedPtr<const FUeAgentCandidateBinding>& OutBinding,
			FGraphCompoundRuntimeError& OutError)
		{
			OutBinding.Reset();
			TArray<FUeAgentCandidateBinding> Bindings;
			FUeAgentDiscoveryError Error;
			if (!TryDiscoverNiagaraCompoundBindings(
					ObjectPathFromRef(Selector.GetTargetRef()),
					Profile.GetExecutionContract().GetDescriptorIdentity(),
					NiagaraCompoundGeneration, Resources, Deadline,
					Cancellation, Bindings, Error))
			{
				SetError(OutError, Error.Code,
					Error.SubjectId, Error.Detail);
				return false;
			}
			const FString ModuleGuid = Selector.GetModuleRef().RightChop(7);
			int32 MatchCount = 0;
			int32 EmitterMatchCount = 0;
			int32 StageMatchCount = 0;
			int32 ModuleMatchCount = 0;
			int32 InputMatchCount = 0;
			TArray<FString> CandidateSamples;
			for (const FUeAgentCandidateBinding& Binding : Bindings)
			{
				FUeAgentNiagaraCompoundSelector Candidate;
				FString ParseError;
				if (TryParseNiagaraCompoundSelector(
						Binding.GetSummary().GetIdentity().GetSelector(),
						Candidate, ParseError))
				{
					const bool bEmitter = Candidate.EmitterHandleId
						== Selector.GetEmitterHandleId();
					const bool bStage = Candidate.StageKey
						== Selector.GetStageRef();
					const bool bModule = Candidate.ModuleNodeGuid == ModuleGuid;
					const bool bInput = Candidate.InputName
						== Selector.GetInputName();
					if (CandidateSamples.Num() < 4)
					{
						CandidateSamples.Add(
							Candidate.EmitterHandleId + TEXT("|")
							+ Candidate.StageKey + TEXT("|")
							+ Candidate.ModuleNodeGuid + TEXT("|")
							+ Candidate.InputName);
					}
					EmitterMatchCount += bEmitter ? 1 : 0;
					StageMatchCount += bStage ? 1 : 0;
					ModuleMatchCount += bModule ? 1 : 0;
					InputMatchCount += bInput ? 1 : 0;
					if (bEmitter && bStage && bModule && bInput)
					{
						OutBinding = MakeShared<const FUeAgentCandidateBinding>(
							Binding);
						++MatchCount;
					}
				}
			}
			if (MatchCount != 1 || !OutBinding.IsValid())
			{
				SetError(OutError,
					TEXT("graph_compound_niagara_compound_binding_not_unique"),
					Selector.GetStableSemanticId(),
					FString::Printf(
						TEXT("matches=%d,candidates=%d,emitter=%d,stage=%d,"
							"module=%d,input=%d,expected=%s|%s|%s|%s,samples=%s"),
						MatchCount, Bindings.Num(), EmitterMatchCount,
						StageMatchCount, ModuleMatchCount, InputMatchCount,
						*Selector.GetEmitterHandleId(), *Selector.GetStageRef(),
						*ModuleGuid, *Selector.GetInputName(),
						*FString::Join(CandidateSamples, TEXT(";"))));
				return false;
			}
			return true;
		}

		bool TryCaptureCompound(
			const FGraphCompoundDomainRouteProfile& Profile,
			const FGraphCompoundNiagaraMutationSelector& Selector,
			FUeAgentResourceTracker& Resources,
			FUeAgentCanonicalValuePtr& OutValue,
			FGraphCompoundRuntimeError& OutError)
		{
			const auto Clock = []() { return FPlatformTime::Seconds(); };
			FUeAgentCancellationSource Cancellation(Clock);
			const FUeAgentOperationDeadline Deadline =
				FUeAgentOperationDeadline::FromTimeout(60.0, Clock);
			TSharedPtr<const FUeAgentCandidateBinding> Binding;
			if (!TryFindCompoundBinding(
					Profile, Selector, Resources, Deadline,
					Cancellation.GetToken(), Binding, OutError))
			{
				return false;
			}
			const TSharedRef<FUeAgentNiagaraCompoundNativeHook> Native =
				MakeShared<FUeAgentNiagaraCompoundNativeHook>(
					NiagaraCompoundGeneration);
			const FUeAgentStateReadRequest Request(
				*Binding, EUeAgentStatePlane::Persisted);
			TSharedPtr<const FUeAgentUeReadCapture> Capture;
			FUeAgentGenericReadError Error;
			if (!Native->TryCapture(
					Request, Resources, Deadline, Cancellation.GetToken(),
					Capture, Error)
				|| !Capture.IsValid() || Capture->GetEntries().Num() != 1)
			{
				SetError(OutError, Error.Code.IsEmpty()
					? TEXT("graph_compound_niagara_compound_capture_invalid")
					: Error.Code,
					Error.SubjectId, Error.Detail);
				return false;
			}
			OutValue = Capture->GetEntries()[0].GetValue();
			return OutValue.IsValid();
		}

		bool TryResolveRendererBinding(
			UNiagaraSystem& System,
			const FGraphCompoundNiagaraMutationSelector& Selector,
			UNiagaraRendererProperties*& OutRenderer,
			FStructProperty*& OutProperty,
			FNiagaraVariableAttributeBinding*& OutBinding,
			FVersionedNiagaraEmitter& OutEmitter,
			FGraphCompoundRuntimeError& OutError)
		{
			OutRenderer = nullptr;
			OutProperty = nullptr;
			OutBinding = nullptr;
			FGuid EmitterHandleId;
			if (!TryParseEmitterGuid(Selector, EmitterHandleId))
			{
				SetError(OutError,
					TEXT("graph_compound_niagara_emitter_handle_invalid"),
					Selector.GetEmitterHandleId());
				return false;
			}
			FNiagaraEmitterHandle* Handle =
				System.GetEmitterHandles().FindByPredicate(
					[EmitterHandleId](const FNiagaraEmitterHandle& Candidate)
					{
						return Candidate.GetId() == EmitterHandleId;
					});
			FVersionedNiagaraEmitterData* Data = Handle
				? Handle->GetInstance().GetEmitterData() : nullptr;
			if (!Handle || !Data)
			{
				SetError(OutError,
					TEXT("graph_compound_niagara_renderer_emitter_missing"),
					Selector.GetEmitterHandleId());
				return false;
			}
			const FString RendererName = Selector.GetRendererRef().RightChop(9);
			int32 RendererCount = 0;
			for (UNiagaraRendererProperties* Renderer : Data->GetRenderers())
			{
				if (Renderer && Renderer->GetName() == RendererName)
				{
					OutRenderer = Renderer;
					++RendererCount;
				}
			}
			if (RendererCount != 1 || !OutRenderer)
			{
				SetError(OutError,
					TEXT("graph_compound_niagara_renderer_not_unique"),
					Selector.GetRendererRef(), FString::FromInt(RendererCount));
				return false;
			}
			int32 PropertyCount = 0;
			for (TFieldIterator<FStructProperty> It(OutRenderer->GetClass());
				It; ++It)
			{
				FStructProperty* Property = *It;
				if (!Property || Property->Struct
					!= FNiagaraVariableAttributeBinding::StaticStruct())
				{
					continue;
				}
				FString Slot = Property->GetName();
				if (Slot.EndsWith(TEXT("Binding")))
				{
					Slot.LeftChopInline(7);
				}
				Slot.ToLowerInline();
				if (Slot == Selector.GetParameterName())
				{
					OutProperty = Property;
					OutBinding = Property
						->ContainerPtrToValuePtr<
							FNiagaraVariableAttributeBinding>(OutRenderer);
					++PropertyCount;
				}
			}
			if (PropertyCount != 1 || !OutProperty || !OutBinding)
			{
				SetError(OutError,
					TEXT("graph_compound_niagara_renderer_binding_not_unique"),
					Selector.GetParameterName(),
					FString::FromInt(PropertyCount));
				return false;
			}
			FString NativeType = OutBinding->GetParamMapBindableVariable()
				.GetType().GetName();
			NativeType.ToLowerInline();
			if (NativeType.StartsWith(TEXT("niagara")))
			{
				NativeType.RightChopInline(7);
			}
			if (NativeType != Selector.GetTypeRef())
			{
				SetError(OutError,
					TEXT("graph_compound_niagara_renderer_type_mismatch"),
					Selector.GetTypeRef(), NativeType);
				return false;
			}
			OutEmitter = Handle->GetInstance();
			return true;
		}

		bool TryCaptureRenderer(
			UNiagaraSystem& System,
			const FGraphCompoundNiagaraMutationSelector& Selector,
			FUeAgentResourceTracker& Resources,
			FUeAgentCanonicalValuePtr& OutValue,
			FGraphCompoundRuntimeError& OutError)
		{
			UNiagaraRendererProperties* Renderer = nullptr;
			FStructProperty* Property = nullptr;
			FNiagaraVariableAttributeBinding* Binding = nullptr;
			FVersionedNiagaraEmitter Emitter;
			if (!TryResolveRendererBinding(
					System, Selector, Renderer, Property, Binding,
					Emitter, OutError))
			{
				return false;
			}
			return TryStringValue(
				Binding->GetParamMapBindableVariable().GetName().ToString(),
				Resources, OutValue, OutError,
				Selector.GetStableSemanticId());
		}

		bool TryCaptureCurrentValue(
			const FGraphCompoundDomainRouteProfile& Profile,
			const FGraphCompoundNiagaraMutationSelector& Selector,
			FMaterialNiagaraGraphNativeHook& NativeHook,
			FUeAgentResourceTracker& Resources,
			FUeAgentCanonicalValuePtr& OutValue,
			FGraphCompoundRuntimeError& OutError)
		{
			OutValue.Reset();
			UNiagaraSystem* const System =
				TryResolveSystem(Selector.GetTargetRef(), OutError);
			if (!System)
			{
				return false;
			}
			switch (Selector.GetInputKind())
			{
			case EGraphCompoundTypedInputKind::NiagaraStackReorder:
				return TryCaptureReorder(
					*System, Selector, NativeHook, Resources,
					OutValue, OutError);
			case EGraphCompoundTypedInputKind::NiagaraStackIssueFix:
			{
				FString State;
				return TryIssueState(
						*System, Selector, false, State, OutError)
					&& TryStringValue(
						State, Resources, OutValue, OutError,
						Selector.GetIssueId());
			}
			case EGraphCompoundTypedInputKind::NiagaraCompoundInput:
				return TryCaptureCompound(
					Profile, Selector, Resources, OutValue, OutError);
			case EGraphCompoundTypedInputKind::NiagaraRendererEventParameter:
				return TryCaptureRenderer(
					*System, Selector, Resources, OutValue, OutError);
			default:
				SetError(OutError,
					TEXT("graph_compound_niagara_selector_kind_invalid"),
					Profile.GetCapabilityId());
				return false;
			}
		}

		bool TryApplyReorder(
			UNiagaraSystem& System,
			const FGraphCompoundNiagaraMutationSelector& Selector,
			FMaterialNiagaraGraphNativeHook& NativeHook,
			FGraphCompoundRuntimeError& OutError)
		{
			FResolvedStage Stage;
			if (!TryResolveStage(System, Selector, Stage, OutError))
			{
				return false;
			}
			FGuid ModuleGuid;
			if (!TryParseModuleGuid(Selector.GetItemRef(), ModuleGuid))
			{
				SetError(OutError,
					TEXT("graph_compound_niagara_module_ref_invalid"),
					Selector.GetItemRef());
				return false;
			}
			TArray<UNiagaraNodeFunctionCall*> Modules;
			GetModules(*Stage.OutputNode, Modules);
			UNiagaraNodeFunctionCall* Module = nullptr;
			int32 SourceIndex = INDEX_NONE;
			for (int32 Index = 0; Index < Modules.Num(); ++Index)
			{
				if (Modules[Index] && Modules[Index]->NodeGuid == ModuleGuid)
				{
					if (Module)
					{
						SetError(OutError,
							TEXT("graph_compound_niagara_module_not_unique"),
							Selector.GetItemRef());
						return false;
					}
					Module = Modules[Index];
					SourceIndex = Index;
				}
			}
			if (!Module)
			{
				SetError(OutError,
					TEXT("graph_compound_niagara_module_missing"),
					Selector.GetItemRef());
				return false;
			}
			int32 TargetIndex = 0;
			if (!Selector.GetPredecessorRef().IsEmpty())
			{
				FGuid PredecessorGuid;
				if (!TryParseModuleGuid(
						Selector.GetPredecessorRef(), PredecessorGuid)
					|| PredecessorGuid == ModuleGuid)
				{
					SetError(OutError,
						TEXT("graph_compound_niagara_predecessor_invalid"),
						Selector.GetPredecessorRef());
					return false;
				}
				const int32 PredecessorIndex = Modules.IndexOfByPredicate(
					[PredecessorGuid](const UNiagaraNodeFunctionCall* Candidate)
					{
						return Candidate && Candidate->NodeGuid == PredecessorGuid;
					});
				if (PredecessorIndex == INDEX_NONE)
				{
					SetError(OutError,
						TEXT("graph_compound_niagara_predecessor_missing"),
						Selector.GetPredecessorRef());
					return false;
				}
				TargetIndex = SourceIndex < PredecessorIndex
					? PredecessorIndex : PredecessorIndex + 1;
			}
			int32 NewIndex = INDEX_NONE;
			FMaterialNiagaraGraphNativeMutationReceipt Receipt;
			FMaterialNiagaraGraphAdapterError Error;
			if (!NativeHook.TryMoveNiagaraModule(
					System, Stage.EmitterHandleId, Stage.Usage, Stage.UsageId,
					*Module, TargetIndex, NewIndex, Receipt, Error))
			{
				SetError(OutError, Error.GetCode(),
					Selector.GetItemRef(), Error.GetDetail());
				return false;
			}
			return true;
		}

		bool TryCreateExecutionContext(
			const FUeAgentDescriptorIdentity& DescriptorIdentity,
			FUeAgentExecutionContextRefPtr& OutContext,
			FGraphCompoundRuntimeError& OutError)
		{
			FUeAgentExecutionContextInput Input;
			Input.ProjectInstanceId = TEXT("graph-compound-project");
			Input.EditorInstanceId = TEXT("graph-compound-editor");
			Input.StatePlane = EUeAgentStatePlane::Persisted;
			Input.ContextRevision = TEXT("graph-compound-niagara-typed-v1");
			FUeAgentExecutionContextError Error;
			if (!FUeAgentExecutionContextRef::TryCreate(
					Input, DescriptorIdentity, EUeAgentRequestKind::Operation,
					FUeAgentExecutionContextPolicy(
						{EUeAgentStatePlane::Persisted}, {},
						EUeAgentContextResolutionPolicy::ExplicitOnly,
						EUeAgentTransientContextPolicy::ReadOnly),
					OutContext, Error)
				|| !OutContext.IsValid())
			{
				SetError(OutError, Error.Code,
					DescriptorIdentity.GetDescriptorId());
				return false;
			}
			return true;
		}

		struct FCompoundApplyContext final
		{
			TSharedPtr<FUeAgentNiagaraCompoundNativeHook> Native;
			TSharedPtr<FUeAgentNiagaraCompoundEffectHook> Effect;
			TOptional<FExecutionMutationRequest> Request;
			FUeAgentGenericEffectApplyResult Result;
			FString OperationId;
			bool bApplied = false;
		};

		bool TryApplyCompound(
			const FGraphCompoundDomainRouteProfile& Profile,
			const FGraphCompoundNiagaraMutationSelector& Selector,
			const FPlanningPreflightPlanRef& OuterPlan,
			const FUeAgentCanonicalValueRef& After,
			const int64 FencingToken,
			TSharedRef<FUeAgentResourceTracker, ESPMode::ThreadSafe> Resources,
			FCompoundApplyContext& OutContext,
			FGraphCompoundRuntimeError& OutError)
		{
			const auto Clock = []() { return FPlatformTime::Seconds(); };
			FUeAgentCancellationSource Cancellation(Clock);
			const FUeAgentOperationDeadline Deadline =
				FUeAgentOperationDeadline::FromTimeout(90.0, Clock);
			TSharedPtr<const FUeAgentCandidateBinding> Binding;
			if (!TryFindCompoundBinding(
					Profile, Selector, *Resources, Deadline,
					Cancellation.GetToken(), Binding, OutError))
			{
				return false;
			}
			OutContext.Native =
				MakeShared<FUeAgentNiagaraCompoundNativeHook>(
					NiagaraCompoundGeneration);
			const FUeAgentStateReadRequest ReadRequest(
				*Binding, EUeAgentStatePlane::Persisted);
			TSharedPtr<const FUeAgentUeReadCapture> BeforeCapture;
			FUeAgentGenericReadError ReadError;
			if (!OutContext.Native->TryCapture(
					ReadRequest, *Resources, Deadline,
					Cancellation.GetToken(), BeforeCapture, ReadError)
				|| !BeforeCapture.IsValid()
				|| BeforeCapture->GetEntries().Num() != 1)
			{
				SetError(OutError, ReadError.Code.IsEmpty()
					? TEXT("graph_compound_niagara_compound_capture_invalid")
					: ReadError.Code,
					ReadError.SubjectId, ReadError.Detail);
				return false;
			}
			const FUeAgentCanonicalIdentity& CandidateIdentity =
				Binding->GetSummary().GetIdentity();
			TArray<FPlannedChange> Changes;
			Changes.Emplace(
				CandidateIdentity, EChangeIntent::Update,
				BeforeCapture->GetEntries()[0].GetValue(), After,
				OuterPlan->GetAuthorityScopes(),
				TEXT("graph-compound:niagara-compound-direct"));
			TArray<FImpact> Impacts;
			Impacts.Emplace(
				CandidateIdentity, CandidateIdentity, TEXT("direct"),
				TEXT("graph-compound:niagara-compound-direct"));
			const FPlanningPreflightPlanRef InnerPlan =
				MakeShared<FPlanningPreflightPlan>(
					Profile.GetExecutionContract().GetDescriptorIdentity(),
					OuterPlan->GetRequestId() + TEXT(":compound"),
					OuterPlan->GetIdempotencyKey() + TEXT(":compound"),
					OuterPlan->GetCallerId(),
					OuterPlan->GetAuthorityScopes(),
					BeforeCapture->GetSnapshotId(),
					BeforeCapture->GetRevision(),
					BeforeCapture->GetEvidenceId(),
					FChangeSet(MoveTemp(Changes)),
					FImpactSet(MoveTemp(Impacts)),
					TArray<FPrecondition>(),
					Json::Sha256Utf8(
						OuterPlan->GetFingerprint() + TEXT(":")
						+ CandidateIdentity.GetCanonicalKey()));
			FUeAgentExecutionContextRefPtr ExecutionContext;
			if (!TryCreateExecutionContext(
					InnerPlan->GetDescriptorIdentity(),
					ExecutionContext, OutError))
			{
				return false;
			}
			OutContext.OperationId =
				TEXT("graph-compound:") + OuterPlan->GetRequestId();
			OutContext.Request.Emplace(
				OutContext.OperationId,
				TEXT("graph-compound-niagara-typed"),
				InnerPlan, ExecutionContext.ToSharedRef(), Resources,
				Deadline, Cancellation.GetToken(),
				EExecutionThreadAffinity::GameThread,
				EExecutionReentrancyPolicy::RejectSameTarget);
			OutContext.Effect = MakeShared<FUeAgentNiagaraCompoundEffectHook>(
				OutContext.Native.ToSharedRef());
			const FUeAgentGenericEffectBinding EffectBinding(
				*Binding, TEXT("UE.NiagaraInput:compound"),
				FUeAgentCanonicalType(
					TEXT("uai.niagara.input.v1"),
					EUeAgentValueKind::Object, false, TEXT("object")),
				TEXT("graph-compound-niagara-native-input"));
			FExecutionMutationError Error;
			if (!OutContext.Effect->TryApply(
					OutContext.Request.GetValue(),
					FLockFenceLease(
						TEXT("graph-compound-niagara-typed"),
						{CandidateIdentity.GetCanonicalKey()}, FencingToken),
					{EffectBinding}, *Resources, OutContext.Result, Error)
				|| !OutContext.Result.IsValid())
			{
				SetError(OutError, Error.GetCode().IsEmpty()
					? TEXT("graph_compound_niagara_compound_apply_failed")
					: Error.GetCode(),
					Selector.GetStableSemanticId(), Error.GetDetail());
				return false;
			}
			OutContext.bApplied = true;
			return true;
		}

		void TryRollbackCompound(
			FCompoundApplyContext& Context,
			FUeAgentResourceTracker& Resources)
		{
			if (!Context.bApplied || !Context.Effect.IsValid()
				|| !Context.Request.IsSet())
			{
				return;
			}
			bool bConfirmed = false;
			FExecutionMutationError Error;
			Context.Effect->TryRollback(
				Context.Request.GetValue(), Context.Result, Resources,
				bConfirmed, Error);
			Context.bApplied = false;
		}

		bool TryApplyRenderer(
			UNiagaraSystem& System,
			const FGraphCompoundNiagaraMutationSelector& Selector,
			const FUeAgentCanonicalValueRef& After,
			FGraphCompoundRuntimeError& OutError)
		{
			if (After->GetKind() != EUeAgentValueKind::String
				|| After->GetText().IsEmpty())
			{
				SetError(OutError,
					TEXT("graph_compound_niagara_renderer_value_invalid"),
					Selector.GetStableSemanticId());
				return false;
			}
			UNiagaraRendererProperties* Renderer = nullptr;
			FStructProperty* Property = nullptr;
			FNiagaraVariableAttributeBinding* Binding = nullptr;
			FVersionedNiagaraEmitter Emitter;
			if (!TryResolveRendererBinding(
					System, Selector, Renderer, Property, Binding,
					Emitter, OutError))
			{
				return false;
			}
			Renderer->PreEditChange(Property);
			Binding->SetValue(
				FName(*After->GetText()), Emitter,
				Renderer->GetCurrentSourceMode());
			FPropertyChangedEvent Event(Property, EPropertyChangeType::ValueSet);
			Renderer->PostEditChangeProperty(Event);
			return true;
		}

		bool TryApplyMutation(
			const FGraphCompoundDomainRouteProfile& Profile,
			UNiagaraSystem& System,
			const FGraphCompoundNiagaraMutationSelector& Selector,
			const FPlanningPreflightPlanRef& Plan,
			const FUeAgentCanonicalValueRef& After,
			const int64 FencingToken,
			FMaterialNiagaraGraphNativeHook& NativeHook,
			TSharedRef<FUeAgentResourceTracker, ESPMode::ThreadSafe> Resources,
			FCompoundApplyContext& OutCompoundContext,
			FGraphCompoundRuntimeError& OutError)
		{
			switch (Selector.GetInputKind())
			{
			case EGraphCompoundTypedInputKind::NiagaraStackReorder:
				return After->GetKind() == EUeAgentValueKind::String
					&& After->GetText() == Selector.GetPredecessorRef()
					&& TryApplyReorder(System, Selector, NativeHook, OutError);
			case EGraphCompoundTypedInputKind::NiagaraStackIssueFix:
			{
				if (After->GetKind() != EUeAgentValueKind::String
					|| After->GetText() != TEXT("resolved"))
				{
					SetError(OutError,
						TEXT("graph_compound_niagara_issue_after_invalid"),
						Selector.GetIssueId());
					return false;
				}
				FString State;
				return TryIssueState(
						System, Selector, true, State, OutError)
					&& State == TEXT("unresolved");
			}
			case EGraphCompoundTypedInputKind::NiagaraCompoundInput:
				return TryApplyCompound(
					Profile, Selector, Plan, After, FencingToken,
					Resources, OutCompoundContext, OutError);
			case EGraphCompoundTypedInputKind::NiagaraRendererEventParameter:
				return TryApplyRenderer(System, Selector, After, OutError);
			default:
				SetError(OutError,
					TEXT("graph_compound_niagara_selector_kind_invalid"),
					Profile.GetCapabilityId());
				return false;
			}
		}

		FAssetDurableSessionRef PersistedSession(
			const FAssetDurableSessionRef& Prepared,
			const FDurableReceipt& Receipt)
		{
			return MakeShared<const FAssetDurableSession>(
				Prepared->GetRequest(), Prepared->GetInventory(),
				Prepared->GetJournalId(), Prepared->GetJournalPath(),
				Prepared->GetJournalRoot(), Prepared->GetTransactionRoot(),
				Prepared->GetStagingRoot(), Receipt.GetPhaseHistory());
		}

		bool TryRollbackDurable(
			const FString& TargetRef,
			const FAssetDurableSessionRef& Session,
			FPackageAssetDurableBackend& Backend,
			const int64 FencingToken,
			FUeAgentResourceTracker& Resources,
			FString& OutDetail)
		{
			FString UnloadDetail;
			const bool bMemoryClean = TryUnloadPackage(TargetRef, UnloadDetail);
			FDurableReceipt Receipt;
			FAssetDurableError Error;
			const bool bDurableClean = TryRollbackAssetDurability(
				Session, Backend, FencingToken, bMemoryClean,
				Resources, Receipt, Error);
			OutDetail = !UnloadDetail.IsEmpty()
				? UnloadDetail : Error.GetCode() + TEXT(":") + Error.GetDetail();
			return bMemoryClean && bDurableClean;
		}

		bool TryValidateCompileReadback(
			UNiagaraSystem& System,
			FString& OutDiagnostic)
		{
			const UeAgentNiagaraCompileValidation::FResult Compile =
				UeAgentNiagaraCompileValidation::InspectAsset(&System);
			if (!Compile.bCompilationComplete || Compile.bHasError
				|| Compile.ScriptCount <= 0
				|| System.HasOutstandingCompilationRequests(true)
				|| System.NeedsRequestCompile())
			{
				OutDiagnostic =
					TEXT("graph_compound_niagara_compile_readback_failed");
				return false;
			}
			return true;
		}

		bool TrySerializeResult(
			const FString& TargetRef,
			const FString& Revision,
			FUeAgentContractValue& OutResult,
			FGraphCompoundRuntimeError& OutError)
		{
			const TSharedRef<FJsonObject> Root = MakeShared<FJsonObject>();
			Root->SetStringField(TEXT("revision"), Revision);
			Root->SetStringField(TEXT("target_ref"), TargetRef);
			FString Canonical;
			FString Error;
			if (!Json::TryCanonicalize(Root, Canonical, Error))
			{
				SetError(OutError,
					TEXT("graph_compound_niagara_result_invalid"),
					TargetRef, Error);
				return false;
			}
			OutResult = FUeAgentContractValue(MoveTemp(Canonical));
			return true;
		}

		class FNiagaraTypedReadbackOracle final
			: public IIndependentReadbackOracle
		{
		public:
			FNiagaraTypedReadbackOracle(
				FGraphCompoundDomainRouteProfile InProfile,
				FGraphCompoundNiagaraMutationSelector InSelector,
				FMaterialNiagaraGraphNativeHook& InNativeHook)
				: Profile(MoveTemp(InProfile))
				, Selector(MoveTemp(InSelector))
				, NativeHook(InNativeHook)
			{
			}

			const FString& GetOracleId() const override
			{
				return NiagaraTypedReadbackOracleId;
			}

			bool TryRead(
				const FIndependentReadbackRequest& Request,
				FUeAgentResourceTracker& Resources,
				FIndependentReadbackObservation& OutObservation,
				FVerificationError& OutError) override
			{
				OutObservation = FIndependentReadbackObservation();
				OutError = FVerificationError();
				FString Detail;
				if (!TryUnloadPackage(Selector.GetTargetRef(), Detail))
				{
					OutObservation = FIndependentReadbackObservation::Unavailable(
						TEXT("graph_compound_niagara_unload_failed"), Detail);
					return true;
				}
				FUeAgentCanonicalValuePtr Actual;
				FGraphCompoundRuntimeError Error;
				if (!TryCaptureCurrentValue(
						Profile, Selector, NativeHook, Resources,
						Actual, Error)
					|| !Actual.IsValid())
				{
					OutObservation = FIndependentReadbackObservation::Unavailable(
						Error.Code, Error.SubjectId + TEXT(":") + Error.Detail);
					return true;
				}
				UNiagaraSystem* const System =
					TryResolveSystem(Selector.GetTargetRef(), Error);
				FString Diagnostic;
				if (!System || System->GetOutermost()->IsDirty()
					|| !TryValidateCompileReadback(*System, Diagnostic))
				{
					OutObservation = FIndependentReadbackObservation::Unavailable(
						!System ? Error.Code : Diagnostic,
						TEXT("reload:") + Selector.GetTargetRef());
					return true;
				}
				OutObservation = FIndependentReadbackObservation::Available(
					Actual.ToSharedRef(),
					TEXT("reload:") + Request.GetSubjectRef() + TEXT(":")
						+ FGuid::NewGuid().ToString(EGuidFormats::Digits));
				return true;
			}

		private:
			FGraphCompoundDomainRouteProfile Profile;
			FGraphCompoundNiagaraMutationSelector Selector;
			FMaterialNiagaraGraphNativeHook& NativeHook;
		};
	}

	FGraphCompoundNiagaraTypedMutationProduction::
	FGraphCompoundNiagaraTypedMutationProduction(
		FString InJournalRoot,
		FMaterialNiagaraGraphNativeHook& InNativeHook,
		FPackageAssetDurableBackend& InDurableBackend,
		int64& InLastFencingToken)
		: JournalRoot(MoveTemp(InJournalRoot))
		, NativeHook(InNativeHook)
		, DurableBackend(InDurableBackend)
		, LastFencingToken(InLastFencingToken)
	{
	}

	bool FGraphCompoundNiagaraTypedMutationProduction::TryCaptureSnapshot(
		const FGraphCompoundDomainRouteProfile& Profile,
		const FGraphCompoundNiagaraMutationSelector& Selector,
		FUeAgentResourceTracker& Resources,
		FUeAgentCanonicalSnapshotPtr& OutSnapshot,
		FGraphCompoundRuntimeError& OutError) const
	{
		OutSnapshot.Reset();
		OutError = {};
		if (!IsInGameThread() || !Selector.IsValid()
			|| Profile.GetTypedInputKind() != Selector.GetInputKind())
		{
			SetError(OutError,
				!IsInGameThread()
					? TEXT("graph_compound_niagara_typed_snapshot_thread_invalid")
					: TEXT("graph_compound_niagara_typed_selector_invalid"),
				Profile.GetCapabilityId());
			return false;
		}
		FUeAgentCanonicalValuePtr Current;
		if (!TryCaptureCurrentValue(
				Profile, Selector, NativeHook, Resources,
				Current, OutError)
			|| !Current.IsValid())
		{
			return false;
		}
		if (Selector.GetInputKind()
				== EGraphCompoundTypedInputKind::NiagaraStackIssueFix
			&& (Current->GetKind() != EUeAgentValueKind::String
				|| Current->GetText() != TEXT("unresolved")))
		{
			SetError(OutError,
				TEXT("graph_compound_niagara_issue_already_resolved"),
				Selector.GetIssueId());
			return false;
		}
		return TryBuildSnapshot(
			Profile, Selector, Current.ToSharedRef(),
			Resources, OutSnapshot, OutError);
	}

	bool FGraphCompoundNiagaraTypedMutationProduction::TryExecute(
		const FGraphCompoundDomainRouteProfile& Profile,
		const FGraphCompoundNiagaraMutationSelector& Selector,
		const FGraphCompoundCanonicalPayload& Payload,
		const FPlanningPreflightPlanRef& Plan,
		const FUeAgentCallerContext& CallerContext,
		TSharedRef<FUeAgentResourceTracker, ESPMode::ThreadSafe> Resources,
		const TSharedRef<IUeAgentCancellationSignal>& Cancellation,
		FUeAgentContractValue& OutResult,
		FGraphCompoundRuntimeError& OutError)
	{
		OutResult = FUeAgentContractValue(TEXT("{}"));
		OutError = {};
		const bool bPayloadValid = Selector.IsValid()
			&& Profile.GetTypedInputKind() == Selector.GetInputKind()
			&& Payload.GetEdits().Num() == 1
			&& Payload.GetEdits()[0].GetIdentity().GetSemanticId()
				== Selector.GetStableSemanticId()
			&& Payload.GetEdits()[0].GetIdentity().GetOwnerId()
				== Selector.GetTargetRef()
			&& Payload.GetEdits()[0].GetProposedAfter().IsValid()
			&& Plan->GetDescriptorIdentity()
				== Profile.GetExecutionContract().GetDescriptorIdentity()
			&& Plan->GetChangeSet().GetChanges().Num() == 1
			&& Plan->GetChangeSet().GetChanges()[0].GetIdentity()
				.GetSemanticId() == Selector.GetStableSemanticId();
		if (!IsInGameThread() || Cancellation->IsCancelled()
			|| !bPayloadValid)
		{
			SetError(OutError,
				Cancellation->IsCancelled()
					? TEXT("graph_compound_niagara_typed_cancelled")
					: !IsInGameThread()
						? TEXT("graph_compound_niagara_typed_thread_invalid")
						: TEXT("graph_compound_niagara_typed_plan_invalid"),
				Selector.GetTargetRef());
			return false;
		}

		FUeAgentCanonicalSnapshotPtr Current;
		if (!TryCaptureSnapshot(
				Profile, Selector, *Resources, Current, OutError))
		{
			return false;
		}
		FPlanningPreflightError RevalidateError;
		if (!TryRevalidatePlan(
				Plan, Current.ToSharedRef(), CallerContext,
				*Resources, RevalidateError))
		{
			SetError(OutError, RevalidateError.GetCode(),
				RevalidateError.GetIdentityKey(),
				RevalidateError.GetDetail());
			return false;
		}

		UNiagaraSystem* System =
			TryResolveSystem(Selector.GetTargetRef(), OutError);
		if (!System)
		{
			return false;
		}
		const int64 FencingToken = ++LastFencingToken;
		const FString JournalOwner =
			TEXT("graph-compound:") + Plan->GetRequestId();
		const FString PackageName = PackageNameFromRef(Selector.GetTargetRef());
		const FAssetDurableRequest DurableRequest(
			Plan,
			{FAssetDurableTarget(
				Plan->GetChangeSet().GetChanges()[0].GetIdentity(),
				PackageName, EPackageHeaderKind::Asset,
				EAssetDurableIntent::Save,
				ERollbackGrade::DurableBackup)},
			FencingToken, JournalOwner, EAtomicityMode::CompensatedSaga);
		FAssetDurableSessionPtr Prepared;
		FAssetDurableError DurableError;
		if (!TryPrepareAssetDurability(
				DurableRequest, DurableBackend, JournalRoot,
				*Resources, Prepared, DurableError)
			|| !Prepared.IsValid())
		{
			SetError(OutError, DurableError.GetCode(),
				PackageName, DurableError.GetDetail());
			return false;
		}

		const FUeAgentCanonicalValueRef After =
			Payload.GetEdits()[0].GetProposedAfter().ToSharedRef();
		FCompoundApplyContext CompoundContext;
		if (!TryApplyMutation(
				Profile, *System, Selector, Plan, After, FencingToken,
				NativeHook, Resources, CompoundContext, OutError))
		{
			TryRollbackCompound(CompoundContext, *Resources);
			System = nullptr;
			FString RollbackDetail;
			TryRollbackDurable(
				PackageName, Prepared.ToSharedRef(), DurableBackend,
				FencingToken, *Resources, RollbackDetail);
			OutError.Detail += TEXT(";") + RollbackDetail;
			return false;
		}

		FMaterialNiagaraGraphCompileSettleReceipt Settle;
		FMaterialNiagaraGraphAdapterError NativeError;
		if (!NativeHook.TryCompileAndSettleNiagara(
				*System, Settle, NativeError)
			|| !Settle.IsCompilationComplete()
			|| Settle.GetCompileErrorCount() != 0
			|| Settle.GetStackIssueCount() != 0)
		{
			TryRollbackCompound(CompoundContext, *Resources);
			System = nullptr;
			FString RollbackDetail;
			TryRollbackDurable(
				PackageName, Prepared.ToSharedRef(), DurableBackend,
				FencingToken, *Resources, RollbackDetail);
			SetError(OutError,
				NativeError.GetCode().IsEmpty()
					? TEXT("graph_compound_niagara_typed_compile_failed")
					: NativeError.GetCode(),
				PackageName,
				NativeError.GetDetail() + TEXT(";") + RollbackDetail);
			return false;
		}
		System->GetOutermost()->MarkPackageDirty();
		FDurableReceipt DurableReceipt;
		if (!TryPersistAssetDurability(
				Prepared.ToSharedRef(), DurableBackend, FencingToken,
				*Resources, DurableReceipt, DurableError))
		{
			TryRollbackCompound(CompoundContext, *Resources);
			System = nullptr;
			FString RollbackDetail;
			TryRollbackDurable(
				PackageName, Prepared.ToSharedRef(), DurableBackend,
				FencingToken, *Resources, RollbackDetail);
			SetError(OutError, DurableError.GetCode(), PackageName,
				DurableError.GetDetail() + TEXT(";") + RollbackDetail);
			return false;
		}
		System->GetOutermost()->SetDirtyFlag(false);
		System = nullptr;

		const FAssetDurableSessionRef Persisted =
			PersistedSession(Prepared.ToSharedRef(), DurableReceipt);
		const TSharedRef<FNiagaraTypedReadbackOracle, ESPMode::ThreadSafe>
			Oracle = MakeShared<FNiagaraTypedReadbackOracle,
				ESPMode::ThreadSafe>(Profile, Selector, NativeHook);
		TSharedPtr<FIndependentReadbackService> Readback;
		FVerificationError VerifyError;
		FIndependentReadbackOutcome Outcome;
		if (!FIndependentReadbackService::TryCreate(
				{Oracle}, Readback, VerifyError)
			|| !Readback.IsValid()
			|| !Readback->TryVerify(
				FIndependentReadbackRequest(
					TEXT("graph-compound:") + Plan->GetRequestId(),
					NiagaraTypedReadbackOracleId,
					Selector.GetStableSemanticId(), After),
				*Resources, Outcome, VerifyError)
			|| Outcome.GetStatus() != EVerificationStatus::Verified)
		{
			FString RollbackDetail;
			TryRollbackDurable(
				PackageName, Persisted, DurableBackend,
				FencingToken, *Resources, RollbackDetail);
			SetError(OutError,
				VerifyError.GetCode().IsEmpty()
					? TEXT("graph_compound_niagara_typed_readback_failed")
					: VerifyError.GetCode(),
				Selector.GetStableSemanticId(),
				Outcome.GetDiagnosticCode() + TEXT(";") + RollbackDetail);
			return false;
		}

		const FDurableVerificationAcknowledgement Acknowledgement(
			DurableReceipt.GetJournalId(), Plan->GetFingerprint(),
			Prepared->GetInventory().GetInventoryHash(),
			Outcome.GetEvidenceId(), JournalOwner, FencingToken);
		if (!TryAcknowledgeAssetDurability(
				Persisted, Acknowledgement, FencingToken,
				*Resources, DurableError))
		{
			FString RollbackDetail;
			TryRollbackDurable(
				PackageName, Persisted, DurableBackend,
				FencingToken, *Resources, RollbackDetail);
			SetError(OutError, DurableError.GetCode(), PackageName,
				DurableError.GetDetail() + TEXT(";") + RollbackDetail);
			return false;
		}
		if (CompoundContext.Native.IsValid()
			&& !CompoundContext.OperationId.IsEmpty())
		{
			CompoundContext.Native->ReleaseVerifiedOperation(
				CompoundContext.OperationId);
		}
		return TrySerializeResult(
			PackageName, Outcome.GetEvidenceId(), OutResult, OutError);
	}
}
