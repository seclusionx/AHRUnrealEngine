// @RyanTorant
#include "RendererPrivate.h"
#include "ScenePrivate.h"
#include "ApproximateHybridRaytracing.h"
#include "SceneUtils.h"
#include "SceneFilterRendering.h"
#include "AHR_Voxelization.h"
#include "math.h"
#include "random"

//#include "../Public/AHRGlobalSignal.h"

//std::atomic<unsigned int> AHRGlobalSignal_RebuildGrids;

//#include <mutex>
//#include <thread>
#include <vector>
using namespace std;

// Using a full screen quad at every stage instead of a cs as the targets are already setted for a quad. Also, not using groupshared memory.
template<int _dummy>
class AHRPassVS : public FGlobalShader
{
	DECLARE_SHADER_TYPE(AHRPassVS,Global);
public:

	static bool ShouldCache(EShaderPlatform Platform)
	{
		return IsFeatureLevelSupported(Platform, ERHIFeatureLevel::SM5);
	}

	AHRPassVS()	{}
	AHRPassVS(const ShaderMetaType::CompiledShaderInitializerType& Initializer):
		FGlobalShader(Initializer)
	{
	}

	void SetParameters(FRHICommandList& RHICmdList, const FViewInfo& View)
	{
		FGlobalShader::SetParameters(RHICmdList, GetVertexShader(),View);
	}

	// Begin FShader Interface
	virtual bool Serialize(FArchive& Ar)
	{
		bool bShaderHasOutdatedParameters = FGlobalShader::Serialize(Ar);
		return bShaderHasOutdatedParameters;
	}
	// End FShader Interface
private:
};


IMPLEMENT_SHADER_TYPE(template<>,AHRPassVS<0>,TEXT("AHRComposite"),TEXT("VS"),SF_Vertex);
IMPLEMENT_SHADER_TYPE(template<>,AHRPassVS<1>,TEXT("AHRTraceSPH"),TEXT("VS"),SF_Vertex);

FCriticalSection cs;

uint32 wang_hash(uint32 seed)
{
	seed = (seed ^ 61) ^ (seed >> 16);
	seed *= 9;
	seed = seed ^ (seed >> 4);
	seed *= 0x27d4eb2d;
	seed = seed ^ (seed >> 15);
	return seed;
}

template<typename T,typename R>
T randReal(T Min, T Max,R rng)
{
	uniform_real_distribution<T> dist(Min, Max);
	return dist(rng);
}

inline uint32 fceil(uint32 a,uint32 b)
{
	return (a + b - 1u)/b;
}

class AHRPerPixelTracingKernelGenerator : public FGlobalShader
{
	DECLARE_SHADER_TYPE(AHRPerPixelTracingKernelGenerator,Global);

public:

	static bool ShouldCache(EShaderPlatform Platform)
	{
		return RHISupportsComputeShaders(Platform);
	}

	static void ModifyCompilationEnvironment( EShaderPlatform Platform, FShaderCompilerEnvironment& OutEnvironment )
	{
		FGlobalShader::ModifyCompilationEnvironment( Platform, OutEnvironment );
	}

	/** Default constructor. */
	AHRPerPixelTracingKernelGenerator()
	{
	}

	/** Initialization constructor. */
	explicit AHRPerPixelTracingKernelGenerator( const ShaderMetaType::CompiledShaderInitializerType& Initializer )
		: FGlobalShader(Initializer)
	{
		kernelTex.Bind( Initializer.ParameterMap, TEXT("kernelTex") );
		RayIndex.Bind( Initializer.ParameterMap, TEXT("RayIndex") );
	}

	/** Serialization. */
	virtual bool Serialize( FArchive& Ar ) override
	{
		bool bShaderHasOutdatedParameters = FGlobalShader::Serialize( Ar );
		Ar << kernelTex;
		Ar << RayIndex;
		return bShaderHasOutdatedParameters;
	}

	/**
	 * Set parameters for this shader.
	 */
	
	void SetParameters(FRHICommandList& RHICmdList, FUnorderedAccessViewRHIParamRef kernelTexUAV,uint32 rayIndex)
	{
		FComputeShaderRHIParamRef ComputeShaderRHI = GetComputeShader();

		if ( kernelTex.IsBound() )
			RHICmdList.SetUAVParameter(ComputeShaderRHI, kernelTex.GetBaseIndex(),kernelTexUAV );

		SetShaderValue(RHICmdList, ComputeShaderRHI, RayIndex, rayIndex );
	}

	/**
	 * Unbinds any buffers that have been bound.
	 */
	void UnbindBuffers(FRHICommandList& RHICmdList)
	{
		FComputeShaderRHIParamRef ComputeShaderRHI = GetComputeShader();
		if ( kernelTex.IsBound() )
			RHICmdList.SetUAVParameter(ComputeShaderRHI, kernelTex.GetBaseIndex(), FUnorderedAccessViewRHIParamRef());
	}

private:
	FShaderResourceParameter kernelTex;
	FShaderParameter RayIndex;
};
IMPLEMENT_SHADER_TYPE(,AHRPerPixelTracingKernelGenerator,TEXT("AHRPerPixelKernelGenerator"),TEXT("tracingKernel"),SF_Compute);

template<int _dummy>
class AHRPerPixelInterpolationKernelGenerator : public FGlobalShader
{
	DECLARE_SHADER_TYPE(AHRPerPixelInterpolationKernelGenerator,Global);

public:

	static bool ShouldCache(EShaderPlatform Platform)
	{
		return RHISupportsComputeShaders(Platform);
	}

	static void ModifyCompilationEnvironment( EShaderPlatform Platform, FShaderCompilerEnvironment& OutEnvironment )
	{
		FGlobalShader::ModifyCompilationEnvironment( Platform, OutEnvironment );
	}

	/** Default constructor. */
	AHRPerPixelInterpolationKernelGenerator()
	{
	}

	/** Initialization constructor. */
	explicit AHRPerPixelInterpolationKernelGenerator( const ShaderMetaType::CompiledShaderInitializerType& Initializer )
		: FGlobalShader(Initializer)
	{
		kernelTex.Bind( Initializer.ParameterMap, TEXT("kernelTex") );
		traceKernelTex.Bind( Initializer.ParameterMap, TEXT("traceKernelTex") );
	}

	/** Serialization. */
	virtual bool Serialize( FArchive& Ar ) override
	{
		bool bShaderHasOutdatedParameters = FGlobalShader::Serialize( Ar );
		Ar << kernelTex;
		Ar << traceKernelTex;
		return bShaderHasOutdatedParameters;
	}

	/**
	 * Set parameters for this shader.
	 */
	
	void SetParameters(FRHICommandList& RHICmdList, FUnorderedAccessViewRHIParamRef kernelTexUAV,FTextureRHIParamRef traceKernelTexSRV)
	{
		FComputeShaderRHIParamRef ComputeShaderRHI = GetComputeShader();

		if ( kernelTex.IsBound() )
			RHICmdList.SetUAVParameter(ComputeShaderRHI, kernelTex.GetBaseIndex(),kernelTexUAV );

		SetTextureParameter(RHICmdList, ComputeShaderRHI, traceKernelTex, traceKernelTexSRV);
	}

	/**
	 * Unbinds any buffers that have been bound.
	 */
	void UnbindBuffers(FRHICommandList& RHICmdList)
	{
		FComputeShaderRHIParamRef ComputeShaderRHI = GetComputeShader();
		if ( kernelTex.IsBound() )
			RHICmdList.SetUAVParameter(ComputeShaderRHI, kernelTex.GetBaseIndex(), FUnorderedAccessViewRHIParamRef());
	}

private:
	FShaderResourceParameter kernelTex;
	FShaderResourceParameter traceKernelTex;
};
IMPLEMENT_SHADER_TYPE(template<>,AHRPerPixelInterpolationKernelGenerator<0>,TEXT("AHRPerPixelKernelGenerator"),TEXT("interpKernel_H"),SF_Compute);
IMPLEMENT_SHADER_TYPE(template<>,AHRPerPixelInterpolationKernelGenerator<1>,TEXT("AHRPerPixelKernelGenerator"),TEXT("interpKernel_V"),SF_Compute);

