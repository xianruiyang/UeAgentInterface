#include "DomainAdapters/WorldRead/UeAgentWorldReadProductionEnvironment.h"

#include "Editor.h"
#include "Engine/Engine.h"
#include "Engine/World.h"

namespace UE::UAI::DomainAdapters::WorldRead
{
	namespace
	{
		bool Fail(
			FUeAgentWorldReadError& OutError,
			FString Code,
			FString SubjectId,
			FString Detail = FString())
		{
			OutError.Code = MoveTemp(Code);
			OutError.SubjectId = MoveTemp(SubjectId);
			OutError.Detail = MoveTemp(Detail);
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
		return IsValid(OutWorld)
			? true
			: Fail(
				OutError,
				TEXT("world_read_world_unresolved"),
				Expected);
	}

	bool FUeAgentWorldReadProductionEnvironment::TryResolveTarget(
		const UE::UAI::Discovery::FUeAgentCandidateBinding& Target,
		UObject*& OutObject,
		FUeAgentWorldReadError& OutError)
	{
		OutError = {};
		OutObject = ResolveObject(Target.GetPublicRef());
		return IsValid(OutObject)
			? true
			: Fail(
				OutError,
				TEXT("world_read_target_unresolved"),
				Target.GetPublicRef());
	}
}
