struct Foo
{
	float4 b[6];
};

cbuffer Buf
{
	Foo foo;
};

float4 main(uint a : A, float4 p : P) : SV_Target
{
	float4 bs[6];
	[unroll]
	for (int i = 0; i < 3; i++)
		bs[i] = foo.b[i];

	float4 result = bs[a % 3];

	for (int i = 3; i < 6; i++)
		bs[i] = foo.b[i];

	return p * bs[a % 6] + result;
}
