#include "DomainAdapters/WorldSimpleEffect/UeAgentWorldSimpleEffectProduction.h"

#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Engine/Engine.h"
#include "Engine/Level.h"
#include "Engine/World.h"

namespace UE::UAI::DomainAdapters::WorldSimpleEffect
{
	using namespace ContextResource;
	using namespace ExecutionMutation;

	namespace
	{
		bool Fail(FExecutionMutationError& OutError, FExecutionMutationError Diagnostic, const FString& InternalContext = FString())
		{
			OutError = MoveTemp(Diagnostic);
			if (OutError.InternalContext.IsEmpty()) OutError.InternalContext = InternalContext;
			return false;
		}

		FExecutionMutationError NativeFact(const FString& Code, const FString& Reason, const FString& Path,
			TSharedPtr<FJsonValue> Actual, TSharedPtr<FJsonValue> Expected)
		{
			auto Error = FExecutionMutationError::Failure(Code, Reason, TEXT("execution"));
			Error.SourceKind = TEXT("native_result");
			Error.PropertyPath = Path;
			Error.ActualValue = MoveTemp(Actual);
			Error.ExpectedValue = MoveTemp(Expected);
			return Error;
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

		bool ValidateContext(const FUeAgentExecutionContextRef& Context, FExecutionMutationError& OutError)
		{
			if (Context.WasResolvedFromCurrent()) return Fail(OutError, NativeFact(TEXT("world_simple_context_invalid"),
				TEXT("Execution requires an explicitly bound world context instead of a current-world resolution."), TEXT("/execution_context/resolution"),
				MakeShared<FJsonValueBoolean>(true), MakeShared<FJsonValueBoolean>(false)));
			if (Context.GetWorldIdentity().IsEmpty()) return Fail(OutError, NativeFact(TEXT("world_simple_context_invalid"),
				TEXT("Execution context has no world binding."), TEXT("/execution_context/world"),
				MakeShared<FJsonValueBoolean>(false), MakeShared<FJsonValueBoolean>(true)));
			if (Context.GetLevelIdentity().IsEmpty()) return Fail(OutError, NativeFact(TEXT("world_simple_context_invalid"),
				TEXT("Execution context has no level binding."), TEXT("/execution_context/level"),
				MakeShared<FJsonValueBoolean>(false), MakeShared<FJsonValueBoolean>(true)));
			if (Context.GetContextRevision().IsEmpty()) return Fail(OutError, NativeFact(TEXT("world_simple_context_invalid"),
				TEXT("Execution context has no revision binding."), TEXT("/execution_context/revision"),
				MakeShared<FJsonValueBoolean>(false), MakeShared<FJsonValueBoolean>(true)));
			if (Context.GetWorldType() == EUeAgentExecutionWorldType::Invalid)
				return Fail(OutError, NativeFact(TEXT("world_simple_context_invalid"),
					TEXT("Execution context has an invalid world type."), TEXT("/execution_context/world_type"),
					MakeShared<FJsonValueNumber>(static_cast<int32>(Context.GetWorldType())), MakeShared<FJsonValueString>(TEXT("supported world type"))));
			if (Context.GetWorldType() == EUeAgentExecutionWorldType::Pie && !Context.GetPieInstanceId().IsSet())
				return Fail(OutError, NativeFact(TEXT("world_simple_context_invalid"),
					TEXT("PIE execution context requires a PIE instance."), TEXT("/execution_context/pie_instance"),
					MakeShared<FJsonValueBoolean>(false), MakeShared<FJsonValueBoolean>(true)));
			return true;
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
		if (!IsInGameThread())
			return Fail(OutError, NativeFact(TEXT("world_simple_context_catalog_unavailable"),
				TEXT("World contexts must be enumerated on the game thread."), TEXT("/game_thread"),
				MakeShared<FJsonValueBoolean>(false), MakeShared<FJsonValueBoolean>(true)));
		if (!GEngine)
			return Fail(OutError, NativeFact(TEXT("world_simple_context_catalog_unavailable"),
				TEXT("Engine is unavailable for world-context enumeration."), TEXT("/engine"),
				MakeShared<FJsonValueBoolean>(false), MakeShared<FJsonValueBoolean>(true)));

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
		if (!ValidateContext(Context, OutError)) return false;

		TArray<FUeAgentWorldSimpleEffectWorldCandidate> Candidates;
		if (!Catalog->TryEnumerate(Candidates, OutError))
		{
			if (OutError.Code.IsEmpty())
			{
				Fail(OutError, NativeFact(TEXT("world_simple_context_catalog_unavailable"),
					TEXT("World catalog failed without returning a diagnostic."), TEXT("/world_catalog/diagnostic"),
					MakeShared<FJsonValueBoolean>(false), MakeShared<FJsonValueBoolean>(true)));
			}
			return false;
		}

		bool bSawInvalid = false;
		bool bSawStale = false;
		FExecutionMutationError InvalidCandidateError;
		FExecutionMutationError StaleCandidateError;
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
			if (StaleCandidateError.IsEmpty())
			{
				StaleCandidateError = NativeFact(TEXT("world_simple_context_stale"),
					TEXT("Matching world context has changed since the request was bound."), TEXT("/execution_context/revision"),
					MakeShared<FJsonValueBoolean>(false), MakeShared<FJsonValueBoolean>(true));
				StaleCandidateError.ActualEvidence = MakeShared<FJsonValueString>(Candidate.GetContextRevision());
				StaleCandidateError.ExpectedEvidence = MakeShared<FJsonValueString>(Context.GetContextRevision());
				if (UWorld* World = Candidate.GetWorld()) StaleCandidateError.TargetPath = World->GetPathName();
			}
			continue;
			}
			if (!Candidate.IsValid())
			{
			bSawInvalid = true;
			if (InvalidCandidateError.IsEmpty())
			{
				const auto Facts = MakeShared<FJsonObject>();
				Facts->SetBoolField(TEXT("world_valid"), ::IsValid(Candidate.GetWorld()));
				Facts->SetBoolField(TEXT("world_binding_present"), !Candidate.GetWorldIdentity().IsEmpty());
				Facts->SetBoolField(TEXT("level_binding_present"), !Candidate.GetLevelIdentity().IsEmpty());
				Facts->SetBoolField(TEXT("world_type_valid"), Candidate.GetWorldType() != EUeAgentExecutionWorldType::Invalid);
				Facts->SetBoolField(TEXT("revision_present"), !Candidate.GetContextRevision().IsEmpty());
				Facts->SetBoolField(TEXT("required_pie_instance_present"), Candidate.GetWorldType() != EUeAgentExecutionWorldType::Pie || Candidate.GetPieInstanceId().IsSet());
				InvalidCandidateError = NativeFact(TEXT("world_simple_context_invalid"),
					TEXT("Matching world candidate is invalid or has incomplete binding information."), TEXT("/world_candidate"),
					MakeShared<FJsonValueObject>(Facts), MakeShared<FJsonValueString>(TEXT("valid world with complete binding information")));
				if (UWorld* World = Candidate.GetWorld()) InvalidCandidateError.TargetPath = World->GetPathName();
			}
			continue;
			}
			ExactWorlds.Add(Candidate.GetWorld());
		}