void  FApproximateHybridRaytracer::StartFrame(FRHICommandListImmediate& RHICmdList,FViewInfo& View)
{	
	SCOPED_DRAW_EVENT(RHICmdList,AHRStartFrame);

	// Check if the bounds are valid
	if(View.FinalPostProcessSettings.AHR_internal_initialized)
	{
		gridSettings.Bounds = View.FinalPostProcessSettings.AHR_internal_SceneBounds;
		gridSettings.Center = View.FinalPostProcessSettings.AHR_internal_SceneOrigins;
		gridSettings.VoxelSize = View.FinalPostProcessSettings.AHRVoxelSize;

		gridSettings.SliceSize.X = ceil(gridSettings.Bounds.X / gridSettings.VoxelSize);
		gridSettings.SliceSize.Y = ceil(gridSettings.Bounds.Y / gridSettings.VoxelSize);
		gridSettings.SliceSize.Z = ceil(gridSettings.Bounds.Z / gridSettings.VoxelSize);

		if(gridSettings.SliceSize.X < 1)
			gridSettings.SliceSize.X = 1;
		else if(gridSettings.SliceSize.Y < 1)
			gridSettings.SliceSize.Y = 1;
		else if(gridSettings.SliceSize.Z < 1)
			gridSettings.SliceSize.Z = 1;
	
		uint32 maxVal = CVarAHRMaxSliceSize.GetValueOnRenderThread();
		maxVal = maxVal*maxVal*maxVal;

		if(uint32(gridSettings.SliceSize.X*gridSettings.SliceSize.Y*gridSettings.SliceSize.Z) > maxVal)
		{
			// Get the voxel size from the max
			gridSettings.VoxelSize = cbrt(gridSettings.Bounds.X*gridSettings.Bounds.Y*gridSettings.Bounds.Z/maxVal);

			gridSettings.SliceSize.X = ceil(gridSettings.Bounds.X / gridSettings.VoxelSize);
			gridSettings.SliceSize.Y = ceil(gridSettings.Bounds.Y / gridSettings.VoxelSize);
			gridSettings.SliceSize.Z = ceil(gridSettings.Bounds.Z / gridSettings.VoxelSize);
		}
	}

	// If the size of the shadow texture changed we need to rebuild the buffer
	{
		FScopeLock ScopeLock(&cs);
		auto shadowRes = GSceneRenderTargets.GetShadowDepthTextureResolution();

		if(shadowRes.X != prevShadowRes.X && shadowRes.Y != prevShadowRes.Y)
		{
			for(int i = 0;i < MAX_AHR_LIGHTS;i++)
			{
				FRHIResourceCreateInfo createInfo;
				lightDepths[i] = RHICreateTexture2D(shadowRes.X,shadowRes.Y,PF_ShadowDepth,1,1,TexCreate_ShaderResource,createInfo);
			}
		}

		prevShadowRes = shadowRes;
	}

	// If the screen res changed, rebuild kernels
	if(screenResChanged)
	{
		for(int n = 0;n < 5;n++)
		{
			// Build the tracing kernel
			TShaderMapRef<AHRPerPixelTracingKernelGenerator> tracingKernelGenCS(GetGlobalShaderMap(View.GetFeatureLevel()));
			RHICmdList.SetComputeShader(tracingKernelGenCS->GetComputeShader());
			tracingKernelGenCS->SetParameters(RHICmdList, GSceneRenderTargets.AHRPerPixelTracingKernel[n]->GetRenderTargetItem().UAV,n+1);

			auto size = GSceneRenderTargets.AHRPerPixelTracingKernel[n]->GetDesc().Extent;
			DispatchComputeShader(RHICmdList, *tracingKernelGenCS,fceil((uint32)size.X,16u), fceil((uint32)size.X,16u), 1);

			tracingKernelGenCS->UnbindBuffers(RHICmdList);

			// Blur the tracing kernel to use it to interpolate
			TShaderMapRef<AHRPerPixelInterpolationKernelGenerator<0>> interpolationKernelGenCS_H(GetGlobalShaderMap(View.GetFeatureLevel()));
			RHICmdList.SetComputeShader(interpolationKernelGenCS_H->GetComputeShader());
			interpolationKernelGenCS_H->SetParameters(RHICmdList, GSceneRenderTargets.AHRPerPixelInterpolationKernel_tmp->GetRenderTargetItem().UAV,GSceneRenderTargets.AHRPerPixelTracingKernel[n]->GetRenderTargetItem().ShaderResourceTexture);

			size = GSceneRenderTargets.AHRPerPixelInterpolationKernel_tmp->GetDesc().Extent;
			DispatchComputeShader(RHICmdList, *interpolationKernelGenCS_H,fceil((uint32)size.X,16u), fceil((uint32)size.Y,16u), 1);

			interpolationKernelGenCS_H->UnbindBuffers(RHICmdList);

			TShaderMapRef<AHRPerPixelInterpolationKernelGenerator<1>> interpolationKernelGenCS_V(GetGlobalShaderMap(View.GetFeatureLevel()));
			RHICmdList.SetComputeShader(interpolationKernelGenCS_V->GetComputeShader());
			interpolationKernelGenCS_V->SetParameters(RHICmdList, GSceneRenderTargets.AHRPerPixelInterpolationKernel[n]->GetRenderTargetItem().UAV,GSceneRenderTargets.AHRPerPixelInterpolationKernel_tmp->GetRenderTargetItem().ShaderResourceTexture);

			size = GSceneRenderTargets.AHRPerPixelInterpolationKernel[n]->GetDesc().Extent;
			DispatchComputeShader(RHICmdList, *interpolationKernelGenCS_V,fceil((uint32)size.X,16u), fceil((uint32)size.Y,16u), 1);

			interpolationKernelGenCS_V->UnbindBuffers(RHICmdList);
		}


		screenResChanged = false;
	}
}

class AHRDynamicStaticVolumeCombine : public FGlobalShader
{
	DECLARE_SHADER_TYPE(AHRDynamicStaticVolumeCombine,Global);

public:

	static bool ShouldCache(EShaderPlatform Platform)
	{
		return RHISupportsComputeShaders(Platform);
	}

	static void ModifyCompilationEnvironment( EShaderPlatform Platform, FShaderCompilerEnvironment& OutEnvironment )
	{
		FGlobalShader::ModifyCompilationEnvironment( Platform, OutEnvironment );
	}

	/** Default constructor. */
	AHRDynamicStaticVolumeCombine()
	{
	}

	/** Initialization constructor. */
	explicit AHRDynamicStaticVolumeCombine( const ShaderMetaType::CompiledShaderInitializerType& Initializer )
		: FGlobalShader(Initializer)
	{
		StaticVolume.Bind( Initializer.ParameterMap, TEXT("StaticVolume") );
		DynamicVolume.Bind( Initializer.ParameterMap, TEXT("DynamicVolume") );
		DynamicEmissiveVolume.Bind( Initializer.ParameterMap, TEXT("DynamicEmissiveVolume") );
		StaticEmissiveVolume.Bind( Initializer.ParameterMap, TEXT("StaticEmissiveVolume") );
		gridRes.Bind( Initializer.ParameterMap, TEXT("gridRes") );
	}

	/** Serialization. */
	virtual bool Serialize( FArchive& Ar ) override
	{
		bool bShaderHasOutdatedParameters = FGlobalShader::Serialize( Ar );
		Ar << StaticVolume;
		Ar << DynamicVolume;
		Ar << DynamicEmissiveVolume;
		Ar << StaticEmissiveVolume;
		Ar << gridRes;
		return bShaderHasOutdatedParameters;
	}

	/**
	 * Set parameters for this shader.
	 */
	
	void SetParameters(FRHICommandList& RHICmdList, FUnorderedAccessViewRHIParamRef DynamicVolumeUAV,FUnorderedAccessViewRHIParamRef emissiveUAV,
													FShaderResourceViewRHIParamRef StaticVolumeSRV,FShaderResourceViewRHIParamRef staticEmissiveSRV,
													FIntVector inGridRes
													)
	{
		FComputeShaderRHIParamRef ComputeShaderRHI = GetComputeShader();

		if ( DynamicVolume.IsBound() )
			RHICmdList.SetUAVParameter(ComputeShaderRHI, DynamicVolume.GetBaseIndex(), DynamicVolumeUAV);
		if ( StaticVolume.IsBound() )
			RHICmdList.SetShaderResourceViewParameter(ComputeShaderRHI,StaticVolume.GetBaseIndex(), StaticVolumeSRV);
		if ( DynamicEmissiveVolume.IsBound() )
			RHICmdList.SetUAVParameter(ComputeShaderRHI,DynamicEmissiveVolume.GetBaseIndex(), emissiveUAV);
		if ( StaticEmissiveVolume.IsBound() )
			RHICmdList.SetShaderResourceViewParameter(ComputeShaderRHI,StaticEmissiveVolume.GetBaseIndex(), staticEmissiveSRV);

		SetShaderValue(RHICmdList, ComputeShaderRHI, gridRes, inGridRes );
	}

	/**
	 * Unbinds any buffers that have been bound.
	 */
	void UnbindBuffers(FRHICommandList& RHICmdList)
	{
		FComputeShaderRHIParamRef ComputeShaderRHI = GetComputeShader();
		if ( DynamicVolume.IsBound() )
			RHICmdList.SetUAVParameter(ComputeShaderRHI, DynamicVolume.GetBaseIndex(), FUnorderedAccessViewRHIParamRef());
		if ( StaticVolume.IsBound() )
			RHICmdList.SetShaderResourceViewParameter(ComputeShaderRHI,StaticVolume.GetBaseIndex(), FShaderResourceViewRHIParamRef());
		if ( DynamicEmissiveVolume.IsBound() )
			RHICmdList.SetUAVParameter(ComputeShaderRHI,DynamicEmissiveVolume.GetBaseIndex(), FUnorderedAccessViewRHIParamRef());
		if ( StaticEmissiveVolume.IsBound() )
			RHICmdList.SetShaderResourceViewParameter(ComputeShaderRHI,StaticEmissiveVolume.GetBaseIndex(), FShaderResourceViewRHIParamRef());
	}

private:
	FShaderResourceParameter StaticVolume;
	FShaderResourceParameter DynamicVolume;
	FShaderResourceParameter DynamicEmissiveVolume;
	FShaderResourceParameter StaticEmissiveVolume;
	FShaderParameter gridRes;
};
// I know, this is ugly. I'm lazy. Sue me!
class AHRDynamicStaticEmissiveVolumeCombine : public FGlobalShader
{
	DECLARE_SHADER_TYPE(AHRDynamicStaticEmissiveVolumeCombine,Global);

public:

	static bool ShouldCache(EShaderPlatform Platform)
	{
		return RHISupportsComputeShaders(Platform);
	}

	static void ModifyCompilationEnvironment( EShaderPlatform Platform, FShaderCompilerEnvironment& OutEnvironment )
	{
		FGlobalShader::ModifyCompilationEnvironment( Platform, OutEnvironment );
	}

	/** Default constructor. */
	AHRDynamicStaticEmissiveVolumeCombine()
	{
	}

	/** Initialization constructor. */
	explicit AHRDynamicStaticEmissiveVolumeCombine( const ShaderMetaType::CompiledShaderInitializerType& Initializer )
		: FGlobalShader(Initializer)
	{
		StaticVolume.Bind( Initializer.ParameterMap, TEXT("StaticVolume") );
		DynamicVolume.Bind( Initializer.ParameterMap, TEXT("DynamicVolume") );
		DynamicEmissiveVolume.Bind( Initializer.ParameterMap, TEXT("DynamicEmissiveVolume") );
		StaticEmissiveVolume.Bind( Initializer.ParameterMap, TEXT("StaticEmissiveVolume") );
		gridRes.Bind( Initializer.ParameterMap, TEXT("gridRes") );
	}

	/** Serialization. */
	virtual bool Serialize( FArchive& Ar ) override
	{
		bool bShaderHasOutdatedParameters = FGlobalShader::Serialize( Ar );
		Ar << StaticVolume;
		Ar << DynamicVolume;
		Ar << DynamicEmissiveVolume;
		Ar << StaticEmissiveVolume;
		Ar << gridRes;
		return bShaderHasOutdatedParameters;
	}

	/**
	 * Set parameters for this shader.
	 */
	
