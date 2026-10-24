// Copyright Epic Games, Inc. All Rights Reserved.

#include "Domains/Level/Authoring/UeAgentLevelAuthoringSpatial.h"

#include "Core/Contract/UeAgentDiagnostic.h"
#include "Utilities/AuthoringCore/UeAgentAuthoringDiagnostics.h"
#include "Utilities/AuthoringCore/UeAgentAuthoringEncodedTypes.h"
#include "Utilities/AuthoringCore/UeAgentAuthoringTypeDescriptors.h"
#include "Utilities/UeAgentPropertyValueCodec.h"
#include "Utilities/UeAgentSpaceContract.h"

#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "UObject/NoExportTypes.h"

namespace UeAgentLevelAuthoring
{
	namespace
	{
		UE::UAI::Contract::FDiagnostic TransformFailure(
			const FString& Code, const FString& Reason, const FJsonObject& Input,
			const FString& Field, TSharedPtr<FJsonValue> Actual,
			TSharedPtr<FJsonValue> Expected, const FString& Pointer = FString())
		{
			auto Error = UE::UAI::Contract::FDiagnostic::Failure(Code, Reason, TEXT("input_decode"));
			Error.PropertyPath = Field;
			Error.ActualValuePresent = Actual.IsValid();
			Error.ActualValue = MoveTemp(Actual);
			Error.ExpectedValue = MoveTemp(Expected);
			Error.Source.JsonPath = Pointer;
			UeAgentAuthoringDiagnostics::LocateAuthoringSource(Error, Input, Field);
			return Error;
		}

		bool ReadComponents3(
			const TSharedPtr<const FJsonObject>& Parent,
			const TCHAR* Field,
			FVector& OutValue,
			bool& bOutPresent,
			UE::UAI::Contract::FDiagnostic& OutError)
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
				const bool bObject = ValueObject && ValueObject->IsValid();
				const bool bArray = bObject && Components;
				OutError = TransformFailure(
					FString::Printf(TEXT("level_%s_components_invalid"), Field),
					FString::Printf(TEXT("Transform '%s' must contain a 'components' array of exactly three numbers."), Field),
					bObject ? **ValueObject : *Parent, bObject ? TEXT("components") : Field,
					bArray ? TSharedPtr<FJsonValue>(MakeShared<FJsonValueNumber>(Components->Num()))
						: bObject ? (*ValueObject)->TryGetField(TEXT("components")) : Parent->TryGetField(Field),
					bArray ? TSharedPtr<FJsonValue>(MakeShared<FJsonValueNumber>(3))
						: MakeShared<FJsonValueString>(bObject ? TEXT("array of three numbers") : TEXT("object with components")));
				return false;
			}
			double Values[3] = {};
			// Preserve the original order: decode all components before checking finiteness.
			for (int32 Index = 0; Index < 3; ++Index)
			{
				if (!(*Components)[Index].IsValid() || !(*Components)[Index]->TryGetNumber(Values[Index]))
				{
					OutError = TransformFailure(
						FString::Printf(TEXT("level_%s_components_non_finite"), Field),
						FString::Printf(TEXT("Transform '%s' component %d must be a number."), Field, Index),
						**ValueObject, TEXT("components"), (*Components)[Index],
						MakeShared<FJsonValueString>(TEXT("finite number")),
						FString::Printf(TEXT("/components/%d"), Index));
					return false;
				}
			}
			for (int32 Index = 0; Index < 3; ++Index)
			{
				if (!FMath::IsFinite(Values[Index]))
				{
					const TCHAR* Value = FMath::IsNaN(Values[Index]) ? TEXT("NaN")
						: Values[Index] > 0.0 ? TEXT("Infinity") : TEXT("-Infinity");
					OutError = TransformFailure(
						FString::Printf(TEXT("level_%s_components_non_finite"), Field),
						FString::Printf(TEXT("Transform '%s' component %d must be finite."), Field, Index),
						**ValueObject, TEXT("components"), MakeShared<FJsonValueString>(Value),
						MakeShared<FJsonValueString>(TEXT("finite number")),
						FString::Printf(TEXT("/components/%d"), Index));
					return false;
				}
			}
			OutValue = FVector(Values[0], Values[1], Values[2]);
			bOutPresent = true;
			return true;
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
		const ETransformFrame /*Frame*/,
		FTransformPatch& OutPatch,
		UE::UAI::Contract::FDiagnostic& OutError)
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
			OutError = TransformFailure(TEXT("level_transform_empty"),
				TEXT("The transform patch must specify at least one of translation, rotation or scale."),
				*Object, FString(), MakeShared<FJsonValueNumber>(0),
				MakeShared<FJsonValueString>(TEXT("at least one transform component field")));
			return false;
		}
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
		const ETransformFrame Frame,
		const bool bIncludeFrameMetadata)
	{
		TSharedRef<FJsonObject> Result = MakeShared<FJsonObject>();
		const auto SetTransformVector = [&Result](const TCHAR* Field, const FVector& Native) {
			const auto Value = MakeShared<FJsonValueObject>(Components(Native));
			UeAgentAuthoringEncodedTypes::FScope::Record(Value,
				UeAgentAuthoringTypeDescriptors::DescribeStruct(TBaseStructure<FVector>::Get()),
				UeAgentPropertyValueCodec::MakeTransformVectorComparisonDefinition());
			Result->SetField(Field, Value);
		};
		SetTransformVector(TEXT("translation"), Transform.GetTranslation());
		Result->SetObjectField(TEXT("rotation"), RotatorComponents(Transform.Rotator()));
		SetTransformVector(TEXT("scale"), Transform.GetScale3D());

		if (bIncludeFrameMetadata)
		{
			const bool bActor = Frame == ETransformFrame::ActorWorld;
			Result->SetObjectField(
				TEXT("space_contract"),
				UeAgentSpaceContract::Make3D(
					bActor ? TEXT("actor.transform") : TEXT("component.relative_transform"),
					bActor
						? TEXT("ue_actor_world_transform")
						: TEXT("ue_scene_component_parent_relative_transform")));
		}
		return Result;
	}
}
