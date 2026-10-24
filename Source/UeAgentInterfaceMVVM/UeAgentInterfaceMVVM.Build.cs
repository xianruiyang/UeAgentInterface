using UnrealBuildTool;

public class UeAgentInterfaceMVVM : ModuleRules
{
	public UeAgentInterfaceMVVM(ReadOnlyTargetRules Target) : base(Target)
	{
		PCHUsage = PCHUsageMode.UseExplicitOrSharedPCHs;
		PublicDependencyModuleNames.AddRange(new[] { "Core" });
		PrivateDependencyModuleNames.AddRange(new[] { "CoreUObject", "Engine", "UnrealEd", "BlueprintGraph", "UMG", "UMGEditor", "ModelViewViewModelBlueprint" });
	}
}
