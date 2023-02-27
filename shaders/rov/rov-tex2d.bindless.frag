RasterizerOrderedTexture2D<float4> RW : register(u0);

[earlydepthstencil]
void main(float4 pos : SV_Position)
{
	uint2 coord = uint2(pos.xy);
	RW[coord] += float4(1.0, 2.0, 3.0, 4.0);
}
