// Copyright Epic Games, Inc. All Rights Reserved.

using System.IO;
using UnrealBuildTool;

public class UeAgentInterfaceTests : ModuleRules
{
	public UeAgentInterfaceTests(ReadOnlyTargetRules Target) : base(Target)
	{
		// Production composition tests stay in family-specific source files.
		// Domain behavior and native scenario tests are discovered as separate sources.
		PCHUsage = PCHUsageMode.UseExplicitOrSharedPCHs;
		bUseUnity = false;

		PrivateIncludePaths.AddRange(
			new string[]
			{
				Path.Combine(ModuleDirectory, "..", "UeAgentInterface", "Private"),
			}
		);

		PrivateDependencyModuleNames.AddRange(
			new string[]
			{
				"UeAgentInterface",
				"Core",
				"CoreUObject",
				"Engine",
				"ImageCore",
				"UnrealEd",
				"PropertyEditor",
				"ClassViewer",
				"RenderCore",
				"RHI",
				"ApplicationCore",
				"Slate",
				"SlateCore",
				"HTTP",
				"Json",
				"JsonUtilities",
				"Localization",
				"AssetRegistry",
				"AssetTools",
				"DataValidation",
				"Kismet",
				"KismetCompiler",
				"BlueprintGraph",
				"GraphEditor",
				"BlueprintEditorLibrary",
				"AnimationBlueprintLibrary",
				"AnimationEditor",
				"AnimGraph",
				"Persona",
				"StaticMeshEditor",
				"TextureEditor",
				"UMG",
				"UMGEditor",
				"EnhancedInput",
				"InputBlueprintNodes",
				"InputCore",
				"GameplayTags",
				"AIModule",
				"AIGraph",
				"BehaviorTreeEditor",
				"EnvironmentQueryEditor",
				"GameplayTasks",
				"NavigationSystem",
				"StateTreeModule",
				"StateTreeEditorModule",
				"GameplayStateTreeModule",
				"GameplayInteractionsModule",
				"SmartObjectsModule",
				"SmartObjectsEditorModule",
				"WorldConditions",
				"PropertyBindingUtils",
				"PhysicsCore",
				"HairStrandsCore",
				"HairStrandsEditor",
				"Landscape",
				"Foliage",
				"MaterialEditor",
				"LevelSequence",
				"MovieScene",
				"MovieSceneTools",
				"MovieSceneTracks",
				"Sequencer",
				"SceneOutliner",
				"SequencerCore",
				"SequencerScriptingEditor",
				"ControlRig",
				"ControlRigDeveloper",
				"ControlRigEditor",
				"IKRig",
				"IKRigEditor",
				"RigVM",
				"RigVMDeveloper",
				"RigVMEditor",
				"Niagara",
				"NiagaraEditor",
				"PCG",
				"PCGEditor",
				"GeometryCache",
				"ComputeFramework",
				"OptimusCore",
				"InteractiveToolsFramework",
				"EditorInteractiveToolsFramework",
				"ModelingToolsEditorMode",
				"MeshModelingTools",
				"ModelingComponents",
				"ModelingComponentsEditorOnly",
				"GeometryFramework",
				"GeometryCore",
				"MeshConversion",
				"MeshDescription",
				"StaticMeshDescription",
				"MediaAssets",
				"Media",
				"Paper2D",
				"TextureGraph",
				"TextureGraphEngine",
				"AudioEditor",
				"AudioMixer",
				"MetasoundEngine",
				"MetasoundEditor",
				"MetasoundFrontend",
				"DataLayerEditor",
				"WorldPartitionEditor",
				"TargetPlatform",
			}
		);

		AddEngineThirdPartyPrivateStaticDependencies(Target, "OpenSSL");
	}
}
