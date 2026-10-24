#include "Domains/Reflection/Authoring/UeAgentReflectionAuthoringGenerationInput.h"
#include "Domains/Reflection/Authoring/UeAgentReflectionAuthoringDescriptors.h"
#include "Domains/Reflection/Authoring/UeAgentReflectionAuthoringProfileState.h"
#include "Core/HjsonWorkspace/UeAgentHjsonWorkspace.h"
#include "Core/StateSemantic/UeAgentCanonicalValueJson.h"
#include "DomainAdapters/AssetDataSimpleEffect/UeAgentAssetDataSimpleEffectTextureSourceMechanics.h"
#include "DomainAdapters/AssetDataSimpleEffect/UeAgentAssetDataSimpleEffectTextureSourceProjection.h"
#include "Utilities/UeAgentPropertyValueCodec.h"
#include "Utilities/AuthoringCore/UeAgentAuthoringDiagnostics.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "UObject/Class.h"
#include "Engine/TextureCube.h"
#include "Engine/Texture2D.h"
#include "Engine/VolumeTexture.h"

namespace UeAgentReflectionAuthoring::GenerationInput
{
namespace Native = UE::UAI::DomainAdapters::AssetDataSimpleEffect;
namespace
{
const TCHAR* CubeFaces[] = {TEXT("positive_x"), TEXT("negative_x"), TEXT("positive_y"),
    TEXT("negative_y"), TEXT("positive_z"), TEXT("negative_z")};
bool Fail(UE::UAI::Contract::FDiagnostic& Error, const UE::UAI::Contract::FDiagnostic& Diagnostic) { Error = Diagnostic; return false; }
UTexture* ResolveTarget(const FProfile& Profile, UObject& Target)
{
    if ((Profile.TypeId == TEXT("texture_cube") && Target.GetClass() == UTextureCube::StaticClass())
        || (Profile.TypeId == TEXT("volume_texture") && Target.GetClass() == UVolumeTexture::StaticClass()))
        return CastChecked<UTexture>(&Target);
    return nullptr;
}
UE::UAI::Contract::FDiagnostic InvalidTarget(const FProfile& Profile, UObject& Target)
{
    auto Error = UE::UAI::Contract::FDiagnostic::Failure(TEXT("generation_input_target_invalid"),
        TEXT("Generation input requires the exact native texture class declared by a supported profile."), TEXT("generation_input"));
    Error.SourceKind = TEXT("native_result");
    Error.TargetPath = Target.GetPathName();
    Error.PropertyPath = TEXT("target_class");
    auto Actual = MakeShared<FJsonObject>();
    Actual->SetStringField(TEXT("profile_type"), Profile.TypeId);
    Actual->SetStringField(TEXT("asset_class"), Target.GetClass()->GetPathName());
    Error.ActualValue = MakeShared<FJsonValueObject>(Actual);
    Error.ExpectedValue = MakeShared<FJsonValueString>(Profile.TypeId == TEXT("texture_cube")
        ? UTextureCube::StaticClass()->GetPathName() : Profile.TypeId == TEXT("volume_texture")
        ? UVolumeTexture::StaticClass()->GetPathName() : TEXT("texture_cube or volume_texture profile"));
    return Error;
}
}
struct FPreparedInput
{
    Native::FTextureSourcePreparedState Source;
    UE::UAI::Contract::FSourceLocation InputSource;
    FString SemanticAssetPath;