	void SetParameters(FRHICommandList& RHICmdList, FUnorderedAccessViewRHIParamRef DynamicVolumeUAV,FUnorderedAccessViewRHIParamRef emissiveUAV,
													FShaderResourceViewRHIParamRef StaticVolumeSRV,FShaderResourceViewRHIParamRef staticEmissiveSRV,
													FIntVector inGridRes)
	{
		FComputeShaderRHIParamRef ComputeShaderRHI = GetComputeShader();

		if ( DynamicVolume.IsBound() )
			RHICmdList.SetUAVParameter(ComputeShaderRHI, DynamicVolume.GetBaseIndex(), DynamicVolumeUAV);
		if ( StaticVolume.IsBound() )
			RHICmdList.SetShaderResourceViewParameter(ComputeShaderRHI,StaticVolume.GetBaseIndex(), StaticVolumeSRV);
		if ( DynamicEmissiveVolume.IsBound() )
			RHICmdList.SetUAVParameter(ComputeShaderRHI,DynamicEmissiveVolume.GetBaseIndex(), emissiveUAV);
		if ( StaticEmissiveVolume.IsBound() )
			RHICmdList.SetShaderResourceViewParameter(ComputeShaderRHI,StaticEmissiveVolume.GetBaseIndex(), staticEmissiveSRV);

		SetShaderValue(RHICmdList, ComputeShaderRHI, gridRes, inGridRes );
	}

	/**
	 * Unbinds any buffers that have been bound.
	 */
	void UnbindBuffers(FRHICommandList& RHICmdList)
	{
		FComputeShaderRHIParamRef ComputeShaderRHI = GetComputeShader();

		if ( DynamicVolume.IsBound() )
			RHICmdList.SetUAVParameter(ComputeShaderRHI, DynamicVolume.GetBaseIndex(), FUnorderedAccessViewRHIParamRef());
		if ( StaticVolume.IsBound() )
			RHICmdList.SetShaderResourceViewParameter(ComputeShaderRHI,StaticVolume.GetBaseIndex(), FShaderResourceViewRHIParamRef());
		if ( DynamicEmissiveVolume.IsBound() )
			RHICmdList.SetUAVParameter(ComputeShaderRHI,DynamicEmissiveVolume.GetBaseIndex(), FUnorderedAccessViewRHIParamRef());
		if ( StaticEmissiveVolume.IsBound() )
			RHICmdList.SetShaderResourceViewParameter(ComputeShaderRHI,StaticEmissiveVolume.GetBaseIndex(), FShaderResourceViewRHIParamRef());
	}

private:
	FShaderResourceParameter StaticVolume;
	FShaderResourceParameter DynamicVolume;
	FShaderResourceParameter DynamicEmissiveVolume;
	FShaderResourceParameter StaticEmissiveVolume;
	FShaderParameter gridRes;
};

class AHREmissiveConvolution : public FGlobalShader
{
	DECLARE_SHADER_TYPE(AHREmissiveConvolution,Global);

public:

	static bool ShouldCache(EShaderPlatform Platform)
	{
		return RHISupportsComputeShaders(Platform);
	}

	static void ModifyCompilationEnvironment( EShaderPlatform Platform, FShaderCompilerEnvironment& OutEnvironment )
	{
		FGlobalShader::ModifyCompilationEnvironment( Platform, OutEnvironment );
	}

	/** Default constructor. */
	AHREmissiveConvolution()
	{
	}

	/** Initialization constructor. */
	explicit AHREmissiveConvolution( const ShaderMetaType::CompiledShaderInitializerType& Initializer )
		: FGlobalShader(Initializer)
	{
		SceneVolume.Bind( Initializer.ParameterMap, TEXT("SceneVolume") );
		EmissiveVolume.Bind( Initializer.ParameterMap, TEXT("EmissiveVolume") );
		gridRes.Bind( Initializer.ParameterMap, TEXT("gridRes") );
	}

	/** Serialization. */
	virtual bool Serialize( FArchive& Ar ) override
	{
		bool bShaderHasOutdatedParameters = FGlobalShader::Serialize( Ar );
		Ar << SceneVolume;
		Ar << EmissiveVolume;
		Ar << gridRes;
		return bShaderHasOutdatedParameters;
	}

	/**
	 * Set parameters for this shader.
	 */
	
	void SetParameters(FRHICommandList& RHICmdList, FUnorderedAccessViewRHIParamRef EmissiveVolumeUAV,
													FShaderResourceViewRHIParamRef SceneVolumeSRV,
													FIntVector inGridRes
													)
	{
		FComputeShaderRHIParamRef ComputeShaderRHI = GetComputeShader();

		if ( EmissiveVolume.IsBound() )
			RHICmdList.SetUAVParameter(ComputeShaderRHI, EmissiveVolume.GetBaseIndex(),EmissiveVolumeUAV );
		if ( SceneVolume.IsBound() )
			RHICmdList.SetShaderResourceViewParameter(ComputeShaderRHI,SceneVolume.GetBaseIndex(), SceneVolumeSRV);

		SetShaderValue(RHICmdList, ComputeShaderRHI, gridRes, inGridRes );
	}

	/**
	 * Unbinds any buffers that have been bound.
	 */
	void UnbindBuffers(FRHICommandList& RHICmdList)
	{
		FComputeShaderRHIParamRef ComputeShaderRHI = GetComputeShader();
		if ( EmissiveVolume.IsBound() )
			RHICmdList.SetUAVParameter(ComputeShaderRHI, EmissiveVolume.GetBaseIndex(), FUnorderedAccessViewRHIParamRef());
		if ( SceneVolume.IsBound() )
			RHICmdList.SetShaderResourceViewParameter(ComputeShaderRHI,SceneVolume.GetBaseIndex(), FShaderResourceViewRHIParamRef());
	}

private:
	FShaderResourceParameter SceneVolume;
	FShaderResourceParameter EmissiveVolume;
	FShaderParameter gridRes;
};



IMPLEMENT_SHADER_TYPE(,AHRDynamicStaticVolumeCombine,TEXT("AHRDynamicStaticVolumeCombine"),TEXT("mainBinary"),SF_Compute);
IMPLEMENT_SHADER_TYPE(,AHRDynamicStaticEmissiveVolumeCombine,TEXT("AHRDynamicStaticVolumeCombine"),TEXT("mainEmissive"),SF_Compute);
IMPLEMENT_SHADER_TYPE(,AHREmissiveConvolution,TEXT("AHREmissiveConvolution"),TEXT("main"),SF_Compute);


//std::mutex cs;
//pthread_mutex_t Mutex;
AHRGridSettings prevGridSettings;
vector<FName> prevStaticObjects;
//once_flag stdOnceFlag;
template<class Function>
void Once(Function&& f)
{
	static bool run = true;
	if(run)
		f();
	run = false;
	return;
}

