#include "Misc/AutomationTest.h"

// Recompile this leaf when the production resolver surface becomes available.
#include "Composition/SimpleAuthoring/UeAgentSimpleAuthoringRuntimeResolvers.h"


#include "Core/ContextResource/UeAgentContextResource.h"
#include "Core/AssetDurable/UeAgentAssetDurable.h"
#include "Core/Discovery/UeAgentDiscovery.h"
#include "DomainAdapters/AssetDataSimpleEffect/UeAgentAssetDataSimpleEffectTestTypes.h"
#include "Engine/DataAsset.h"
#include "Engine/World.h"
#include "Misc/ConfigCacheIni.h"
#include "Misc/PackageName.h"
#include "Misc/Paths.h"
#include "UObject/Package.h"
#include "UObject/UnrealType.h"
#include "UObject/UObjectGlobals.h"

namespace
{
	using namespace UE::UAI::Composition::SimpleAuthoring;
	using namespace UE::UAI::AssetDurable;
	using namespace UE::UAI::ContextResource;
	using namespace UE::UAI::Discovery;
	using namespace UE::UAI::DomainAdapters::AssetDataSimpleEffect;
	using namespace UE::UAI::ExecutionMutation;

	TSharedRef<FUeAgentResourceTracker, ESPMode::ThreadSafe> MakeTracker()
	{
		return MakeShared<FUeAgentResourceTracker, ESPMode::ThreadSafe>(
			FUeAgentResourceBudget({
				{EUeAgentResourceDimension::InputBytes, 1048576},
				{EUeAgentResourceDimension::OutputBytes, 1048576},
				{EUeAgentResourceDimension::StringBytes, 1048576},
				{EUeAgentResourceDimension::Nodes, 4096},
				{EUeAgentResourceDimension::Items, 4096},
				{EUeAgentResourceDimension::Files, 64},
				{EUeAgentResourceDimension::ReferenceEdges, 4096},
				{EUeAgentResourceDimension::WorkUnits, 65536},
				{
					EUeAgentResourceDimension::EstimatedMemoryBytes,
					16777216,
				},
			}));
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUeAgentSimpleAuthoringEditorProjectCatalogRuntimeResolverTest,
	"UeAgentInterface.Composition.SimpleAuthoring.E3.RuntimeResolvers.EditorProjectCatalog",
	EAutomationTestFlags::EditorContext
		| EAutomationTestFlags::EngineFilter)

bool FUeAgentSimpleAuthoringEditorProjectCatalogRuntimeResolverTest::RunTest(
	const FString& Parameters)
{
	FUeAgentEditorProjectSimpleAuthoringRuntimeResolvers Resolvers;
	FUeAgentSimpleAuthoringRuntimeResolverError Error;
	TestTrue(
		TEXT("EditorProject catalog and three resolver roles are constructible"),
		TryCreateEditorProjectSimpleAuthoringRuntimeResolvers(
			Resolvers,
			Error));
	if (!Resolvers.IsValid())
	{
		AddError(TEXT("editor_project_runtime_resolvers_missing:" ) + Error.Code);
		return false;
	}
	TestTrue(
		TEXT("target and settings-field catalogs are public and stable"),
		Resolvers.CatalogSource->GetCatalogKinds()
			== TArray<FString>{
				TEXT("editor_project_settings_field"),
				TEXT("editor_project_target"),
			});
	TestNotEqual(
		TEXT("pre-state and persisted target resolvers are independent owners"),
		Resolvers.PreStateTargetResolver.Get(),
		Resolvers.PersistedTargetResolver.Get());

	const TSharedRef<FUeAgentResourceTracker, ESPMode::ThreadSafe> Resources =
		MakeTracker();
	const auto Clock = []() { return FPlatformTime::Seconds(); };
	const FUeAgentOperationDeadline Deadline =
		FUeAgentOperationDeadline::FromTimeout(30.0, Clock);
	FUeAgentCancellationSource Cancellation(Clock);
	TSharedPtr<const FUeAgentCatalogSnapshot> TargetCatalog;
	FUeAgentDiscoveryError DiscoveryError;
	TestTrue(
		TEXT("Project Settings and config targets are discoverable through C2"),
		Resolvers.CatalogSource->TrySearch(
			FUeAgentDiscoveryQuery(
				TEXT("editor_project_target"),
				TEXT("GeneralProjectSettings"),
				EUeAgentDiscoveryMatchMode::Broad,
				EUeAgentMaterializationPolicy::IndexOnly),
			*Resources,
			Deadline,
			Cancellation.GetToken(),
			TargetCatalog,
			DiscoveryError));
	if (!TargetCatalog.IsValid())
	{
		AddError(TEXT("editor_project_target_catalog_missing:" ) + DiscoveryError.Code);
		return false;
	}
	const FUeAgentCandidateSummary* SettingsSummary = nullptr;
	const FUeAgentCandidateSummary* ConfigSummary = nullptr;
	for (const FUeAgentCandidateSummary& Candidate
		: TargetCatalog->GetCandidates())
	{
		if (Candidate.GetIdentity().GetObjectRole()
			== TEXT("project_settings"))
		{
			SettingsSummary = &Candidate;
		}
		else if (Candidate.GetIdentity().GetObjectRole()
			== TEXT("config_section"))
		{
			ConfigSummary = &Candidate;
		}
	}
	if (!SettingsSummary || !ConfigSummary)
	{
		AddError(TEXT("editor_project_expected_targets_not_discovered"));
		return false;
	}

	TSharedPtr<const FUeAgentCandidateBinding> SettingsBinding;
	TestTrue(
		TEXT("exact C2 resolve reconstructs the settings target binding"),
		Resolvers.CatalogSource->TryResolveExact(
			SettingsSummary->GetIdentity().GetCanonicalKey(),
			TargetCatalog->GetRevision(),
			*Resources,
			Deadline,
			Cancellation.GetToken(),
			SettingsBinding,
			DiscoveryError));
	if (!SettingsBinding.IsValid())
	{
		AddError(TEXT("editor_project_settings_binding_missing"));
		return false;
	}

	UE::UAI::DomainAdapters::EditorProjectSimpleEffect::
		FUeAgentEditorProjectSimpleEffectResolvedTarget PreTarget;
	FExecutionMutationError ResolveError;
	TestTrue(
		TEXT("authoring resolves the exact queried settings ref"),
		Resolvers.PreStateTargetResolver->TryResolveTarget(
			SettingsBinding->GetPublicRef(),
			PreTarget,
			ResolveError));
	TestEqual(
		TEXT("query and authoring share the target revision"),
		PreTarget.GetRevision(),
		SettingsSummary->GetIdentity().GetGeneration());
	TestEqual(
		TEXT("query and authoring share the profile"),
		PreTarget.GetProfileId(),
		FString(TEXT("project_settings")));

	UE::UAI::Composition::SimpleAuthoring::EditorProject::
		FUeAgentEditorProjectSimpleAuthoringDurableBinding Durable;
	TestTrue(
		TEXT("durable binding is derived from the same target"),
		Resolvers.DurableBindingResolver->TryResolveDurableBinding(
			PreTarget,
			Durable,
			ResolveError));
	TestEqual(
		TEXT("Project Settings uses the ProjectConfig durable kind"),
		Durable.GetArtifactKind(),
		EDurableArtifactKind::ProjectConfig);
	TestEqual(
		TEXT("Project Settings durable location is its UE default config file"),
		FPaths::ConvertRelativePathToFull(Durable.GetDurableLocation()),
		FPaths::ConvertRelativePathToFull(
			PreTarget.GetSettingsObject()->GetDefaultConfigFilename()));

	UE::UAI::DomainAdapters::EditorProjectSimpleEffect::
		FUeAgentEditorProjectSimpleEffectResolvedTarget PersistedTarget;
	TestTrue(
		TEXT("persisted resolver reloads the same logical settings target"),
		Resolvers.PersistedTargetResolver->TryResolveTarget(
			SettingsBinding->GetPublicRef(),
			PersistedTarget,
			ResolveError));
	TestNotEqual(
		TEXT("persisted settings readback does not reuse the live UObject"),
		PersistedTarget.GetSettingsObject(),
		PreTarget.GetSettingsObject());

	TSharedPtr<const FUeAgentCatalogSnapshot> FieldCatalog;
	TestTrue(
		TEXT("supported settings fields are discoverable through C2"),
		Resolvers.CatalogSource->TrySearch(
			FUeAgentDiscoveryQuery(
				TEXT("editor_project_settings_field"),
				TEXT("CompanyName"),
				EUeAgentDiscoveryMatchMode::Broad,
				EUeAgentMaterializationPolicy::IndexOnly),
			*Resources,
			Deadline,
			Cancellation.GetToken(),
			FieldCatalog,
			DiscoveryError));
	const FUeAgentCandidateSummary* FieldSummary = nullptr;
	if (FieldCatalog.IsValid())
	{
		for (const FUeAgentCandidateSummary& Candidate
			: FieldCatalog->GetCandidates())
		{
			if (Candidate.GetIdentity().GetOwnerId()
					== SettingsBinding->GetPublicRef()
				&& Candidate.GetDisplayName() == TEXT("CompanyName"))
			{
				FieldSummary = &Candidate;
				break;
			}
		}
	}
	if (!FieldSummary)
	{
		AddError(TEXT("editor_project_settings_field_missing"));
		return false;
	}
	TestEqual(
		TEXT("field query exposes the canonical type_id"),
		FieldSummary->GetIdentity().GetTargetKind(),
		FString(TEXT("uai.value.string.v1")));
	TSharedPtr<const FUeAgentCandidateBinding> FieldBinding;
	TestTrue(
		TEXT("field exact resolve uses the owning catalog source revision"),
		Resolvers.CatalogSource->TryResolveExact(
			FieldSummary->GetIdentity().GetCanonicalKey(),
			FieldCatalog->GetRevision(),
			*Resources,
			Deadline,
			Cancellation.GetToken(),
			FieldBinding,
			DiscoveryError));
	TestTrue(
		TEXT("field exact resolve reconstructs the queried public ref"),
		FieldBinding.IsValid()
			&& FieldBinding->GetPublicRef()
				== FieldSummary->GetIdentity().GetSelector());
	FProperty* Property = nullptr;
	FString CurrentFieldRevision;
	TestTrue(
		TEXT("authoring accepts the queried field ref and schema revision"),
		Resolvers.PreStateTargetResolver->TryResolveSettingsField(
			*PreTarget.GetSettingsObject(),
			FieldSummary->GetIdentity().GetSelector(),
			FieldSummary->GetIdentity().GetGeneration(),
			Property,
			CurrentFieldRevision,
			ResolveError));
	TestEqual(
		TEXT("field resolver returns the exact reflected property"),
		Property ? Property->GetName() : FString(),
		FString(TEXT("CompanyName")));
	const FStrProperty* StringProperty = CastField<FStrProperty>(Property);
	FProperty* PersistedProperty = nullptr;
	FString PersistedFieldRevision;
	TestNotNull(
		TEXT("CompanyName uses the public string field contract"),
		StringProperty);
	TestTrue(
		TEXT("persisted snapshot resolves the same reflected field"),
		Resolvers.PersistedTargetResolver->TryResolveSettingsField(
			*PersistedTarget.GetSettingsObject(),
			FieldSummary->GetIdentity().GetSelector(),
			FieldSummary->GetIdentity().GetGeneration(),
			PersistedProperty,
			PersistedFieldRevision,
			ResolveError));
	const FStrProperty* PersistedStringProperty =
		CastField<FStrProperty>(PersistedProperty);
	if (StringProperty && PersistedStringProperty)
	{
		void* LiveAddress = Property->ContainerPtrToValuePtr<void>(
			PreTarget.GetSettingsObject());
		const void* PersistedAddress =
			PersistedProperty->ContainerPtrToValuePtr<void>(
				PersistedTarget.GetSettingsObject());
		const FString LiveBefore =
			StringProperty->GetPropertyValue(LiveAddress);
		const FString PersistedBefore =
			PersistedStringProperty->GetPropertyValue(PersistedAddress);
		const FString SettingsSection =
			PreTarget.GetSettingsObject()->GetClass()->GetPathName();
		const FString SettingsFilename =
			PreTarget.GetSettingsObject()->GetDefaultConfigFilename();
		FString CachedBefore;
		const bool bHadCachedValue = GConfig->GetString(
			*SettingsSection,
			TEXT("CompanyName"),
			CachedBefore,
			SettingsFilename);
		StringProperty->SetPropertyValue(
			LiveAddress,
			TEXT("UAI-SA008-Live-Memory-Only"));
		GConfig->SetString(
			*SettingsSection,
			TEXT("CompanyName"),
			TEXT("UAI-SA008-Live-Memory-Only"),
			SettingsFilename);

		UE::UAI::DomainAdapters::EditorProjectSimpleEffect::
			FUeAgentEditorProjectSimpleEffectResolvedTarget ReloadedTarget;
		FProperty* ReloadedProperty = nullptr;
		FString ReloadedFieldRevision;
		const bool bReloaded =
			Resolvers.PersistedTargetResolver->TryResolveTarget(
				SettingsBinding->GetPublicRef(),
				ReloadedTarget,
				ResolveError)
			&& Resolvers.PersistedTargetResolver->TryResolveSettingsField(
				*ReloadedTarget.GetSettingsObject(),
				FieldSummary->GetIdentity().GetSelector(),
				FieldSummary->GetIdentity().GetGeneration(),
				ReloadedProperty,
				ReloadedFieldRevision,
				ResolveError);
		FString ReloadedValue;
		if (const FStrProperty* ReloadedStringProperty =
				CastField<FStrProperty>(ReloadedProperty))
		{
			ReloadedValue = ReloadedStringProperty->GetPropertyValue(
				ReloadedProperty->ContainerPtrToValuePtr<void>(
					ReloadedTarget.GetSettingsObject()));
		}
		StringProperty->SetPropertyValue(LiveAddress, LiveBefore);
		if (bHadCachedValue)
		{
			GConfig->SetString(
				*SettingsSection,
				TEXT("CompanyName"),
				*CachedBefore,
				SettingsFilename);
		}
		else
		{
			GConfig->RemoveKey(
				*SettingsSection,
				TEXT("CompanyName"),
				SettingsFilename);
		}
		TestTrue(
			TEXT("persisted settings can be reloaded after live memory diverges"),
			bReloaded);
		TestEqual(
			TEXT("persisted settings readback ignores the mutated live CDO"),
			ReloadedValue,
			PersistedBefore);
	}
	TestFalse(
		TEXT("stale field schema revision fails closed"),
		Resolvers.PreStateTargetResolver->TryResolveSettingsField(
			*PreTarget.GetSettingsObject(),
			FieldSummary->GetIdentity().GetSelector(),
			TEXT("sha256:stale"),
			Property,
			CurrentFieldRevision,
			ResolveError));

	UE::UAI::DomainAdapters::EditorProjectSimpleEffect::
		FUeAgentEditorProjectSimpleEffectResolvedTarget RejectedTarget;
	TestFalse(
		TEXT("filesystem paths are never interpreted as EditorProject refs"),
		Resolvers.PreStateTargetResolver->TryResolveTarget(
			FPaths::Combine(
				FPaths::ProjectConfigDir(),
				TEXT("DefaultGame.ini")),
			RejectedTarget,
			ResolveError));
	return true;
}


IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUeAgentSimpleAuthoringExactReferenceRuntimeResolverTest,
	"UeAgentInterface.Composition.SimpleAuthoring.E3.RuntimeResolvers.ExactReference",
	EAutomationTestFlags::EditorContext
		| EAutomationTestFlags::EngineFilter)

