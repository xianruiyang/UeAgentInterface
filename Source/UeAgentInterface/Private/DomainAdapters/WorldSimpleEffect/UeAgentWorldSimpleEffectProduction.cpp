#include "DomainAdapters/WorldSimpleEffect/UeAgentWorldSimpleEffectProduction.h"

#include "Engine/Engine.h"
#include "Engine/Level.h"
#include "Engine/World.h"

namespace UE::UAI::DomainAdapters::WorldSimpleEffect
{
	using namespace ContextResource;
	using namespace ExecutionMutation;

	namespace
	{
		bool Fail(
			FExecutionMutationError& OutError,
			FString Code,
			FString Detail = FString())
		{
			OutError = FExecutionMutationError(
				MoveTemp(Code),
				MoveTemp(Detail));
			return false;
		}

		EUeAgentExecutionWorldType ConvertWorldType(
			const EWorldType::Type WorldType)
		{
			switch (WorldType)
			{
			case EWorldType::Editor:
				return EUeAgentExecutionWorldType::Editor;
			case EWorldType::PIE:
				return EUeAgentExecutionWorldType::Pie;
			case EWorldType::Game:
				return EUeAgentExecutionWorldType::Game;
			case EWorldType::GamePreview:
			case EWorldType::EditorPreview:
				return EUeAgentExecutionWorldType::Preview;
			default:
				return EUeAgentExecutionWorldType::Invalid;
			}
		}

		TOptional<int32> PieInstanceId(
			const FWorldContext& WorldContext,
			const EUeAgentExecutionWorldType WorldType)
		{
			return WorldType == EUeAgentExecutionWorldType::Pie
				&& WorldContext.PIEInstance != INDEX_NONE
				? TOptional<int32>(WorldContext.PIEInstance)
				: TOptional<int32>();
		}

		FString ContextRevision(
			const FWorldContext& WorldContext,
			const UWorld& World)
		{
			return FString::Printf(
				TEXT("world-context:%s:%s:%d:%u"),
				*World.GetPathName(),
				*WorldContext.ContextHandle.ToString(),
				WorldContext.PIEInstance,
				World.GetUniqueID());
		}

		bool SameScope(
			const FUeAgentWorldSimpleEffectWorldCandidate& Candidate,
			const FUeAgentExecutionContextRef& Context)
		{
			return Candidate.GetWorldIdentity()
					== Context.GetWorldIdentity()
				&& Candidate.GetLevelIdentity()
					== Context.GetLevelIdentity()
				&& Candidate.GetWorldType()
					== Context.GetWorldType()
				&& Candidate.GetPieInstanceId()
					== Context.GetPieInstanceId();
		}

		bool ContextIsValid(
			const FUeAgentExecutionContextRef& Context)
		{
			if (Context.WasResolvedFromCurrent()
				|| Context.GetWorldIdentity().IsEmpty()
				|| Context.GetLevelIdentity().IsEmpty()
				|| Context.GetContextRevision().IsEmpty()
				|| Context.GetWorldType()
					== EUeAgentExecutionWorldType::Invalid)
			{
				return false;
			}
			return Context.GetWorldType()
					!= EUeAgentExecutionWorldType::Pie
				|| Context.GetPieInstanceId().IsSet();
		}
	}

	FUeAgentWorldSimpleEffectWorldCandidate::
	FUeAgentWorldSimpleEffectWorldCandidate(
		UWorld* InWorld,
		FString InWorldIdentity,
		FString InLevelIdentity,
		const EUeAgentExecutionWorldType InWorldType,
		TOptional<int32> InPieInstanceId,
		FString InContextRevision)
		: World(InWorld)
		, WorldIdentity(MoveTemp(InWorldIdentity))
		, LevelIdentity(MoveTemp(InLevelIdentity))
		, WorldType(InWorldType)
		, PieInstanceId(MoveTemp(InPieInstanceId))
		, ContextRevision(MoveTemp(InContextRevision))
	{
	}

	UWorld* FUeAgentWorldSimpleEffectWorldCandidate::
	GetWorld() const
	{
		return World.Get();
	}

	const FString& FUeAgentWorldSimpleEffectWorldCandidate::
	GetWorldIdentity() const
	{
		return WorldIdentity;
	}

	const FString& FUeAgentWorldSimpleEffectWorldCandidate::
	GetLevelIdentity() const
	{
		return LevelIdentity;
	}

	EUeAgentExecutionWorldType
	FUeAgentWorldSimpleEffectWorldCandidate::
	GetWorldType() const
	{
		return WorldType;
	}

	const TOptional<int32>&
	FUeAgentWorldSimpleEffectWorldCandidate::
	GetPieInstanceId() const
	{
		return PieInstanceId;
	}

	const FString& FUeAgentWorldSimpleEffectWorldCandidate::
	GetContextRevision() const
	{
		return ContextRevision;
	}

