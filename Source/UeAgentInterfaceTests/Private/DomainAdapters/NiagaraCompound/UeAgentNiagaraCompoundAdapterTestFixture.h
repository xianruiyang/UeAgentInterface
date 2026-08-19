#pragma once

#include "Core/ContextResource/UeAgentContextResource.h"
#include "Core/Contract/UeAgentContract.h"
#include "Core/Discovery/UeAgentDiscovery.h"
#include "Core/ExecutionMutation/UeAgentExecutionMutation.h"
#include "Core/PlanningPreflight/UeAgentPlanningPreflight.h"
#include "Core/StateSemantic/UeAgentStateSemantic.h"
#include "DomainAdapters/GenericEffect/UeAgentGenericEffectAdapter.h"
#include "DomainAdapters/GenericRead/UeAgentGenericReadAdapter.h"

namespace UE::UAI::Tests::NiagaraCompound
{
	using namespace ContextResource;
	using namespace Contract;
	using namespace Discovery;
	using namespace DomainAdapters::GenericEffect;
	using namespace DomainAdapters::GenericRead;
	using namespace ExecutionMutation;
	using namespace PlanningPreflight;
	using namespace StateSemantic;

	inline TSharedRef<FUeAgentResourceTracker, ESPMode::ThreadSafe>
	MakeTracker(const int64 WorkUnits = 131072)
	{
		return MakeShared<
			FUeAgentResourceTracker,
			ESPMode::ThreadSafe>(
			FUeAgentResourceBudget({
				{EUeAgentResourceDimension::InputBytes, 1048576},
				{EUeAgentResourceDimension::OutputBytes, 1048576},
				{EUeAgentResourceDimension::StringBytes, 1048576},
				{EUeAgentResourceDimension::Nodes, 16384},
				{EUeAgentResourceDimension::Items, 16384},
				{EUeAgentResourceDimension::Files, 64},
				{EUeAgentResourceDimension::ReferenceEdges, 16384},
				{EUeAgentResourceDimension::WorkUnits, WorkUnits},
				{
					EUeAgentResourceDimension::EstimatedMemoryBytes,
					33554432,
				},
			}));
	}

	inline FUeAgentCanonicalValueRef MakeString(
		const FString& Text,
		FUeAgentResourceTracker& Resources)
	{
		FUeAgentCanonicalValuePtr Value;
		FUeAgentStateSemanticError Error;
		check(FUeAgentCanonicalValue::TryString(
			Text,
			Resources,
			Value,
			Error));
		return Value.ToSharedRef();
	}

	inline FUeAgentCanonicalValueRef MakeDecimal(
		const FString& Lexeme,
		FUeAgentResourceTracker& Resources)
	{
		FUeAgentCanonicalValuePtr Value;
		FUeAgentStateSemanticError Error;
		check(FUeAgentCanonicalValue::TryDecimal(
			Lexeme,
			Resources,
			Value,
			Error));
		return Value.ToSharedRef();
	}

	inline FUeAgentCanonicalValueRef MakeObject(
		TArray<TPair<FString, FUeAgentCanonicalValueRef>> Fields,
		FUeAgentResourceTracker& Resources)
	{
		FUeAgentCanonicalValuePtr Value;
		FUeAgentStateSemanticError Error;
		check(FUeAgentCanonicalValue::TryObject(
			Fields,
			Resources,
			Value,
			Error));
		return Value.ToSharedRef();
	}

	inline FUeAgentCanonicalValueRef MakeArray(
		TArray<FUeAgentCanonicalValueRef> Items,
		FUeAgentResourceTracker& Resources)
	{
		FUeAgentCanonicalValuePtr Value;
		FUeAgentStateSemanticError Error;
		check(FUeAgentCanonicalValue::TryArray(
			Items,
			Resources,
			Value,
			Error));
		return Value.ToSharedRef();
	}

	inline FUeAgentCanonicalValueRef MakeLiteralInput(
		const FString& InputName,
		const FString& Lexeme,
		FUeAgentResourceTracker& Resources)
	{
		return MakeObject(
			{
				{TEXT("kind"), MakeString(TEXT("niagara_input"), Resources)},
				{TEXT("input_name"), MakeString(InputName, Resources)},
				{TEXT("type_id"), MakeString(TEXT("float"), Resources)},
				{TEXT("mode"), MakeString(TEXT("literal"), Resources)},
				{TEXT("literal"), MakeDecimal(Lexeme, Resources)},
			},
			Resources);
	}

	inline FUeAgentCanonicalValueRef MakeDynamicInput(
		const FString& InputName,
		const FString& ScriptIdentity,
		TArray<FUeAgentCanonicalValueRef> Children,
		FUeAgentResourceTracker& Resources)
	{
		return MakeObject(
			{
				{TEXT("kind"), MakeString(TEXT("niagara_input"), Resources)},
				{TEXT("input_name"), MakeString(InputName, Resources)},
				{TEXT("type_id"), MakeString(TEXT("float"), Resources)},
				{TEXT("mode"), MakeString(TEXT("dynamic_input"), Resources)},
				{
					TEXT("dynamic_script_identity"),
					MakeString(ScriptIdentity, Resources),
				},
				{
					TEXT("inputs"),
					MakeArray(MoveTemp(Children), Resources),
				},
			},
			Resources);
	}

