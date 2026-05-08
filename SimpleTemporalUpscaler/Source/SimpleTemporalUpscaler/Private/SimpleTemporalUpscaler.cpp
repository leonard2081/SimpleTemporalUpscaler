#include "SimpleTemporalUpscaler.h"

#include "ScreenPass.h"
#include "SceneView.h"

namespace
{
const TCHAR* GSimpleTemporalUpscalerDebugName = TEXT("SimpleTemporalUpscaler");

static TAutoConsoleVariable<int32> CVarSimpleTemporalUpscalerEnable(
	TEXT("r.SimpleTemporalUpscaler.Enable"),
	0,
	TEXT("Enables the sample third-party temporal upscaler skeleton.\n")
	TEXT(" 0: disabled;\n")
	TEXT(" 1: enabled;\n"),
	ECVF_RenderThreadSafe);
}

uint32 FSimpleTemporalUpscaler::FHistory::AddRef() const
{
	return FRefCountBase::AddRef();
}

uint32 FSimpleTemporalUpscaler::FHistory::Release() const
{
	return FRefCountBase::Release();
}

uint32 FSimpleTemporalUpscaler::FHistory::GetRefCount() const
{
	return FRefCountBase::GetRefCount();
}

const TCHAR* FSimpleTemporalUpscaler::FHistory::GetDebugName() const
{
	return GSimpleTemporalUpscalerDebugName;
}

uint64 FSimpleTemporalUpscaler::FHistory::GetGPUSizeBytes() const
{
	return 0;
}

const TCHAR* FSimpleTemporalUpscaler::GetDebugName() const
{
	return GSimpleTemporalUpscalerDebugName;
}

UE::Renderer::Private::ITemporalUpscaler::FOutputs FSimpleTemporalUpscaler::AddPasses(
	FRDGBuilder& GraphBuilder,
	const FSceneView& View,
	const UE::Renderer::Private::ITemporalUpscaler::FInputs& Inputs) const
{
	UE_LOG(LogTemp, Log, TEXT("SimpleTemporalUpscaler chuxile +++++++++++++++++++++++++++++++++++++++++++++++++++++++"));
	UE::Renderer::Private::ITemporalUpscaler::FOutputs Outputs;
	Outputs.FullRes = Inputs.SceneColor;
	Outputs.FullRes.ViewRect = Inputs.OutputViewRect;
	Outputs.NewHistory = new FHistory();
	return Outputs;
}

float FSimpleTemporalUpscaler::GetMinUpsampleResolutionFraction() const
{
	return 0.5f;
}

float FSimpleTemporalUpscaler::GetMaxUpsampleResolutionFraction() const
{
	return 1.0f;
}

UE::Renderer::Private::ITemporalUpscaler* FSimpleTemporalUpscaler::Fork_GameThread(const FSceneViewFamily& ViewFamily) const
{
	return new FSimpleTemporalUpscaler();
}

FSimpleTemporalUpscalerViewExtension::FSimpleTemporalUpscalerViewExtension(const FAutoRegister& AutoRegister)
	: FSceneViewExtensionBase(AutoRegister)
{
}

void FSimpleTemporalUpscalerViewExtension::SetupViewFamily(FSceneViewFamily& InViewFamily)
{
}

void FSimpleTemporalUpscalerViewExtension::SetupView(FSceneViewFamily& InViewFamily, FSceneView& InView)
{
}

void FSimpleTemporalUpscalerViewExtension::BeginRenderViewFamily(FSceneViewFamily& InViewFamily)
{
	UE_LOG(LogTemp, Warning, TEXT("SimpleTemporalUpscaler BeginRenderViewFamily Enable=%d Existing=%p"),
		CVarSimpleTemporalUpscalerEnable.GetValueOnGameThread(),
		InViewFamily.GetTemporalUpscalerInterface());
	if (CVarSimpleTemporalUpscalerEnable.GetValueOnGameThread() == 0)
	{
		return;
	}

	if (InViewFamily.GetTemporalUpscalerInterface() != nullptr)
	{
		return;
	}

	InViewFamily.SetTemporalUpscalerInterface(new FSimpleTemporalUpscaler());
}

int32 FSimpleTemporalUpscalerViewExtension::GetPriority() const
{
	return 0;
}

bool FSimpleTemporalUpscalerViewExtension::IsActiveThisFrame_Internal(const FSceneViewExtensionContext& Context) const
{
	return CVarSimpleTemporalUpscalerEnable.GetValueOnAnyThread() != 0;
}
