#pragma once

#include "SceneViewExtension.h"
#include "TemporalUpscaler.h"
#include "RenderTargetPool.h"
#include "Templates/RefCounting.h"

class FSimpleTemporalUpscaler final : public UE::Renderer::Private::ITemporalUpscaler
{
public:
	class FHistory final : public UE::Renderer::Private::ITemporalUpscaler::IHistory, private FRefCountBase
	{
	public:
		TRefCountPtr<IPooledRenderTarget> ColorHistory;
		FIntRect ViewRect;
		FIntPoint Extent = FIntPoint::ZeroValue;

		virtual uint32 AddRef() const override;
		virtual uint32 Release() const override;
		virtual uint32 GetRefCount() const override;
		virtual const TCHAR* GetDebugName() const override;
		virtual uint64 GetGPUSizeBytes() const override;
	};

	virtual const TCHAR* GetDebugName() const override;

	virtual UE::Renderer::Private::ITemporalUpscaler::FOutputs AddPasses(
		FRDGBuilder& GraphBuilder,
		const FSceneView& View,
		const UE::Renderer::Private::ITemporalUpscaler::FInputs& Inputs) const override;

	virtual float GetMinUpsampleResolutionFraction() const override;
	virtual float GetMaxUpsampleResolutionFraction() const override;
	virtual UE::Renderer::Private::ITemporalUpscaler* Fork_GameThread(const FSceneViewFamily& ViewFamily) const override;
};

class FSimpleTemporalUpscalerViewExtension final : public FSceneViewExtensionBase
{
public:
	FSimpleTemporalUpscalerViewExtension(const FAutoRegister& AutoRegister);

	virtual void SetupViewFamily(FSceneViewFamily& InViewFamily) override;
	virtual void SetupView(FSceneViewFamily& InViewFamily, FSceneView& InView) override;
	virtual void BeginRenderViewFamily(FSceneViewFamily& InViewFamily) override;
	virtual int32 GetPriority() const override;

protected:
	virtual bool IsActiveThisFrame_Internal(const FSceneViewExtensionContext& Context) const override;
};
