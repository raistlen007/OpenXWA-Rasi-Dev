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
    float3 shape_uv : TEXCOORD0;
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

    // Film-style widths in projected screen pixels: consistent across the
    // view, preserving the original 3D centerline and the flypast animation.
    float2 size = max(viewport.xy, float2(1.0f, 1.0f));
    float edge_w = abs(edge.w) > 1.0e-4f ? edge.w : (edge.w < 0.0f ? -1.0e-4f : 1.0e-4f);
    float center_w = abs(center.w) > 1.0e-4f ? center.w : (center.w < 0.0f ? -1.0e-4f : 1.0e-4f);
    float2 center_ndc = center.xy / center_w;
    float2 radial_px = (edge.xy / edge_w - center_ndc) * (0.5f * size);
    float radial_len = length(radial_px);
    float2 outward = radial_len > 1.0e-5f
        ? radial_px / radial_len : float2(input.shape_uv.x, 0.0f);
    float pixel_scale = max(0.70f, size.y / 1080.0f);
    float half_width_px = input.shape_uv.z * pixel_scale;
    const float fringe_px = 1.0f;
    edge.xy = (center_ndc + outward * (2.0f * (half_width_px + fringe_px) / size)) * edge.w;

    output.position = edge;
    output.color = input.color;
    output.shape_uv = float2(
        input.shape_uv.x * (1.0f + fringe_px / max(half_width_px, 1.0e-4f)),
        input.shape_uv.y);
    return output;
}
