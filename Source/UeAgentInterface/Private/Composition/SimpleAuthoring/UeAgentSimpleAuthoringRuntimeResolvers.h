#pragma once

#include "Core/Discovery/UeAgentDiscovery.h"
#include "Composition/SimpleAuthoring/Families/EditorProject/UeAgentEditorProjectSimpleAuthoringProduction.h"
#include "DomainAdapters/AssetDataSimpleEffect/UeAgentAssetDataSimpleEffectCapabilities.h"
#include "DomainAdapters/EditorProjectSimpleEffect/UeAgentEditorProjectSimpleEffectCapabilities.h"
#include "DomainAdapters/LandscapeGeometryEffect/UeAgentLandscapeGeometryEffectCapabilities.h"
#include "DomainAdapters/ModelingGeometryEffect/UeAgentModelingGeometryEffectCapabilities.h"
#include "DomainAdapters/WorldSimpleEffect/UeAgentWorldSimpleEffectCapabilities.h"
#include "Templates/SharedPointer.h"

#define UAI_SIMPLE_AUTHORING_RUNTIME_RESOLVERS_V1 1

namespace UE::UAI::Composition::SimpleAuthoring
{
	struct UEAGENTINTERFACE_API
		FUeAgentSimpleAuthoringRuntimeResolverError
	{
		FString Code;
		FString SubjectId;
		FString Detail;

		bool IsValid() const
		{
			return !Code.IsEmpty();
		}
	};

	UEAGENTINTERFACE_API bool
	TryCreateWorldSimpleAuthoringReferenceResolver(
		TSharedPtr<
			DomainAdapters::WorldSimpleEffect::
				IUeAgentWorldSimpleEffectReferenceResolver,
			ESPMode::ThreadSafe>& OutResolver,
		FUeAgentSimpleAuthoringRuntimeResolverError& OutError);

	UEAGENTINTERFACE_API bool
	TryCreateLandscapeSimpleAuthoringReferenceResolver(
		TSharedPtr<
			DomainAdapters::LandscapeGeometryEffect::
				IUeAgentLandscapeGeometryEffectReferenceResolver,
			ESPMode::ThreadSafe>& OutResolver,
		FUeAgentSimpleAuthoringRuntimeResolverError& OutError);

	UEAGENTINTERFACE_API bool
	TryCreateModelingSimpleAuthoringReferenceResolver(
		TSharedPtr<
			DomainAdapters::ModelingGeometryEffect::
				IUeAgentModelingGeometryEffectReferenceResolver,
			ESPMode::ThreadSafe>& OutResolver,
		FUeAgentSimpleAuthoringRuntimeResolverError& OutError);

	UEAGENTINTERFACE_API bool
	TryCreateAssetDataSimpleAuthoringAssetResolver(
		TSharedPtr<
			DomainAdapters::AssetDataSimpleEffect::
				IUeAgentAssetDataSimpleEffectAssetResolver,
			ESPMode::ThreadSafe>& OutResolver,
		FUeAgentSimpleAuthoringRuntimeResolverError& OutError);

	UEAGENTINTERFACE_API bool
	TryCreateAssetDataSimpleAuthoringFactoryCatalog(
		TSharedPtr<Discovery::IUeAgentCatalogSource, ESPMode::ThreadSafe>&
			OutCatalogSource,
		TSharedPtr<
			DomainAdapters::AssetDataSimpleEffect::
				IUeAgentAssetDataSimpleEffectFactoryCandidateResolver,
			ESPMode::ThreadSafe>& OutResolver,
		FUeAgentSimpleAuthoringRuntimeResolverError& OutError);

	struct UEAGENTINTERFACE_API
		FUeAgentEditorProjectSimpleAuthoringRuntimeResolvers
	{
		TSharedPtr<Discovery::IUeAgentCatalogSource, ESPMode::ThreadSafe>
			CatalogSource;
		TSharedPtr<
			DomainAdapters::EditorProjectSimpleEffect::
				IUeAgentEditorProjectSimpleEffectTargetResolver,
			ESPMode::ThreadSafe> MutationTargetResolver;
		TSharedPtr<
			DomainAdapters::EditorProjectSimpleEffect::
				IUeAgentEditorProjectSimpleEffectTargetResolver,
			ESPMode::ThreadSafe> PreStateTargetResolver;
		TSharedPtr<
			DomainAdapters::EditorProjectSimpleEffect::
				IUeAgentEditorProjectSimpleEffectTargetResolver,
			ESPMode::ThreadSafe> PersistedTargetResolver;
		TSharedPtr<
			EditorProject::
				IUeAgentEditorProjectSimpleAuthoringDurableBindingResolver,
			ESPMode::ThreadSafe> DurableBindingResolver;

		bool IsValid() const;
	};

	UEAGENTINTERFACE_API bool
	TryCreateEditorProjectSimpleAuthoringRuntimeResolvers(
		FUeAgentEditorProjectSimpleAuthoringRuntimeResolvers& OutResolvers,
		FUeAgentSimpleAuthoringRuntimeResolverError& OutError);
}
