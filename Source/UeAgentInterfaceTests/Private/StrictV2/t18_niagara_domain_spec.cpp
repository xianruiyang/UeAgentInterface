#include "Misc/AutomationTest.h"

#include "Core/HjsonWorkspace/ImportInitialization/UeAgentHjsonDomainProviderRegistry.h"
#include "Core/ContentDiscovery/UeAgentNameDiscoveryProjection.h"
#include "Module/CommandRegistration/Active/UeAgentActiveCommandComposition.h"
#include "NiagaraEmitter.h"
#include "NiagaraScript.h"
#include "NiagaraSystem.h"
#include "NiagaraSystemFactoryNew.h"

#include "OperationOwners/Niagara/UeAgentNiagaraDomainService.h"

#include "QueryOwners/Niagara/UeAgentNiagaraModuleCandidateQuery.h"

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUeAgentStrictV2T18NiagaraDomainTest,
	"UAI.StrictV2.T18.NIAGARA.DOMAIN",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FUeAgentStrictV2T18NiagaraDomainTest::RunTest(const FString& Parameters)
{
	(void)Parameters;
	using namespace UE::UAI::ContentDiscovery;
	{
		TArray<FScopedNameDiscoveryEntry> FullActionSet = {
			{TEXT("Multiply"), {TEXT("particle_update"), TEXT("asset_module")}},
			{TEXT("Multiply"), {TEXT("system_update"), TEXT("asset_module")}},
		};
		TArray<FString> Aliases;
		FString ScopeError;
		TestTrue(
			TEXT("duplicate Niagara names receive aliases from the complete action set"),
			TryBuildScopedDiscoveryNames(
				FullActionSet, ENameComparison::IgnoreCase, Aliases, ScopeError));
		if (Aliases.Num() == 2)
		{
			TestEqual(TEXT("filtering keeps the alias selected from the complete set"),
				Aliases[1], FString(TEXT("Multiply [system_update]")));
			FString FilteredPayload;
			TestTrue(
				TEXT("filtered Niagara results project the existing alias without recomputing it"),
				TryBuildNamesOnlyPayload(
					{Aliases[1]}, ENameComparison::IgnoreCase,
					FilteredPayload, ScopeError));
			TestTrue(TEXT("the filtered payload retains the scoped alias"),
				FilteredPayload.Contains(TEXT("system_update")));
		}
		TArray<FScopedNameDiscoveryEntry> FocusedEntries = {
			{TEXT("Blocked"), {TEXT("same_scope")}},
			{TEXT("Blocked"), {TEXT("same_scope")}},
			{TEXT("Unique"), {TEXT("particle_update"), TEXT("asset_module")}},
		};
		TestTrue(TEXT("a focused Niagara match ignores unrelated ambiguity"),
			TryBuildScopedDiscoveryNamesForIndices(
				FocusedEntries, {2}, ENameComparison::IgnoreCase, Aliases, ScopeError));
		TestEqual(TEXT("the focused Niagara name stays readable"),
			Aliases.Num() == 1 ? Aliases[0] : FString(), FString(TEXT("Unique")));
		TestFalse(TEXT("a matched Niagara ambiguity still fails closed"),
			TryBuildScopedDiscoveryNamesForIndices(
				FocusedEntries, {0}, ENameComparison::IgnoreCase, Aliases, ScopeError));
		TestEqual(TEXT("the Niagara ambiguity error is typed"), ScopeError,
			FString(TEXT("name_discovery_semantic_scope_ambiguous")));
	}
	using namespace UE::UAI::HjsonWorkspace;
	using namespace UE::UAI::OperationOwners::Niagara;

	TSharedPtr<IImportInitializationDomainProvider> Provider;
	FWorkspaceError Error;
	if (!TestTrue(
		TEXT("the Niagara domain provider is created"),
		TryCreateNiagaraDomainProvider(Provider, Error))
		|| !Provider.IsValid())
	{
		AddError(Error.GetCode());
		return false;
	}

	struct FCase
	{
		const TCHAR* Type;
		EInitializationNodeRole Role;
		const TCHAR* Contract;
	};
	const TArray<FCase> Cases = {
		{TEXT("/Script/Niagara.NiagaraSystem"), EInitializationNodeRole::RootTarget, TEXT("uai.init.niagara_system.root/1")},
		{TEXT("/Script/Niagara.NiagaraEmitter"), EInitializationNodeRole::RootTarget, TEXT("uai.init.niagara_emitter.root/1")},
		{TEXT("/Script/Niagara.NiagaraScript"), EInitializationNodeRole::RootTarget, TEXT("uai.init.niagara_script.root/1")},
		{TEXT("uai.niagara.stack"), EInitializationNodeRole::ContainerItem, TEXT("uai.init.niagara.stack/1")},
		{TEXT("/Script/NiagaraEditor.NiagaraNodeFunctionCall"), EInitializationNodeRole::OwnedObject, TEXT("uai.init.niagara.module/1")},
		{TEXT("uai.niagara.input"), EInitializationNodeRole::ContainerItem, TEXT("uai.init.niagara.input/1")},
		{TEXT("uai.niagara.parameter"), EInitializationNodeRole::ContainerItem, TEXT("uai.init.niagara.parameter/1")},
	};
	for (const FCase& Case : Cases)
	{
		const FImportInitializationDomainRequest Request(
			TEXT("content_graph.import_workspace"),
			TEXT("uai.niagara.edit/1"),
			TEXT("/Game/UAI/StrictV2/T18/Target.Target"),
			Case.Type,
			Case.Role,
			TEXT("{}"));
		FString Contract;
		TestTrue(
			*FString::Printf(TEXT("the Niagara initialization surface resolves: %s"), Case.Type),
			Provider->Matches(Request)
				&& Provider->TryResolveInitializationContract(Request, Contract, Error));
		TestEqual(TEXT("the Niagara initialization contract is exact"), Contract,
			FString(Case.Contract));
	}

	const FImportInitializationDomainRequest Unsupported(
		TEXT("content_graph.import_workspace"),
		TEXT("uai.niagara.edit/1"),
		TEXT("/Game/UAI/StrictV2/T18/Unsupported.Unsupported"),
		TEXT("/Script/Engine.SoundWave"),
		EInitializationNodeRole::RootTarget,
		TEXT("{}"));
	TestFalse(TEXT("an unregistered Niagara root is rejected before write"),
		Provider->Matches(Unsupported));

	TSharedPtr<const UE::UAI::CommandRegistry::
		FUeAgentImmutableCommandRegistry> Registry;
	TSharedPtr<const UE::UAI::CommandEdge::FUeAgentExactCommandDispatch>
		Dispatch;
	FString CompositionError;
	if (!TestTrue(TEXT("the process active composition freezes"),
		UE::UAI::Module::CommandRegistration::Active::
			TryComposeProcessActiveCommandComposition(
				Registry, Dispatch, CompositionError))
		|| !Registry.IsValid())
	{
		AddError(CompositionError);
		return false;
	}
	const auto* const ImportContract =
		Registry->FindExact(TEXT("content_graph.import_workspace"));
	if (TestNotNull(TEXT("the shared HJSON ingress remains active"),
		ImportContract))
	{
		const auto* const ExpectedRevisionField =
			ImportContract->GetInputFields().FindByPredicate(
				[](const auto& Field)
				{
					return Field.Name == TEXT("expected_revision");
				});
		TestNull(
			TEXT("the shared HJSON ingress no longer asks the model for expected_revision"),
			ExpectedRevisionField);
	}
	TestNull(TEXT("the replaced Niagara script HJSON command is inactive"),
		Registry->FindExact(TEXT("niagara.script.graph.apply")));
	TestNull(TEXT("the replaced Niagara stack HJSON command is inactive"),
		Registry->FindExact(TEXT("niagara.stack.apply")));

	using namespace UE::UAI::QueryOwners::Niagara;
	UNiagaraSystem* CandidateTarget = NewObject<UNiagaraSystem>(GetTransientPackage());
	UNiagaraSystemFactoryNew::InitializeSystem(CandidateTarget, true);
	FNiagaraModuleCandidateContext CandidateContext;
	CandidateContext.TargetUsage = TEXT("SystemUpdateScript");
	CandidateContext.TargetUsageId = CandidateTarget->GetSystemUpdateScript()
		->GetUsageId().ToString(EGuidFormats::DigitsWithHyphensLower);
	FNiagaraModuleCandidateQueryResult CandidateResult;
	FNiagaraModuleCandidateQueryError CandidateError;
	if (!TestTrue(
		TEXT("the Niagara owner resolves real module candidates"),
		TryQueryNiagaraModuleCandidates(
			*CandidateTarget,
			FString(),
			CandidateContext,
			CandidateResult,
			CandidateError)))
	{
		AddError(CandidateError.Code + TEXT(":") + CandidateError.PublicMessage);
		return false;
	}
	if (!TestTrue(TEXT("the real Niagara module catalog is non-empty"),
		!CandidateResult.Candidates.IsEmpty()))
	{
		return false;
	}
	const FNiagaraModuleCandidate* Candidate = nullptr;
	for (const FNiagaraModuleCandidate& CandidateEntry : CandidateResult.Candidates)
	{
		if (CandidateEntry.ActionKind == TEXT("asset_module") && CandidateEntry.bSupported)
		{
			Candidate = &CandidateEntry;
			break;
		}
	}
	if (!TestNotNull(TEXT("the native action page includes a supported asset module"), Candidate))
	{
		return false;
	}
	TestEqual(TEXT("candidate uses the readable native action name"),
		Candidate->NativeCandidateName, Candidate->Name);
	TestFalse(TEXT("candidate.name is the native display name, not an asset path"),
		Candidate->Name.StartsWith(TEXT("/")));
	TestEqual(TEXT("candidate kind is explicit"), Candidate->Kind,
		FString(TEXT("niagara_module")));
	TestEqual(TEXT("candidate action kind is explicit"), Candidate->ActionKind,
		FString(TEXT("asset_module")));
	TestTrue(TEXT("candidate is supported in this exact output context"),
		Candidate->bSupported);
	TestTrue(TEXT("candidate declares create support"),
		Candidate->SupportedOperations.Contains(TEXT("create")));
	TestTrue(TEXT("the native script asset remains server-side"),
		Candidate->ScriptAssetPath.StartsWith(TEXT("/"))
			&& Candidate->ScriptAssetPath.Contains(TEXT(".")));
	TestEqual(TEXT("the query resolves the exact real output usage"),
		CandidateResult.Context.TargetUsage,
		FString(TEXT("SystemUpdateScript")));
	TestEqual(TEXT("the query resolves the real usage id"),
		CandidateResult.Context.TargetUsageId,
		CandidateContext.TargetUsageId);
	FNiagaraModuleCandidate ResolvedCandidate;
	FNiagaraModuleCandidateContext ResolvedContext;
	FNiagaraModuleCandidateQueryError ResolveError;
	TestTrue(
		TEXT("the readable name re-resolves the same native action in the same output context"),
		TryResolveNiagaraModuleCandidate(
			*CandidateTarget,
			Candidate->Name,
			CandidateContext,
			ResolvedCandidate,
			ResolvedContext,
			ResolveError));
	if (!ResolveError.Code.IsEmpty())
	{
		AddError(ResolveError.Code + TEXT(":") + ResolveError.PublicMessage);
	}
	TestEqual(TEXT("name resolution preserves the server-side asset action"),
		ResolvedCandidate.ScriptAssetPath, Candidate->ScriptAssetPath);
	return !HasAnyErrors();
}
