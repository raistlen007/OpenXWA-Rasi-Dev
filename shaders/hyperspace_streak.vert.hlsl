cbuffer HyperspaceStreakVS : register(b0, space1)
{
    row_major float4x4 view_proj;
    float4 viewport; // xy: render-target size in pixels
};

struct VSIn
{
    float3 position : POSITION;
    float4 color : COLOR0;
    float3 center_position : POSITION1;
    float2 shape_uv : TEXCOORD0;
};

struct VSOut
{
    float4 position : SV_Position;
    float4 color : COLOR0;
    float2 shape_uv : TEXCOORD0;
};

VSOut main(VSIn input)
{
    VSOut output;
    float4 edge = mul(view_proj, float4(input.position, 1.0f));
    float4 center = mul(view_proj, float4(input.center_position, 1.0f));

    // A pixel-wide guard band lets fragment AA represent subpixel-thin
    // streaks instead of dropping them when the rasterizer misses the quad.
    float2 size = max(viewport.xy, float2(1.0f, 1.0f));
    float edge_w = abs(edge.w) > 1.0e-4f ? edge.w : 1.0e-4f;
    float center_w = abs(center.w) > 1.0e-4f ? center.w : 1.0e-4f;
    float2 radial_px = (edge.xy / edge_w - center.xy / center_w) * (0.5f * size);
    float half_width_px = length(radial_px);
    float2 outward = half_width_px > 1.0e-5f ? radial_px / half_width_px : float2(1.0f, 0.0f);
    const float fringe_px = 1.0f;
    edge.xy += outward * (2.0f * fringe_px / size) * edge.w;

    output.position = edge;
    output.color = input.color;
    output.shape_uv = float2(
        input.shape_uv.x * (1.0f + fringe_px / max(half_width_px, 1.0e-4f)),
        input.shape_uv.y);
    return output;
}
