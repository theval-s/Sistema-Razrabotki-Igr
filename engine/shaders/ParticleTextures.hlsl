Texture2D<float4> source : register(t0);
RWTexture2DArray<float4> destination : register(u0);
SamplerState linearClamp : register(s0);
cbuffer Frame : register(b0) { uint layer, hasSource; uint2 pad; };
[numthreads(8,8,1)] void CSMain(uint2 pixel:SV_DispatchThreadID) {
    float2 uv=(pixel+.5)/256.0, q=uv*2-1;
    float4 value=float4(1,1,1,pow(saturate(1-dot(q,q)),2));
    if(hasSource) {
        // Sample the mip nearest a 256-texel footprint so large images don't alias.
        uint w,h,levels; source.GetDimensions(0,w,h,levels);
        value=source.SampleLevel(linearClamp,uv,clamp(log2(max(w,h)/256.0),0,levels-1.0));
    }
    destination[uint3(pixel,layer)]=value;
}