bool FUeAgentSimpleAuthoringExactReferenceRuntimeResolverTest::RunTest(
	const FString& Parameters)
{
	TSharedPtr<
		UE::UAI::DomainAdapters::WorldSimpleEffect::
			IUeAgentWorldSimpleEffectReferenceResolver,
		ESPMode::ThreadSafe> WorldResolver;
	TSharedPtr<
		UE::UAI::DomainAdapters::LandscapeGeometryEffect::
			IUeAgentLandscapeGeometryEffectReferenceResolver,
		ESPMode::ThreadSafe> LandscapeResolver;
	TSharedPtr<
		UE::UAI::DomainAdapters::ModelingGeometryEffect::
			IUeAgentModelingGeometryEffectReferenceResolver,
		ESPMode::ThreadSafe> ModelingResolver;
	FUeAgentSimpleAuthoringRuntimeResolverError Error;
	TestTrue(
		TEXT("World exact reference resolver is constructible"),
		TryCreateWorldSimpleAuthoringReferenceResolver(
			WorldResolver,
			Error));
	TestTrue(
		TEXT("Landscape exact reference resolver is constructible"),
		TryCreateLandscapeSimpleAuthoringReferenceResolver(
			LandscapeResolver,
			Error));
	TestTrue(
		TEXT("Modeling exact reference resolver is constructible"),
		TryCreateModelingSimpleAuthoringReferenceResolver(
			ModelingResolver,
			Error));
	if (!WorldResolver.IsValid()
		|| !LandscapeResolver.IsValid()
		|| !ModelingResolver.IsValid())
	{
		AddError(TEXT("simple_authoring_exact_reference_resolver_missing"));
		return false;
	}

	UWorld* SelectedWorld = NewObject<UWorld>(
		GetTransientPackage(),
		NAME_None,
		RF_Transient);
	UWorld* OtherWorld = NewObject<UWorld>(
		GetTransientPackage(),
		NAME_None,
		RF_Transient);
	UObject* SelectedObject =
		NewObject<UUeAgentAssetDataSimpleEffectDataAsset>(
		SelectedWorld,
		NAME_None,
		RF_Transient);
	UObject* OtherObject =
		NewObject<UUeAgentAssetDataSimpleEffectDataAsset>(
		OtherWorld,
		NAME_None,
		RF_Transient);
	UPackage* AssetPackage = CreatePackage(
		TEXT("/Temp/UAI/SA008/RuntimeResolverAsset"));
	UUeAgentAssetDataSimpleEffectDataAsset* Asset =
		NewObject<UUeAgentAssetDataSimpleEffectDataAsset>(
		AssetPackage,
		TEXT("RuntimeResolverAsset"),
		RF_Public | RF_Standalone | RF_Transient);
	FExecutionMutationError ResolveError;
	UObject* Resolved = nullptr;
	TestTrue(
		TEXT("world-owned exact object resolves in the selected world"),
		WorldResolver->TryResolveReference(
			*SelectedWorld,
			SelectedObject->GetPathName(),
			Resolved,
			ResolveError));
	TestEqual(TEXT("world exact resolve returns the same object"), Resolved, SelectedObject);
	Resolved = nullptr;
	TestTrue(
		TEXT("world-independent asset resolves for Landscape"),
		LandscapeResolver->TryResolveReference(
			*SelectedWorld,
			Asset->GetPathName(),
			Resolved,
			ResolveError));
	TestEqual(
		TEXT("Landscape asset resolve is exact"),
		Resolved,
		static_cast<UObject*>(Asset));
	Resolved = nullptr;
	TestFalse(
		TEXT("cross-world object is rejected without fallback"),
		ModelingResolver->TryResolveReference(
			*SelectedWorld,
			OtherObject->GetPathName(),
			Resolved,
			ResolveError));
	TestEqual(
		TEXT("cross-world rejection is typed"),
		ResolveError.GetCode(),
		FString(TEXT("simple_authoring_reference_world_mismatch")));
	Resolved = nullptr;
	TestFalse(
		TEXT("unresolved reference is rejected without search"),
		WorldResolver->TryResolveReference(
			*SelectedWorld,
			TEXT("/Game/UAI/Missing.Missing"),
			Resolved,
			ResolveError));
	TestEqual(
		TEXT("missing reference rejection is typed"),
		ResolveError.GetCode(),
		FString(TEXT("simple_authoring_reference_not_found")));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUeAgentSimpleAuthoringAssetRuntimeResolverTest,
	"UeAgentInterface.Composition.SimpleAuthoring.E3.RuntimeResolvers.Asset",
	EAutomationTestFlags::EditorContext
		| EAutomationTestFlags::EngineFilter)

