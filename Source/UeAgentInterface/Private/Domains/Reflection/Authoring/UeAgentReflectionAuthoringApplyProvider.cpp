// Copyright Epic Games, Inc. All Rights Reserved.

#include "Domains/Reflection/Authoring/UeAgentReflectionAuthoringApplyProvider.h"

#include "Core/Contract/UeAgentDiagnostic.h"
#include "Utilities/AuthoringCore/UeAgentAuthoringDiagnostics.h"
#include "Domains/Reflection/Authoring/UeAgentReflectionAuthoringGenerationInput.h"

#include <type_traits>

#include "BehaviorTree/BlackboardData.h"
#include "ComponentReregisterContext.h"
#include "Components/RuntimeVirtualTextureComponent.h"
#include "EnhancedInputLibrary.h"
#include "InputMappingContext.h"
#include "Sound/SoundClass.h"
#include "Sound/SoundSubmix.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Core/HjsonWorkspace/DomainAuthoring/UeAgentDomainHjsonAuthoringKit.h"
#include "Domains/Reflection/Authoring/UeAgentReflectionAuthoringAssets.h"
#include "Domains/Reflection/Authoring/UeAgentReflectionAuthoringDescriptors.h"
#include "Domains/Reflection/Authoring/UeAgentReflectionAuthoringProfileState.h"
#include "Utilities/AuthoringCore/UeAgentNativeNormalizationIntent.h"
#include "Utilities/AuthoringCore/UeAgentMovieSceneAuthoring.h"
#include "Domains/Material/Authoring/UeAgentMaterialFunctionInstanceAuthoring.h"
#include "Materials/MaterialFunctionInstance.h"
#include "Domains/Reflection/Authoring/UeAgentReflectionAuthoringState.h"
#include "Domains/Reflection/Authoring/UeAgentSoundClassAuthoringRelations.h"
#include "Domains/Reflection/Authoring/UeAgentSoundSubmixAuthoringRelations.h"
#include "Domains/Reflection/Authoring/UeAgentEqPresetRuntimeState.h"
#include "Domains/Reflection/Authoring/UeAgentPaperSpriteAuthoringState.h"
#include "Domains/Reflection/Authoring/UeAgentPaperAtlasAuthoringState.h"
#include "AssetRegistry/AssetRegistryModule.h"
#include "Domains/Reflection/Authoring/UeAgentPhysicsAssetAuthoringState.h"
#include "Domains/Reflection/Authoring/UeAgentMeshMaterialRelations.h"
#include "PhysicsEngine/PhysicsAsset.h"
#include "PaperSprite.h"
#include "PaperTileSet.h"
#include "Engine/Texture2D.h"
#include "Engine/Texture2DArray.h"
#include "Engine/TextureCubeArray.h"
#include "Engine/TextureCube.h"
#include "Engine/TextureDefines.h"
#include "Engine/TextureRenderTarget2D.h"
#include "VT/RuntimeVirtualTexture.h"
#include "SubmixEffects/AudioMixerSubmixEffectEQ.h"
#include "Misc/PackageName.h"
#include "Templates/UnrealTemplate.h"
#include "CoreGlobals.h"
#include "ObjectTools.h"
#include "RenderingThread.h"
#include "UObject/StrongObjectPtr.h"
#include "UObject/AssetRegistryTagsContext.h"
#include "UObject/UObjectIterator.h"
#include "Utilities/AuthoringCore/UeAgentAuthoringState.h"
#include "Utilities/AuthoringOperationApply/UeAgentAuthoringAssetRemovalStage.h"
#include "Utilities/AuthoringOperationApply/UeAgentAuthoringObjectApplyProvider.h"
#include "Utilities/AuthoringOperationApply/UeAgentAuthoringOperationCoordinator.h"
#include "Utilities/AuthoringWorkspaceApply/UeAgentApplyWorkspaceContract.h"
#include "Utilities/AuthoringWorkspaceApply/UeAgentAuthoringAssetWorkspaceApply.h"
#include "Utilities/AuthoringWorkspaceApply/UeAgentAuthoringAssetWorkspaceInput.h"
#include "Utilities/AuthoringWorkspaceApply/UeAgentAuthoringWorkspaceDependencyGraph.h"
#include "Utilities/AuthoringWorkspaceApply/UeAgentAuthoringWorkspaceTransaction.h"

namespace UeAgentReflectionAuthoring {
namespace {
using namespace UeAgentApplyInputContract;
using namespace UeAgentAuthoringOperationApply;

UE::UAI::Contract::FDiagnostic LifecycleFailure(const FString& Code, const FString& Reason,
    const FString& Target, const FString& Field, TSharedPtr<FJsonValue> Actual, TSharedPtr<FJsonValue> Expected) {
  auto Error = UE::UAI::Contract::FDiagnostic::Failure(Code, Reason, TEXT("authoring_apply"));
  Error.SourceKind = TEXT("native_result");
  Error.TargetPath = Target;
  Error.PropertyPath = Field;
  Error.ActualValuePresent = Actual.IsValid();
  Error.ExpectedValuePresent = Expected.IsValid();
  Error.ActualValue = MoveTemp(Actual);
  Error.ExpectedValue = MoveTemp(Expected);
  return Error;
}

void SetFailure(FOperationFailure& OutFailure, UE::UAI::Contract::FDiagnostic Diagnostic) {
  OutFailure.Diagnostic = MoveTemp(Diagnostic);
}







bool ReadBool(const TSharedPtr<const FJsonObject> &Object, const TCHAR *Field,
              const bool DefaultValue) {
  bool Value = DefaultValue;
  if (Object.IsValid())
    Object->TryGetBoolField(Field, Value);
  return Value;
}

template <typename TTextureArray>
bool ValidateTextureArrayIntent(UObject& Proposed, const FString& SemanticAssetPath,
    const TArray<TSharedPtr<FJsonValue>>& Descriptors,
    const UeAgentAuthoringObjectApply::FOptions& Options,
    const FString& FailureCode, FOperationFailure& OutFailure) {
  // Details permits incomplete slots while retaining old generated pixels.
  // Decode final intent with the shared property owner before native notification.
  TStrongObjectPtr<TTextureArray> Validation(NewObject<TTextureArray>(GetTransientPackage(), NAME_None, RF_Transient));
  for (TFieldIterator<FProperty> It(TTextureArray::StaticClass()); It; ++It)
    It->CopyCompleteValue(It->ContainerPtrToValuePtr<void>(Validation.Get()), It->ContainerPtrToValuePtr<void>(&Proposed));
  auto ValidationOptions = Options;
  ValidationOptions.bEmitEditorNotifications = false;
  ValidationOptions.bMarkPackageDirty = false;
  ValidationOptions.NativeStages.Reset();
  UeAgentAuthoringObjectApply::FPreparedApply ValidationApply;
  UeAgentAuthoringObjectApply::FFailure Failure;
  UeAgentAuthoringObjectApply::FApplyResult Result;
  if (!UeAgentAuthoringObjectApply::PrepareObject(Validation.Get(), Descriptors, ValidationOptions, ValidationApply, Failure)
      || !UeAgentAuthoringObjectApply::ApplyPrepared(ValidationApply, Result)) {
    OutFailure.Diagnostic = Failure.Diagnostic.IsEmpty() ? Result.Diagnostic : Failure.Diagnostic;
    ProfileState::BindTarget(OutFailure.Diagnostic, Validation->GetPathName(), SemanticAssetPath);
    return false;
  }
  const auto Reject = [&](const int32 Index, const FString& Field, const FString& Reason,
      TSharedPtr<FJsonValue> Actual, TSharedPtr<FJsonValue> Expected) {
    SetFailure(OutFailure, LifecycleFailure(FailureCode, Reason, SemanticAssetPath,
        FString::Printf(TEXT("SourceTextures[%d].%s"), Index, *Field), MoveTemp(Actual), MoveTemp(Expected)));
    return false;
  };
  // Cube's native predicate rejects empty input, but its PostEdit explicitly
  // supports clearing the array. Keep that complete, resource-invalidating edit.
  if (Validation->SourceTextures.IsEmpty()) return true;
  if constexpr (std::is_same_v<TTextureArray, UTextureCubeArray>) {
    for (int32 Index = 0; Index < Validation->SourceTextures.Num(); ++Index) {
      const auto& Source = Validation->SourceTextures[Index];
      if (Source && Source->Source.IsValid() && Source->Source.GetNumSlices() != 1 && Source->Source.GetNumSlices() != 6)
        return Reject(Index, TEXT("source_slices"),
            FString::Printf(TEXT("Cube source '%s' must contain six faces or one long/lat image."), *Source->GetPathName()),
            MakeShared<FJsonValueNumber>(Source->Source.GetNumSlices()), MakeShared<FJsonValueString>(TEXT("1 or 6")));
    }
  }
  if (!Validation->CheckArrayTexturesCompatibility()) {
    // The native predicate remains authoritative. Explain its observed rejection
    // from the same source list; 2D arrays do not require matching dimensions.
    for (int32 Index = 0; Index < Validation->SourceTextures.Num(); ++Index) {
      const auto& Source = Validation->SourceTextures[Index];
      if (!Source)
        return Reject(Index, TEXT("available"), TEXT("The texture source slot is empty."),
            MakeShared<FJsonValueBoolean>(false), MakeShared<FJsonValueBoolean>(true));
      if (!Source->Source.IsValid())
        return Reject(Index, TEXT("source_art_valid"),
            FString::Printf(TEXT("Texture source '%s' has no valid source art."), *Source->GetPathName()),
            MakeShared<FJsonValueBoolean>(false), MakeShared<FJsonValueBoolean>(true));
    }
    if constexpr (std::is_same_v<TTextureArray, UTextureCubeArray>) {
      const auto& First = Validation->SourceTextures[0];
      const auto SizeValue = [](const FTextureSource& Source) {
        auto Value = MakeShared<FJsonObject>();
        Value->SetNumberField(TEXT("width"), Source.GetSizeX());
        Value->SetNumberField(TEXT("height"), Source.GetSizeY());
        return MakeShared<FJsonValueObject>(Value);
      };
      const auto FormatValue = [](const ETextureSourceFormat Format) {
        const int32 Index = static_cast<int32>(Format);
        return MakeShared<FJsonValueString>(Index >= 0 && Index < TSF_MAX
            ? FString(GTextureSourceFormats[Index].Name)
            : FString::Printf(TEXT("invalid ETextureSourceFormat (%d)"), Index));
      };
      for (int32 Index = 1; Index < Validation->SourceTextures.Num(); ++Index) {
        const auto& Source = Validation->SourceTextures[Index];
        const FString Reason = FString::Printf(TEXT("Cube texture source '%s' must match source 0 '%s'."),
            *Source->GetPathName(), *First->GetPathName());
        if (Source->Source.GetSizeX() != First->Source.GetSizeX() || Source->Source.GetSizeY() != First->Source.GetSizeY())
          return Reject(Index, TEXT("source_size"), Reason, SizeValue(Source->Source), SizeValue(First->Source));
        if (Source->Source.GetNumSlices() != First->Source.GetNumSlices())
          return Reject(Index, TEXT("source_slices"), Reason,
              MakeShared<FJsonValueNumber>(Source->Source.GetNumSlices()), MakeShared<FJsonValueNumber>(First->Source.GetNumSlices()));
        if (Source->Source.GetFormat() != First->Source.GetFormat())
          return Reject(Index, TEXT("source_format"), Reason, FormatValue(Source->Source.GetFormat()), FormatValue(First->Source.GetFormat()));
      }
    }
    SetFailure(OutFailure, LifecycleFailure(FailureCode,
        TEXT("The native texture array compatibility check rejected the source list."),
        SemanticAssetPath, TEXT("source_compatibility"), MakeShared<FJsonValueBoolean>(false), MakeShared<FJsonValueBoolean>(true)));
    return false;
  }
  return true;
}

void RetireTransientAsset(TStrongObjectPtr<UObject> &Asset,
                          UObject *PreservedAsset = nullptr) {
  if (!Asset.IsValid())
    return;
  if (Asset.Get() != PreservedAsset) {
    // RVT construction queues InitNullResource with a raw render-resource
    // pointer; its destructor has no render fence. Even a rejected preview must
    // finish that work before becoming eligible for garbage collection.
    if (Asset->IsA<URuntimeVirtualTexture>()) FlushRenderingCommands();
    Asset->ClearFlags(RF_Public | RF_Standalone);
    Asset->MarkAsGarbage();
  }
  Asset.Reset();
}

// RVT scene proxies retain a raw asset pointer outside UObject reference fixup.
// Tear them down while the old asset is alive, then let native registration
// consume the component's redirected reference after durable reload/restore.
class FRvtComponentReloadScope final {
public:
  explicit FRvtComponentReloadScope(URuntimeVirtualTexture& Texture) {
    for (TObjectIterator<URuntimeVirtualTextureComponent> It; It; ++It) {
      if (It->GetVirtualTexture() == &Texture && It->IsRegistered())
        Components.Add(MakeUnique<FComponentReregisterContext>(*It));
    }
    // RVT proxy Release queues InitNullResource; it must finish before package
    // replacement can destroy the old render resource.
    FlushRenderingCommands();
  }
  ~FRvtComponentReloadScope() {
    Components.Reset();
    FlushRenderingCommands();
  }
private:
  TArray<TUniquePtr<FComponentReregisterContext>> Components;
};

class FAssetLifecycleState final : public IReflectionOperationLifecycleState {
public:
  FAssetLifecycleState(const FProfile &InProfile, FString InPackagePath,
                       const EOperation InOperation, UObject *InExisting,
                       UObject *InProposed,
                       TSharedPtr<FJsonValue> InProfileStatePatch,
                       const bool bInHasProfileStatePatch, const bool bInSave,
                       TUniquePtr<UeAgentAuthoringAssetRelations::FPlan> InAssetRelations,
                       TUniquePtr<FEqPresetRuntimeState> InEqRuntime,
                       TUniquePtr<PaperSpriteState::FAtlasState> InAtlasState,
                       TSharedPtr<GenerationInput::FPreparedInput> InGenerationInput)
      : Profile(InProfile), PackagePath(MoveTemp(InPackagePath)),
        Operation(InOperation), Existing(InExisting), Proposed(InProposed),
        ProfileStatePatch(InProfileStatePatch.IsValid() ? UeAgentAuthoringState::CloneCanonicalValue(InProfileStatePatch) : nullptr),
        bHasProfileStatePatch(bInHasProfileStatePatch), bSave(bInSave),
        AssetRelationPlan(MoveTemp(InAssetRelations)), EqRuntime(MoveTemp(InEqRuntime)), AtlasState(MoveTemp(InAtlasState)), GenerationPlan(MoveTemp(InGenerationInput)) {
    ProfileStateSourceRoot = MakeShared<FJsonObject>();
    if (InProfileStatePatch.IsValid()) {
      // Capture original value identities before rebinding the immutable prepared copy.
      ProfileStateSourceRoot->SetField(TEXT("profile_state"), InProfileStatePatch);
      ProfileStateSources = UeAgentAuthoringDiagnostics::FScopedSourceLocations::CaptureProjection(*ProfileStateSourceRoot);
      ProfileStateSourceRoot->SetField(TEXT("profile_state"), ProfileStatePatch);
    }
  }

