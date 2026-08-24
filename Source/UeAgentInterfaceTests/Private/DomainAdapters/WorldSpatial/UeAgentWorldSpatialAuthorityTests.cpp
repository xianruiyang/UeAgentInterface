#include "Misc/AutomationTest.h"

#include "Composition/TimelineSpatial/UeAgentTimelineSpatialMechanicsHost.h"
#include "Core/DomainCapability/UeAgentDomainCapability.h"
#include "OperationOwners/SpatialContent/UeAgentSpatialContentOperationOwner.h"
#include "OperationOwners/ViewportGraph/UeAgentViewportGraphOperationOwner.h"
#include "QueryOwners/SpatialContent/UeAgentSpatialContentQueryOwner.h"

namespace
{
	const TArray<FString>& SpatialOperationIds()
	{
		static const TArray<FString> Values = {
			TEXT("mesh.element.select_by_ray"),
			TEXT("mesh.pivot.update"),
			TEXT("mesh.transform.bake"),
			TEXT("world.object_align"),
			TEXT("world.object_snap_to_surface"),
			TEXT("world.object_transform.update"),
		};
		return Values;
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUeAgentWorldSpatialDomainCapabilityAuthorityTest,
	"UeAgentInterface.DomainAdapters.WorldSpatial.E0.Authority."
		"SpatialOperationFacet",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FUeAgentWorldSpatialDomainCapabilityAuthorityTest::RunTest(
	const FString& Parameters)
{
	using namespace UE::UAI::DomainCapability;

	FString Error;
	TSharedPtr<const FFrozenDomainCapabilitySet> Frozen;
	if (!TestTrue(
			TEXT("the process domain capability set freezes"),
			TryFreezeProcessDomainCapabilities(Frozen, Error))
		|| !Frozen.IsValid())
	{
		AddError(Error);
		return false;
	}

	const TSharedRef<const FDomainCapabilityContribution>* Spatial =
		Frozen->GetContributions().FindByPredicate(
			[](const TSharedRef<const FDomainCapabilityContribution>& Item)
			{
				return Item->GetDomainId()
					== TEXT("spatial-content-operation");
			});
	TestNotNull(
		TEXT("spatial content owns one frozen domain contribution"),
		Spatial);
	if (!Spatial)
	{
		return false;
	}

	const FDomainCapabilityDeclaration& Declaration =
		(*Spatial)->GetDeclaration();
	TestEqual(
		TEXT("spatial content contributes one public command owner"),
		Declaration.Command.Contributions.Num(),
		1);
	TestEqual(
		TEXT("spatial content publishes the operation facet"),
		Declaration.Operation.Support,
		EDomainCapabilitySupport::Supported);
	TestEqual(
		TEXT("spatial content declares exactly six mutation operations"),
		Declaration.Operation.Operations.Num(),
		SpatialOperationIds().Num());

	for (const FString& OperationId : SpatialOperationIds())
	{
		const FDomainOperationEntry* Operation =
			Declaration.Operation.Operations.FindByPredicate(
				[&OperationId](const FDomainOperationEntry& Item)
				{
					return Item.OperationId == OperationId;
				});
		TestNotNull(
			*FString::Printf(
				TEXT("%s has one typed operation"),
				*OperationId),
			Operation);
		if (!Operation)
		{
			continue;
		}
		TestEqual(
			*FString::Printf(TEXT("%s is immediate"), *OperationId),
			Operation->ExecutionPolicy,
			EDomainOperationExecutionPolicy::Immediate);
		TestEqual(
			*FString::Printf(
				TEXT("%s has the correct transient-state policy"),
				*OperationId),
			Operation->TransientStatePolicy,
			UE::UAI::OperationOwners::SpatialContent::
				IsDurableMutation(OperationId)
				? EDomainTransientStatePolicy::None
				: EDomainTransientStatePolicy::RecoverableSession);
		TestEqual(
			*FString::Printf(
				TEXT("%s declares its exact persistence lifetime"),
				*OperationId),
			Operation->bPersists,
			UE::UAI::OperationOwners::SpatialContent::
				IsDurableMutation(OperationId));
		TestTrue(
			*FString::Printf(
				TEXT("%s declares independent domain readback"),
				*OperationId),
			Operation->bIndependentReadback);
		TestTrue(
			*FString::Printf(
				TEXT("%s has a request-scoped invocation factory"),
				*OperationId),
			Operation->InvocationFactory.IsValid());
		TestTrue(
			*FString::Printf(
				TEXT("%s publishes the owner factory without a second path"),
				*OperationId),
			Operation->InvocationFactory.Get()
				== UE::UAI::OperationOwners::SpatialContent::
					FindOperationInvocationFactory(OperationId).Get());
	}

	TestEqual(
		TEXT("spatial content owns its migration identity"),
		Declaration.Migration.OwnerId,
		FString(TEXT("T46-SPATIAL-CONTENT-OPERATION")));
	TestTrue(
		TEXT("spatial content declares authoring-memory state"),
		Declaration.State.Planes.Contains(
			UE::UAI::StateSemantic::EUeAgentStatePlane::AuthoringMemory));
	TestTrue(
		TEXT("spatial content declares persisted actor and mesh state"),
		Declaration.State.Planes.Contains(
			UE::UAI::StateSemantic::EUeAgentStatePlane::Persisted));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUeAgentWorldSpatialSharedMechanicsHostTest,
	"UeAgentInterface.DomainAdapters.WorldSpatial.E1.Authority."
		"ViewportAndSpatialShareMechanicsHost",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FUeAgentWorldSpatialSharedMechanicsHostTest::RunTest(
	const FString& Parameters)
{
	using namespace UE::UAI;

	struct FResolution final
	{
		TSharedPtr<CommandEdge::IUeAgentCommandHandler> Direct;
		TSharedPtr<CommandEdge::IUeAgentCommandHandler> Owner;
		TArray<FString> DirectScopes;
		TArray<FString> OwnerScopes;
		FString DirectError;
		FString OwnerError;
	};

	auto CheckShared = [this](
		const FString& CommandId,
		const TFunction<bool(
			const FString&,
			TSharedPtr<CommandEdge::IUeAgentCommandHandler>&,
			TArray<FString>&,
			FString&)>& ResolveOwner)
	{
		FResolution Resolution;
		const bool bDirect = Composition::TimelineSpatial::
			TryResolveTimelineSpatialMechanics(
				CommandId,
				Resolution.Direct,
				Resolution.DirectScopes,
				Resolution.DirectError);
		const bool bOwner = ResolveOwner(
			CommandId,
			Resolution.Owner,
			Resolution.OwnerScopes,
			Resolution.OwnerError);
		TestTrue(
			TEXT("the common host resolves ") + CommandId,
			bDirect && Resolution.Direct.IsValid());
		TestTrue(
			TEXT("the public owner resolves ") + CommandId,
			bOwner && Resolution.Owner.IsValid());
		if (!bDirect || !bOwner
			|| !Resolution.Direct.IsValid()
			|| !Resolution.Owner.IsValid())
		{
			AddError(
				CommandId + TEXT(":")
				+ Resolution.DirectError + TEXT(":")
				+ Resolution.OwnerError);
			return false;
		}
		TestTrue(
			TEXT("the public owner consumes the common mechanics instance for ")
				+ CommandId,
			Resolution.Direct.Get() == Resolution.Owner.Get());
		TestEqual(
			TEXT("the public owner preserves common authority scopes for ")
				+ CommandId,
			Resolution.OwnerScopes,
			Resolution.DirectScopes);
		return true;
	};

	const bool bViewportShared = CheckShared(
		TEXT("viewport.instance.list"),
		[](const FString& CommandId,
			TSharedPtr<CommandEdge::IUeAgentCommandHandler>& OutHandler,
			TArray<FString>& OutScopes,
			FString& OutError)
		{
			return OperationOwners::ViewportGraph::TryResolveMechanics(
				CommandId,
				OutHandler,
				OutScopes,
				OutError);
		});
	const bool bSpatialShared = CheckShared(
		TEXT("world.object_transform.read"),
		[](const FString& CommandId,
			TSharedPtr<CommandEdge::IUeAgentCommandHandler>& OutHandler,
			TArray<FString>& OutScopes,
			FString& OutError)
		{
			return OperationOwners::SpatialContent::TryResolveMechanics(
				CommandId,
				OutHandler,
				OutScopes,
				OutError);
		});
	return bViewportShared && bSpatialShared;
}
