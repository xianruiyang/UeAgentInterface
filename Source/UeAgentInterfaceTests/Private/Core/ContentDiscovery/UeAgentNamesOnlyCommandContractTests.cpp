#include "Misc/AutomationTest.h"

#include "Core/DomainCapability/UeAgentDomainCapability.h"

namespace
{
	using namespace UE::UAI;

	const TArray<FString>& NamesOnlyCommands()
	{
		static const TArray<FString> Commands = {
			TEXT("query.content.search"),
			TEXT("asset.factory.candidates"),
			TEXT("blueprint.component.candidates"),
			TEXT("blueprint.node.candidates"),
			TEXT("animation.state_machine.candidates"),
			TEXT("audio.graph.candidates"),
			TEXT("animation_rig_graph.candidates"),
			TEXT("ai.behavior.candidates"),
			TEXT("material.expression.candidates"),
			TEXT("niagara.module.candidates"),
			TEXT("pcg.node.candidates"),
			TEXT("umg.widget.candidates"),
			TEXT("world.actor.candidates"),
		};
		return Commands;
	}

	bool IsForbiddenModelField(const FString& FieldName)
	{
		const FString Name = FieldName.ToLower();
		return Name == TEXT("id")
			|| Name == TEXT("cursor")
			|| Name == TEXT("candidate")
			|| Name == TEXT("candidate_id")
			|| Name == TEXT("graph_id")
			|| Name == TEXT("node_id")
			|| Name == TEXT("pin_id")
			|| Name == TEXT("factory")
			|| Name == TEXT("factory_ref")
			|| Name == TEXT("class_path")
			|| Name.EndsWith(TEXT("_id"))
			|| Name.Contains(TEXT("guid"))
			|| Name.Contains(TEXT("hash"))
			|| Name.Contains(TEXT("digest"))
			|| Name.Contains(TEXT("fingerprint"))
			|| Name.Contains(TEXT("revision"));
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUeAgentNamesOnlyCommandContractTest,
	"UeAgentInterface.Core.ContentDiscovery.NamesOnly.CommandContracts",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FUeAgentNamesOnlyCommandContractTest::RunTest(const FString& Parameters)
{
	DomainCapability::FDomainCapabilityAuthorities Authorities;
	FString Error;
	if (!TestTrue(
			TEXT("process command authority composes"),
			DomainCapability::TryGetOrBuildProcessDomainCapabilityAuthorities(
				Authorities, Error))
		|| !TestTrue(
			TEXT("process command registry is available"),
			Authorities.GetCommandRegistry().IsValid()))
	{
		AddError(Error);
		return false;
	}

	for (const FString& CommandId : NamesOnlyCommands())
	{
		const CommandContract::FUeAgentFrozenCommandContract* Contract =
			Authorities.GetCommandRegistry()->FindExact(CommandId);
		if (!TestNotNull(TEXT("names-only command exists: ") + CommandId,
				Contract))
		{
			continue;
		}
		const TArray<CommandContract::FUeAgentCommandFieldDeclaration>& Results =
			Contract->GetResultFields();
		const auto* Names = Results.FindByPredicate([](const auto& Field)
		{
			return Field.Name == TEXT("names");
		});
		if (TestNotNull(TEXT("success result retains names: ") + CommandId, Names))
		{
			TestEqual(
				TEXT("names is an array: ") + CommandId,
				Names->WireType,
				FString(TEXT("array")));
		}
		for (const auto& Field : Results)
		{
			TestTrue(TEXT("candidate response field is public: ") + CommandId + TEXT(".") + Field.Name,
				Field.Name == TEXT("names") || Field.Name == TEXT("scope")
				|| Field.Name == TEXT("candidate_context") || Field.Name == TEXT("placement_context")
				|| Field.Name == TEXT("description"));
		}
		for (const CommandContract::FUeAgentCommandFieldDeclaration& Field :
			Contract->GetInputFields())
		{
			TestFalse(
				TEXT("model input has no machine selector: ")
					+ CommandId + TEXT(".") + Field.Name,
				IsForbiddenModelField(Field.Name));
		}
	}
	return !HasAnyErrors();
}
