#include "Misc/AutomationTest.h"

#include "DomainAdapters/NiagaraCompound/UeAgentNiagaraCompoundAdapterTestFixture.h"
#include "EdGraph/EdGraphPin.h"

#include "DomainAdapters/NiagaraCompound/UeAgentNiagaraCompoundDiscovery.h"
#include "DomainAdapters/NiagaraCompound/UeAgentNiagaraCompoundNativeHook.h"

namespace
{
	using namespace UE::UAI::ContextResource;
	using namespace UE::UAI::Discovery;
	using namespace UE::UAI::DomainAdapters::GenericEffect;
	using namespace UE::UAI::DomainAdapters::GenericRead;
	using namespace UE::UAI::DomainAdapters::NiagaraCompound;
	using namespace UE::UAI::ExecutionMutation;
	using namespace UE::UAI::PlanningPreflight;
	using namespace UE::UAI::StateSemantic;
	using namespace UE::UAI::Tests::NiagaraCompound;

	bool DiscoverFirstLiteralBinding(
		FUeAgentResourceTracker& Resources,
		const FUeAgentOperationDeadline& Deadline,
		const FUeAgentCancellationToken& Cancellation,
		TSharedPtr<const FUeAgentCandidateBinding>& OutBinding,
		FString& OutError)
	{
		TArray<FUeAgentCandidateBinding> Bindings;
		FUeAgentDiscoveryError Error;
		if (!TryDiscoverNiagaraCompoundBindings(
				TEXT(
					"/Game/Niagara/NS_UAI_HighQuality_Fire."
					"NS_UAI_HighQuality_Fire"),
				GetNiagaraCompoundOperationDescriptorIdentity(),
				TEXT("p5-005-owned-worker-generation"),
				Resources,
				Deadline,
				Cancellation,
				Bindings,
				Error))
		{
			OutError = Error.Code + TEXT(":") + Error.Detail;
			return false;
		}
		for (const FUeAgentCandidateBinding& Binding : Bindings)
		{
			FUeAgentResolvedNiagaraCompoundInput Resolved;
			FString Code;
			FString Detail;
			if (TryResolveNiagaraCompoundInput(
					Binding,
					TEXT("p5-005-owned-worker-generation"),
					Resolved,
					Code,
					Detail)
				&& Resolved.Input.VisiblePin
				&& Resolved.Input.VisiblePin->LinkedTo.IsEmpty()
				&& Resolved.Input.InputVariable.GetType()
					== FNiagaraTypeDefinition::GetBoolDef())
			{
				OutBinding =
					MakeShared<const FUeAgentCandidateBinding>(
						Binding);
				return true;
			}
		}
		OutError = TEXT("literal_niagara_input_not_found");
		return false;
	}

	bool DiscoverTwoLiteralBindings(
		FUeAgentResourceTracker& Resources,
		const FUeAgentOperationDeadline& Deadline,
		const FUeAgentCancellationToken& Cancellation,
		TArray<TSharedPtr<const FUeAgentCandidateBinding>>& OutBindings,
		FString& OutError)
	{
		OutBindings.Reset();
		TArray<FUeAgentCandidateBinding> Bindings;
		FUeAgentDiscoveryError Error;
		if (!TryDiscoverNiagaraCompoundBindings(
				TEXT(
					"/Game/Niagara/NS_UAI_HighQuality_Fire."
					"NS_UAI_HighQuality_Fire"),
				GetNiagaraCompoundOperationDescriptorIdentity(),
				TEXT("p5-005-owned-worker-generation"),
				Resources,
				Deadline,
				Cancellation,
				Bindings,
				Error))
		{
			OutError = Error.Code + TEXT(":") + Error.Detail;
			return false;
		}
		for (const FUeAgentCandidateBinding& Binding : Bindings)
		{
			FUeAgentResolvedNiagaraCompoundInput Resolved;
			FString Code;
			FString Detail;
			if (!TryResolveNiagaraCompoundInput(
					Binding,
					TEXT("p5-005-owned-worker-generation"),
					Resolved,
					Code,
					Detail)
				|| !Resolved.Input.VisiblePin
				|| !Resolved.Input.VisiblePin->LinkedTo.IsEmpty())
			{
				continue;
			}
			const FNiagaraTypeDefinition& Type =
				Resolved.Input.InputVariable.GetType();
			if (Type != FNiagaraTypeDefinition::GetBoolDef()
				&& Type != FNiagaraTypeDefinition::GetFloatDef()
				&& Type != FNiagaraTypeDefinition::GetIntDef())
			{
				continue;
			}
			OutBindings.Add(
				MakeShared<const FUeAgentCandidateBinding>(Binding));
			if (OutBindings.Num() == 2)
			{
				return true;
			}
		}
		OutError = TEXT("two_literal_niagara_inputs_not_found");
		return false;
	}

