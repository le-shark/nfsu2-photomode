// Focus probe (focusprobe.h): the INTZ depth texture's value (0..1) copied into an R32F target for the readback.
// Sampling INTZ returns the depth in every channel (point filter, set by focusprobe.cpp).
sampler2D depthTex : register(s0);

float4 main(float2 uv : TEXCOORD0) : COLOR0 {
  return tex2D(depthTex, uv).rrrr;
}