  virtual ~FAssetLifecycleState() override {
    if (AtlasState) AtlasState->Release();
    RetireTransientAsset(Proposed, Actual);
  }

  virtual FString GetTargetKey() const override { return PackagePath; }
  virtual EOperation GetResolvedOperation() const override { return Operation; }
  virtual bool IsStateNoOp() const override {
    return Operation == EOperation::Update && !bHasProfileStatePatch && !GenerationPlan.IsValid();
  }
  virtual bool IsNoOp() const override { return IsStateNoOp() && !bSave; }
  virtual FReflectionRoot GetProposedRoot() const override {
    return FReflectionRoot::MakeObject(Proposed.Get());
  }
  virtual FReflectionRoot GetActualRoot() const override {
    return FReflectionRoot::MakeObject(Actual ? Actual : Existing);
  }
  virtual bool ShouldApplyPropertiesToActualRoot() const override {
    return Operation == EOperation::Update;
  }
  virtual void CollectRollbackObjects(TArray<UObject*>& OutObjects) const override {
    if (AssetRelationPlan) AssetRelationPlan->CollectRollbackObjects(OutObjects);
    if (AtlasState) AtlasState->CollectRollbackObjects(OutObjects);
  }
  virtual bool ShouldNotifyProposedProperties() const override {
    // Flipbook's notification globally re-registers components. Its only value
    // normalization clamps negative FPS, already rejected by reflected bounds.
    return Profile.TypeId != TEXT("sound_submix") && Profile.TypeId != TEXT("paper_flipbook") && Profile.TypeId != TEXT("paper_tileset") && Profile.TypeId != TEXT("paper_sprite");
  }
  virtual void ConfigurePropertyOptions(UeAgentAuthoringObjectApply::FOptions& Options) const override {
    Options.IncludeRootProperty = Profile.IncludeProperty;
    Options.AuthorizeProperty = [Profile = Profile](const FProperty* Property) { return Profile.AuthorizesProperty(Property); };
    if (Profile.TypeId == TEXT("static_mesh")) MeshMaterialEdits.Configure(Options);
    if (AtlasState) {
      Options.NotificationChangeType = EPropertyChangeType::ValueSet;
      Options.PostEditNotification = [this](UObject& Object, FPropertyChangedEvent& Event) {
        if (&Object == Proposed.Get()) return AtlasState->NotifyPreview(Object, Event);
        if (Object.IsA<UPaperSprite>())
          FModuleManager::LoadModuleChecked<FAssetRegistryModule>(TEXT("AssetRegistry")).Get()
              .AssetUpdateTags(&Object, EAssetRegistryTagsCaller::AssetRegistryQuery);
        Object.PostEditChangeProperty(Event);
        return true;
      };
    }
    if (Proposed.IsValid() && Proposed->IsA<UMaterialFunctionInstance>()) {
      Options.CheckedPostEditNotification = [this](UObject& Object, FPropertyChangedEvent& Event,
          UeAgentAuthoringObjectApply::FFailure& Failure) {
        if (auto* Instance = Cast<UMaterialFunctionInstance>(&Object)) {
          UE::UAI::Contract::FDiagnostic Error;
          const auto* Destination = &Object == Proposed.Get() && Operation == EOperation::Update
              ? Cast<UMaterialFunctionInstance>(Existing) : nullptr;
          if (!UeAgentMaterialAuthoring::FunctionInstance::ValidateParentChain(*Instance, Error, Destination)) {
            Failure.Diagnostic = Error;
            Failure.Diagnostic.TargetPath = Object.GetPathName();
            Failure.Diagnostic.PropertyPath = TEXT("Parent");
            return false;
          }
          Object.PostEditChangeProperty(Event);
          if (!UeAgentMaterialAuthoring::FunctionInstance::Update(*Instance, Error)) {
            Failure.Diagnostic = Error;
            Failure.Diagnostic.TargetPath = Object.GetPathName();
            Failure.Diagnostic.PropertyPath = TEXT("Parent");
            return false;
          }
        } else {
          Object.PostEditChangeProperty(Event);
        }
        return true;
      };
    }
    if (Profile.TypeId == TEXT("texture_array") || Profile.TypeId == TEXT("texture_cube_array")) {
      // Source generation can set native format defaults. Apply the source first,
      // then let any explicitly authored texture settings take final precedence.
      Options.NativeStages = {{{TEXT("SourceTextures")}, false, {}}};
    }
    if (Profile.TypeId == TEXT("render_target")) {
      // Set the complete resource intent before native resource notification.
      Options.NativeStages = {{{TEXT("SizeX"), TEXT("SizeY"), TEXT("RenderTargetFormat"),
          TEXT("ClearColor"), TEXT("bAutoGenerateMips")}, false, {}}};
    }
    if (Profile.TypeId == TEXT("paper_tileset")) {
      Options.bEmitEditorNotifications = true;
      Options.NotificationChangeType = EPropertyChangeType::ValueSet;
      Options.NativeStages = {
          {{TEXT("TileSheet"), TEXT("TileSize"), TEXT("BorderMargin"), TEXT("PerTileSpacing")}, true,
              {TEXT("PerTileData"), TEXT("WidthInTiles"), TEXT("HeightInTiles"), TEXT("AllocatedWidth"), TEXT("AllocatedHeight")}},
          {{TEXT("PerTileData"), TEXT("Terrains")}, true}};
    }
    if (Profile.TypeId == TEXT("paper_sprite_atlas")) {
      // Generate from the complete packing intent, not intermediate dimensions.
      Options.NativeStages = {{{TEXT("MaxWidth"), TEXT("MaxHeight"), TEXT("MipCount"),
          TEXT("Padding"), TEXT("PaddingType"), TEXT("CompressionSettings"), TEXT("Filter")}, true}};
    }
  }
  virtual bool NormalizeProposedPropertyDescriptors(
      TArray<TSharedPtr<FJsonValue>>& Descriptors,
      const UeAgentAuthoringObjectApply::FOptions& Options,
      FOperationFailure& OutFailure) override {
    if (Profile.TypeId == TEXT("texture_array"))
      return ValidateTextureArrayIntent<UTexture2DArray>(*Proposed.Get(), PackagePath, Descriptors, Options, TEXT("texture_array_source_invalid"), OutFailure);
    if (Profile.TypeId == TEXT("texture_cube_array"))
      return ValidateTextureArrayIntent<UTextureCubeArray>(*Proposed.Get(), PackagePath, Descriptors, Options, TEXT("texture_cube_array_source_invalid"), OutFailure);
    if (Profile.TypeId == TEXT("render_target") && Proposed->IsA<UTextureRenderTarget2D>()) {
      // Inspect the final intent without running native notifications or allocating
      // a resource. Preserve the shared codec/authorization and transaction owner.
      TStrongObjectPtr<UTextureRenderTarget2D> Validation(NewObject<UTextureRenderTarget2D>(GetTransientPackage(), NAME_None, RF_Transient));
      for (TFieldIterator<FProperty> It(UTextureRenderTarget2D::StaticClass()); It; ++It)
        It->CopyCompleteValue(It->ContainerPtrToValuePtr<void>(Validation.Get()), It->ContainerPtrToValuePtr<void>(Proposed.Get()));
      auto ValidationOptions = Options;
      ValidationOptions.bEmitEditorNotifications = false;
      ValidationOptions.bMarkPackageDirty = false;
      ValidationOptions.NativeStages.Reset();
      UeAgentAuthoringObjectApply::FPreparedApply ValidationApply;
      UeAgentAuthoringObjectApply::FFailure Failure;
      UeAgentAuthoringObjectApply::FApplyResult Result;
      if (!UeAgentAuthoringObjectApply::PrepareObject(Validation.Get(), Descriptors, ValidationOptions, ValidationApply, Failure)
          || !UeAgentAuthoringObjectApply::ApplyPrepared(ValidationApply, Result)) {
        OutFailure.Diagnostic = Failure.Diagnostic.IsEmpty() ? Result.Diagnostic : Failure.Diagnostic;
        ProfileState::BindTarget(OutFailure.Diagnostic, Validation->GetPathName(), PackagePath);
        return false;
      }
      if (Validation->SizeX < 1 || Validation->SizeY < 1) {
        const bool bInvalidX = Validation->SizeX < 1;
        SetFailure(OutFailure, LifecycleFailure(TEXT("render_target_dimensions_invalid"),
            TEXT("Render target dimensions must be positive."),
            PackagePath, bInvalidX ? TEXT("SizeX") : TEXT("SizeY"),
            MakeShared<FJsonValueNumber>(bInvalidX ? Validation->SizeX : Validation->SizeY), MakeShared<FJsonValueString>(TEXT("an integer >= 1"))));
        return false;
      }
    }
    if (Profile.TypeId == TEXT("paper_tileset")) {
      // Shared stages settle the complete layout before preparing metadata.
      // Validate the final native layout without notification before allowing a
      // reallocation. This scratch has no registrations or external mutations.
      TStrongObjectPtr<UPaperTileSet> Validation(NewObject<UPaperTileSet>(Proposed->GetOuter(), NAME_None, RF_Transient));
      for (TFieldIterator<FProperty> It(UPaperTileSet::StaticClass()); It; ++It)
        It->CopyCompleteValue(It->ContainerPtrToValuePtr<void>(Validation.Get()), It->ContainerPtrToValuePtr<void>(Proposed.Get()));
      auto ValidationOptions = Options; ValidationOptions.bEmitEditorNotifications = false;
      UeAgentAuthoringObjectApply::FPreparedApply ValidationApply;
      UeAgentAuthoringObjectApply::FFailure ValidationFailure;
      UeAgentAuthoringObjectApply::FApplyResult ValidationResult;
      if (!UeAgentAuthoringObjectApply::PrepareObject(Validation.Get(), Descriptors, ValidationOptions, ValidationApply, ValidationFailure)
          || !UeAgentAuthoringObjectApply::ApplyPrepared(ValidationApply, ValidationResult)) {
        OutFailure.Diagnostic = ValidationFailure.Diagnostic.IsEmpty() ? ValidationResult.Diagnostic : ValidationFailure.Diagnostic;
        ProfileState::BindTarget(OutFailure.Diagnostic, Validation->GetPathName(), PackagePath);
        return false;
      }
      const FIntPoint Size = Validation->GetTileSize(), Spacing = Validation->GetPerTileSpacing();
      const auto Margin = Validation->GetMargin();
      const auto* Texture = Validation->GetTileSheetTexture();
      const auto Pixels = Texture ? Texture->GetImportedSize() : FIntPoint::ZeroValue;
      const auto RejectNumber = [&](const TCHAR* Code, const FString& Field, const int64 Actual,
          const int64 Expected, const TCHAR* Reason) {
        SetFailure(OutFailure, LifecycleFailure(Code, Reason, PackagePath, Field,
            MakeShared<FJsonValueNumber>(Actual), MakeShared<FJsonValueNumber>(Expected)));
        return false;
      };
      if (Size.X <= 0 || Size.Y <= 0)
        return RejectNumber(TEXT("paper_tileset_layout_invalid"), Size.X <= 0 ? TEXT("TileSize.X") : TEXT("TileSize.Y"),
            Size.X <= 0 ? Size.X : Size.Y, 1, TEXT("Tile dimensions must be at least 1 pixel."));
      if (Spacing.X < 0 || Spacing.Y < 0)
        return RejectNumber(TEXT("paper_tileset_layout_invalid"), Spacing.X < 0 ? TEXT("PerTileSpacing.X") : TEXT("PerTileSpacing.Y"),
            Spacing.X < 0 ? Spacing.X : Spacing.Y, 0, TEXT("Per-tile spacing must be non-negative."));
      if (Margin.Left < 0)
        return RejectNumber(TEXT("paper_tileset_layout_invalid"), TEXT("BorderMargin.Left"), Margin.Left, 0, TEXT("Tile sheet margins must be non-negative."));
      if (Margin.Right < 0)
        return RejectNumber(TEXT("paper_tileset_layout_invalid"), TEXT("BorderMargin.Right"), Margin.Right, 0, TEXT("Tile sheet margins must be non-negative."));
      if (Margin.Top < 0)
        return RejectNumber(TEXT("paper_tileset_layout_invalid"), TEXT("BorderMargin.Top"), Margin.Top, 0, TEXT("Tile sheet margins must be non-negative."));
      if (Margin.Bottom < 0)
        return RejectNumber(TEXT("paper_tileset_layout_invalid"), TEXT("BorderMargin.Bottom"), Margin.Bottom, 0, TEXT("Tile sheet margins must be non-negative."));
      if (Texture && (int64(Margin.Left) + Margin.Right > Pixels.X || int64(Margin.Top) + Margin.Bottom > Pixels.Y)) {
        const bool bHorizontal = int64(Margin.Left) + Margin.Right > Pixels.X;
        return RejectNumber(TEXT("paper_tileset_layout_invalid"),
            bHorizontal ? TEXT("BorderMargin.Left+Right") : TEXT("BorderMargin.Top+Bottom"),
            bHorizontal ? int64(Margin.Left) + Margin.Right : int64(Margin.Top) + Margin.Bottom,
            bHorizontal ? Pixels.X : Pixels.Y, TEXT("Combined margins must not exceed the imported texture dimension."));
      }
      if (int64(Size.X) + Spacing.X > MAX_int32 || int64(Size.Y) + Spacing.Y > MAX_int32) {
        const bool bHorizontal = int64(Size.X) + Spacing.X > MAX_int32;
        return RejectNumber(TEXT("paper_tileset_layout_invalid"),
            bHorizontal ? TEXT("TileSize.X+PerTileSpacing.X") : TEXT("TileSize.Y+PerTileSpacing.Y"),
            bHorizontal ? int64(Size.X) + Spacing.X : int64(Size.Y) + Spacing.Y, MAX_int32,
            TEXT("Tile dimension plus spacing must fit in a signed 32-bit integer."));
      }
      const int64 NumeratorX = int64(Pixels.X) - Margin.Left - Margin.Right + Spacing.X;
      const int64 NumeratorY = int64(Pixels.Y) - Margin.Top - Margin.Bottom + Spacing.Y;
      if (Texture) {
        if (NumeratorX > MAX_int32 || NumeratorY > MAX_int32)
          return RejectNumber(TEXT("paper_tileset_layout_overflow"), NumeratorX > MAX_int32 ? TEXT("layout_numerator_x") : TEXT("layout_numerator_y"),
              NumeratorX > MAX_int32 ? NumeratorX : NumeratorY, MAX_int32,
              TEXT("Texture pixels minus margins plus spacing must fit in a signed 32-bit integer."));
        const int64 TileCount = (NumeratorX / (int64(Size.X) + Spacing.X)) * (NumeratorY / (int64(Size.Y) + Spacing.Y));
        if (TileCount > MAX_int32)
          return RejectNumber(TEXT("paper_tileset_layout_overflow"), TEXT("tile_count"), TileCount, MAX_int32,
              TEXT("The tile grid count must fit in a signed 32-bit integer."));
      }
      auto PreviewOptions = Options;
      PreviewOptions.bEmitEditorNotifications = true;
      PreviewOptions.NotificationChangeType = EPropertyChangeType::Interactive;
      PreviewOptions.bVerifyNativeStages = false;
      UeAgentAuthoringObjectApply::FPreparedApply Preview;
      UeAgentAuthoringObjectApply::FFailure Failure;
      UeAgentAuthoringObjectApply::FApplyResult Result;
      if (!UeAgentAuthoringObjectApply::PrepareObject(Proposed.Get(), Descriptors, PreviewOptions, Preview, Failure)
          || !Preview.ApplyNativeNormalizationPreview(Result)) {
        OutFailure.Diagnostic = Failure.Diagnostic.IsEmpty() ? Result.Diagnostic : Failure.Diagnostic;
        ProfileState::BindTarget(OutFailure.Diagnostic, Proposed->GetPathName(), PackagePath);
        return false;
      }
      auto* TileSet = CastChecked<UPaperTileSet>(Proposed.Get());
      if (TileSet->GetNumTerrains() > 254)
        return RejectNumber(TEXT("paper_tileset_terrain_limit"), TEXT("Terrains.count"), TileSet->GetNumTerrains(), 254,
            TEXT("A tile set supports at most 254 terrains."));
      for (int32 Index = 0; Index < TileSet->GetNumTerrains(); ++Index) {
        const auto Terrain = TileSet->GetTerrain(Index);
        if (Terrain.TerrainName.IsEmpty()) {
          SetFailure(OutFailure, LifecycleFailure(TEXT("paper_tileset_terrain_invalid"), TEXT("Terrain names must not be empty."),
              PackagePath, FString::Printf(TEXT("Terrains[%d].TerrainName"), Index),
              MakeShared<FJsonValueString>(Terrain.TerrainName), MakeShared<FJsonValueString>(TEXT("a non-empty terrain name"))));
          return false;
        }
        if (Terrain.CenterTileIndex < 0 || Terrain.CenterTileIndex >= TileSet->GetTileCount()) {
          SetFailure(OutFailure, LifecycleFailure(TEXT("paper_tileset_terrain_invalid"), TEXT("The terrain center tile must lie within the tile grid."),
              PackagePath, FString::Printf(TEXT("Terrains[%d].CenterTileIndex"), Index),
              MakeShared<FJsonValueNumber>(Terrain.CenterTileIndex),
              MakeShared<FJsonValueString>(FString::Printf(TEXT("0 <= index < %d"), TileSet->GetTileCount()))));
          return false;
        }
      }
      const auto* Data = FindFProperty<FArrayProperty>(UPaperTileSet::StaticClass(), TEXT("PerTileData"));
      FScriptArrayHelper Items(Data, Data->ContainerPtrToValuePtr<void>(TileSet));
      if (Items.Num() != TileSet->GetTileCount())
        return RejectNumber(TEXT("paper_tileset_metadata_grid_size_mismatch"), TEXT("PerTileData.count"), Items.Num(), TileSet->GetTileCount(),
            TEXT("Tile metadata must have exactly one entry per tile in the native grid."));
      if (!Preview.CaptureNativeNormalizedDescriptors(Descriptors, Failure)) {
        OutFailure.Diagnostic = Failure.Diagnostic;
        ProfileState::BindTarget(OutFailure.Diagnostic, Proposed->GetPathName(), PackagePath);
        return false;
      }
      return true;
    }
    const bool bNormalizeNativeResource = Profile.TypeId == TEXT("paper_sprite_atlas")
        || (Profile.TypeId == TEXT("render_target") && Proposed->IsA<UTextureRenderTarget2D>());
    if (Profile.TypeId != TEXT("paper_sprite") && !bNormalizeNativeResource) return true;
    // Native source replacement may initialize a region, and pivot mode updates
    // its custom point. Explicit region/point edits must follow those drivers.
    auto Rank = [](const TSharedPtr<FJsonValue>& Value) {
      FString Path;
      if (Value.IsValid() && Value->Type == EJson::Object)
        Value->AsObject()->TryGetStringField(TEXT("property_path"), Path);
      FString Root = Path; int32 End;
      if (Root.FindChar(TCHAR('.'), End)) Root.LeftInline(End);
      if (Root == TEXT("SourceTexture")) return 10;
      if (Root == TEXT("SourceUV")) return 11;
      if (Root == TEXT("SourceDimension")) return 12;
      if (Root == TEXT("PivotMode")) return 20;
      if (Root == TEXT("CustomPivotPoint")) return 21;
      return 0;
    };
    if (Profile.TypeId == TEXT("paper_sprite"))
      Descriptors.StableSort([&](const auto& A, const auto& B) { return Rank(A) < Rank(B); });
    UeAgentAuthoringObjectApply::FPreparedApply Preview;
    UeAgentAuthoringObjectApply::FFailure Failure;
    UeAgentAuthoringObjectApply::FApplyResult Result;
    auto NativeOptions = Options;
    NativeOptions.bEmitEditorNotifications = true;
    // Native atlas packing and render-target notifications normalize dimensions.
    // Capture that normalization only on the isolated preview; actual stages
    // still verify against these prepared values and retain full rollback.
    if (bNormalizeNativeResource) NativeOptions.bVerifyNativeStages = false;
    auto* Sprite = Cast<UPaperSprite>(Proposed.Get());
    const auto* BeforeTexture = Sprite ? Sprite->GetSourceTexture() : nullptr;
    if (!UeAgentAuthoringObjectApply::PrepareObject(Proposed.Get(), Descriptors, NativeOptions, Preview, Failure)
        || !Preview.ApplyNativeNormalizationPreview(Result)) {
      OutFailure.Diagnostic = Failure.Diagnostic.IsEmpty() ? Result.Diagnostic : Failure.Diagnostic;
      ProfileState::BindTarget(OutFailure.Diagnostic, Proposed->GetPathName(), PackagePath);
      return false;
    }
    bRefreshSpriteSourceBasis = Sprite && BeforeTexture != Sprite->GetSourceTexture();
    if (bRefreshSpriteSourceBasis) PaperSpriteState::RefreshSourceTextureBasis(*Sprite);
    if (!Preview.CaptureNativeNormalizedDescriptors(Descriptors, Failure)) {
      OutFailure.Diagnostic = Failure.Diagnostic;
      ProfileState::BindTarget(OutFailure.Diagnostic, Proposed->GetPathName(), PackagePath);
      return false;
    }
    return true;
  }