    void BindFailure(UE::UAI::Contract::FDiagnostic& Error, const UObject* Target) const
    {
        ProfileState::BindTarget(Error, Target ? Target->GetPathName() : FString(), SemanticAssetPath);
        // A native failure keeps its own provenance. The original generation field
        // is related input, not a guessed failing face, slice, or native property.
        if (InputSource.SourceFile.IsEmpty() && InputSource.JsonPath.IsEmpty()) return;
        auto Related = MakeShared<FJsonObject>();
        Related->SetStringField(TEXT("source_kind"), TEXT("related_input"));
        if (!InputSource.SourceFile.IsEmpty()) Related->SetStringField(TEXT("source_file"), InputSource.SourceFile);
        if (!InputSource.JsonPath.IsEmpty()) Related->SetStringField(TEXT("json_path"), InputSource.JsonPath);
        if (InputSource.Line > 0) Related->SetNumberField(TEXT("line"), InputSource.Line);
        if (InputSource.Column > 0) Related->SetNumberField(TEXT("column"), InputSource.Column);
        Error.RelatedSources.Add(MakeShared<FJsonValueObject>(Related));
    }
};

bool ReadTemplate(const FProfile& Profile, TSharedPtr<FJsonValue>& OutValue, UE::UAI::Contract::FDiagnostic& OutError)
{
    auto Input = MakeShared<FJsonObject>();
    if (Profile.TypeId == TEXT("texture_cube"))
        for (const TCHAR* Face : CubeFaces) Input->SetStringField(Face, FString());
    else if (Profile.TypeId == TEXT("volume_texture")) Input->SetArrayField(TEXT("slices"), {});
    else {
        OutError = UE::UAI::Contract::FDiagnostic::Failure(TEXT("generation_input_profile_unsupported"),
            TEXT("Generation input templates support only cube and volume texture profiles."), TEXT("generation_input"));
        OutError.SourceKind = TEXT("native_result");
        OutError.PropertyPath = TEXT("profile_type");
        OutError.ActualValue = MakeShared<FJsonValueString>(Profile.TypeId);
        OutError.ExpectedValue = MakeShared<FJsonValueString>(TEXT("texture_cube or volume_texture"));
        return false;
    }
    OutValue = MakeShared<FJsonValueObject>(Input);
    return true;
}

bool ReadGeneratedState(const FProfile& Profile, UObject& Target, TSharedPtr<FJsonValue>& OutValue, UE::UAI::Contract::FDiagnostic& OutError)
{
    auto* Texture = ResolveTarget(Profile, Target);
    if (!Texture)
        return Fail(OutError, InvalidTarget(Profile, Target));
    auto Resources = UE::UAI::HjsonWorkspace::MakeSemanticWorkspaceResourceTracker();
    UE::UAI::StateSemantic::FUeAgentCanonicalValuePtr State;
    UE::UAI::ExecutionMutation::FExecutionMutationError Error;
    if (!Native::TryReadAssembledTextureSourceState(*Texture, Resources, State, Error))
        return Fail(OutError, Error);
    return UE::UAI::StateSemantic::CanonicalJson::TryToJsonValue(*State, OutValue, OutError);
}

bool Prepare(const FProfile& Profile, UObject& Target, const TSharedPtr<const FJsonObject>& Extension,
    const FString& SemanticAssetPath, TSharedPtr<FPreparedInput>& OutInput, UE::UAI::Contract::FDiagnostic& OutError)
{
    OutInput.Reset();
    if (Profile.GenerationInputField.IsEmpty() || !Extension.IsValid()
        || !Extension->HasField(Profile.GenerationInputField)) return true;
    auto* Texture = ResolveTarget(Profile, Target);
    if (!Texture) {
        OutError = InvalidTarget(Profile, Target);
        ProfileState::BindTarget(OutError, Target.GetPathName(), SemanticAssetPath);
        return false;
    }
    const FString InputPath = TEXT("/") + Profile.GenerationInputField.Replace(TEXT("~"), TEXT("~0")).Replace(TEXT("/"), TEXT("~1"));
    const auto Invalid = [&](const FString& Code, const FString& Path, const FString& Expected,
        const TSharedPtr<FJsonValue>& Actual, const TCHAR* Reason) {
        auto Error = UeAgentAuthoringDiagnostics::MakeValueFailure(Code, Path,
            MakeShared<FJsonValueString>(Expected), Actual, TEXT("generation_input"), Reason);
        Error.TargetPath = SemanticAssetPath;
        Error.SourceKind = TEXT("authored_value");
        Error.Source.InternalJsonPath = Path;
        UeAgentAuthoringDiagnostics::LocateAuthoringSource(Error, *Extension);
        return Error;
    };
    const TSharedPtr<FJsonObject>* Input = nullptr;
    if (!Extension->TryGetObjectField(Profile.GenerationInputField, Input) || !Input || !Input->IsValid())
        return Fail(OutError, Invalid(TEXT("generation_input_object_required"), InputPath,
            TEXT("object"), Extension->TryGetField(Profile.GenerationInputField), TEXT("Generation input must be an object.")));
    TArray<TSharedPtr<FJsonValue>> Values;
    TArray<FString> Labels;
    if (Profile.TypeId == TEXT("texture_cube"))
    {
        if ((*Input)->Values.Num() != UE_ARRAY_COUNT(CubeFaces))
            return Fail(OutError, Invalid(TEXT("texture_cube_six_faces_required"), InputPath,
                TEXT("positive_x, negative_x, positive_y, negative_y, positive_z, negative_z"),
                MakeShared<FJsonValueObject>(*Input), TEXT("Cube generation input must provide exactly the six named faces.")));
        bool bEmptyTemplate = true;
        UeAgentAuthoringDiagnostics::FDiagnostic MissingFaces;
        for (const TCHAR* Face : CubeFaces)
        {
            FString Value;
            if (!(*Input)->HasField(Face))
            {
                MissingFaces.Append(Invalid(TEXT("texture_cube_face_missing"), InputPath + TEXT("/") + Face,
                    TEXT("Texture2D reference"), nullptr, TEXT("The required cube face is missing.")));
                continue;
            }
            bEmptyTemplate &= (*Input)->TryGetStringField(Face, Value) && Value.IsEmpty();
            Values.Add((*Input)->TryGetField(Face)); Labels.Add(Face);
        }
        if (!MissingFaces.Code.IsEmpty()) return Fail(OutError, MissingFaces);
        if (bEmptyTemplate) return true;
    }
    else
    {
        const TArray<TSharedPtr<FJsonValue>>* Slices = nullptr;
        if ((*Input)->Values.Num() != 1 || !(*Input)->TryGetArrayField(TEXT("slices"), Slices) || !Slices)
            return Fail(OutError, Invalid(TEXT("volume_texture_slices_required"), InputPath,
                TEXT("object containing a slices array"), MakeShared<FJsonValueObject>(*Input), TEXT("Volume generation input must contain only a slices array.")));
        // The empty exported template does not request source replacement.
        if (Slices->IsEmpty()) return true;
        Values = *Slices;
        for (int32 Index = 0; Index < Values.Num(); ++Index) Labels.Add(FString::FromInt(Index));
    }
    TArray<UTexture2D*> Sources;
    UeAgentAuthoringDiagnostics::FDiagnostic InvalidSources;
    for (int32 Index = 0; Index < Values.Num(); ++Index)
    {
        UObject* Source = nullptr;
        UE::UAI::Contract::FDiagnostic Error;
        if (!UeAgentPropertyValueCodec::DecodeObjectReference(Values[Index], UTexture2D::StaticClass(),
            nullptr, Source, Error) || !Source || Source->GetClass() != UTexture2D::StaticClass())
        {
            const bool bCube = Profile.TypeId == TEXT("texture_cube");
            auto Diagnostic = Invalid(bCube ? TEXT("texture_cube_face_invalid") : TEXT("volume_texture_slice_invalid"),
                InputPath + (bCube ? TEXT("/") : TEXT("/slices/")) + Labels[Index], TEXT("an exact Texture2D reference"), Values[Index],
                bCube ? TEXT("The cube face must reference a concrete Texture2D asset.") : TEXT("The volume slice must reference a concrete Texture2D asset."));
            if (!Error.IsEmpty())
            {
                Diagnostic.CauseCode = Error.Code;
                Diagnostic.PublicCause = MakeShared<UE::UAI::Contract::FDiagnostic>(Error);
            }
            InvalidSources.Append(Diagnostic);
            continue;
        }
        Sources.Add(CastChecked<UTexture2D>(Source));
    }
    if (!InvalidSources.Code.IsEmpty()) return Fail(OutError, InvalidSources);
    auto Prepared = MakeShared<FPreparedInput>();
    Prepared->SemanticAssetPath = SemanticAssetPath;
    UE::UAI::Contract::FDiagnostic InputLocation;
    InputLocation.SourceKind = TEXT("authored_value");
    InputLocation.Source.InternalJsonPath = InputPath;
    UeAgentAuthoringDiagnostics::LocateAuthoringSource(InputLocation, *Extension, Profile.GenerationInputField);
    Prepared->InputSource = MoveTemp(InputLocation.Source);
    UE::UAI::ExecutionMutation::FExecutionMutationError Error;
    if (!Native::TryPrepareResolvedTextureSourceAssembly(*Texture, Sources, Prepared->Source, Error)) {
        Prepared->BindFailure(Error, &Target);
        return Fail(OutError, Error);
    }
    OutInput = MoveTemp(Prepared);
    return true;
}

bool Apply(UObject& Target, const TSharedPtr<FPreparedInput>& Input, UE::UAI::Contract::FDiagnostic& OutError)
{
    if (!Input.IsValid()) return true;
    UE::UAI::ExecutionMutation::FExecutionMutationError Error;
    if (!Native::TryApplyTextureSourceReplacement(&Target, Input->Source, Error)) {
        Input->BindFailure(Error, &Target);
        return Fail(OutError, Error);
    }
    return true;
}

bool Verify(UObject* Target, const TSharedPtr<FPreparedInput>& Input, UE::UAI::Contract::FDiagnostic& OutError)
{
    if (!Input.IsValid()) return true;
    UE::UAI::ExecutionMutation::FExecutionMutationError Error;
    if (!Native::TryVerifyTextureSourceAssembly(Target, Input->Source, Error)) {
        Input->BindFailure(Error, Target);
        return Fail(OutError, Error);
    }
    return true;
}
}
