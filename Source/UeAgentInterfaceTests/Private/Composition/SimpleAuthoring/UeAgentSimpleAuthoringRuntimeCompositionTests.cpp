#include "Misc/AutomationTest.h"

#include "Composition/SimpleAuthoring/UeAgentSimpleAuthoringComposition.h"
#include "Composition/SimpleAuthoring/UeAgentSimpleAuthoringRuntime.h"
#include "Core/BindingGateway/Ticket/UeAgentBindingTicket.h"
#include "Core/Projection/UeAgentProjection.h"
#include "Editor.h"
#include "Engine/World.h"
#include "HAL/PlatformTime.h"
#include "Misc/Guid.h"
#include "Misc/Paths.h"

namespace
{
	static_assert(UAI_SIMPLE_AUTHORING_RUNTIME_V1 == 1);

	using namespace UE::UAI::Composition::SimpleAuthoring;
	using namespace UE::UAI::ContextResource;
	using namespace UE::UAI::Projection;

	class FRuntimeTokenEstimator final : public IUeAgentTokenEstimator
	{
	public:
		bool TryEstimate(
			const FUeAgentProjectedPayload& Payload,
			const TArray<FUeAgentArtifactDescriptor>& Artifacts,
			const TArray<FUeAgentProjectionDiagnostic>& Diagnostics,
			FUeAgentResourceTracker& Resources,
			const FUeAgentOperationDeadline& Deadline,
			const FUeAgentCancellationToken& Cancellation,
			FUeAgentTokenEstimate& OutEstimate,
			FUeAgentProjectionError& OutError) override
		{
			OutEstimate = FUeAgentTokenEstimate(
				EUeAgentTokenEstimateKind::Exact,
				32,
				128);
			OutError = {};
			return true;
		}
	};
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUeAgentSimpleAuthoringRuntimeActivationTest,
	"UeAgentInterface.Composition.SimpleAuthoring.E3.RuntimeActivation",
	EAutomationTestFlags::EditorContext
		| EAutomationTestFlags::EngineFilter)

bool FUeAgentSimpleAuthoringRuntimeActivationTest::RunTest(
	const FString& Parameters)
{
	UWorld* World = GEditor
		? GEditor->GetEditorWorldContext().World()
		: nullptr;
	if (!TestNotNull(TEXT("an explicit Editor world is available"), World))
	{
		return false;
	}

	TSharedPtr<const UE::UAI::BindingGateway::Ticket::
		FUeAgentOperationTicketService> TicketService;
	UE::UAI::BindingGateway::Ticket::FUeAgentOperationTicketError
		TicketError;
	TestTrue(
		TEXT("shared operation ticket service composes"),
		UE::UAI::BindingGateway::Ticket::
			FUeAgentOperationTicketService::TryCreate(
				TEXT("sa008-runtime-activation-integrity"),
				60.0,
				[]() { return FPlatformTime::Seconds(); },
				[]()
				{
					return FGuid::NewGuid().ToString(
						EGuidFormats::Digits);
				},
				TicketService,
				TicketError));
	TSharedPtr<const FUeAgentResponseProjector> Projector;
	FUeAgentProjectionError ProjectionError;
	TestTrue(
		TEXT("shared response projector composes"),
		FUeAgentResponseProjector::TryCreate(
			MakeShared<FRuntimeTokenEstimator>(),
			Projector,
			ProjectionError));
	if (!TicketService.IsValid() || !Projector.IsValid())
	{
		return false;
	}

	FUeAgentSimpleAuthoringRuntimeConfig Config;
	Config.ProjectInstanceId = TEXT("project:sa008-runtime");
	Config.EditorInstanceId = TEXT("editor:sa008-runtime");
	Config.ServerSessionId = TEXT("session:sa008-runtime");
	Config.RegistryEpoch = 1;

	Config.JournalRoot = FPaths::ProjectSavedDir()
		/ TEXT("UeAgentInterface/SimpleAuthoring");

	TSharedPtr<FUeAgentSimpleAuthoringRuntime> Runtime;
	FUeAgentSimpleAuthoringRuntimeError RuntimeError;
	TestTrue(
		TEXT("the production runtime composes once"),
		FUeAgentSimpleAuthoringRuntime::TryCreate(
			Config,
			*World,
			TicketService.ToSharedRef(),
			Projector.ToSharedRef(),
			Runtime,
			RuntimeError));
	if (!Runtime.IsValid())
	{
		AddError(
			TEXT("sa008_runtime_activation_failed:")
			+ RuntimeError.Code
			+ TEXT(":")
			+ RuntimeError.Detail);
		return false;
	}

	const TArray<FString> ExpectedAuthorityScopes{
		TEXT("uai:asset-data:write"),
		TEXT("uai:asset:write"),
		TEXT("uai:editor-project:write"),
		TEXT("uai:landscape:write"),
		TEXT("uai:modeling-geometry:write"),
		TEXT("uai:project-config:write"),
		TEXT("uai:world:write"),
	};
	TestEqual(
		TEXT("runtime grants only the seven scopes declared by families"),
		Runtime->GetAuthorityScopes(),
		ExpectedAuthorityScopes);
	const TArray<FUeAgentSimpleAuthoringCommandRoute>& Routes =
		GetSimpleAuthoringCommandRoutes();
	TestEqual(TEXT("one application binding per route"),
		Runtime->GetApplication()->GetBindingCount(), Routes.Num());
	TestEqual(TEXT("one thin public command handler per route"),
		Runtime->GetCommandHandlers().Num(), Routes.Num());
	for (const FUeAgentSimpleAuthoringCommandRoute& Route : Routes)
	{
		TestNotNull(
			TEXT("runtime command registry contains route: ")
				+ Route.CommandId,
			Runtime->GetCommandRegistry()->FindExact(Route.CommandId));
	}
	TSet<FString> CatalogKinds;
	for (const TSharedRef<UE::UAI::Discovery::IUeAgentCatalogSource>& Source
		: Runtime->GetCatalogSources())
	{
		for (const FString& Kind : Source->GetCatalogKinds())
		{
			CatalogKinds.Add(Kind);
		}
	}
	TestTrue(TEXT("factory candidates are publicly discoverable"),
		CatalogKinds.Contains(TEXT("asset_data_factory")));
	TestTrue(TEXT("EditorProject targets are publicly discoverable"),
		CatalogKinds.Contains(TEXT("editor_project_target")));
	TestTrue(TEXT("EditorProject fields are publicly discoverable"),
		CatalogKinds.Contains(TEXT("editor_project_settings_field")));
	return true;
}
