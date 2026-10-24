#include "Misc/AutomationTest.h"

#include "Composition/GraphCompound/UeAgentGraphCompoundActivation.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Module/CommandRegistration/Active/UeAgentActiveCommandComposition.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"

namespace UE::UAI::Tests::StrictV2::T56
{
	const TArray<FString>& ExpectedCommands()
	{
		static const TArray<FString> Commands = {
			TEXT("animation.blend_space.asset.create"),
			TEXT("animation.blend_space.sample"),
			TEXT("animation.montage.create"),
			TEXT("animation.sequence.pose.sample"),
			TEXT("animation.sequence.root_motion.analyze"),
		};
		return Commands;
	}

	const TArray<FString>& ExpectedDormantCommands()
	{
		static const TArray<FString> Commands = {
			TEXT("animation.blend_space.definition.apply"),
			TEXT("animation.montage.read"),
			TEXT("animation.montage.structure.replace"),
			TEXT("animation.sequence.bone_tracks.replace"),
			TEXT("animation.sequence.curves.update"),
			TEXT("animation.sequence.events.update"),
			TEXT("animation.sequence.metadata.update"),
			TEXT("animation.sequence.read"),
		};
		return Commands;
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUeAgentStrictV2T56AnimationCurveCommandMigrationTest,
	"UAI.StrictV2.T56.ANIMATION.CURVE.COMMAND.MIGRATION.InventoryAndActivation",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FUeAgentStrictV2T56AnimationCurveCommandMigrationTest::RunTest(const FString& Parameters)
{
	using namespace UE::UAI;
	using namespace UE::UAI::Module::CommandRegistration::Active;
	using namespace UE::UAI::Tests::StrictV2::T56;
	TSharedPtr<const CommandRegistry::FUeAgentImmutableCommandRegistry> Registry;
	TSharedPtr<const CommandEdge::FUeAgentExactCommandDispatch> Dispatch;
	FString Error;
	if (!TestTrue(TEXT("the process active composition freezes"),
		TryComposeProcessActiveCommandComposition(Registry, Dispatch, Error))
		|| !Registry.IsValid())
	{
		AddError(Error);
		return false;
	}
	for (const FString& CommandId : ExpectedCommands())
	{
		const auto* Contract = Registry->FindExact(CommandId);
		TestNotNull(*FString::Printf(TEXT("%s is active"), *CommandId), Contract);
		if (Contract)
		{
			TestEqual(*FString::Printf(TEXT("%s is enabled"), *CommandId),
				Contract->GetAvailability(),
				CommandContract::EUeAgentCommandAvailability::Enabled);
			TestEqual(*FString::Printf(TEXT("%s has one T56 owner"), *CommandId),
				Contract->GetPublicOwner(), FString(TEXT("animation-curve-command-owner")));
			TestEqual(*FString::Printf(TEXT("%s uses the stable animation namespace"),
				*CommandId), Contract->GetPythonNamespace(),
				FString(TEXT("animation")));
			TestEqual(*FString::Printf(TEXT("%s uses the stable generated method"),
				*CommandId), Contract->GetPythonFunction(),
				CommandId.RightChop(10).Replace(TEXT("."), TEXT("_")));
		}
	}
	for (const FString& CommandId : ExpectedDormantCommands())
	{
		const auto* Contract = Registry->FindExact(CommandId);
		TestNotNull(*FString::Printf(TEXT("%s remains registered as dormant"), *CommandId), Contract);
		if (Contract)
		{
			TestEqual(*FString::Printf(TEXT("%s is dormant"), *CommandId),
				Contract->GetAvailability(),
				CommandContract::EUeAgentCommandAvailability::Dormant);
		}
	}

	TSharedPtr<const Contract::FUeAgentDescriptorRegistry> Descriptors;
	if (!TestTrue(
			TEXT("GraphCompound descriptor registry composes"),
			Composition::GraphCompound::TryCreateGraphCompoundDescriptorRegistry(
				Descriptors,
				Error))
		|| !Descriptors.IsValid())
	{
		AddError(Error);
		return false;
	}
	const Contract::FUeAgentOperationDescriptor* BlendSpaceCreate =
		Descriptors->GetDescriptors().FindByPredicate(
			[](const auto& Descriptor)
			{
				return Descriptor.GetCapabilityId()
					== TEXT("animation.blend_space.asset.create");
			});
	TestNotNull(
		TEXT("BlendSpace create exact descriptor exists"),
		BlendSpaceCreate);
	if (BlendSpaceCreate)
	{
		TSharedPtr<FJsonObject> ResultSchema;
		const TSharedRef<TJsonReader<>> Reader =
			TJsonReaderFactory<>::Create(
				BlendSpaceCreate->GetResultSchema().GetJsonValue());
		TestTrue(
			TEXT("BlendSpace create result schema parses"),
			FJsonSerializer::Deserialize(Reader, ResultSchema)
				&& ResultSchema.IsValid());
		if (ResultSchema.IsValid())
		{
			const TArray<TSharedPtr<FJsonValue>>& Required =
				ResultSchema->GetArrayField(TEXT("required"));
			TestTrue(
				TEXT("BlendSpace create result requires top-level target_ref"),
				Required.ContainsByPredicate(
					[](const TSharedPtr<FJsonValue>& Value)
					{
						return Value.IsValid()
							&& Value->AsString() == TEXT("target_ref");
					}));
		}
	}
	return !HasAnyErrors();
}
