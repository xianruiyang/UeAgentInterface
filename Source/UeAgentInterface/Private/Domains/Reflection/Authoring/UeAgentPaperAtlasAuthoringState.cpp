// Copyright Epic Games, Inc. All Rights Reserved.
#include "Domains/Reflection/Authoring/UeAgentPaperAtlasAuthoringState.h"

#include "Core/Contract/UeAgentDiagnostic.h"
#include "Domains/Reflection/Authoring/UeAgentPaperSpriteAuthoringState.h"
#include "Domains/Reflection/Authoring/UeAgentReflectionAuthoringProfileState.h"
#include "Utilities/AuthoringCore/UeAgentAuthoringDiagnostics.h"
#include "AssetRegistry/AssetRegistryModule.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Engine/Texture2D.h"
#include "Misc/Crc.h"
#include "Misc/PackageName.h"
#include "Misc/ScopeExit.h"
#include "PaperSprite.h"
#include "PaperSpriteAtlas.h"
#include "Serialization/ArchiveReplaceObjectRef.h"
#include "UObject/Package.h"
#include "UObject/AssetRegistryTagsContext.h"
#include "UObject/StrongObjectPtr.h"
#include "Utilities/AuthoringObjectApply/UeAgentAuthoringObjectApply.h"
#include "Utilities/AuthoringOperationApply/UeAgentAuthoringOperationCoordinator.h"

namespace UeAgentReflectionAuthoring::PaperSpriteState {
namespace {
struct FSpriteMembers : UPaperSprite {
  static auto Atlas() { return &FSpriteMembers::AtlasGroup; }
  static auto Previous() { return &FSpriteMembers::PreviousAtlasGroup; }
  static auto BakedTexture() { return &FSpriteMembers::BakedSourceTexture; }
  static auto BakedUV() { return &FSpriteMembers::BakedSourceUV; }
  static auto BakedSize() { return &FSpriteMembers::BakedSourceDimension; }
};
UE::UAI::Contract::FDiagnostic NativeFailure(const FString& Code, const FString& Reason,
    const FString& Target, const FString& Field, const TSharedPtr<FJsonValue>& Actual,
    const TSharedPtr<FJsonValue>& Expected) {
  auto Error = UeAgentAuthoringDiagnostics::MakeValueFailure(Code, Field, Expected, Actual,
      TEXT("paper_atlas"), Reason);
  Error.SourceKind = TEXT("native_result");
  Error.TargetPath = Target;
  return Error;
}
IAssetRegistry& Registry() {
  return FModuleManager::LoadModuleChecked<FAssetRegistryModule>(TEXT("AssetRegistry")).Get();
}
}

struct FAtlasState::FImpl {
  UObject* Proposed = nullptr;
  FString TargetPath;
  TArray<FString> Paths;
  TArray<TStrongObjectPtr<UObject>> Actual;
  TArray<TStrongObjectPtr<UObject>> Preview;
  TMap<UObject*, UObject*> Copies;
  TMap<UObject*, FString> CanonicalPaths;
  TArray<UPaperSprite*> Registered;

