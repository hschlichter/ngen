#include "sceneloader.h"

#include <cstdio>
#include <cstring>
#include <string>

#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtc/quaternion.hpp>

#define CGLTF_IMPLEMENTATION
#include <cgltf.h>

#define STB_IMAGE_IMPLEMENTATION
#include <stb_image.h>

static void loadPrimitive(MeshData& mesh, cgltf_primitive* prim, const char* filepath) {
    if (prim->indices) {
        size_t count = prim->indices->count;
        mesh.indices.resize(count);
        for (size_t i = 0; i < count; i++) {
            mesh.indices[i] = (uint32_t) cgltf_accessor_read_index(prim->indices, i);
        }
    }

    cgltf_accessor* posAccessor = nullptr;
    cgltf_accessor* normAccessor = nullptr;
    cgltf_accessor* uvAccessor = nullptr;

    for (size_t i = 0; i < prim->attributes_count; i++) {
        if (prim->attributes[i].type == cgltf_attribute_type_position) {
            posAccessor = prim->attributes[i].data;
        } else if (prim->attributes[i].type == cgltf_attribute_type_normal) {
            normAccessor = prim->attributes[i].data;
        } else if (prim->attributes[i].type == cgltf_attribute_type_texcoord) {
            uvAccessor = prim->attributes[i].data;
        }
    }

    if (!posAccessor) {
        return;
    }
    size_t vertCount = posAccessor->count;
    mesh.vertices.resize(vertCount);

    for (size_t i = 0; i < vertCount; i++) {
        Vertex& v = mesh.vertices[i];
        cgltf_accessor_read_float(posAccessor, i, v.position, 3);
        if (normAccessor) {
            cgltf_accessor_read_float(normAccessor, i, v.normal, 3);
        } else {
            v.normal[0] = v.normal[1] = 0;
            v.normal[2] = 1;
        }
        if (uvAccessor) {
            cgltf_accessor_read_float(uvAccessor, i, v.texCoord, 2);
        } else {
            v.texCoord[0] = v.texCoord[1] = 0;
        }
        v.color[0] = v.color[1] = v.color[2] = 1.0f;
    }

    if (mesh.indices.empty()) {
        mesh.indices.resize(vertCount);
        for (size_t i = 0; i < vertCount; i++) {
            mesh.indices[i] = (uint32_t) i;
        }
    }

    if (prim->material && prim->material->pbr_metallic_roughness.base_color_texture.texture) {
        cgltf_image* img = prim->material->pbr_metallic_roughness.base_color_texture.texture->image;
        if (img) {
            int channels;
            uint8_t* pixels = nullptr;
            if (img->buffer_view) {
                const uint8_t* bufData = (const uint8_t*) img->buffer_view->buffer->data + img->buffer_view->offset;
                pixels = stbi_load_from_memory(bufData, (int) img->buffer_view->size, &mesh.texWidth, &mesh.texHeight, &channels, 4);
            } else if (img->uri) {
                std::string dir(filepath);
                size_t slash = dir.find_last_of("/\\");
                std::string texPath = (slash != std::string::npos ? dir.substr(0, slash + 1) : "") + img->uri;
                pixels = stbi_load(texPath.c_str(), &mesh.texWidth, &mesh.texHeight, &channels, 4);
            }
            if (pixels) {
                mesh.texPixels.assign(pixels, pixels + (size_t) mesh.texWidth * mesh.texHeight * 4);
                stbi_image_free(pixels);
            }
        }
    }
}

static glm::mat4 getNodeTransform(cgltf_node* node) {
    glm::mat4 t(1.0f);
    if (node->has_matrix) {
        memcpy(&t, node->matrix, sizeof(float) * 16);
    } else {
        if (node->has_translation) {
            t = glm::translate(t, glm::vec3(node->translation[0], node->translation[1], node->translation[2]));
        }
        if (node->has_rotation) {
            glm::quat q(node->rotation[3], node->rotation[0], node->rotation[1], node->rotation[2]);
            t *= glm::mat4_cast(q);
        }
        if (node->has_scale) {
            t = glm::scale(t, glm::vec3(node->scale[0], node->scale[1], node->scale[2]));
        }
    }
    return t;
}

static void processNode(Scene& scene, cgltf_node* node, const glm::mat4& parentTransform, const char* filepath) {
    glm::mat4 world = parentTransform * getNodeTransform(node);
    if (node->mesh) {
        for (size_t p = 0; p < node->mesh->primitives_count; p++) {
            MeshData md;
            md.transform = world;
            loadPrimitive(md, &node->mesh->primitives[p], filepath);
            if (!md.vertices.empty()) {
                printf("  mesh: %zu verts, %zu indices, tex %dx%d\n", md.vertices.size(), md.indices.size(), md.texWidth, md.texHeight);
                scene.meshes.push_back(std::move(md));
            }
        }
    }
    for (size_t i = 0; i < node->children_count; i++) {
        processNode(scene, node->children[i], world, filepath);
    }
}

Scene loadGltf(const char* filepath) {
    Scene scene;
    cgltf_options options = {};
    cgltf_data* gltf = nullptr;
    if (cgltf_parse_file(&options, filepath, &gltf) != cgltf_result_success) {
        fprintf(stderr, "cgltf_parse_file failed\n");
        return scene;
    }
    if (cgltf_load_buffers(&options, gltf, filepath) != cgltf_result_success) {
        fprintf(stderr, "cgltf_load_buffers failed\n");
        cgltf_free(gltf);
        return scene;
    }

    if (gltf->scene) {
        for (size_t i = 0; i < gltf->scene->nodes_count; i++) {
            processNode(scene, gltf->scene->nodes[i], glm::mat4(1.0f), filepath);
        }
    } else {
        for (size_t i = 0; i < gltf->nodes_count; i++) {
            if (!gltf->nodes[i].parent) {
                processNode(scene, &gltf->nodes[i], glm::mat4(1.0f), filepath);
            }
        }
    }

    printf("Loaded %zu mesh(es)\n", scene.meshes.size());
    cgltf_free(gltf);
    return scene;
}
