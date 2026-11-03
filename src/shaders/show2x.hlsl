// A 2x accumulated linear image shown at 1x: 2x2 box filter in linear light, then sRGB.
// The quad maps each target pixel centre onto the corner shared by four source texels;
// c0.xy = one source texel (1/w, 1/h), so the taps land on their centres (point sampling).
sampler2D accTex : register(s0);
float4 texel : register(c0);

float3 LinearToSrgb(float3 l) {
  float3 lo = l * 12.92;
  float3 hi = 1.055 * pow(max(l, 1e-10), 1.0 / 2.4) - 0.055;
  return lerp(hi, lo, step(l, 0.0031308));
}

float4 main(float2 uv : TEXCOORD0) : COLOR {
  float2 h = texel.xy * 0.5;
  float3 m = tex2D(accTex, uv + float2(-h.x, -h.y)).rgb + tex2D(accTex, uv + float2(h.x, -h.y)).rgb +
             tex2D(accTex, uv + float2(-h.x, h.y)).rgb + tex2D(accTex, uv + float2(h.x, h.y)).rgb;
  return float4(LinearToSrgb(saturate(m * 0.25)), 1);
}
