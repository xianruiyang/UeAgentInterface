#include "Misc/AutomationTest.h"

#if __has_include("OperationOwners/DataTableCurveTable/UeAgentDataTableCurveTableDomainService.h")
#define UAI_T27_DATATABLE_CURVETABLE_DOMAIN_AVAILABLE 1
#include "Core/HjsonWorkspace/ImportInitialization/UeAgentHjsonDomainProviderRegistry.h"
#include "OperationOwners/DataTableCurveTable/UeAgentDataTableCurveTableDomainService.h"
#else
#define UAI_T27_DATATABLE_CURVETABLE_DOMAIN_AVAILABLE 0
#endif

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUeAgentStrictV2T27DataTableCurveTableProviderTest,
	"UAI.StrictV2.T27.DATATABLE.CURVETABLE.DOMAIN.ProviderContract",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FUeAgentStrictV2T27DataTableCurveTableProviderTest::RunTest(
	const FString& Parameters)
{
#if !UAI_T27_DATATABLE_CURVETABLE_DOMAIN_AVAILABLE
	TestTrue(TEXT("T27 DataTable/CurveTable domain provider is available"), false);
	return false;
#else
	using namespace UE::UAI::HjsonWorkspace;
	FWorkspaceError Error;
	TSharedPtr<const FImportInitializationDomainProviderRegistry> Registry;
	if (!TestTrue(
		TEXT("the process provider registry freezes with T27"),
		TryFreezeProcessImportInitializationDomainProviders(Registry, Error))
		|| !Registry.IsValid())
	{
		AddError(Error.GetCode());
		return false;
	}
	struct FCase
	{
		const TCHAR* Type;
		const TCHAR* Contract;
	};
	const FCase Cases[] = {
		{TEXT("/Script/Engine.DataTable"),
			TEXT("uai.init.datatable_curvetable.data_table.root/1")},
		{TEXT("/Script/Engine.CurveTable"),
			TEXT("uai.init.datatable_curvetable.curve_table.root/1")},
	};
	for (const FCase& Case : Cases)
	{
		FImportInitializationDomainResolution Resolution;
		TestTrue(
			*FString::Printf(TEXT("%s resolves the T27 provider"), Case.Type),
			Registry->TryResolve(
				FImportInitializationDomainRequest(
					TEXT("content_graph.import_workspace"),
					TEXT("uai.datatable_curvetable.edit/1"),
					TEXT("/Game/UAI/StrictV2/T27/Fixture.Fixture"),
					Case.Type,
					EInitializationNodeRole::RootTarget,
					TEXT("{}")),
				Resolution,
				Error));
		TestTrue(TEXT("the T27 resolution is valid"), Resolution.IsValid());
		if (Resolution.IsValid())
		{
			TestEqual(
				TEXT("the DataTable/CurveTable provider identity is unique"),
				Resolution.GetProvider()->GetProviderId(),
				FString(TEXT("datatable-curvetable-domain-provider")));
			TestEqual(
				TEXT("the root initialization contract is provider-owned"),
				Resolution.GetInitializationContractId(),
				FString(Case.Contract));
		}
	}
	return !HasAnyErrors();
#endif
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUeAgentStrictV2T27DataTableCurveTableRejectsUnknownTypeTest,
	"UAI.StrictV2.T27.DATATABLE.CURVETABLE.DOMAIN.UnknownTypePrewriteFailure",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FUeAgentStrictV2T27DataTableCurveTableRejectsUnknownTypeTest::RunTest(
	const FString& Parameters)
{
#if !UAI_T27_DATATABLE_CURVETABLE_DOMAIN_AVAILABLE
	TestTrue(TEXT("T27 DataTable/CurveTable prewrite gate is available"), false);
	return false;
#else
	using namespace UE::UAI::HjsonWorkspace;
	FWorkspaceError Error;
	TSharedPtr<const FImportInitializationDomainProviderRegistry> Registry;
	if (!TryFreezeProcessImportInitializationDomainProviders(Registry, Error)
		|| !Registry.IsValid())
	{
		AddError(Error.GetCode());
		return false;
	}
	FImportInitializationDomainResolution Resolution;
	TestFalse(
		TEXT("an unregistered table-like root fails before execution"),
		Registry->TryResolve(
			FImportInitializationDomainRequest(
				TEXT("content_graph.import_workspace"),
				TEXT("uai.datatable_curvetable.edit/1"),
				TEXT("/Game/UAI/StrictV2/T27/Unknown.Unknown"),
				TEXT("/Script/CoreUObject.Object"),
				EInitializationNodeRole::RootTarget,
				TEXT("{}")),
			Resolution,
			Error));
	TestFalse(TEXT("the rejected resolution stays invalid"), Resolution.IsValid());
	return !HasAnyErrors();
#endif
}
