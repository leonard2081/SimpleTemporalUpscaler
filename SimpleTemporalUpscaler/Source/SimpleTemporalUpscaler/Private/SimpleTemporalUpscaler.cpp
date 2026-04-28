#include "SimpleTemporalUpscaler.h"

#include "RenderGraphBuilder.h"
#include "RenderGraphUtils.h"
#include "RenderTargetPool.h"
#include "ScreenPass.h"
#include "SceneView.h"

class FSimpleTemporalUpscalerBlendCS : public FGlobalShader
{
public:
	DECLARE_GLOBAL_SHADER(FSimpleTemporalUpscalerBlendCS);
	SHADER_USE_PARAMETER_STRUCT(FSimpleTemporalUpscalerBlendCS, FGlobalShader);

	BEGIN_SHADER_PARAMETER_STRUCT(FParameters, )
		SHADER_PARAMETER_RDG_TEXTURE(Texture2D, CurrentSceneColorTexture)
		SHADER_PARAMETER_SAMPLER(SamplerState, CurrentSceneColorSampler)
		SHADER_PARAMETER_RDG_TEXTURE(Texture2D, PrevHistoryTexture)
		SHADER_PARAMETER_SAMPLER(SamplerState, PrevHistorySampler)
		SHADER_PARAMETER_RDG_TEXTURE_UAV(RWTexture2D<float4>, OutputTexture)
		SHADER_PARAMETER(FVector4f, CurrentViewRect)
		SHADER_PARAMETER(FVector4f, OutputViewRect)
		SHADER_PARAMETER(FVector2f, CurrentTextureExtentInverse)
		SHADER_PARAMETER(FVector2f, HistoryTextureExtentInverse)
		SHADER_PARAMETER(float, HistoryWeight)
		SHADER_PARAMETER(uint32, bHasHistory)
	END_SHADER_PARAMETER_STRUCT()
};

IMPLEMENT_GLOBAL_SHADER(FSimpleTemporalUpscalerBlendCS, "/Plugin/Runtime/SimpleTemporalUpscaler/Private/SimpleTemporalUpscalerBlend.usf", "MainCS", SF_Compute);

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

static TAutoConsoleVariable<float> CVarSimpleTemporalUpscalerHistoryWeight(
	TEXT("r.SimpleTemporalUpscaler.HistoryWeight"),
	0.2f,
	TEXT("History weight for the simple temporal upscaler fixed blend.\n")
	TEXT(" 0.0: current frame only;\n")
	TEXT(" 1.0: history only.\n"),
	ECVF_RenderThreadSafe);

