#pragma once

#include "CoreMinimal.h"
#include "Core/StateSemantic/UeAgentStateSemantic.h"

namespace UE::UAI::HjsonWorkspace
{
	class FWorkspaceError;
	class FAuthoringModel;
	class FAcceptedWorkspaceInventory;
	enum class ESemanticUnitKind : uint8;
	enum class EAuthoringContentDirectory { Placement, NamedChild, Inherited };
	EAuthoringContentDirectory GetAuthoringContentDirectoryRole(ESemanticUnitKind Kind, const FString& SemanticType);
	namespace PropertyDocumentName
	{
		inline constexpr TCHAR Properties[] = TEXT("properties.hjson");
		inline constexpr TCHAR Inputs[] = TEXT("inputs.hjson");
		inline constexpr TCHAR SlotProperties[] = TEXT("slot/properties.hjson");
	}
	bool TryGetAcceptedNamedChildDirectory(const FAuthoringModel& Model, const FAcceptedWorkspaceInventory& Inventory,
		const FString& Id, FString& OutDirectory, FWorkspaceError& OutError);
	bool IsInternalAuthoringStructureSurface(ESemanticUnitKind Kind, const FString& SemanticType, int32 ValueCount);
	// Physical organization only. Providers continue to consume their original values.
	using FPublishAuthoringContent = TFunctionRef<bool(
		const StateSemantic::FUeAgentCanonicalValuePtr& Value, const FString& RelativeStem,
		StateSemantic::FUeAgentCanonicalValuePtr& OutReference)>;

	FString MakeAuthoringContentPathSegment(const FString& Name);
	// The same declaration used by the organizer. Ordinary property rows have no
	// external document pieces; callers need not execute a speculative organizer.
	bool HasAuthoringPropertyContent(const FString& SemanticType, const FString& Role);

	bool TryOrganizeAuthoringContent(const FString& SemanticType, const FString& Role,
		StateSemantic::FUeAgentCanonicalValuePtr& VisibleRow, FPublishAuthoringContent Publish,
		ContextResource::FUeAgentResourceTracker& Resources, FWorkspaceError& OutError);
}
