#include "Misc/AutomationTest.h"

#include "DomainAdapters/NiagaraCompound/UeAgentNiagaraCompoundNativeHook.h"

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUeAgentNiagaraCompoundNativeHookModuleAvailableTest,
	"UeAgentInterface.DomainAdapters.NiagaraCompound.NativeHook.ModuleAvailable",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FUeAgentNiagaraCompoundNativeHookModuleAvailableTest::RunTest(
	const FString& Parameters)
{
	using namespace UE::UAI::DomainAdapters::NiagaraCompound;

	TestFalse(
		TEXT("catalog kind is stable and non-empty"),
		GetNiagaraCompoundCatalogKind().IsEmpty());
	TestFalse(
		TEXT("catalog source has one stable owner"),
		GetNiagaraCompoundCatalogSourceId().IsEmpty());
	TestFalse(
		TEXT("read port has one stable owner"),
		GetNiagaraCompoundReadPortId().IsEmpty());
	TestEqual(
		TEXT("operation descriptor is the P5 compound descriptor"),
		GetNiagaraCompoundOperationDescriptorIdentity().GetDescriptorId(),
		FString(TEXT("uai.operation.niagara.compound")));

	const TSharedRef<
		FUeAgentNiagaraCompoundNativeHook,
		ESPMode::ThreadSafe> Hook =
		MakeShared<
			FUeAgentNiagaraCompoundNativeHook,
			ESPMode::ThreadSafe>(TEXT("native-hook-test-generation"));
	TestTrue(
		TEXT("the concrete hook is constructible without legacy providers"),
		Hook->IsAvailable());
	return !HasAnyErrors();
}
