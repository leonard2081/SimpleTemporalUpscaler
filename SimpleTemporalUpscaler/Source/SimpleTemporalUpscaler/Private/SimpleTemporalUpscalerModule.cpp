#include "SimpleTemporalUpscalerModule.h"

#include "Misc/CoreDelegates.h"
#include "Engine/Engine.h"
#include "Interfaces/IPluginManager.h"
#include "Misc/Paths.h"
#include "SceneViewExtension.h"
#include "ShaderCore.h"
#include "SimpleTemporalUpscaler.h"

#define LOCTEXT_NAMESPACE "FSimpleTemporalUpscalerModule"

void FSimpleTemporalUpscalerModule::StartupModule()
{
	UE_LOG(LogTemp, Warning, TEXT("SimpleTemporalUpscaler StartupModule"));

	if (TSharedPtr<IPlugin> Plugin = IPluginManager::Get().FindPlugin(TEXT("SimpleTemporalUpscaler")))
	{
		const FString PluginShaderDir = FPaths::Combine(Plugin->GetBaseDir(), TEXT("Shaders"));
		AddShaderSourceDirectoryMapping(TEXT("/Plugin/Runtime/SimpleTemporalUpscaler"), PluginShaderDir);
	}

	if (GEngine)
	{
		RegisterViewExtension();
	}
	else
	{
		PostEngineInitHandle = FCoreDelegates::OnPostEngineInit.AddRaw(this, &FSimpleTemporalUpscalerModule::RegisterViewExtension);
	}
}

void FSimpleTemporalUpscalerModule::ShutdownModule()
{
	if (PostEngineInitHandle.IsValid())
	{
		FCoreDelegates::OnPostEngineInit.Remove(PostEngineInitHandle);
		PostEngineInitHandle.Reset();
	}

	ViewExtension.Reset();
}

void FSimpleTemporalUpscalerModule::RegisterViewExtension()
{
	if (ViewExtension.IsValid())
	{
		return;
	}

	if (!ensure(GEngine))
	{
		return;
	}

	UE_LOG(LogTemp, Warning, TEXT("SimpleTemporalUpscaler RegisterViewExtension"));
	ViewExtension = FSceneViewExtensions::NewExtension<FSimpleTemporalUpscalerViewExtension>();
}

#undef LOCTEXT_NAMESPACE

IMPLEMENT_MODULE(FSimpleTemporalUpscalerModule, SimpleTemporalUpscaler)
