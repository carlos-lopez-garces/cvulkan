// Compute shader that generates/updates a depth pyramid.

#if defined(COMPUTE_DEPTH_PYRAMID)

// Texture corresponding to upper mip level, input.
layout(set = MATERIAL_SET, binding = 0) uniform sampler2D src;

// Texture corresponding to lower mip level, output.
layout(set = MATERIAL_SET, binding = 1) uniform writeonly image2D dst;

layout (local_size_x = 8, local_size_y = 8, local_size_z = 1) in;

void main() {
	// To generate the corresponding lower-level depth texel, read 4 contiguous
	// texels and choose the maximum value.

	ivec2 texel_position00 = ivec2(gl_GlobalInvocationID.xy) * 2;
	ivec2 texel_position01 = texel_position00 + ivec2(0, 1);
	ivec2 texel_position10 = texel_position00 + ivec2(1, 0);
	ivec2 texel_position11 = texel_position00 + ivec2(1, 1);

	float color00 = texelFetch(src, texel_position00, 0).r;
	float color01 = texelFetch(src, texel_position01, 0).r;
	float color10 = texelFetch(src, texel_position10, 0).r;
	float color11 = texelFetch(src, texel_position11, 0).r;

	// 0 is closest to camera, 1 is farthest.
	float result = max(max(max(color00, color01), color10), color11);

	// The output is used for occlusion culling in the task shader in meshlet.glsl.
	imageStore(dst, ivec2(gl_GlobalInvocationID.xy), vec4(result, 0, 0, 0));

	groupMemoryBarrier();
	barrier();
}

#endif