	inline FUeAgentCanonicalValueRef MakeInvalidMixedBranchInput(
		FUeAgentResourceTracker& Resources)
	{
		return MakeObject(
			{
				{TEXT("kind"), MakeString(TEXT("niagara_input"), Resources)},
				{
					TEXT("input_name"),
					MakeString(TEXT("Particles.Scale"), Resources),
				},
				{TEXT("type_id"), MakeString(TEXT("float"), Resources)},
				{TEXT("mode"), MakeString(TEXT("dynamic_input"), Resources)},
				{
					TEXT("dynamic_script_identity"),
					MakeString(
						TEXT("/Niagara/Functions/Dynamic/MultiplyFloat"),
						Resources),
				},
				{TEXT("literal"), MakeDecimal(TEXT("2.5"), Resources)},
				{TEXT("inputs"), MakeArray({}, Resources)},
			},
			Resources);
	}

	inline FUeAgentCanonicalValueRef MakeDeepDynamicInput(
		const int32 Levels,
		FUeAgentResourceTracker& Resources)
	{
		FUeAgentCanonicalValueRef Current =
			MakeLiteralInput(
				TEXT("Leaf.Value"),
				TEXT("1.25"),
				Resources);
		for (int32 Index = Levels - 1; Index >= 0; --Index)
		{
			Current = MakeDynamicInput(
				FString::Printf(TEXT("Layer.%d"), Index),
				TEXT("/Niagara/Functions/Dynamic/MultiplyFloat"),
				{Current},
				Resources);
		}
		return Current;
	}

	inline FUeAgentCanonicalIdentity MakeIdentity()
	{
		return FUeAgentCanonicalIdentity(
			TEXT("uai.semantic.niagara.dynamic_input"),
			TEXT("niagara_dynamic_input"),
			TEXT("/Game/UAI/P5_003/NS_CompoundFixture"),
			TEXT(
				"EmitterA/particle_update/ScaleColor/"
				"Particles.Scale"),
			TEXT("compound_input"),
			TEXT("niagara-fixture-generation-17"));
	}

	inline FUeAgentDescriptorIdentity MakeDescriptor()
	{
		return FUeAgentDescriptorIdentity(
			TEXT("uai.operation.niagara.compound"),
			1,
			TEXT(
				"sha256:7777777777777777777777777777777777777777777777777777777777777777"));
	}

	inline FUeAgentCandidateBinding MakeCandidateBinding()
	{
		return FUeAgentCandidateBinding(
			TEXT("binding:p5-003:niagara-compound"),
			FUeAgentCandidateSummary(
				MakeIdentity(),
				MakeDescriptor(),
				TEXT("niagara_compound"),
				TEXT("Particles.Scale"),
				TEXT("P5-003 independent Niagara compound fixture"),
				{TEXT("Niagara"), TEXT("EmitterA"), TEXT("ParticleUpdate")}),
			FUeAgentCatalogRevision(
				TEXT("niagara-compound-catalog"),
				TEXT("niagara-fixture-generation-17"),
				43),
			TEXT(
				"uai://asset/%2FGame%2FUAI%2FP5_003%2F"
				"NS_CompoundFixture#Particles.Scale"),
			TEXT("niagara-compound-read"),
			{
				EUeAgentCandidateOperation::Read,
				EUeAgentCandidateOperation::Create,
				EUeAgentCandidateOperation::Update,
				EUeAgentCandidateOperation::Replace,
				EUeAgentCandidateOperation::Delete,
			},
			TEXT("fixture:p5-003:catalog-binding"));
	}

	inline FUeAgentGenericEffectBinding MakeEffectBinding()
	{
		return FUeAgentGenericEffectBinding(
			MakeCandidateBinding(),
			TEXT("UE.NiagaraInput:compound"),
			FUeAgentCanonicalType(
				TEXT("uai.niagara.input.v1"),
				EUeAgentValueKind::Object,
				false,
				TEXT("object")),
			TEXT("fixture:p5-003:niagara-type-binding"));
	}