static TAutoConsoleVariable<int32> CVarSimpleTemporalUpscalerLogStats(
	TEXT("r.SimpleTemporalUpscaler.LogStats"),
	0,
	TEXT("Logs the input/output resolution fraction and history state of SimpleTemporalUpscaler.\n")
	TEXT(" 0: disabled;\n")
	TEXT(" 1: enabled.\n"),
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
	UE::Renderer::Private::ITemporalUpscaler::FOutputs Outputs;

	const FIntPoint OutputExtent = Inputs.OutputViewRect.Size();
	check(OutputExtent.X > 0 && OutputExtent.Y > 0);

	FRDGTextureDesc OutputDesc = FRDGTextureDesc::Create2D(
		FIntPoint(Inputs.OutputViewRect.Max.X, Inputs.OutputViewRect.Max.Y),
		PF_FloatRGBA,
		FClearValueBinding::Black,
		TexCreate_ShaderResource | TexCreate_UAV);

	FRDGTextureRef OutputTexture = GraphBuilder.CreateTexture(OutputDesc, TEXT("SimpleTemporalUpscaler.Output"));

	FHistory* PrevHistory = static_cast<FHistory*>(Inputs.PrevHistory.GetReference());
	const bool bHasHistory =
		PrevHistory &&
		PrevHistory->ColorHistory.IsValid() &&
		PrevHistory->ViewRect == Inputs.OutputViewRect &&
		PrevHistory->Extent == FIntPoint(Inputs.OutputViewRect.Max.X, Inputs.OutputViewRect.Max.Y);

	FRDGTextureRef PrevHistoryTexture = nullptr;
	FIntPoint PrevHistoryExtent = FIntPoint::ZeroValue;
	if (bHasHistory)
	{
		PrevHistoryTexture = GraphBuilder.RegisterExternalTexture(PrevHistory->ColorHistory, TEXT("SimpleTemporalUpscaler.PrevHistory"));
		PrevHistoryExtent = PrevHistory->Extent;
	}

	const float HistoryWeight = FMath::Clamp(CVarSimpleTemporalUpscalerHistoryWeight.GetValueOnRenderThread(), 0.0f, 1.0f);
	if (CVarSimpleTemporalUpscalerLogStats.GetValueOnRenderThread() != 0)
	{
		const float InputFractionX = float(Inputs.SceneColor.ViewRect.Width()) / float(OutputExtent.X);
		const float InputFractionY = float(Inputs.SceneColor.ViewRect.Height()) / float(OutputExtent.Y);
		UE_LOG(LogTemp, Warning, TEXT("SimpleTemporalUpscaler Input=%dx%d Output=%dx%d Fraction=(%.3f, %.3f) Supported=(%.3f, %.3f) HasHistory=%d HistoryWeight=%.3f"),
			Inputs.SceneColor.ViewRect.Width(),
			Inputs.SceneColor.ViewRect.Height(),
			OutputExtent.X,
			OutputExtent.Y,
			InputFractionX,
			InputFractionY,
			GetMinUpsampleResolutionFraction(),
			GetMaxUpsampleResolutionFraction(),
			bHasHistory ? 1 : 0,
			HistoryWeight);
	}

	FSimpleTemporalUpscalerBlendCS::FParameters* Parameters = GraphBuilder.AllocParameters<FSimpleTemporalUpscalerBlendCS::FParameters>();
	Parameters->CurrentSceneColorTexture = Inputs.SceneColor.Texture;
	Parameters->CurrentSceneColorSampler = TStaticSamplerState<SF_Bilinear>::GetRHI();
	Parameters->PrevHistoryTexture = bHasHistory ? PrevHistoryTexture : Inputs.SceneColor.Texture;
	Parameters->PrevHistorySampler = TStaticSamplerState<SF_Bilinear>::GetRHI();
	Parameters->OutputTexture = GraphBuilder.CreateUAV(OutputTexture);
	Parameters->CurrentViewRect = FVector4f(
		Inputs.SceneColor.ViewRect.Min.X,
		Inputs.SceneColor.ViewRect.Min.Y,
		Inputs.SceneColor.ViewRect.Width(),
		Inputs.SceneColor.ViewRect.Height());
	Parameters->OutputViewRect = FVector4f(
		Inputs.OutputViewRect.Min.X,
		Inputs.OutputViewRect.Min.Y,
		Inputs.OutputViewRect.Width(),
		Inputs.OutputViewRect.Height());
	Parameters->CurrentTextureExtentInverse = FVector2f(
		1.0f / Inputs.SceneColor.Texture->Desc.Extent.X,
		1.0f / Inputs.SceneColor.Texture->Desc.Extent.Y);
	Parameters->HistoryTextureExtentInverse = bHasHistory
		? FVector2f(1.0f / PrevHistoryExtent.X, 1.0f / PrevHistoryExtent.Y)
		: FVector2f(1.0f / Inputs.SceneColor.Texture->Desc.Extent.X, 1.0f / Inputs.SceneColor.Texture->Desc.Extent.Y);
	Parameters->HistoryWeight = HistoryWeight;
	Parameters->bHasHistory = bHasHistory ? 1u : 0u;

	TShaderMapRef<FSimpleTemporalUpscalerBlendCS> ComputeShader(GetGlobalShaderMap(View.GetFeatureLevel()));
	FComputeShaderUtils::AddPass(
		GraphBuilder,
		RDG_EVENT_NAME("SimpleTemporalUpscaler::Blend"),
		ComputeShader,
		Parameters,
		FComputeShaderUtils::GetGroupCount(OutputExtent, FIntPoint(8, 8)));

	FHistory* NewHistory = new FHistory();
	NewHistory->ViewRect = Inputs.OutputViewRect;
	NewHistory->Extent = FIntPoint(Inputs.OutputViewRect.Max.X, Inputs.OutputViewRect.Max.Y);
	GraphBuilder.QueueTextureExtraction(OutputTexture, &NewHistory->ColorHistory);

	Outputs.FullRes = FScreenPassTexture(OutputTexture, Inputs.OutputViewRect);
	Outputs.NewHistory = NewHistory;
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
