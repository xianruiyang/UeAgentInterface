#include "Misc/AutomationTest.h"
#include "Engine/Texture2D.h"
#include "Engine/TextureCube.h"
#include "AssetCompilingManager.h"
#include "RenderingThread.h"
#include "UObject/StrongObjectPtr.h"
#include "DomainAdapters/AssetDataSimpleEffect/UeAgentAssetDataSimpleEffectTextureSourceMechanics.h"
#include "DomainAdapters/AssetDataSimpleEffect/UeAgentAssetDataSimpleEffectTextureSourceProjection.h"

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FUeAgentT208TextureCubeSourceAssembly,
    "UAI.Root.T208.TextureCube.SourceAssembly",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FUeAgentT208TextureCubeSourceAssembly::RunTest(const FString& Parameters)
{
    using namespace UE::UAI::DomainAdapters::AssetDataSimpleEffect;
    using namespace UE::UAI::ContextResource;
    TStrongObjectPtr<UTextureCube> Target(NewObject<UTextureCube>());
    Target->MipGenSettings = TMGS_NoMipmaps;
    TArray<TStrongObjectPtr<UTexture2D>> Owners;
    TArray<UTexture2D*> Sources;
    TArray64<uint8> Expected;
    for (int32 Face = 0; Face < 6; ++Face)
    {
        UTexture2D* Source = NewObject<UTexture2D>();
        Owners.Emplace(Source);
        Sources.Add(Source);
        TArray<FColor> Pixels;
        for (int32 Pixel = 0; Pixel < 16 * 16; ++Pixel)
            Pixels.Add(FColor(Face * 30 + Pixel % 16, Pixel / 16, 255 - Face * 20, 255));
        Source->Source.Init(16, 16, 1, 1, TSF_BGRA8, reinterpret_cast<const uint8*>(Pixels.GetData()));
        Expected.Append(reinterpret_cast<const uint8*>(Pixels.GetData()), Pixels.Num() * sizeof(FColor));
    }
    FTextureSourcePreparedState Prepared;
    UE::UAI::ExecutionMutation::FExecutionMutationError Error;
    if (!TestTrue(TEXT("resolved native preparation"), TryPrepareResolvedTextureSourceAssembly(*Target, Sources, Prepared, Error))) return false;

    // Preparing captures bytes, not a promise to reread mutable source assets at apply.
    TArray<FColor> Changed; Changed.Init(FColor::Black, 16 * 16);
    Sources[0]->Source.Init(16, 16, 1, 1, TSF_BGRA8, reinterpret_cast<const uint8*>(Changed.GetData()));
    Target->SRGB = false;
    Target->CompressionSettings = TC_Masks;
    if (!TestTrue(TEXT("native assembly"), TryApplyTextureSourceReplacement(Target.Get(), Prepared, Error))) return false;
    FAssetCompilingManager::Get().FinishAllCompilation();
    FlushRenderingCommands();
    TArray64<uint8> Actual;
    TestTrue(TEXT("read generated source"), Target->Source.GetMipData(Actual, 0));
    TestTrue(TEXT("exact frozen face and pixel order"), Actual == Expected);
    TestEqual(TEXT("six source faces"), Target->Source.GetNumSlices(), 6);
    TestEqual(TEXT("source width"), Target->Source.GetSizeX(), int64(16));
    TestFalse(TEXT("current explicit sRGB retained"), Target->SRGB);
    TestEqual(TEXT("current explicit compression retained"), Target->CompressionSettings, TC_Masks);

    FUeAgentResourceTracker Resources;
    UE::UAI::StateSemantic::FUeAgentCanonicalValuePtr Before;
    if (!TestTrue(TEXT("read generated state without typed envelope"), TryReadAssembledTextureSourceState(*Target, Resources, Before, Error))) return false;
    TArray<UTexture2D*> InvalidSources = Sources;
    InvalidSources[2] = nullptr;
    FTextureSourcePreparedState Rejected;
    TestFalse(TEXT("reject incomplete input before mutation"), TryPrepareResolvedTextureSourceAssembly(*Target, InvalidSources, Rejected, Error));
    TestFalse(TEXT("no prepared output on rejection"), Rejected.Data.IsValid());
    UE::UAI::StateSemantic::FUeAgentCanonicalValuePtr After;
    TestTrue(TEXT("read after rejected input"), TryReadAssembledTextureSourceState(*Target, Resources, After, Error));
    if (!After.IsValid()) return false;
    UE::UAI::StateSemantic::FUeAgentSemanticComparison Comparison;
    UE::UAI::StateSemantic::FUeAgentStateSemanticError ComparisonError;
    if (!UE::UAI::StateSemantic::TryCompareValues(Before.ToSharedRef(), After.ToSharedRef(),
        TEXT("/source"), Resources, Comparison, ComparisonError))
    {
        AddError(ComparisonError.Code + TEXT(": ") + ComparisonError.Path);
        return false;
    }
    for (const auto& Difference : Comparison.GetDifferences())
        AddError(TEXT("generated source difference: ") + Difference.GetPath());
    TestTrue(TEXT("rejected input preserved generated state"), Comparison.IsEquivalent());
    return !HasAnyErrors();
}
