#include "DomainAdapters/WorldRead/UeAgentWorldReadProductionEnvironment.h"
#include "DomainAdapters/ContentDiscovery/UeAgentEditorContentDiscovery.h"

#include "Dom/JsonValue.h"
#include "Editor.h"
#include "Engine/Engine.h"
#include "Engine/World.h"

namespace UE::UAI::DomainAdapters::WorldRead
{
	namespace
	{
		bool Fail(FUeAgentWorldReadError& OutError, FUeAgentWorldReadError Diagnostic)
		{
			OutError = MoveTemp(Diagnostic);
			return false;
		}

		UObject* ResolveObject(const FString& PublicRef)
		{
			FString ObjectPath = PublicRef;
			ObjectPath.TrimStartAndEndInline();
			if (ObjectPath.IsEmpty())
			{
				return nullptr;
			}
			if (UObject* Existing = StaticFindObject(
				UObject::StaticClass(),
				nullptr,
				*ObjectPath))
			{
				return Existing;
			}
			return StaticLoadObject(
				UObject::StaticClass(),
				nullptr,
				*ObjectPath);
		}
	}

	bool FUeAgentWorldReadProductionEnvironment::TryResolveWorld(
		const UE::UAI::ContextResource::FUeAgentExecutionContextRef&
			Context,
		UWorld*& OutWorld,
		FUeAgentWorldReadError& OutError)
	{
		OutWorld = nullptr;
		OutError = {};
		const FString& Expected = Context.GetWorldIdentity();
		if (GEngine)
		{
			for (const FWorldContext& WorldContext
				: GEngine->GetWorldContexts())
			{
				UWorld* World = WorldContext.World();
				if (!IsValid(World))
				{
					continue;
				}
				if (Expected.IsEmpty()
					|| World->GetPathName() == Expected
					|| World->GetOutermost()->GetName() == Expected)
				{
					OutWorld = World;
					return true;
				}
			}
		}
		if (GEditor)
		{
			OutWorld = GEditor->GetEditorWorldContext().World();
		}
		if (IsValid(OutWorld)) return true;
		auto Diagnostic = FUeAgentWorldReadError::Failure(TEXT("world_read_world_unresolved"),
			TEXT("No live world could be resolved for this query execution context."), TEXT("world_read"), FString(), Expected);
		Diagnostic.SourceKind = TEXT("native_result");
		Diagnostic.PropertyPath = TEXT("/world");
		Diagnostic.ActualValue = MakeShared<FJsonValueBoolean>(false);
		Diagnostic.ExpectedValue = MakeShared<FJsonValueBoolean>(true);
		return Fail(OutError, MoveTemp(Diagnostic));
	}

	bool FUeAgentWorldReadProductionEnvironment::TryResolveTarget(
		const UE::UAI::Discovery::FUeAgentCandidateBinding& Target,
		UObject*& OutObject,
		FUeAgentWorldReadError& OutError)
	{
		OutError = {};
		OutObject = nullptr;
		FString ObjectPath = Target.GetPublicRef();
		UE::UAI::ContentDiscovery::FContentDiscoveryError ResolveError;
		if (!ContentDiscovery::TryResolveEditorContentReference(ObjectPath, FString(), ResolveError))
		{
			OutError = MoveTemp(ResolveError);
			if (OutError.TargetPath.IsEmpty()) OutError.TargetPath = Target.GetPublicRef();
			return false;
		}
		OutObject = ResolveObject(ObjectPath);
		if (IsValid(OutObject)) return true;
		auto Diagnostic = FUeAgentWorldReadError::Failure(TEXT("world_read_target_unresolved"),
			TEXT("The query target reference did not resolve to a valid object."), TEXT("world_read"));
		Diagnostic.SourceKind = TEXT("native_result");
		Diagnostic.TargetPath = Target.GetPublicRef();
		Diagnostic.PropertyPath = TEXT("/target");
		Diagnostic.ActualValue = MakeShared<FJsonValueString>(Target.GetPublicRef());
		Diagnostic.ExpectedValue = MakeShared<FJsonValueString>(TEXT("loadable valid object"));
		return Fail(OutError, MoveTemp(Diagnostic));
	}
}