	bool FUeAgentWorldSimpleEffectWorldCandidate::IsValid() const
	{
		return ::IsValid(World.Get())
			&& !WorldIdentity.IsEmpty()
			&& !LevelIdentity.IsEmpty()
			&& WorldType != EUeAgentExecutionWorldType::Invalid
			&& !ContextRevision.IsEmpty()
			&& (WorldType != EUeAgentExecutionWorldType::Pie
				|| PieInstanceId.IsSet());
	}

	bool FUeAgentWorldSimpleEffectEngineWorldCatalog::
	TryEnumerate(
		TArray<FUeAgentWorldSimpleEffectWorldCandidate>&
			OutCandidates,
		FExecutionMutationError& OutError)
	{
		OutCandidates.Reset();
		OutError = FExecutionMutationError();
		if (!IsInGameThread() || !GEngine)
		{
			return Fail(
				OutError,
				TEXT(
					"world_simple_context_catalog_unavailable"));
		}

		for (const FWorldContext& WorldContext
			: GEngine->GetWorldContexts())
		{
			UWorld* World = WorldContext.World();
			if (!::IsValid(World) || !World->PersistentLevel)
			{
				continue;
			}
			const EUeAgentExecutionWorldType WorldType =
				ConvertWorldType(World->WorldType);
			if (WorldType == EUeAgentExecutionWorldType::Invalid)
			{
				continue;
			}
			OutCandidates.Emplace(
				World,
				World->GetPathName(),
				World->PersistentLevel->GetPathName(),
				WorldType,
				PieInstanceId(WorldContext, WorldType),
				ContextRevision(WorldContext, *World));
		}
		return true;
	}

	FUeAgentWorldSimpleEffectExactWorldResolver::
	FUeAgentWorldSimpleEffectExactWorldResolver(
		TSharedRef<
			IUeAgentWorldSimpleEffectWorldCatalog,
			ESPMode::ThreadSafe> InCatalog)
		: Catalog(MoveTemp(InCatalog))
	{
	}

	bool FUeAgentWorldSimpleEffectExactWorldResolver::
	TryResolveWorld(
		const FUeAgentExecutionContextRef& Context,
		UWorld*& OutWorld,
		FExecutionMutationError& OutError)
	{
		OutWorld = nullptr;
		OutError = FExecutionMutationError();
		if (!ContextIsValid(Context))
		{
			return Fail(
				OutError,
				TEXT("world_simple_context_invalid"),
				Context.GetWorldIdentity());
		}

		TArray<FUeAgentWorldSimpleEffectWorldCandidate> Candidates;
		if (!Catalog->TryEnumerate(Candidates, OutError))
		{
			if (OutError.GetCode().IsEmpty())
			{
				Fail(
					OutError,
					TEXT(
						"world_simple_context_catalog_unavailable"));
			}
			return false;
		}

		bool bSawInvalid = false;
		bool bSawStale = false;
		TArray<UWorld*> ExactWorlds;
		for (const FUeAgentWorldSimpleEffectWorldCandidate& Candidate
			: Candidates)
		{
			if (!SameScope(Candidate, Context))
			{
				continue;
			}
			if (Candidate.GetContextRevision()
				!= Context.GetContextRevision())
			{
				bSawStale = true;
				continue;
			}
			if (!Candidate.IsValid())
			{
				bSawInvalid = true;
				continue;
			}
			ExactWorlds.Add(Candidate.GetWorld());
		}

		if (ExactWorlds.Num() > 1)
		{
			return Fail(
				OutError,
				TEXT("world_simple_context_ambiguous"),
				Context.GetWorldIdentity());
		}
		if (ExactWorlds.Num() == 1)
		{
			OutWorld = ExactWorlds[0];
			return true;
		}
		if (bSawInvalid)
		{
			return Fail(
				OutError,
				TEXT("world_simple_context_invalid"),
				Context.GetWorldIdentity());
		}
		if (bSawStale)
		{
			return Fail(
				OutError,
				TEXT("world_simple_context_stale"),
				Context.GetWorldIdentity());
		}
		return Fail(
			OutError,
			TEXT("world_simple_context_unresolved"),
			Context.GetWorldIdentity());
	}

	TSharedRef<
		FUeAgentWorldSimpleEffectUeHook,
		ESPMode::ThreadSafe>
	FUeAgentWorldSimpleEffectProductionHost::Create(
		TSharedRef<
			IUeAgentWorldSimpleEffectWorldCatalog,
			ESPMode::ThreadSafe> Catalog,
		TSharedRef<
			IUeAgentWorldSimpleEffectNativeHook,
			ESPMode::ThreadSafe> NativeHook)
	{
		const TSharedRef<
			FUeAgentWorldSimpleEffectExactWorldResolver,
			ESPMode::ThreadSafe> Resolver = MakeShared<
				FUeAgentWorldSimpleEffectExactWorldResolver,
				ESPMode::ThreadSafe>(MoveTemp(Catalog));
		return MakeShared<
			FUeAgentWorldSimpleEffectUeHook,
			ESPMode::ThreadSafe>(
				Resolver,
				MoveTemp(NativeHook));
	}
}

