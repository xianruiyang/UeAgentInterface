// Copyright Epic Games, Inc. All Rights Reserved.

#include "Domains/Level/Authoring/UeAgentLevelAuthoringSpatial.h"
#include "Utilities/UeAgentSpaceContract.h"

#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"

namespace UeAgentLevelAuthoring
{
	namespace
	{
		bool ReadComponents3(
			const TSharedPtr<const FJsonObject>& Parent,
			const TCHAR* Field,
			FVector& OutValue,
			bool& bOutPresent,
			FString& OutError)
		{
			bOutPresent = false;
			if (!Parent.IsValid() || !Parent->HasField(Field)) return true;
			const TSharedPtr<FJsonObject>* ValueObject = nullptr;
			const TArray<TSharedPtr<FJsonValue>>* Components = nullptr;
			if (!Parent->TryGetObjectField(Field, ValueObject)
				|| !ValueObject || !ValueObject->IsValid()
				|| !(*ValueObject)->TryGetArrayField(TEXT("components"), Components)
				|| !Components || Components->Num() != 3)
			{
				OutError = FString::Printf(TEXT("level_%s_components_invalid"), Field);
				return false;
			}
			double X = 0.0;
			double Y = 0.0;
			double Z = 0.0;
			if (!(*Components)[0].IsValid() || !(*Components)[0]->TryGetNumber(X)
				|| !(*Components)[1].IsValid() || !(*Components)[1]->TryGetNumber(Y)
				|| !(*Components)[2].IsValid() || !(*Components)[2]->TryGetNumber(Z)
				|| !FMath::IsFinite(X) || !FMath::IsFinite(Y) || !FMath::IsFinite(Z))
			{
				OutError = FString::Printf(TEXT("level_%s_components_non_finite"), Field);
				return false;
			}
			OutValue = FVector(X, Y, Z);
			bOutPresent = true;
			return true;
		}

		bool ValidateSpaceContract(
			const TSharedPtr<const FJsonObject>& TransformObject,
			const ETransformFrame Frame,
			FString& OutError)
		{
			const TSharedPtr<FJsonObject>* Contract = nullptr;
			if (!TransformObject.IsValid()
				|| !TransformObject->TryGetObjectField(TEXT("space_contract"), Contract)
				|| !Contract || !Contract->IsValid())
			{
				OutError = TEXT("level_space_contract_missing");
				return false;
			}

			const bool bActor = Frame == ETransformFrame::ActorWorld;
			return UeAgentSpaceContract::Validate3D(
				*Contract,
				bActor ? TEXT("actor.transform") : TEXT("component.relative_transform"),
				bActor
					? TEXT("ue_actor_world_transform")
					: TEXT("ue_scene_component_parent_relative_transform"),
				TEXT("level_space_contract"),
				OutError);
		}

		TSharedRef<FJsonObject> Components(const FVector& Value)
		{
			TSharedRef<FJsonObject> Result = MakeShared<FJsonObject>();
			Result->SetArrayField(TEXT("components"), {
				MakeShared<FJsonValueNumber>(Value.X),
				MakeShared<FJsonValueNumber>(Value.Y),
				MakeShared<FJsonValueNumber>(Value.Z),
			});
			return Result;
		}

		TSharedRef<FJsonObject> RotatorComponents(const FRotator& Value)
		{
			TSharedRef<FJsonObject> Result = MakeShared<FJsonObject>();
			Result->SetArrayField(TEXT("components"), {
				MakeShared<FJsonValueNumber>(Value.Pitch),
				MakeShared<FJsonValueNumber>(Value.Yaw),
				MakeShared<FJsonValueNumber>(Value.Roll),
			});
			return Result;
		}
	}

	bool ParseTransformPatch(
		const TSharedPtr<const FJsonObject>& Object,
		const ETransformFrame Frame,
		FTransformPatch& OutPatch,
		FString& OutError)
	{
		OutPatch = FTransformPatch();
		OutError.Reset();
		if (!Object.IsValid()) return true;

		FVector Translation;
		FVector Rotation;
		FVector Scale;
		bool bTranslation = false;
		bool bRotation = false;
		bool bScale = false;
		if (!ReadComponents3(Object, TEXT("translation"), Translation, bTranslation, OutError)
			|| !ReadComponents3(Object, TEXT("rotation"), Rotation, bRotation, OutError)
			|| !ReadComponents3(Object, TEXT("scale"), Scale, bScale, OutError))
		{
			return false;
		}
		if (!bTranslation && !bRotation && !bScale)
		{
			OutError = TEXT("level_transform_empty");
			return false;
		}
		if (!ValidateSpaceContract(Object, Frame, OutError)) return false;
		if (bTranslation) OutPatch.Translation = Translation;
		if (bRotation) OutPatch.Rotation = FRotator(Rotation.X, Rotation.Y, Rotation.Z);
		if (bScale) OutPatch.Scale = Scale;
		return true;
	}

	void ApplyTransformPatch(const FTransformPatch& Patch, FTransform& InOutTransform)
	{
		if (Patch.Translation.IsSet()) InOutTransform.SetTranslation(Patch.Translation.GetValue());
		if (Patch.Rotation.IsSet()) InOutTransform.SetRotation(Patch.Rotation.GetValue().Quaternion());
		if (Patch.Scale.IsSet()) InOutTransform.SetScale3D(Patch.Scale.GetValue());
	}

	TSharedRef<FJsonObject> BuildTransformState(
		const FTransform& Transform,
		const ETransformFrame Frame)
	{
		TSharedRef<FJsonObject> Result = MakeShared<FJsonObject>();
		Result->SetObjectField(TEXT("translation"), Components(Transform.GetTranslation()));
		Result->SetObjectField(TEXT("rotation"), RotatorComponents(Transform.Rotator()));
		Result->SetObjectField(TEXT("scale"), Components(Transform.GetScale3D()));

		const bool bActor = Frame == ETransformFrame::ActorWorld;
		Result->SetObjectField(
			TEXT("space_contract"),
			UeAgentSpaceContract::Make3D(
				bActor ? TEXT("actor.transform") : TEXT("component.relative_transform"),
				bActor
					? TEXT("ue_actor_world_transform")
					: TEXT("ue_scene_component_parent_relative_transform")));
		return Result;
	}
}