void FApproximateHybridRaytracer::VoxelizeScene(FRHICommandListImmediate& RHICmdList,FViewInfo& View)
{
	SCOPED_DRAW_EVENT(RHICmdList,AHRVoxelizeScene);
#if 0
	/*static bool ran = false;
	if(ran) return;
	ran = true;
	if(View.PrimitivesToVoxelize.Num() == 0)
		return;*/
	
	bool RebuildGrids = View.FinalPostProcessSettings.AHRRebuildGrids;
	
	/*if(AHRGlobalSignal_RebuildGrids.load() == 1)
	{
		RebuildGrids = true;
		AHRGlobalSignal_RebuildGrids.store(0);
	}*/
	TArray<const FPrimitiveSceneInfo*,SceneRenderingAllocator> staticObjects;
	TArray<const FPrimitiveSceneInfo*,SceneRenderingAllocator> dynamicsObjects;
	bool voxelizeStatic = false;
	uint32 currentMatIDX = 0;
	//cs.lock();
	//pthread_mutex_lock(&Mutex);
	{
		// Make this thread-safe
		FScopeLock ScopeLock(&cs);
		// Could this be optimized? It feels wrong...
		bool staticListChanged = false;
		for(auto obj : View.PrimitivesToVoxelize)
		{
			if(obj->Proxy->NeedsEveryFrameVoxelization())
			{
				dynamicsObjects.Add(obj);
			}
			else
			{
				// Check if the object exists on the prev statics. O(x^2). Need to make this faster
				bool found = false;
				for(auto sObjName : prevStaticObjects)
					found |= sObjName == obj->Proxy->GetOwnerName();
				staticListChanged |= !found;
				staticObjects.Add(obj);
			}
		}
		// Voxelize static only once, to the static grid. Revoxelize static if something changed (add, delete, grid res changed)
		voxelizeStatic = prevGridSettings.Bounds != gridSettings.Bounds ||
						 prevGridSettings.Center != gridSettings.Center ||
						 prevGridSettings.SliceSize != gridSettings.SliceSize ||
						 prevStaticObjects.size() != staticObjects.Num() ||
					     staticListChanged ||
						 RebuildGrids;
		if(voxelizeStatic)
		{
			// we are going to voxelize, so store state
			prevStaticObjects.clear();
			for(auto obj : staticObjects)
				prevStaticObjects.push_back(obj->Proxy->GetOwnerName());
			prevGridSettings = gridSettings;
		}
		// Check if the palette changed
		/*
		bool paletteChanged = false;
		for(int i = 0; i < 256; i++) paletteChanged |= (prevPalette[i] != palette[i]);
		if(paletteChanged)
		{
			// TODO: a dynamic texture could be more efficient, specially if the material emissive color is driven trough blueprints/c++ code and changes often
			// Recreate the texture
			for(int i = 0; i < 256; i++) prevPalette[i] = palette[i];
			// Destroy the texture
			EmissivePaletteTexture.SafeRelease();
			EmissivePaletteSRV.SafeRelease();
			FRHIResourceCreateInfo CreateInfo;
			EmissivePaletteTexture = RHICreateTexture2D(256,1,PF_B8G8R8A8,1,1,TexCreate_ShaderResource,CreateInfo);
			// Copy palette
			uint8 palleteTexBuff[256*4];
			for(int i = 0; i < 256; i++)
			{
				auto col = palette[i].Quantize();
				palleteTexBuff[i*4]     =  col.B;
				palleteTexBuff[i*4 + 1] =  col.G;
				palleteTexBuff[i*4 + 2] =  col.R;
				palleteTexBuff[i*4 + 3] =  col.A; // Alpha is used as a multiplier
			}
			uint32 Stride = 0;
			uint8* textureData = (uint8*)RHILockTexture2D( EmissivePaletteTexture, 0, RLM_WriteOnly, Stride, false );
			FMemory::Memcpy(textureData, palleteTexBuff, 256*4);
			RHIUnlockTexture2D( EmissivePaletteTexture, 0, false );
			EmissivePaletteSRV = RHICreateShaderResourceView(EmissivePaletteTexture,0,1,PF_B8G8R8A8);
		}*/
	}
	//pthread_mutex_unlock(&Mutex);
	//cs.unlock();
	uint32 cls[4] = { 0,0,0,0 };
	// Voxelize only static
	if( staticObjects.Num( ) > 0  && voxelizeStatic)
	{
		// Clear
		RHICmdList.ClearUAV(StaticSceneVolume->UAV, cls);
		RHICmdList.ClearUAV(StaticEmissiveVolume->UAV, cls);
		SetStaticVolumeAsActive();
		TAHRVoxelizerElementPDI<FAHRVoxelizerDrawingPolicyFactory> Drawer(
			&View, FAHRVoxelizerDrawingPolicyFactory::ContextType(RHICmdList) );
		for( auto PrimitiveSceneInfo : staticObjects )
		{
			FScopeCycleCounter Context( PrimitiveSceneInfo->Proxy->GetStatId( ) );
			Drawer.SetPrimitive( PrimitiveSceneInfo->Proxy );
			
			// Calls SceneProxy DrawDynamicElements function
			PrimitiveSceneInfo->Proxy->DrawDynamicElements(&Drawer,&View,EDrawDynamicFlags::Voxelize);
		}
	}
	
	// Voxelize dynamic to the dynamic grid
	RHICmdList.ClearUAV(DynamicSceneVolume->UAV, cls);
	RHICmdList.ClearUAV(DynamicEmissiveVolume->UAV, cls);
	SetDynamicVolumeAsActive();
	TAHRVoxelizerElementPDI<FAHRVoxelizerDrawingPolicyFactory> Drawer(
			&View, FAHRVoxelizerDrawingPolicyFactory::ContextType(RHICmdList) );
	for( auto PrimitiveSceneInfo : dynamicsObjects )
	{
		FScopeCycleCounter Context( PrimitiveSceneInfo->Proxy->GetStatId( ) );
		Drawer.SetPrimitive( PrimitiveSceneInfo->Proxy );
		
		FAHRVoxelizerDrawingPolicyFactory::DrawDynamicMesh(RHICmdList, View, Context, *(PrimitiveSceneInfo->Proxy-> false, true, p.PrimitiveSceneProxy, MeshBatch.BatchHitProxyId);
		// Calls SceneProxy DrawDynamicElements function
		PrimitiveSceneInfo->Proxy->DrawDynamicElements(&Drawer,&View,EDrawDynamicFlags::Voxelize);
	}
	
	// Dispatch a compute shader that applies a per voxel pack or between the static grid and the dynamic grid.
	// The dynamic grid is the one that gets binded as an SRV
	TShaderMapRef<AHRDynamicStaticVolumeCombine> combineCS(GetGlobalShaderMap(View.GetFeatureLevel()));
	RHICmdList.SetComputeShader(combineCS->GetComputeShader());
	uint32 l = gridSettings.SliceSize.X*gridSettings.SliceSize.Y*gridSettings.SliceSize.Z/32;
	uint32 x = ceil(cbrt(l / 256.0f)); // cbrt = cubic root
	combineCS->SetParameters(RHICmdList, DynamicSceneVolume->UAV,DynamicEmissiveVolume->UAV,StaticSceneVolume->SRV,StaticEmissiveVolume->SRV,FIntVector(x*8,x*8,x*4));
	DispatchComputeShader(RHICmdList, *combineCS, x, x, x);
	combineCS->UnbindBuffers(RHICmdList);
	
	TShaderMapRef<AHRDynamicStaticEmissiveVolumeCombine> combineCSEmissive(GetGlobalShaderMap(View.GetFeatureLevel()));
	RHICmdList.SetComputeShader(combineCSEmissive->GetComputeShader());
	l = gridSettings.SliceSize.X*gridSettings.SliceSize.Y*gridSettings.SliceSize.Z/4;
	x = ceil(cbrt(l / 256.0f)); // cbrt = cubic root
	combineCSEmissive->SetParameters(RHICmdList, DynamicSceneVolume->UAV,DynamicEmissiveVolume->UAV,StaticSceneVolume->SRV,StaticEmissiveVolume->SRV,FIntVector(x*8,x*8,x*4));
	DispatchComputeShader(RHICmdList, *combineCSEmissive, x, x ,x);
	combineCSEmissive->UnbindBuffers(RHICmdList);
#endif
	uint32 cls[4] = { 0,0,0,0 };

	// Voxelize to the dynamic grid
	RHICmdList.ClearUAV(DynamicSceneVolume->UAV, cls);
	RHICmdList.ClearUAV(DynamicEmissiveVolume->UAV, cls);
	SetDynamicVolumeAsActive();

	for(auto e : View.PrimitivesElementsToVoxelize)
	{
		FAHRVoxelizerDrawingPolicyFactory::DrawDynamicMesh(RHICmdList, View, FAHRVoxelizerDrawingPolicyFactory::ContextType(), *e.Mesh, false, true, e.PrimitiveSceneProxy, e.Mesh->BatchHitProxyId);
	}

	// Blur the emissive grid to get multiple bounces
	/*
	TShaderMapRef<AHREmissiveConvolution> blurCS(GetGlobalShaderMap(View.GetFeatureLevel()));
	RHICmdList.SetComputeShader(blurCS->GetComputeShader());
	blurCS->SetParameters(RHICmdList, DynamicEmissiveVolume->UAV,DynamicSceneVolume->SRV,FIntVector(gridSettings.SliceSize.X/2,gridSettings.SliceSize.Y/2,gridSettings.SliceSize.Z/2));
	DispatchComputeShader(RHICmdList, *blurCS, gridSettings.SliceSize.X/8u, gridSettings.SliceSize.Y/8u, gridSettings.SliceSize.Z/4u);
	blurCS->UnbindBuffers(RHICmdList);*/

	// New frame, new starting idx
	currentLightIDX = 0;
}

///
/// Tracing
///
BEGIN_UNIFORM_BUFFER_STRUCT(AHRTraceSceneCB,)
	DECLARE_UNIFORM_BUFFER_STRUCT_MEMBER(FVector2D,ScreenRes)
	DECLARE_UNIFORM_BUFFER_STRUCT_MEMBER(FVector2D,SamplingKernelUVScaling)

	DECLARE_UNIFORM_BUFFER_STRUCT_MEMBER(FIntVector,SliceSize)
	DECLARE_UNIFORM_BUFFER_STRUCT_MEMBER(FVector,HalfInvSceneBounds)
	DECLARE_UNIFORM_BUFFER_STRUCT_MEMBER(FVector,WorldToVoxelOffset) // -SceneCenter/SceneBounds
	DECLARE_UNIFORM_BUFFER_STRUCT_MEMBER(FVector,invVoxel)
	DECLARE_UNIFORM_BUFFER_STRUCT_MEMBER(FVector,VoxelScaleMult)

	DECLARE_UNIFORM_BUFFER_STRUCT_MEMBER(float,InitialDispMult)
	DECLARE_UNIFORM_BUFFER_STRUCT_MEMBER(float,SamplesDispMultiplier)

	DECLARE_UNIFORM_BUFFER_STRUCT_MEMBER(uint32,GlossyRayCount)
	DECLARE_UNIFORM_BUFFER_STRUCT_MEMBER(uint32,GlossySamplesCount)
	DECLARE_UNIFORM_BUFFER_STRUCT_MEMBER(uint32,DiffuseRayCount)
	DECLARE_UNIFORM_BUFFER_STRUCT_MEMBER(uint32,DiffuseSamplesCount)

	DECLARE_UNIFORM_BUFFER_STRUCT_MEMBER(FVector,LostRayColor)

	DECLARE_UNIFORM_BUFFER_STRUCT_MEMBER(float,RayIndex)
END_UNIFORM_BUFFER_STRUCT(AHRTraceSceneCB)
IMPLEMENT_UNIFORM_BUFFER_STRUCT(AHRTraceSceneCB,TEXT("AHRTraceCB"));

template<int _dummy>
class AHRTraceScenePS : public FGlobalShader
{
	DECLARE_SHADER_TYPE(AHRTraceScenePS,Global)
public:

	static bool ShouldCache(EShaderPlatform Platform)
	{
		return IsFeatureLevelSupported(Platform, ERHIFeatureLevel::SM5);
	}

	static void ModifyCompilationEnvironment(EShaderPlatform Platform, FShaderCompilerEnvironment& OutEnvironment)
	{
		FGlobalShader::ModifyCompilationEnvironment(Platform, OutEnvironment);
		if(CVarAHRTraceReflections.GetValueOnRenderThread() == 1)
			OutEnvironment.SetDefine(TEXT("_GLOSSY"),1);
	}

	AHRTraceScenePS(const ShaderMetaType::CompiledShaderInitializerType& Initializer)
	:	FGlobalShader(Initializer)
	{
		DeferredParameters.Bind(Initializer.ParameterMap);
		SceneVolume.Bind(Initializer.ParameterMap, TEXT("SceneVolume"));
		LinearSampler.Bind(Initializer.ParameterMap, TEXT("samLinear"));
		cb.Bind(Initializer.ParameterMap, TEXT("AHRTraceCB"));

		EmissiveVolume.Bind(Initializer.ParameterMap, TEXT("EmissiveVolume"));

		SamplingKernel.Bind(Initializer.ParameterMap, TEXT("SamplingKernel"));
		samPoint.Bind(Initializer.ParameterMap, TEXT("samPoint"));

		ObjNormal.Bind(Initializer.ParameterMap, TEXT("ObjNormal"));
	}

	AHRTraceScenePS()
	{
	}

	void SetParameters(	FRHICommandList& RHICmdList, const FSceneView& View, 
						const FShaderResourceViewRHIRef& sceneVolumeSRV, 
						const FShaderResourceViewRHIRef& emissiveVolumeSRV )
	{
		FRHIResourceCreateInfo CreateInfo;

		const FPixelShaderRHIParamRef ShaderRHI = GetPixelShader();
		FGlobalShader::SetParameters(RHICmdList, ShaderRHI,View);
		DeferredParameters.Set(RHICmdList, ShaderRHI, View);

		if(SceneVolume.IsBound())
			RHICmdList.SetShaderResourceViewParameter(ShaderRHI,SceneVolume.GetBaseIndex(),sceneVolumeSRV);
		if(EmissiveVolume.IsBound())
			RHICmdList.SetShaderResourceViewParameter(ShaderRHI,EmissiveVolume.GetBaseIndex(),emissiveVolumeSRV);
		if(LinearSampler.IsBound())
			RHICmdList.SetShaderSampler(ShaderRHI,LinearSampler.GetBaseIndex(),TStaticSamplerState<SF_Trilinear,AM_Wrap,AM_Wrap,AM_Wrap>::GetRHI());
	
		if(ObjNormal.IsBound())
			RHICmdList.SetShaderResourceViewParameter(ShaderRHI,ObjNormal.GetBaseIndex(),AHREngine.ObjectNormalSRV);
	}

	void SetSamplingKernel(FRHICommandList& RHICmdList,const FTexture2DRHIRef& samplingKernelTex,const float& RayIndex,const FVector2D& ScreenRes, const FSceneView& View)
	{
		const FPixelShaderRHIParamRef ShaderRHI = GetPixelShader();

		auto spoint = TStaticSamplerState<SF_Point,AM_Wrap,AM_Wrap,AM_Wrap>::GetRHI();
		SetTextureParameter(RHICmdList, ShaderRHI, SamplingKernel, samPoint,spoint, samplingKernelTex);	

		if(samPoint.IsBound())
			RHICmdList.SetShaderSampler(ShaderRHI,LinearSampler.GetBaseIndex(),spoint);

		AHRTraceSceneCB cbdata;

		AHRGridSettings ahrGrid = AHREngine.GetGridSettings();

		
		cbdata.SamplingKernelUVScaling = ScreenRes / FVector2D(4.0f,4.0f);
		cbdata.SliceSize.X = ahrGrid.SliceSize.X;
		cbdata.SliceSize.Y = ahrGrid.SliceSize.Y;
		cbdata.SliceSize.Z = ahrGrid.SliceSize.Z;
		cbdata.ScreenRes = ScreenRes;
		cbdata.invVoxel = FVector(1.0f / float(cbdata.SliceSize.X),
								  1.0f / float(cbdata.SliceSize.Y),
								  1.0f / float(cbdata.SliceSize.Z));
		cbdata.VoxelScaleMult = cbdata.invVoxel*ahrGrid.Bounds;
		cbdata.HalfInvSceneBounds = FVector(0.5f) / ahrGrid.Bounds;
		cbdata.WorldToVoxelOffset = -ahrGrid.Center*cbdata.HalfInvSceneBounds + 0.5f; // -SceneCenter/SceneBounds
		cbdata.GlossyRayCount = View.FinalPostProcessSettings.AHRGlossyRayCount;
		cbdata.GlossySamplesCount = View.FinalPostProcessSettings.AHRGlossySamplesCount;
		cbdata.DiffuseRayCount = View.FinalPostProcessSettings.AHRDiffuseRayCount;
		cbdata.DiffuseSamplesCount = View.FinalPostProcessSettings.AHRDiffuseSamplesCount;
		cbdata.LostRayColor.X = View.FinalPostProcessSettings.AHRLostRayColor.R;
		cbdata.LostRayColor.Y = View.FinalPostProcessSettings.AHRLostRayColor.G;
		cbdata.LostRayColor.Z = View.FinalPostProcessSettings.AHRLostRayColor.B;
		cbdata.InitialDispMult = View.FinalPostProcessSettings.AHRInitialDisplacement;
		cbdata.SamplesDispMultiplier = View.FinalPostProcessSettings.AHRSamplesDisplacement;
		cbdata.RayIndex = RayIndex;
		SetUniformBufferParameterImmediate(RHICmdList, ShaderRHI,cb,cbdata);
	}
	virtual bool Serialize(FArchive& Ar)
	{		
		bool bShaderHasOutdatedParameters = FGlobalShader::Serialize(Ar);
		Ar << DeferredParameters;
		Ar << SceneVolume;
		Ar << LinearSampler;
		Ar << cb;
		Ar << EmissiveVolume;
		Ar << SamplingKernel;
		Ar << samPoint;

		Ar << ObjNormal;
		return bShaderHasOutdatedParameters;
	}

	FGlobalBoundShaderState& GetBoundShaderState()
	{
		static FGlobalBoundShaderState State;

		return State;
	}

private:
	FDeferredPixelShaderParameters DeferredParameters;
	FShaderResourceParameter SceneVolume;
	FShaderResourceParameter LinearSampler;
	TShaderUniformBufferParameter<AHRTraceSceneCB> cb;

	FShaderResourceParameter EmissiveVolume;

	FShaderResourceParameter SamplingKernel;
	FShaderResourceParameter samPoint;

	FShaderResourceParameter ObjNormal;
};
IMPLEMENT_SHADER_TYPE(template<>,AHRTraceScenePS<0>,TEXT("AHRTraceSPH"),TEXT("main"),SF_Pixel);
IMPLEMENT_SHADER_TYPE(template<>,AHRTraceScenePS<1>,TEXT("AHRTraceSPH"),TEXT("traceReflections"),SF_Pixel);

void FApproximateHybridRaytracer::TraceScene(FRHICommandListImmediate& RHICmdList,FViewInfo& View)
{
	SCOPED_DRAW_EVENT(RHICmdList,AHRTraceScene);
	RHICmdList.SetBlendState(TStaticBlendState<CW_RGBA>::GetRHI());

	// Draw a full screen quad into the half res target
	const auto& ___tmpTarget = GSceneRenderTargets.AHRRaytracingTarget[0]->GetRenderTargetItem().TargetableTexture->GetTexture2D();
	//FVector2D texSize(View.Family->FamilySizeX,View.Family->FamilySizeY);//(___tmpTarget->GetSizeX(),___tmpTarget->GetSizeY());
	//FIntRect SrcRect = View.ViewRect;
	//FIntRect DestRect(FIntPoint(0,0),FIntPoint(texSize.X/2,texSize.Y/2));
	
	FIntRect SrcRect = FIntRect(View.ViewRect.Min,FIntPoint(View.Family->FamilySizeX,View.Family->FamilySizeY))/2;//View.ViewRect/2;
	// Viewport size not even also causes issue
	FIntRect DestRect = FIntRect::DivideAndRoundUp(SrcRect, 2);

	RHICmdList.SetViewport(0, 0, 0.0f, DestRect.Max.X, DestRect.Max.Y, 1.0f );

	//RHICmdList.SetViewport(SrcRect.Min.X, SrcRect.Min.Y, 0.0f,texSize.X, texSize.Y, 1.0f);
	RHICmdList.SetRasterizerState(TStaticRasterizerState<FM_Solid, CM_None>::GetRHI());
	RHICmdList.SetDepthStencilState(TStaticDepthStencilState<false, CF_Always>::GetRHI());

	// Get the shaders
	TShaderMapRef<AHRPassVS<1>> VertexShader(View.ShaderMap);
	TShaderMapRef<AHRTraceScenePS<0>> PixelShader(View.ShaderMap);
	TShaderMapRef<AHRTraceScenePS<1>> PixelShaderRefl(View.ShaderMap);

	SetGlobalBoundShaderState(RHICmdList, View.FeatureLevel, PixelShader->GetBoundShaderState(),  GFilterVertexDeclaration.VertexDeclarationRHI, *VertexShader, *PixelShader);
	VertexShader->SetParameters(RHICmdList,View);

	// The dynamic grid should have both the static and dynamic data by now
	PixelShader->SetParameters(RHICmdList, View, 
									DynamicSceneVolume->SRV,
									DynamicEmissiveVolume->SRV );

	// Trace one ray per direction, hardcoding at 5 + reflection
	for(int i = 0;i < 5;i++)
	{
		SCOPED_DRAW_EVENT(RHICmdList,AHRTraceScene_diffuse);

		// Set the render target
		const auto& target = GSceneRenderTargets.AHRRaytracingTarget[i]->GetRenderTargetItem().TargetableTexture->GetTexture2D();
		SetRenderTarget(RHICmdList, target, FTextureRHIRef());
		
		// Clear the target before drawing
		RHICmdList.Clear(true, FLinearColor::Black, false, 1.0f, false, 0, FIntRect());

		// Bound shader parameters
		PixelShader->SetSamplingKernel(RHICmdList,GSceneRenderTargets.AHRPerPixelTracingKernel[i]->GetRenderTargetItem().ShaderResourceTexture->GetTexture2D(),i,FVector2D(DestRect.Max.X,DestRect.Max.Y),View);

		// Draw a quad mapping scene color to the view's render target
		DrawRectangle(
			RHICmdList,
			0.0f, 0.0f,
			DestRect.Width(), DestRect.Height(),
			DestRect.Min.X, DestRect.Min.Y, 
			SrcRect.Width(), SrcRect.Height(),
			DestRect.Size(),
			GSceneRenderTargets.GetBufferSizeXY(),
			*VertexShader,
			EDRF_UseTriangleOptimization);
	}

	{
		SCOPED_DRAW_EVENT(RHICmdList,AHRTraceScene_reflection);

		// Trace reflections
		SetGlobalBoundShaderState(RHICmdList, View.FeatureLevel, PixelShaderRefl->GetBoundShaderState(),  GFilterVertexDeclaration.VertexDeclarationRHI, *VertexShader, *PixelShaderRefl);
		// The dynamic grid should have both the static and dynamic data by now
		PixelShaderRefl->SetParameters(RHICmdList, View, 
										DynamicSceneVolume->SRV,
										DynamicEmissiveVolume->SRV );

		// Set the render target
		const auto& target = GSceneRenderTargets.AHRRaytracingTarget[5]->GetRenderTargetItem().TargetableTexture->GetTexture2D();
		SetRenderTarget(RHICmdList, target, FTextureRHIRef());
		
		// Clear the target before drawing
		RHICmdList.Clear(true, FLinearColor::Black, false, 1.0f, false, 0, FIntRect());

		// Bound shader parameters
		PixelShaderRefl->SetSamplingKernel(RHICmdList,FTexture2DRHIRef(),-1,FVector2D(DestRect.Max.X,DestRect.Max.Y),View);

		// Draw a quad mapping scene color to the view's render target
		DrawRectangle(
				RHICmdList,
				0.0f, 0.0f,
				DestRect.Width(), DestRect.Height(),
				DestRect.Min.X, DestRect.Min.Y, 
				SrcRect.Width(), SrcRect.Height(),
				DestRect.Size(),
				GSceneRenderTargets.GetBufferSizeXY(),
				*VertexShader,
				EDRF_UseTriangleOptimization);
	}
}


///
/// Upsampling and composite
///
class AHRBlur : public FGlobalShader
{
	DECLARE_SHADER_TYPE(AHRBlur,Global)
public:
	static bool ShouldCache(EShaderPlatform Platform)
	{
		return IsFeatureLevelSupported(Platform, ERHIFeatureLevel::SM5);
	}

	static void ModifyCompilationEnvironment(EShaderPlatform Platform, FShaderCompilerEnvironment& OutEnvironment)
	{
		FGlobalShader::ModifyCompilationEnvironment(Platform, OutEnvironment);
	}

	AHRBlur(const ShaderMetaType::CompiledShaderInitializerType& Initializer)
	:	FGlobalShader(Initializer)
	{
		DeferredParameters.Bind(Initializer.ParameterMap);
		samLinear.Bind(Initializer.ParameterMap, TEXT("samLinear"));
		ObjNormal.Bind(Initializer.ParameterMap,TEXT("ObjNormal"));
		Trace.Bind(Initializer.ParameterMap,TEXT("Trace"));
		prevTrace.Bind(Initializer.ParameterMap,TEXT("prevTrace"));
		BlurData.Bind(Initializer.ParameterMap,TEXT("BlurData"));
	}

	AHRBlur()
	{
	}

	virtual bool Serialize(FArchive& Ar)
	{
		bool bShaderHasOutdatedParameters = FGlobalShader::Serialize(Ar);
		Ar << DeferredParameters;
		Ar << samLinear;
		Ar << ObjNormal;
		Ar << Trace;
		Ar << prevTrace;
		Ar << BlurData;
		return bShaderHasOutdatedParameters;
	}
	void SetParameters(FRHICommandList& RHICmdList, const FSceneView& View, FRHITexture2D* TraceTex, FRHITexture2D* prevTraceTex, float blurXMask,float blurYMask)
	{
		const FPixelShaderRHIParamRef ShaderRHI = GetPixelShader();
		FGlobalShader::SetParameters(RHICmdList, ShaderRHI,View);
		DeferredParameters.Set(RHICmdList, ShaderRHI, View);

		auto sampler = TStaticSamplerState<SF_Trilinear,AM_Wrap,AM_Wrap,AM_Wrap>::GetRHI();
		if(ObjNormal.IsBound())
			RHICmdList.SetShaderResourceViewParameter(ShaderRHI,ObjNormal.GetBaseIndex(),AHREngine.ObjectNormalSRV);
		if(samLinear.IsBound())
			RHICmdList.SetShaderSampler(ShaderRHI,samLinear.GetBaseIndex(),sampler);

		SetTextureParameter(RHICmdList, ShaderRHI, Trace, samLinear,sampler, TraceTex);
		SetTextureParameter(RHICmdList, ShaderRHI, prevTrace, samLinear,sampler, prevTraceTex);	

		SetShaderValue(RHICmdList, ShaderRHI, BlurData, FVector4(1.0f / float(TraceTex->GetSizeX()),1.0f / float(TraceTex->GetSizeY()),
																 blurXMask,blurYMask));
	}

	FGlobalBoundShaderState& GetBoundShaderState()
	{
		static FGlobalBoundShaderState State;
		return State;
	}
private:
	FDeferredPixelShaderParameters DeferredParameters;
	FShaderResourceParameter samLinear;
	FShaderResourceParameter ObjNormal;
	FShaderResourceParameter Trace;
	FShaderResourceParameter prevTrace;
	FShaderParameter BlurData;
};
/*
class AHRBlurH : public FGlobalShader
{
	DECLARE_SHADER_TYPE(AHRBlurH,Global)
public:

	static bool ShouldCache(EShaderPlatform Platform)
	{
		return IsFeatureLevelSupported(Platform, ERHIFeatureLevel::SM5);
	}

	static void ModifyCompilationEnvironment(EShaderPlatform Platform, FShaderCompilerEnvironment& OutEnvironment)
	{
		FGlobalShader::ModifyCompilationEnvironment(Platform, OutEnvironment);
	}

	AHRBlurH(const ShaderMetaType::CompiledShaderInitializerType& Initializer)
	:	FGlobalShader(Initializer)
	{
		DeferredParameters.Bind(Initializer.ParameterMap);
		GIBufferTexture.Bind(Initializer.ParameterMap, TEXT("tGI"));
		LinearSampler.Bind(Initializer.ParameterMap, TEXT("samLinear"));
		BlurKernelSize.Bind(Initializer.ParameterMap,TEXT("size"));
		zMax.Bind(Initializer.ParameterMap,TEXT("zMax"));
		NormalTex.Bind(Initializer.ParameterMap,TEXT("NormalTex"));
	}

	AHRBlurH()
	{
	}

	void SetParameters(FRHICommandList& RHICmdList, const FSceneView& View, const FShaderResourceViewRHIRef giSRV,float blurKernelSize)
	{
		const FPixelShaderRHIParamRef ShaderRHI = GetPixelShader();
		FGlobalShader::SetParameters(RHICmdList, ShaderRHI,View);
		DeferredParameters.Set(RHICmdList, ShaderRHI, View);

		if(GIBufferTexture.IsBound())
			RHICmdList.SetShaderResourceViewParameter(ShaderRHI,GIBufferTexture.GetBaseIndex(),giSRV);
		if(NormalTex.IsBound())
			RHICmdList.SetShaderResourceViewParameter(ShaderRHI,NormalTex.GetBaseIndex(),AHREngine.ObjectNormalSRV);
		if(LinearSampler.IsBound())
			RHICmdList.SetShaderSampler(ShaderRHI,LinearSampler.GetBaseIndex(),TStaticSamplerState<SF_Trilinear,AM_Wrap,AM_Wrap,AM_Wrap>::GetRHI());

		SetShaderValue(RHICmdList, ShaderRHI, BlurKernelSize,blurKernelSize);
		SetShaderValue(RHICmdList, ShaderRHI, zMax,View.PixelToWorld(0,0,1).Z);
	}

	virtual bool Serialize(FArchive& Ar)
	{		
		bool bShaderHasOutdatedParameters = FGlobalShader::Serialize(Ar);
		Ar << DeferredParameters;
		Ar << GIBufferTexture;
		Ar << LinearSampler;
		Ar << BlurKernelSize;
		Ar << zMax;
		Ar << NormalTex;
		return bShaderHasOutdatedParameters;
	}

	FGlobalBoundShaderState& GetBoundShaderState()
	{
		static FGlobalBoundShaderState State;

		return State;
	}

private:
	FDeferredPixelShaderParameters DeferredParameters;
	FShaderResourceParameter GIBufferTexture;
	FShaderResourceParameter NormalTex;
	FShaderResourceParameter LinearSampler;
	FShaderParameter BlurKernelSize;
	FShaderParameter zMax;
};
class AHRBlurV : public FGlobalShader
{
	DECLARE_SHADER_TYPE(AHRBlurV,Global)
public:

	static bool ShouldCache(EShaderPlatform Platform)
	{
		return IsFeatureLevelSupported(Platform, ERHIFeatureLevel::SM5);
	}

	static void ModifyCompilationEnvironment(EShaderPlatform Platform, FShaderCompilerEnvironment& OutEnvironment)
	{
		FGlobalShader::ModifyCompilationEnvironment(Platform, OutEnvironment);
	}

	AHRBlurV(const ShaderMetaType::CompiledShaderInitializerType& Initializer)
	:	FGlobalShader(Initializer)
	{
		DeferredParameters.Bind(Initializer.ParameterMap);
		GIBufferTexture.Bind(Initializer.ParameterMap, TEXT("tGI"));
		LinearSampler.Bind(Initializer.ParameterMap, TEXT("samLinear"));
		BlurKernelSize.Bind(Initializer.ParameterMap,TEXT("size"));
		zMax.Bind(Initializer.ParameterMap,TEXT("zMax"));
		NormalTex.Bind(Initializer.ParameterMap,TEXT("NormalTex"));
	}

	AHRBlurV()
	{
	}

	void SetParameters(FRHICommandList& RHICmdList, const FSceneView& View, const FShaderResourceViewRHIRef giSRV,float blurKernelSize)
	{
		const FPixelShaderRHIParamRef ShaderRHI = GetPixelShader();
		FGlobalShader::SetParameters(RHICmdList, ShaderRHI,View);
		DeferredParameters.Set(RHICmdList, ShaderRHI, View);

		if(GIBufferTexture.IsBound())
			RHICmdList.SetShaderResourceViewParameter(ShaderRHI,GIBufferTexture.GetBaseIndex(),giSRV);
		if(NormalTex.IsBound())
			RHICmdList.SetShaderResourceViewParameter(ShaderRHI,NormalTex.GetBaseIndex(),AHREngine.ObjectNormalSRV);
		if(LinearSampler.IsBound())
			RHICmdList.SetShaderSampler(ShaderRHI,LinearSampler.GetBaseIndex(),TStaticSamplerState<SF_Trilinear,AM_Wrap,AM_Wrap,AM_Wrap>::GetRHI());

		SetShaderValue(RHICmdList, ShaderRHI, BlurKernelSize,blurKernelSize);
		SetShaderValue(RHICmdList, ShaderRHI, zMax,View.PixelToWorld(0,0,1).Z);
	}

	virtual bool Serialize(FArchive& Ar)
	{		
		bool bShaderHasOutdatedParameters = FGlobalShader::Serialize(Ar);
		Ar << DeferredParameters;
		Ar << GIBufferTexture;
		Ar << LinearSampler;
		Ar << BlurKernelSize;
		Ar << zMax;
		Ar << NormalTex;
		return bShaderHasOutdatedParameters;
	}

	FGlobalBoundShaderState& GetBoundShaderState()
	{
		static FGlobalBoundShaderState State;

		return State;
	}

private:
	FDeferredPixelShaderParameters DeferredParameters;
	FShaderResourceParameter GIBufferTexture;
	FShaderResourceParameter NormalTex;
	FShaderResourceParameter LinearSampler;
	FShaderParameter BlurKernelSize;
	FShaderParameter zMax;
};*/
//IMPLEMENT_SHADER_TYPE(,AHRUpsamplePS,TEXT("AHRUpsample"),TEXT("PS"),SF_Pixel);
//IMPLEMENT_SHADER_TYPE(,AHRBlurH,TEXT("AHRUpsample"),TEXT("BlurH"),SF_Pixel);
//IMPLEMENT_SHADER_TYPE(,AHRBlurV,TEXT("AHRUpsample"),TEXT("BlurV"),SF_Pixel);

// Sadly can't use templates here, as for some reason switching shader causes a MASIVE slowdown
IMPLEMENT_SHADER_TYPE(,AHRBlur,TEXT("AHRBlur"),TEXT("main"),SF_Pixel);


void FApproximateHybridRaytracer::Upsample(FRHICommandListImmediate& RHICmdList,FViewInfo& View)
{

	SCOPED_DRAW_EVENT(RHICmdList,AHRUpsample);

	RHICmdList.SetBlendState(TStaticBlendState<CW_RGBA>::GetRHI());
	RHICmdList.SetRasterizerState(TStaticRasterizerState<FM_Solid, CM_None>::GetRHI());
	RHICmdList.SetDepthStencilState(TStaticDepthStencilState<false, CF_Always>::GetRHI());

	const auto& ___tmpTarget = GSceneRenderTargets.AHRRaytracingTarget[0]->GetRenderTargetItem().TargetableTexture->GetTexture2D();
	FVector2D texSize(___tmpTarget->GetSizeX(),___tmpTarget->GetSizeY());
	FIntRect SrcRect = View.ViewRect;
	FIntRect DestRect(FIntPoint(0,0),FIntPoint(texSize.X,texSize.Y));

	RHICmdList.SetViewport(SrcRect.Min.X, SrcRect.Min.Y, 0.0f,texSize.X, texSize.Y, 1.0f);

	// Get the shaders
	TShaderMapRef<AHRPassVS<0>> VertexShader(View.ShaderMap);
	TShaderMapRef<AHRBlur> PSBlur(View.ShaderMap);

	SetGlobalBoundShaderState(RHICmdList, View.FeatureLevel, PSBlur->GetBoundShaderState(),  GFilterVertexDeclaration.VertexDeclarationRHI, *VertexShader, *PSBlur);
	VertexShader->SetParameters(RHICmdList,View);

	// The reflection buffer is not blurred
	for(int i = 0;i < 5;i++)
	{
		// Horizontal pass
		FRHITexture2D * target = GSceneRenderTargets.AHRUpsampledTarget0->GetRenderTargetItem().TargetableTexture->GetTexture2D();
		SetRenderTarget(RHICmdList, target, FTextureRHIRef());
		PSBlur->SetParameters(RHICmdList, View,GSceneRenderTargets.AHRRaytracingTarget[i]->GetRenderTargetItem().ShaderResourceTexture->GetTexture2D(),
											   GSceneRenderTargets.AHRUpsampledTarget1->GetRenderTargetItem().ShaderResourceTexture->GetTexture2D(),1,0);

		// Draw!
		DrawRectangle( 
			RHICmdList,
			0, 0,
			DestRect.Width(), DestRect.Height(),
			SrcRect.Min.X, SrcRect.Min.Y, 
			SrcRect.Width(), SrcRect.Height(),
			DestRect.Size(),
			GSceneRenderTargets.GetBufferSizeXY(),
			*VertexShader,
			EDRF_UseTriangleOptimization);

		// Vertical pass
		target = GSceneRenderTargets.AHRRaytracingTarget[i]->GetRenderTargetItem().TargetableTexture->GetTexture2D();
		SetRenderTarget(RHICmdList, target, FTextureRHIRef());
		PSBlur->SetParameters(RHICmdList, View,GSceneRenderTargets.AHRUpsampledTarget0->GetRenderTargetItem().ShaderResourceTexture->GetTexture2D(),
											   GSceneRenderTargets.AHRUpsampledTarget1->GetRenderTargetItem().ShaderResourceTexture->GetTexture2D(),0,1);

		// Draw!
		DrawRectangle( 
			RHICmdList,
			0, 0,
			DestRect.Width(), DestRect.Height(),
			SrcRect.Min.X, SrcRect.Min.Y, 
			SrcRect.Width(), SrcRect.Height(),
			DestRect.Size(),
			GSceneRenderTargets.GetBufferSizeXY(),
			*VertexShader,
			EDRF_UseTriangleOptimization);

		// Copy to the prev target
		
		// DEBUG!!!
		// Disabled for now

		/*RHICmdList.CopyToResolveTarget(GSceneRenderTargets.AHRRaytracingTarget[i]->GetRenderTargetItem().ShaderResourceTexture,
			                           GSceneRenderTargets.AHRUpsampledTarget1->GetRenderTargetItem().TargetableTexture,
									   true, FResolveParams());*/
	}
		
	return;
#if 0








	SCOPED_DRAW_EVENT(RHICmdList,AHRUpsample);

	RHICmdList.SetBlendState(TStaticBlendState<CW_RGBA>::GetRHI());
	RHICmdList.SetRasterizerState(TStaticRasterizerState<FM_Solid, CM_None>::GetRHI());
	RHICmdList.SetDepthStencilState(TStaticDepthStencilState<false, CF_Always>::GetRHI());
	RHICmdList.SetViewport(View.ViewRect.Min.X, View.ViewRect.Min.Y, 0.0f,RaytracingTarget->GetSizeX(), RaytracingTarget->GetSizeY(), 1.0f);

	FIntRect SrcRect = View.ViewRect;
	FIntRect DestRect(FIntPoint(0,0),FIntPoint(RaytracingTarget->GetSizeX(),RaytracingTarget->GetSizeY()));

	// Get the shaders
	TShaderMapRef<AHRPassVS> VertexShader(View.ShaderMap);
	TShaderMapRef<AHRBlurH> PSBlurH(View.ShaderMap);
	TShaderMapRef<AHRBlurV> PSBlurV(View.ShaderMap);

	///////// Pass 0
	SetRenderTarget(RHICmdList, UpsampledTarget0, FTextureRHIRef());

	// Clear the target before drawing
	RHICmdList.Clear(true, FLinearColor::Black, false, 1.0f, false, 0, FIntRect());

	SetGlobalBoundShaderState(RHICmdList, View.FeatureLevel, PSBlurH->GetBoundShaderState(),  GFilterVertexDeclaration.VertexDeclarationRHI, *VertexShader, *PSBlurH);
	VertexShader->SetParameters(RHICmdList,View);
	PSBlurH->SetParameters(RHICmdList, View, RaytracingTargetSRV,1.0);

	// Draw!
	DrawRectangle( 
		RHICmdList,
		0, 0,
		DestRect.Width(), DestRect.Height(),
		SrcRect.Min.X, SrcRect.Min.Y, 
		SrcRect.Width(), SrcRect.Height(),
		DestRect.Size(),
		GSceneRenderTargets.GetBufferSizeXY(),
		*VertexShader,
		EDRF_UseTriangleOptimization);

	///////// Pass 1
	SetRenderTarget(RHICmdList, UpsampledTarget1, FTextureRHIRef());

	// Clear the target before drawing
	RHICmdList.Clear(true, FLinearColor::Black, false, 1.0f, false, 0, FIntRect());

	SetGlobalBoundShaderState(RHICmdList, View.FeatureLevel, PSBlurV->GetBoundShaderState(),  GFilterVertexDeclaration.VertexDeclarationRHI, *VertexShader, *PSBlurV);
	VertexShader->SetParameters(RHICmdList,View);
	PSBlurV->SetParameters(RHICmdList, View, UpsampledTargetSRV0,1.0);

	// Draw!
	DrawRectangle( 
		RHICmdList,
		0, 0,
		DestRect.Width(), DestRect.Height(),
		SrcRect.Min.X, SrcRect.Min.Y, 
		SrcRect.Width(), SrcRect.Height(),
		DestRect.Size(),
		GSceneRenderTargets.GetBufferSizeXY(),
		*VertexShader,
		EDRF_UseTriangleOptimization);
#define TWO_PASS_BLUR
#ifdef TWO_PASS_BLUR
	///////// Pass 2
	SetRenderTarget(RHICmdList, UpsampledTarget0, FTextureRHIRef());

	// Clear the target before drawing
	RHICmdList.Clear(true, FLinearColor::Black, false, 1.0f, false, 0, FIntRect());

	SetGlobalBoundShaderState(RHICmdList, View.FeatureLevel, PSBlurH->GetBoundShaderState(),  GFilterVertexDeclaration.VertexDeclarationRHI, *VertexShader, *PSBlurH);
	VertexShader->SetParameters(RHICmdList,View);
	PSBlurH->SetParameters(RHICmdList, View, UpsampledTargetSRV1,1.0);

	// Draw!
	DrawRectangle( 
		RHICmdList,
		0, 0,
		DestRect.Width(), DestRect.Height(),
		SrcRect.Min.X, SrcRect.Min.Y, 
		SrcRect.Width(), SrcRect.Height(),
		DestRect.Size(),
		GSceneRenderTargets.GetBufferSizeXY(),
		*VertexShader,
		EDRF_UseTriangleOptimization);

	///////// Pass 3
	SetRenderTarget(RHICmdList, UpsampledTarget1, FTextureRHIRef());

	// Clear the target before drawing
	RHICmdList.Clear(true, FLinearColor::Black, false, 1.0f, false, 0, FIntRect());

	SetGlobalBoundShaderState(RHICmdList, View.FeatureLevel, PSBlurV->GetBoundShaderState(),  GFilterVertexDeclaration.VertexDeclarationRHI, *VertexShader, *PSBlurV);
	VertexShader->SetParameters(RHICmdList,View);
	PSBlurV->SetParameters(RHICmdList, View, UpsampledTargetSRV0,1.0);

	// Draw!
	DrawRectangle( 
		RHICmdList,
		0, 0,
		DestRect.Width(), DestRect.Height(),
		SrcRect.Min.X, SrcRect.Min.Y, 
		SrcRect.Width(), SrcRect.Height(),
		DestRect.Size(),
		GSceneRenderTargets.GetBufferSizeXY(),
		*VertexShader,
		EDRF_UseTriangleOptimization);
#endif


#endif
}

BEGIN_UNIFORM_BUFFER_STRUCT(AHRCompositeCB,)
	DECLARE_UNIFORM_BUFFER_STRUCT_MEMBER(float,GIMultiplier)
END_UNIFORM_BUFFER_STRUCT(AHRCompositeCB)
IMPLEMENT_UNIFORM_BUFFER_STRUCT(AHRCompositeCB,TEXT("AHRCompositeCB"));

class AHRCompositePS : public FGlobalShader
{
	DECLARE_SHADER_TYPE(AHRCompositePS,Global)
public:

	static bool ShouldCache(EShaderPlatform Platform)
	{
		return IsFeatureLevelSupported(Platform, ERHIFeatureLevel::SM5);
	}

	static void ModifyCompilationEnvironment(EShaderPlatform Platform, FShaderCompilerEnvironment& OutEnvironment)
	{
		FGlobalShader::ModifyCompilationEnvironment(Platform, OutEnvironment);
	}

	AHRCompositePS(const ShaderMetaType::CompiledShaderInitializerType& Initializer)
	:	FGlobalShader(Initializer)
	{
		DeferredParameters.Bind(Initializer.ParameterMap);
		LinearSampler.Bind(Initializer.ParameterMap, TEXT("samLinear"));
		cb.Bind(Initializer.ParameterMap,TEXT("AHRCompositeCB"));
		ObjNormal.Bind(Initializer.ParameterMap,TEXT("ObjNormal"));

		Trace0.Bind(Initializer.ParameterMap, TEXT("Trace0"));
		Trace1.Bind(Initializer.ParameterMap, TEXT("Trace1"));
		Trace2.Bind(Initializer.ParameterMap, TEXT("Trace2"));
		Trace3.Bind(Initializer.ParameterMap, TEXT("Trace3"));
		Trace4.Bind(Initializer.ParameterMap, TEXT("Trace4"));
		Trace5.Bind(Initializer.ParameterMap, TEXT("Trace5"));

		Kernel0.Bind(Initializer.ParameterMap, TEXT("Kernel0"));
		Kernel1.Bind(Initializer.ParameterMap, TEXT("Kernel1"));
		Kernel2.Bind(Initializer.ParameterMap, TEXT("Kernel2"));
		Kernel3.Bind(Initializer.ParameterMap, TEXT("Kernel3"));
		Kernel4.Bind(Initializer.ParameterMap, TEXT("Kernel4"));
	}

	AHRCompositePS()
	{
	}

	void SetParameters(FRHICommandList& RHICmdList, const FSceneView& View)
	{
		const FPixelShaderRHIParamRef ShaderRHI = GetPixelShader();
		FGlobalShader::SetParameters(RHICmdList, ShaderRHI,View);
		DeferredParameters.Set(RHICmdList, ShaderRHI, View);

		auto sampler = TStaticSamplerState<SF_Trilinear,AM_Wrap,AM_Wrap,AM_Wrap>::GetRHI();
		
		if(ObjNormal.IsBound())
			RHICmdList.SetShaderResourceViewParameter(ShaderRHI,ObjNormal.GetBaseIndex(),AHREngine.ObjectNormalSRV);
		AHRCompositeCB cbdata;

		cbdata.GIMultiplier = View.FinalPostProcessSettings.AHRIntensity;

		SetUniformBufferParameterImmediate(RHICmdList, ShaderRHI,cb,cbdata);

		SetTextureParameter(RHICmdList, ShaderRHI, Trace0, LinearSampler,sampler, GSceneRenderTargets.AHRRaytracingTarget[0]->GetRenderTargetItem().ShaderResourceTexture->GetTexture2D());	
		SetTextureParameter(RHICmdList, ShaderRHI, Trace1, LinearSampler,sampler, GSceneRenderTargets.AHRRaytracingTarget[1]->GetRenderTargetItem().ShaderResourceTexture->GetTexture2D());	
		SetTextureParameter(RHICmdList, ShaderRHI, Trace2, LinearSampler,sampler, GSceneRenderTargets.AHRRaytracingTarget[2]->GetRenderTargetItem().ShaderResourceTexture->GetTexture2D());	
		SetTextureParameter(RHICmdList, ShaderRHI, Trace3, LinearSampler,sampler, GSceneRenderTargets.AHRRaytracingTarget[3]->GetRenderTargetItem().ShaderResourceTexture->GetTexture2D());	
		SetTextureParameter(RHICmdList, ShaderRHI, Trace4, LinearSampler,sampler, GSceneRenderTargets.AHRRaytracingTarget[4]->GetRenderTargetItem().ShaderResourceTexture->GetTexture2D());	
		SetTextureParameter(RHICmdList, ShaderRHI, Trace5, LinearSampler,sampler, GSceneRenderTargets.AHRRaytracingTarget[5]->GetRenderTargetItem().ShaderResourceTexture->GetTexture2D());	

		SetTextureParameter(RHICmdList, ShaderRHI, Kernel0, LinearSampler,sampler, GSceneRenderTargets.AHRPerPixelInterpolationKernel[0]->GetRenderTargetItem().ShaderResourceTexture->GetTexture2D());	
		SetTextureParameter(RHICmdList, ShaderRHI, Kernel1, LinearSampler,sampler, GSceneRenderTargets.AHRPerPixelInterpolationKernel[1]->GetRenderTargetItem().ShaderResourceTexture->GetTexture2D());	
		SetTextureParameter(RHICmdList, ShaderRHI, Kernel2, LinearSampler,sampler, GSceneRenderTargets.AHRPerPixelInterpolationKernel[2]->GetRenderTargetItem().ShaderResourceTexture->GetTexture2D());	
		SetTextureParameter(RHICmdList, ShaderRHI, Kernel3, LinearSampler,sampler, GSceneRenderTargets.AHRPerPixelInterpolationKernel[3]->GetRenderTargetItem().ShaderResourceTexture->GetTexture2D());	
		SetTextureParameter(RHICmdList, ShaderRHI, Kernel4, LinearSampler,sampler, GSceneRenderTargets.AHRPerPixelInterpolationKernel[4]->GetRenderTargetItem().ShaderResourceTexture->GetTexture2D());	

		if(LinearSampler.IsBound())
			RHICmdList.SetShaderSampler(ShaderRHI,LinearSampler.GetBaseIndex(),sampler);
	}

	virtual bool Serialize(FArchive& Ar)
	{		
		bool bShaderHasOutdatedParameters = FGlobalShader::Serialize(Ar);
		Ar << DeferredParameters;
		Ar << LinearSampler;
		Ar << cb;
		Ar << ObjNormal;

		Ar << Trace0;
		Ar << Trace1;
		Ar << Trace2;
		Ar << Trace3;
		Ar << Trace4;
		Ar << Trace5;

		Ar << Kernel0;
		Ar << Kernel1;
		Ar << Kernel2;
		Ar << Kernel3;
		Ar << Kernel4;

		return bShaderHasOutdatedParameters;
	}

	FGlobalBoundShaderState& GetBoundShaderState()
	{
		static FGlobalBoundShaderState State;
		
		return State;
	}

private:
	FDeferredPixelShaderParameters DeferredParameters;
	FShaderResourceParameter LinearSampler;
	FShaderResourceParameter ObjNormal;

	FShaderResourceParameter Trace0,Trace1,Trace2,Trace3,Trace4,Trace5;
	FShaderResourceParameter Kernel0,Kernel1,Kernel2,Kernel3,Kernel4;

	TShaderUniformBufferParameter<AHRCompositeCB> cb;
};
IMPLEMENT_SHADER_TYPE(,AHRCompositePS,TEXT("AHRComposite"),TEXT("PS"),SF_Pixel);

void FApproximateHybridRaytracer::Composite(FRHICommandListImmediate& RHICmdList,FViewInfo& View)
{
	SCOPED_DRAW_EVENT(RHICmdList,AHRComposite);

	// Simply render a full screen quad and sample the upsampled buffer. Use additive blending to mix it with the light accumulation buffer
	// Only one view at a time for now (1/11/2014)

	// Set additive blending
	RHICmdList.SetBlendState(TStaticBlendState<CW_RGBA, BO_Add, BF_One, BF_One, BO_Add, BF_One, BF_One>::GetRHI());
	//RHICmdList.SetBlendState(TStaticBlendState<CW_RGBA, BO_Add, BF_One, BF_Zero, BO_Add, BF_One, BF_Zero>::GetRHI());

	// add gi and multiply scene color by ao
	// final = gi + ao*direct
	//RHICmdList.SetBlendState(TStaticBlendState<CW_RGBA, BO_Add, BF_One, BF_SourceAlpha, BO_Add, BF_One, BF_One>::GetRHI());


	//		DEBUG!!!!!
	//RHICmdList.SetBlendState(TStaticBlendState<CW_RGBA, BO_Add, BF_Zero, BF_SourceColor, BO_Add, BF_One, BF_One>::GetRHI());



	// Set the viewport, raster state and depth stencil
	FIntRect DestRect(View.ViewRect.Min,FIntPoint(View.Family->FamilySizeX,View.Family->FamilySizeY));
	RHICmdList.SetViewport(DestRect.Min.X,DestRect.Min.Y,0.0f,DestRect.Max.X,DestRect.Max.Y,1.0f);
	//RHICmdList.SetViewport(0, 0, 0.0f,ResX, ResY, 1.0f);
	RHICmdList.SetRasterizerState(TStaticRasterizerState<FM_Solid, CM_None>::GetRHI());
	RHICmdList.SetDepthStencilState(TStaticDepthStencilState<false, CF_Always>::GetRHI());

	// Get the shaders
	TShaderMapRef<AHRPassVS<0>> VertexShader(View.ShaderMap);
	TShaderMapRef<AHRCompositePS> PixelShader(View.ShaderMap);

	// Bound shader parameters
	SetGlobalBoundShaderState(RHICmdList, View.FeatureLevel, PixelShader->GetBoundShaderState(),  GFilterVertexDeclaration.VertexDeclarationRHI, *VertexShader, *PixelShader);
	VertexShader->SetParameters(RHICmdList,View);
	float scalingX = float(View.Family->FamilySizeX) / float(GSceneRenderTargets.GetBufferSizeXY().X);
	float scalingY = float(View.Family->FamilySizeY) / float(GSceneRenderTargets.GetBufferSizeXY().Y);
	PixelShader->SetParameters(RHICmdList, View);

	// Draw!
	DrawRectangle( 
				RHICmdList,
				0, 0,
				DestRect.Width(), DestRect.Height(),
				DestRect.Min.X, DestRect.Min.Y, 
				View.ViewRect.Width(), View.ViewRect.Height(),
				DestRect.Size(),
				GSceneRenderTargets.GetBufferSizeXY(),
				*VertexShader,
				EDRF_UseTriangleOptimization);
	/*	DrawRectangle( 
		RHICmdList,
		0, 0,
		ResX, ResY,
		0, 0, 
		ResX, ResY,
		FIntPoint(ResX,ResY),
		GSceneRenderTargets.GetBufferSizeXY(),
		*VertexShader,
		EDRF_UseTriangleOptimization);*/
}