  virtual bool CaptureRollback(FOperationFailure &OutFailure) override {
    OutFailure = FOperationFailure();
    UE::UAI::Contract::FDiagnostic RuntimeError;
    if (EqRuntime && !EqRuntime->Validate(RuntimeError)) { SetFailure(OutFailure, RuntimeError); return false; }
    if (Operation == EOperation::Remove && Existing) {
      UE::UAI::Contract::FDiagnostic Error;
      if (!RemovalStage.Capture(Existing, Error)) {
        SetFailure(OutFailure, Error);
        return false;
      }
    }
    return true;
  }

  virtual bool ApplyProposed(FOperationFailure &OutFailure) override {
    OutFailure = FOperationFailure();
    if (auto* Instance = Cast<UMaterialFunctionInstance>(Proposed.Get())) {
      UE::UAI::Contract::FDiagnostic Error;
      const auto* Destination = Operation == EOperation::Update ? Cast<UMaterialFunctionInstance>(Existing) : nullptr;
      if (!UeAgentMaterialAuthoring::FunctionInstance::ValidateParentChain(*Instance, Error, Destination)) {
        ProfileState::BindTarget(Error, Proposed->GetPathName(), PackagePath);
        if (Error.Stage.IsEmpty()) Error.Stage = TEXT("proposed_validation");
        if (Error.PropertyPath.IsEmpty()) Error.PropertyPath = GET_MEMBER_NAME_CHECKED(UMaterialFunctionInstance, Parent).ToString();
        SetFailure(OutFailure, MoveTemp(Error));
        return false;
      }
    }
    if (AtlasState && (Operation == EOperation::Create || Operation == EOperation::Clone)) {
      FPropertyChangedEvent Event(nullptr, EPropertyChangeType::ValueSet);
      AtlasState->NotifyPreview(*Proposed.Get(), Event);
    }
    if (GenerationPlan.IsValid()) {
      UE::UAI::Contract::FDiagnostic Error;
      if (!Proposed.IsValid()) {
        SetFailure(OutFailure, LifecycleFailure(TEXT("generation_preview_missing"),
            TEXT("Generation input cannot be applied because the proposed asset is unavailable."),
            PackagePath, TEXT("proposed_asset"), nullptr, MakeShared<FJsonValueString>(Profile.AssetClassPath)));
        return false;
      }
      if (!GenerationInput::Apply(*Proposed.Get(), GenerationPlan, Error)) {
        SetFailure(OutFailure, Error);
        return false;
      }
    }
    if (!bHasProfileStatePatch || Operation == EOperation::Remove)
      return true;
    if (!Proposed.IsValid()) {
      SetFailure(OutFailure, LifecycleFailure(TEXT("reflection_profile_state_proposed_missing"),
          TEXT("Profile state cannot be applied because the proposed asset is unavailable."),
          PackagePath, TEXT("proposed_asset"), nullptr, MakeShared<FJsonValueString>(Profile.AssetClassPath)));
      return false;
    }
    if (AssetRelationPlan) {
      AssetRelationPlan->ApplyPreview(*Proposed.Get());
      return true;
    }
    UeAgentAuthoringDiagnostics::FScopedSourceLocations ProfileSources(ProfileStateSourceRoot.Get(),
        [this](const FString& Pointer) {
          return UeAgentAuthoringDiagnostics::FScopedSourceLocations::ResolveProjection(ProfileStateSources, Pointer);
        });
    UE::UAI::Contract::FDiagnostic Error;
    auto ApplyProfileState = [&]() {
      if (!UeAgentNativeNormalizationIntent::IsActive())
        return ProfileState::Apply(Profile, *Proposed.Get(), ProfileStatePatch, PackagePath, Error,
            Operation == EOperation::Update, Operation == EOperation::Update && Existing ? Existing->GetPackage() : nullptr,
            &MovieSceneCreation, &ProposedProfileStateIntent);
      TArray<UeAgentAuthoringState::FReadRequest> Requests;
      if (!BuildReadRequests(Proposed.Get(), false, Requests, OutFailure)) {
        Error = OutFailure.Diagnostic;
        return false;
      }
      const auto* Request = Requests.FindByPredicate([&](const auto& Item) { return Item.SemanticId == Profile.ProfileStateSemanticId; });
      if (!Request) {
        TArray<TSharedPtr<FJsonValue>> ActualSemantics;
        for (const auto& Item : Requests) ActualSemantics.Add(MakeShared<FJsonValueString>(Item.SemanticId));
        Error = LifecycleFailure(TEXT("reflection_profile_state_read_request_missing"),
            TEXT("Native normalization requires a read request for the profile state semantic."),
            PackagePath, TEXT("state_read_requests"), MakeShared<FJsonValueArray>(MoveTemp(ActualSemantics)),
            MakeShared<FJsonValueString>(Profile.ProfileStateSemanticId));
        return false;
      }
	      UeAgentNativeNormalizationIntent::FValueScope Scope(Request->TargetIdentity, Request->SemanticId);
      return ProfileState::Apply(Profile, *Proposed.Get(), ProfileStatePatch,
                             PackagePath, Error, Operation == EOperation::Update,
                             Operation == EOperation::Update && Existing ? Existing->GetPackage() : nullptr, &MovieSceneCreation,
                             &ProposedProfileStateIntent);
    };
    if (!ApplyProfileState()) {
      // The producer's pointer is relative to the profile value. Qualify it
      // against the prepared source carrier before a transaction can bind it.
      UeAgentAuthoringDiagnostics::PrefixInternalDocumentFailure(Error, TEXT("/profile_state"));
      UeAgentAuthoringDiagnostics::LocateAuthoringSource(Error, *ProfileStateSourceRoot);
      SetFailure(OutFailure, Error);
      return false;
    }
    return true;
  }

