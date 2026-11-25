/*
 * Kernels for lapis_metaltest (device/lapis_metal_plan.md, milestone 4):
 * built into a .metallib by Apple's compiler on CI, run on the phone's Mali
 * through LapisMetal (AIR -> NIR -> panfrost).
 */
#include <metal_stdlib>
using namespace metal;

/* out = a * k + b: two device buffers read, one written, a constant */
kernel void lapis_madd(device const uint *a [[buffer(0)]], device const uint *b [[buffer(1)]],
                       device uint *out [[buffer(2)]], constant uint &k [[buffer(3)]],
                       uint i [[thread_position_in_grid]])
{
	out[i] = a[i] * k + b[i];
}

/* every thread's own coordinates: grid position, group, position in group */
kernel void lapis_ids(device uint *out [[buffer(0)]], uint i [[thread_position_in_grid]],
                      uint g [[threadgroup_position_in_grid]], uint l [[thread_position_in_threadgroup]])
{
	out[i] = (g << 16) | l;
}

/* float math and a threadgroup reduction */
kernel void lapis_sum(device const float *in [[buffer(0)]], device float *out [[buffer(1)]],
                      uint i [[thread_position_in_grid]], uint l [[thread_position_in_threadgroup]],
                      uint g [[threadgroup_position_in_grid]])
{
	threadgroup float part[64];
	part[l] = sqrt(in[i]) * 2.0f;
	threadgroup_barrier(mem_flags::mem_threadgroup);
	for (uint s = 32; s > 0; s >>= 1) {
		if (l < s)
			part[l] += part[l + s];
		threadgroup_barrier(mem_flags::mem_threadgroup);
	}
	if (l == 0)
		out[g] = part[0];
}

/* milestone 5: a triangle through a render pass. The vertices are read out of
 * a constant buffer by vertex_id (no vertex descriptor): a position and a
 * colour each; the fragment shader multiplies the interpolated colour by a
 * tint given with setFragmentBytes. */
struct lapis_vout {
	float4 position [[position]];
	float4 color;
};

vertex lapis_vout lapis_tri_vs(uint vid [[vertex_id]], constant float4 *verts [[buffer(0)]])
{
	lapis_vout o;
	o.position = verts[vid * 2];
	o.color = verts[vid * 2 + 1];
	return o;
}

fragment float4 lapis_tri_fs(lapis_vout in [[stage_in]], constant float4 &tint [[buffer(0)]])
{
	return in.color * tint;
}

/* m5b: what loses the colour (m5 drew the triangle's pixels as 00000000) */
fragment float4 lapis_fs_const(lapis_vout in [[stage_in]]) { return float4(1.0, 0.0, 0.0, 1.0); }
fragment float4 lapis_fs_color(lapis_vout in [[stage_in]]) { return in.color; }
fragment float4 lapis_fs_tint0(lapis_vout in [[stage_in]], constant float4 &tint [[buffer(0)]]) { return tint; }
fragment float4 lapis_fs_tint1(lapis_vout in [[stage_in]], constant float4 &tint [[buffer(1)]]) { return tint; }
fragment float4 lapis_fs_dtint(lapis_vout in [[stage_in]], device const float4 *tint [[buffer(0)]]) { return tint[0]; }
fragment float4 lapis_fs_pos(lapis_vout in [[stage_in]]) { return float4(in.position.x / 256.0, in.position.y / 256.0, 0.0, 1.0); }

/* m6: a full-target quad through a vertex descriptor ([[stage_in]]), and
 * fragment shaders that sample a texture (an API sampler and a constexpr
 * one) or give a colour for the blending, viewport and scissor checks */
struct lapis_qin {
	float2 pos [[attribute(0)]];
	float2 uv [[attribute(1)]];
};

struct lapis_qout {
	float4 position [[position]];
	float2 uv;
};

vertex lapis_qout lapis_quad_vs(lapis_qin in [[stage_in]])
{
	lapis_qout o;
	o.position = float4(in.pos, 0.0, 1.0);
	o.uv = in.uv;
	return o;
}

fragment float4 lapis_tex_fs(lapis_qout in [[stage_in]], texture2d<float> t [[texture(0)]], sampler s [[sampler(0)]])
{
	return t.sample(s, in.uv);
}

fragment float4 lapis_lin_fs(lapis_qout in [[stage_in]], texture2d<float> t [[texture(0)]])
{
	constexpr sampler s(filter::linear, address::clamp_to_edge);
	return t.sample(s, in.uv);
}

fragment float4 lapis_solid_fs(lapis_qout in [[stage_in]], constant float4 &c [[buffer(0)]])
{
	return c;
}
