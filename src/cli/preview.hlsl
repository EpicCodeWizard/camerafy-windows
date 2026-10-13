// Compiled at build time by fxc (CMake); nothing is compiled at runtime.
Texture2D<float> texY : register(t0);
Texture2D<float2> texUV : register(t1);
SamplerState samp : register(s0);

struct VSOut {
    float4 pos : SV_Position;
    float2 uv : TEXCOORD0;
};

// Fullscreen triangle, no vertex buffer.
VSOut vsMain(uint id : SV_VertexID) {
    VSOut o;
    o.uv = float2((id << 1) & 2, id & 2);
    o.pos = float4(o.uv * float2(2, -2) + float2(-1, 1), 0, 1);
    return o;
}

// BT.709 limited range (the phone sends video-range 420v with BT.709 VUI).
float4 psMain(VSOut i) : SV_Target {
    float y = (texY.Sample(samp, i.uv) - 16.0 / 255.0) * (255.0 / 219.0);
    float2 c = (texUV.Sample(samp, i.uv) - 128.0 / 255.0) * (255.0 / 224.0);
    float r = y + 1.5748 * c.y;
    float g = y - 0.1873 * c.x - 0.4681 * c.y;
    float b = y + 1.8556 * c.x;
    return float4(saturate(float3(r, g, b)), 1);
}