	bool DiscoverNestedDynamicBinding(
		FUeAgentResourceTracker& Resources,
		const FUeAgentOperationDeadline& Deadline,
		const FUeAgentCancellationToken& Cancellation,
		TSharedPtr<const FUeAgentCandidateBinding>& OutBinding,
		FString& OutError)
	{
		TArray<FUeAgentCandidateBinding> Bindings;
		FUeAgentDiscoveryError Error;
		if (!TryDiscoverNiagaraCompoundBindings(
				TEXT(
					"/Game/Niagara/OfficialTemplateStudy/"
					"BehaviorExamples/NE_WaveForms.NE_WaveForms"),
				GetNiagaraCompoundOperationDescriptorIdentity(),
				TEXT("p5-005-owned-worker-generation"),
				Resources,
				Deadline,
				Cancellation,
				Bindings,
				Error))
		{
			OutError = Error.Code + TEXT(":") + Error.Detail;
			return false;
		}
		for (const FUeAgentCandidateBinding& Binding : Bindings)
		{
			FUeAgentResolvedNiagaraCompoundInput Resolved;
			FString Code;
			FString Detail;
			if (TryResolveNiagaraCompoundInput(
					Binding,
					TEXT("p5-005-owned-worker-generation"),
					Resolved,
					Code,
					Detail)
				&& Resolved.Input.FullInputName
					== TEXT("Module.Position Offset"))
			{
				OutBinding =
					MakeShared<const FUeAgentCandidateBinding>(
						Binding);
				return true;
			}
		}
		OutError = TEXT("nested_dynamic_niagara_input_not_found");
		return false;
	}

	const FUeAgentCanonicalValueRef* FindField(
		const FUeAgentCanonicalValue& Value,
		const FString& Name)
	{
		const TPair<FString, FUeAgentCanonicalValueRef>* Found =
			Value.GetObjectItems().FindByPredicate(
				[&Name](
					const TPair<
						FString,
						FUeAgentCanonicalValueRef>& Pair)
				{
					return Pair.Key == Name;
				});
		return Found ? &Found->Value : nullptr;
	}

	FUeAgentCanonicalValueRef MakeChangedLiteral(
		const FUeAgentCanonicalValue& Before,
		FUeAgentResourceTracker& Resources)
	{
		FString InputName;
		FString TypeId;
		if (const FUeAgentCanonicalValueRef* Field =
				FindField(Before, TEXT("input_name")))
		{
			InputName = (*Field)->GetText();
		}
		if (const FUeAgentCanonicalValueRef* Field =
				FindField(Before, TEXT("type_id")))
		{
			TypeId = (*Field)->GetText();
		}
		FString ChangedLiteral(TEXT("0.12345"));
		if (TypeId == TEXT("bool"))
		{
			FString BeforeLiteral;
			if (const FUeAgentCanonicalValueRef* Field =
					FindField(Before, TEXT("literal")))
			{
				BeforeLiteral = (*Field)->GetText();
			}
			ChangedLiteral =
				BeforeLiteral == TEXT("true")
					? TEXT("false")
					: TEXT("true");
		}
		else if (TypeId == TEXT("int32"))
		{
			FString BeforeLiteral;
			if (const FUeAgentCanonicalValueRef* Field =
					FindField(Before, TEXT("literal")))
			{
				BeforeLiteral = (*Field)->GetText();
			}
			ChangedLiteral =
				BeforeLiteral == TEXT("1")
					? TEXT("0")
					: TEXT("1");
		}
		return MakeObject(
			{
				{
					TEXT("kind"),
					MakeString(TEXT("niagara_input"), Resources),
				},
				{
					TEXT("input_name"),
					MakeString(InputName, Resources),
				},
				{
					TEXT("type_id"),
					MakeString(TypeId, Resources),
				},
				{
					TEXT("mode"),
					MakeString(TEXT("literal"), Resources),
				},
				{
					TEXT("literal"),
					MakeString(ChangedLiteral, Resources),
				},
			},
			Resources);
	}

	FString ReadStringField(
		const FUeAgentCanonicalValue& Value,
		const FString& Name)
	{
		if (const FUeAgentCanonicalValueRef* Field =
				FindField(Value, Name);
			Field
			&& (*Field)->GetKind()
				== EUeAgentValueKind::String)
		{
			return (*Field)->GetText();
		}
		return FString();
	}

	FPlanningPreflightPlanRef MakeSingleChangePlan(
		const FUeAgentCandidateBinding& Binding,
		const FUeAgentUeReadCapture& BeforeCapture,
		const FUeAgentCanonicalValueRef& Before,
		const FUeAgentCanonicalValueRef& After,
		const FString& Suffix)
	{
		const FUeAgentCanonicalIdentity& Identity =
			Binding.GetSummary().GetIdentity();
		TArray<FPlannedChange> Changes;
		Changes.Emplace(
			Identity,
			EChangeIntent::Update,
			Before,
			After,
			TArray<FString>{TEXT("uai:niagara:write")},
			TEXT("p5-005:") + Suffix);
		TArray<FImpact> Impacts;
		Impacts.Emplace(
			Identity,
			Identity,
			TEXT("direct"),
			TEXT("p5-005:") + Suffix);
		return MakeShared<FPlanningPreflightPlan>(
			GetNiagaraCompoundOperationDescriptorIdentity(),
			TEXT("request:p5-005:") + Suffix,
			TEXT("idempotency:p5-005:") + Suffix,
			TEXT("agent:p5-005"),
			TArray<FString>{TEXT("uai:niagara:write")},
			BeforeCapture.GetSnapshotId(),
			BeforeCapture.GetRevision(),
			BeforeCapture.GetEvidenceId(),
			FChangeSet(MoveTemp(Changes)),
			FImpactSet(MoveTemp(Impacts)),
			TArray<FPrecondition>(),
			TEXT(
				"sha256:"
				"9999999999999999999999999999999999999999999999999999999999999999"));
	}

