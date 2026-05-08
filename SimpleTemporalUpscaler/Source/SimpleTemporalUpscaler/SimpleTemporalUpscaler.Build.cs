using UnrealBuildTool;

public class SimpleTemporalUpscaler : ModuleRules
{
	public SimpleTemporalUpscaler(ReadOnlyTargetRules Target) : base(Target)
	{
		PCHUsage = ModuleRules.PCHUsageMode.UseExplicitOrSharedPCHs;

		PublicDependencyModuleNames.AddRange(
			new string[]
			{
				"Core",
				"Engine",
				"Renderer",
				"RenderCore",
				"RHI"
			});

		PrivateDependencyModuleNames.AddRange(
			new string[]
			{
				"Projects"
			});
	}
}
