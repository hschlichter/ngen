// Meshlet drawing, shared by the task shader (meshlet.task) and the mesh shaders of every mesh pass (src/renderer/README.md, "Meshlets").
// The geometry set (GeometryPass::descriptorBindings): the view, the scene tables, the geometry pool and the frame slot's culling buffers.
//
// A draw is one region (instancecull.comp lays out every drawn instance's meshlets as work items, in instance order); each task workgroup
// takes 32 work items, whatever their instance, tests them and launches a mesh workgroup per meshlet that survives, in order.

// The rendered camera's view and projection, and the culling camera's view-projection (the depth prepass draws from it while it isn't the
// rendered one: meshletFlagCullCamera).
layout(set = 0, binding = 0) uniform UBO {
    mat4 view;
    mat4 proj;
    mat4 cullViewProj;
} ubo;

// GpuInstanceRecord.
struct Instance {
    mat4 model;
    uint material;
    uint mesh;
    uint meshletOffset; // the submesh's meshlets, from the mesh's first
    uint meshletCount;
    vec3 boundsMin;
    uint flags;
    vec3 boundsMax;
    uint lodLevel;   // drawn LOD level + 1, 0 under no LOD root
    uint lodGroup;   // its render-LOD group + 1, 0 for none
    uint triangleCount;
    uint streamRoot; // its streaming root's slot + 1, 0 under none
    uint lodDepth;   // how many LOD roots it is under, 0 under none
};
layout(std430, set = 0, binding = 2) readonly buffer Instances {
    Instance data[];
} instances;

// GpuMaterial: the texture slot the fragment shader samples, and the cutout opacity (cutout.glsl).
struct Material {
    uint baseColorTexture;
    uint opacityTexture;
    uint opacityInfo;
    float opacityThreshold;
};
layout(std430, set = 0, binding = 3) readonly buffer Materials {
    Material data[];
} materials;

// The vertex pool: Vertex (position, normal, colour, texture coordinate), 11 floats each.
layout(std430, set = 0, binding = 4) readonly buffer Vertices {
    float data[];
} vertices;

// The position pool: 3 floats per vertex, at the vertex pool's index.
layout(std430, set = 0, binding = 5) readonly buffer Positions {
    float data[];
} positions;

// GpuMeshlet: offsets into the meshlet vertex and triangle pools, vertexCount | triangleCount << 8, the normal cone as four signed
// bytes (axis xyz, cos of the half angle, each * 127), the bounding sphere in the mesh's space.
struct Meshlet {
    uint vertexOffset;
    uint triangleOffset;
    uint counts;
    uint cone;
    vec3 center;
    float radius;
};
layout(std430, set = 0, binding = 6) readonly buffer Meshlets {
    Meshlet data[];
} meshlets;

// Mesh-local vertex indices, one per meshlet vertex.
layout(std430, set = 0, binding = 7) readonly buffer MeshletVertices {
    uint data[];
} meshletVertices;

// One word per triangle: three meshlet-local vertex indices in its low three bytes.
layout(std430, set = 0, binding = 8) readonly buffer MeshletTriangles {
    uint data[];
} meshletTriangles;

// GpuMeshEntry: where a mesh's vertices and meshlets start in the pool.
struct MeshEntry {
    int vertexOffset;
    uint meshletOffset;
    uint meshletCount;
    uint triangleCount;
};
layout(std430, set = 0, binding = 9) readonly buffer Meshes {
    MeshEntry data[];
} meshes;

// The meshlets to draw (instancecull.comp's work items): the instance and the meshlet's place in the meshlet pool.
layout(std430, set = 0, binding = 10) readonly buffer WorkItems {
    uvec2 data[];
} workItems;

// The camera (0), the shadow cascades (1-4), the camera's late phase (5, occlusion culling's second phase).
const uint maxViews = 6;
const uint lateView = 5;
// DrawLists::CullParams, up to the planes (the region offsets after them are the culling's): the views' frustum planes (normalized; the camera's far plane always passes; view 5 repeats the camera's), the
// culling camera's position and view-projection, the depth pyramid's size.
layout(std430, set = 0, binding = 11) readonly buffer Params {
    uint instanceCount;
    uint viewCount;
    uint cullEnabled;
    uint commandCapacity;
    uint groupCount;
    uint occlusion;
    uint hzbWidth;
    uint hzbHeight;
    uint workCapacity;
    uint commandsUsed;
    uint pad1;
    uint pad2;
    vec4 cameraPosition;
    mat4 cameraViewProj;
    vec4 planes[maxViews * 6];
} params;

// Meshlet counts per view (DrawLists::meshletCounter*), summed by the passes that count (meshletFlagCount); the culling scan zeroes them.
const uint meshletCounterTested = 0;
const uint meshletCounterFrustum = 1;
const uint meshletCounterCone = 2;
const uint meshletCounterOccluded = 3;
const uint meshletCounterDrawn = 4;
const uint meshletCounterTriangles = 5;
const uint meshletCountersPerView = 6;
layout(std430, set = 0, binding = 12) buffer MeshletStats {
    uint data[];
} meshletStats;