		if (ExactWorlds.Num() > 1)
		{
			TArray<FString> WorldPaths;
			for (const UWorld* World : ExactWorlds) WorldPaths.AddUnique(World->GetPathName());
			TArray<TSharedPtr<FJsonValue>> Paths;
			for (const FString& Path : WorldPaths) Paths.Add(MakeShared<FJsonValueString>(Path));
			const auto Actual = MakeShared<FJsonObject>();
			Actual->SetNumberField(TEXT("count"), ExactWorlds.Num());
			Actual->SetArrayField(TEXT("world_paths"), MoveTemp(Paths));
			const auto Expected = MakeShared<FJsonObject>();
			Expected->SetNumberField(TEXT("count"), 1);
			return Fail(OutError, NativeFact(TEXT("world_simple_context_ambiguous"),
				TEXT("More than one valid world matches the bound execution context."), TEXT("/world_candidates"),
				MakeShared<FJsonValueObject>(Actual), MakeShared<FJsonValueObject>(Expected)), Context.GetWorldIdentity());
		}
		if (ExactWorlds.Num() == 1)
		{
			OutWorld = ExactWorlds[0];
			return true;
		}
		if (bSawInvalid)
		{
			return Fail(OutError, MoveTemp(InvalidCandidateError), Context.GetWorldIdentity());
		}
		if (bSawStale)
		{
			return Fail(OutError, MoveTemp(StaleCandidateError), Context.GetWorldIdentity());
		}
		return Fail(OutError, NativeFact(TEXT("world_simple_context_unresolved"),
			TEXT("No valid world matches the bound world, level, type and PIE instance."), TEXT("/world_candidates"),
			MakeShared<FJsonValueNumber>(ExactWorlds.Num()), MakeShared<FJsonValueNumber>(1)), Context.GetWorldIdentity());
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

