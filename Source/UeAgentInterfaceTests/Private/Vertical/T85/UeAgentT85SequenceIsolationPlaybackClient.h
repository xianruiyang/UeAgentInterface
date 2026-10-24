#pragma once

#include "CoreMinimal.h"
#include "IMovieScenePlaybackClient.h"
#include "UObject/Object.h"

#include "UeAgentT85SequenceIsolationPlaybackClient.generated.h"

/** Test-owned binding override for the original Proposed native-action probe. */
UCLASS()
class UUeAgentT85SequenceIsolationPlaybackClient : public UObject, public IMovieScenePlaybackClient
{
	GENERATED_BODY()

public:
	UPROPERTY(Transient)
	TMap<FGuid, TObjectPtr<UObject>> RootBindings;

	mutable int32 OverrideCalls = 0;
	mutable int32 DeniedCalls = 0;

	virtual bool RetrieveBindingOverrides(const FGuid& Binding, FMovieSceneSequenceID SequenceID,
		TArray<UObject*, TInlineAllocator<1>>& OutObjects) const override
	{
		++OverrideCalls;
		if (SequenceID == MovieSceneSequenceID::Root)
		{
			if (const TObjectPtr<UObject>* Object = RootBindings.Find(Binding))
			{
				if (IsValid(Object->Get())) OutObjects.Add(Object->Get());
				return false;
			}
		}
		++DeniedCalls;
		// UE 5.6 MovieScenePlayer.cpp uses the return value as bAllowDefault.
		// False denies all fallback, contrary to this version's header comment.
		return false;
	}

	virtual UObject* GetInstanceData() const override { return nullptr; }
};
