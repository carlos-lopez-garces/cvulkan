#include "frame_graph.hpp"

#include "foundation/file.hpp"
#include "foundation/memory.hpp"
#include "foundation/string.hpp"

#include "graphics/command_buffer.hpp"
#include "graphics/gpu_device.hpp"
#include "graphics/gpu_resources.hpp"

#include "external/json.hpp"

#include <string>

namespace raptor
{

static FrameGraphResourceType string_to_resource_type( cstring input_type ) {
    if ( strcmp( input_type, "texture" ) == 0 ) {
        return FrameGraphResourceType_Texture;
    }

    if ( strcmp( input_type, "attachment" ) == 0 ) {
        return FrameGraphResourceType_Attachment;
    }

    if ( strcmp( input_type, "buffer" ) == 0 ) {
        return FrameGraphResourceType_Buffer;
    }

    if ( strcmp( input_type, "reference" ) == 0 ) {
        // This is used for resources that need to create an edge but are not actually
        // used by the render pass
        return FrameGraphResourceType_Reference;
    }

    RASSERT( false );
    return FrameGraphResourceType_Invalid;
}

RenderPassOperation::Enum string_to_render_pass_operation( cstring op ) {
    if ( strcmp( op, "VK_ATTACHMENT_LOAD_OP_CLEAR" ) == 0 ) {
        return RenderPassOperation::Clear;
    } else if ( strcmp( op, "VK_ATTACHMENT_LOAD_OP_LOAD" ) == 0 ) {
        return RenderPassOperation::Load;
    }

    RASSERT( false );
    return RenderPassOperation::DontCare;
}

// FrameGraph /////////////////////////////////////////////////////////////

void FrameGraph::init( FrameGraphBuilder* builder_ ) {
    allocator = &MemoryService::instance()->system_allocator;

    local_allocator.init( rmega( 1 ) );

    builder = builder_;

    nodes.init( allocator, FrameGraphBuilder::k_max_nodes_count );
}

void FrameGraph::shutdown() {
    for ( u32 i = 0; i < nodes.size; ++i ) {
        FrameGraphNodeHandle handle = nodes[ i ];
        FrameGraphNode* node = builder->access_node( handle );

        builder->device->destroy_render_pass( node->render_pass );
        builder->device->destroy_framebuffer( node->framebuffer );

        node->inputs.shutdown();
        node->outputs.shutdown();
        node->edges.shutdown();
    }

    nodes.shutdown();

    local_allocator.shutdown();
}

void FrameGraph::parse( cstring file_path, StackAllocator* temp_allocator ) {
    using json = nlohmann::json;

    if ( !file_exists( file_path ) ) {
        rprint( "Cannot find file %s\n", file_path );
        return;
    }

    sizet current_allocator_marker = temp_allocator->get_marker();

    FileReadResult read_result = file_read_text( file_path, temp_allocator );

    json graph_data = json::parse( read_result.data );

    StringBuffer string_buffer;
    string_buffer.init( 1024, &local_allocator );

    std::string name_value = graph_data.value( "name", "" );
    name = string_buffer.append_use_f( "%s", name_value.c_str() );

    // Passes have nodes and node have inputs and outputs. Inputs and outputs are
    // resources.
    //
    // For each node in each pass, create a FrameGraphNodeCreation with its inputs
    // and outputs. Then have the builder create a node with it. Store ther resulting
    // handle in FrameGraph::nodes.
    json passes = graph_data[ "passes" ];
    for ( sizet i = 0; i < passes.size(); ++i ) {
        json pass = passes[ i ];

        json pass_inputs = pass[ "inputs" ];
        json pass_outputs = pass[ "outputs" ];

        FrameGraphNodeCreation node_creation{ };
        node_creation.inputs.init( temp_allocator, pass_inputs.size() );
        node_creation.outputs.init( temp_allocator, pass_outputs.size() );

        // Inputs.
        for ( sizet ii = 0; ii < pass_inputs.size(); ++ii ) {
            json pass_input = pass_inputs[ ii ];

            FrameGraphResourceInputCreation input_creation{ };

            std::string input_type = pass_input.value( "type", "" );
            RASSERT( !input_type.empty() );

            std::string input_name = pass_input.value( "name", "" );
            RASSERT( !input_name.empty() );

            input_creation.type = string_to_resource_type( input_type.c_str() );
            input_creation.resource_info.external = false;

            input_creation.name = string_buffer.append_use_f( "%s", input_name.c_str() );

            node_creation.inputs.push( input_creation );
        }

        // Outputs.
        for ( sizet oi = 0; oi < pass_outputs.size(); ++oi ) {
            json pass_output = pass_outputs[ oi ];

            FrameGraphResourceOutputCreation output_creation{ };

            std::string output_type = pass_output.value( "type", "" );
            RASSERT( !output_type.empty() );

            std::string output_name = pass_output.value( "name", "" );
            RASSERT( !output_name.empty() );

            output_creation.type = string_to_resource_type( output_type.c_str() );

            output_creation.name = string_buffer.append_use_f( "%s", output_name.c_str() );

            switch ( output_creation.type ) {
                case FrameGraphResourceType_Attachment:
                case FrameGraphResourceType_Texture:
                {
                    std::string format = pass_output.value( "format", "" );
                    RASSERT( !format.empty() );

                    output_creation.resource_info.texture.format = util_string_to_vk_format( format.c_str() );

                    std::string load_op = pass_output.value( "op", "" );
                    RASSERT( !load_op.empty() );

                    output_creation.resource_info.texture.load_op = string_to_render_pass_operation( load_op.c_str() );

                    json resolution = pass_output[ "resolution" ];

                    output_creation.resource_info.texture.width = resolution[0];
                    output_creation.resource_info.texture.height = resolution[1];
                    output_creation.resource_info.texture.depth = 1;
                } break;
                case FrameGraphResourceType_Buffer:
                {
                    // TODO(marco)
                    RASSERT( false );
                } break;
            }

            node_creation.outputs.push( output_creation );
        }

        name_value = pass.value( "name", "" );
        RASSERT( !name_value.empty() );

        bool enabled = pass.value( "enabled", true );

        node_creation.name = string_buffer.append_use_f( "%s", name_value.c_str() );
        node_creation.enabled = enabled;

        // node_creation stores inputs and outputs. Have the building create a node
        // with it. Store the resulting handle in FrameGraph::nodes.
        FrameGraphNodeHandle node_handle = builder->create_node(node_creation);
        nodes.push(node_handle);
    }

    temp_allocator->free_marker( current_allocator_marker );
}

// Compute edges of the given node, i.e. find the nodes that produce as outputs the
// inputs that this node needs; connect those producer nodes with the given node with
// and edge.
static void compute_edges(FrameGraph* frame_graph, FrameGraphNode* node, u32 node_index) {
    // For each input of the node, find the node that outputs it. Connect the
    // producer node with this node with an edge.
    for ( u32 r = 0; r < node->inputs.size; ++r ) {
        // TODO: what's the difference between resource and output_resource? output_resource
        // is obtained using resource's name.
        //
        // The book says: "Note that internally, the graph stores the outputs in a map keyed by name".
        // So FrameGraph::get_resource() retrieves output resources exclusively.
        FrameGraphResource* resource = frame_graph->access_resource(node->inputs[r]);

        FrameGraphResource* output_resource = frame_graph->get_resource(resource->name);
        if (output_resource == nullptr && !resource->resource_info.external) {
            RASSERTM(false, "Requested resource is not produced by any node and is not external.");
            continue;
        }

        resource->producer = output_resource->producer;
        resource->resource_info = output_resource->resource_info;
        resource->output_handle = output_resource->output_handle;

        FrameGraphNode* parent_node = frame_graph->access_node(resource->producer);

        // rprint( "Adding edge from %s [%d] to %s [%d]\n", parent_node->name, resource->producer.index, node->name, node_index )

        // Create an edge from the producer (parent) node to this node.
        parent_node->edges.push(frame_graph->nodes[node_index]);
    }
}

static void create_framebuffer( FrameGraph* frame_graph, FrameGraphNode* node ) {
    FramebufferCreation framebuffer_creation{ };
    framebuffer_creation.render_pass = node->render_pass;
    framebuffer_creation.set_name( node->name );

    u32 width = 0;
    u32 height = 0;

    for ( u32 r = 0; r < node->outputs.size; ++r ) {
        FrameGraphResource* resource = frame_graph->access_resource( node->outputs[ r ] );

        FrameGraphResourceInfo& info = resource->resource_info;

        if ( resource->type == FrameGraphResourceType_Buffer || resource->type == FrameGraphResourceType_Reference ) {
            continue;
        }

        if ( width == 0 ) {
            width = info.texture.width;
        } else {
            RASSERT( width == info.texture.width );
        }

        if ( height == 0 ) {
            height = info.texture.height;
        } else {
            RASSERT( height == info.texture.height );
        }

        if ( info.texture.format == VK_FORMAT_D32_SFLOAT ) {
            framebuffer_creation.set_depth_stencil_texture( info.texture.texture );
        } else {
            framebuffer_creation.add_render_texture( info.texture.texture );
        }
    }

    for ( u32 r = 0; r < node->inputs.size; ++r ) {
        FrameGraphResource* input_resource = frame_graph->access_resource( node->inputs[ r ] );

        if ( input_resource->type == FrameGraphResourceType_Buffer || input_resource->type == FrameGraphResourceType_Reference) {
            continue;
        }

        FrameGraphResource* resource = frame_graph->get_resource( input_resource->name );

        FrameGraphResourceInfo& info = resource->resource_info;

        input_resource->resource_info.texture.texture = info.texture.texture;

        if ( width == 0 ) {
            width = info.texture.width;
        } else {
            RASSERT( width == info.texture.width );
        }

        if ( height == 0 ) {
            height = info.texture.height;
        } else {
            RASSERT( height == info.texture.height );
        }

        if ( input_resource->type == FrameGraphResourceType_Texture ) {
            continue;
        }

        if ( info.texture.format == VK_FORMAT_D32_SFLOAT ) {
            framebuffer_creation.set_depth_stencil_texture( info.texture.texture );
        } else {
            framebuffer_creation.add_render_texture( info.texture.texture );
        }
    }

    framebuffer_creation.width = width;
    framebuffer_creation.height = height;
    node->framebuffer = frame_graph->builder->device->create_framebuffer( framebuffer_creation );
}

static void create_render_pass( FrameGraph* frame_graph, FrameGraphNode* node ) {
    RenderPassCreation render_pass_creation{ };
    render_pass_creation.set_name( node->name );

    // NOTE(marco): first create the outputs, then we can patch the input resources
    // with the right handles
    for ( sizet i = 0; i < node->outputs.size; ++i ) {
        FrameGraphResource* output_resource = frame_graph->access_resource( node->outputs[ i ] );

        FrameGraphResourceInfo& info = output_resource->resource_info;

        if ( output_resource->type == FrameGraphResourceType_Attachment ) {
            if ( info.texture.format == VK_FORMAT_D32_SFLOAT ) {
                render_pass_creation.set_depth_stencil_texture( info.texture.format, VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL );

                render_pass_creation.depth_operation = RenderPassOperation::Clear;
            } else {
                render_pass_creation.add_attachment( info.texture.format, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, info.texture.load_op );
            }
        }
    }

    for ( sizet i = 0; i < node->inputs.size; ++i ) {
        FrameGraphResource* input_resource = frame_graph->access_resource( node->inputs[ i ] );

        FrameGraphResourceInfo& info = input_resource->resource_info;

        if ( input_resource->type == FrameGraphResourceType_Attachment ) {
            if ( info.texture.format == VK_FORMAT_D32_SFLOAT ) {
                render_pass_creation.set_depth_stencil_texture( info.texture.format, VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL );

                render_pass_creation.depth_operation = RenderPassOperation::Load;
            } else {
                render_pass_creation.add_attachment( info.texture.format, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, RenderPassOperation::Load );
            }
        }
    }

    // TODO(marco): make sure formats are valid for attachment
    node->render_pass = frame_graph->builder->device->create_render_pass( render_pass_creation );
}

void FrameGraph::enable_render_pass( cstring render_pass_name ) {
    FrameGraphNode* node = builder->get_node( render_pass_name );
    node->enabled = true;
}

void FrameGraph::disable_render_pass( cstring render_pass_name ) {
    FrameGraphNode* node = builder->get_node( render_pass_name );
    node->enabled = false;
}

// Computes the edges between nodes of the frame graph. When the frame graph is parsed,
// only nodes are created. Here, edges are determined based on nodes' inputs and outputs.
void FrameGraph::compile() {
    // TODO(marco)
    // - check that input has been produced by a different node
    // - cull inactive nodes

    for ( u32 i = 0; i < nodes.size; ++i ) {
        FrameGraphNode* node = builder->access_node( nodes[ i ] );

        // NOTE(marco): we want to clear all edges first, then populate them. If we clear them inside the loop
        // below we risk clearing the list after it has already been used by one of the child nodes
        node->edges.clear();
    }

    // Compute edges.
    for (u32 i = 0; i < nodes.size; ++i) {
        FrameGraphNode* node = builder->access_node(nodes[i]);
        if (!node->enabled) {
            continue;
        }

        // Upon return, all the nodes that output a resource that this node needs will
        // have a connecting edge with this node.
        compute_edges(this, node, i);
    }

    // Different arrays used in the sorting operation.
    Array<FrameGraphNodeHandle> sorted_nodes;
    sorted_nodes.init(&local_allocator, nodes.size);
    Array<u8> visited;
    visited.init(&local_allocator, nodes.size, nodes.size);
    memset(visited.data, 0, sizeof(bool) * nodes.size);
    Array<FrameGraphNodeHandle> stack;
    stack.init(&local_allocator, nodes.size);

    // "Topological" sort of the frame DAG. A topological sort or topological ordering of
    // a directed graph is a linear ordering of its vertices such that for every directed
    // edge uv from vertex u to vertex v, u comes before v in the ordering.
    //
    // Having nodes listed in topological order is essential for memory aliasing, because
    // we need to identify the node that first outputs each resource.
    for ( u32 n = 0; n < nodes.size; ++n ) {
        FrameGraphNode* node = builder->access_node( nodes[ n ] );
        if ( !node->enabled ) {
            continue;
        }

        stack.push( nodes[ n ] );

        while ( stack.size > 0 ) {
            FrameGraphNodeHandle node_handle = stack.back();

            if (visited[ node_handle.index ] == 2) {
                stack.pop();

                continue;
            }

            if ( visited[ node_handle.index ]  == 1) {
                visited[ node_handle.index ] = 2; // added

                sorted_nodes.push( node_handle );

                stack.pop();

                continue;
            }

            visited[ node_handle.index ] = 1; // visited

            FrameGraphNode* node = builder->access_node( node_handle );

            // Leaf node
            if ( node->edges.size == 0 ) {
                continue;
            }

            for ( u32 r = 0; r < node->edges.size; ++r ) {
                FrameGraphNodeHandle child_handle = node->edges[ r ];

                if ( !visited[ child_handle.index ] ) {
                    stack.push( child_handle );
                }
            }
        }
    }

    RASSERT( sorted_nodes.size == nodes.size );

    // Reset the unordered list of nodes. Insert them back in topological order.
    nodes.clear();
    for (i32 i = sorted_nodes.size - 1; i >= 0; --i) {
        nodes.push(sorted_nodes[i]);
    }

    visited.shutdown();
    stack.shutdown();
    sorted_nodes.shutdown();

    // Do memory aliasing. Multiple *different* resources declared in the graph can
    // point to the same memory allocation. This is possible when these resources don't
    // coexist in time, that is, two resources may be declared to point to the same
    // memory allocation, but one of them uses it first and then the other.
    //
    // To that end, the frame graph is analyzed to determine which resources reside
    // in memory simultaneously; when they do, they can't share memory allocations.
    //
    // For each resource, we need to determine which node outputs it for the first time
    // and which is the last node that receives it as input. The allocations array
    // records the node that outputs a given resource first and the deallocations array
    // records the last node that receives it as input.
    //
    // Note that topological sorting is essential for doing this. At each step (node)
    // of the traversal, we know which resources haven't been allocated, which ones
    // have been allocated but not deallocated yet, and which ones have been allocated
    // and then deallocated, becoming free for reuse at subsequent steps. 

    // The allocations array has an entry for each resource; the entry will store
    // the handle of the node that outputs that resource FIRST. This array allows us
    // to determine at each step (node) of the topological travesal, whether a given
    // resource has been allocated already.
    sizet resource_count = builder->resource_cache.resources.used_indices;
    Array<FrameGraphNodeHandle> allocations;
    allocations.init(&local_allocator, resource_count, resource_count);
    for (u32 i = 0; i < resource_count; ++i) {
        allocations[i].index = k_invalid_index;
    }

    // The deallocations array has an entry for each resource; the entry will store
    // the handle of the last node that receives it as input. This array allows us
    // to determine at each step (node) of the topological traversal, whether a given
    // resource has been deallocated already (and thus available for reuse at
    // subsequent steps).
    Array<FrameGraphNodeHandle> deallocations;
    deallocations.init( &local_allocator, resource_count, resource_count );
    for ( u32 i = 0; i < resource_count; ++i) {
        deallocations[ i ].index = k_invalid_index;
    }

    // Throughout the topological traversal, an output resource that is encountered
    // for the first time will be allocated either from scratch or reuse a free
    // resource. When that output resource gets deallocated, it becomes free for
    // reuse at subsequent steps.
    Array<TextureHandle> free_list;
    free_list.init(&local_allocator, resource_count);

    // Increase the reference count of each output resource: number of inputs that
    // refer to it.
    for (u32 i = 0; i < nodes.size; ++i) {
        FrameGraphNode* node = builder->access_node(nodes[i]);
        if (!node->enabled) {
            continue;
        }

        for (u32 j = 0; j < node->inputs.size; ++j) {
            FrameGraphResource *input_resource = builder->access_resource(node->inputs[j]);
            FrameGraphResource *resource = builder->access_resource(input_resource->output_handle);
            resource->ref_count++;
        }
    }

    // Traverse the graph in topological order. 
    for (u32 i = 0; i < nodes.size; ++i) {
        FrameGraphNode *node = builder->access_node(nodes[i]);
        if (!node->enabled) {
            continue;
        }

        // For each output resource of this node, determine if this node is the first
        // one that outputs it; if so, either create the resource from scratch or reuse
        // a free one if available. (A resource becomes available for reuse when it's
        // reference count drops to 0; note that this may happen more than once throughout
        // the topological traversal.) When a resource reuses a free resource, we call it
        // an alias.
        for (u32 j = 0; j < node->outputs.size; ++j) {
            u32 resource_index = node->outputs[j].index;
            FrameGraphResource* resource = builder->access_resource(node->outputs[j]);

            if (!resource->resource_info.external && allocations[resource_index].index == k_invalid_index) {
                // There shouldn't be a deallocation for this output resource yet.
                RASSERT(deallocations[resource_index].index == k_invalid_index)

                // This node allocates this resource.
                allocations[resource_index] = nodes[i];

                if (resource->type == FrameGraphResourceType_Attachment) {
                    FrameGraphResourceInfo &info = resource->resource_info;

                    if (free_list.size > 0) {
                        // A memory allocation can be reused to create this resource.
                        TextureHandle alias_texture = free_list.back();
                        free_list.pop();

                        TextureCreation texture_creation{};
                        texture_creation
                        .set_data(nullptr)
                        // This resource is considered an alias because this memory allocation existed
                        // before under another name.
                        .set_alias(alias_texture)
                        // This memory allocation used to go by another name.
                        .set_name(resource->name)
                        .set_format_type(info.texture.format, TextureType::Enum::Texture2D)
                        .set_size(info.texture.width, info.texture.height, info.texture.depth)
                        .set_flags(1, TextureFlags::RenderTarget_mask);
                        TextureHandle handle = builder->device->create_texture(texture_creation);

                        info.texture.texture = handle;
                    } else {
                        // No available memory allocations to reuse for the creation of this resource.
                        TextureCreation texture_creation{};
                        texture_creation
                        .set_data(nullptr)
                        // First name given to this memory allocation.
                        .set_name(resource->name)
                        .set_format_type(info.texture.format, TextureType::Enum::Texture2D)
                        .set_size(info.texture.width, info.texture.height, info.texture.depth)
                        .set_flags(1, TextureFlags::RenderTarget_mask);
                        TextureHandle handle = builder->device->create_texture(texture_creation);

                        info.texture.texture = handle;
                    }
                }

                rprint("Output %s allocated on node %d\n", resource->name, nodes[i].index);
            }
        }

        // For each input resource of this node, determine if it's the last one that's currently
        // consuming it. If so, record this node in the deallocations entry of the resource and
        // put the resource in the free_list; subsequent nodes in this topological traversal
        // can reuse the resource's memory allocation.
        for (u32 j = 0; j < node->inputs.size; ++j) {
            FrameGraphResource *input_resource = builder->access_resource(node->inputs[j]);

            u32 resource_index = input_resource->output_handle.index;
            FrameGraphResource *resource = builder->access_resource(input_resource->output_handle);

            // Decrease the reference count of the output resource. When it reaches 0, this node
            // is the last one that is currently consuming it.
            resource->ref_count--;
            if (!resource->resource_info.external && resource->ref_count == 0) {
                // This is the last node that consumes this resource.
                RASSERT(deallocations[resource_index].index == k_invalid_index);
                deallocations[resource_index] = nodes[i];

                if (resource->type == FrameGraphResourceType_Attachment || resource->type == FrameGraphResourceType_Texture) {
                    // Make this resource's memory allocation available for subsequent nodes.
                    free_list.push(resource->resource_info.texture.texture);
                }

                rprint("Output %s deallocated on node %d\n", resource->name, nodes[i].index);
            }
        }
    }

    allocations.shutdown();
    deallocations.shutdown();
    free_list.shutdown();

    for ( u32 i = 0; i < nodes.size; ++i ) {
        FrameGraphNode* node = builder->access_node( nodes[ i ] );
        if ( !node->enabled ) {
            continue;
        }

        if ( node->render_pass.index == k_invalid_index ) {
            create_render_pass( this, node );
        }

        if ( node->framebuffer.index == k_invalid_index ) {
            create_framebuffer( this, node );
        }
    }
}

void FrameGraph::add_ui() {
    for ( u32 n = 0; n < nodes.size; ++n ) {
        FrameGraphNode* node = builder->access_node( nodes[ n ] );
        if ( !node->enabled ) {
            continue;
        }

        node->graph_render_pass->add_ui();
    }
}

void FrameGraph::render(CommandBuffer *gpu_commands, RenderScene *render_scene) {
    for (u32 n = 0; n < nodes.size; ++n) {
        FrameGraphNode *node = builder->access_node(nodes[n]);
        if (!node->enabled) {
            continue;
        }

        gpu_commands->push_marker( node->name );
        // TODO(marco): add clear colour to json
        gpu_commands->clear( 0.3f, 0.3f, 0.3f, 1.f );
        gpu_commands->clear_depth_stencil( 1.0f, 0 );

        u32 width = 0;
        u32 height = 0;

        // Process all the node's inputs.
        for (u32 i = 0; i < node->inputs.size; ++i) {
            FrameGraphResource *resource = builder->access_resource(node->inputs[i]);

            if (resource->type == FrameGraphResourceType_Texture) {
                Texture *texture = gpu_commands->device->access_texture(resource->resource_info.texture.texture);

                // Insert a barrier to transition the input resource from an attachment layout
                // (for use as a render target) to a shader stage layout (for use in a fragment shader).
                util_add_image_barrier(
                    gpu_commands->vk_command_buffer,
                    texture->vk_image,
                    RESOURCE_STATE_RENDER_TARGET,
                    RESOURCE_STATE_PIXEL_SHADER_RESOURCE,
                    0,
                    1,
                    resource->resource_info.texture.format == VK_FORMAT_D32_SFLOAT
                );
            } else if ( resource->type == FrameGraphResourceType_Attachment ) {
                Texture* texture = gpu_commands->device->access_texture( resource->resource_info.texture.texture );

                // These dimensions will determine the dimensions of the scissors and viewport.
                width = texture->width;
                height = texture->height;
            }
        }

        // Process the node's outputs.
        for (u32 o = 0; o < node->outputs.size; ++o) {
            FrameGraphResource *resource = builder->access_resource(node->outputs[o]);

            if (resource->type == FrameGraphResourceType_Attachment) {
                // Resouce will be used as an attachment to a render pass.

                Texture *texture = gpu_commands->device->access_texture(resource->resource_info.texture.texture);

                width = texture->width;
                height = texture->height;

                if (texture->vk_format == VK_FORMAT_D32_SFLOAT) {
                    // The resource is a depth buffer.
                    util_add_image_barrier(
                        gpu_commands->vk_command_buffer,
                        texture->vk_image,
                        RESOURCE_STATE_UNDEFINED,
                        RESOURCE_STATE_DEPTH_WRITE,
                        0,
                        1,
                        resource->resource_info.texture.format == VK_FORMAT_D32_SFLOAT
                    );
                } else {
                    // The resource is a render target.
                    util_add_image_barrier(
                        gpu_commands->vk_command_buffer,
                        texture->vk_image,
                        RESOURCE_STATE_UNDEFINED,
                        RESOURCE_STATE_RENDER_TARGET,
                        0,
                        1,
                        resource->resource_info.texture.format == VK_FORMAT_D32_SFLOAT
                    );
                }
            }
        }

        // Set scissor's dimensions.
        Rect2DInt scissor{ 0, 0, (u16) width, (u16) height };
        gpu_commands->set_scissor(&scissor);

        // Set viewport's dimensions.
        Viewport viewport{ };
        viewport.rect = { 0, 0, (u16) width, (u16) height };
        viewport.min_depth = 0.0f;
        viewport.max_depth = 1.0f;
        gpu_commands->set_viewport(&viewport);

        // Nodes might have actions that need to be performed before the render pass is done.
        node->graph_render_pass->pre_render(gpu_commands, render_scene);

        // Bind and execute the render pass of this node. 
        gpu_commands->bind_pass(node->render_pass, node->framebuffer, false);
        node->graph_render_pass->render(gpu_commands, render_scene);
        gpu_commands->end_current_render_pass();

        gpu_commands->pop_marker();
    }
}

void FrameGraph::on_resize( GpuDevice& gpu, u32 new_width, u32 new_height ) {
    for ( u32 n = 0; n < nodes.size; ++n ) {
        FrameGraphNode* node = builder->access_node( nodes[ n ] );
        if ( !node->enabled ) {
            continue;
        }

        node->graph_render_pass->on_resize( gpu, new_width, new_height );

        gpu.resize_output_textures( node->framebuffer, new_width, new_height );
    }
}

FrameGraphNode* FrameGraph::get_node( cstring name ) {
    return builder->get_node( name );
}

FrameGraphNode* FrameGraph::access_node( FrameGraphNodeHandle handle ) {
    return builder->access_node( handle );
}

FrameGraphResource* FrameGraph::get_resource( cstring name ) {
    return builder->get_resource( name );
}

FrameGraphResource* FrameGraph::access_resource( FrameGraphResourceHandle handle ) {
    return builder->access_resource( handle );
}

// FrameGraphRenderPassCache /////////////////////////////////////////////////////////////

void FrameGraphRenderPassCache::init( Allocator* allocator )
{
    render_pass_map.init( allocator, FrameGraphBuilder::k_max_render_pass_count );
}

void FrameGraphRenderPassCache::shutdown( )
{
    render_pass_map.shutdown( );
}

// FrameGraphResourceCache /////////////////////////////////////////////////////////////

void FrameGraphResourceCache::init( Allocator* allocator, GpuDevice* device_ )
{
    device = device_;

    resources.init( allocator, FrameGraphBuilder::k_max_resources_count );
    resource_map.init( allocator, FrameGraphBuilder::k_max_resources_count );
}

void FrameGraphResourceCache::shutdown( )
{
    FlatHashMapIterator it = resource_map.iterator_begin();
    while ( it.is_valid() ) {

        u32 resource_index = resource_map.get( it );
        FrameGraphResource* resource = resources.get( resource_index );

        if ( resource->type == FrameGraphResourceType_Texture || resource->type == FrameGraphResourceType_Attachment ) {
            Texture* texture = device->access_texture( resource->resource_info.texture.texture );
            device->destroy_texture( texture->handle );
        }
        else if ( resource->type == FrameGraphResourceType_Buffer ) {
            Buffer* buffer = device->access_buffer( resource->resource_info.buffer.buffer );
            device->destroy_buffer( buffer->handle );
        }

        resource_map.iterator_advance( it );
    }

    resources.free_all_resources();
    resources.shutdown();
    resource_map.shutdown( );
}

// FrameGraphNodeCache /////////////////////////////////////////////////////////////

void FrameGraphNodeCache::init( Allocator* allocator, GpuDevice* device_ )
{
    device = device_;

    nodes.init( allocator, FrameGraphBuilder::k_max_nodes_count, sizeof( FrameGraphNode ) );
    node_map.init( allocator, FrameGraphBuilder::k_max_nodes_count );
}

void FrameGraphNodeCache::shutdown( )
{
    nodes.free_all_resources();
    nodes.shutdown( );
    node_map.shutdown();
}

// FrameGraphBuilder /////////////////////////////////////////////////////////////

void FrameGraphBuilder::init( GpuDevice* device_ ) {
    device = device_;

    allocator = device->allocator;

    resource_cache.init( allocator, device );
    node_cache.init( allocator, device );
    render_pass_cache.init( allocator );
}

void FrameGraphBuilder::shutdown() {
    resource_cache.shutdown( );
    node_cache.shutdown( );
    render_pass_cache.shutdown( );
}

FrameGraphResourceHandle FrameGraphBuilder::create_node_output( const FrameGraphResourceOutputCreation& creation, FrameGraphNodeHandle producer )
{
    FrameGraphResourceHandle resource_handle{ k_invalid_index };
    resource_handle.index = resource_cache.resources.obtain_resource();

    if ( resource_handle.index == k_invalid_index ) {
        return resource_handle;
    }

    FrameGraphResource* resource = resource_cache.resources.get( resource_handle.index );
    resource->name = creation.name;
    resource->type = creation.type;

    if ( creation.type != FrameGraphResourceType_Reference ) {
        resource->resource_info = creation.resource_info;
        resource->output_handle = resource_handle;
        resource->producer = producer;
        resource->ref_count = 0;

        resource_cache.resource_map.insert( hash_bytes( ( void* )resource->name, strlen( creation.name ) ), resource_handle.index );
    }

    return resource_handle;
}

FrameGraphResourceHandle FrameGraphBuilder::create_node_input( const FrameGraphResourceInputCreation& creation )
{
    FrameGraphResourceHandle resource_handle = { k_invalid_index };

    resource_handle.index = resource_cache.resources.obtain_resource();

    if ( resource_handle.index == k_invalid_index ) {
        return resource_handle;
    }

    FrameGraphResource* resource = resource_cache.resources.get( resource_handle.index );

    resource->resource_info = { };
    resource->producer.index = k_invalid_index;
    resource->output_handle.index = k_invalid_index;
    resource->type = creation.type;
    resource->name = creation.name;
    resource->ref_count = 0;

    return resource_handle;
}

// Reserves a FrameGraphNode from the builder's FrameGraphNodeCache and populates it
// using the input FrameGraphNodeCreation. Returns the handle of that FrameGraphNode. 
FrameGraphNodeHandle FrameGraphBuilder::create_node(const FrameGraphNodeCreation& creation) {
    FrameGraphNodeHandle node_handle{ k_invalid_index };
    node_handle.index = node_cache.nodes.obtain_resource();

    if ( node_handle.index == k_invalid_index ) {
        return node_handle;
    }

    FrameGraphNode* node = ( FrameGraphNode* )node_cache.nodes.access_resource( node_handle.index );
    node->name = creation.name;
    node->enabled = creation.enabled;
    node->inputs.init( allocator, creation.inputs.size );
    node->outputs.init( allocator, creation.outputs.size );
    node->edges.init( allocator, creation.outputs.size );
    node->framebuffer = k_invalid_framebuffer;
    node->render_pass = { k_invalid_index };

    node_cache.node_map.insert( hash_bytes( ( void* )node->name, strlen( node->name ) ), node_handle.index );

    // NOTE(marco): first create the outputs, then we can patch the input resources
    // with the right handles
    for ( sizet i = 0; i < creation.outputs.size; ++i ) {
        const FrameGraphResourceOutputCreation& output_creation = creation.outputs[ i ];

        FrameGraphResourceHandle output = create_node_output( output_creation, node_handle );

        node->outputs.push( output );
    }

    for ( sizet i = 0; i < creation.inputs.size; ++i ) {
        const FrameGraphResourceInputCreation& input_creation = creation.inputs[ i ];

        FrameGraphResourceHandle input_handle = create_node_input( input_creation );

        node->inputs.push( input_handle );
    }

    return node_handle;
}

FrameGraphNode* FrameGraphBuilder::get_node( cstring name ) {
    FlatHashMapIterator it = node_cache.node_map.find( hash_calculate( name ) );
    if ( it.is_invalid() ) {
        return nullptr;
    }

    FrameGraphNode* node = ( FrameGraphNode* )node_cache.nodes.access_resource( node_cache.node_map.get( it ) );

    return node;
}

FrameGraphNode* FrameGraphBuilder::access_node( FrameGraphNodeHandle handle ) {
    FrameGraphNode* node = ( FrameGraphNode* )node_cache.nodes.access_resource( handle.index );

    return node;
}

FrameGraphResource* FrameGraphBuilder::get_resource( cstring name ) {
    FlatHashMapIterator it = resource_cache.resource_map.find( hash_calculate( name ) );
    if ( it.is_invalid() ) {
        return nullptr;
    }

    FrameGraphResource* resource = resource_cache.resources.get( resource_cache.resource_map.get( it ) );

    return resource;
}

FrameGraphResource* FrameGraphBuilder::access_resource( FrameGraphResourceHandle handle ) {
    FrameGraphResource* resource = resource_cache.resources.get( handle.index );

    return resource;
}

void FrameGraphBuilder::register_render_pass( cstring name, FrameGraphRenderPass* render_pass )
{
    u64 key = hash_calculate( name );

    FlatHashMapIterator it = render_pass_cache.render_pass_map.find( key );
    if ( it.is_valid() ) {
        return;
    }

    render_pass_cache.render_pass_map.insert( key, render_pass );

    it = node_cache.node_map.find( key );
    RASSERT( it.is_valid() );

    FrameGraphNode* node = ( FrameGraphNode* )node_cache.nodes.access_resource( node_cache.node_map.get( it ) );
    node->graph_render_pass = render_pass;
}

} // namespace raptor
