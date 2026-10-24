#pragma once

#include "CoreMinimal.h"
#include "DomainAdapters/AssetProjectRead/UeAgentAssetProjectReadProvider.h"
#include "Templates/SharedPointer.h"

class FConfigFile;
class UObject;
struct FAssetData;

namespace UE::UAI::DomainAdapters::AssetProjectRead
{
	struct FUeAgentEditorResourceObservation
	{
		FString ResourceId;
		FString ResourceKind;
		FString PackageName;
		FString ObjectPath;
		FString ClassPath;
		FString EditorIdentity;
		bool bDirty = false;
	};

	struct FUeAgentProjectPlatformObservation
	{
		FString PlatformName;
		FString IniPlatformName;
		FString DisplayName;
		FString SdkDocumentationPath;
		bool bRunningPlatform = false;
		bool bSdkInstalled = false;
	};

	enum class EUeAgentAssetRelationDirection : uint8
	{
		Dependencies,
		Referencers,
		Both,
	};

	enum class EUeAgentAssetRelationKind : uint8
	{
		Hard,
		Soft,
		Manage,
		SearchableName,
	};

	enum class EUeAgentAssetRelationPackageScope : uint8
	{
		All,
		Game,
		EditorOnly,
		Build,
	};

	struct FUeAgentAssetRelationGraphRequest
	{
		EUeAgentAssetRelationDirection Direction =
			EUeAgentAssetRelationDirection::Dependencies;
		TArray<EUeAgentAssetRelationKind> RelationKinds;
		EUeAgentAssetRelationPackageScope PackageScope =
			EUeAgentAssetRelationPackageScope::All;
	};

	struct FUeAgentAssetRelationNode
	{
		FString StableIdentifier;
		FString PackageName;
		FString ObjectPath;
		bool bResolved = false;
		bool bRedirector = false;
		FString RedirectTarget;
	};

	struct FUeAgentAssetRelationEdge
	{
		FString SourceIdentifier;
		FString SourcePackageName;
		FString SourceObjectPath;
		FString TargetIdentifier;
		FString TargetPackageName;
		FString TargetObjectPath;
		FString TraversalDirection;
		int32 Depth = 0;
		TArray<FString> TraversalPath;
		bool bCycle = false;
		FString Category;
		int32 CategoryFlags = 0;
		int32 PropertyFlags = 0;
		bool bHard = false;
		bool bGame = false;
		bool bBuild = false;
		bool bDirect = false;
		bool bSourceResolved = false;
		bool bTargetResolved = false;
		bool bSourceRedirector = false;
		bool bTargetRedirector = false;
		FString SourceRedirectTarget;
		FString TargetRedirectTarget;
	};

	struct FUeAgentAssetRelationGraphResult
	{
		FUeAgentAssetRelationNode Root;
		TArray<FUeAgentAssetRelationNode> Nodes;
		TArray<FUeAgentAssetRelationEdge> Edges;
		bool bRegistryLoading = false;
		bool bComplete = false;
		bool bOnDiskAssetRegistryOnly = true;
		bool bRuntimeReferencesIncluded = false;
	};

	class UEAGENTINTERFACE_API
		IUeAgentAssetProjectReadEnvironment
	{
	public:
		virtual ~IUeAgentAssetProjectReadEnvironment() = default;

		virtual bool TryResolveAsset(
			const UE::UAI::Discovery::
				FUeAgentCandidateBinding& Target,
			UObject*& OutAsset,
			FUeAgentAssetProjectReadError& OutError) = 0;

		virtual bool TryResolveRegistryRecord(
			const UE::UAI::Discovery::
				FUeAgentCandidateBinding& Target,
			FAssetData& OutRecord,
			FUeAgentAssetProjectReadError& OutError) = 0;

		virtual bool TryResolveRegistryRelations(
			const FAssetData& Record,
			const FUeAgentAssetRelationGraphRequest& Request,
			FUeAgentAssetRelationGraphResult& OutGraph,
			FUeAgentAssetProjectReadError& OutError) = 0;

		virtual bool TryResolveOpenAssets(
			const UE::UAI::ContextResource::
				FUeAgentExecutionContextRef& Context,
			bool bIncludeEditorIdentity,
			TArray<FUeAgentEditorResourceObservation>& OutResources,
			FUeAgentAssetProjectReadError& OutError) = 0;

		virtual bool TryResolveDirtyResources(
			const UE::UAI::ContextResource::
				FUeAgentExecutionContextRef& Context,
			const TArray<FString>& ResourceKinds,
			TArray<FUeAgentEditorResourceObservation>& OutResources,
			FUeAgentAssetProjectReadError& OutError) = 0;

		virtual bool TryResolveProjectPlatforms(
			const UE::UAI::ContextResource::
				FUeAgentExecutionContextRef& Context,
			const TArray<FString>& PlatformIds,
			TArray<FUeAgentProjectPlatformObservation>& OutPlatforms,
			FUeAgentAssetProjectReadError& OutError) = 0;

		virtual bool TryResolveProjectObject(
			const UE::UAI::ContextResource::
				FUeAgentExecutionContextRef& Context,
			const FString& SourceId,
			UObject*& OutObject,
			FUeAgentAssetProjectReadError& OutError) = 0;

		virtual bool TryResolveProjectSettingsObject(
			const UE::UAI::ContextResource::FUeAgentExecutionContextRef& Context,
			const FString& SectionId,
			UObject*& OutObject,
			FUeAgentAssetProjectReadError& OutError) = 0;

		virtual bool TryResolveConfig(
			const UE::UAI::ContextResource::
				FUeAgentExecutionContextRef& Context,
			const FString& ConfigScope,
			const FConfigFile*& OutConfig,
			FUeAgentAssetProjectReadError& OutError) = 0;
	};

	class UEAGENTINTERFACE_API
		FUeAgentAssetProjectReadNativeHookFactory final
	{
	public:
		static bool TryCreate(
			const FString& ProviderBinding,
			TSharedRef<IUeAgentAssetProjectReadEnvironment>
				Environment,
			TSharedPtr<IUeAgentAssetProjectReadHook>& OutHook,
			FUeAgentAssetProjectReadError& OutError);
	};
}