bool FUeAgentSimpleAuthoringAssetRuntimeResolverTest::RunTest(
	const FString& Parameters)
{
	TSharedPtr<IUeAgentAssetDataSimpleEffectAssetResolver, ESPMode::ThreadSafe>
		Resolver;
	FUeAgentSimpleAuthoringRuntimeResolverError Error;
	TestTrue(
		TEXT("AssetData canonical asset resolver is constructible"),
		TryCreateAssetDataSimpleAuthoringAssetResolver(Resolver, Error));
	if (!Resolver.IsValid())
	{
		AddError(TEXT("simple_authoring_asset_resolver_missing"));
		return false;
	}

	const FString PackageName =
		TEXT("/Game/UAI/SA008/RuntimeResolverAsset");
	const FString ObjectPath = PackageName
		+ TEXT(".RuntimeResolverAsset");
	UPackage* Package = nullptr;
	FName ObjectName = NAME_None;
	FExecutionMutationError ResolveError;
	const bool bCreationTargetResolved = TestTrue(
		TEXT("canonical absent target resolves to exact package and name"),
		Resolver->TryResolveCreationTarget(
			ObjectPath,
			Package,
			ObjectName,
			ResolveError));
	if (!bCreationTargetResolved || !Package || ObjectName.IsNone())
	{
		AddError(
			TEXT("asset_creation_target_unavailable:")
			+ ResolveError.GetCode());
		return false;
	}
	TestEqual(TEXT("creation package is exact"), Package->GetName(), PackageName);
	TestEqual(TEXT("creation object name is exact"), ObjectName, FName(TEXT("RuntimeResolverAsset")));

	UUeAgentAssetDataSimpleEffectDataAsset* Asset =
		NewObject<UUeAgentAssetDataSimpleEffectDataAsset>(
		Package,
		ObjectName,
		RF_Public | RF_Standalone | RF_Transient);
	UObject* Existing = nullptr;
	TestTrue(
		TEXT("existing canonical object resolves exactly"),
		Resolver->TryResolveExisting(
			ObjectPath,
			Existing,
			ResolveError));
	TestEqual(
		TEXT("existing object identity is preserved"),
		Existing,
		static_cast<UObject*>(Asset));
	Package = nullptr;
	ObjectName = NAME_None;
	TestFalse(
		TEXT("existing target conflicts before create"),
		Resolver->TryResolveCreationTarget(
			ObjectPath,
			Package,
			ObjectName,
			ResolveError));
	TestEqual(
		TEXT("existing create conflict is typed"),
		ResolveError.GetCode(),
		FString(TEXT("asset_data_creation_target_exists")));
	TestFalse(
		TEXT("object-name mismatch is rejected"),
		Resolver->TryResolveCreationTarget(
			PackageName + TEXT(".WrongName"),
			Package,
			ObjectName,
			ResolveError));
	TestFalse(
		TEXT("non-project mount is rejected"),
		Resolver->TryResolveCreationTarget(
			TEXT("/Engine/UAI/Forbidden.Forbidden"),
			Package,
			ObjectName,
			ResolveError));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUeAgentSimpleAuthoringFactoryCatalogRuntimeResolverTest,
	"UeAgentInterface.Composition.SimpleAuthoring.E3.RuntimeResolvers.FactoryCatalog",
	EAutomationTestFlags::EditorContext
		| EAutomationTestFlags::EngineFilter)

