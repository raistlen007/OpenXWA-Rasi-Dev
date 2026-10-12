struct PSIn
{
    float4 position : SV_Position;
    float4 color : COLOR0;
    float2 shape_uv : TEXCOORD0;
};

float4 main(PSIn input) : SV_Target0
{
    // Integrate the hard-sided original streak width over approximately
    // one screen pixel. Unlike edge blur alone, this preserves the energy
    // of narrow lines while suppressing stair-step and subpixel flicker.
    float across = input.shape_uv.x;
    float footprint = max(fwidth(across), 1.0e-4f);
    float visible = max(0.0f,
        min(1.0f, across + 0.5f * footprint) -
        max(-1.0f, across - 0.5f * footprint));
    float coverage = saturate(visible / footprint);

    // Gentle end falloff, confined to the existing quad rather than an
    // additional bloom layer. Leave the central length crisp.
    float along = saturate(input.shape_uv.y);
    float ends = smoothstep(0.0f, 0.055f, along) *
                 smoothstep(0.0f, 0.055f, 1.0f - along);
    float opacity = coverage * ends;
    return float4(input.color.rgb * opacity, input.color.a * opacity);
}
