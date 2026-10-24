#include "Misc/AutomationTest.h"

#include "Core/Contract/Command/UeAgentCommandLifecyclePolicy.h"
#include "Composition/SimpleAuthoring/UeAgentSimpleAuthoringComposition.h"
#include "DomainAdapters/AssetDataSimpleEffect/UeAgentAssetDataSimpleEffectCapabilities.h"
#include "DomainAdapters/AssetSimple/UeAgentAssetSimpleMechanicsBoundary.h"
#include "OperationOwners/AssetSimple/UeAgentAssetSimpleOperationOwner.h"

#include "Module/CommandRegistration/Active/UeAgentActiveCommandComposition.h"

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUeAgentStrictV2T37AssetSimpleInventoryTest,
	"UAI.StrictV2.T37.ASSET.SIMPLE.OPERATION.InventoryAndActivation",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FUeAgentStrictV2T37AssetSimpleInventoryTest::RunTest(
	const FString& Parameters)
{
	using namespace UE::UAI;
	using namespace UE::UAI::Module::CommandRegistration::Active;
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
	const TArray<Composition::SimpleAuthoring::
		FUeAgentSimpleAuthoringCommandRoute>& PublicRoutes =
		OperationOwners::AssetSimple::GetAssetSimplePublicOperationRoutes();
	TArray<FString> PublicIds;
	for (const auto& Route : PublicRoutes)
	{
		PublicIds.Add(Route.CommandId);
	}
	TestFalse(TEXT("public AssetSimple routes are non-empty"),
		PublicRoutes.IsEmpty());
	TestTrue(TEXT("generic asset creation is canonical public mechanics"),
		PublicIds.Contains(TEXT("asset.object.create")));
	for (const FString& CommandId : PublicIds)
	{
		const auto* Contract = Registry->FindExact(CommandId);
		TestNotNull(*FString::Printf(TEXT("%s remains registered"), *CommandId), Contract);
		if (Contract)
		{
			const CommandLifecycle::FCommandLifecyclePolicy* Policy =
				CommandLifecycle::FindExact(CommandId);
			const CommandContract::EUeAgentCommandAvailability ExpectedAvailability =
				Policy ? Policy->Availability
					: CommandContract::EUeAgentCommandAvailability::Enabled;
			TestEqual(
				*FString::Printf(TEXT("%s uses its centrally declared availability"), *CommandId),
				Contract->GetAvailability(),
				ExpectedAvailability);
			TestEqual(*FString::Printf(TEXT("%s has one T37 owner"), *CommandId),
				Contract->GetPublicOwner(),
				FString(TEXT("asset-simple-operation-owner")));
			if (CommandId.EndsWith(TEXT(".create")))
			{
				const auto& Fields = Contract->GetModelResultFields();
				TestEqual(TEXT("create has two compact model result fields"),
					Fields.Num(), 2);
				if (Fields.Num() == 2)
				{
					TestEqual(TEXT("create model status source"),
						Fields[0].SourcePath,
						TArray<FString>{TEXT("projection"), TEXT("payload"),
							TEXT("status")});
					TestEqual(TEXT("create model outcome source"),
						Fields[1].SourcePath,
						TArray<FString>{TEXT("projection"), TEXT("payload"),
							TEXT("effect_outcome")});
				}
			}
		}
	}
	const TArray<FString>& DelegatedIds = DomainAdapters::AssetSimple::
		GetAssetSimpleDelegatedLifecycleCommandIds();
	const TArray<FString>& LifecycleIds = DomainAdapters::AssetSimple::
		GetAssetSimpleLifecycleCommandIds();
	const TArray<FString>& MechanicsIds = DomainAdapters::AssetSimple::
		GetAssetSimpleMechanicsCommandIds();
	TestEqual(TEXT("one lifecycle route is delegated"),
		DelegatedIds, TArray<FString>{TEXT("asset.object.clone")});
	for (const FString& PublicId : PublicIds)
	{
		TestTrue(TEXT("every public identity has internal mechanics"),
			MechanicsIds.Contains(PublicId));
		TestTrue(TEXT("every public identity belongs to lifecycle"),
			LifecycleIds.Contains(PublicId));
		TestFalse(TEXT("public identity is not delegated"),
			DelegatedIds.Contains(PublicId));
	}
	for (const FString& DelegatedId : DelegatedIds)
	{
		TestFalse(TEXT("public and delegated identities are disjoint"),
			PublicIds.Contains(DelegatedId));
		TestTrue(TEXT("delegated identity belongs to lifecycle"),
			LifecycleIds.Contains(DelegatedId));
	}
	TArray<DomainAdapters::AssetDataSimpleEffect::
		FUeAgentAssetDataSimpleEffectProductionProfile> Profiles;
	Error.Reset();
	TestTrue(TEXT("production profiles resolve for successor projection"),
		DomainAdapters::AssetDataSimpleEffect::
			TryGetAssetDataSimpleEffectProductionProfiles(Profiles, Error));
	for (const auto& Profile : Profiles)
	{
		if (DomainAdapters::AssetDataSimpleEffect::
				IsGenericAssetObjectCreateSuccessor(Profile))
		{
			TestFalse(
				*FString::Printf(
					TEXT("%s has no duplicate typed public create façade"),
					*Profile.CapabilityId),
				PublicIds.Contains(Profile.CapabilityId));
			TestNull(
				*FString::Printf(
					TEXT("%s is absent from the active command registry"),
					*Profile.CapabilityId),
				Registry->FindExact(Profile.CapabilityId));
		}
	}
	TestTrue(TEXT("generic asset creation has a public route"),
		PublicRoutes.ContainsByPredicate([](const auto& Route)
		{
			return Route.CommandId == TEXT("asset.object.create");
		}));
	const auto* GenericCreate =
		Registry->FindExact(TEXT("asset.object.create"));
	TestNotNull(TEXT("generic asset creation is active"), GenericCreate);
	if (GenericCreate)
	{
		const auto* Factory = GenericCreate->GetInputFields().FindByPredicate(
			[](const auto& Field)
			{
				return Field.Name == TEXT("factory");
			});
		TestTrue(TEXT("generic create consumes a returned readable name"),
			Factory && Factory->WireType == TEXT("string"));
		TestTrue(
			TEXT("generic create points to the factory candidate query"),
			GenericCreate->GetPurpose().RelatedFlows.Contains(
				TEXT("asset.factory.candidates")));
		TestTrue(
			TEXT("generic create exposes the canonical object-path shape"),
			GenericCreate->GetPurpose().ParameterGuidance.Contains(
				TEXT("/Game/Folder/Asset.Asset")));
	}
	for (const FString& LifecycleId : LifecycleIds)
	{
		TestTrue(TEXT("every lifecycle identity has an internal route"),
			OperationOwners::AssetSimple::GetAssetSimpleOperationRoutes().
				ContainsByPredicate([&LifecycleId](const auto& Route)
				{
					return Route.CommandId == LifecycleId;
				}));
	}
	TestTrue(TEXT("full SimpleAuthoring routes contain delegated clone"),
		Composition::SimpleAuthoring::GetSimpleAuthoringCommandRoutes().
			ContainsByPredicate([](const auto& Route)
			{
				return Route.CommandId == TEXT("asset.object.clone");
			}));
	TestTrue(TEXT("delegated clone reuses the OperationHost factory"),
		OperationOwners::AssetSimple::FindOperationInvocationFactory(
			TEXT("asset.object.clone")).IsValid());
	const auto* Clone = Registry->FindExact(TEXT("asset.object.clone"));
	TestNotNull(TEXT("clone remains public through destructive owner"), Clone);
	if (Clone)
	{
		TestEqual(TEXT("clone public owner remains destructive"),
			Clone->GetPublicOwner(),
			FString(TEXT("destructive-content-owner")));
	}
	return !HasAnyErrors();
}
