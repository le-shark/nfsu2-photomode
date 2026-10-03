// Running mean in linear light: out = prev + (linear(scene) - prev) * k.x, k.x = 1/(n+1).
// CPU reference: sampling.h SrgbToLinear / RunningMean (unit-tested).
sampler2D prevTex : register(s0);
sampler2D sceneTex : register(s1);
float4 k : register(c0);

float3 SrgbToLinear(float3 c) {
  float3 lo = c / 12.92;
  float3 hi = pow(abs((c + 0.055) / 1.055), 2.4);  // c >= 0: abs only silences X3571
  return lerp(hi, lo, step(c, 0.04045));  // step(c, t) = 1 where c <= t
}

float4 main(float2 uv : TEXCOORD0) : COLOR {
  float3 prev = tex2D(prevTex, uv).rgb;
  float3 s = SrgbToLinear(tex2D(sceneTex, uv).rgb);
  return float4(prev + (s - prev) * k.x, 1);
}
