// Plain copy of an 8-bit texture (the frame held on screen during tile captures).
sampler2D src : register(s0);

float4 main(float2 uv : TEXCOORD0) : COLOR {
  return float4(tex2D(src, uv).rgb, 1);
}