	inline FPlanningPreflightPlanRef MakePlan(
		FUeAgentResourceTracker& Resources,
		const FUeAgentCanonicalValueRef& After)
	{
		TArray<FPlannedChange> Changes;
		Changes.Emplace(
			MakeIdentity(),
			EChangeIntent::Create,
			FUeAgentCanonicalValuePtr(),
			After,
			TArray<FString>{TEXT("uai:niagara:write")},
			TEXT("fixture:p5-003:direct-compound-impact"));
		TArray<FImpact> Impacts;
		Impacts.Emplace(
			MakeIdentity(),
			MakeIdentity(),
			TEXT("direct"),
			TEXT("fixture:p5-003:direct-compound-impact"));
		return MakeShared<FPlanningPreflightPlan>(
			MakeDescriptor(),
			TEXT("request:p5-003"),
			TEXT("idempotency:p5-003"),
			TEXT("agent:p5-003"),
			TArray<FString>{TEXT("uai:niagara:write")},
			TEXT("snapshot:p5-003:before"),
			FUeAgentStateRevision(
				TEXT("niagara-compound-read"),
				TEXT("niagara-fixture-generation-17"),
				42),
			TEXT("fixture:p5-003:independent-before"),
			FChangeSet(MoveTemp(Changes)),
			FImpactSet(MoveTemp(Impacts)),
			TArray<FPrecondition>(),
			TEXT(
				"sha256:8888888888888888888888888888888888888888888888888888888888888888"));
	}

	inline FUeAgentExecutionContextRefOwner MakeExecutionContext(
		const FUeAgentDescriptorIdentity& DescriptorIdentity)
	{
		FUeAgentExecutionContextInput Input;
		Input.ProjectInstanceId = TEXT("fixture-project:p5-003");
		Input.EditorInstanceId = TEXT("fixture-editor:p5-003");
		Input.StatePlane = EUeAgentStatePlane::Persisted;
		Input.ContextRevision = TEXT("fixture-context:p5-003:r1");

		FUeAgentExecutionContextRefPtr Context;
		FUeAgentExecutionContextError Error;
		check(FUeAgentExecutionContextRef::TryCreate(
			Input,
			DescriptorIdentity,
			EUeAgentRequestKind::Operation,
			FUeAgentExecutionContextPolicy(
				{EUeAgentStatePlane::Persisted},
				{},
				EUeAgentContextResolutionPolicy::ExplicitOnly,
				EUeAgentTransientContextPolicy::ReadOnly),
			Context,
			Error));
		return Context.ToSharedRef();
	}

	struct FRequestFixture
	{
		explicit FRequestFixture(
			const FUeAgentCanonicalValueRef& After,
			const int64 WorkUnits = 131072)
			: Resources(MakeTracker(WorkUnits))
			, Cancellation([this]() { return Now; })
			, Plan(MakePlan(*Resources, After))
			, ExecutionContext(
				MakeExecutionContext(
					Plan->GetDescriptorIdentity()))
		{
		}

		FExecutionMutationRequest MakeRequest()
		{
			return FExecutionMutationRequest(
				TEXT("operation:p5-003"),
				TEXT("owned-worker:p5-003"),
				Plan,
				ExecutionContext,
				Resources,
				FUeAgentOperationDeadline::FromTimeout(
					30.0,
					[this]() { return Now; }),
				Cancellation.GetToken(),
				EExecutionThreadAffinity::GameThread,
				EExecutionReentrancyPolicy::RejectSameTarget);
		}

		double Now = 100.0;
		TSharedRef<FUeAgentResourceTracker, ESPMode::ThreadSafe>
			Resources;
		FUeAgentCancellationSource Cancellation;
		FPlanningPreflightPlanRef Plan;
		FUeAgentExecutionContextRefOwner ExecutionContext;
	};

	inline TSharedPtr<const FUeAgentUeReadCapture> MakeCapture(
		FUeAgentResourceTracker& Resources,
		const int64 Sequence,
		const FString& Evidence,
		const FUeAgentCanonicalValueRef& Value)
	{
		const FUeAgentStateRevision Revision(
			TEXT("niagara-compound-read"),
			TEXT("niagara-fixture-generation-17"),
			Sequence);
		TArray<FUeAgentUeReadEntry> Entries;
		Entries.Emplace(
			MakeIdentity(),
			FUeAgentUeTypeBinding(
				TEXT("UE.NiagaraInput:compound"),
				FUeAgentCanonicalType(
					TEXT("uai.niagara.input.v1"),
					EUeAgentValueKind::Object,
					false,
					TEXT("object")),
				TEXT("fixture:p5-003:niagara-type-binding")),
			Value,
			FUeAgentDefaultValue(
				EUeAgentDefaultSourceKind::DomainNative,
				TEXT("Niagara.DynamicInput"),
				TEXT("fixture:p5-003:native-default"),
				Value),
			FUeAgentStateDetails(
				TEXT("Particles.Scale"),
				TEXT("Independent Niagara graph capture"),
				{TEXT("Niagara"), TEXT("Inputs")},
				true,
				TEXT("available"),
				{{TEXT("input_mode"), TEXT("dynamic_input")}}),
			Revision);
		return MakeShared<FUeAgentUeReadCapture>(
			FString::Printf(
				TEXT("snapshot:p5-003:%lld"),
				Sequence),
			EUeAgentStatePlane::Persisted,
			Revision,
			FUeAgentTemporalPoint(
				TEXT("editor-session"),
				FString::Printf(TEXT("%lld"), Sequence),
				TEXT("1"),
				TEXT("1"),
				TEXT("captured")),
			Evidence,
			true,
			MoveTemp(Entries));
	}
}
