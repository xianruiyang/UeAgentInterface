#pragma once

#include "CoreMinimal.h"
#include "DomainAdapters/WorldSimpleEffect/UeAgentWorldSimpleEffectAdapter.h"
#include "Templates/SharedPointer.h"
#include "UObject/WeakObjectPtrTemplates.h"

#define UAI_WORLD_SIMPLE_EFFECT_PRODUCTION_V1 1

namespace UE::UAI::DomainAdapters::WorldSimpleEffect
{
	class UEAGENTINTERFACE_API
		FUeAgentWorldSimpleEffectWorldCandidate final
	{
	public:
		FUeAgentWorldSimpleEffectWorldCandidate(
			UWorld* InWorld,
			FString InWorldIdentity,
			FString InLevelIdentity,
			ContextResource::EUeAgentExecutionWorldType InWorldType,
			TOptional<int32> InPieInstanceId,
			FString InContextRevision);

		UWorld* GetWorld() const;
		const FString& GetWorldIdentity() const;
		const FString& GetLevelIdentity() const;
		ContextResource::EUeAgentExecutionWorldType
			GetWorldType() const;
		const TOptional<int32>& GetPieInstanceId() const;
		const FString& GetContextRevision() const;
		bool IsValid() const;

	private:
		TWeakObjectPtr<UWorld> World;
		FString WorldIdentity;
		FString LevelIdentity;
		ContextResource::EUeAgentExecutionWorldType WorldType =
			ContextResource::EUeAgentExecutionWorldType::Invalid;
		TOptional<int32> PieInstanceId;
		FString ContextRevision;
	};

	class UEAGENTINTERFACE_API
		IUeAgentWorldSimpleEffectWorldCatalog
	{
	public:
		virtual ~IUeAgentWorldSimpleEffectWorldCatalog() = default;

		virtual bool TryEnumerate(
			TArray<FUeAgentWorldSimpleEffectWorldCandidate>&
				OutCandidates,
			ExecutionMutation::FExecutionMutationError& OutError) = 0;
	};

	class UEAGENTINTERFACE_API
		FUeAgentWorldSimpleEffectEngineWorldCatalog final
		: public IUeAgentWorldSimpleEffectWorldCatalog
	{
	public:
		bool TryEnumerate(
			TArray<FUeAgentWorldSimpleEffectWorldCandidate>&
				OutCandidates,
			ExecutionMutation::FExecutionMutationError& OutError) override;
	};

	class UEAGENTINTERFACE_API
		FUeAgentWorldSimpleEffectExactWorldResolver final
		: public IUeAgentWorldSimpleEffectWorldResolver
	{
	public:
		explicit FUeAgentWorldSimpleEffectExactWorldResolver(
			TSharedRef<
				IUeAgentWorldSimpleEffectWorldCatalog,
				ESPMode::ThreadSafe> InCatalog);

		bool TryResolveWorld(
			const ContextResource::FUeAgentExecutionContextRef& Context,
			UWorld*& OutWorld,
			ExecutionMutation::FExecutionMutationError& OutError) override;

	private:
		TSharedRef<
			IUeAgentWorldSimpleEffectWorldCatalog,
			ESPMode::ThreadSafe> Catalog;
	};

	class UEAGENTINTERFACE_API
		FUeAgentWorldSimpleEffectProductionHost final
	{
	public:
		static TSharedRef<
			FUeAgentWorldSimpleEffectUeHook,
			ESPMode::ThreadSafe> Create(
				TSharedRef<
					IUeAgentWorldSimpleEffectWorldCatalog,
					ESPMode::ThreadSafe> Catalog,
				TSharedRef<
					IUeAgentWorldSimpleEffectNativeHook,
					ESPMode::ThreadSafe> NativeHook);
	};
}

