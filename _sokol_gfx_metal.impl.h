/*
    Sokol Metal rendering backend.
*/
#ifndef SOKOL_IMPL_GUARD
#error "Please do not include *.impl.h files directly"
#endif

/* memset() */
#include <string.h>
#import <Metal/Metal.h>

#ifdef __cplusplus
extern "C" {
#endif
    
enum {
    _SG_MTL_NUM_INFLIGHT_FRAMES = 2,
    _SG_MTL_DEFAULT_UB_SIZE = 4 * 1024 * 1024,
};

/*-- enum translation functions ----------------------------------------------*/
_SOKOL_PRIVATE MTLLoadAction _sg_mtl_load_action(sg_action a) {
    switch (a) {
        case SG_ACTION_CLEAR:       return MTLLoadActionClear;
        case SG_ACTION_LOAD:        return MTLLoadActionLoad;
        case SG_ACTION_DONTCARE:    return MTLLoadActionDontCare;
        default:
            SOKOL_UNREACHABLE;
            return 0;
    }
}

_SOKOL_PRIVATE MTLResourceOptions _sg_mtl_buffer_resource_options(sg_usage usg) {
    // FIXME: managed mode only on MacOS
    switch (usg) {
        case SG_USAGE_IMMUTABLE:
            return MTLResourceStorageModeShared;
        case SG_USAGE_DYNAMIC:
        case SG_USAGE_STREAM:
            return MTLCPUCacheModeWriteCombined|MTLResourceStorageModeManaged;
        default:
            SOKOL_UNREACHABLE;
            return 0;
    }
}

/*-- a pool for all Metal resource object, with deferred release queue -------*/
static uint32_t _sg_mtl_pool_size;
static NSMutableArray* _sg_mtl_pool;
static uint32_t _sg_mtl_free_queue_top;
static uint32_t* _sg_mtl_free_queue;
static uint32_t _sg_mtl_release_queue_front;
static uint32_t _sg_mtl_release_queue_back;
typedef struct {
    uint32_t frame_index;
    uint32_t pool_index;
} _sg_mtl_release_item;
static _sg_mtl_release_item* _sg_mtl_release_queue;

_SOKOL_PRIVATE void _sg_mtl_init_pool(const sg_desc* desc) {
    _sg_mtl_pool_size =
        2 * _sg_select(desc->buffer_pool_size, _SG_DEFAULT_BUFFER_POOL_SIZE) +
        3 * _sg_select(desc->image_pool_size, _SG_DEFAULT_IMAGE_POOL_SIZE) +
        4 * _sg_select(desc->shader_pool_size, _SG_DEFAULT_SHADER_POOL_SIZE) +
        2 * _sg_select(desc->pipeline_pool_size, _SG_DEFAULT_PIPELINE_POOL_SIZE) +
        _sg_select(desc->pass_pool_size, _SG_DEFAULT_PASS_POOL_SIZE);
    /* an id array which holds strong references to MTLResource objects */
    _sg_mtl_pool = [NSMutableArray arrayWithCapacity:_sg_mtl_pool_size];
    NSNull* null = [NSNull null];
    for (uint32_t i = 0; i < _sg_mtl_pool_size; i++) {
        [_sg_mtl_pool addObject:null];
    }
    SOKOL_ASSERT([_sg_mtl_pool count] == _sg_mtl_pool_size);
    /* a queue of currently free slot indices */
    _sg_mtl_free_queue_top = 0;
    _sg_mtl_free_queue = SOKOL_MALLOC(_sg_mtl_pool_size * sizeof(int));
    for (int i = _sg_mtl_pool_size-1; i >= 0; i--) {
        _sg_mtl_free_queue[_sg_mtl_free_queue_top++] = (uint32_t)i;
    }
    /* a circular queue which holds release items (frame index
       when a resource is to be released, and the resource's
       pool index
    */
    _sg_mtl_release_queue_front = 0;
    _sg_mtl_release_queue_back = 0;
    _sg_mtl_release_queue = SOKOL_MALLOC(_sg_mtl_pool_size * sizeof(_sg_mtl_release_item));
    for (uint32_t i = 0; i < _sg_mtl_pool_size; i++) {
        _sg_mtl_release_queue[i].frame_index = 0;
        _sg_mtl_release_queue[i].pool_index = 0xFFFFFFFF;
    }
}

_SOKOL_PRIVATE void _sg_mtl_destroy_pool() {
    SOKOL_FREE(_sg_mtl_release_queue);  _sg_mtl_release_queue = 0;
    SOKOL_FREE(_sg_mtl_free_queue);     _sg_mtl_free_queue = 0;
    _sg_mtl_pool = nil;
}

/* add an MTLResource to the pool, return pool index */
_SOKOL_PRIVATE uint32_t _sg_mtl_add_resource(id res) {
    SOKOL_ASSERT(_sg_mtl_free_queue_top > 0);
    const uint32_t slot_index = _sg_mtl_free_queue[--_sg_mtl_free_queue_top];
    SOKOL_ASSERT([NSNull null] == _sg_mtl_pool[slot_index]);
    _sg_mtl_pool[slot_index] = res;
    return slot_index;
}

/* mark an MTLResource for release, this will put the resource into the
   deferred-release queue, and the resource will then be releases N frames later
*/
_SOKOL_PRIVATE void _sg_mtl_release_resource(uint32_t frame_index, uint32_t pool_index) {
    SOKOL_ASSERT((pool_index >= 0) && (pool_index < _sg_mtl_pool_size));
    SOKOL_ASSERT([NSNull null] != _sg_mtl_pool[pool_index]);
    int slot_index = _sg_mtl_release_queue_front++;
    if (_sg_mtl_release_queue_front >= _sg_mtl_pool_size) {
        /* wrap-around */
        _sg_mtl_release_queue_front = 0;
    }
    /* release queue full? */
    SOKOL_ASSERT(_sg_mtl_release_queue_front != _sg_mtl_release_queue_back);
    SOKOL_ASSERT(0 == _sg_mtl_release_queue[slot_index].frame_index);
    _sg_mtl_release_queue[slot_index].frame_index = frame_index;
    _sg_mtl_release_queue[slot_index].pool_index = pool_index;
}

/* run garbage-collection pass on all resources in the release-queue */
_SOKOL_PRIVATE void _sg_mtl_garbage_collect(uint32_t frame_index) {
    const uint32_t safe_release_frame_index = frame_index + _SG_MTL_NUM_INFLIGHT_FRAMES + 1;
    while (_sg_mtl_release_queue_back != _sg_mtl_release_queue_front) {
        if (_sg_mtl_release_queue[_sg_mtl_release_queue_back].frame_index < safe_release_frame_index) {
            /* don't need to check further, release-items past this are too young */
            break;
        }
        /* safe to release this resource */
        const uint32_t pool_index = _sg_mtl_release_queue[_sg_mtl_release_queue_back].pool_index;
        SOKOL_ASSERT(pool_index < _sg_mtl_pool_size);
        SOKOL_ASSERT(_sg_mtl_pool[pool_index] != [NSNull null]);
        _sg_mtl_pool[pool_index] = [NSNull null];
        /* reset the release queue slot and advance the back index */
        _sg_mtl_release_queue[_sg_mtl_release_queue_back].frame_index = 0;
        _sg_mtl_release_queue[_sg_mtl_release_queue_back].pool_index = 0xFFFFFFFF;
        _sg_mtl_release_queue_back++;
        if (_sg_mtl_release_queue_back >= _sg_mtl_pool_size) {
            /* wrap-around */
            _sg_mtl_release_queue_back = 0;
        }
    }
}

/*-- Metal backend resource structs ------------------------------------------*/
typedef struct {
    _sg_slot slot;
    int size;
    sg_buffer_type type;
    sg_usage usage;
    uint32_t upd_frame_index;
    int num_slots;
    int active_slot;
    uint32_t mtl_buf[_SG_MTL_NUM_INFLIGHT_FRAMES];  /* index intp _sg_mtl_pool */
} _sg_buffer;

_SOKOL_PRIVATE void _sg_init_buffer(_sg_buffer* buf) {
    SOKOL_ASSERT(buf);
    memset(buf, 0, sizeof(_sg_buffer));
}

typedef struct {
    _sg_slot slot;
    sg_image_type type;
    bool render_target;
    uint16_t width;
    uint16_t height;
    uint16_t depth;
    uint16_t num_mipmaps;
    sg_usage usage;
    sg_pixel_format pixel_format;
    int sample_count;
} _sg_image;

_SOKOL_PRIVATE void _sg_init_image(_sg_image* img) {
    SOKOL_ASSERT(img);
    memset(img, 0, sizeof(_sg_image));
}

typedef struct {
    uint16_t size;
} _sg_uniform_block;

typedef struct {
    sg_image_type type;
} _sg_shader_image;

typedef struct {
    uint16_t num_uniform_blocks;
    uint16_t num_images;
    _sg_uniform_block uniform_blocks[SG_MAX_SHADERSTAGE_UBS];
    _sg_shader_image images[SG_MAX_SHADERSTAGE_IMAGES];
} _sg_shader_stage;

typedef struct {
    _sg_slot slot;
    _sg_shader_stage stage[SG_NUM_SHADER_STAGES];
} _sg_shader;

_SOKOL_PRIVATE void _sg_init_shader(_sg_shader* shd) {
    SOKOL_ASSERT(shd);
    memset(shd, 0, sizeof(_sg_shader));
}

typedef struct {
    _sg_slot slot;
    _sg_shader* shader;
    sg_shader shader_id;
    sg_primitive_type primitive_type;
    sg_index_type index_type;
} _sg_pipeline;

_SOKOL_PRIVATE void _sg_init_pipeline(_sg_pipeline* pip) {
    SOKOL_ASSERT(pip);
    memset(pip, 0, sizeof(_sg_pipeline));
}

typedef struct {
    _sg_image* image;
    sg_image image_id;
    int mip_level;
    int slice;
} _sg_attachment;

typedef struct {
    _sg_slot slot;
    _sg_attachment color_atts[SG_MAX_COLOR_ATTACHMENTS];
    _sg_attachment ds_att;
} _sg_pass;

_SOKOL_PRIVATE void _sg_init_pass(_sg_pass* pass) {
    SOKOL_ASSERT(pass);
    memset(pass, 0, sizeof(_sg_pass));
}

/*-- main Metal backend state and functions ----------------------------------*/
static bool _sg_mtl_valid;
static bool _sg_mtl_in_pass;
static bool _sg_mtl_pass_valid;
static const void*(*_sg_mtl_renderpass_descriptor_cb)(void);
static const void*(*_sg_mtl_drawable_cb)(void);
static uint32_t _sg_mtl_frame_index;
static uint32_t _sg_mtl_cur_frame_rotate_index;
static uint32_t _sg_mtl_cur_ub_offset;
static uint8_t* _sg_mtl_cur_ub_base_ptr;
static id<MTLDevice> _sg_mtl_device;
static id<MTLCommandQueue> _sg_mtl_cmd_queue;
static id<MTLCommandBuffer> _sg_mtl_cmd_buffer;
static id<MTLRenderCommandEncoder> _sg_mtl_cmd_encoder;
static id<MTLBuffer> _sg_mtl_uniform_buffers[_SG_MTL_NUM_INFLIGHT_FRAMES];
static dispatch_semaphore_t _sg_mtl_sem;

_SOKOL_PRIVATE void _sg_setup_backend(const sg_desc* desc) {
    SOKOL_ASSERT(desc);
    SOKOL_ASSERT(desc->mtl_device);
    SOKOL_ASSERT(desc->mtl_renderpass_descriptor_cb);
    SOKOL_ASSERT(desc->mtl_drawable_cb);
    _sg_mtl_init_pool(desc);
    const int ub_size = _sg_select(desc->mtl_global_uniform_buffer_size, _SG_MTL_DEFAULT_UB_SIZE);
    _sg_mtl_valid = true;
    _sg_mtl_in_pass = false;
    _sg_mtl_pass_valid = false;
    _sg_mtl_renderpass_descriptor_cb = desc->mtl_renderpass_descriptor_cb;
    _sg_mtl_drawable_cb = desc->mtl_drawable_cb;
    _sg_mtl_frame_index = 1;
    _sg_mtl_cur_frame_rotate_index = 0;
    _sg_mtl_cur_ub_offset = 0;
    _sg_mtl_cur_ub_base_ptr = 0;
    _sg_mtl_device = CFBridgingRelease(desc->mtl_device);
    _sg_mtl_sem = dispatch_semaphore_create(_SG_MTL_NUM_INFLIGHT_FRAMES);
    _sg_mtl_cmd_queue = [_sg_mtl_device newCommandQueue];
    for (int i = 0; i < _SG_MTL_NUM_INFLIGHT_FRAMES; i++) {
        _sg_mtl_uniform_buffers[i] = [_sg_mtl_device
            newBufferWithLength:ub_size
            options:MTLResourceCPUCacheModeWriteCombined|MTLResourceStorageModeManaged
        ];
    }
}

_SOKOL_PRIVATE void _sg_discard_backend() {
    SOKOL_ASSERT(_sg_mtl_valid);
    /* wait for the last frame to finish */
    for (int i = 0; i < _SG_MTL_NUM_INFLIGHT_FRAMES; i++) {
        dispatch_semaphore_wait(_sg_mtl_sem, DISPATCH_TIME_FOREVER);
    }
    _sg_mtl_valid = false;
    _sg_mtl_cmd_encoder = nil;
    _sg_mtl_cmd_buffer = nil;
    _sg_mtl_cmd_queue = nil;
    for (int i = 0; i < _SG_MTL_NUM_INFLIGHT_FRAMES; i++) {
        _sg_mtl_uniform_buffers[i] = nil;
    }
    _sg_mtl_device = nil;
    _sg_mtl_destroy_pool();
}

_SOKOL_PRIVATE bool _sg_query_feature(sg_feature f) {
    // FIXME: find out if we're running on iOS
    switch (f) {
        case SG_FEATURE_INSTANCED_ARRAYS:
        case SG_FEATURE_TEXTURE_COMPRESSION_DXT:
        case SG_FEATURE_TEXTURE_FLOAT:
        case SG_FEATURE_ORIGIN_TOP_LEFT:
        case SG_FEATURE_MSAA_RENDER_TARGETS:
        case SG_FEATURE_PACKED_VERTEX_FORMAT_10_2:
        case SG_FEATURE_MULTIPLE_RENDER_TARGET:
        case SG_FEATURE_IMAGETYPE_3D:
        case SG_FEATURE_IMAGETYPE_ARRAY:
            return true;
        default:
            return false;
    }
}

_SOKOL_PRIVATE void _sg_create_buffer(_sg_buffer* buf, const sg_buffer_desc* desc) {
    SOKOL_ASSERT(buf && desc);
    SOKOL_ASSERT(buf->slot.state == SG_RESOURCESTATE_ALLOC);
    SOKOL_ASSERT(desc->data_size <= desc->size);
    buf->size = desc->size;
    buf->type = _sg_select(desc->type, SG_BUFFERTYPE_VERTEXBUFFER);
    buf->usage = _sg_select(desc->usage, SG_USAGE_IMMUTABLE);
    buf->upd_frame_index = 0;
    buf->num_slots = buf->usage==SG_USAGE_STREAM ? _SG_MTL_NUM_INFLIGHT_FRAMES : 1;
    buf->active_slot = 0;
    MTLResourceOptions mtl_options = _sg_mtl_buffer_resource_options(buf->usage);
    for (int slot = 0; slot < buf->num_slots; slot++) {
        id<MTLBuffer> mtl_buf;
        if (buf->usage == SG_USAGE_IMMUTABLE) {
            SOKOL_ASSERT(desc->data_ptr && (desc->data_size == buf->size));
            mtl_buf = [_sg_mtl_device newBufferWithBytes:desc->data_ptr length:desc->data_size options:mtl_options];
        }
        else {
            mtl_buf = [_sg_mtl_device newBufferWithLength:buf->size options:mtl_options];
        }
        buf->mtl_buf[slot] = _sg_mtl_add_resource(mtl_buf);
    }
    buf->slot.state = SG_RESOURCESTATE_VALID;
}

_SOKOL_PRIVATE void _sg_destroy_buffer(_sg_buffer* buf) {
    SOKOL_ASSERT(buf);
    for (int slot = 0; slot < buf->num_slots; slot++) {
        _sg_mtl_release_resource(_sg_mtl_frame_index, buf->mtl_buf[slot]);
    }
    _sg_init_buffer(buf);
}

_SOKOL_PRIVATE void _sg_create_image(_sg_image* img, const sg_image_desc* desc) {
    SOKOL_ASSERT(img && desc);
    SOKOL_ASSERT(img->slot.state == SG_RESOURCESTATE_ALLOC);
    // FIXME
}

_SOKOL_PRIVATE void _sg_destroy_image(_sg_image* img) {
    SOKOL_ASSERT(img);
    // FIXME
    _sg_init_image(img);
}

_SOKOL_PRIVATE void _sg_create_shader(_sg_shader* shd, const sg_shader_desc* desc) {
    SOKOL_ASSERT(shd && desc);
    SOKOL_ASSERT(shd->slot.state == SG_RESOURCESTATE_ALLOC);
    // FIXME
}

_SOKOL_PRIVATE void _sg_destroy_shader(_sg_shader* shd) {
    SOKOL_ASSERT(shd);
    // FIXME
    _sg_init_shader(shd);
}

_SOKOL_PRIVATE void _sg_create_pipeline(_sg_pipeline* pip, _sg_shader* shd, const sg_pipeline_desc* desc) {
    SOKOL_ASSERT(pip && shd && desc);
    SOKOL_ASSERT(pip->slot.state == SG_RESOURCESTATE_ALLOC);
    SOKOL_ASSERT(desc->shader.id == shd->slot.id);
    // FIXME
}

_SOKOL_PRIVATE void _sg_destroy_pipeline(_sg_pipeline* pip) {
    SOKOL_ASSERT(pip);
    // FIXME
    _sg_init_pipeline(pip);
}

_SOKOL_PRIVATE void _sg_create_pass(_sg_pass* pass, _sg_image** att_images, const sg_pass_desc* desc) {
    SOKOL_ASSERT(pass && desc);
    SOKOL_ASSERT(pass->slot.state == SG_RESOURCESTATE_ALLOC);
}

_SOKOL_PRIVATE void _sg_destroy_pass(_sg_pass* pass) {
    SOKOL_ASSERT(pass);
    // FIXME
    _sg_init_pass(pass);
}

_SOKOL_PRIVATE void _sg_begin_pass(_sg_pass* pass, const sg_pass_action* action, int w, int h) {
    SOKOL_ASSERT(action);
    SOKOL_ASSERT(!_sg_mtl_in_pass);
    SOKOL_ASSERT(_sg_mtl_cmd_queue);
    SOKOL_ASSERT(!_sg_mtl_cmd_encoder);
    SOKOL_ASSERT(_sg_mtl_renderpass_descriptor_cb);
    _sg_mtl_in_pass = true;

    /* if this is the first pass in the frame, create a command buffer */
    if (nil == _sg_mtl_cmd_buffer) {
        /* block until the oldest frame in flight has finished */
        dispatch_semaphore_wait(_sg_mtl_sem, DISPATCH_TIME_FOREVER);
        _sg_mtl_cmd_buffer = [_sg_mtl_cmd_queue commandBufferWithUnretainedReferences];
    }

    /* if this is first pass in frame, get uniform buffer base pointer */
    if (0 == _sg_mtl_cur_ub_base_ptr) {
        _sg_mtl_cur_ub_base_ptr = (uint8_t*)[_sg_mtl_uniform_buffers[_sg_mtl_cur_frame_rotate_index] contents];
    }

    /* initialize a render pass descriptor */
    MTLRenderPassDescriptor* pass_desc = nil;
    if (pass) {
        /* offscreen render pass */
        pass_desc = [MTLRenderPassDescriptor renderPassDescriptor];
    }
    else {
        /* default render pass, call user-provided callback to provide render pass descriptor */
        pass_desc = CFBridgingRelease(_sg_mtl_renderpass_descriptor_cb());

    }
    if (pass_desc) {
        _sg_mtl_pass_valid = true;
    }
    else {
        /* default pass descriptor will not be valid if window is minized,
           don't do any rendering in this case */
        _sg_mtl_pass_valid = false;
        return;
    }
    if (pass) {
        /* FIXME: setup pass descriptor for offscreen rendering */
    }
    else {
        /* setup pass descriptor for default rendering */
        pass_desc.colorAttachments[0].loadAction = _sg_mtl_load_action(action->colors[0].action);
        const float* c = &(action->colors[0].val[0]);
        pass_desc.colorAttachments[0].clearColor = MTLClearColorMake(c[0], c[1], c[2], c[3]);
        pass_desc.depthAttachment.loadAction = _sg_mtl_load_action(action->depth.action);
        pass_desc.depthAttachment.clearDepth = action->depth.val;
        pass_desc.stencilAttachment.loadAction = _sg_mtl_load_action(action->stencil.action);
        pass_desc.stencilAttachment.clearStencil = action->stencil.val;
    }

    /* create a render command encoder, this might return nil if window is minimized */
    _sg_mtl_cmd_encoder = [_sg_mtl_cmd_buffer renderCommandEncoderWithDescriptor:pass_desc];
    if (_sg_mtl_cmd_encoder == nil) {
        _sg_mtl_pass_valid = false;
        return;
    }

    /* bind the global uniform buffer, this only happens once per frame */
    for (int slot = 0; slot < SG_MAX_SHADERSTAGE_UBS; slot++) {
        [_sg_mtl_cmd_encoder
            setVertexBuffer:_sg_mtl_uniform_buffers[_sg_mtl_cur_frame_rotate_index]
            offset:0
            atIndex:slot];
        [_sg_mtl_cmd_encoder
            setFragmentBuffer:_sg_mtl_uniform_buffers[_sg_mtl_cur_frame_rotate_index]
            offset:0
            atIndex:slot];
    }
}

_SOKOL_PRIVATE void _sg_end_pass() {
    SOKOL_ASSERT(_sg_mtl_in_pass);
    _sg_mtl_in_pass = false;
    _sg_mtl_pass_valid = false;
    if (nil != _sg_mtl_cmd_encoder) {
        [_sg_mtl_cmd_encoder endEncoding];
        _sg_mtl_cmd_encoder = nil;
    }
}

_SOKOL_PRIVATE void _sg_commit() {
    SOKOL_ASSERT(!_sg_mtl_in_pass);
    SOKOL_ASSERT(!_sg_mtl_pass_valid);
    SOKOL_ASSERT(_sg_mtl_drawable_cb);
    SOKOL_ASSERT(nil == _sg_mtl_cmd_encoder);
    SOKOL_ASSERT(nil != _sg_mtl_cmd_buffer);

    // FIXME: didModifyRange only on MacOS
    [_sg_mtl_uniform_buffers[_sg_mtl_cur_frame_rotate_index] didModifyRange:NSMakeRange(0, _sg_mtl_cur_ub_offset)];

    /* present, commit and signal semaphore when done */
    id cur_drawable = CFBridgingRelease(_sg_mtl_drawable_cb());
    [_sg_mtl_cmd_buffer presentDrawable:cur_drawable];
    __block dispatch_semaphore_t sem = _sg_mtl_sem;
    [_sg_mtl_cmd_buffer addCompletedHandler:^(id<MTLCommandBuffer> cmd_buffer) {
        dispatch_semaphore_signal(sem);
    }];
    [_sg_mtl_cmd_buffer commit];

    /* garbage-collect resources pending for release */
    _sg_mtl_garbage_collect(_sg_mtl_frame_index);

    /* rotate uniform buffer slot */
    if (++_sg_mtl_cur_frame_rotate_index >= _SG_MTL_NUM_INFLIGHT_FRAMES) {
        _sg_mtl_cur_frame_rotate_index = 0;
    }
    _sg_mtl_frame_index++;
    _sg_mtl_cur_ub_offset = 0;
    _sg_mtl_cur_ub_base_ptr = 0;
    _sg_mtl_cmd_buffer = nil;
}

_SOKOL_PRIVATE void _sg_apply_viewport(int x, int y, int w, int h, bool origin_top_left) {
    // FIXME
}

_SOKOL_PRIVATE void _sg_apply_scissor_rect(int x, int y, int w, int h, bool origin_top_left) {
    // FIXME
}

_SOKOL_PRIVATE void _sg_apply_draw_state(
    _sg_pipeline* pip,
    _sg_buffer** vbs, int num_vbs, _sg_buffer* ib,
    _sg_image** vs_imgs, int num_vs_imgs,
    _sg_image** fs_imgs, int num_fs_imgs)
{
    SOKOL_ASSERT(pip);
    // FIXME
}

_SOKOL_PRIVATE void _sg_apply_uniform_block(sg_shader_stage stage_index, int ub_index, const void* data, int num_bytes) {
    SOKOL_ASSERT(data && (num_bytes > 0));
    SOKOL_ASSERT((stage_index >= 0) && ((int)stage_index < SG_NUM_SHADER_STAGES));
    // FIXME
}

_SOKOL_PRIVATE void _sg_draw(int base_element, int num_elements, int num_instances) {
    // FIXME
}

_SOKOL_PRIVATE void _sg_update_buffer(_sg_buffer* buf, const void* data, int data_size) {
    SOKOL_ASSERT(buf && data && (data_size > 0));
    // FIXME
}

#ifdef __cplusplus
} /* extern "C" */
#endif