  virtual bool ApplyLifecycle(FOperationFailure &OutFailure) override {
    OutFailure = FOperationFailure();
    if (Operation == EOperation::Remove) {
      UE::UAI::Contract::FDiagnostic Error;
      if (!RemovalStage.Stage(Error)) {
        SetFailure(OutFailure, Error);
        return false;
      }
      Actual = nullptr;
      return true;
    }
    if (Operation == EOperation::Update) {
      Actual = Existing;
      if (GenerationPlan.IsValid()) {
        UE::UAI::Contract::FDiagnostic Error;
        if (!GenerationInput::Apply(*Actual, GenerationPlan, Error)) { SetFailure(OutFailure, Error); return false; }
      }
      if (AssetRelationPlan) {
        UE::UAI::Contract::FDiagnostic Error;
        if (!AssetRelationPlan->Apply(Error)) { SetFailure(OutFailure, Error); return false; }
        return true;
      }
      if (bHasProfileStatePatch) {
        UeAgentAuthoringDiagnostics::FScopedSourceLocations ProfileSources(ProfileStateSourceRoot.Get(),
            [this](const FString& Pointer) {
              return UeAgentAuthoringDiagnostics::FScopedSourceLocations::ResolveProjection(ProfileStateSources, Pointer);
            });
        UE::UAI::Contract::FDiagnostic Error;
        if (!ProfileState::Apply(Profile, *Actual, ProfileStatePatch,
                                 PackagePath, Error, true, nullptr, &MovieSceneCreation, &ActualProfileStateIntent)) {
          UeAgentAuthoringDiagnostics::PrefixInternalDocumentFailure(Error, TEXT("/profile_state"));
          UeAgentAuthoringDiagnostics::LocateAuthoringSource(Error, *ProfileStateSourceRoot);
          SetFailure(OutFailure, Error);
          return false;
        }
      }
      return true;
    }
    UE::UAI::Contract::FDiagnostic Error;
    Actual = PublishAsset(Proposed.Get(), PackagePath, Error);
    if (!Actual) {
      SetFailure(OutFailure, Error);
      return false;
    }
    ActualAsset.Reset(Actual);
    if (AtlasState) {
      AtlasState->BindActual(*Actual);
      FModuleManager::LoadModuleChecked<FAssetRegistryModule>(TEXT("AssetRegistry")).Get()
          .AssetUpdateTags(Actual, EAssetRegistryTagsCaller::AssetRegistryQuery);
      FPropertyChangedEvent Event(nullptr, EPropertyChangeType::ValueSet);
      Actual->PostEditChangeProperty(Event);
    } else {
      UeAgentAuthoringObjectApply::FOptions Options;
      ConfigurePropertyOptions(Options);
      UeAgentAuthoringObjectApply::FFailure Failure;
      if (!UeAgentAuthoringObjectApply::NotifyObject(*Actual, Options, Failure)) {
        OutFailure.Diagnostic = Failure.Diagnostic;
        return false;
      }
    }
    return true;
  }

  virtual bool Compile(FOperationFailure &OutFailure) override {
    OutFailure = FOperationFailure();
    // Mapping structs are edited through the shared recursive property owner.
    // Unlike InputAction, the context has no PostEditChange rebuild override.
    if (Operation != EOperation::Remove)
      RefreshInputMappings(Actual ? Actual : Existing);
    RefreshAssetRelations();
    if (bRefreshSpriteSourceBasis)
      if (auto* Sprite = Cast<UPaperSprite>(Actual ? Actual : Existing)) PaperSpriteState::RefreshSourceTextureBasis(*Sprite);
    if (!bHasProfileStatePatch || Operation == EOperation::Remove)
      return true;
    UObject *Asset = Actual ? Actual : (Existing ? Existing : Proposed.Get());
    if (!Asset) {
      SetFailure(OutFailure, LifecycleFailure(TEXT("reflection_profile_state_compile_target_missing"),
          TEXT("Profile state compilation has no actual, existing, or proposed asset."),
          PackagePath, TEXT("compile_target"), nullptr, MakeShared<FJsonValueString>(Profile.AssetClassPath)));
      return false;
    }
    UE::UAI::Contract::FDiagnostic Error;
    if (!ProfileState::CompileIfNeeded(Profile, *Asset, PackagePath, Error)) {
      SetFailure(OutFailure, Error);
      return false;
    }
    return true;
  }

  virtual bool Save(FOperationFailure &OutFailure) override {
    OutFailure = FOperationFailure();
    if (!bSave || Operation == EOperation::Remove)
      return true;
    UObject *Asset = Actual ? Actual : Existing;
    UE::UAI::Contract::FDiagnostic Error;
    if (!Asset) {
      SetFailure(OutFailure, LifecycleFailure(TEXT("reflection_asset_save_target_missing"),
          TEXT("The asset cannot be saved because its actual or existing native object is unavailable."),
          PackagePath, TEXT("save_target"), nullptr, MakeShared<FJsonValueString>(Profile.AssetClassPath)));
      return false;
    }
    if (!SaveAsset(*Asset, Error)) {
      SetFailure(OutFailure, Error);
      return false;
    }
    return true;
  }

  virtual bool RequiresCheckpointRollback() const override {
    return ((Operation == EOperation::Update || Operation == EOperation::Create) && bSave)
        || (Operation == EOperation::Update && Profile.TypeId == TEXT("skeletal_mesh") && bHasProfileStatePatch);
  }
  virtual bool SupportsPersistentReadback() const override {
    return (bSave && Operation != EOperation::Remove)
        || (Operation == EOperation::Update && Profile.TypeId == TEXT("skeletal_mesh") && bHasProfileStatePatch);
  }
  virtual bool ReleaseForPersistentReadback(FOperationFailure &OutFailure) override {
    OutFailure = FOperationFailure();
    if (!RvtReloadScope) {
      if (auto* Texture = Cast<URuntimeVirtualTexture>(Actual ? Actual : Existing))
        RvtReloadScope = MakeUnique<FRvtComponentReloadScope>(*Texture);
    }
    Existing = nullptr;
    Actual = nullptr;
    ActualAsset.Reset();
    if (AtlasState) AtlasState->Release();
    RetireTransientAsset(Proposed);
    if (AssetRelationPlan) AssetRelationPlan->Release();
    return true;
  }
  virtual bool RebindAfterPersistentReadback(FOperationFailure &OutFailure) override {
    OutFailure = FOperationFailure();
    UE::UAI::Contract::FDiagnostic ClassError;
    UClass *ExpectedClass = ResolveAssetClass(Profile, false, ClassError);
    if (!ExpectedClass) {
      if (ClassError.TargetPath.IsEmpty()) ClassError.TargetPath = PackagePath;
      SetFailure(OutFailure, MoveTemp(ClassError));
      return false;
    }
    const FString ObjectPath = PackagePath + TEXT(".") +
                               FPackageName::GetLongPackageAssetName(PackagePath);
    Existing = StaticLoadObject(ExpectedClass, nullptr, *ObjectPath, nullptr,
                                LOAD_NoWarn | LOAD_Quiet);
    Actual = Existing;
    if (!Actual) {
      auto Error = UE::UAI::Contract::FDiagnostic::Failure(TEXT("reflection_persistent_reload_failed"),
          TEXT("The persisted reflection asset could not be reloaded after saving."), TEXT("persistent_readback"));
      Error.SourceKind = TEXT("native_result");
      Error.TargetPath = PackagePath;
      Error.PropertyPath = TEXT("asset_class");
      Error.ActualValuePresent = false;
      Error.ExpectedValue = MakeShared<FJsonValueString>(ExpectedClass->GetPathName());
      SetFailure(OutFailure, MoveTemp(Error));
      return false;
    }
    ActualAsset.Reset(Actual);
    RvtReloadScope.Reset();
    if (auto* Physics = Cast<UPhysicsAsset>(Actual)) PhysicsAssetState::Refresh(*Physics);
    if (AssetRelationPlan && !AssetRelationPlan->Rebind(ClassError)) {
      SetFailure(OutFailure, ClassError); return false;
    }
    if (AtlasState && !AtlasState->Rebind(ClassError)) {
      SetFailure(OutFailure, ClassError); return false;
    }
    if (auto* Sprite = Cast<UPaperSprite>(Actual)) PaperSpriteState::RefreshComponents(*Sprite);
    // ReloadPackages rebinds external references; Blackboard's transient caches
    // still need their native refresh against the restored or persisted parent.
    if (UBlackboardData *Blackboard = Cast<UBlackboardData>(Actual))
      Blackboard->PropagateKeyChangesToDerivedBlackboardAssets();
    RefreshInputMappings(Actual);
    RefreshAssetRelations();
    // Package replacement rebinds component source references; native editor
    // notification rebuilds their cached frame and render state at the same time.
    if (Profile.TypeId == TEXT("paper_flipbook") || Profile.TypeId == TEXT("paper_tileset")) Actual->PostEditChange();
    // Preset SettingsCopy is native runtime state, not a serialized property.
    if (Profile.TypeId == TEXT("audio_effect_preset")) {
      Actual->PostEditChange();
      if (EqRuntime && !EqRuntime->Rebind(*CastChecked<USubmixEffectSubmixEQPreset>(Actual), ClassError)) {
        SetFailure(OutFailure, ClassError); return false;
      }
    }
    return true;
  }

  virtual bool PrepareCheckpointRestore(FOperationFailure &OutFailure) override {
    if (Operation != EOperation::Create)
      return ReleaseForPersistentReadback(OutFailure);
    UObject *Created = Actual;
    ReleaseForPersistentReadback(OutFailure);
    if (Created) {
      // Complete UE deletion also releases the package's loader before the
      // durable transaction restores the original absent disk state.
      const int32 Deleted = ObjectTools::DeleteObjects({Created}, false);
      if (Deleted != 1) {
        SetFailure(OutFailure, LifecycleFailure(TEXT("reflection_created_restore_delete_failed"),
            TEXT("Checkpoint restore could not delete the newly created asset."),
            PackagePath, TEXT("deleted_assets"), MakeShared<FJsonValueNumber>(Deleted), MakeShared<FJsonValueNumber>(1)));
        return false;
      }
    }
    return true;
  }

  virtual bool RebindAfterCheckpointRestore(FOperationFailure &OutFailure) override {
    if (Operation != EOperation::Create)
      return RebindAfterPersistentReadback(OutFailure);
    UE::UAI::Contract::FDiagnostic AtlasError;
    if (AtlasState && !AtlasState->Rebind(AtlasError, true)) {
      SetFailure(OutFailure, AtlasError); return false;
    }
    bool bPresent = false;
    const bool bInspected = UE::UAI::HjsonWorkspace::DomainAuthoring::TryInspectObjectTargetPresence(PackagePath, bPresent);
    if (!bInspected || bPresent) {
      TSharedPtr<FJsonValue> ActualPresence;
      if (bInspected) ActualPresence = MakeShared<FJsonValueBoolean>(bPresent);
      SetFailure(OutFailure, LifecycleFailure(TEXT("reflection_absent_restore_unconfirmed"),
          bInspected ? TEXT("The newly created asset remains present after checkpoint restore.")
              : TEXT("Asset presence could not be inspected after checkpoint restore."),
          PackagePath, TEXT("asset_present"), ActualPresence, MakeShared<FJsonValueBoolean>(false)));
      return false;
    }
    OutFailure = FOperationFailure();
    return true;
  }

  virtual bool
  BuildDurablePackagePlans(TArray<FPackageDurablePlan> &OutPlans,
                           FOperationFailure &OutFailure) const override {
    if (!BuildPackageDurablePlan(PackagePath, PackagePath,
                                   Operation == EOperation::Remove
                                       ? Existing
                                       : Actual,
                                   Operation == EOperation::Remove
                                       ? EPackageDurableIntent::Delete
                                       : EPackageDurableIntent::Save,
                                   OutPlans, OutFailure, true,
                                   Operation == EOperation::Create ||
                                           Operation == EOperation::Clone
                                       ? EPackageBeforePresence::Absent
                                       : EPackageBeforePresence::Present)) return false;
    // These native consumers retain external references. UnloadPackages can
    // report success while retaining reachable objects with an unloaded package
    // marker. Native replacement performs a real reload and repoints referencers;
    // RVT's raw scene-proxy reference also needs the native component scope above.
    if (Operation != EOperation::Remove && (Profile.TypeId == TEXT("blackboard") || Profile.TypeId == TEXT("sound_class") || Profile.TypeId == TEXT("sound_submix") || Profile.TypeId == TEXT("audio_effect_preset") || Profile.TypeId == TEXT("paper_flipbook") || Profile.TypeId == TEXT("paper_sprite") || Profile.TypeId == TEXT("paper_tileset") || Profile.TypeId == TEXT("runtime_virtual_texture") || Profile.TypeId == TEXT("sparse_volume_texture") || Profile.TypeId == TEXT("texture_collection") || Profile.TypeId == TEXT("texture_array") || Profile.TypeId == TEXT("texture_cube_array") || Profile.TypeId == TEXT("texture_cube") || Profile.TypeId == TEXT("volume_texture")))
      OutPlans.Last().ReadbackPolicy = EPackageReadbackPolicy::ReplaceResidentPackage;
    if (AssetRelationPlan && !AssetRelationPlan->AppendPackagePlans(OutPlans, OutFailure)) return false;
    if (AtlasState && !AtlasState->AppendPackagePlans(OutPlans, OutFailure)) return false;
    return true;
  }