  void Add(UObject* Object) {
    if (!Object || Paths.Contains(Object->GetPathName())) return;
    Paths.Add(Object->GetPathName());
    Actual.Emplace(Object);
  }
  void Expand() {
    for (int32 Index = 0; Index < Actual.Num(); ++Index) {
      UObject* Object = Actual[Index].Get();
      if (auto* Sprite = Cast<UPaperSprite>(Object)) {
        Add(const_cast<UPaperSpriteAtlas*>(Sprite->GetAtlasGroup()));
        Add((Sprite->*FSpriteMembers::Previous()).LoadSynchronous());
      }
      if (auto* Atlas = Cast<UPaperSpriteAtlas>(Object)) {
        for (const auto& Slot : Atlas->AtlasSlots) Add(Slot.SpriteRef.LoadSynchronous());
        TMultiMap<FName, FString> Tags;
        Tags.Add(TEXT("AtlasGroupGUID"), Atlas->AtlasGUID.ToString(EGuidFormats::Digits));
        TArray<FAssetData> Assets;
        Registry().GetAssetsByTagValues(Tags, Assets);
        for (const auto& Asset : Assets)
          if (auto* Sprite = Cast<UPaperSprite>(Asset.GetAsset()))
            if (Sprite->GetAtlasGroup() == Atlas) Add(Sprite);
      }
    }
  }
  void ClearPreview() {
    for (auto* Sprite : Registered) {
      Sprite->ClearFlags(RF_Transient);
      Registry().AssetDeleted(Sprite);
      if (Sprite == Proposed) Sprite->SetFlags(RF_Transient);
    }
    Registered.Reset();
    for (auto& Copy : Preview) {
      if (Copy.Get() == Proposed) continue;
      Copy->ClearFlags(RF_Public | RF_Standalone);
      Copy->MarkAsGarbage();
    }
    Preview.Reset();
    Copies.Reset();
    CanonicalPaths.Reset();
  }
  FString Path(const UObject* Object) const {
    if (!Object) return {};
    if (const FString* Canonical = CanonicalPaths.Find(const_cast<UObject*>(Object))) return *Canonical;
    return Object->GetPathName();
  }
};

FAtlasState::FAtlasState() : Impl(MakeUnique<FImpl>()) {}
FAtlasState::~FAtlasState() { Impl->ClearPreview(); }

bool FAtlasState::Prepare(UObject* Existing, UObject& Proposed, const FString& TargetPath,
    const TArray<TSharedPtr<FJsonValue>>& Properties, UE::UAI::Contract::FDiagnostic& Error) {
  Impl->Proposed = &Proposed;
  Impl->TargetPath = TargetPath;
  // Registry queries include loaded public objects, even without AssetCreated.
  // Exclude this fresh duplicate before discovering the real atlas membership.
  Proposed.SetFlags(RF_Transient);
  Impl->Add(Existing);
  // Only discover the requested atlas reference here. Other properties retain
  // their original values until the shared ordered normalization stage.
  if (auto* Sprite = Cast<UPaperSprite>(&Proposed)) {
    // PreviousAtlasGroup is an unreflected editor cache: UObject duplication
    // does not copy it. Seed the pre-edit relationship before applying intent.
    Sprite->*FSpriteMembers::Previous() = const_cast<UPaperSpriteAtlas*>(Sprite->GetAtlasGroup());
    TArray<TSharedPtr<FJsonValue>> Group;
    TArray<TSharedPtr<FJsonObject>> GroupSources;
    for (const auto& Value : Properties) {
      if (!Value.IsValid() || Value->Type != EJson::Object) continue;
      auto Descriptor = UeAgentAuthoringObjectApply::ProjectDescriptorForApply(Value->AsObject());
      FString Path;
      if (Descriptor.IsValid() && Descriptor->TryGetStringField(TEXT("property_path"), Path)
          && Path == TEXT("AtlasGroup")) {
        Group.Add(MakeShared<FJsonValueObject>(Descriptor));
        GroupSources.Add(Value->AsObject());
      }
    }
    UeAgentAuthoringObjectApply::FOptions Options;
    Options.bAllowDomainAuthorizedOwners = true;
    Options.bCompactValues = true;
    Options.bEmitEditorNotifications = false;
    Options.bMarkPackageDirty = false;
    UeAgentAuthoringObjectApply::FPreparedApply Prepared;
    UeAgentAuthoringObjectApply::FFailure Failure;
    UeAgentAuthoringObjectApply::FApplyResult Result;
    TStrongObjectPtr<UPaperSprite> Intent(CastChecked<UPaperSprite>(StaticDuplicateObject(Sprite, GetTransientPackage())));
    // A transient outer does not remove the source asset's standalone flag.
    // Retire this intent-only copy, including its references to baked atlas pages.
    Intent->ClearFlags(RF_Public | RF_Standalone);
    Options.DiagnosticTargetPath = TargetPath;
    Options.SupplementDiagnosticSource = [GroupSources = MoveTemp(GroupSources), Root = Intent.Get()](
        UE::UAI::Contract::FDiagnostic& Diagnostic, const FJsonObject&, const UStruct*, const void* RootData, int32 Index) {
      if (RootData == Root && GroupSources.IsValidIndex(Index))
        UeAgentAuthoringDiagnostics::LocatePropertyValueSource(Diagnostic, *GroupSources[Index]);
    };
    ON_SCOPE_EXIT { Intent->MarkAsGarbage(); Intent.Reset(); };
    if (!Group.IsEmpty() && (!UeAgentAuthoringObjectApply::PrepareObject(Intent.Get(), Group, Options, Prepared, Failure)
        || !UeAgentAuthoringObjectApply::ApplyPrepared(Prepared, Result))) {
      Error = Failure.Diagnostic.IsEmpty() ? Result.Diagnostic : Failure.Diagnostic;
      ProfileState::BindTarget(Error, Intent->GetPathName(), TargetPath);
      return false;
    }
    Impl->Add(const_cast<UPaperSpriteAtlas*>(Intent->GetAtlasGroup()));
    Impl->Add((Sprite->*FSpriteMembers::Previous()).LoadSynchronous());
  }
  Impl->Expand();
  const FString Prefix = TEXT("/Temp/UAI_AtlasPreview_") + FGuid::NewGuid().ToString(EGuidFormats::Digits);
  // Preserve relative paths: the native packer uses sprite paths as its tie-break.
  if (!Proposed.Rename(*FPackageName::ObjectPathToObjectName(TargetPath),
      CreatePackage(*(Prefix + FPackageName::ObjectPathToPackageName(TargetPath))),
      REN_DontCreateRedirectors | REN_NonTransactional)) {
    Error = NativeFailure(TEXT("paper_atlas_preview_path_failed"),
        TEXT("The atlas preview object could not be moved to its canonical relative preview path."),
        TargetPath, TEXT("preview_rename"), MakeShared<FJsonValueBoolean>(false), MakeShared<FJsonValueBoolean>(true));
    Error.InternalContext = Proposed.GetPathName() + TEXT(" -> ") + Prefix + FPackageName::ObjectPathToPackageName(TargetPath);
    return false;
  }
  Impl->CanonicalPaths.Add(&Proposed, TargetPath);
  for (int32 Index = 0; Index < Impl->Actual.Num(); ++Index) {
    UObject* Original = Impl->Actual[Index].Get();
    UObject* Copy = Original == Existing ? &Proposed
        : StaticDuplicateObject(Original, CreatePackage(*(Prefix + Original->GetPackage()->GetName())), Original->GetFName());
    if (!Copy) {
      Error = NativeFailure(TEXT("paper_atlas_preview_duplicate_failed"),
          TEXT("An affected atlas or sprite asset could not be duplicated for preview."),
          Impl->Paths[Index], FString::Printf(TEXT("affected_assets[%d]"), Index),
          MakeShared<FJsonValueNull>(), MakeShared<FJsonValueString>(TEXT("a valid preview object")));
      Error.InternalContext = Prefix + Original->GetPackage()->GetName();
      return false;
    }
    Impl->Copies.Add(Original, Copy);
    Impl->CanonicalPaths.Add(Copy, Impl->Paths[Index]);
    Impl->Preview.Emplace(Copy);
    if (auto* Atlas = Cast<UPaperSpriteAtlas>(Original)) {
      auto* CopyAtlas = CastChecked<UPaperSpriteAtlas>(Copy);
      // Never let registry lookup find the real group's sprites during preview.
      CopyAtlas->AtlasGUID = FGuid::NewGuid();
      for (int32 Page = 0; Page < Atlas->GeneratedTextures.Num(); ++Page)
        if (Atlas->GeneratedTextures[Page] && CopyAtlas->GeneratedTextures.IsValidIndex(Page))
          Impl->Copies.Add(Atlas->GeneratedTextures[Page], CopyAtlas->GeneratedTextures[Page]);
    }
  }
  if (!Existing) Impl->Preview.Emplace(&Proposed);
  for (auto& Copy : Impl->Preview) {
    FArchiveReplaceObjectRef<UObject> Replace(Copy.Get(), Impl->Copies,
        EArchiveReplaceObjectFlags::IgnoreOuterRef | EArchiveReplaceObjectFlags::IgnoreArchetypeRef);
    if (auto* Atlas = Cast<UPaperSpriteAtlas>(Copy.Get()))
      for (auto& Slot : Atlas->AtlasSlots)
        if (UObject** Sprite = Impl->Copies.Find(Slot.SpriteRef.Get())) Slot.SpriteRef = CastChecked<UPaperSprite>(*Sprite);
    if (auto* Sprite = Cast<UPaperSprite>(Copy.Get())) {
      auto* Previous = (Sprite->*FSpriteMembers::Previous()).Get();
      if (!Previous) Previous = const_cast<UPaperSpriteAtlas*>(Sprite->GetAtlasGroup());
      if (UObject** Atlas = Impl->Copies.Find(Previous)) Previous = CastChecked<UPaperSpriteAtlas>(*Atlas);
      Sprite->*FSpriteMembers::Previous() = Previous;
      Sprite->ClearFlags(RF_Transient);
      Registry().AssetCreated(Sprite);
      Impl->Registered.Add(Sprite);
    }
  }
  // Ordinary property snapshots retain public asset reference identities.
  if (auto* Sprite = Cast<UPaperSprite>(&Proposed)) {
    for (const auto& Pair : Impl->Copies) {
      if (Sprite->GetAtlasGroup() == Pair.Value) Sprite->*FSpriteMembers::Atlas() = Cast<UPaperSpriteAtlas>(Pair.Key);
      if ((Sprite->*FSpriteMembers::Previous()).Get() == Pair.Value) Sprite->*FSpriteMembers::Previous() = Cast<UPaperSpriteAtlas>(Pair.Key);
    }
    // Only the native preview callback exposes this member to registry lookup.
    // Outside it the public AtlasGroup identity is restored for property reads.
    Sprite->SetFlags(RF_Transient);
  }
  return true;
}

bool FAtlasState::NotifyPreview(UObject& Object, FPropertyChangedEvent& Event) {
  auto* Sprite = Cast<UPaperSprite>(&Object);
  if (Sprite) {
    Sprite->ClearFlags(RF_Transient);
    if (UObject** Atlas = Impl->Copies.Find(Sprite->GetAtlasGroup())) Sprite->*FSpriteMembers::Atlas() = CastChecked<UPaperSpriteAtlas>(*Atlas);
    if (UObject** Atlas = Impl->Copies.Find((Sprite->*FSpriteMembers::Previous()).Get())) Sprite->*FSpriteMembers::Previous() = CastChecked<UPaperSpriteAtlas>(*Atlas);
  }
  Object.PostEditChangeProperty(Event);
  if (Sprite) {
    for (const auto& Pair : Impl->Copies) {
      if (Sprite->GetAtlasGroup() == Pair.Value) Sprite->*FSpriteMembers::Atlas() = Cast<UPaperSpriteAtlas>(Pair.Key);
      if ((Sprite->*FSpriteMembers::Previous()).Get() == Pair.Value) Sprite->*FSpriteMembers::Previous() = Cast<UPaperSpriteAtlas>(Pair.Key);
    }
    Sprite->SetFlags(RF_Transient);
  }
  return true;
}

void FAtlasState::BindActual(UObject& Object) { Impl->Add(&Object); }

void FAtlasState::CollectRollbackObjects(TArray<UObject*>& Objects) const {
  for (const auto& Object : Impl->Actual) Objects.AddUnique(Object.Get());
}
bool FAtlasState::AppendPackagePlans(TArray<UeAgentAuthoringOperationApply::FPackageDurablePlan>& Plans,
    UeAgentAuthoringOperationApply::FOperationFailure& Failure) const {
  using namespace UeAgentAuthoringOperationApply;
  for (const auto& Object : Impl->Actual) {
    const FString Package = Object->GetPackage()->GetName();
    if (!Plans.ContainsByPredicate([&](const auto& Plan) { return Plan.PackageName == Package; })
        && !BuildPackageDurablePlan(Package, Package, Object.Get(), EPackageDurableIntent::Save, Plans, Failure)) return false;
  }
  for (auto& Plan : Plans) Plan.ReadbackPolicy = EPackageReadbackPolicy::ReplaceResidentPackage;
  return true;
}

TSharedPtr<FJsonValue> FAtlasState::Capture(bool bExpected) const {
  auto Result = MakeShared<FJsonObject>();
  const auto& Objects = bExpected ? Impl->Preview : Impl->Actual;
  for (const auto& Object : Objects) {
    auto Item = MakeShared<FJsonObject>();
    if (const auto* Atlas = Cast<UPaperSpriteAtlas>(Object.Get())) {
      TArray<TSharedPtr<FJsonValue>> Slots;
      for (const auto& Slot : Atlas->AtlasSlots) {
        auto Entry = MakeShared<FJsonObject>();
        Entry->SetStringField(TEXT("sprite"), Impl->Path(Slot.SpriteRef.Get()));
        Entry->SetNumberField(TEXT("page"), Slot.AtlasIndex);
        Entry->SetNumberField(TEXT("x"), Slot.X); Entry->SetNumberField(TEXT("y"), Slot.Y);
        Entry->SetNumberField(TEXT("width"), Slot.Width); Entry->SetNumberField(TEXT("height"), Slot.Height);
        Slots.Add(MakeShared<FJsonValueObject>(Entry));
      }
      Item->SetArrayField(TEXT("slots"), Slots);
      TArray<TSharedPtr<FJsonValue>> Pages;
      for (const auto& Page : Atlas->GeneratedTextures) {
        auto Entry = MakeShared<FJsonObject>();
        if (auto* Texture = Cast<UTexture2D>(Page)) {
          Entry->SetNumberField(TEXT("width"), Texture->Source.GetSizeX());
          Entry->SetNumberField(TEXT("height"), Texture->Source.GetSizeY());
          Entry->SetNumberField(TEXT("mips"), Texture->Source.GetNumMips());
          TArray64<uint8> Bytes;
          if (Texture->Source.GetMipData(Bytes, 0))
            Entry->SetNumberField(TEXT("pixels_crc"), FCrc::MemCrc32(Bytes.GetData(), Bytes.Num()));
        }
        Pages.Add(MakeShared<FJsonValueObject>(Entry));
      }
      Item->SetArrayField(TEXT("pages"), Pages);
      Item->SetNumberField(TEXT("built_width"), Atlas->BuiltWidth);
      Item->SetNumberField(TEXT("built_height"), Atlas->BuiltHeight);
      Item->SetNumberField(TEXT("built_padding"), Atlas->BuiltPadding);
    } else if (const auto* Sprite = Cast<UPaperSprite>(Object.Get())) {
      Item->SetStringField(TEXT("atlas"), Impl->Path(Sprite->GetAtlasGroup()));
      const FVector2D UV = Sprite->*FSpriteMembers::BakedUV();
      const FVector2D Size = Sprite->*FSpriteMembers::BakedSize();
      Item->SetNumberField(TEXT("u"), UV.X); Item->SetNumberField(TEXT("v"), UV.Y);
      Item->SetNumberField(TEXT("width"), Size.X); Item->SetNumberField(TEXT("height"), Size.Y);
      int32 PageIndex = INDEX_NONE;
      auto* Group = Sprite->GetAtlasGroup();
      if (bExpected && Group)
        if (UObject* const* Copy = Impl->Copies.Find(Group)) Group = CastChecked<UPaperSpriteAtlas>(*Copy);
      if (Group) PageIndex = Group->GeneratedTextures.IndexOfByKey(Sprite->*FSpriteMembers::BakedTexture());
      Item->SetNumberField(TEXT("page"), PageIndex);
    } else continue;
    Result->SetObjectField(Impl->Path(Object.Get()), Item);
  }
  return MakeShared<FJsonValueObject>(Result);
}

void FAtlasState::Release() { Impl->ClearPreview(); Impl->Actual.Reset(); }
bool FAtlasState::Rebind(UE::UAI::Contract::FDiagnostic& Error, bool bTargetAbsent) {
  Impl->Actual.Reset();
  for (const FString& Path : Impl->Paths) {
    if (bTargetAbsent && Path == Impl->TargetPath) continue;
    UObject* Object = StaticLoadObject(UObject::StaticClass(), nullptr, *Path);
    if (!Object) {
      Error = NativeFailure(TEXT("paper_atlas_affected_reload_failed"),
          TEXT("An affected atlas or sprite asset could not be loaded while restoring the relationship state."),
          Path, TEXT("affected_asset"), MakeShared<FJsonValueNull>(),
          MakeShared<FJsonValueString>(TEXT("a loadable UObject at the target path")));
      return false;
    }
    Impl->Actual.Emplace(Object);
    if (auto* Sprite = Cast<UPaperSprite>(Object)) RefreshComponents(*Sprite);
  }
  return true;
}
}
