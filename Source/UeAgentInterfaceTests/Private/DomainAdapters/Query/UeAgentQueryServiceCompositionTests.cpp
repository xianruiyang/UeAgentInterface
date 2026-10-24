#include "Misc/AutomationTest.h"

#include "DomainAdapters/Query/UeAgentQueryService.h"

namespace
{
	using namespace UE::UAI::DomainAdapters::Query;

	const TArray<FString> ContentGraphProviderBindings = {
		TEXT("content_graph.read.summary"),
		TEXT("content_graph.read.structure"),
		TEXT("content_graph.read.items"),
		TEXT("content_graph.read.relations"),
		TEXT("content_graph.read.properties"),
		TEXT("content_graph.read.diagnostics"),
		TEXT("content_graph.read.compound"),
	};
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUeAgentQueryServiceContentGraphCompositionTest,
	"UAI.DomainAdapters.Query.ServiceComposition.ContentGraphRead",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FUeAgentQueryServiceContentGraphCompositionTest::RunTest(
	const FString& Parameters)
{
	FProductionQueryComposition Composition;
	FString Error;
	TestTrue(
		TEXT("production Query service composes"),
		TryCreateProductionQueryService(Composition, Error));
	if (!Composition.IsValid())
	{
		AddError(Error);
		return false;
	}
	TestEqual(
		TEXT("production Query descriptor inventory includes seven ContentGraph reads"),
		Composition.DescriptorRegistry->GetDescriptors().Num(),
		49);
	for (const FString& ProviderBinding : ContentGraphProviderBindings)
	{
		const FString DescriptorId = FString(TEXT("uai.query."))
			+ ProviderBinding.Replace(
				TEXT("content_graph.read."),
				TEXT("content_graph."));
		TestNotNull(
			*FString::Printf(
				TEXT("%s descriptor is in the public Query authority"),
				*DescriptorId),
			Composition.DescriptorRegistry->FindByDescriptorId(
				DescriptorId));
	}

	TArray<UE::UAI::BindingGateway::Query::
		FUeAgentDomainQueryProviderRegistration> Registrations;
	TestTrue(
		TEXT("production Query providers compose"),
		TryCreateProductionQueryProviderRegistrations(
			Registrations,
			Error));
	TestEqual(
		TEXT("production Query provider inventory includes seven ContentGraph providers"),
		Registrations.Num(),
		48);
	TSet<FString> ProviderBindings;
	for (const auto& Registration : Registrations)
	{
		TestTrue(
			TEXT("each production Query provider registration is valid"),
			Registration.IsValid());
		ProviderBindings.Add(Registration.GetProviderBinding());
	}
	TestEqual(
		TEXT("production Query provider bindings remain unique"),
		ProviderBindings.Num(),
		Registrations.Num());
	for (const FString& ProviderBinding : ContentGraphProviderBindings)
	{
		TestTrue(
			*FString::Printf(
				TEXT("%s is registered once"),
				*ProviderBinding),
			ProviderBindings.Contains(ProviderBinding));
	}
	return true;
}