  virtual bool BuildDomainExpectedReadRequests(
      TArray<UeAgentAuthoringState::FReadRequest> &OutRequests,
      FOperationFailure &OutFailure) const override {
    if (!VerifyGenerationIntent(Proposed.Get(), OutFailure)) return false;
    if (!VerifyProfileStateIntent(Proposed.Get(), ProposedProfileStateIntent, OutFailure)) return false;
    return BuildReadRequests(
        Operation == EOperation::Remove ? nullptr : Proposed.Get(),
        Operation == EOperation::Remove, OutRequests, OutFailure)
        && (!AssetRelationPlan || AssetRelationPlan->AppendReadRequests(true, OutRequests, OutFailure));
  }

  virtual bool BuildDomainActualReadRequests(
      TArray<UeAgentAuthoringState::FReadRequest> &OutRequests,
      FOperationFailure &OutFailure) const override {
    if (!VerifyGenerationIntent(Actual ? Actual : Existing, OutFailure)) return false;
    if (!VerifyProfileStateIntent(Actual ? Actual : Existing,
        Operation == EOperation::Update ? ActualProfileStateIntent : ProposedProfileStateIntent, OutFailure)) return false;
    return BuildReadRequests(
        Operation == EOperation::Remove ? nullptr
                                        : (Actual ? Actual : Existing),
        Operation == EOperation::Remove, OutRequests, OutFailure)
        && (!AssetRelationPlan || AssetRelationPlan->AppendReadRequests(false, OutRequests, OutFailure));
  }

  virtual bool BuildDomainBeforeReadRequests(
      TArray<UeAgentAuthoringState::FReadRequest>& OutRequests,
      FOperationFailure& OutFailure) const override {
    // Rollback compares restored state with the captured baseline, never with
    // the new profile-state intent. Removal still has a present before state.
    UObject* Owner = Actual ? Actual : Existing;
    return BuildReadRequests(Owner, !Owner, OutRequests, OutFailure)
        && (!AssetRelationPlan || AssetRelationPlan->AppendReadRequests(false, OutRequests, OutFailure));
  }

  virtual TSharedPtr<FJsonObject> BuildResultMetadata() const override {
    TSharedRef<FJsonObject> Result = MakeShared<FJsonObject>();
    Result->SetStringField(TEXT("profile"), Profile.TypeId);
    Result->SetStringField(TEXT("class_path"), Profile.AssetClassPath);
    return Result;
  }

  virtual bool Commit(FOperationFailure &OutFailure) override {
    OutFailure = FOperationFailure();
    if (Operation == EOperation::Remove && RemovalStage.IsStaged()) {
      UE::UAI::Contract::FDiagnostic Error;
      if (!DeleteAsset(RemovalStage.GetAsset(), PackagePath, Error)) {
        SetFailure(OutFailure, Error);
        return false;
      }
      RemovalStage.Reset();
      Existing = nullptr;
    }
    if (AtlasState) AtlasState->Release();
    RetireTransientAsset(Proposed, Actual);
    return true;
  }

  virtual bool Rollback(FOperationFailure &OutFailure) override {
    OutFailure = FOperationFailure();
    UE::UAI::Contract::FDiagnostic Error;
    if (AssetRelationPlan && !AssetRelationPlan->Rollback(Error)) {
      SetFailure(OutFailure, Error); return false;
    }
    RefreshAssetRelations();
    if ((Operation == EOperation::Create || Operation == EOperation::Clone) &&
        Actual) {
      if (!DeleteAsset(Actual, PackagePath, Error)) {
        SetFailure(OutFailure, Error);
        return false;
      }
      ActualAsset.Reset();
      Actual = nullptr;
    }
    if (Operation == EOperation::Remove && RemovalStage.IsCaptured()) {
      if (!RemovalStage.Rollback(Error)) {
        SetFailure(OutFailure, Error);
        return false;
      }
      Actual = Existing;
    }
    RefreshInputMappings(Actual ? Actual : Existing);
    if (AtlasState) AtlasState->Release();
    RetireTransientAsset(Proposed, Actual);
    return true;
  }

  static void RefreshInputMappings(UObject* Object) {
    if (const auto* Context = Cast<UInputMappingContext>(Object))
      UEnhancedInputLibrary::RequestRebuildControlMappingsUsingContext(Context);
  }

private:
  void RefreshAssetRelations() const {
    if (Profile.TypeId != TEXT("sound_class") && Profile.TypeId != TEXT("sound_submix")) return;
    TArray<UObject*> Objects;
    if (AssetRelationPlan) AssetRelationPlan->CollectRollbackObjects(Objects);
    if (UObject* Root = Actual ? Actual : Existing) Objects.AddUnique(Root);
    for (UObject* Object : Objects) {
      Object->PostEditChange();
      if (auto* Submix = Cast<USoundSubmixBase>(Object)) SoundSubmixRelations::RefreshDevices(*Submix);
    }
    if (auto* SoundClass = Cast<USoundClass>(Actual ? Actual : Existing)) SoundClassRelations::RefreshDerivedGraphs(*SoundClass);
  }
  bool
  BuildReadRequests(UObject *Asset, const bool bExpectedAbsent,
                    TArray<UeAgentAuthoringState::FReadRequest> &OutRequests,
                    FOperationFailure &OutFailure) const {
    UE::UAI::Contract::FDiagnostic Error;
    if (!BuildAssetSnapshotReadRequests(Profile, Asset, PackagePath,
                                        bExpectedAbsent, OutRequests, Error)) {
      SetFailure(OutFailure, Error);
      return false;
    }
    if (AtlasState && !bExpectedAbsent) {
      for (auto& Request : OutRequests) {
        if (Request.SemanticId != Profile.ExportTargetSemanticId) continue;
        Request.RootData = AtlasState.Get();
        Request.Context.Add(TEXT("paper_atlas_snapshot"), Asset == Proposed.Get() ? TEXT("expected") : TEXT("actual"));
      }
    }
    return true;
  }