	FUeAgentGenericEffectBinding MakeNativeEffectBinding(
		const FUeAgentCandidateBinding& Binding)
	{
		return FUeAgentGenericEffectBinding(
			Binding,
			TEXT("UE.NiagaraInput:compound"),
			FUeAgentCanonicalType(
				TEXT("uai.niagara.input.v1"),
				EUeAgentValueKind::Object,
				false,
				TEXT("object")),
			TEXT("niagara-native-graph-input"));
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUeAgentNiagaraCompoundDiscoveryCaptureTest,
	"UeAgentInterface.DomainAdapters.NiagaraCompound.Native.DiscoveryCapture",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FUeAgentNiagaraCompoundDiscoveryCaptureTest::RunTest(
	const FString& Parameters)
{
	using namespace UE::UAI::ContextResource;
	using namespace UE::UAI::Discovery;
	using namespace UE::UAI::DomainAdapters::GenericRead;
	using namespace UE::UAI::DomainAdapters::NiagaraCompound;
	using namespace UE::UAI::Tests::NiagaraCompound;

	const FString AssetPath(
		TEXT(
			"/Game/Niagara/NS_UAI_HighQuality_Fire."
			"NS_UAI_HighQuality_Fire"));
	const FString Generation(TEXT("p5-005-owned-worker-generation"));
	const TSharedRef<FUeAgentResourceTracker, ESPMode::ThreadSafe>
		Resources = MakeTracker();
	const auto Clock = []() { return 100.0; };
	FUeAgentCancellationSource Cancellation(Clock);
	TArray<FUeAgentCandidateBinding> Bindings;
	FUeAgentDiscoveryError DiscoveryError;
	TestTrue(
		TEXT("the native catalog discovers real Niagara compound inputs"),
		TryDiscoverNiagaraCompoundBindings(
			AssetPath,
			GetNiagaraCompoundOperationDescriptorIdentity(),
			Generation,
			*Resources,
			FUeAgentOperationDeadline::FromTimeout(30.0, Clock),
			Cancellation.GetToken(),
			Bindings,
			DiscoveryError));
	if (Bindings.IsEmpty())
	{
		AddError(
			DiscoveryError.Code + TEXT(":") + DiscoveryError.Detail);
		return false;
	}

	const FUeAgentCandidateBinding& Binding = Bindings[0];
	TestEqual(
		TEXT("the discovered candidate uses the one native read port"),
		Binding.GetReadPortId(),
		GetNiagaraCompoundReadPortId());
	TestEqual(
		TEXT("the candidate and operation share one descriptor"),
		Binding.GetSummary().GetDescriptorIdentity(),
		GetNiagaraCompoundOperationDescriptorIdentity());
	TestEqual(
		TEXT("the candidate owner is the real Niagara asset"),
		Binding.GetSummary().GetIdentity().GetOwnerId(),
		AssetPath);

	const TSharedRef<FUeAgentNiagaraCompoundNativeHook> NativeHook =
		MakeShared<FUeAgentNiagaraCompoundNativeHook>(Generation);
	const TSharedRef<FUeAgentNiagaraCompoundReadHook> ReadHook =
		MakeShared<FUeAgentNiagaraCompoundReadHook>(NativeHook);
	const FUeAgentStateReadRequest Request(
		Binding,
		EUeAgentStatePlane::Persisted);
	TSharedPtr<const FUeAgentUeReadCapture> Capture;
	FUeAgentGenericReadError ReadError;
	TestTrue(
		TEXT("the native hook captures the real graph state"),
		ReadHook->TryCapture(
			Request,
			*Resources,
			FUeAgentOperationDeadline::FromTimeout(30.0, Clock),
			Cancellation.GetToken(),
			Capture,
			ReadError));
	if (!Capture.IsValid() || Capture->GetEntries().Num() != 1)
	{
		AddError(ReadError.Code + TEXT(":") + ReadError.Detail);
		return false;
	}
	TestEqual(
		TEXT("C2 binding and fresh C4 capture use one identity"),
		Capture->GetEntries()[0].GetIdentity().GetCanonicalKey(),
		Binding.GetSummary().GetIdentity().GetCanonicalKey());
	TestEqual(
		TEXT("fresh capture uses the native read revision source"),
		Capture->GetRevision().GetSourceId(),
		GetNiagaraCompoundReadPortId());
	TSharedPtr<const FUeAgentUeReadCapture> RepeatedCapture;
	TestTrue(
		TEXT("an unchanged native graph can be captured again"),
		ReadHook->TryCapture(
			Request,
			*Resources,
			FUeAgentOperationDeadline::FromTimeout(30.0, Clock),
			Cancellation.GetToken(),
			RepeatedCapture,
			ReadError));
	TestTrue(
		TEXT("unchanged persisted graph state keeps one revision"),
		RepeatedCapture.IsValid()
			&& RepeatedCapture->GetRevision()
				== Capture->GetRevision());
	TestTrue(
		TEXT("native state revision survives an exact JSON round trip"),
		Capture->GetRevision().GetSequence()
			<= 9007199254740991ll);
	TestTrue(
		TEXT("fresh capture evidence is graph-derived"),
		Capture->GetEvidenceId().StartsWith(
			TEXT("niagara-compound-native:")));
	TestTrue(
		TEXT("the captured Niagara input is a typed C4 object"),
		Capture->GetEntries()[0].GetValue()->GetKind()
			== UE::UAI::StateSemantic::EUeAgentValueKind::Object);
	return !HasAnyErrors();
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUeAgentNiagaraCompoundNativeLiteralMutationTest,
	"UeAgentInterface.DomainAdapters.NiagaraCompound.Native.LiteralMutationRollback",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FUeAgentNiagaraCompoundNativeLiteralMutationTest::RunTest(
	const FString& Parameters)
{
	const TSharedRef<FUeAgentResourceTracker, ESPMode::ThreadSafe>
		Resources = MakeTracker();
	const auto Clock = []() { return 100.0; };
	FUeAgentCancellationSource Cancellation(Clock);
	const FUeAgentOperationDeadline Deadline =
		FUeAgentOperationDeadline::FromTimeout(30.0, Clock);
	TSharedPtr<const FUeAgentCandidateBinding> Binding;
	FString FixtureError;
	if (!DiscoverFirstLiteralBinding(
			*Resources,
			Deadline,
			Cancellation.GetToken(),
			Binding,
			FixtureError))
	{
		AddError(FixtureError);
		return false;
	}
	if (!Binding.IsValid())
	{
		AddError(TEXT("literal_niagara_binding_invalid"));
		return false;
	}

	const TSharedRef<FUeAgentNiagaraCompoundNativeHook> NativeHook =
		MakeShared<FUeAgentNiagaraCompoundNativeHook>(
			TEXT("p5-005-owned-worker-generation"));
	const TSharedRef<FUeAgentNiagaraCompoundReadHook> ReadHook =
		MakeShared<FUeAgentNiagaraCompoundReadHook>(NativeHook);
	const FUeAgentStateReadRequest ReadRequest(
		*Binding,
		EUeAgentStatePlane::AuthoringMemory);
	TSharedPtr<const FUeAgentUeReadCapture> BeforeCapture;
	FUeAgentGenericReadError ReadError;
	if (!ReadHook->TryCapture(
			ReadRequest,
			*Resources,
			Deadline,
			Cancellation.GetToken(),
			BeforeCapture,
			ReadError)
		|| !BeforeCapture.IsValid()
		|| BeforeCapture->GetEntries().Num() != 1)
	{
		AddError(ReadError.Code + TEXT(":") + ReadError.Detail);
		return false;
	}
	const FUeAgentCanonicalValueRef Before =
		BeforeCapture->GetEntries()[0].GetValue();
	const FUeAgentCanonicalValueRef After =
		MakeChangedLiteral(*Before, *Resources);
	const FUeAgentCanonicalIdentity& Identity =
		Binding->GetSummary().GetIdentity();
	TArray<FPlannedChange> Changes;
	Changes.Emplace(
		Identity,
		EChangeIntent::Update,
		Before,
		After,
		TArray<FString>{TEXT("uai:niagara:write")},
		TEXT("p5-005:native-literal"));
	TArray<FImpact> Impacts;
	Impacts.Emplace(
		Identity,
		Identity,
		TEXT("direct"),
		TEXT("p5-005:native-literal"));
	const FPlanningPreflightPlanRef Plan =
		MakeShared<FPlanningPreflightPlan>(
			GetNiagaraCompoundOperationDescriptorIdentity(),
			TEXT("request:p5-005:native-literal"),
			TEXT("idempotency:p5-005:native-literal"),
			TEXT("agent:p5-005"),
			TArray<FString>{TEXT("uai:niagara:write")},
			BeforeCapture->GetSnapshotId(),
			BeforeCapture->GetRevision(),
			BeforeCapture->GetEvidenceId(),
			FChangeSet(MoveTemp(Changes)),
			FImpactSet(MoveTemp(Impacts)),
			TArray<FPrecondition>(),
			TEXT(
				"sha256:"
				"9999999999999999999999999999999999999999999999999999999999999999"));
	const FExecutionMutationRequest MutationRequest(
		TEXT("operation:p5-005:native-literal"),
		TEXT("owned-worker:p5-005"),
		Plan,
		MakeExecutionContext(Plan->GetDescriptorIdentity()),
		Resources,
		Deadline,
		Cancellation.GetToken(),
		EExecutionThreadAffinity::GameThread,
		EExecutionReentrancyPolicy::RejectSameTarget);
	const FUeAgentGenericEffectBinding EffectBinding(
		*Binding,
		TEXT("UE.NiagaraInput:compound"),
		FUeAgentCanonicalType(
			TEXT("uai.niagara.input.v1"),
			EUeAgentValueKind::Object,
			false,
			TEXT("object")),
		TEXT("niagara-native-graph-input"));
	const TSharedRef<FUeAgentNiagaraCompoundEffectHook> EffectHook =
		MakeShared<FUeAgentNiagaraCompoundEffectHook>(NativeHook);
	FUeAgentGenericEffectApplyResult ApplyResult;
	FExecutionMutationError MutationError;
	TestTrue(
		TEXT("the concrete hook applies one literal graph mutation"),
		EffectHook->TryApply(
			MutationRequest,
			FLockFenceLease(
				TEXT("owned-worker:p5-005"),
				{Identity.GetCanonicalKey()},
				1),
			{EffectBinding},
			*Resources,
			ApplyResult,
			MutationError));
	if (!ApplyResult.IsValid())
	{
		AddError(
			MutationError.GetCode()
			+ TEXT(":")
			+ MutationError.GetDetail());
		return false;
	}
	TSharedPtr<const FUeAgentUeReadCapture> ChangedCapture;
	TestTrue(
		TEXT("a fresh authoring-memory capture observes the mutation"),
		ReadHook->TryCapture(
			ReadRequest,
			*Resources,
			Deadline,
			Cancellation.GetToken(),
			ChangedCapture,
			ReadError));
	TestFalse(
		TEXT("the fresh value differs from the before capture"),
		CompareValues(
			ChangedCapture->GetEntries()[0].GetValue(),
			Before,
			TEXT("/niagara_input"),
			*Resources)
			.IsEquivalent());
	TestTrue(
		TEXT("the fresh value uses the same canonical representation as the requested literal"),
		CompareValues(
			ChangedCapture->GetEntries()[0].GetValue(),
			After,
			TEXT("/niagara_input"),
			*Resources)
			.IsEquivalent());
	TestTrue(
		TEXT("a semantic graph mutation changes the state revision"),
		ChangedCapture->GetRevision()
			!= BeforeCapture->GetRevision());

	bool bRollbackConfirmed = false;
	TestTrue(
		TEXT("rollback restores the captured before value"),
		EffectHook->TryRollback(
			MutationRequest,
			ApplyResult,
			*Resources,
			bRollbackConfirmed,
			MutationError));
	TestTrue(
		TEXT("rollback is confirmed by the native hook"),
		bRollbackConfirmed);
	return !HasAnyErrors();
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUeAgentNiagaraCompoundNativeAtomicFailureTest,
	"UeAgentInterface.DomainAdapters.NiagaraCompound.Native.AtomicApplyFailureRollback",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FUeAgentNiagaraCompoundNativeAtomicFailureTest::RunTest(
	const FString& Parameters)
{
	const TSharedRef<FUeAgentResourceTracker, ESPMode::ThreadSafe>
		Resources = MakeTracker();
	const auto Clock = []() { return 100.0; };
	FUeAgentCancellationSource Cancellation(Clock);
	const FUeAgentOperationDeadline Deadline =
		FUeAgentOperationDeadline::FromTimeout(30.0, Clock);
	TArray<TSharedPtr<const FUeAgentCandidateBinding>> Bindings;
	FString FixtureError;
	if (!DiscoverTwoLiteralBindings(
			*Resources,
			Deadline,
			Cancellation.GetToken(),
			Bindings,
			FixtureError)
		|| Bindings.Num() != 2)
	{
		AddError(FixtureError.IsEmpty()
			? TEXT("atomic_fixture_bindings_invalid")
			: FixtureError);
		return false;
	}

	const TSharedRef<FUeAgentNiagaraCompoundNativeHook> NativeHook =
		MakeShared<FUeAgentNiagaraCompoundNativeHook>(
			TEXT("p5-005-owned-worker-generation"));
	const TSharedRef<FUeAgentNiagaraCompoundReadHook> ReadHook =
		MakeShared<FUeAgentNiagaraCompoundReadHook>(NativeHook);
	const TSharedRef<FUeAgentNiagaraCompoundEffectHook> EffectHook =
		MakeShared<FUeAgentNiagaraCompoundEffectHook>(NativeHook);
	TArray<TSharedPtr<const FUeAgentUeReadCapture>> Captures;
	for (const TSharedPtr<const FUeAgentCandidateBinding>& Binding :
		Bindings)
	{
		const FUeAgentStateReadRequest ReadRequest(
			*Binding,
			EUeAgentStatePlane::AuthoringMemory);
		TSharedPtr<const FUeAgentUeReadCapture> Capture;
		FUeAgentGenericReadError ReadError;
		if (!ReadHook->TryCapture(
				ReadRequest,
				*Resources,
				Deadline,
				Cancellation.GetToken(),
				Capture,
				ReadError)
			|| !Capture.IsValid()
			|| Capture->GetEntries().Num() != 1)
		{
			AddError(ReadError.Code + TEXT(":") + ReadError.Detail);
			return false;
		}
		Captures.Add(Capture);
	}

	const FUeAgentCanonicalValueRef FirstBefore =
		Captures[0]->GetEntries()[0].GetValue();
	const FUeAgentCanonicalValueRef FirstAfter =
		MakeChangedLiteral(*FirstBefore, *Resources);
	const FUeAgentCanonicalValueRef SecondBefore =
		Captures[1]->GetEntries()[0].GetValue();
	TArray<FPlannedChange> Changes;
	TArray<FImpact> Impacts;
	for (int32 Index = 0; Index < Bindings.Num(); ++Index)
	{
		const FUeAgentCanonicalIdentity& Identity =
			Bindings[Index]->GetSummary().GetIdentity();
		const FUeAgentCanonicalValueRef Before =
			Index == 0 ? FirstBefore : SecondBefore;
		const FUeAgentCanonicalValueRef After =
			Index == 0 ? FirstAfter : SecondBefore;
		Changes.Emplace(
			Identity,
			EChangeIntent::Update,
			Before,
			After,
			TArray<FString>{TEXT("uai:niagara:write")},
			TEXT("p5-005:atomic-failure"));
		Impacts.Emplace(
			Identity,
			Identity,
			TEXT("direct"),
			TEXT("p5-005:atomic-failure"));
	}
	const FPlanningPreflightPlanRef Plan =
		MakeShared<FPlanningPreflightPlan>(
			GetNiagaraCompoundOperationDescriptorIdentity(),
			TEXT("request:p5-005:atomic-failure"),
			TEXT("idempotency:p5-005:atomic-failure"),
			TEXT("agent:p5-005"),
			TArray<FString>{TEXT("uai:niagara:write")},
			Captures[0]->GetSnapshotId(),
			Captures[0]->GetRevision(),
			Captures[0]->GetEvidenceId(),
			FChangeSet(MoveTemp(Changes)),
			FImpactSet(MoveTemp(Impacts)),
			TArray<FPrecondition>(),
			TEXT(
				"sha256:"
				"9999999999999999999999999999999999999999999999999999999999999999"));
	const FExecutionMutationRequest Request(
		TEXT("operation:p5-005:atomic-failure"),
		TEXT("owned-worker:p5-005"),
		Plan,
		MakeExecutionContext(Plan->GetDescriptorIdentity()),
		Resources,
		Deadline,
		Cancellation.GetToken(),
		EExecutionThreadAffinity::GameThread,
		EExecutionReentrancyPolicy::RejectSameTarget);
	const FString FirstKey =
		Bindings[0]->GetSummary().GetIdentity().GetCanonicalKey();
	const FString SecondKey =
		Bindings[1]->GetSummary().GetIdentity().GetCanonicalKey();
	FUeAgentGenericEffectApplyResult Apply;
	FExecutionMutationError MutationError;
	TestFalse(
		TEXT("the intentionally missing second binding rejects the operation"),
		EffectHook->TryApply(
			Request,
			FLockFenceLease(
				TEXT("owned-worker:p5-005"),
				{FirstKey, SecondKey},
				1),
			{MakeNativeEffectBinding(*Bindings[0])},
			*Resources,
			Apply,
			MutationError));

	const FUeAgentStateReadRequest FirstReadRequest(
		*Bindings[0],
		EUeAgentStatePlane::AuthoringMemory);
	TSharedPtr<const FUeAgentUeReadCapture> RestoredCapture;
	FUeAgentGenericReadError ReadError;
	TestTrue(
		TEXT("the failed compound operation restores its earlier mutation"),
		ReadHook->TryCapture(
			FirstReadRequest,
			*Resources,
			Deadline,
			Cancellation.GetToken(),
			RestoredCapture,
			ReadError)
		&& RestoredCapture.IsValid()
		&& RestoredCapture->GetEntries().Num() == 1
		&& CompareValues(
			RestoredCapture->GetEntries()[0].GetValue(),
			FirstBefore,
			TEXT("/niagara_input"),
			*Resources)
			.IsEquivalent());
	return !HasAnyErrors();
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUeAgentNiagaraCompoundNativeDynamicMutationTest,
	"UeAgentInterface.DomainAdapters.NiagaraCompound.Native.DynamicInputMutationRollback",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FUeAgentNiagaraCompoundNativeDynamicMutationTest::RunTest(
	const FString& Parameters)
{
	const TSharedRef<FUeAgentResourceTracker, ESPMode::ThreadSafe>
		Resources = MakeTracker();
	const auto Clock = []() { return 100.0; };
	FUeAgentCancellationSource Cancellation(Clock);
	const FUeAgentOperationDeadline Deadline =
		FUeAgentOperationDeadline::FromTimeout(30.0, Clock);
	TSharedPtr<const FUeAgentCandidateBinding> Binding;
	FString FixtureError;
	if (!DiscoverNestedDynamicBinding(
			*Resources,
			Deadline,
			Cancellation.GetToken(),
			Binding,
			FixtureError)
		|| !Binding.IsValid())
	{
		AddError(FixtureError.IsEmpty()
			? TEXT("dynamic_fixture_binding_invalid")
			: FixtureError);
		return false;
	}

	const TSharedRef<FUeAgentNiagaraCompoundNativeHook> NativeHook =
		MakeShared<FUeAgentNiagaraCompoundNativeHook>(
			TEXT("p5-005-owned-worker-generation"));
	const TSharedRef<FUeAgentNiagaraCompoundReadHook> ReadHook =
		MakeShared<FUeAgentNiagaraCompoundReadHook>(NativeHook);
	const TSharedRef<FUeAgentNiagaraCompoundEffectHook> EffectHook =
		MakeShared<FUeAgentNiagaraCompoundEffectHook>(NativeHook);
	const FUeAgentStateReadRequest ReadRequest(
		*Binding,
		EUeAgentStatePlane::AuthoringMemory);
	TSharedPtr<const FUeAgentUeReadCapture> BeforeCapture;
	FUeAgentGenericReadError ReadError;
	if (!ReadHook->TryCapture(
			ReadRequest,
			*Resources,
			Deadline,
			Cancellation.GetToken(),
			BeforeCapture,
			ReadError)
		|| !BeforeCapture.IsValid()
		|| BeforeCapture->GetEntries().Num() != 1)
	{
		AddError(ReadError.Code + TEXT(":") + ReadError.Detail);
		return false;
	}
	const FUeAgentCanonicalValueRef Before =
		BeforeCapture->GetEntries()[0].GetValue();
	TestEqual(
		TEXT("fixture root is a real native dynamic input"),
		ReadStringField(*Before, TEXT("mode")),
		FString(TEXT("dynamic_input")));
	TArray<FUeAgentCanonicalValueRef> Work;
	Work.Add(Before);
	bool bNestedDynamicObserved = false;
	while (!Work.IsEmpty())
	{
		const FUeAgentCanonicalValueRef Current =
			Work.Pop(EAllowShrinking::No);
		const FUeAgentCanonicalValueRef* Inputs =
			FindField(*Current, TEXT("inputs"));
		if (!Inputs
			|| (*Inputs)->GetKind() != EUeAgentValueKind::Array)
		{
			continue;
		}
		for (const FUeAgentCanonicalValueRef& Child :
			(*Inputs)->GetArrayItems())
		{
			if (ReadStringField(*Child, TEXT("mode"))
				== TEXT("dynamic_input"))
			{
				bNestedDynamicObserved = true;
			}
			Work.Add(Child);
		}
	}
	if (!bNestedDynamicObserved)
	{
		AddError(TEXT("nested_dynamic_fixture_projection_missing"));
		return false;
	}

	const FPlanningPreflightPlanRef Plan =
		MakeSingleChangePlan(
			*Binding,
			*BeforeCapture,
			Before,
			Before,
			TEXT("native-dynamic-rebuild"));
	const FExecutionMutationRequest Request(
		TEXT("operation:p5-005:native-dynamic-rebuild"),
		TEXT("owned-worker:p5-005"),
		Plan,
		MakeExecutionContext(Plan->GetDescriptorIdentity()),
		Resources,
		Deadline,
		Cancellation.GetToken(),
		EExecutionThreadAffinity::GameThread,
		EExecutionReentrancyPolicy::RejectSameTarget);
	const FUeAgentGenericEffectBinding EffectBinding =
		MakeNativeEffectBinding(*Binding);
	const FLockFenceLease Lease(
		TEXT("owned-worker:p5-005"),
		{
			Binding->GetSummary()
				.GetIdentity()
				.GetCanonicalKey(),
		},
		1);
	FUeAgentGenericEffectApplyResult Apply;
	FExecutionMutationError MutationError;
	if (!EffectHook->TryApply(
			Request,
			Lease,
			{EffectBinding},
			*Resources,
			Apply,
			MutationError)
		|| !Apply.IsValid()
		|| !Apply.DidMutationStart())
	{
		AddError(
			MutationError.GetCode()
			+ TEXT(":")
			+ MutationError.GetDetail());
		return false;
	}

	TSharedPtr<const FUeAgentUeReadCapture> RebuiltCapture;
	if (!ReadHook->TryCapture(
			ReadRequest,
			*Resources,
			Deadline,
			Cancellation.GetToken(),
			RebuiltCapture,
			ReadError)
		|| !RebuiltCapture.IsValid()
		|| RebuiltCapture->GetEntries().Num() != 1)
	{
		AddError(ReadError.Code + TEXT(":") + ReadError.Detail);
		return false;
	}
	TestTrue(
		TEXT("fresh native readback preserves the complete nested tree"),
		CompareValues(
			RebuiltCapture->GetEntries()[0].GetValue(),
			Before,
			TEXT("/niagara_input"),
			*Resources)
			.IsEquivalent());
	TestTrue(
		TEXT(
			"semantically identical native reconstruction keeps the "
			"same state revision"),
		RebuiltCapture->GetRevision()
			== BeforeCapture->GetRevision());

	bool bRollback = false;
	TestTrue(
		TEXT("rollback restores the original nested dynamic input"),
		EffectHook->TryRollback(
			Request,
			Apply,
			*Resources,
			bRollback,
			MutationError));
	TSharedPtr<const FUeAgentUeReadCapture> RollbackCapture;
	TestTrue(
		TEXT("fresh read confirms recursive rollback"),
		ReadHook->TryCapture(
			ReadRequest,
			*Resources,
			Deadline,
			Cancellation.GetToken(),
			RollbackCapture,
			ReadError));
	TestTrue(
		TEXT("recursive rollback restores the exact original tree"),
		RollbackCapture.IsValid()
			&& CompareValues(
				RollbackCapture->GetEntries()[0].GetValue(),
				Before,
				TEXT("/niagara_input"),
				*Resources)
				.IsEquivalent());
	return !HasAnyErrors();
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUeAgentNiagaraCompoundNativeCompileSettleTest,
	"UeAgentInterface.DomainAdapters.NiagaraCompound.Native.CompileSettle",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FUeAgentNiagaraCompoundNativeCompileSettleTest::RunTest(
	const FString& Parameters)
{
	const TSharedRef<FUeAgentResourceTracker, ESPMode::ThreadSafe>
		Resources = MakeTracker();
	const auto Clock = []() { return 100.0; };
	FUeAgentCancellationSource Cancellation(Clock);
	const FUeAgentOperationDeadline Deadline =
		FUeAgentOperationDeadline::FromTimeout(30.0, Clock);
	TSharedPtr<const FUeAgentCandidateBinding> Binding;
	FString FixtureError;
	if (!DiscoverFirstLiteralBinding(
			*Resources,
			Deadline,
			Cancellation.GetToken(),
			Binding,
			FixtureError)
		|| !Binding.IsValid())
	{
		AddError(FixtureError.IsEmpty()
			? TEXT("settle_fixture_binding_invalid")
			: FixtureError);
		return false;
	}

	const TSharedRef<FUeAgentNiagaraCompoundNativeHook> NativeHook =
		MakeShared<FUeAgentNiagaraCompoundNativeHook>(
			TEXT("p5-005-owned-worker-generation"));
	const TSharedRef<FUeAgentNiagaraCompoundReadHook> ReadHook =
		MakeShared<FUeAgentNiagaraCompoundReadHook>(NativeHook);
	const TSharedRef<FUeAgentNiagaraCompoundEffectHook> EffectHook =
		MakeShared<FUeAgentNiagaraCompoundEffectHook>(NativeHook);
	const FUeAgentStateReadRequest ReadRequest(
		*Binding,
		EUeAgentStatePlane::AuthoringMemory);
	TSharedPtr<const FUeAgentUeReadCapture> BeforeCapture;
	FUeAgentGenericReadError ReadError;
	if (!ReadHook->TryCapture(
			ReadRequest,
			*Resources,
			Deadline,
			Cancellation.GetToken(),
			BeforeCapture,
			ReadError)
		|| !BeforeCapture.IsValid()
		|| BeforeCapture->GetEntries().Num() != 1)
	{
		AddError(ReadError.Code + TEXT(":") + ReadError.Detail);
		return false;
	}
	const FUeAgentCanonicalValueRef Before =
		BeforeCapture->GetEntries()[0].GetValue();
	const FUeAgentCanonicalValueRef After =
		MakeChangedLiteral(*Before, *Resources);
	const FPlanningPreflightPlanRef Plan =
		MakeSingleChangePlan(
			*Binding,
			*BeforeCapture,
			Before,
			After,
			TEXT("native-settle"));
	const FExecutionMutationRequest Request(
		TEXT("operation:p5-005:native-settle"),
		TEXT("owned-worker:p5-005"),
		Plan,
		MakeExecutionContext(Plan->GetDescriptorIdentity()),
		Resources,
		Deadline,
		Cancellation.GetToken(),
		EExecutionThreadAffinity::GameThread,
		EExecutionReentrancyPolicy::RejectSameTarget);
	const FUeAgentCanonicalIdentity& Identity =
		Binding->GetSummary().GetIdentity();
	FUeAgentGenericEffectApplyResult Apply;
	FExecutionMutationError MutationError;
	if (!EffectHook->TryApply(
			Request,
			FLockFenceLease(
				TEXT("owned-worker:p5-005"),
				{Identity.GetCanonicalKey()},
				1),
			{MakeNativeEffectBinding(*Binding)},
			*Resources,
			Apply,
			MutationError)
		|| !Apply.IsValid())
	{
		AddError(
			MutationError.GetCode()
			+ TEXT(":")
			+ MutationError.GetDetail());
		return false;
	}

	FBarrierResult SettleResult;
	TestTrue(
		TEXT("the concrete hook compiles and settles the mutated asset"),
		EffectHook->TryWaitForSettle(
			FBarrierRequest(
				Apply.GetSubjectGeneration(),
				Apply.GetRequiredBarrierSignals()),
			*Resources,
			SettleResult,
			MutationError));
	TestTrue(
		TEXT("settle receipt is tied to the applied generation"),
		SettleResult.IsValid()
			&& SettleResult.GetSubjectGeneration()
				== Apply.GetSubjectGeneration()
			&& SettleResult.IsSettled()
			&& !SettleResult.GetPostBarrierRevision().IsEmpty());

	bool bRollback = false;
	TestTrue(
		TEXT("settled mutation still rolls back through the same hook"),
		EffectHook->TryRollback(
			Request,
			Apply,
			*Resources,
			bRollback,
			MutationError));
	TSharedPtr<const FUeAgentUeReadCapture> RollbackCapture;
	TestTrue(
		TEXT("fresh read confirms rollback after settle"),
		ReadHook->TryCapture(
			ReadRequest,
			*Resources,
			Deadline,
			Cancellation.GetToken(),
			RollbackCapture,
			ReadError));
	TestTrue(
		TEXT("settle does not prevent exact rollback"),
		RollbackCapture.IsValid()
			&& CompareValues(
				RollbackCapture->GetEntries()[0].GetValue(),
				Before,
				TEXT("/niagara_input"),
				*Resources)
				.IsEquivalent());
	return !HasAnyErrors();
}