// The depth pyramid (DepthPyramid, hzb.glsl), for the meshlets' occlusion test.
layout(set = 0, binding = 13) uniform sampler2D hzb;

// RegionWork (instancecull.comp): a region's draw and its range of work items.
struct RegionWork {
    uint taskGroupsX;
    uint taskGroupsY;
    uint taskGroupsZ;
    uint firstItem;
    uint itemCount;
    uint pad0;
    uint pad1;
    uint pad2;
};
layout(std430, set = 0, binding = 14) readonly buffer RegionWorks {
    RegionWork data[];
} regionWork;

// Per render-LOD group, lodselect.comp's state: the level fading in and its share (lodfade.glsl). The mesh shaders of the cutout buckets,
// where a fading group's instances draw, pass an instance's share of the fade to the fragment shader; gbuffer.mesh its LOD tag.
layout(std430, set = 0, binding = 15) readonly buffer LodStates {
    uvec4 data[];
} lodStates;

#include "lodfade.glsl"

const uint bucketCount = 4;
const uint meshletsPerTask = 32;

const uint flagDoubleSided = 1u << 1;
const uint flagBoundsValid = 1u << 2;
const uint flagCutout = 1u << 3;

// Push-constant flags, the second word of every mesh pass's push constants (after the region).
const uint meshletFlagCount = 1u << 0; // this pass adds to the meshlet counts
const uint meshletFlagSplit = 1u << 1;     // two mesh workgroups per meshlet, each half its triangles (the debug views)
const uint meshletFlagOcclusion = 1u << 2; // test meshlets against the depth pyramid
const uint meshletFlagClassify = 1u << 3;  // keep every meshlet, with the test that culls it (the meshletcull debug view)
const uint meshletFlagHiddenOnly = 1u << 4; // with meshletFlagClassify: keep only the meshlets the depth pyramid hides
const uint meshletFlagCullCamera = 1u << 5; // the depth prepass draws from the culling camera (ubo.cullViewProj), into its own depth

// Why a meshlet is culled (meshletFlagClassify): kept, outside the frustum, facing away, hidden by depth.
const uint meshletKept = 0;
const uint meshletCulledFrustum = 1;
const uint meshletCulledCone = 2;
const uint meshletCulledOccluded = 3;

// Packs: the mesh passes (but the debug views') draw the survivors of a task workgroup packed in order, as many consecutive meshlets per
// mesh workgroup as fit in packMaxVertices and packMaxTriangles (a full meshlet alone, or up to 32 small ones), a vertex and up to two
// triangles a thread. Small meshlets (grass, pebbles: about 20 triangles) would otherwise each take a workgroup of their own. The
// primitive order is the meshlets', so the image is the same.
const uint packThreads = 64;
const uint packMaxVertices = 64;
const uint packMaxTriangles = 124;

// What a task workgroup hands its mesh workgroups: the meshlets that survived, each with its instance and, classifying, why it would be
// culled; and their packs: pack p holds survivors packFirst[p] .. packFirst[p + 1], with packVertices[p] vertices and packTriangles[p]
// triangles, each survivor's first vertex and triangle in its pack at vertexStarts and triangleStarts.
struct MeshletPayload {
    uint meshlets[meshletsPerTask];
    uint instances[meshletsPerTask];
    uint reasons[meshletsPerTask];
    uint vertexStarts[meshletsPerTask];
    uint triangleStarts[meshletsPerTask];
    uint packFirst[meshletsPerTask + 1];
    uint packVertices[meshletsPerTask];
    uint packTriangles[meshletsPerTask];
    // Per survivor, so a mesh lane reads neither the meshlet nor the mesh table: where its vertex indices and triangles start in their
    // pools, and its mesh's first vertex in the vertex pools.
    uint vertexIndexBases[meshletsPerTask];
    uint triangleBases[meshletsPerTask];
    int poolVertexOffsets[meshletsPerTask];
};

// The task shader writes it, its mesh workgroups read it.
taskPayloadSharedEXT MeshletPayload payload;

// A mesh workgroup's meshlet and instance, and where the instance's mesh's vertices start (meshlet vertex indices are mesh-local).
struct MeshletDraw {
    Meshlet meshlet;
    uint meshletIndex;
    uint instance;
    int vertexOffset;
    uint reason; // meshletKept, or meshletCulled* while classifying
};

MeshletDraw meshletDraw(uint slot) {
    MeshletDraw draw;
    draw.meshletIndex = payload.meshlets[slot];
    draw.meshlet = meshlets.data[draw.meshletIndex];
    draw.instance = payload.instances[slot];
    draw.reason = payload.reasons[slot];
    draw.vertexOffset = meshes.data[instances.data[draw.instance].mesh].vertexOffset;
    return draw;
}

uint meshletVertexCount(Meshlet meshlet) {
    return meshlet.counts & 0xFFu;
}

uint meshletTriangleCount(Meshlet meshlet) {
    return (meshlet.counts >> 8) & 0xFFu;
}

// A vertex of the pool by its pool index.
vec3 poolPosition(uint vertex) {
    return vec3(positions.data[vertex * 3u], positions.data[vertex * 3u + 1u], positions.data[vertex * 3u + 2u]);
}

