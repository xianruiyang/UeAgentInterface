#include "Misc/AutomationTest.h"

#include "DomainAdapters/NiagaraCommands/UeAgentNiagaraCommandMechanicsBoundary.h"

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUeAgentStrictV2T51NiagaraHjsonRuntimeTest,
	"UAI.StrictV2.T51.NIAGARA.HJSON.RUNTIME",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FUeAgentStrictV2T51NiagaraHjsonRuntimeTest::RunTest(const FString& Parameters)
{
	(void)Parameters;
	using namespace UE::UAI::DomainAdapters::NiagaraCommands;
	const TArray<FString>& CommandIds = GetCommandIds();
	TestFalse(TEXT("the script HJSON route is not a T51 command"),
		CommandIds.Contains(TEXT("niagara.script.graph.apply")));
	TestFalse(TEXT("the stack HJSON route is not a T51 command"),
		CommandIds.Contains(TEXT("niagara.stack.apply")));
	TArray<UE::UAI::Composition::GraphCompound::
		FUeAgentGraphCompoundActivationProfile> Profiles;
	FString Error;
	if (!TestTrue(TEXT("the eight Niagara mechanics profiles resolve"),
		TryGetActivationProfiles(Profiles, Error)))
	{
		AddError(Error);
		return false;
	}
	for (const auto& Profile : Profiles)
	{
		TestFalse(TEXT("T51 profiles require no HJSON import authority"),
			RequiresHjsonImportAuthority(Profile.GetCommandId()));
	}
	return !HasAnyErrors();
}
