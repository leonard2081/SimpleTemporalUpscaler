#include "SimpleTemporalUpscalerModule.h"

#include "Misc/CoreDelegates.h"
#include "SceneViewExtension.h"
#include "SimpleTemporalUpscaler.h"

#define LOCTEXT_NAMESPACE "FSimpleTemporalUpscalerModule"

void FSimpleTemporalUpscalerModule::StartupModule()
{
	UE_LOG(LogTemp, Warning, TEXT("SimpleTemporalUpscaler StartupModule"));

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
