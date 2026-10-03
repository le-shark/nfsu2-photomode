// Accumulated linear image -> sRGB backbuffer. CPU reference: sampling.h LinearToSrgb.
sampler2D accTex : register(s0);

float3 LinearToSrgb(float3 l) {
  float3 lo = l * 12.92;
  float3 hi = 1.055 * pow(max(l, 1e-10), 1.0 / 2.4) - 0.055;
  return lerp(hi, lo, step(l, 0.0031308));
}

float4 main(float2 uv : TEXCOORD0) : COLOR {
  return float4(LinearToSrgb(saturate(tex2D(accTex, uv).rgb)), 1);
}