vec3 poolNormal(uint vertex) {
    uint base = vertex * 11u;
    return vec3(vertices.data[base + 3u], vertices.data[base + 4u], vertices.data[base + 5u]);
}

vec3 poolColor(uint vertex) {
    uint base = vertex * 11u;
    return vec3(vertices.data[base + 6u], vertices.data[base + 7u], vertices.data[base + 8u]);
}

vec2 poolTexCoord(uint vertex) {
    uint base = vertex * 11u;
    return vec2(vertices.data[base + 9u], vertices.data[base + 10u]);
}

// The pool index of a meshlet's local vertex.
uint meshletPoolVertex(Meshlet meshlet, int vertexOffset, uint local) {
    return uint(int(meshletVertices.data[meshlet.vertexOffset + local]) + vertexOffset);
}

uvec3 meshletTriangle(Meshlet meshlet, uint triangle) {
    uint packed = meshletTriangles.data[meshlet.triangleOffset + triangle];
    return uvec3(packed & 0xFFu, (packed >> 8) & 0xFFu, (packed >> 16) & 0xFFu);
}

// A pack in a mesh shader: its first survivor in the payload and how many it holds. The payload lives in memory, not on chip: a pack of
// one meshlet reads it at uniform addresses; a pack of several copies its meshlets' words to shared memory once (loadPackShared) and its
// lanes search there.
struct PackInfo {
    uint first;
    uint count;
};

shared uint packVertexStarts[meshletsPerTask];
shared uint packTriangleStarts[meshletsPerTask];
shared uint packVertexBases[meshletsPerTask];
shared uint packTriangleBases[meshletsPerTask];
shared int packPoolOffsets[meshletsPerTask];
shared uint packInstances[meshletsPerTask];

PackInfo packInfo(uint pack) {
    PackInfo info;
    info.first = payload.packFirst[pack];
    info.count = payload.packFirst[pack + 1u] - info.first;
    return info;
}

// For a pack of several meshlets, by the lanes below its count; the caller's barrier makes it visible.
void loadPackShared(PackInfo info) {
    uint i = gl_LocalInvocationIndex;
    if (i < info.count) {
        uint slot = info.first + i;
        packVertexStarts[i] = payload.vertexStarts[slot];
        packTriangleStarts[i] = payload.triangleStarts[slot];
        packVertexBases[i] = payload.vertexIndexBases[slot];
        packTriangleBases[i] = payload.triangleBases[slot];
        packPoolOffsets[i] = payload.poolVertexOffsets[slot];
        packInstances[i] = payload.instances[slot];
    }
}

// The meshlet of a several-meshlet pack whose range holds the pack's vertex (or triangle) `index`: the last whose start is at most it.
// Starts rise strictly (a meshlet has at least one vertex and one triangle).
uint packMeshletOf(PackInfo info, uint index, bool byTriangle) {
    uint low = 0u;
    uint high = info.count - 1u;
    while (low < high) {
        uint middle = (low + high + 1u) / 2u;
        uint start = byTriangle ? packTriangleStarts[middle] : packVertexStarts[middle];
        if (start <= index) {
            low = middle;
        } else {
            high = middle - 1u;
        }
    }
    return low;
}

// A pack's vertex: its meshlet's place in the pack (to index what the mesh shader keeps per meshlet), its instance and its pool index.
struct PackedVertex {
    uint meshlet;
    uint instance;
    uint vertex;
};

PackedVertex packedVertex(PackInfo info, uint v) {
    PackedVertex packed;
    if (info.count == 1u) {
        packed.meshlet = 0u;
        packed.instance = payload.instances[info.first];
        packed.vertex = uint(int(meshletVertices.data[payload.vertexIndexBases[info.first] + v]) + payload.poolVertexOffsets[info.first]);
        return packed;
    }
    uint m = packMeshletOf(info, v, false);
    packed.meshlet = m;
    packed.instance = packInstances[m];
    packed.vertex = uint(int(meshletVertices.data[packVertexBases[m] + (v - packVertexStarts[m])]) + packPoolOffsets[m]);
    return packed;
}

// A pack's triangle, its corners as the pack's vertices.
uvec3 packedTriangle(PackInfo info, uint t) {
    uint word = 0u;
    uint vertexStart = 0u;
    if (info.count == 1u) {
        word = meshletTriangles.data[payload.triangleBases[info.first] + t];
    } else {
        uint m = packMeshletOf(info, t, true);
        word = meshletTriangles.data[packTriangleBases[m] + (t - packTriangleStarts[m])];
        vertexStart = packVertexStarts[m];
    }
    return uvec3(word & 0xFFu, (word >> 8) & 0xFFu, (word >> 16) & 0xFFu) + uvec3(vertexStart);
}

#ifdef MESHLET_MESH_SHADER
// The mesh passes' workgroups: a pack, a vertex and up to two triangles a lane.
layout(local_size_x = packThreads) in;
layout(triangles, max_vertices = packMaxVertices, max_primitives = packMaxTriangles) out;

#endif

bool cameraView(uint view) {
    return view == 0u || view == lateView;
}