  FProfile Profile;
  FString PackagePath;
  EOperation Operation;
  UObject *Existing = nullptr;
  TStrongObjectPtr<UObject> Proposed;
  TStrongObjectPtr<UObject> ActualAsset;
  bool VerifyGenerationIntent(UObject* Asset, FOperationFailure& OutFailure) const {
    if (Operation == EOperation::Remove) return true;
    UE::UAI::Contract::FDiagnostic Error;
    if (!GenerationInput::Verify(Asset, GenerationPlan, Error)) {
      SetFailure(OutFailure, Error);
      return false;
    }
    return true;
  }
  bool VerifyProfileStateIntent(UObject* Asset, const ProfileState::FIntentVerifier& Intent,
      FOperationFailure& OutFailure) const {
    if (Operation == EOperation::Remove || !Intent) return true;
    UE::UAI::Contract::FDiagnostic Error;
    if (!Asset) {
      Error = UE::UAI::Contract::FDiagnostic::Failure(TEXT("reflection_profile_state_intent_owner_missing"),
          TEXT("The profile state intent cannot be verified because its native asset is unavailable."), TEXT("profile_state_verify"));
      Error.SourceKind = TEXT("native_result");
      Error.TargetPath = PackagePath;
      Error.ActualValuePresent = false;
      Error.ExpectedValue = MakeShared<FJsonValueString>(Profile.AssetClassPath);
      SetFailure(OutFailure, Error);
      return false;
    }
    UeAgentAuthoringDiagnostics::FScopedSourceLocations ProfileSources(ProfileStateSourceRoot.Get(),
        [this](const FString& Pointer) {
          return UeAgentAuthoringDiagnostics::FScopedSourceLocations::ResolveProjection(ProfileStateSources, Pointer);
        });
    if (!Intent(*Asset, Error)) {
      if (Error.SourceKind == TEXT("authored_value"))
        UeAgentAuthoringDiagnostics::LocateAuthoringSource(Error, *ProfileStateSourceRoot, TEXT("profile_state"));
      SetFailure(OutFailure, Error);
      return false;
    }
    return true;
  }
  ProfileState::FIntentVerifier ProposedProfileStateIntent;
  mutable MeshMaterialRelations::FPropertyEdits MeshMaterialEdits;
  ProfileState::FIntentVerifier ActualProfileStateIntent;
  TSharedPtr<FJsonValue> ProfileStatePatch;
  TSharedPtr<FJsonObject> ProfileStateSourceRoot;
  TMap<FString, UE::UAI::Contract::FSourceLocation> ProfileStateSources;
  UE::UAI::Authoring::Timeline::FMovieSceneCreationState MovieSceneCreation;
  FAssetRemovalStage RemovalStage;
  UObject *Actual = nullptr;
  bool bHasProfileStatePatch = false;
  bool bSave = false;
  TUniquePtr<UeAgentAuthoringAssetRelations::FPlan> AssetRelationPlan;
  TUniquePtr<FEqPresetRuntimeState> EqRuntime;
  TUniquePtr<PaperSpriteState::FAtlasState> AtlasState;
  TUniquePtr<FRvtComponentReloadScope> RvtReloadScope;
  bool bRefreshSpriteSourceBasis = false;
  TSharedPtr<GenerationInput::FPreparedInput> GenerationPlan;
};

class FAssetLifecycle final : public IReflectionOperationLifecycle {
public:
  explicit FAssetLifecycle(TOptional<bool> InSaveOverride = {}) : SaveOverride(InSaveOverride) {}
  virtual bool
  PrepareLifecycle(const FPrepareRequest &Request,
                   TUniquePtr<IReflectionOperationLifecycleState> &OutState,
                   FOperationFailure &OutFailure) const override {
    OutState.Reset();
    OutFailure = FOperationFailure();
    if (!Request.IsValid() || !Request.Input->Target.IsValid()) {
      auto Actual = MakeShared<FJsonObject>();
      Actual->SetBoolField(TEXT("contract_present"), Request.Contract != nullptr);
      Actual->SetBoolField(TEXT("unit_present"), Request.Unit != nullptr);
      Actual->SetBoolField(TEXT("input_present"), Request.Input != nullptr);
      Actual->SetBoolField(TEXT("target_present"), Request.Input && Request.Input->Target.IsValid());
      SetFailure(OutFailure, LifecycleFailure(TEXT("reflection_apply_request_invalid"),
          TEXT("The prepared Reflection apply request is missing a required component."),
          FString(), TEXT("apply_request"), MakeShared<FJsonValueObject>(Actual),
          MakeShared<FJsonValueString>(TEXT("contract, workspace unit, operation input, and target present"))));
      return false;
    }
    const FProfile *Profile =
        FindProfileByTargetKind(Request.Contract->TargetKind);
    if (!Profile) {
      SetFailure(OutFailure, LifecycleFailure(TEXT("reflection_apply_profile_missing"),
          TEXT("No Reflection apply profile is registered for the contract target kind."),
          FString(), TEXT("target_kind"), MakeShared<FJsonValueString>(Request.Contract->TargetKind),
          MakeShared<FJsonValueString>(TEXT("a registered Reflection target kind"))));
      return false;
    }
    if (Profile->TypeId == TEXT("sound_class") || Profile->TypeId == TEXT("sound_submix")) {
      for (const auto& Property : Request.Input->Properties) {
        FString Path;
        if (!Property || Property->Type != EJson::Object
            || !Property->AsObject()->TryGetStringField(TEXT("property_path"), Path)) continue;
        FString Root = Path;
        int32 Boundary;
        if (Root.FindChar(TEXT('.'), Boundary)) Root.LeftInline(Boundary);
        if (Root.FindChar(TEXT('['), Boundary)) Root.LeftInline(Boundary);
        if ((Profile->TypeId == TEXT("sound_class") && FName(*Root) == TEXT("ChildClasses"))
            || (Profile->TypeId == TEXT("sound_submix") && (FName(*Root) == TEXT("ChildSubmixes") || FName(*Root) == TEXT("ParentSubmix")))) {
          auto Error = UeAgentAuthoringDiagnostics::MakeValueFailure(Profile->TypeId + TEXT("_relationship_surface_required"),
              TEXT("property_path"), MakeShared<FJsonValueString>(Profile->ProfileStateField), MakeShared<FJsonValueString>(Path),
              TEXT("preflight"), TEXT("Asset relationships must be edited through the profile relationship field."));
          Error.SourceKind = TEXT("authored_value");
          Error.Source.InternalJsonPath = TEXT("/property_path");
          UeAgentAuthoringDiagnostics::LocateAuthoringSource(Error, *Property->AsObject(), TEXT("property_path"));
          SetFailure(OutFailure, MoveTemp(Error));
          return false;
        }
      }
    }
    FString PackagePath;
    Request.Input->Target->TryGetStringField(TEXT("asset_path"), PackagePath);
    PackagePath = NormalizePackagePath(PackagePath);
    if (!FPackageName::IsValidLongPackageName(PackagePath) ||
        FPackageName::GetLongPackageAssetName(PackagePath).IsEmpty() ||
        (!PackagePath.Equals(TEXT("/Game")) &&
         !PackagePath.StartsWith(TEXT("/Game/")))) {
      auto Error = UeAgentAuthoringDiagnostics::MakeValueFailure(TEXT("invalid_asset_path"), TEXT("asset_path"),
          MakeShared<FJsonValueString>(TEXT("a valid asset package path under /Game")), Request.Input->Target->TryGetField(TEXT("asset_path")),
          TEXT("preflight"), TEXT("The target asset path is not a valid package path under /Game."));
      Error.SourceKind = TEXT("authored_value");
      Error.Source.InternalJsonPath = TEXT("/asset_path");
      Error.InternalContext = PackagePath;
      UeAgentAuthoringDiagnostics::LocateAuthoringSource(Error, *Request.Input->Target, TEXT("asset_path"));
      SetFailure(OutFailure, MoveTemp(Error));
      return false;
    }
    FString AssetKind;
    if (Request.Input->Target->TryGetStringField(TEXT("asset_kind"),
                                                 AssetKind) &&
        !AssetKind.Equals(Profile->TypeId, ESearchCase::IgnoreCase)) {
      auto Error = UeAgentAuthoringDiagnostics::MakeValueFailure(TEXT("reflection_asset_kind_conflict"), TEXT("asset_kind"),
          MakeShared<FJsonValueString>(Profile->TypeId), MakeShared<FJsonValueString>(AssetKind),
          TEXT("preflight"), TEXT("The requested asset kind conflicts with the selected Reflection profile."));
      Error.TargetPath = PackagePath;
      Error.SourceKind = TEXT("authored_value");
      Error.Source.InternalJsonPath = TEXT("/asset_kind");
      UeAgentAuthoringDiagnostics::LocateAuthoringSource(Error, *Request.Input->Target, TEXT("asset_kind"));
      SetFailure(OutFailure, MoveTemp(Error));
      return false;
    }
    FString RequestedClassPath;
    UClass *RequestedClass = nullptr;
    if (Request.Input->Target->TryGetStringField(TEXT("class_path"),
                                                 RequestedClassPath) &&
        !RequestedClassPath.TrimStartAndEnd().IsEmpty()) {
      RequestedClass =
          FSoftClassPath(RequestedClassPath).TryLoadClass<UObject>();
      UE::UAI::Contract::FDiagnostic ClassError;
      UClass *AcceptedClass = ResolveAssetClass(*Profile, false, ClassError);
      if (!RequestedClass) {
        auto Error = UeAgentAuthoringDiagnostics::MakeValueFailure(TEXT("reflection_asset_class_not_allowed"),
            TEXT("class_path"), MakeShared<FJsonValueString>(TEXT("a loadable UObject class")),
            MakeShared<FJsonValueString>(RequestedClassPath), TEXT("authoring_prepare"),
            TEXT("The requested asset class could not be loaded."));
        Error.TargetPath = PackagePath;
        UeAgentAuthoringDiagnostics::LocateAuthoringSource(Error, *Request.Input->Target, TEXT("class_path"));
        SetFailure(OutFailure, MoveTemp(Error));
        return false;
      }
      if (!AcceptedClass) {
        if (ClassError.TargetPath.IsEmpty()) ClassError.TargetPath = PackagePath;
        SetFailure(OutFailure, MoveTemp(ClassError));
        return false;
      }
      if (!RequestedClass->IsChildOf(AcceptedClass)) {
        auto Error = UeAgentAuthoringDiagnostics::MakeValueFailure(TEXT("reflection_asset_class_not_allowed"),
            TEXT("class_path"), MakeShared<FJsonValueString>(AcceptedClass->GetPathName()),
            MakeShared<FJsonValueString>(RequestedClass->GetPathName()), TEXT("authoring_prepare"),
            TEXT("The requested asset class is not derived from the reflection profile asset class."));
        Error.TargetPath = PackagePath;
        UeAgentAuthoringDiagnostics::LocateAuthoringSource(Error, *Request.Input->Target, TEXT("class_path"));
        SetFailure(OutFailure, MoveTemp(Error));
        return false;
      }
    }

    UE::UAI::Contract::FDiagnostic LoadError;
    UObject *Existing = LoadAsset(*Profile, PackagePath, LoadError);
    if (LoadError.IsValid()) {
      SetFailure(OutFailure, MoveTemp(LoadError));
      return false;
    }
    EOperation Operation = Request.Input->RequestedOperation;
    if (Operation == EOperation::Upsert)
      Operation = Existing ? EOperation::Update : EOperation::Create;
    TSharedPtr<FJsonValue> ProfileStatePatch;
    bool bHasProfileStatePatch = false;
    UE::UAI::Contract::FDiagnostic ProfileStateError;
    if (!ProfileState::PreparePatch(*Profile, Request.Input->ProviderExtension,
                                    ProfileStatePatch, bHasProfileStatePatch,
                                    ProfileStateError)) {
      SetFailure(OutFailure, ProfileStateError);
      return false;
    }
    if (Operation == EOperation::Remove && bHasProfileStatePatch) {
      auto Error = UeAgentAuthoringDiagnostics::MakeValueFailure(TEXT("reflection_profile_state_remove_not_allowed"),
          Profile->ProfileStateField, nullptr, ProfileStatePatch, TEXT("preflight"),
          TEXT("A remove operation cannot supply profile state changes."));
      Error.ExpectedValuePresent = false;
      Error.ActualValuePresent = true;
      Error.TargetPath = PackagePath;
      Error.SourceKind = TEXT("authored_value");
      UeAgentAuthoringDiagnostics::LocateAuthoringSource(Error, *Request.Input->ProviderExtension, Profile->ProfileStateField);
      SetFailure(OutFailure, MoveTemp(Error));
      return false;
    }
    if (Operation == EOperation::Remove && !Profile->GenerationInputField.IsEmpty()
        && Request.Input->ProviderExtension.IsValid() && Request.Input->ProviderExtension->HasField(Profile->GenerationInputField)) {
      auto Error = UeAgentAuthoringDiagnostics::MakeValueFailure(TEXT("generation_input_remove_not_allowed"),
          Profile->GenerationInputField, nullptr, Request.Input->ProviderExtension->TryGetField(Profile->GenerationInputField),
          TEXT("preflight"), TEXT("A remove operation cannot supply generation input."));
      Error.ExpectedValuePresent = false;
      Error.ActualValuePresent = true;
      Error.TargetPath = PackagePath;
      Error.SourceKind = TEXT("authored_value");
      UeAgentAuthoringDiagnostics::LocateAuthoringSource(Error, *Request.Input->ProviderExtension, Profile->GenerationInputField);
      SetFailure(OutFailure, MoveTemp(Error));
      return false;
    }
    if ((Operation == EOperation::Create || Operation == EOperation::Clone) &&
        Existing) {
      auto Error = UE::UAI::Contract::FDiagnostic::Failure(TEXT("asset_already_exists"),
          TEXT("Creating or cloning this asset requires the destination to be absent."), TEXT("authoring_prepare"));
      Error.SourceKind = TEXT("native_result");
      Error.TargetPath = PackagePath;
      Error.PropertyPath = TEXT("asset_present");
      Error.ActualValue = MakeShared<FJsonValueBoolean>(true);
      Error.ExpectedValue = MakeShared<FJsonValueBoolean>(false);
      SetFailure(OutFailure, MoveTemp(Error));
      return false;
    }
    if ((Operation == EOperation::Update || Operation == EOperation::Remove) &&
        !Existing) {
      auto Error = UE::UAI::Contract::FDiagnostic::Failure(TEXT("asset_not_found"),
          TEXT("Updating or removing this asset requires an existing destination asset."), TEXT("authoring_prepare"));
      Error.SourceKind = TEXT("native_result");
      Error.TargetPath = PackagePath;
      Error.PropertyPath = TEXT("asset_present");
      Error.ActualValue = MakeShared<FJsonValueBoolean>(false);
      Error.ExpectedValue = MakeShared<FJsonValueBoolean>(true);
      SetFailure(OutFailure, MoveTemp(Error));
      return false;
    }
    if (Existing && RequestedClass && Existing->GetClass() != RequestedClass) {
      auto Error = UeAgentAuthoringDiagnostics::MakeValueFailure(TEXT("reflection_asset_class_conflict"),
          TEXT("class_path"), MakeShared<FJsonValueString>(RequestedClassPath),
          MakeShared<FJsonValueString>(Existing->GetClass()->GetPathName()), TEXT("authoring_prepare"),
          TEXT("The existing asset class differs from the requested class."));
      Error.TargetPath = PackagePath;
      UeAgentAuthoringDiagnostics::LocateAuthoringSource(Error, *Request.Input->Target, TEXT("class_path"));
      SetFailure(OutFailure, MoveTemp(Error));
      return false;
    }

    UObject *Source = nullptr;
    if (Operation == EOperation::Clone) {
      FString SourcePath;
      Request.Input->Target->TryGetStringField(TEXT("source_asset_path"),
                                               SourcePath);
      Source = LoadAsset(*Profile, SourcePath, LoadError);
      if (LoadError.IsValid()) {
        SetFailure(OutFailure, MoveTemp(LoadError));
        return false;
      }
      if (!Source) {
        auto Error = UE::UAI::Contract::FDiagnostic::Failure(TEXT("source_asset_not_found"),
            TEXT("The source asset requested for cloning does not exist."), TEXT("authoring_prepare"));
        Error.SourceKind = TEXT("native_result");
        Error.TargetPath = NormalizePackagePath(SourcePath);
        Error.PropertyPath = TEXT("source_asset_present");
        Error.ActualValue = MakeShared<FJsonValueBoolean>(false);
        Error.ExpectedValue = MakeShared<FJsonValueBoolean>(true);
        SetFailure(OutFailure, MoveTemp(Error));
        return false;
      }
      if (RequestedClass && Source->GetClass() != RequestedClass) {
        auto Error = UeAgentAuthoringDiagnostics::MakeValueFailure(TEXT("reflection_asset_class_conflict"),
            TEXT("class_path"), MakeShared<FJsonValueString>(RequestedClassPath),
            MakeShared<FJsonValueString>(Source->GetClass()->GetPathName()), TEXT("authoring_prepare"),
            TEXT("The clone source asset class differs from the requested class."));
        Error.TargetPath = NormalizePackagePath(SourcePath);
        UeAgentAuthoringDiagnostics::LocateAuthoringSource(Error, *Request.Input->Target, TEXT("class_path"));
        SetFailure(OutFailure, MoveTemp(Error));
        return false;
      }
    }

    UObject *Proposed = nullptr;
    if (Operation != EOperation::Remove) {
      UPackage *Scratch = CreatePackage(
          *FString::Printf(TEXT("/Temp/UAI/Reflection/Proposed_%s"),
                           *FGuid::NewGuid().ToString(EGuidFormats::Digits)));
      UE::UAI::Contract::FDiagnostic Error;
      if (!Scratch) {
        Error = UE::UAI::Contract::FDiagnostic::Failure(TEXT("reflection_proposed_package_create_failed"),
            TEXT("Unreal could not create the temporary package for the prepared asset."), TEXT("authoring_prepare"));
        Error.SourceKind = TEXT("native_result");
        Error.TargetPath = PackagePath;
        Error.PropertyPath = TEXT("scratch_package_available");
        Error.ActualValue = MakeShared<FJsonValueBoolean>(false);
        Error.ExpectedValue = MakeShared<FJsonValueBoolean>(true);
        SetFailure(OutFailure, MoveTemp(Error));
        return false;
      }
      Proposed =
          Operation == EOperation::Update && Profile->TypeId == TEXT("sound_submix")
              ? SoundSubmixRelations::CreatePreview(*CastChecked<USoundSubmixBase>(Existing), *Scratch, Error)
              : Operation == EOperation::Create
              ? CreateFactorySeed(*Profile, Scratch, NAME_None,
                                  RF_Public | RF_Standalone | RF_Transactional,
                                  Error, RequestedClass)
              : DuplicateAsset(
                    Operation == EOperation::Clone ? Source : Existing, Scratch,
                    NAME_None, RF_Public | RF_Standalone | RF_Transactional, Error);
      if (!Proposed) {
        if (Error.TargetPath.IsEmpty()) Error.TargetPath = PackagePath;
        SetFailure(OutFailure, MoveTemp(Error));
        return false;
      }
      if (Operation == EOperation::Update &&
      !ProfileState::InitializeUpdatePreview(*Existing, *Proposed, PackagePath, Error)) {
        SetFailure(OutFailure, Error);
        TStrongObjectPtr<UObject> RejectedPreview(Proposed);
        RetireTransientAsset(RejectedPreview, Existing);
        return false;
      }
    }

    TUniquePtr<UeAgentAuthoringAssetRelations::FPlan> AssetRelationPlan;
    if ((Profile->TypeId == TEXT("sound_class") || Profile->TypeId == TEXT("sound_submix")) && bHasProfileStatePatch) {
      UE::UAI::Contract::FDiagnostic Error;
      AssetRelationPlan = MakeUnique<UeAgentAuthoringAssetRelations::FPlan>();
      bool bPrepared = false;
      if (!Existing || Operation != EOperation::Update) {
        auto Actual = MakeShared<FJsonObject>();
        Actual->SetBoolField(TEXT("existing_asset_present"), Existing != nullptr);
        Actual->SetBoolField(TEXT("update_operation"), Operation == EOperation::Update);
        Error = LifecycleFailure(TEXT("asset_relationship_target_missing"),
            TEXT("Relationship changes require an existing asset and an update operation."),
            PackagePath, TEXT("relationship_target"), MakeShared<FJsonValueObject>(Actual),
            MakeShared<FJsonValueString>(TEXT("an existing asset with update operation")));
      } else {
        bPrepared = Profile->TypeId == TEXT("sound_class")
            ? SoundClassRelations::Prepare(*CastChecked<USoundClass>(Existing), ProfileStatePatch, *AssetRelationPlan, Error)
            : SoundSubmixRelations::Prepare(*CastChecked<USoundSubmixBase>(Existing), ProfileStatePatch, *AssetRelationPlan, Error);
        if (!bPrepared && Error.SourceKind == TEXT("authored_value") && Request.Input->ProviderExtension.IsValid())
          UeAgentAuthoringDiagnostics::LocateAuthoringSource(Error, *Request.Input->ProviderExtension, Profile->ProfileStateField);
      }
      if (!bPrepared) {
        SetFailure(OutFailure, Error);
        TStrongObjectPtr<UObject> RejectedPreview(Proposed);
        RetireTransientAsset(RejectedPreview, Existing);
        return false;
      }
    }
    TUniquePtr<FEqPresetRuntimeState> EqRuntime;
    if (Profile->TypeId == TEXT("audio_effect_preset") && Existing && Operation == EOperation::Update) {
      EqRuntime = MakeUnique<FEqPresetRuntimeState>();
      UE::UAI::Contract::FDiagnostic Error;
      if (!EqRuntime->Capture(*CastChecked<USubmixEffectSubmixEQPreset>(Existing), Error)) {
        SetFailure(OutFailure, Error);
        TStrongObjectPtr<UObject> RejectedPreview(Proposed);
        RetireTransientAsset(RejectedPreview, Existing);
        return false;
      }
    }
    TUniquePtr<PaperSpriteState::FAtlasState> AtlasState;
    if ((Profile->TypeId == TEXT("paper_sprite") || Profile->TypeId == TEXT("paper_sprite_atlas")) && Proposed) {
      UE::UAI::Contract::FDiagnostic Error;
      AtlasState = MakeUnique<PaperSpriteState::FAtlasState>();
      if (!AtlasState->Prepare(Existing, *Proposed, PackagePath + TEXT(".") + FPackageName::GetLongPackageAssetName(PackagePath), Request.Input->Properties, Error)) {
        SetFailure(OutFailure, Error);
        AtlasState->Release();
        TStrongObjectPtr<UObject> RejectedPreview(Proposed);
        RetireTransientAsset(RejectedPreview, Existing);
        return false;
      }
    }
    TSharedPtr<GenerationInput::FPreparedInput> GenerationPlan;
    if (Proposed) {
      UE::UAI::Contract::FDiagnostic Error;
      if (!GenerationInput::Prepare(*Profile, *Proposed, Request.Input->ProviderExtension, PackagePath, GenerationPlan, Error)) {
        SetFailure(OutFailure, Error);
        TStrongObjectPtr<UObject> RejectedPreview(Proposed);
        RetireTransientAsset(RejectedPreview, Existing);
        return false;
      }
    }
    OutState = MakeUnique<FAssetLifecycleState>(
        *Profile, PackagePath, Operation, Existing, Proposed,
        MoveTemp(ProfileStatePatch), bHasProfileStatePatch,
        SaveOverride.IsSet() ? SaveOverride.GetValue()
            : ReadBool(Request.Input->ProviderExtension, TEXT("save"), false), MoveTemp(AssetRelationPlan), MoveTemp(EqRuntime), MoveTemp(AtlasState), MoveTemp(GenerationPlan));
    return true;
  }
private:
  TOptional<bool> SaveOverride;
};

FInputFieldDescriptor Field(const TCHAR *Name, const TCHAR *Role,
                            const TCHAR *TypeId,
                            const TArray<EOperation> &RequiredFor = {}) {
  FInputFieldDescriptor Result;
  Result.Name = Name;
  Result.Role = Role;
  Result.TypeId = TypeId;
  Result.RequiredFor = RequiredFor;
  return Result;
}

FOperationDescriptor OperationDescriptor(const EOperation Operation) {
  FOperationDescriptor Result;
  Result.Operation = Operation;
  Result.IdentityOneOf = {{TEXT("asset_path")}};
  Result.AcceptedSections = {TEXT("target"), TEXT("provider_extension")};
  if (Operation != EOperation::Remove)
    Result.AcceptedSections.Add(TEXT("properties"));
  if (Operation == EOperation::Create) {
    Result.OmittedFieldPolicy = TEXT("ue_native_default");
    Result.NativeDefaultPolicy = TEXT("required");
  } else if (Operation == EOperation::Update) {
    Result.OmittedFieldPolicy = TEXT("preserve_current");
    Result.NativeDefaultPolicy = TEXT("forbidden");
  } else if (Operation == EOperation::Clone ||
             Operation == EOperation::Upsert) {
    Result.OmittedFieldPolicy = TEXT("ue_native_default");
    Result.NativeDefaultPolicy = TEXT("allowed");
  } else {
    Result.OmittedFieldPolicy = TEXT("not_applicable");
    Result.NativeDefaultPolicy = TEXT("not_applicable");
  }
  Result.bDestructive = Operation == EOperation::Remove;
  return Result;
}

FContract MakeInputContract(const FProfile &Profile) {
  FContract Result;
  Result.Schema = TEXT("uai.apply_input_contract.v1");
  Result.ContractId = Profile.InputContractId;
  Result.ContractVersion = TEXT("1.0.0");
  Result.ProfileId = Profile.TypeId;
  Result.TargetKind = Profile.TargetKind;
  Result.SemanticDescriptorRefs = {Profile.SeedSemanticId,
                                   Profile.ExportTargetSemanticId};
  if (!Profile.ProfileStateSemanticId.IsEmpty()) {
    Result.SemanticDescriptorRefs.Add(Profile.ProfileStateSemanticId);
  }
  if (!Profile.GenerationInputSemanticId.IsEmpty()) {
    Result.SemanticDescriptorRefs.Add(Profile.GenerationInputSemanticId);
    Result.SemanticDescriptorRefs.Add(Profile.GeneratedStateSemanticId);
  }
  Result.ReflectionBaseline.bEnabled = true;
  Result.ReflectionBaseline.RootKinds = {TEXT("uobject"), TEXT("ustruct")};
  Result.ReflectionBaseline.RecursiveNodes = {
      TEXT("scalar"),    TEXT("struct"),      TEXT("owned_object"),
      TEXT("reference"), TEXT("array"),       TEXT("set"),
      TEXT("map"),       TEXT("fixed_array"), TEXT("optional")};
  Result.ReflectionBaseline.UnknownFieldPolicy = TEXT("reject");
  Result.ReflectionBaseline.ExternalReferencePolicy =
      TEXT("replace_reference_only");
  Result.ReflectionBaseline.OwnedObjectPolicy =
      TEXT("create_or_locate_with_real_outer_then_recurse");
  Result.PropertyPayload.Mode = TEXT("reflection");
  Result.PropertyPayload.PropertyPathField = TEXT("property_path");
  Result.PropertyPayload.ValueField = TEXT("value");
  Result.PropertyPayload.OptionalAssertionFields = {
      TEXT("kind"),         TEXT("struct_path"),  TEXT("enum_path"),
      TEXT("element_type"), TEXT("key_type"),     TEXT("mapped_type"),
      TEXT("inner_type"),   TEXT("fixed_length"), TEXT("allowed_class")};
  Result.PropertyPayload.OmittedPatchFieldPolicy = TEXT("preserve_current");
  TArray<EOperation> Operations = {EOperation::Update, EOperation::Remove,
                                   EOperation::Clone};
  if (Profile.bAllowCreate) {
    Operations.Insert(EOperation::Create, 0);
    Operations.Add(EOperation::Upsert);
  }
  Result.InputFields = {
      Field(TEXT("asset_path"), TEXT("identity"), TEXT("ue_asset_path"),
            Operations),
      Field(TEXT("asset_kind"), TEXT("type_assertion"), *Profile.TypeId),
      Field(TEXT("class_path"), TEXT("type_assertion"), TEXT("ue_class_path")),
      Field(TEXT("source_asset_path"), TEXT("selector"), TEXT("ue_asset_path"),
            {EOperation::Clone}),
      Field(TEXT("operation"), TEXT("operation_control"),
            TEXT("apply_operation"), Operations),
      Field(TEXT("properties"), TEXT("intent"),
            TEXT("property_descriptor_list")),
      Field(TEXT("save"), TEXT("provider_extension"), TEXT("bool")),
  };
  if (!Profile.ProfileStateField.IsEmpty()) {
    Result.InputFields.Add(Field(*Profile.ProfileStateField,
                                 TEXT("provider_extension"),
                                 *Profile.ProfileStateInputType));
  }
  if (!Profile.GenerationInputField.IsEmpty())
    Result.InputFields.Add(Field(*Profile.GenerationInputField, TEXT("provider_extension"), TEXT("json_object")));
  for (const EOperation Item : Operations)
    Result.Operations.Add(OperationDescriptor(Item));
  Result.ProviderId = ApplyProviderId;
  Result.UnknownInputFieldPolicy = TEXT("reject");
  Result.Verification.StateSource = TEXT("shared_state_snapshot");
  Result.Verification.CompareSource = TEXT("shared_semantic_compare");
  Result.Verification.bRequired = true;
  return Result;
}

UeAgentApplyWorkspaceContract::FContract
MakeWorkspaceContract(const FProfile &Profile) {
  using namespace UeAgentApplyWorkspaceContract;
  UeAgentApplyWorkspaceContract::FContract Result;
  Result.Schema = TEXT("uai.apply_workspace_contract.v1");
  Result.ContractId = Profile.WorkspaceContractId;
  Result.ContractVersion = TEXT("1.0.0");
  Result.ProfileId = Profile.TypeId;
  Result.Sources = {
      {ESourceKind::Directory, TEXT("physical"), TEXT("index")},
      {ESourceKind::SingleFile, TEXT("virtual"), TEXT("object")},
      {ESourceKind::Inline, TEXT("virtual"), TEXT("object")},
  };
  Result.Entry.DirectoryEntry = TEXT("index.hjson");
  Result.Entry.VirtualUnitId = TEXT("asset");
  FUnitDescriptor Unit;
  Unit.UnitId = TEXT("asset");
  Unit.PathPattern = TEXT("index.hjson");
  Unit.Role = TEXT("object");
  Unit.Cardinality = TEXT("one");
  Unit.bRequired = true;
  Unit.RouteId = Profile.TypeId;
  Unit.ApplyContractRef = Profile.InputContractId;
  Unit.MergePolicy = TEXT("replace");
  Unit.AbsentSemantics = TEXT("error");
  Unit.EmptySemantics = TEXT("error");
  Result.Units = {Unit};
  Result.ReferencePolicy.Base = TEXT("workspace_root");
  Result.ReferencePolicy.RootContainment = TEXT("required");
  Result.ReferencePolicy.ExternalReferences = TEXT("reject");
  Result.ReferencePolicy.CaseCollision = TEXT("reject");
  Result.DependencyPolicy.EdgeSource = TEXT("contract_and_references");
  Result.DependencyPolicy.CyclePolicy = TEXT("reject");
  Result.DependencyPolicy.UnresolvedPolicy = TEXT("reject");
  Result.DependencyPolicy.Order = TEXT("stable_topological");
  Result.Preflight.Scope = TEXT("whole_workspace");
  Result.Preflight.MutationBeforeSuccess = TEXT("forbid");
  Result.Preflight.bDiagnosticsIncludeSource = true;
  Result.Transaction.Mode = TEXT("atomic");
  Result.Transaction.RollbackOn = {TEXT("mutation_failed"),
                                   TEXT("compile_failed"), TEXT("save_failed"),
                                   TEXT("verification_failed")};
  Result.Execution.Compile = TEXT("provider_hook");
  Result.Execution.Save = TEXT("caller_controlled");
  Result.Execution.Verification = TEXT("shared_snapshot_required");
  Result.UnknownFilePolicy = TEXT("reject");
  Result.DuplicateTargetPolicy = TEXT("reject");
  return Result;
}

bool BuildDependencyGraph(
    const UeAgentApplyWorkspaceContract::FContract &Contract,
    const UeAgentAuthoringWorkspaceRouter::FWorkspace &Workspace,
    UeAgentAuthoringWorkspaceDependencyGraph::FDependencyGraph &OutGraph,
    UE::UAI::Contract::FDiagnostic& OutDiagnostic) {
  UeAgentAuthoringWorkspaceDependencyGraph::FGraphFailure Failure;
  if (UeAgentAuthoringWorkspaceDependencyGraph::BuildDependencyGraph(
          Contract, Workspace,
          UeAgentAuthoringWorkspaceDependencyGraph::FDependencyInputs(),
          OutGraph, Failure))
    return true;
  OutDiagnostic = Failure.Diagnostic;
  return false;
}

bool LoadWorkspace(
    const FProfile &Profile,
    const UeAgentAuthoringWorkspaceLoader::FSourceRequest &Source,
    UeAgentApplyWorkspaceContract::FContract &OutContract,
    UeAgentAuthoringWorkspaceLoader::FLoadResult &OutLoad,
    UE::UAI::Contract::FDiagnostic& OutDiagnostic) {
  if (!UeAgentApplyWorkspaceContract::FRegistry::Get().FindContract(
          Profile.WorkspaceContractId, OutContract)) {
    auto Actual = MakeShared<FJsonObject>();
    Actual->SetStringField(TEXT("profile_type"), Profile.TypeId);
    Actual->SetStringField(TEXT("workspace_contract"), Profile.WorkspaceContractId);
    OutDiagnostic = LifecycleFailure(TEXT("reflection_workspace_contract_missing"),
        TEXT("The workspace contract required by the Reflection profile is not registered."),
        FString(), TEXT("workspace_contract"), MakeShared<FJsonValueObject>(Actual),
        MakeShared<FJsonValueString>(TEXT("a registered workspace contract")));
    return false;
  }
  if (!UeAgentAuthoringWorkspaceLoader::LoadWorkspace(OutContract, Source,
                                                      OutLoad)) {
    OutDiagnostic = OutLoad.Diagnostic;
    return false;
  }
  return true;
}
} // namespace

TSharedRef<FJsonObject> FAssetApplyResult::ToJsonObject() const {
  TSharedRef<FJsonObject> Result = MakeShared<FJsonObject>();
  Result->SetStringField(TEXT("status"), Status);
  Result->SetStringField(TEXT("phase"), Phase);
  Result->SetStringField(TEXT("profile"), ProfileId);
  Result->SetStringField(TEXT("asset_path"), AssetPath);
  Result->SetStringField(TEXT("resolved_operation"), ResolvedOperation);
  Result->SetNumberField(TEXT("units_checked"), UnitsChecked);
  Result->SetNumberField(TEXT("mutations_applied"), MutationsApplied);
  Result->SetBoolField(TEXT("created"), bCreated);
  Result->SetBoolField(TEXT("removed"), bRemoved);
  Result->SetBoolField(TEXT("no_op"), bNoOp);
  Result->SetBoolField(TEXT("rollback_attempted"), bRollbackAttempted);
  Result->SetBoolField(TEXT("rollback_succeeded"), bRollbackSucceeded);
  if (!Diagnostic.IsEmpty())
  {
    Result->SetStringField(TEXT("error_code"), Diagnostic.Code);
    Result->SetObjectField(TEXT("diagnostic"), Diagnostic.ToJson());
  }
  return Result;
}

TSharedRef<FJsonObject> FWorkspaceInspectResult::ToJsonObject() const {
  TSharedRef<FJsonObject> Result = MakeShared<FJsonObject>();
  Result->SetStringField(TEXT("status"), Status);
  Result->SetStringField(TEXT("profile"), ProfileId);
  Result->SetStringField(TEXT("asset_path"), AssetPath);
  Result->SetStringField(TEXT("requested_operation"), RequestedOperation);
  Result->SetNumberField(TEXT("units_checked"), UnitsChecked);
  Result->SetBoolField(TEXT("asset_exists"), bAssetExists);
  Result->SetBoolField(TEXT("would_create"), bWouldCreate);
  Result->SetBoolField(TEXT("no_op"), bNoOp);
  if (!Diagnostic.IsEmpty())
  {
    Result->SetStringField(TEXT("error_code"), Diagnostic.Code);
    Result->SetObjectField(TEXT("diagnostic"), Diagnostic.ToJson());
  }
  return Result;
}

bool RegisterApplyContracts(UE::UAI::Contract::FDiagnostic& OutError) {
  for (const FProfile &Profile : GetProfiles()) {
    UeAgentApplyInputContract::ERegistrationResult InputResult;
    if (!UeAgentApplyInputContract::FRegistry::Get().RegisterContract(
            MakeInputContract(Profile), InputResult, OutError))
      return false;
    UeAgentApplyWorkspaceContract::ERegistrationResult WorkspaceResult;
    if (!UeAgentApplyWorkspaceContract::FRegistry::Get().RegisterContract(
            MakeWorkspaceContract(Profile), WorkspaceResult, OutError))
      return false;
  }
  return true;
}

TSharedRef<IApplyProvider> MakeApplyProvider(bool bCompactValues, bool bEmitEditorNotifications, TOptional<bool> SaveOverride) {
  const TSharedRef<IReflectionOperationLifecycle> Lifecycle =
      MakeShared<FAssetLifecycle>(SaveOverride);
  UeAgentAuthoringObjectApply::FOptions Options;
  Options.bAllowDomainAuthorizedOwners = true;
  Options.bEmitEditorNotifications = bEmitEditorNotifications;
  Options.bCompactValues = bCompactValues;
  return MakeShared<FReflectionApplyProvider>(ApplyProviderId, Lifecycle,
                                              Options);
}

bool InspectAssetSource(
    const FProfile &Profile,
    const UeAgentAuthoringWorkspaceLoader::FSourceRequest &Source,
    const FString &ExpectedAssetPath, FWorkspaceInspectResult &OutResult) {
  OutResult = FWorkspaceInspectResult();
  OutResult.Status = TEXT("failed");
  OutResult.ProfileId = Profile.TypeId;
  UeAgentApplyWorkspaceContract::FContract Contract;
  UeAgentAuthoringWorkspaceLoader::FLoadResult Load;
  if (!LoadWorkspace(Profile, Source, Contract, Load, OutResult.Diagnostic))
    return false;
  UeAgentAuthoringAssetWorkspaceInput::FTargetRequest TargetRequest;
  TargetRequest.ExpectedAssetPath = ExpectedAssetPath;
  TargetRequest.Errors.EntryInvalid = TEXT("reflection_workspace_entry_invalid");
  TargetRequest.Errors.AssetPathConflict =
      TEXT("reflection_workspace_asset_path_conflict");
  UeAgentAuthoringAssetWorkspaceInput::FTargetResult Target;
  if (!UeAgentAuthoringAssetWorkspaceInput::ResolveTarget(
          Load.Workspace, TargetRequest, Target)) {
    OutResult.Diagnostic = Target.Diagnostic;
    return false;
  }
  const EOperation Operation = Target.Input.RequestedOperation;
  OutResult.AssetPath = Target.AssetPath;
  OutResult.RequestedOperation = OperationToString(Operation);
  OutResult.bAssetExists = LoadAsset(Profile, OutResult.AssetPath, OutResult.Diagnostic) != nullptr;
  if (OutResult.Diagnostic.IsValid()) return false;
  OutResult.bWouldCreate =
      Operation == EOperation::Create ||
      (Operation == EOperation::Upsert && !OutResult.bAssetExists);
  UeAgentAuthoringWorkspaceDependencyGraph::FDependencyGraph Graph;
  if (!BuildDependencyGraph(Contract, Load.Workspace, Graph,
                            OutResult.Diagnostic))
    return false;
  FWorkspaceOperationPreflightProvider Provider(
      UeAgentApplyInputContract::FRegistry::Get(), FProviderRegistry::Get());
  UeAgentAuthoringWorkspaceTransaction::FPreflightResult Preflight;
  if (!UeAgentAuthoringWorkspaceTransaction::PreflightWorkspace(
          Contract, Load.Workspace, Graph, Provider, Preflight)) {
    const FApplyFailureProjection Failure = ProjectApplyFailure(Preflight);
    OutResult.UnitsChecked = Preflight.UnitsChecked;
    OutResult.Diagnostic = Failure.Diagnostic;
    return false;
  }
  OutResult.UnitsChecked = Preflight.UnitsChecked;
  OutResult.bNoOp = Preflight.Units.Num() == 1 &&
                    Preflight.Units[0].Mutation.IsValid() &&
                    Preflight.Units[0].Mutation->IsStateNoOp();
  OutResult.Status = TEXT("ready");
  return true;
}

bool ApplyAssetSource(
    const FProfile &Profile,
    const UeAgentAuthoringWorkspaceLoader::FSourceRequest &Source,
    const FString &ExpectedAssetPath, FAssetApplyResult &OutResult,
    const TOptional<bool> SaveOverride, const FString &RequestId,
    const bool bCompactValues,
    TFunction<bool(FOperationFailure &)> FinalReadback,
    const TSharedPtr<IDurablePackageStager> &PackageStager,
    bool bEmitEditorNotifications) {
  OutResult = FAssetApplyResult();
  OutResult.Status = TEXT("failed");
  OutResult.ProfileId = Profile.TypeId;
  UeAgentApplyWorkspaceContract::FContract Contract;
  UeAgentAuthoringWorkspaceLoader::FLoadResult Load;
  if (!LoadWorkspace(Profile, Source, Contract, Load, OutResult.Diagnostic)) {
    OutResult.Phase = TEXT("load");
    return false;
  }
  UeAgentAuthoringAssetWorkspaceInput::FTargetRequest TargetRequest;
  TargetRequest.ExpectedAssetPath = ExpectedAssetPath;
  TargetRequest.Errors.EntryInvalid = TEXT("reflection_workspace_entry_invalid");
  TargetRequest.Errors.AssetPathConflict =
      TEXT("reflection_workspace_asset_path_conflict");
  UeAgentAuthoringAssetWorkspaceInput::FTargetResult Target;
  if (!UeAgentAuthoringAssetWorkspaceInput::ResolveTarget(
          Load.Workspace, TargetRequest, Target)) {
    OutResult.Phase = Target.Phase;
    OutResult.Diagnostic = Target.Diagnostic;
    return false;
  }
  const FOperationInput &OperationInput = Target.Input;
  const EOperation RequestedOperation = OperationInput.RequestedOperation;
  OutResult.AssetPath = Target.AssetPath;
  const bool bExisted = LoadAsset(Profile, OutResult.AssetPath, OutResult.Diagnostic) != nullptr;
  if (OutResult.Diagnostic.IsValid()) {
    OutResult.Phase = TEXT("preflight");
    return false;
  }
  bool bSave = false;
  if (OperationInput.ProviderExtension.IsValid())
    OperationInput.ProviderExtension->TryGetBoolField(TEXT("save"), bSave);
  if (SaveOverride.IsSet())
    bSave = SaveOverride.GetValue();
  FBackendOptions BackendOptions;
  BackendOptions.FinalReadback = MoveTemp(FinalReadback);
  if (PackageStager.IsValid())
    BackendOptions.PackageStager = PackageStager;
  BackendOptions.bUseEditorTransaction =
      RequestedOperation != EOperation::Remove;
  BackendOptions.TransactionDescription =
      NSLOCTEXT("UeAgentInterface", "ReflectionAssetWorkspaceApply",
                "Apply reflection asset workspace");
  FApplyWorkspaceResult ApplyResult;
  UeAgentAuthoringAssetWorkspaceApply::FFailure ApplyFailure;
  FProviderRegistry LocalProviders;
  const FProviderRegistry *Providers = &FProviderRegistry::Get();
  if (bCompactValues || bEmitEditorNotifications || SaveOverride.IsSet()) {
    UeAgentAuthoringOperationApply::ERegistrationResult Registered;
    UE::UAI::Contract::FDiagnostic RegistrationError;
    if (!LocalProviders.RegisterProvider(MakeApplyProvider(bCompactValues, bEmitEditorNotifications, SaveOverride), Registered, RegistrationError)) {
      OutResult.Phase = TEXT("preflight");
      OutResult.Diagnostic = RegistrationError; if (OutResult.Diagnostic.Stage.IsEmpty()) OutResult.Diagnostic.Stage = OutResult.Phase;
      if (OutResult.Diagnostic.TargetPath.IsEmpty()) OutResult.Diagnostic.TargetPath = OutResult.AssetPath;
      return false;
    }
    Providers = &LocalProviders;
  }
  const bool bApplied = [&]() {
    // Atlas regeneration can offer optional repacking while native package reload
    // runs. Keep its existing layout in this synchronous automation scope, including
    // rollback; never leave an unattended flag alive in a retained lifecycle state.
    TGuardValue<bool> UnattendedGuard(GIsRunningUnattendedScript,
        GIsRunningUnattendedScript || Profile.TypeId == TEXT("paper_sprite")
        || Profile.TypeId == TEXT("paper_sprite_atlas"));
    return UeAgentAuthoringAssetWorkspaceApply::Execute(
        Contract, Load.Workspace, *Providers, RequestId, bSave,
        BackendOptions, ApplyResult, ApplyFailure);
  }();
  OutResult.UnitsChecked = ApplyResult.Preflight.UnitsChecked;
  OutResult.MutationsApplied = ApplyResult.Execution.MutationsApplied;
  OutResult.bRollbackAttempted = ApplyResult.Execution.bRollbackAttempted;
  OutResult.bRollbackSucceeded = ApplyResult.Execution.bRollbackSucceeded;
  if (!bApplied) {
    OutResult.Phase = ApplyFailure.Phase;
    OutResult.Diagnostic = ApplyFailure.Diagnostic;
    if (OutResult.Diagnostic.TargetPath.IsEmpty()) OutResult.Diagnostic.TargetPath = OutResult.AssetPath;
    UE_LOG(LogTemp, Warning, TEXT("UAI Reflection apply failed: target=%s phase=%s code=%s detail=%s"),
        *OutResult.AssetPath, *OutResult.Phase, *OutResult.Diagnostic.Code, *OutResult.Diagnostic.InternalContext);
    return false;
  }
  const EOperation Resolved =
      RequestedOperation == EOperation::Upsert
          ? (bExisted ? EOperation::Update : EOperation::Create)
          : RequestedOperation;
  OutResult.Status = TEXT("applied");
  OutResult.Phase = TEXT("complete");
  OutResult.ResolvedOperation = OperationToString(Resolved);
  OutResult.bCreated =
      Resolved == EOperation::Create || Resolved == EOperation::Clone;
  OutResult.bRemoved = Resolved == EOperation::Remove;
  OutResult.bNoOp = ApplyResult.Execution.MutationsApplied == 0;
  return true;
}

bool ApplyAsset(const FAssetApplyRequest &Request, FAssetApplyResult &OutResult,
                const FString &RequestId) {
  const FProfile *Profile = FindProfileByTypeId(Request.TypeId);
  if (!Profile) {
    OutResult = FAssetApplyResult();
    OutResult.Phase = TEXT("preflight");
    OutResult.Diagnostic = UE::UAI::Contract::FDiagnostic::Failure(TEXT("reflection_apply_profile_missing"),
        TEXT("No Reflection apply profile is registered for the requested type."), OutResult.Phase);
    OutResult.Diagnostic.SourceKind = TEXT("command_input");
    OutResult.Diagnostic.PropertyPath = TEXT("type_id");
    OutResult.Diagnostic.ActualValue = MakeShared<FJsonValueString>(Request.TypeId);
    OutResult.Diagnostic.ExpectedValue = MakeShared<FJsonValueString>(TEXT("a registered Reflection profile type"));
    return false;
  }
  TSharedRef<FJsonObject> Target = MakeShared<FJsonObject>();
  Target->SetStringField(TEXT("asset_path"),
                         NormalizePackagePath(Request.AssetPath));
  Target->SetStringField(TEXT("asset_kind"), Profile->TypeId);
  if (!Request.ClassPath.IsEmpty())
    Target->SetStringField(TEXT("class_path"), Request.ClassPath);
  if (!Request.SourceAssetPath.IsEmpty())
    Target->SetStringField(TEXT("source_asset_path"),
                           NormalizePackagePath(Request.SourceAssetPath));
  TSharedRef<FJsonObject> Extension = MakeShared<FJsonObject>();
  if (Request.ProviderExtension.IsValid()) {
    for (const TPair<FString, TSharedPtr<FJsonValue>> &Pair :
         Request.ProviderExtension->Values) {
      Extension->SetField(Pair.Key, Pair.Value);
    }
  }
  Extension->SetBoolField(TEXT("save"), Request.bSave);
  TSharedRef<FJsonObject> Document = MakeShared<FJsonObject>();
  Document->SetStringField(TEXT("operation"),
                           OperationToString(Request.Operation));
  Document->SetObjectField(TEXT("target"), Target);
  Document->SetObjectField(TEXT("provider_extension"), Extension);
  if (!Request.Properties.IsEmpty())
    Document->SetArrayField(TEXT("properties"), Request.Properties);
  return ApplyAssetSource(
      *Profile,
      UeAgentAuthoringWorkspaceLoader::FSourceRequest::MakeInlineObject(
          Document, TEXT("inline://reflection-asset-apply")),
      Request.AssetPath, OutResult, TOptional<bool>(), RequestId,
      Request.bCompactValues, Request.FinalReadback, Request.PackageStager,
      Request.bEmitEditorNotifications);
}
} // namespace UeAgentReflectionAuthoring
