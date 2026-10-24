#include "Misc/AutomationTest.h"

#include "Composition/GraphCompound/UeAgentGraphCompoundActivation.h"
#include "Core/Contract/UeAgentContractJson.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Module/CommandRegistration/Active/UeAgentActiveCommandComposition.h"

namespace UE::UAI::Tests::StrictV2::T207
{
	bool IsAssetCreate(const FString& CommandId)
	{
		return CommandId.EndsWith(TEXT(".asset.create"))
			|| CommandId == TEXT("pcg.data_asset.create_from_level");
	}

	bool HasRequiredTargetRef(
		const Contract::FUeAgentOperationDescriptor& Descriptor,
		FString& OutError)
	{
		TSharedPtr<FJsonObject> Schema;
		if (!Contract::Json::TryParseObject(
				Descriptor.GetResultSchema().GetJsonValue(), Schema, OutError)
			|| !Schema.IsValid())
		{
			return false;
		}
		const TSharedPtr<FJsonObject>* Properties = nullptr;
		const TArray<TSharedPtr<FJsonValue>>* Required = nullptr;
		if (!Schema->TryGetObjectField(TEXT("properties"), Properties)
			|| !Properties || !Properties->IsValid()
			|| !(*Properties)->HasField(TEXT("target_ref"))
			|| !Schema->TryGetArrayField(TEXT("required"), Required)
			|| !Required)
		{
			OutError = TEXT("graph_compound_create_target_ref_schema_missing");
			return false;
		}
		for (const TSharedPtr<FJsonValue>& Value : *Required)
		{
			FString Name;
			if (Value.IsValid() && Value->TryGetString(Name)
				&& Name == TEXT("target_ref"))
			{
				return true;
			}
		}
		OutError = TEXT("graph_compound_create_target_ref_not_required");
		return false;
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUeAgentStrictV2T207GraphCompoundCreateModelResultTest,
	"UAI.StrictV2.T207.GRAPH_COMPOUND.CREATE.ModelResultProjection",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FUeAgentStrictV2T207GraphCompoundCreateModelResultTest::RunTest(
	const FString& Parameters)
{
	using namespace UE::UAI;
	using namespace UE::UAI::Module::CommandRegistration::Active;
	(void)Parameters;

	TSharedPtr<const CommandRegistry::FUeAgentImmutableCommandRegistry> Registry;
	TSharedPtr<const CommandEdge::FUeAgentExactCommandDispatch> Dispatch;
	TSharedPtr<const Contract::FUeAgentDescriptorRegistry> Descriptors;
	FString Error;
	if (!TestTrue(TEXT("the process active composition freezes"),
			TryComposeProcessActiveCommandComposition(Registry, Dispatch, Error))
		|| !Registry.IsValid()
		|| !TestTrue(TEXT("the GraphCompound descriptor registry freezes"),
			Composition::GraphCompound::TryCreateGraphCompoundDescriptorRegistry(
				Descriptors, Error))
		|| !Descriptors.IsValid())
	{
		AddError(Error);
		return false;
	}

	const TSet<FString> GraphCompoundCreateOwners = {
		TEXT("ai-command-owner"),
		TEXT("animation-blueprint-command-owner"),
		TEXT("animation-rig-command-owner"),
		TEXT("audio-media-command-owner"),
		TEXT("blueprint-command-owner"),
		TEXT("material-command-owner"),
		TEXT("niagara-command-owner"),
		TEXT("pcg-command-owner"),
		TEXT("physics-deformer-operation-owner"),
		TEXT("umg-command-owner"),
	};
	TSet<FString> CoveredOwners;
	bool bCoveredPcgDataAssetFromLevel = false;
	const TArray<Composition::GraphCompound::FUeAgentGraphCompoundActivationProfile>* CachedGraphProfiles = nullptr;
	Contract::FDiagnostic ProfileError;
	if (!Composition::GraphCompound::TryGetGraphCompoundActivationProfiles(CachedGraphProfiles, ProfileError))
	{
		AddError(ProfileError.PublicMessage);
		return false;
	}
	for (const CommandContract::FUeAgentFrozenCommandContract& Command
		: Registry->GetContracts())
	{
		if (Command.GetAvailability()
				!= CommandContract::EUeAgentCommandAvailability::Enabled
			|| !UE::UAI::Tests::StrictV2::T207::IsAssetCreate(
				Command.GetCommandId())
			|| !GraphCompoundCreateOwners.Contains(Command.GetPublicOwner()))
		{
			continue;
		}
		CoveredOwners.Add(Command.GetPublicOwner());
		bCoveredPcgDataAssetFromLevel = bCoveredPcgDataAssetFromLevel
			|| Command.GetCommandId()
				== TEXT("pcg.data_asset.create_from_level");
		const TArray<CommandContract::FUeAgentModelResultFieldDeclaration>& Fields =
			Command.GetModelResultFields();
		TestEqual(*FString::Printf(TEXT("%s exposes one short field"),
			*Command.GetCommandId()), Fields.Num(), 1);
		if (Fields.Num() == 1)
		{
			TestEqual(*FString::Printf(TEXT("%s exposes target_ref"),
				*Command.GetCommandId()), Fields[0].Name, FString(TEXT("target_ref")));
			TestTrue(*FString::Printf(TEXT("%s reads the top-level target_ref"),
				*Command.GetCommandId()),
				Fields[0].SourcePath == TArray<FString>{TEXT("target_ref")});
		}

		const auto* Profile = CachedGraphProfiles->FindByPredicate(
				[&Command](const auto& Candidate)
				{
					return Candidate.GetCommandId() == Command.GetCommandId();
				});
		TestNotNull(*FString::Printf(TEXT("%s has a GraphCompound profile"),
			*Command.GetCommandId()), Profile);
		if (Profile)
		{
			const Contract::FUeAgentOperationDescriptor* Descriptor =
				Descriptors->FindExact(Profile->GetDescriptorIdentity());
			TestNotNull(*FString::Printf(TEXT("%s has an exact descriptor"),
				*Command.GetCommandId()), Descriptor);
			if (Descriptor)
			{
				FString SchemaError;
				TestTrue(*FString::Printf(
					TEXT("%s result schema requires top-level target_ref"),
					*Command.GetCommandId()),
					UE::UAI::Tests::StrictV2::T207::HasRequiredTargetRef(
						*Descriptor, SchemaError));
				if (!SchemaError.IsEmpty()) AddError(SchemaError);
			}
		}
	}

	for (const FString& Owner : GraphCompoundCreateOwners)
	{
		TestTrue(*FString::Printf(TEXT("%s has an enabled asset create"), *Owner),
			CoveredOwners.Contains(Owner));
	}
	TestTrue(TEXT("PCG data asset create-from-level is covered as create semantics"),
		bCoveredPcgDataAssetFromLevel);
	for (const TPair<FString, CommandContract::EUeAgentCommandAvailability>& Pair : {
		TPair<FString, CommandContract::EUeAgentCommandAvailability>(
			TEXT("animation.anim_blueprint.asset.create"),
			CommandContract::EUeAgentCommandAvailability::Dormant),
		TPair<FString, CommandContract::EUeAgentCommandAvailability>(
			TEXT("animation.deformer_source_library.asset.create"),
			CommandContract::EUeAgentCommandAvailability::Disabled),
		TPair<FString, CommandContract::EUeAgentCommandAvailability>(
			TEXT("animation.ml_deformer.asset.create"),
			CommandContract::EUeAgentCommandAvailability::Disabled)})
	{
		const auto* Command = Registry->FindExact(Pair.Key);
		TestNotNull(*FString::Printf(TEXT("%s remains registered"), *Pair.Key),
			Command);
		if (Command)
		{
			TestEqual(*FString::Printf(TEXT("%s preserves lifecycle state"), *Pair.Key),
				Command->GetAvailability(), Pair.Value);
		}
	}
	return !HasAnyErrors();
}