bool FUeAgentSimpleAuthoringFactoryCatalogRuntimeResolverTest::RunTest(
	const FString& Parameters)
{
	TSharedPtr<IUeAgentCatalogSource, ESPMode::ThreadSafe> CatalogSource;
	TSharedPtr<
		IUeAgentAssetDataSimpleEffectFactoryCandidateResolver,
		ESPMode::ThreadSafe> Resolver;
	FUeAgentSimpleAuthoringRuntimeResolverError Error;
	TestTrue(
		TEXT("one production owner exposes both C2 catalog and factory resolver"),
		TryCreateAssetDataSimpleAuthoringFactoryCatalog(
			CatalogSource,
			Resolver,
			Error));
	if (!CatalogSource.IsValid() || !Resolver.IsValid())
	{
		AddError(TEXT("simple_authoring_factory_catalog_missing"));
		return false;
	}
	TestTrue(
		TEXT("factory catalog kind is public and stable"),
		CatalogSource->GetCatalogKinds()
			== TArray<FString>{TEXT("asset_data_factory")});

	const TSharedRef<FUeAgentResourceTracker, ESPMode::ThreadSafe> Resources =
		MakeTracker();
	const auto Clock = []() { return FPlatformTime::Seconds(); };
	const FUeAgentOperationDeadline Deadline =
		FUeAgentOperationDeadline::FromTimeout(30.0, Clock);
	FUeAgentCancellationSource Cancellation(Clock);
	TSharedPtr<const FUeAgentCatalogSnapshot> Catalog;
	FUeAgentDiscoveryError DiscoveryError;
	TestTrue(
		TEXT("data_object factory is discoverable through C2"),
		CatalogSource->TrySearch(
			FUeAgentDiscoveryQuery(
				TEXT("asset_data_factory"),
				UUeAgentAssetDataSimpleEffectDataAsset::StaticClass()
					->GetPathName(),
				EUeAgentDiscoveryMatchMode::Exact,
				EUeAgentMaterializationPolicy::IndexOnly),
			*Resources,
			Deadline,
			Cancellation.GetToken(),
			Catalog,
			DiscoveryError));
	if (!Catalog.IsValid() || Catalog->GetCandidates().Num() != 1)
	{
		AddError(
			TEXT("factory_catalog_exact_candidate_missing:")
			+ DiscoveryError.Code);
		return false;
	}

	TSharedPtr<const FUeAgentCandidateBinding> Candidate;
	TestTrue(
		TEXT("C2 exact resolve reconstructs the factory binding"),
		CatalogSource->TryResolveExact(
			Catalog->GetCandidates()[0]
				.GetIdentity()
				.GetCanonicalKey(),
			Catalog->GetRevision(),
			*Resources,
			Deadline,
			Cancellation.GetToken(),
			Candidate,
			DiscoveryError));
	if (!Candidate.IsValid())
	{
		AddError(TEXT("factory_catalog_binding_missing"));
		return false;
	}
	TestTrue(
		TEXT("public factory ref carries no raw class field"),
		Candidate->GetPublicRef().StartsWith(
			TEXT("uai://factory/asset-data/data_object/")));

	FUeAgentAssetDataSimpleEffectFactoryBinding PlanningBinding;
	FExecutionMutationError ResolveError;
	TestTrue(
		TEXT("authoring resolver accepts the exact queried ref and revision"),
		Resolver->TryResolveForPlanning(
			Candidate->GetPublicRef(),
			Candidate->GetCatalogRevision(),
			TEXT("data_object"),
			PlanningBinding,
			ResolveError));
	TestEqual(
		TEXT("factory catalog and authoring share one binding"),
		PlanningBinding.GetCandidateBinding()->GetBindingId(),
		Candidate->GetBindingId());

	FUeAgentAssetDataSimpleEffectFactoryBinding FrozenBinding;
	TestTrue(
		TEXT("mutation resolves the same frozen binding"),
		Resolver->TryResolveFrozenBinding(
			Candidate->GetBindingId(),
			Candidate->GetCatalogRevision(),
			TEXT("data_object"),
			FrozenBinding,
			ResolveError));
	TestEqual(
		TEXT("frozen binding preserves exact concrete class"),
		FrozenBinding.GetConcreteClass(),
		UUeAgentAssetDataSimpleEffectDataAsset::StaticClass());
	TestFalse(
		TEXT("wrong profile cannot reuse a valid public ref"),
		Resolver->TryResolveForPlanning(
			Candidate->GetPublicRef(),
			Candidate->GetCatalogRevision(),
			TEXT("media_source"),
			PlanningBinding,
			ResolveError));
	TestFalse(
		TEXT("stale catalog revision fails before mutation"),
		Resolver->TryResolveForPlanning(
			Candidate->GetPublicRef(),
			FUeAgentCatalogRevision(
				Candidate->GetCatalogRevision().GetSourceId(),
				TEXT("sha256:stale"),
				Candidate->GetCatalogRevision().GetSequence()),
			TEXT("data_object"),
			PlanningBinding,
			ResolveError));
	return true;
}
