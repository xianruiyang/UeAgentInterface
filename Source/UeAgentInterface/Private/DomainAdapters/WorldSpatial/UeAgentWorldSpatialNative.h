#pragma once

#include "CoreMinimal.h"
#include "DomainAdapters/WorldSpatial/UeAgentWorldSpatialRequests.h"

class FEditorViewportClient;
class FViewport;
class SGraphEditor;
class UObject;
class UGeometrySelectionManager;
class UPrimitiveComponent;

#define UAI_WORLD_SPATIAL_NATIVE_PORTS_V2 1

namespace UE::UAI::DomainAdapters::WorldSpatial
{
	struct UEAGENTINTERFACE_API FUeAgentWorldSpatialMeshSelectionTarget
	{
		FString ExplicitRef;
		UPrimitiveComponent* Component = nullptr;

		bool IsValid() const;
	};

	class UEAGENTINTERFACE_API IUeAgentWorldSpatialNativeEnvironment
	{
	public:
		virtual ~IUeAgentWorldSpatialNativeEnvironment() = default;

		virtual bool TryResolveObject(
			const FString& ExplicitRef,
			UObject*& OutObject,
			FUeAgentWorldSpatialExecutionError& OutError) = 0;

		virtual bool TryResolveGraphEditor(
			const FString& AssetEditorIdentity,
			TSharedPtr<SGraphEditor>& OutGraphEditor,
			FUeAgentWorldSpatialExecutionError& OutError) = 0;

		virtual bool TryResolveViewport(
			const FString& ViewportIdentity,
			FEditorViewportClient*& OutClient,
			FViewport*& OutViewport,
			FUeAgentWorldSpatialExecutionError& OutError) = 0;

		virtual bool TryGetExplicitObjectRef(
			const UObject* Object,
			FString& OutExplicitRef,
			FUeAgentWorldSpatialExecutionError& OutError) = 0;

		virtual bool TryPersistObject(
			UObject& Object,
			FUeAgentWorldSpatialExecutionError& OutError) = 0;

		virtual bool TryResolveMeshSelectionSession(
			const FString& ViewportIdentity,
			const TArray<FString>& SelectionIdentities,
			UGeometrySelectionManager*& OutManager,
			TArray<FUeAgentWorldSpatialMeshSelectionTarget>& OutTargets,
			FUeAgentWorldSpatialExecutionError& OutError) = 0;

		virtual void GetExplicitViewportIdentities(
			TArray<FString>& OutViewportIdentities) const = 0;
	};

	struct UEAGENTINTERFACE_API FUeAgentWorldSpatialCompiledValue
	{
		FString Subject;
		FString Quantity;
		FString FrameId;
		int32 Dimension = 0;
		FString Unit;
		TArray<double> Components;
		FString EvidenceId;

		bool IsValid() const;
	};

	class UEAGENTINTERFACE_API IUeAgentWorldSpatialSpaceCompiler
	{
	public:
		virtual ~IUeAgentWorldSpatialSpaceCompiler() = default;

		virtual bool TryConvertToNative(
			const FWorldSpatialValue& Value,
			const FString& NativeFrameId,
			const FString& NativeUnit,
			FUeAgentWorldSpatialCompiledValue& OutValue,
			FUeAgentWorldSpatialExecutionError& OutError) = 0;

		virtual bool TryConvertFromNative(
			const FUeAgentWorldSpatialCompiledValue& Value,
			const FWorldSpatialFrame& SemanticFrame,
			FWorldSpatialValue& OutValue,
			FUeAgentWorldSpatialExecutionError& OutError) = 0;
	};

	class UEAGENTINTERFACE_API IUeAgentWorldSpatialRevisionSource
	{
	public:
		virtual ~IUeAgentWorldSpatialRevisionSource() = default;

		virtual bool TryReadRevision(
			const FString& ExplicitRef,
			FWorldSpatialRevision& OutRevision,
			FUeAgentWorldSpatialExecutionError& OutError) = 0;
	};

	struct UEAGENTINTERFACE_API FUeAgentWorldSpatialNativeDependencies
	{
		TSharedPtr<IUeAgentWorldSpatialNativeEnvironment> Environment;
		TSharedPtr<IUeAgentWorldSpatialSpaceCompiler> SpaceCompiler;
		TSharedPtr<IUeAgentWorldSpatialRevisionSource> RevisionSource;

		bool IsValid() const;
	};

	struct UEAGENTINTERFACE_API FUeAgentWorldSpatialNativePorts
	{
		TSharedPtr<IUeAgentWorldSpatialReadbackPort> Readback;
		TSharedPtr<IUeAgentWorldSpatialMutationPort> Mutation;
		TSharedPtr<IUeAgentWorldSpatialSettlePort> Settle;
		TSharedPtr<IUeAgentWorldSpatialResultComparator> Comparator;

		bool IsValid() const;
	};

	UEAGENTINTERFACE_API bool TryCreateWorldSpatialNativePorts(
		const FUeAgentWorldSpatialNativeDependencies& Dependencies,
		FUeAgentWorldSpatialNativePorts& OutPorts,
		FUeAgentWorldSpatialExecutionError& OutError);
}
