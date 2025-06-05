/*
 * chr  - Andrew Chronister &
 * inso - Alex Baines
 *
 * 12.19.2019
 *
 * Updated linux layer for 4coder
 *
 */

// TOP

#include <stdio.h>

#define FPS 120
#define frame_useconds (Million(1) / FPS)
#define frame_nseconds (Billion(1) / FPS)
#define SLASH '/'
#define DLL "so"

#include "4coder_base_types.h"
#include "4coder_version.h"
#include "4coder_events.h"

#include "4coder_table.h"
#include "4coder_types.h"
#include "4coder_default_colors.h"
#include "4coder_system_types.h"
#include "4ed_font_interface.h"

#define STATIC_LINK_API
#include "generated/system_api.h"

#define STATIC_LINK_API
#include "generated/graphics_api.h"

#define STATIC_LINK_API
#include "generated/font_api.h"

#include "4ed_font_set.h"
#include "4ed_render_target.h"
#include "4coder_search_list.h"
#include "4ed.h"

#include "generated/system_api.cpp"
#include "generated/graphics_api.cpp"
#include "generated/font_api.cpp"

#include "4coder_base_types.cpp"
#include "4coder_stringf.cpp"
#include "4coder_events.cpp"
#include "4coder_hash_functions.cpp"
#include "4coder_table.cpp"
#include "4coder_log.cpp"

#include "4coder_hash_functions.cpp"
#include "4coder_system_allocator.cpp"
#include "4coder_malloc_allocator.cpp"
#include "4coder_codepoint_map.cpp"

#include "4ed_mem.cpp"
#include "4ed_font_set.cpp"
#include "4coder_search_list.cpp"
#include "4ed_font_provider_freetype.h"
#include "4ed_font_provider_freetype.cpp"

#include <dirent.h>
#include <dlfcn.h>
#include <fcntl.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <locale.h>
#include <errno.h>
#include <pthread.h>

#include <sys/epoll.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <sys/timerfd.h>
#include <sys/eventfd.h>
#include <sys/syscall.h>

#define Cursor XCursor
#undef function
#undef internal
#include <X11/Xlib.h>
#include <X11/Xutil.h>
#include <X11/cursorfont.h>
#include <X11/Xatom.h>
#include <X11/extensions/Xfixes.h>
#include <X11/XKBlib.h>
#include <X11/keysym.h>
#include <X11/Xresource.h>
#define function static
#undef Cursor

//#include <fontconfig/fontconfig.h>
#define internal static

#undef global
#include <wayland-client.h>
#include "third_party/wayland/xdg_shell.h"
#include "third_party/wayland/xdg_shell.c"
#include "third_party/wayland/xdg_decoration.h"
#include "third_party/wayland/xdg_decoration.c"
#include "third_party/wayland/fractional_scale.h"
#include "third_party/wayland/fractional_scale.c"
#include "third_party/wayland/viewporter.h"
#include "third_party/wayland/viewporter.c"
#include "third_party/wayland/tablet.h"
#include "third_party/wayland/tablet.c"
#include "third_party/wayland/cursor_shape.h"
#include "third_party/wayland/cursor_shape.c"
#define global static

#include <xkbcommon/xkbcommon.h>
#include <linux/input.h>

#include <wayland-egl.h>

#include <EGL/egl.h>
#include <EGL/eglext.h>

#ifdef INSO_DEBUG
#define LINUX_FN_DEBUG(fmt, ...) do { \
fprintf(stderr, "%s: " fmt "\n", __func__, ##__VA_ARGS__);\
} while (0)

// I want to see a message
#undef AssertBreak
#define AssertBreak(m) ({\
fprintf(stderr, "\n** ASSERTION FAILURE: %s:%d: %s\n\n", __FILE__, __LINE__, #m);\
*((volatile u64*)0) = 0xba771e70ad5;\
})
#else
#define LINUX_FN_DEBUG(...)
#endif

////////////////////////////////

global b32 log_os_enabled = false;
#define log_os(...) \
Stmnt( if (log_os_enabled){ fprintf(stdout, __VA_ARGS__); fflush(stdout); } )

////////////////////////////

struct Linux_Input_Chunk_Transient {
    Input_List event_list;
    b8 mouse_l_press;
    b8 mouse_l_release;
    b8 mouse_r_press;
    b8 mouse_r_release;
    i8 mouse_wheel;
    b8 trying_to_kill;
};

struct Linux_Input_Chunk_Persistent {
    Vec2_i32 mouse;
    Input_Modifier_Set_Fixed modifiers;
    b8 mouse_l;
    b8 mouse_r;
    b8 mouse_out_of_window;
};

struct Linux_Input_Chunk {
    Linux_Input_Chunk_Transient trans;
    Linux_Input_Chunk_Persistent pers;
};

struct Linux_Memory_Tracker_Node {
    Linux_Memory_Tracker_Node* prev;
    Linux_Memory_Tracker_Node* next;
    String_Const_u8 location;
    u64 size;
};

struct Linux_Vars {
    Thread_Context tctx;
    Arena frame_arena;
    
    Linux_Input_Chunk input;
    int xkb_event;
    int xkb_group; // active keyboard layout (0-3)
    KeyCode prev_filtered_key;
    
    Key_Mode key_mode;
    
    int epoll;
    int step_timer_fd;
    u64 last_step_time;
    
    Application_Mouse_Cursor cursor;
    i32 cursor_show;
    i32 prev_cursor_show;
    
    Node free_linux_objects;
    Node timer_objects;
    
    System_Mutex global_frame_mutex;
    pthread_mutex_t memory_tracker_mutex;
    Linux_Memory_Tracker_Node* memory_tracker_head;
    Linux_Memory_Tracker_Node* memory_tracker_tail;
    int memory_tracker_count;
    
    Arena clipboard_arena;
    String_Const_u8 clipboard_contents;
    b32 received_new_clipboard;
    b32 clipboard_catch_all;
    
    pthread_mutex_t audio_mutex;
    pthread_cond_t audio_cond;
    void* audio_ctx;
    Audio_Mix_Sources_Function* audio_src_func;
    Audio_Mix_Destination_Function* audio_dst_func;
    System_Thread audio_thread;
    
    Log_Function *log_string;
    
    struct
    {
        b32 WaitingForPresent;
        b32 GotInitialConfigure;
        
        f32 Scale;
        i32 Width;
        i32 Height;
        i32 BufferWidth;
        i32 BufferHeight;
        b32 IsFullscreen;
        
        u32 KeyboardEnterSerial;
        u32 LastPointerSerial;
        u32 LastPointerEnterSerial;
        
        xkb_state *XKBState;
        xkb_context *XKBContext;
        xkb_keymap *XKBKeymap;
        
        int RepeatRate;
        int RepeatDelay;
        int RepeatHandle;
        u32 RepeatKeyCode;
        Input_Modifier_Set_Fixed RepeatMods;
        
        wl_display *Display;
        wl_registry *Registry;
        wl_compositor *Compositor;
        wl_seat *Seat;
        wl_pointer *Pointer;
        wl_keyboard *Keyboard;
        xdg_wm_base *Shell;
        zxdg_decoration_manager_v1 *DecorationManager;
        wp_fractional_scale_manager_v1  *FractionalManager;
        wp_viewporter *Viewporter;
        wl_data_device_manager *DataDeviceManager;
        wp_cursor_shape_manager_v1 *CursorShapeManager;
        
        wl_egl_window *Window;
        wl_surface *Surface;
        xdg_surface *ShellSurface;
        xdg_toplevel *ShellToplevel;
        zxdg_toplevel_decoration_v1 *Decoration;
        wp_fractional_scale_v1 *Fractional;
        wp_viewport *Viewport;
        wl_data_device *DataDevice;
        wp_cursor_shape_device_v1 *CursorShapeDevice;
        wl_callback *FrameCallback;
        
        wl_registry_listener RegistryListener;
        wl_seat_listener SeatListener;
        wl_pointer_listener PointerListener;
        wl_keyboard_listener KeyboardListener;
        xdg_wm_base_listener ShellListener;
        xdg_surface_listener ShellSurfaceListener;
        xdg_toplevel_listener ShellToplevelListener;
        wp_fractional_scale_v1_listener FractionalListener;
        wl_callback_listener FrameCallbackListener;
        
        wl_data_device_listener DataDeviceListener;
        wl_data_offer_listener DataOfferListener;
        wl_data_source_listener DataSourceListener;
    } Wayland;
    
    struct
    {
        Display* dpy;
        Window win;
        
        b32 has_xfixes;
        int xfixes_selection_event;
        XIM xim;
        XIC xic;
        //FcConfig* fontconfig;
        XkbDescPtr xkb;
        
        XCursor xcursors[APP_MOUSE_CURSOR_COUNT];
        XCursor hidden_cursor;
        
        Atom atom_TARGETS;
        Atom atom_CLIPBOARD;
        Atom atom_UTF8_STRING;
        Atom atom__NET_WM_STATE;
        Atom atom__NET_WM_STATE_MAXIMIZED_HORZ;
        Atom atom__NET_WM_STATE_MAXIMIZED_VERT;
        Atom atom__NET_WM_STATE_FULLSCREEN;
        Atom atom__NET_WM_PING;
        Atom atom__NET_WM_WINDOW_TYPE;
        Atom atom__NET_WM_WINDOW_TYPE_NORMAL;
        Atom atom__NET_WM_PID;
        Atom atom_WM_DELETE_WINDOW;
    } X11;
    
    struct
    {
        EGLDisplay Display;
        EGLContext Context;
        EGLSurface Surface;
    } EGL;
};

global b32 GlobalRunning;
global Linux_Vars linuxvars;
global Render_Target render_target;
global Key_Code keycode_lookup_table_wayland[255];

////////////////////////////

// Defererencing an epoll_event's .data.ptr will always give one of these event types.

typedef i32 Epoll_Kind;
enum {
    EPOLL_STEP_TIMER,
    EPOLL_X11,
    EPOLL_X11_INTERNAL,
    EPOLL_CLI_PIPE,
    EPOLL_USER_TIMER,
    EPOLL_WAYLAND,
    EPOLL_XKB,
};

// Where per-event epoll data is not needed, .data.ptr will point to one of
// these static vars below.
// If per-event data is needed, container_of can be used on data.ptr
// to access the containing struct and all its other members.

internal Epoll_Kind epoll_tag_step_timer = EPOLL_STEP_TIMER;
internal Epoll_Kind epoll_tag_x11 = EPOLL_X11;
internal Epoll_Kind epoll_tag_x11_internal = EPOLL_X11_INTERNAL;
internal Epoll_Kind epoll_tag_cli_pipe = EPOLL_CLI_PIPE;
internal Epoll_Kind epoll_tag_wayland = EPOLL_WAYLAND;
internal Epoll_Kind epoll_tag_xkb = EPOLL_XKB;

////////////////////////////

typedef i32 Linux_Object_Kind;
enum {
    LinuxObjectKind_ERROR = 0,
    LinuxObjectKind_Timer = 1,
    LinuxObjectKind_Thread = 2,
    LinuxObjectKind_Mutex = 3,
    LinuxObjectKind_ConditionVariable = 4,
};

struct Linux_Object {
    Linux_Object_Kind kind;
    Node node;
    union {
        struct {
            int fd;
            Epoll_Kind epoll_tag;
        } timer;
        struct {
            pthread_t pthread;
            Thread_Function* proc;
            void* ptr;
        } thread;
        pthread_mutex_t mutex;
        pthread_cond_t condition_variable;
    };
};

Linux_Object*
handle_to_object(Plat_Handle ph){
    return *(Linux_Object**)&ph;
}

Plat_Handle
object_to_handle(Linux_Object* obj) {
    return *(Plat_Handle*)&obj;
}

internal Linux_Object*
linux_alloc_object(Linux_Object_Kind kind){
    Linux_Object* result = NULL;
    
    if (linuxvars.free_linux_objects.next != &linuxvars.free_linux_objects) {
        result = CastFromMember(Linux_Object, node, linuxvars.free_linux_objects.next);
    }
    
    if (result == NULL) {
        i32 count = 512;
        
        Linux_Object* objects = (Linux_Object*)system_memory_allocate(
                                                                      sizeof(Linux_Object) * count,
                                                                      file_name_line_number_lit_u8
                                                                      );
        
        objects[0].node.prev = &linuxvars.free_linux_objects;
        linuxvars.free_linux_objects.next = &objects[0].node;
        for (i32 i = 1; i < count; ++i) {
            objects[i - 1].node.next = &objects[i].node;
            objects[i].node.prev = &objects[i - 1].node;
        }
        objects[count - 1].node.next = &linuxvars.free_linux_objects;
        linuxvars.free_linux_objects.prev = &objects[count - 1].node;
        
        result = CastFromMember(Linux_Object, node, linuxvars.free_linux_objects.next);
    }
    
    Assert(result != 0);
    dll_remove(&result->node);
    block_zero_struct(result);
    result->kind = kind;
    return result;
}

internal void
linux_free_object(Linux_Object *object){
    if (object->node.next != 0){
        dll_remove(&object->node);
    }
    dll_insert(&linuxvars.free_linux_objects, &object->node);
}

////////////////////////////

internal int
linux_compare_file_infos(File_Info** a, File_Info** b) {
    b32 a_hidden = (*a)->file_name.str[0] == '.';
    b32 b_hidden = (*b)->file_name.str[0] == '.';
    
    // hidden files lower in list
    if(a_hidden != b_hidden) {
        return a_hidden - b_hidden;
    }
    
    // push_stringf seems to null terminate
    return strcoll((char*)(*a)->file_name.str, (char*)(*b)->file_name.str);
}

internal int
linux_system_get_file_list_filter(const struct dirent *dirent) {
    String_Const_u8 file_name = SCu8((u8*)dirent->d_name);
    if (string_match(file_name, string_u8_litexpr("."))) {
        return 0;
    }
    else if (string_match(file_name, string_u8_litexpr(".."))) {
        return 0;
    }
    return 1;
}

internal u64
linux_us_from_timespec(const struct timespec timespec) {
    return timespec.tv_nsec/Thousand(1) + Million(1) * timespec.tv_sec;
}

internal File_Attribute_Flag
linux_convert_file_attribute_flags(int mode) {
    File_Attribute_Flag result = {};
    MovFlag(mode, S_IFDIR, result, FileAttribute_IsDirectory);
    return result;
}

internal File_Attributes
linux_file_attributes_from_struct_stat(struct stat* file_stat) {
    File_Attributes result = {};
    result.size = file_stat->st_size;
    result.last_write_time = linux_us_from_timespec(file_stat->st_mtim);
    result.flags = linux_convert_file_attribute_flags(file_stat->st_mode);
    return(result);
}

internal void
linux_schedule_step(){
    u64 now  = system_now_time();
    u64 diff = (now - linuxvars.last_step_time);
    
    struct itimerspec its = {};
    timerfd_gettime(linuxvars.step_timer_fd, &its);
    
    if (diff > frame_useconds) {
        its.it_value.tv_nsec = 1;
        timerfd_settime(linuxvars.step_timer_fd, 0, &its, NULL);
    } else {
        if (its.it_value.tv_sec == 0 && its.it_value.tv_nsec == 0){
            its.it_value.tv_nsec = (frame_useconds - diff) * 1000UL;
            timerfd_settime(linuxvars.step_timer_fd, 0, &its, NULL);
        }
    }
}

enum wm_state_mode {
    WM_STATE_DEL = 0,
    WM_STATE_ADD = 1,
    WM_STATE_TOGGLE = 2,
};

internal void
linux_set_wm_state(Atom one, Atom two, enum wm_state_mode mode){
    //NOTE(inso): this will only work after the window has been mapped
    
    XEvent e = {};
    e.xany.type = ClientMessage;
    e.xclient.message_type = linuxvars.X11.atom__NET_WM_STATE;
    e.xclient.format = 32;
    e.xclient.window = linuxvars.X11.win;
    e.xclient.data.l[0] = mode;
    e.xclient.data.l[1] = one;
    e.xclient.data.l[2] = two;
    e.xclient.data.l[3] = 1L;
    
    XSendEvent(linuxvars.X11.dpy,
               RootWindow(linuxvars.X11.dpy, 0),
               0, SubstructureNotifyMask | SubstructureRedirectMask, &e);
}

internal void
linux_window_maximize(enum wm_state_mode mode){
    linux_set_wm_state(linuxvars.X11.atom__NET_WM_STATE_MAXIMIZED_HORZ, linuxvars.X11.atom__NET_WM_STATE_MAXIMIZED_VERT, mode);
}

internal void
linux_window_fullscreen(enum wm_state_mode mode) {
    linux_set_wm_state(linuxvars.X11.atom__NET_WM_STATE_FULLSCREEN, 0, mode);
}

internal int
linux_get_xsettings_dpi(Display* dpy, int screen){
    struct XSettingHeader {
        u8 type;
        u8 pad0;
        u16 name_len;
        char name[0];
    };
    
    struct XSettings {
        u8 byte_order;
        u8 pad[3];
        u32 serial;
        u32 num_settings;
    };
    
    enum { XSettingsTypeInt, XSettingsTypeString, XSettingsTypeColor };
    
    int dpi = -1;
    unsigned char* prop = NULL;
    char sel_buffer[64];
    struct XSettings* xs;
    const char* p;
    
    snprintf(sel_buffer, sizeof(sel_buffer), "_XSETTINGS_S%d", screen);
    
    Atom XSET_SEL = XInternAtom(dpy, sel_buffer, True);
    Atom XSET_SET = XInternAtom(dpy, "_XSETTINGS_SETTINGS", True);
    
    if (XSET_SEL == None || XSET_SET == None){
        //LOG("XSETTINGS unavailable.\n");
        return(dpi);
    }
    
    Window xset_win = XGetSelectionOwner(dpy, XSET_SEL);
    if (xset_win == None){
        // TODO(inso): listen for the ClientMessage about it becoming available?
        //             there's not much point atm if DPI scaling is only done at startup
        goto out;
    }
    
    {
        Atom type;
        int fmt;
        unsigned long pad, num;
        
        if (XGetWindowProperty(dpy, xset_win, XSET_SET, 0, 1024, False, XSET_SET, &type, &fmt, &num, &pad, &prop) != Success){
            //LOG("XSETTINGS: GetWindowProperty failed.\n");
            goto out;
        }
        
        if (fmt != 8){
            //LOG("XSETTINGS: Wrong format.\n");
            goto out;
        }
    }
    
    xs = (struct XSettings*)prop;
    p  = (char*)(xs + 1);
    
    if (xs->byte_order != 0){
        //LOG("FIXME: XSETTINGS not host byte order?\n");
        goto out;
    }
    
    for (int i = 0; i < xs->num_settings; ++i){
        struct XSettingHeader* h = (struct XSettingHeader*)p;
        
        p += sizeof(struct XSettingHeader);
        p += h->name_len;
        p += ((4 - (h->name_len & 3)) & 3);
        p += 4; // serial
        
        switch (h->type){
            case XSettingsTypeInt: {
                if (strncmp(h->name, "Xft/DPI", h->name_len) == 0){
                    dpi = *(i32*)p;
                    if (dpi != -1) dpi /= 1024;
                }
                p += 4;
            } break;
            
            case XSettingsTypeString: {
                u32 len = *(u32*)p;
                p += 4;
                p += len;
                p += ((4 - (len & 3)) & 3);
            } break;
            
            case XSettingsTypeColor: {
                p += 8;
            } break;
            
            default: {
                //LOG("XSETTINGS: Got invalid type...\n");
                goto out;
            } break;
        }
    }
    
    out:
    if (prop){
        XFree(prop);
    }
    
    return dpi;
}

internal void*
linux_thread_proc_start(void* arg) {
    Linux_Object* info = (Linux_Object*)arg;
    Assert(info->kind == LinuxObjectKind_Thread);
    info->thread.proc(info->thread.ptr);
    return NULL;
}

#include "linux_icon.h"
internal void
linux_set_icon(Display* d, Window w){
    Atom WM_ICON = XInternAtom(d, "_NET_WM_ICON", False);
    XChangeProperty(d, w, WM_ICON, XA_CARDINAL, 32, PropModeReplace, (unsigned char*)linux_icon, sizeof(linux_icon) / sizeof(long));
}

#include "linux_error_box.cpp"

function void
os_popup_error(char *title, char *message){
    system_error_box(message);
    exit(1);
}

////////////////////////////

#include "linux_4ed_functions.cpp"
#include "linux_4ed_audio.cpp"

////////////////////////////

#include <GL/gl.h>
#include "opengl/4ed_opengl_defines.h"
#define GL_FUNC(N,R,P) typedef R (CALL_CONVENTION N##_Function)P; N##_Function *N = 0;
#include "opengl/4ed_opengl_funcs.h"
#include "opengl/4ed_opengl_render.cpp"

internal
graphics_get_texture_sig(){
    return(gl__get_texture(dim, texture_kind));
}

internal
graphics_fill_texture_sig(){
    return(gl__fill_texture(texture_kind, texture, p, dim, data));
}

////////////////////////////

internal Face*
font_make_face(Arena* arena, Face_Description* description, f32 scale_factor) {
    
    Face_Description local_description = *description;
    String_Const_u8* name = &local_description.font.file_name;
    
    // if description->font.file_name is a relative path, prepend the font directory.
    if(string_get_character(*name, 0) != '/') {
        String_Const_u8 binary = system_get_path(arena, SystemPath_Binary);
        *name = push_u8_stringf(arena, "%.*sfonts/%.*s", string_expand(binary), string_expand(*name));
    }
    
    Face* result = ft__font_make_face(arena, &local_description, scale_factor);
    
    if(!result) {
        // is this fatal? 4ed.cpp:277 (caller) does not check for null.
        char msg[4096];
        snprintf(msg, sizeof(msg), "Unable to load font: %.*s", string_expand(*name));
        system_error_box(msg);
    }
    
    return(result);
}

////////////////////////////

internal b32
egl_init(void) {
    b32 result = false;
    
    linuxvars.EGL.Display = eglGetPlatformDisplay(EGL_PLATFORM_WAYLAND_KHR, linuxvars.Wayland.Display, 0);
    if(linuxvars.EGL.Display !=  EGL_NO_DISPLAY) {
        int egl_maj = 0;
        int egl_min = 0;
        if(eglInitialize(linuxvars.EGL.Display, &egl_maj, &egl_min) == EGL_TRUE) {
            if((egl_maj > 1) || (egl_maj == 1 && egl_min >= 5)) {
                eglBindAPI(EGL_OPENGL_API);
                EGLint ContextAttributes[] = {
                    EGL_CONTEXT_MAJOR_VERSION, 3,
                    EGL_CONTEXT_MINOR_VERSION, 2,
                    EGL_CONTEXT_OPENGL_PROFILE_MASK, EGL_CONTEXT_OPENGL_COMPATIBILITY_PROFILE_BIT,
#if GL_DEBUG_MODE
                    EGL_CONTEXT_OPENGL_DEBUG
#endif
                    EGL_NONE,
                };
                
                linuxvars.EGL.Context = eglCreateContext(linuxvars.EGL.Display, EGL_NO_CONFIG_KHR, EGL_NO_CONTEXT, ContextAttributes);
                
                if(linuxvars.EGL.Context != EGL_NO_CONTEXT) {
                    
#define GL_FUNC(f,R,P) f = (f##_Function *)eglGetProcAddress(#f);
#include "opengl/4ed_opengl_funcs.h"
                    
                    result = true;
                }
            }
        }
    }
    
    return result;
}

internal b32
egl_create_surface(void) {
    b32 result = false;
    
    Scratch_Block scratch(&linuxvars.tctx);
    
    EGLint config_attributes[] = {
        EGL_COLOR_BUFFER_TYPE, EGL_RGB_BUFFER,
        EGL_CONFORMANT, EGL_OPENGL_BIT,
        EGL_RENDERABLE_TYPE, EGL_OPENGL_BIT,
        EGL_SURFACE_TYPE, EGL_WINDOW_BIT,
        
        EGL_RED_SIZE, 8,
        EGL_GREEN_SIZE, 8,
        EGL_BLUE_SIZE, 8,
        EGL_ALPHA_SIZE, 8,
        
        EGL_DEPTH_SIZE, 24,
        EGL_STENCIL_SIZE, 8,
        
        EGL_NONE,
    };
    
    EGLAttrib surface_attributes[] = {
        EGL_GL_COLORSPACE, EGL_GL_COLORSPACE_SRGB,
        EGL_RENDER_BUFFER, EGL_BACK_BUFFER,
        
        // NOTE(maria): for wayland later
        EGL_PRESENT_OPAQUE_EXT, EGL_TRUE,
        
        EGL_NONE,
    };
    
    EGLint config_count = 0;
    eglChooseConfig(linuxvars.EGL.Display, config_attributes, 0, 0, &config_count);
    
    EGLConfig *configs = push_array(scratch, EGLConfig, config_count);
    eglChooseConfig(linuxvars.EGL.Display, config_attributes, configs, config_count, &config_count);
    
    for(int config_index = 0;
        config_index < config_count;
        ++config_index) {
        EGLConfig test_config = configs[config_index];
        EGLSurface test_surface = eglCreatePlatformWindowSurface(linuxvars.EGL.Display, test_config, linuxvars.Wayland.Window, surface_attributes);
        if(test_surface != EGL_NO_SURFACE) {
            linuxvars.EGL.Surface = test_surface;
            result = true;
            break;
        }
    }
    
    return result;
}

////////////////////////////

internal String_Const_u8
linux_filter_text(Arena* arena, u8* buf, int len) {
    u8* const result = push_array(arena, u8, len);
    u8* outp = result;
    
    for(int i = 0; i < len; ++i) {
        u8 c = buf[i];
        
        if(c == '\r') {
            *outp++ = '\n';
        } else if(c > 127 || (' ' <= c && c <= '~') || c == '\t') {
            *outp++ = c;
        }
    }
    
    return SCu8(result, outp - result);
}

internal void
LinuxHandleWaylandRegistryGlobal(void *UserData, wl_registry *Registry, u32 Name,
                                 const char *Interface, u32 Version)
{
    Linux_Vars *State = &linuxvars;
    
    if(strcmp(Interface, "wl_compositor") == 0)
    {
        State->Wayland.Compositor = (wl_compositor *)wl_registry_bind(Registry, Name, &wl_compositor_interface, 4);
    }
    if(strcmp(Interface, "wl_seat") == 0)
    {
        State->Wayland.Seat = (wl_seat *)wl_registry_bind(Registry, Name, &wl_seat_interface, 4);
    }
    else if(strcmp(Interface, "xdg_wm_base") == 0)
    {
        State->Wayland.Shell = (xdg_wm_base *)wl_registry_bind(Registry, Name, &xdg_wm_base_interface, 2);
    }
    else if(strcmp(Interface, "zxdg_decoration_manager_v1") == 0)
    {
        State->Wayland.DecorationManager = (zxdg_decoration_manager_v1 *)wl_registry_bind(Registry, Name, &zxdg_decoration_manager_v1_interface, 1);
    }
    else if(strcmp(Interface, "wp_fractional_scale_manager_v1") == 0)
    {
        State->Wayland.FractionalManager = (wp_fractional_scale_manager_v1 *)wl_registry_bind(Registry, Name, &wp_fractional_scale_manager_v1_interface, 1);
    }
    else if(strcmp(Interface, "wp_viewporter") == 0)
    {
        State->Wayland.Viewporter = (wp_viewporter *)wl_registry_bind(Registry, Name, &wp_viewporter_interface, 1);
    }
    else if(strcmp(Interface, "wl_data_device_manager") == 0)
    {
        State->Wayland.DataDeviceManager = (wl_data_device_manager *)wl_registry_bind(Registry, Name, &wl_data_device_manager_interface, 3);
    }
    else if(strcmp(Interface, "wp_cursor_shape_manager_v1") == 0)
    {
        State->Wayland.CursorShapeManager = (wp_cursor_shape_manager_v1 *)wl_registry_bind(Registry, Name, &wp_cursor_shape_manager_v1_interface, 1);
    }
}

internal void
LinuxHandleWaylandRegistryGlobalRemove(void *UserData, wl_registry *Registry, u32 Name)
{
}

internal void
LinuxHandleWaylandShellPing(void *UserData, xdg_wm_base *Shell, u32 Serial)
{
    xdg_wm_base_pong(Shell, Serial);
}

internal void
LinuxHandleWaylandShellSurfaceConfigure(void *UserData, xdg_surface *ShellSurface, u32 Serial)
{
    linuxvars.Wayland.GotInitialConfigure = true;
    xdg_surface_ack_configure(ShellSurface, Serial);
}

internal void
LinuxHandleWaylandShellToplevelClose(void *UserData, xdg_toplevel *ShellToplevel)
{
    // TODO(maria): wayland
    linuxvars.input.trans.trying_to_kill = true;
    
    decltype(linuxvars.Wayland) *State = &linuxvars.Wayland;
    State->WaitingForPresent = false;
    
    linux_schedule_step();
}

internal void
LinuxHandleWaylandShellToplevelConfigure(void *UserData, xdg_toplevel *ShellToplevel,
                                         i32 Width, i32 Height, wl_array *Flags)
{
    decltype(linuxvars.Wayland) *State = &linuxvars.Wayland;
    
    if(Width > 0 &&
       Height > 0)
    {
        State->Width = Width;
        State->Height = Height;
        State->BufferWidth = (i32)(State->Scale*State->Width+0.5f);
        State->BufferHeight = (i32)(State->Scale*State->Height+0.5f);
        
        wp_viewport_set_destination(linuxvars.Wayland.Viewport, linuxvars.Wayland.Width, linuxvars.Wayland.Height);
    }
    
    State->IsFullscreen = false;
    for(u32 *Flag = (u32 *)Flags->data;
        (u8 *)Flag < (u8 *)Flags->data + Flags->size;
        ++Flag)
    {
        if(*Flag == XDG_TOPLEVEL_STATE_FULLSCREEN)
        {
            State->IsFullscreen = true;
        }
    }
    
    linux_schedule_step();
}

internal void
LinuxHandleWaylandFractionalPreferredScale(void *UserData, wp_fractional_scale_v1 *Fractional, u32 Scale120)
{
    decltype(linuxvars.Wayland) *State = &linuxvars.Wayland;
    State->Scale = (f32)Scale120 / 120.0f;
    State->BufferWidth = (i32)(State->Scale*State->Width+0.5f);
    State->BufferHeight = (i32)(State->Scale*State->Height+0.5f);
    
    linux_schedule_step();
}

internal void
LinuxHandleWaylandSeatCapabilities(void *UserData, wl_seat *Seat, u32 Capabilities)
{
    decltype(linuxvars.Wayland) *State = &linuxvars.Wayland;
    
    b32 HasKeyboard = Capabilities & WL_SEAT_CAPABILITY_KEYBOARD;
    if(HasKeyboard && !State->Keyboard)
    {
        State->Keyboard = wl_seat_get_keyboard(Seat);
        wl_keyboard_add_listener(State->Keyboard, &State->KeyboardListener, 0);
    }
    else if(!HasKeyboard && State->Keyboard)
    {
        wl_keyboard_release(State->Keyboard);
        State->Keyboard = 0;
    }
    
    b32 HasPointer = Capabilities & WL_SEAT_CAPABILITY_POINTER;
    if(HasPointer && !State->Pointer)
    {
        State->Pointer = wl_seat_get_pointer(Seat);
        wl_pointer_add_listener(State->Pointer, &State->PointerListener, 0);
        State->CursorShapeDevice = wp_cursor_shape_manager_v1_get_pointer(State->CursorShapeManager, State->Pointer);
    }
    else if(!HasPointer && State->Pointer)
    {
        wl_pointer_release(State->Pointer);
        State->Pointer = 0;
    }
}

internal void
LinuxHandleWaylandSeatName(void *UserData, wl_seat *Seat, const char *Name)
{
}

internal void
LinuxHandleWaylandKeyboardKeymap(void *UserData, wl_keyboard *Keyboard, u32 Format,
                                 int KeymapHandle, u32 KeymapSize)
{
    decltype(linuxvars.Wayland) *State = &linuxvars.Wayland;
    
    u8 *KeymapContents = (u8 *)mmap(0, KeymapSize, PROT_READ,
                                    MAP_SHARED, KeymapHandle, 0);
    
    Assert(KeymapContents != MAP_FAILED);
    
    //s64 BytesRead = read(KeymapHandle, KeymapContents, KeymapSize);
    //close(KeymapHandle);
    
    xkb_keymap_unref(State->XKBKeymap);
    xkb_state_unref(State->XKBState);
    
    State->XKBKeymap = xkb_keymap_new_from_string(State->XKBContext, (char *)KeymapContents,
                                                  XKB_KEYMAP_FORMAT_TEXT_V1,
                                                  XKB_KEYMAP_COMPILE_NO_FLAGS);
    State->XKBState = xkb_state_new(State->XKBKeymap);
    
    munmap(KeymapContents, KeymapSize);
    close(KeymapHandle);
}

internal void
LinuxHandleWaylandKeyboardEnter(void *UserData, wl_keyboard *Keyboard, u32 Serial,
                                wl_surface *Surface, wl_array *Keys)
{
    decltype(linuxvars.Wayland) *State = &linuxvars.Wayland;
    State->KeyboardEnterSerial = Serial;
    
    linux_schedule_step();
}

internal void
LinuxHandleWaylandKeyboardLeave(void *UserData, wl_keyboard *Keyboard, u32 Serial,
                                wl_surface *Surface)
{
    decltype(linuxvars.Wayland) *State = &linuxvars.Wayland;
    
    itimerspec Timer = {};
    timerfd_settime(State->RepeatHandle, 0, &Timer, 0);
    
    linux_schedule_step();
}

internal void
LinuxProcessKeyboardInputDown(u32 KeyCode, Input_Modifier_Set_Fixed *Mods)
{
    decltype(linuxvars.Wayland) *State = &linuxvars.Wayland;
    
    char Buffer[128];
    i32 BufferFilled = xkb_state_key_get_utf8(State->XKBState, KeyCode+8, Buffer, sizeof(Buffer));
    
    KeyCode = keycode_lookup_table_wayland[KeyCode];
    
    Input_Event* key_event = NULL;
    if(KeyCode) {
        add_modifier(Mods, KeyCode);
        //printf(" push key %d\n", KeyCode);
        
        key_event = push_input_event(&linuxvars.frame_arena, &linuxvars.input.trans.event_list);
        key_event->kind = InputEventKind_KeyStroke;
        key_event->key.code = KeyCode;
        key_event->key.modifiers = copy_modifier_set(&linuxvars.frame_arena, Mods);
        key_event->key.flags = 0;
        
        //printf("KeyDown: %u\n", KeyCode);
    }
    
    Input_Event* text_event = NULL;
    if(BufferFilled) {
        String_Const_u8 str = linux_filter_text(&linuxvars.frame_arena, (u8 *)Buffer, BufferFilled);
        if(str.size) {
            text_event = push_input_event(&linuxvars.frame_arena, &linuxvars.input.trans.event_list);
            text_event->kind = InputEventKind_TextInsert;
            text_event->text.string = str;
            //printf("TextInput: length %d: %.*s\n", (int)str.size, (int)str.size, (char *)str.str);
        }
    }
    
    if(key_event && text_event)
    {
        key_event->key.first_dependent_text = text_event;
    }
    
    linux_schedule_step();
}

internal void
LinuxHandleWaylandKeyboardKey(void *UserData, wl_keyboard *Keyboard, u32 Serial,
                              u32 TimestampMS, u32 KeyCode, u32 KeyState)
{
    decltype(linuxvars.Wayland) *State = &linuxvars.Wayland;
    
    u32 OriginalKeyCode = KeyCode;
    State->KeyboardEnterSerial = Serial;
    
    Input_Modifier_Set_Fixed* mods = &linuxvars.input.pers.modifiers;
    
    b32 Shift = xkb_state_mod_name_is_active(State->XKBState, XKB_MOD_NAME_SHIFT, XKB_STATE_MODS_EFFECTIVE) == 1;
    b32 Control = xkb_state_mod_name_is_active(State->XKBState, XKB_MOD_NAME_CTRL, XKB_STATE_MODS_EFFECTIVE) == 1;
    b32 Caps = xkb_state_mod_name_is_active(State->XKBState, XKB_MOD_NAME_CAPS, XKB_STATE_MODS_EFFECTIVE) == 1;
    b32 Alt = xkb_state_mod_name_is_active(State->XKBState, XKB_MOD_NAME_ALT, XKB_STATE_MODS_EFFECTIVE) == 1;
    
    set_modifier(mods, KeyCode_Shift, Shift);
    set_modifier(mods, KeyCode_Control, Control);
    set_modifier(mods, KeyCode_CapsLock, Caps);
    set_modifier(mods, KeyCode_Alt, Alt);
    
    char Buffer[128];
    i32 BufferFilled = xkb_state_key_get_utf8(State->XKBState, KeyCode+8, Buffer, sizeof(Buffer));
    
    KeyCode = keycode_lookup_table_wayland[KeyCode];
    
    itimerspec Timer = {};
    if(KeyState == WL_KEYBOARD_KEY_STATE_PRESSED)
    {
        LinuxProcessKeyboardInputDown(OriginalKeyCode, mods);
        
        if(State->RepeatRate > 0 && xkb_keymap_key_repeats(State->XKBKeymap, OriginalKeyCode+8))
        {
            State->RepeatKeyCode = OriginalKeyCode;
            State->RepeatMods = *mods;
            
            Timer.it_interval.tv_sec = State->RepeatRate / 1000;
            Timer.it_interval.tv_nsec = (State->RepeatRate % 1000) * 1000000;
            Timer.it_value.tv_sec = State->RepeatDelay / 1000;
            Timer.it_value.tv_nsec = (State->RepeatDelay % 1000) * 1000000;
        }
    }
    else if(KeyState == WL_KEYBOARD_KEY_STATE_RELEASED)
    {
        Input_Event* key_event = NULL;
        if(KeyCode) {
            remove_modifier(mods, KeyCode);
            key_event = push_input_event(&linuxvars.frame_arena, &linuxvars.input.trans.event_list);
            key_event->kind = InputEventKind_KeyRelease;
            key_event->key.code = KeyCode;
            key_event->key.modifiers = copy_modifier_set(&linuxvars.frame_arena, mods);
        }
    }
    
    if(State->RepeatKeyCode == OriginalKeyCode)
    {
        timerfd_settime(State->RepeatHandle, 0, &Timer, 0);
    }
    
    linux_schedule_step();
}

internal void
LinuxHandleWaylandKeyboardModifiers(void *UserData, wl_keyboard *Keyboard, u32 Serial,
                                    u32 ModsDepressed, u32 ModsLatched, u32 ModsLocked,
                                    u32 Group)
{
    decltype(linuxvars.Wayland) *State = &linuxvars.Wayland;
    xkb_state_update_mask(State->XKBState, ModsDepressed, ModsLocked, ModsLocked, 0, 0, Group);
    
    linux_schedule_step();
}

internal void
LinuxHandleWaylandKeyboardRepeatInfo(void *UserData, wl_keyboard *Keyboard, i32 Rate, i32 Delay)
{
    decltype(linuxvars.Wayland) *State = &linuxvars.Wayland;
    
    State->RepeatRate = Rate;
    State->RepeatDelay = Delay;
    
    linux_schedule_step();
}

internal void
LinuxHandleWaylandPointerEnter(void *UserData, wl_pointer *Pointer, u32 Serial, wl_surface *Surface,
                               wl_fixed_t SurfaceX, wl_fixed_t SurfaceY)
{
    decltype(linuxvars.Wayland) *State = &linuxvars.Wayland;
    
    u32 Shape = WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_DEFAULT;
    switch(linuxvars.cursor)
    {
        case APP_MOUSE_CURSOR_IBEAM:
        {
            Shape = WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_TEXT;
        } break;
        
        case APP_MOUSE_CURSOR_LEFTRIGHT:
        {
            Shape = WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_EW_RESIZE;
        } break;
        
        case APP_MOUSE_CURSOR_UPDOWN:
        {
            Shape = WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_NS_RESIZE;
        } break;
    }
    
    linuxvars.Wayland.LastPointerSerial = Serial;
    linuxvars.Wayland.LastPointerEnterSerial = Serial;
    wp_cursor_shape_device_v1_set_shape(State->CursorShapeDevice, Serial, Shape);
    
    f32 X = State->Scale*wl_fixed_to_double(SurfaceX);
    f32 Y = State->Scale*wl_fixed_to_double(SurfaceY);
    linuxvars.input.pers.mouse = {(i32)(X + 0.5f), (i32)(Y + 0.5f)};
    
    linux_schedule_step();
}

internal void
LinuxHandleWaylandPointerLeave(void *UserData, wl_pointer *Pointer, u32 Serial, wl_surface *Surface)
{
    linux_schedule_step();
}

internal void
LinuxHandleWaylandPointerMotion(void *UserData, wl_pointer *Pointer, u32 TimestampMS,
                                wl_fixed_t SurfaceX, wl_fixed_t SurfaceY)
{
    decltype(linuxvars.Wayland) *State = &linuxvars.Wayland;
    f32 X = State->Scale*wl_fixed_to_double(SurfaceX);
    f32 Y = State->Scale*wl_fixed_to_double(SurfaceY);
    linuxvars.input.pers.mouse = {(i32)(X + 0.5f), (i32)(Y + 0.5f)};
    
    linux_schedule_step();
}

internal void
LinuxHandleWaylandPointerButton(void *UserData, wl_pointer *Pointer, u32 Serial, u32 TimestampMS,
                                u32 Button, u32 ButtonState)
{
    linuxvars.Wayland.LastPointerSerial = Serial;
    
    b32 Pressed = ButtonState == 1;
    
    switch(Button)
    {
        case BTN_LEFT:
        {
            linuxvars.input.pers.mouse_l = Pressed;
            if(Pressed)
            {
                linuxvars.input.trans.mouse_l_press = true;
            }
            else
            {
                linuxvars.input.trans.mouse_l_release = true;
            }
        } break;
        
        case BTN_RIGHT:
        {
            linuxvars.input.pers.mouse_r = Pressed;
            if(Pressed)
            {
                linuxvars.input.trans.mouse_r_press = true;
            }
            else
            {
                linuxvars.input.trans.mouse_l_release = true;
            }
        } break;
    }
    
    linux_schedule_step();
}

internal void
LinuxHandleWaylandPointerAxis(void *UserData, wl_pointer *Pointer, u32 TimestampMS, u32 Axis,
                              wl_fixed_t Value)
{
    if(Axis == 0){linuxvars.input.trans.mouse_wheel = -10*wl_fixed_to_double(Value);};
    linux_schedule_step();
}

internal void
LinuxHandleWaylandDataDeviceDataEnter(void *UserData, wl_data_device *DataDevice, u32 Serial,
                                      wl_surface *Surface, wl_fixed_t SurfaceX, wl_fixed_t SurfaceY,
                                      wl_data_offer *DataOffer)
{
}

internal void
LinuxHandleWaylandDataDeviceDataLeave(void *UserData, wl_data_device *DataDevice)
{
}

internal void
LinuxHandleWaylandDataDeviceDataMotion(void *UserData, wl_data_device *DataDevice, u32 TimestampMS,
                                       wl_fixed_t SurfaceX, wl_fixed_t SurfaceY)
{
}

internal void
LinuxHandleWaylandDataDeviceDataDrop(void *UserData, wl_data_device *DataDevice)
{
}

internal void
LinuxHandleWaylandDataDeviceDataOffer(void *UserData, wl_data_device *DataDevice, wl_data_offer *DataOffer)
{
    //wl_data_offer_add_listener(DataOffer, &linuxvars.Wayland.DataOfferListener, 0);
}

internal void
LinuxHandleWaylandDataDeviceSelection(void *UserData, wl_data_device *DataDevice, wl_data_offer *DataOffer)
{
    decltype(linuxvars.Wayland) *Wayland = &linuxvars.Wayland;
    
    if(DataOffer)
    {
        int ReceiveClipboardHandles[2];
        pipe(ReceiveClipboardHandles);
        
        wl_data_offer_receive(DataOffer, "text/plain", ReceiveClipboardHandles[1]);
        close(ReceiveClipboardHandles[1]);
        wl_display_roundtrip(Wayland->Display);
        
        Scratch_Block scratch(&linuxvars.tctx);
        int MaxContentsSize = 32 << 20;
        int ContentsSize = 0;
        u8 *Contents = push_array(scratch, u8, MaxContentsSize);
        
        while(true)
        {
            int BytesRead = read(ReceiveClipboardHandles[0], Contents + ContentsSize, sizeof(MaxContentsSize - ContentsSize));
            if(BytesRead <= 0)
            {
                break;
            }
            
            ContentsSize += BytesRead;
            if(ContentsSize == MaxContentsSize)
            {
                // TODO(maria): handle this properly
                break;
            }
        }
        
        linalloc_clear(&linuxvars.clipboard_arena);
        linuxvars.clipboard_contents = push_string_copy(&linuxvars.clipboard_arena, SCu8(Contents, ContentsSize));
        linuxvars.received_new_clipboard = true;
        //linux_schedule_step();
        
        close(ReceiveClipboardHandles[0]);
        wl_data_offer_destroy(DataOffer);
    }
}

internal void
LinuxHandleWaylandDataSourceTarget(void *UserData, wl_data_source *DataSource, const char *MimeType)
{
}

internal void
LinuxHandleWaylandDataSourceSend(void *UserData, wl_data_source *DataSource,
                                 const char *MimeType, int FileHandle)
{
    if(DataSource && FileHandle >= 0)
    {
        write(FileHandle, linuxvars.clipboard_contents.str, linuxvars.clipboard_contents.size);
        close(FileHandle);
    }
}

internal void
LinuxHandleWaylandDataSourceCancelled(void *UserData, wl_data_source *DataSource)
{
    wl_data_source_destroy(DataSource);
}

internal void
LinuxHandleWaylandFrameCallbackDone(void *UserData, wl_callback *FrameCallback, u32 TimestampMS)
{
    linuxvars.Wayland.WaitingForPresent = false;
    wl_callback_destroy(FrameCallback);
}

internal void
linux_wayland_init(int argc, char** argv, Plat_Settings* settings) {
    decltype(linuxvars.Wayland) *Wayland = &linuxvars.Wayland;
    
    keycode_lookup_table_wayland[KEY_A] = KeyCode_A;
    keycode_lookup_table_wayland[KEY_B] = KeyCode_B;
    keycode_lookup_table_wayland[KEY_C] = KeyCode_C;
    keycode_lookup_table_wayland[KEY_D] = KeyCode_D;
    keycode_lookup_table_wayland[KEY_E] = KeyCode_E;
    keycode_lookup_table_wayland[KEY_F] = KeyCode_F;
    keycode_lookup_table_wayland[KEY_G] = KeyCode_G;
    keycode_lookup_table_wayland[KEY_H] = KeyCode_H;
    keycode_lookup_table_wayland[KEY_I] = KeyCode_I;
    keycode_lookup_table_wayland[KEY_J] = KeyCode_J;
    keycode_lookup_table_wayland[KEY_K] = KeyCode_K;
    keycode_lookup_table_wayland[KEY_L] = KeyCode_L;
    keycode_lookup_table_wayland[KEY_M] = KeyCode_M;
    keycode_lookup_table_wayland[KEY_N] = KeyCode_N;
    keycode_lookup_table_wayland[KEY_O] = KeyCode_O;
    keycode_lookup_table_wayland[KEY_P] = KeyCode_P;
    keycode_lookup_table_wayland[KEY_Q] = KeyCode_Q;
    keycode_lookup_table_wayland[KEY_R] = KeyCode_R;
    keycode_lookup_table_wayland[KEY_S] = KeyCode_S;
    keycode_lookup_table_wayland[KEY_T] = KeyCode_T;
    keycode_lookup_table_wayland[KEY_U] = KeyCode_U;
    keycode_lookup_table_wayland[KEY_V] = KeyCode_V;
    keycode_lookup_table_wayland[KEY_W] = KeyCode_W;
    keycode_lookup_table_wayland[KEY_X] = KeyCode_X;
    keycode_lookup_table_wayland[KEY_Y] = KeyCode_Y;
    keycode_lookup_table_wayland[KEY_Z] = KeyCode_Z;
    
    keycode_lookup_table_wayland[KEY_0] = KeyCode_0;
    keycode_lookup_table_wayland[KEY_1] = KeyCode_1;
    keycode_lookup_table_wayland[KEY_2] = KeyCode_2;
    keycode_lookup_table_wayland[KEY_3] = KeyCode_3;
    keycode_lookup_table_wayland[KEY_4] = KeyCode_4;
    keycode_lookup_table_wayland[KEY_5] = KeyCode_5;
    keycode_lookup_table_wayland[KEY_6] = KeyCode_6;
    keycode_lookup_table_wayland[KEY_7] = KeyCode_7;
    keycode_lookup_table_wayland[KEY_8] = KeyCode_8;
    keycode_lookup_table_wayland[KEY_9] = KeyCode_9;
    
    keycode_lookup_table_wayland[KEY_SPACE] = KeyCode_Space;
    keycode_lookup_table_wayland[KEY_GRAVE] = KeyCode_Tick;
    keycode_lookup_table_wayland[KEY_MINUS] = KeyCode_Minus;
    keycode_lookup_table_wayland[KEY_EQUAL] = KeyCode_Equal;
    keycode_lookup_table_wayland[KEY_LEFTBRACE] = KeyCode_LeftBracket;
    keycode_lookup_table_wayland[KEY_RIGHTBRACE] = KeyCode_RightBracket;
    keycode_lookup_table_wayland[KEY_SEMICOLON] = KeyCode_Semicolon;
    keycode_lookup_table_wayland[KEY_APOSTROPHE] = KeyCode_Quote;
    keycode_lookup_table_wayland[KEY_COMMA] = KeyCode_Comma;
    keycode_lookup_table_wayland[KEY_DOT] = KeyCode_Period;
    keycode_lookup_table_wayland[KEY_COMMA] = KeyCode_Comma;
    keycode_lookup_table_wayland[KEY_SLASH] = KeyCode_ForwardSlash;
    keycode_lookup_table_wayland[KEY_BACKSLASH] = KeyCode_BackwardSlash;
    
    keycode_lookup_table_wayland[KEY_TAB] = KeyCode_Tab;
    keycode_lookup_table_wayland[KEY_PAUSE] = KeyCode_Pause;
    keycode_lookup_table_wayland[KEY_ESC] = KeyCode_Escape;
    
    keycode_lookup_table_wayland[KEY_UP] = KeyCode_Up;
    keycode_lookup_table_wayland[KEY_DOWN] = KeyCode_Down;
    keycode_lookup_table_wayland[KEY_LEFT] = KeyCode_Left;
    keycode_lookup_table_wayland[KEY_RIGHT] = KeyCode_Right;
    
    keycode_lookup_table_wayland[KEY_BACKSPACE] = KeyCode_Backspace;
    keycode_lookup_table_wayland[KEY_ENTER] = KeyCode_Return;
    
    keycode_lookup_table_wayland[KEY_DELETE] = KeyCode_Delete;
    keycode_lookup_table_wayland[KEY_INSERT] = KeyCode_Insert;
    keycode_lookup_table_wayland[KEY_HOME] = KeyCode_Home;
    keycode_lookup_table_wayland[KEY_END] = KeyCode_End;
    keycode_lookup_table_wayland[KEY_PAGEUP] = KeyCode_PageUp;
    keycode_lookup_table_wayland[KEY_PAGEDOWN] = KeyCode_PageDown;
    
    keycode_lookup_table_wayland[KEY_CAPSLOCK] = KeyCode_CapsLock;
    keycode_lookup_table_wayland[KEY_NUMLOCK] = KeyCode_NumLock;
    keycode_lookup_table_wayland[KEY_SCROLLLOCK] = KeyCode_ScrollLock;
    keycode_lookup_table_wayland[KEY_MENU] = KeyCode_Menu;
    
    keycode_lookup_table_wayland[KEY_LEFTSHIFT] = KeyCode_Shift;
    keycode_lookup_table_wayland[KEY_RIGHTSHIFT] = KeyCode_Shift;
    
    keycode_lookup_table_wayland[KEY_LEFTCTRL] = KeyCode_Control;
    keycode_lookup_table_wayland[KEY_RIGHTCTRL] = KeyCode_Control;
    
    keycode_lookup_table_wayland[KEY_LEFTALT] = KeyCode_Alt;
    keycode_lookup_table_wayland[KEY_RIGHTALT] = KeyCode_Alt;
    
    keycode_lookup_table_wayland[KEY_F1] = KeyCode_F1;
    keycode_lookup_table_wayland[KEY_F2] = KeyCode_F2;
    keycode_lookup_table_wayland[KEY_F3] = KeyCode_F3;
    keycode_lookup_table_wayland[KEY_F4] = KeyCode_F4;
    keycode_lookup_table_wayland[KEY_F5] = KeyCode_F5;
    keycode_lookup_table_wayland[KEY_F6] = KeyCode_F6;
    keycode_lookup_table_wayland[KEY_F7] = KeyCode_F7;
    keycode_lookup_table_wayland[KEY_F8] = KeyCode_F8;
    
    keycode_lookup_table_wayland[KEY_F9] = KeyCode_F9;
    keycode_lookup_table_wayland[KEY_F10] = KeyCode_F10;
    keycode_lookup_table_wayland[KEY_F11] = KeyCode_F11;
    keycode_lookup_table_wayland[KEY_F12] = KeyCode_F12;
    keycode_lookup_table_wayland[KEY_F13] = KeyCode_F13;
    keycode_lookup_table_wayland[KEY_F14] = KeyCode_F14;
    keycode_lookup_table_wayland[KEY_F15] = KeyCode_F15;
    keycode_lookup_table_wayland[KEY_F16] = KeyCode_F16;
    
    keycode_lookup_table_wayland[KEY_F17] = KeyCode_F17;
    keycode_lookup_table_wayland[KEY_F18] = KeyCode_F18;
    keycode_lookup_table_wayland[KEY_F19] = KeyCode_F19;
    keycode_lookup_table_wayland[KEY_F20] = KeyCode_F20;
    keycode_lookup_table_wayland[KEY_F21] = KeyCode_F21;
    keycode_lookup_table_wayland[KEY_F22] = KeyCode_F22;
    keycode_lookup_table_wayland[KEY_F23] = KeyCode_F23;
    keycode_lookup_table_wayland[KEY_F24] = KeyCode_F24;
    
    Wayland->Scale = 2.0f;
    Wayland->Width = Wayland->BufferWidth = 800;
    Wayland->Height = Wayland->BufferHeight = 600;
    
#undef global
    Wayland->RegistryListener.global = LinuxHandleWaylandRegistryGlobal;
    Wayland->RegistryListener.global_remove = LinuxHandleWaylandRegistryGlobalRemove;
    Wayland->SeatListener.capabilities = LinuxHandleWaylandSeatCapabilities;
    Wayland->SeatListener.name = LinuxHandleWaylandSeatName;
    Wayland->KeyboardListener.keymap = LinuxHandleWaylandKeyboardKeymap;
    Wayland->KeyboardListener.enter = LinuxHandleWaylandKeyboardEnter;
    Wayland->KeyboardListener.leave = LinuxHandleWaylandKeyboardLeave;
    Wayland->KeyboardListener.key = LinuxHandleWaylandKeyboardKey;
    Wayland->KeyboardListener.modifiers = LinuxHandleWaylandKeyboardModifiers;
    Wayland->KeyboardListener.repeat_info = LinuxHandleWaylandKeyboardRepeatInfo;
    Wayland->PointerListener.enter = LinuxHandleWaylandPointerEnter;
    Wayland->PointerListener.leave = LinuxHandleWaylandPointerLeave;
    Wayland->PointerListener.motion = LinuxHandleWaylandPointerMotion;
    Wayland->PointerListener.button = LinuxHandleWaylandPointerButton;
    Wayland->PointerListener.axis = LinuxHandleWaylandPointerAxis;
    Wayland->ShellListener.ping = LinuxHandleWaylandShellPing;
    Wayland->ShellSurfaceListener.configure = LinuxHandleWaylandShellSurfaceConfigure;
    Wayland->ShellToplevelListener.close = LinuxHandleWaylandShellToplevelClose;
    Wayland->ShellToplevelListener.configure = LinuxHandleWaylandShellToplevelConfigure;
    Wayland->FractionalListener.preferred_scale = LinuxHandleWaylandFractionalPreferredScale;
    Wayland->DataDeviceListener.data_offer = LinuxHandleWaylandDataDeviceDataOffer;
    Wayland->DataDeviceListener.enter = LinuxHandleWaylandDataDeviceDataEnter;
    Wayland->DataDeviceListener.leave = LinuxHandleWaylandDataDeviceDataLeave;
    Wayland->DataDeviceListener.motion = LinuxHandleWaylandDataDeviceDataMotion;
    Wayland->DataDeviceListener.drop = LinuxHandleWaylandDataDeviceDataDrop;
    Wayland->DataDeviceListener.selection = LinuxHandleWaylandDataDeviceSelection;
    Wayland->DataSourceListener.target = LinuxHandleWaylandDataSourceTarget;
    Wayland->DataSourceListener.send = LinuxHandleWaylandDataSourceSend;
    Wayland->DataSourceListener.cancelled = LinuxHandleWaylandDataSourceCancelled;
    Wayland->FrameCallbackListener.done = LinuxHandleWaylandFrameCallbackDone;
#define global static
    
    Wayland->XKBContext = xkb_context_new(XKB_CONTEXT_NO_FLAGS);
    Wayland->RepeatHandle = timerfd_create(CLOCK_MONOTONIC, TFD_CLOEXEC|TFD_NONBLOCK);
    
    Wayland->Display = wl_display_connect(0);
    Wayland->Registry = wl_display_get_registry(Wayland->Display);
    wl_registry_add_listener(Wayland->Registry, &Wayland->RegistryListener, 0);
    
    wl_display_roundtrip(Wayland->Display);
    
    wl_seat_add_listener(linuxvars.Wayland.Seat, &linuxvars.Wayland.SeatListener, 0);
    xdg_wm_base_add_listener(linuxvars.Wayland.Shell, &linuxvars.Wayland.ShellListener, 0);
    wl_display_roundtrip(Wayland->Display);
    
    Wayland->DataDevice = wl_data_device_manager_get_data_device(Wayland->DataDeviceManager, Wayland->Seat);
    wl_data_device_add_listener(Wayland->DataDevice, &Wayland->DataDeviceListener, 0);
    
    Wayland->Surface = wl_compositor_create_surface(Wayland->Compositor);
    
    Wayland->ShellSurface = xdg_wm_base_get_xdg_surface(Wayland->Shell, Wayland->Surface);
    xdg_surface_add_listener(Wayland->ShellSurface, &Wayland->ShellSurfaceListener, 0);
    
    Wayland->ShellToplevel = xdg_surface_get_toplevel(Wayland->ShellSurface);
    xdg_toplevel_add_listener(Wayland->ShellToplevel, &Wayland->ShellToplevelListener, 0);
    
    xdg_toplevel_set_min_size(Wayland->ShellToplevel, 50, 50);
    xdg_toplevel_set_title(Wayland->ShellToplevel, WINDOW_NAME);
    xdg_toplevel_set_app_id(Wayland->ShellToplevel, "4coder");
    
    Wayland->Decoration = zxdg_decoration_manager_v1_get_toplevel_decoration(Wayland->DecorationManager, Wayland->ShellToplevel);
    zxdg_toplevel_decoration_v1_set_mode(Wayland->Decoration, ZXDG_TOPLEVEL_DECORATION_V1_MODE_SERVER_SIDE);
    
    Wayland->Viewport = wp_viewporter_get_viewport(Wayland->Viewporter, Wayland->Surface);
    wp_viewport_set_destination(Wayland->Viewport, Wayland->Width, Wayland->Height);
    
    Wayland->Fractional = wp_fractional_scale_manager_v1_get_fractional_scale(Wayland->FractionalManager, Wayland->Surface);
    wp_fractional_scale_v1_add_listener(Wayland->Fractional, &Wayland->FractionalListener, 0);
    
    wl_surface_commit(Wayland->Surface);
    while(!Wayland->GotInitialConfigure)
    {
        wl_display_dispatch(Wayland->Display);
    }
    
    Wayland->Window = wl_egl_window_create(Wayland->Surface, Wayland->BufferWidth, Wayland->BufferHeight);
    
    // TEMP
    render_target.width = Wayland->BufferWidth;
    render_target.height = Wayland->BufferHeight;
    
    if (settings->maximize_window){
        xdg_toplevel_set_maximized(Wayland->ShellToplevel);
    } else if (settings->fullscreen_window){
        xdg_toplevel_set_fullscreen(Wayland->ShellToplevel, 0);
    }
    
    wl_surface_commit(Wayland->Surface);
    
    if (!egl_init()){
        system_error_box("Your EGL version is too old. EGL 1.5+ is required.");
    }
    
    if(!egl_create_surface()) {
        system_error_box("Unable to create EGL surface.");
    }
    
    eglMakeCurrent(linuxvars.EGL.Display, linuxvars.EGL.Surface, linuxvars.EGL.Surface, linuxvars.EGL.Context);
    eglSwapInterval(linuxvars.EGL.Display, 0);
}

////////////////////////////

internal void
linux_x11_init(int argc, char** argv, Plat_Settings* settings) {
    
    Display* dpy = XOpenDisplay(0);
    if (!dpy){
        fprintf(stderr, "FATAL: Cannot open X11 Display!\n");
        exit(1);
    }
    
    linuxvars.X11.dpy = dpy;
    
#define LOAD_ATOM(x) linuxvars.X11.atom_##x = XInternAtom(linuxvars.X11.dpy, #x, False);
    
    LOAD_ATOM(TARGETS);
    LOAD_ATOM(CLIPBOARD);
    LOAD_ATOM(UTF8_STRING);
    LOAD_ATOM(_NET_WM_STATE);
    LOAD_ATOM(_NET_WM_STATE_MAXIMIZED_HORZ);
    LOAD_ATOM(_NET_WM_STATE_MAXIMIZED_VERT);
    LOAD_ATOM(_NET_WM_STATE_FULLSCREEN);
    LOAD_ATOM(_NET_WM_PING);
    LOAD_ATOM(_NET_WM_WINDOW_TYPE);
    LOAD_ATOM(_NET_WM_WINDOW_TYPE_NORMAL);
    LOAD_ATOM(_NET_WM_PID);
    LOAD_ATOM(WM_DELETE_WINDOW);
    
#undef LOAD_ATOM
    
    if (!egl_init()){
        system_error_box("Your EGL version is too old. EGL 1.5+ is required.");
    }
    
    // TODO: window size
#define WINDOW_W_DEFAULT 800
#define WINDOW_H_DEFAULT 600
    int w = WINDOW_W_DEFAULT;
    int h = WINDOW_H_DEFAULT;
    
    // TEMP
    render_target.width = w;
    render_target.height = h;
    
    XSetWindowAttributes swa = {};
    swa.backing_store = WhenMapped;
    swa.event_mask = StructureNotifyMask;
    swa.bit_gravity = NorthWestGravity;
    
    u32 CWflags = CWBackingStore|CWBitGravity|CWBackPixel|CWBorderPixel|CWColormap|CWEventMask;
    linuxvars.X11.win = XCreateWindow(dpy, DefaultRootWindow(dpy), 0, 0, w, h, 0, CopyFromParent, InputOutput, CopyFromParent, CWflags, &swa);
    
    if (!linuxvars.X11.win){
        system_error_box("XCreateWindow failed. Make sure your display is set up correctly.");
    }
    
    //NOTE(inso): Set the window's type to normal
    XChangeProperty(linuxvars.X11.dpy, linuxvars.X11.win, linuxvars.X11.atom__NET_WM_WINDOW_TYPE, XA_ATOM, 32, PropModeReplace, (unsigned char*)&linuxvars.X11.atom__NET_WM_WINDOW_TYPE_NORMAL, 1);
    
    //NOTE(inso): window managers want the PID as a window property for some reason.
    pid_t pid = getpid();
    XChangeProperty(linuxvars.X11.dpy, linuxvars.X11.win, linuxvars.X11.atom__NET_WM_PID, XA_CARDINAL, 32, PropModeReplace, (unsigned char*)&pid, 1);
    
    //NOTE(inso): set wm properties
    XStoreName(linuxvars.X11.dpy, linuxvars.X11.win, WINDOW_NAME);
    
    XSizeHints *sz_hints = XAllocSizeHints();
    XWMHints   *wm_hints = XAllocWMHints();
    XClassHint *cl_hints = XAllocClassHint();
    
    sz_hints->flags = PMinSize | PMaxSize | PWinGravity;
    
    sz_hints->min_width = 50;
    sz_hints->min_height = 50;
    
    sz_hints->max_width = sz_hints->max_height = (1UL << 16UL);
    sz_hints->win_gravity = NorthWestGravity;
    
    if (settings->set_window_pos){
        sz_hints->flags |= USPosition;
        sz_hints->x = settings->window_x;
        sz_hints->y = settings->window_y;
    }
    
    wm_hints->flags |= InputHint | StateHint;
    wm_hints->input = True;
    wm_hints->initial_state = NormalState;
    
    cl_hints->res_name = "4coder";
    cl_hints->res_class = "4coder";
    
    char* win_name_list[] = { WINDOW_NAME };
    XTextProperty win_name;
    XStringListToTextProperty(win_name_list, 1, &win_name);
    
    XSetWMProperties(linuxvars.X11.dpy, linuxvars.X11.win, &win_name, NULL, argv, argc, sz_hints, wm_hints, cl_hints);
    
    XFree(win_name.value);
    XFree(sz_hints);
    XFree(wm_hints);
    XFree(cl_hints);
    
    linux_set_icon(linuxvars.X11.dpy, linuxvars.X11.win);
    
    // NOTE(inso): make the window visible
    XMapWindow(linuxvars.X11.dpy, linuxvars.X11.win);
    
    if(!egl_create_surface()) {
        system_error_box("Unable to create EGL surface.");
    }
    
    eglMakeCurrent(linuxvars.EGL.Display, linuxvars.EGL.Surface, linuxvars.EGL.Surface, linuxvars.EGL.Context);
    
    XRaiseWindow(linuxvars.X11.dpy, linuxvars.X11.win);
    
    if (settings->set_window_pos){
        XMoveWindow(linuxvars.X11.dpy, linuxvars.X11.win, settings->window_x, settings->window_y);
    }
    
    if (settings->maximize_window){
        linux_set_wm_state(linuxvars.X11.atom__NET_WM_STATE_MAXIMIZED_HORZ, linuxvars.X11.atom__NET_WM_STATE_MAXIMIZED_VERT, WM_STATE_ADD);
    } else if (settings->fullscreen_window){
        linux_set_wm_state(linuxvars.X11.atom__NET_WM_STATE_FULLSCREEN, 0, WM_STATE_ADD);
    }
    
    XSync(linuxvars.X11.dpy, False);
    
    Atom wm_protos[] = {
        linuxvars.X11.atom_WM_DELETE_WINDOW,
        linuxvars.X11.atom__NET_WM_PING
    };
    
    XSetWMProtocols(linuxvars.X11.dpy, linuxvars.X11.win, wm_protos, 2);
    
    // XFixes extension for clipboard notification.
    {
        int xfixes_version_unused, xfixes_err_unused;
        Bool has_xfixes = XQueryExtension(linuxvars.X11.dpy, "XFIXES", &xfixes_version_unused, &linuxvars.X11.xfixes_selection_event, &xfixes_err_unused);
        linuxvars.X11.has_xfixes = (has_xfixes == True);
        
        // request notifications for CLIPBOARD updates.
        if(has_xfixes) {
            XFixesSelectSelectionInput(linuxvars.X11.dpy, linuxvars.X11.win, linuxvars.X11.atom_CLIPBOARD, XFixesSetSelectionOwnerNotifyMask);
        }
    }
    
    // Input handling init
    
    setlocale(LC_ALL, "");
    XSetLocaleModifiers("");
    b32 locale_supported = XSupportsLocale();
    
    if (!locale_supported){
        setlocale(LC_ALL, "C");
    }
    
    linuxvars.X11.xim = XOpenIM(dpy, 0, 0, 0);
    if (!linuxvars.X11.xim){
        // NOTE(inso): Try falling back to the internal XIM implementation that
        // should in theory always exist.
        XSetLocaleModifiers("@im=none");
        linuxvars.X11.xim = XOpenIM(dpy, 0, 0, 0);
    }
    
    // If it still isn't there we're screwed.
    if (!linuxvars.X11.xim){
        system_error_box("Could not initialize X Input.");
    }
    
    XIMStyles *styles = NULL;
    const XIMStyle style_want = (XIMPreeditNothing | XIMStatusNothing);
    b32 found_style = false;
    
    if (!XGetIMValues(linuxvars.X11.xim, XNQueryInputStyle, &styles, NULL) && styles){
        for (i32 i = 0; i < styles->count_styles; ++i){
            XIMStyle style = styles->supported_styles[i];
            if (style == style_want) {
                found_style = true;
                break;
            }
        }
    }
    
    if(!found_style) {
        system_error_box("Could not find supported X Input style.");
    }
    
    XFree(styles);
    
    linuxvars.X11.xic = XCreateIC(linuxvars.X11.xim,
                                  XNInputStyle, style_want,
                                  XNClientWindow, linuxvars.X11.win,
                                  XNFocusWindow, linuxvars.X11.win,
                                  NULL);
    
    if(!linuxvars.X11.xic) {
        system_error_box("Error creating X Input context.");
    }
    
    int xim_event_mask;
    if (XGetICValues(linuxvars.X11.xic, XNFilterEvents, &xim_event_mask, NULL)){
        xim_event_mask = 0;
    }
    
    u32 event_mask = ExposureMask
        | KeyPressMask | KeyReleaseMask
        | ButtonPressMask | ButtonReleaseMask
        | EnterWindowMask | LeaveWindowMask
        | PointerMotionMask
        | FocusChangeMask
        | StructureNotifyMask
        | ExposureMask | VisibilityChangeMask
        | xim_event_mask;
    
    XSelectInput(linuxvars.X11.dpy, linuxvars.X11.win, event_mask);
    
    // init XKB keyboard extension
    
    if(!XkbQueryExtension(linuxvars.X11.dpy, 0, &linuxvars.xkb_event, 0, 0, 0)) {
        system_error_box("XKB Extension not available.");
    }
    
    XkbSelectEvents(linuxvars.X11.dpy, XkbUseCoreKbd, XkbAllEventsMask, XkbAllEventsMask);
    linuxvars.X11.xkb = XkbGetMap(linuxvars.X11.dpy, XkbKeyTypesMask | XkbKeySymsMask, XkbUseCoreKbd);
    if(!linuxvars.X11.xkb) {
        system_error_box("Error getting XKB keyboard map.");
    }
    
    if(XkbGetNames(linuxvars.X11.dpy, XkbKeyNamesMask, linuxvars.X11.xkb) != Success) {
        system_error_box("Error getting XKB key names.");
    }
    
    // closer to windows behaviour (holding key doesn't generate release events)
    XkbSetDetectableAutoRepeat(linuxvars.X11.dpy, True, NULL);
    
    XCursor cursors[APP_MOUSE_CURSOR_COUNT] = {
        None,
        None,
        XCreateFontCursor(linuxvars.X11.dpy, XC_xterm),
        XCreateFontCursor(linuxvars.X11.dpy, XC_sb_h_double_arrow),
        XCreateFontCursor(linuxvars.X11.dpy, XC_sb_v_double_arrow)
    };
    block_copy(linuxvars.X11.xcursors, cursors, sizeof(cursors));
    
    // sneaky invisible cursor
    {
        char data = 0;
        XColor c  = {};
        Pixmap p  = XCreateBitmapFromData(linuxvars.X11.dpy, linuxvars.X11.win, &data, 1, 1);
        
        linuxvars.X11.hidden_cursor = XCreatePixmapCursor(linuxvars.X11.dpy, p, p, &c, &c, 0, 0);
        
        XFreePixmap(linuxvars.X11.dpy, p);
    }
}

global Key_Code keycode_lookup_table_physical[255];
global Key_Code keycode_lookup_table_language[255];

struct SymCode {
    KeySym sym;
    Key_Code code;
};

internal void
linux_keycode_init_common(Display* dpy, Key_Code* keycode_lookup_table, SymCode* sym_table, SymCode* p, size_t sym_table_size){
    
    *p++ = { XK_space, KeyCode_Space };
    *p++ = { XK_Tab, KeyCode_Tab };
    *p++ = { XK_Escape, KeyCode_Escape };
    *p++ = { XK_Pause, KeyCode_Pause };
    *p++ = { XK_Up, KeyCode_Up };
    *p++ = { XK_Down, KeyCode_Down };
    *p++ = { XK_Left, KeyCode_Left };
    *p++ = { XK_Right, KeyCode_Right };
    *p++ = { XK_BackSpace, KeyCode_Backspace };
    *p++ = { XK_Return, KeyCode_Return };
    *p++ = { XK_Delete, KeyCode_Delete };
    *p++ = { XK_Insert, KeyCode_Insert };
    *p++ = { XK_Home, KeyCode_Home };
    *p++ = { XK_End, KeyCode_End };
    *p++ = { XK_Page_Up, KeyCode_PageUp };
    *p++ = { XK_Page_Down, KeyCode_PageDown };
    *p++ = { XK_Caps_Lock, KeyCode_CapsLock };
    *p++ = { XK_Num_Lock, KeyCode_NumLock };
    *p++ = { XK_Scroll_Lock, KeyCode_ScrollLock };
    *p++ = { XK_Menu, KeyCode_Menu };
    *p++ = { XK_Shift_L, KeyCode_Shift };
    *p++ = { XK_Shift_R, KeyCode_Shift };
    *p++ = { XK_Control_L, KeyCode_Control };
    *p++ = { XK_Control_R, KeyCode_Control };
    *p++ = { XK_Alt_L, KeyCode_Alt };
    *p++ = { XK_Alt_R, KeyCode_Alt };
    *p++ = { XK_Super_L, KeyCode_Command };
    *p++ = { XK_Super_R, KeyCode_Command };
    
    for (Key_Code k = KeyCode_F1; k <= KeyCode_F24; ++k){
        *p++ = { XK_F1 + (k - KeyCode_F1), k };
    }
    
    for (Key_Code k = KeyCode_NumPad0; k <= KeyCode_NumPad9; ++k){
        *p++ = { XK_KP_0 + (k - KeyCode_NumPad0), k };
    }
    
    *p++ = { XK_KP_Multiply, KeyCode_NumPadStar };
    *p++ = { XK_KP_Add, KeyCode_NumPadPlus };
    *p++ = { XK_KP_Subtract, KeyCode_NumPadMinus };
    *p++ = { XK_KP_Decimal, KeyCode_NumPadDot };
    *p++ = { XK_KP_Delete, KeyCode_NumPadDot }; // seems to take precedence over Decimal...
    *p++ = { XK_KP_Divide, KeyCode_NumPadSlash };
    *p++ = { XK_KP_Enter, KeyCode_Return }; // NumPadEnter?
    
    const int table_size = p - sym_table;
    Assert(table_size < sym_table_size);
    
    Key_Code next_extra = KeyCode_Ex1;
    const Key_Code max_extra = KeyCode_Ex29;
    
    for(int i = XkbMinLegalKeyCode; i <= XkbMaxLegalKeyCode; ++i) {
        KeySym sym = NoSymbol;
        
        // lookup key in current layout with no modifiers held (0)
        if(!XkbTranslateKeyCode(linuxvars.X11.xkb, i, XkbBuildCoreState(0, linuxvars.xkb_group), NULL, &sym)) {
            continue;
        }
        
        int j;
        for(j = 0; j < table_size; ++j) {
            if(sym_table[j].sym == sym) {
                keycode_lookup_table[i] = sym_table[j].code;
                //printf("lookup %s = %d\n", key_code_name[sym_table[j].code], i);
                break;
            }
        }
        
        if(j != table_size){
            continue;
        }
        
        // nothing found - try with shift held (needed for e.g. belgian numbers to bind).
        KeySym shift_sym = NoSymbol;
        
        if(!XkbTranslateKeyCode(linuxvars.X11.xkb, i, XkbBuildCoreState(ShiftMask, linuxvars.xkb_group), NULL, &shift_sym)) {
            continue;
        }
        
        for(j = 0; j < table_size; ++j) {
            if(sym_table[j].sym == shift_sym) {
                keycode_lookup_table[i] = sym_table[j].code;
                //printf("lookup %s = %d\n", key_code_name[sym_table[j].code], i);
                break;
            }
        }
        
        // something unknown bound, put it in extra
        if(j == table_size && sym != NoSymbol && next_extra <= max_extra && keycode_lookup_table[i] == 0) {
            keycode_lookup_table[i] = next_extra++;
        }
    }
    
}

internal void
linux_keycode_init_language(Display* dpy, Key_Code* keycode_lookup_table){
    SymCode sym_table[300];
    SymCode* p = sym_table;
    
    for(unsigned int i = 0; i < 26; ++i) {
        *p++ = { XK_a + i, KeyCode_A + i};
    }
    
    for(unsigned int i = 0; i < 26; ++i) {
        *p++ = { XK_A + i, KeyCode_A + i};
    }
    
    for(unsigned int i = 0; i <= 9; ++i) {
        *p++ = { XK_0 + i, KeyCode_0 + i};
    }
    
    *p++ = { XK_grave, KeyCode_Tick };
    *p++ = { XK_minus, KeyCode_Minus };
    *p++ = { XK_equal, KeyCode_Equal };
    *p++ = { XK_bracketleft, KeyCode_LeftBracket };
    *p++ = { XK_bracketright, KeyCode_RightBracket };
    *p++ = { XK_semicolon, KeyCode_Semicolon };
    *p++ = { XK_apostrophe, KeyCode_Quote };
    *p++ = { XK_comma, KeyCode_Comma };
    *p++ = { XK_period, KeyCode_Period };
    *p++ = { XK_slash, KeyCode_ForwardSlash };
    *p++ = { XK_backslash, KeyCode_BackwardSlash };
    
    linux_keycode_init_common(dpy, keycode_lookup_table, sym_table, p, ArrayCount(sym_table));
}

internal void
linux_keycode_init_physical(Display* dpy, Key_Code* keycode_lookup_table){
    
    // Find common keys by their key label
    SymCode sym_table[100];
    linux_keycode_init_common(dpy, keycode_lookup_table, sym_table, sym_table, ArrayCount(sym_table));
    
    // Find these keys by physical position, and map to QWERTY KeyCodes
#define K(k) glue(KeyCode_, k)
    static const u8 positional_keys[] = {
        K(1), K(2), K(3), K(4), K(5), K(6), K(7), K(8), K(9), K(0), K(Minus), K(Equal),
        K(Q), K(W), K(E), K(R), K(T), K(Y), K(U), K(I), K(O), K(P), K(LeftBracket), K(RightBracket),
        K(A), K(S), K(D), K(F), K(G), K(H), K(J), K(K), K(L), K(Semicolon), K(Quote), /*uk hash*/0,
        K(Z), K(X), K(C), K(V), K(B), K(N), K(M), K(Comma), K(Period), K(ForwardSlash), 0, 0
    };
#undef K
    
    // XKB gives the alphanumeric keys names like AE01 -> E is the row (from B-E), 01 is the column (01-12).
    // to get key names in .ps file: setxkbmap -print | xkbcomp - - | xkbprint -label name - out.ps
    
    static const int ncols = 12;
    static const int nrows = 4;
    
    for(int i = XkbMinLegalKeyCode; i <= XkbMaxLegalKeyCode; ++i) {
        const char* name = linuxvars.X11.xkb->names->keys[i].name;
        
        // alphanumeric keys
        
        if(name[0] == 'A' && name[1] >= 'B' && name[1] <= 'E') {
            int row = (nrows - 1) - (name[1] - 'B');
            int col = (name[2] - '0') * 10 + (name[3] - '0') - 1;
            
            if(row >= 0 && row < nrows && col >= 0 && col < ncols) {
                keycode_lookup_table[i] = positional_keys[row * ncols + col];
            }
        }
        
        // numpad
        
        else if(name[0] == 'K' && name[1] == 'P' && name[2] >= '0' && name[2] <= '9' && !name[3]) {
            
            // don't overwrite - for e.g. laptops with numpad keys embedded in the normal ones, toggling with numlock
            if(keycode_lookup_table[i] == 0) {
                keycode_lookup_table[i] = KeyCode_NumPad0 + name[2] - '0';
            }
        }
        
        // a few special cases:
        
        else if(memcmp(name, "TLDE", XkbKeyNameLength) == 0) {
            keycode_lookup_table[i] = KeyCode_Tick;
        } else if(memcmp(name, "BKSL", XkbKeyNameLength) == 0) {
            keycode_lookup_table[i] = KeyCode_BackwardSlash;
        } else if(memcmp(name, "LSGT", XkbKeyNameLength) == 0) {
            // UK extra key between left shift and Z
            // it prints \ and | with shift. KeyCode_Backslash will be where UK # is.
            keycode_lookup_table[i] = KeyCode_Ex0;
        }
    }
}

internal void
linux_keycode_init(Display* dpy){
    block_zero_array(keycode_lookup_table_physical);
    block_zero_array(keycode_lookup_table_language);
    
    linux_keycode_init_physical(dpy, keycode_lookup_table_physical);
    linux_keycode_init_language(dpy, keycode_lookup_table_language);
}

internal void
linux_epoll_init(void) {
    struct epoll_event e = {};
    e.events = EPOLLIN | EPOLLET;
    
    linuxvars.step_timer_fd = timerfd_create(CLOCK_MONOTONIC, TFD_NONBLOCK);
    linuxvars.epoll = epoll_create(16);
    
    //e.data.ptr = & epoll_tag_x11;
    //epoll_ctl(linuxvars.epoll, EPOLL_CTL_ADD, ConnectionNumber(linuxvars.X11.dpy), &e);
    
    e.data.ptr = & epoll_tag_xkb;
    epoll_ctl(linuxvars.epoll, EPOLL_CTL_ADD, linuxvars.Wayland.RepeatHandle, &e);
    
    e.data.ptr = & epoll_tag_wayland;
    epoll_ctl(linuxvars.epoll, EPOLL_CTL_ADD, wl_display_get_fd(linuxvars.Wayland.Display), &e);
    
    e.data.ptr = &epoll_tag_step_timer;
    epoll_ctl(linuxvars.epoll, EPOLL_CTL_ADD, linuxvars.step_timer_fd, &e);
}

#if 0
internal void
linux_clipboard_send(XSelectionRequestEvent* req) {
    
    XSelectionEvent rsp = {};
    rsp.type = SelectionNotify;
    rsp.requestor = req->requestor;
    rsp.selection = req->selection;
    rsp.target = req->target;
    rsp.time = req->time;
    rsp.property = None;
    
    Atom formats[] = {
        linuxvars.X11.atom_UTF8_STRING,
        XA_STRING,
    };
    
    if(linuxvars.clipboard_contents.size == 0) {
        goto done;
    }
    
    if(req->selection != linuxvars.X11.atom_CLIPBOARD || req->property == None) {
        goto done;
    }
    
    if (req->target == linuxvars.X11.atom_TARGETS){
        
        XChangeProperty(
                        req->display,
                        req->requestor,
                        req->property,
                        XA_ATOM,
                        32,
                        PropModeReplace,
                        (u8*)formats,
                        ArrayCount(formats));
        
        rsp.property = req->property;
        
    } else {
        
        int i;
        for(i = 0; i < ArrayCount(formats); ++i){
            if (req->target == formats[i]){
                break;
            }
        }
        
        if (i != ArrayCount(formats)){
            XChangeProperty(
                            req->display,
                            req->requestor,
                            req->property,
                            req->target,
                            8,
                            PropModeReplace,
                            linuxvars.clipboard_contents.str,
                            linuxvars.clipboard_contents.size
                            );
            
            rsp.property = req->property;
        }
    }
    
    done:
    XSendEvent(req->display, req->requestor, True, 0, (XEvent*)&rsp);
}

internal String_Const_u8
linux_clipboard_recv(Arena *arena){
    Atom type;
    int fmt;
    unsigned long nitems;
    unsigned long bytes_left;
    u8 *data;
    
    int result = XGetWindowProperty(linuxvars.X11.dpy,
                                    linuxvars.X11.win,
                                    linuxvars.X11.atom_CLIPBOARD,
                                    0L, 0x20000000L, False,
                                    linuxvars.X11.atom_UTF8_STRING,
                                    &type, &fmt, &nitems,
                                    &bytes_left, &data);
    
    String_Const_u8 clip = {};
    if(result == Success && fmt == 8){
        clip= push_string_copy(arena, SCu8(data, nitems));
        XFree(data);
        XDeleteProperty(linuxvars.X11.dpy, linuxvars.X11.win, linuxvars.X11.atom_CLIPBOARD);
    }
    
    return(clip);
}

internal void
linux_clipboard_recv(XSelectionEvent* ev) {
    
    if(ev->selection != linuxvars.X11.atom_CLIPBOARD ||
       ev->target != linuxvars.X11.atom_UTF8_STRING ||
       ev->property == None) {
        return;
    }
    
    Scratch_Block scratch(&linuxvars.tctx);
    String_Const_u8 clip = linux_clipboard_recv(scratch);
    if (clip.size > 0){
        linalloc_clear(&linuxvars.clipboard_arena);
        linuxvars.clipboard_contents = push_string_copy(&linuxvars.clipboard_arena, clip);
        linuxvars.received_new_clipboard = true;
        linux_schedule_step();
    }
}

internal
system_get_clipboard_sig(){
    // TODO(inso): index?
    return(push_string_copy(arena, linuxvars.clipboard_contents));
}

internal void
system_post_clipboard(String_Const_u8 str, i32 index){
    // TODO(inso): index?
    //LINUX_FN_DEBUG("%.*s", string_expand(str));
    linalloc_clear(&linuxvars.clipboard_arena);
    linuxvars.clipboard_contents = push_u8_stringf(&linuxvars.clipboard_arena, "%.*s", string_expand(str));
    XSetSelectionOwner(linuxvars.X11.dpy, linuxvars.X11.atom_CLIPBOARD, linuxvars.X11.win, CurrentTime);
}
#else
internal
system_get_clipboard_sig(){
    // TODO(inso): index?
    return(push_string_copy(arena, linuxvars.clipboard_contents));
}

internal void
system_post_clipboard(String_Const_u8 str, i32 index){
    // TODO(inso): index?
    linalloc_clear(&linuxvars.clipboard_arena);
    linuxvars.clipboard_contents = push_u8_stringf(&linuxvars.clipboard_arena, "%.*s", string_expand(str));
    
    decltype(linuxvars.Wayland) *Wayland = &linuxvars.Wayland;
    
    wl_data_source *DataSource = wl_data_device_manager_create_data_source(Wayland->DataDeviceManager);
    wl_data_source_add_listener(DataSource, &Wayland->DataSourceListener, 0);
    wl_data_source_offer(DataSource, "text/plain");
    wl_data_source_offer(DataSource, "text/plain;charset=utf-8");
    wl_data_source_offer(DataSource, "TEXT");
    wl_data_source_offer(DataSource, "STRING");
    wl_data_source_offer(DataSource, "UTF8_STRING");
    wl_data_device_set_selection(Wayland->DataDevice, DataSource, Wayland->KeyboardEnterSerial);
}
#endif

internal void
system_set_clipboard_catch_all(b32 enabled){
    LINUX_FN_DEBUG("%d", enabled);
    linuxvars.clipboard_catch_all = !!enabled;
}

internal b32
system_get_clipboard_catch_all(void){
    return linuxvars.clipboard_catch_all;
}

internal KeyCode
linux_numlock_convert(KeyCode in){
    static const KeyCode lookup[] = {
        KeyCode_Insert,
        KeyCode_End,
        KeyCode_Down,
        KeyCode_PageDown,
        KeyCode_Left,
        0,
        KeyCode_Right,
        KeyCode_Home,
        KeyCode_Up,
        KeyCode_PageUp,
        0, 0, 0,
        KeyCode_Delete,
    };
    
    if(in >= KeyCode_NumPad0 && in <= KeyCode_NumPadDot) {
        KeyCode ret = lookup[in - KeyCode_NumPad0];
        if(ret != 0) {
            return ret;
        }
    }
    
    return in;
}

internal void
linux_handle_x11_events() {
    static XEvent prev_event = {};
    b32 should_step = false;
    
    while (XPending(linuxvars.X11.dpy)) {
        XEvent event;
        XNextEvent(linuxvars.X11.dpy, &event);
        
        b32 filtered = false;
        if (XFilterEvent(&event, None) == True){
            filtered = true;
            if(event.type != KeyPress && event.type != KeyRelease) {
                continue;
            }
        }
        
        u64 event_id = (u64)event.xkey.serial << 32 | event.xkey.time;
        
        switch(event.type) {
            case KeyPress: {
                should_step = true;
                
                Input_Modifier_Set_Fixed* mods = &linuxvars.input.pers.modifiers;
                
                int state = event.xkey.state;
                set_modifier(mods, KeyCode_Shift, state & ShiftMask);
                set_modifier(mods, KeyCode_Control, state & ControlMask);
                set_modifier(mods, KeyCode_CapsLock, state & LockMask);
                set_modifier(mods, KeyCode_Alt, state & Mod1Mask);
                
                event.xkey.state &= ~(ControlMask);
                
                Status status;
                KeySym keysym = NoSymbol;
                u8 buf[256] = {};
                
                int len = Xutf8LookupString(linuxvars.X11.xic, &event.xkey, (char*)buf, sizeof(buf) - 1, &keysym, &status);
                
                if (status == XBufferOverflow){
                    Xutf8ResetIC(linuxvars.X11.xic);
                    XSetICFocus(linuxvars.X11.xic);
                }
                
                if (keysym == XK_ISO_Left_Tab){
                    add_modifier(mods, KeyCode_Shift);
                }
                
                Key_Code key;
                if(linuxvars.key_mode == KeyMode_Physical) {
                    key = keycode_lookup_table_physical[(u8)event.xkey.keycode];
                } else {
                    key = keycode_lookup_table_language[(u8)event.xkey.keycode];
                }
                
                if(!(state & Mod2Mask)) {
                    key = linux_numlock_convert(key);
                }
                
                //printf("key %d = %s (f:%d)\n", event.xkey.keycode, key_code_name[key], filtered);
                
                b32 is_dead = false;
                if (keysym >= XK_dead_grave && keysym <= XK_dead_greek && len == 0) {
                    is_dead = true;
                }
                
                if(!is_dead && filtered) {
                    linuxvars.prev_filtered_key = key;
                    break;
                }
                
                // send a keycode for the key after the dead key
                if(!key && linuxvars.prev_filtered_key) {
                    key = linuxvars.prev_filtered_key;
                    linuxvars.prev_filtered_key = 0;
                }
                
                Input_Event* key_event = NULL;
                if(key) {
                    add_modifier(mods, key);
                    // printf(" push key %d\n", key);
                    
                    key_event = push_input_event(&linuxvars.frame_arena, &linuxvars.input.trans.event_list);
                    key_event->kind = InputEventKind_KeyStroke;
                    key_event->key.code = key;
                    key_event->key.modifiers = copy_modifier_set(&linuxvars.frame_arena, mods);
                    key_event->key.flags = 0;
                    if (is_dead){
                        key_event->key.flags |= KeyFlag_IsDeadKey;
                    }
                }
                
                Input_Event* text_event = NULL;
                if(status == XLookupChars || status == XLookupBoth) {
                    String_Const_u8 str = linux_filter_text(&linuxvars.frame_arena, buf, len);
                    if(str.size) {
                        // printf(" push txt %d\n", key);
                        text_event = push_input_event(&linuxvars.frame_arena, &linuxvars.input.trans.event_list);
                        text_event->kind = InputEventKind_TextInsert;
                        text_event->text.string = str;
                    }
                }
                
                if(key_event && text_event) {
                    key_event->key.first_dependent_text = text_event;
                }
            } break;
            
            case KeyRelease: {
                should_step = true;
                
                Input_Modifier_Set_Fixed* mods = &linuxvars.input.pers.modifiers;
                
                int state = event.xkey.state;
                set_modifier(mods, KeyCode_Shift, state & ShiftMask);
                set_modifier(mods, KeyCode_Control, state & ControlMask);
                set_modifier(mods, KeyCode_CapsLock, state & LockMask);
                set_modifier(mods, KeyCode_Alt, state & Mod1Mask);
                
                Key_Code key;
                if(linuxvars.key_mode == KeyMode_Physical) {
                    key = keycode_lookup_table_physical[(u8)event.xkey.keycode];
                } else {
                    key = keycode_lookup_table_language[(u8)event.xkey.keycode];
                }
                
                // num lock off -> convert KP keys to Insert, Home, End etc.
                if(!(state & Mod2Mask)) {
                    key = linux_numlock_convert(key);
                }
                
                Input_Event* key_event = NULL;
                if(key) {
                    remove_modifier(mods, key);
                    key_event = push_input_event(&linuxvars.frame_arena, &linuxvars.input.trans.event_list);
                    key_event->kind = InputEventKind_KeyRelease;
                    key_event->key.code = key;
                    key_event->key.modifiers = copy_modifier_set(&linuxvars.frame_arena, mods);
                }
            } break;
            
            case MotionNotify: {
                int x = clamp(0, event.xmotion.x, render_target.width - 1);
                int y = clamp(0, event.xmotion.y, render_target.height - 1);
                linuxvars.input.pers.mouse = { x, y };
                should_step = true;
            } break;
            
            case ButtonPress: {
                should_step = true;
                switch(event.xbutton.button) {
                    case Button1: {
                        linuxvars.input.trans.mouse_l_press = true;
                        linuxvars.input.pers.mouse_l = true;
                        
                        // NOTE(inso): improves selection dragging (especially in notepad-like mode).
                        // we will still get mouse events when the pointer leaves the window if it's dragging.
                        XGrabPointer(
                                     linuxvars.X11.dpy,
                                     linuxvars.X11.win,
                                     True, PointerMotionMask | ButtonPressMask | ButtonReleaseMask,
                                     GrabModeAsync, GrabModeAsync,
                                     None, None, CurrentTime);
                        
                    } break;
                    
                    case Button3: {
                        linuxvars.input.trans.mouse_r_press = true;
                        linuxvars.input.pers.mouse_r = true;
                    } break;
                    
                    case Button4: {
                        linuxvars.input.trans.mouse_wheel = -100;
                    } break;
                    
                    case Button5: {
                        linuxvars.input.trans.mouse_wheel = +100;
                    } break;
                }
            } break;
            
            case ButtonRelease: {
                should_step = true;
                switch(event.xbutton.button) {
                    case Button1: {
                        linuxvars.input.trans.mouse_l_release = true;
                        linuxvars.input.pers.mouse_l = false;
                        
                        XUngrabPointer(linuxvars.X11.dpy, CurrentTime);
                    } break;
                    
                    case Button3: {
                        linuxvars.input.trans.mouse_r_release = true;
                        linuxvars.input.pers.mouse_r = false;
                    } break;
                }
            } break;
            
            case FocusIn:
            case FocusOut: {
                linuxvars.input.pers.mouse_l = false;
                linuxvars.input.pers.mouse_r = false;
                block_zero_struct(&linuxvars.input.pers.modifiers);
            } break;
            
            case EnterNotify: {
                linuxvars.input.pers.mouse_out_of_window = 0;
            } break;
            
            case LeaveNotify: {
                linuxvars.input.pers.mouse_out_of_window = 1;
            } break;
            
            case ConfigureNotify: {
                i32 w = event.xconfigure.width;
                i32 h = event.xconfigure.height;
                
                if (w != render_target.width || h != render_target.height){
                    should_step = true;
                    render_target.width = w;
                    render_target.height = h;
                }
            } break;
            
            case ClientMessage: {
                Atom atom = event.xclient.data.l[0];
                
                // Window X button clicked
                if(atom == linuxvars.X11.atom_WM_DELETE_WINDOW) {
                    should_step = true;
                    linuxvars.input.trans.trying_to_kill = true;
                }
                
                // Notify WM that we're still responding (don't grey our window out).
                else if(atom == linuxvars.X11.atom__NET_WM_PING) {
                    event.xclient.window = DefaultRootWindow(linuxvars.X11.dpy);
                    XSendEvent(linuxvars.X11.dpy,
                               event.xclient.window,
                               False,
                               SubstructureRedirectMask | SubstructureNotifyMask,
                               &event);
                }
            } break;
            
            case SelectionRequest: {
                //linux_clipboard_send((XSelectionRequestEvent*)&event);
            } break;
            
            case SelectionNotify: {
                //linux_clipboard_recv((XSelectionEvent*)&event);
            } break;
            
            case SelectionClear: {
                if(event.xselectionclear.selection == linuxvars.X11.atom_CLIPBOARD) {
                    linalloc_clear(&linuxvars.clipboard_arena);
                    block_zero_struct(&linuxvars.clipboard_contents);
                }
            } break;
            
            case Expose:
            case VisibilityNotify: {
                should_step = true;
            } break;
            
            default: {
                // clipboard update notification - ask for the new content
                if (event.type == linuxvars.X11.xfixes_selection_event) {
                    XFixesSelectionNotifyEvent* sne = (XFixesSelectionNotifyEvent*)&event;
                    if (sne->subtype == XFixesSelectionNotify && sne->owner != linuxvars.X11.win){
                        XConvertSelection(linuxvars.X11.dpy,
                                          linuxvars.X11.atom_CLIPBOARD,
                                          linuxvars.X11.atom_UTF8_STRING,
                                          linuxvars.X11.atom_CLIPBOARD,
                                          linuxvars.X11.win,
                                          CurrentTime);
                    }
                }
                
                else if(event.type == linuxvars.xkb_event) {
                    XkbEvent* kb = (XkbEvent*)&event;
                    
                    // Keyboard layout changed, refresh lookup table.
                    if(kb->any.xkb_type == XkbStateNotify && kb->state.group != linuxvars.xkb_group) {
                        linuxvars.xkb_group = kb->state.group;
                        XkbRefreshKeyboardMapping((XkbMapNotifyEvent*)kb);
                        linux_keycode_init(linuxvars.X11.dpy);
                    }
                }
            } break;
        }
    }
    
    if(should_step) {
        linux_schedule_step();
    }
}

internal b32
linux_epoll_process(struct epoll_event* events, int num_events) {
    b32 do_step = false;
    
    b32 GotWayland = false;
    
    for (int i = 0; i < num_events; ++i){
        struct epoll_event* ev = events + i;
        Epoll_Kind* tag = (Epoll_Kind*)ev->data.ptr;
        
        switch (*tag){
            case EPOLL_X11: {
                //linux_handle_x11_events();
            } break;
            
            case EPOLL_WAYLAND:
            {
                wl_display_read_events(linuxvars.Wayland.Display);
                GotWayland = true;
            } break;
            
            case EPOLL_XKB:
            {
                u64 RepeatCount = 0;
                if(read(linuxvars.Wayland.RepeatHandle, &RepeatCount, sizeof(RepeatCount)) == 8)
                {
                    for(u64 RepeatIndex = 0;
                        RepeatIndex < RepeatCount;
                        ++RepeatIndex)
                    {
                        LinuxProcessKeyboardInputDown(linuxvars.Wayland.RepeatKeyCode, &linuxvars.Wayland.RepeatMods);
                    }
                }
            } break;
            
            case EPOLL_X11_INTERNAL: {
                //XProcessInternalConnection(linuxvars.dpy, fd);
            } break;
            
            case EPOLL_STEP_TIMER: {
                u64 count;
                int ret;
                do {
                    ret = read(linuxvars.step_timer_fd, &count, 8);
                } while (ret != -1 || errno != EAGAIN);
                do_step = true;
            } break;
            
            case EPOLL_CLI_PIPE: {
                linux_schedule_step();
            } break;
            
            case EPOLL_USER_TIMER: {
                Linux_Object* obj = CastFromMember(Linux_Object, timer.epoll_tag, tag);
                close(obj->timer.fd);
                obj->timer.fd = -1;
                linux_schedule_step();
            } break;
        }
    }
    
    if(!GotWayland)
    {
        wl_display_cancel_read(linuxvars.Wayland.Display);
    }
    
    return do_step;
}

int
main(int argc, char **argv){
    // NOTE(allen): fucking bullshit. someone get my shit togeth :(er
    
    for (i32 i = 0; i < argc; i += 1){
        String_Const_u8 arg = SCu8(argv[i]);
        if (string_match(arg, str8_lit("-L"))){
            log_os_enabled = true;
        }
    }
    
    // NOTE(allen): All of This thing
    pthread_mutexattr_t attr;
    pthread_mutexattr_init(&attr);
    pthread_mutexattr_settype(&attr, PTHREAD_MUTEX_RECURSIVE);
    pthread_mutex_init(&linuxvars.memory_tracker_mutex, &attr);
    
    pthread_mutex_init(&linuxvars.audio_mutex, &attr);
    pthread_cond_init(&linuxvars.audio_cond, NULL);
    
    // NOTE(allen): context setup
    {
        Base_Allocator* alloc = get_base_allocator_system();
        thread_ctx_init(&linuxvars.tctx, ThreadKind_Main, alloc, alloc);
    }
    
    API_VTable_system system_vtable = {};
    system_api_fill_vtable(&system_vtable);
    
    API_VTable_graphics graphics_vtable = {};
    graphics_api_fill_vtable(&graphics_vtable);
    
    API_VTable_font font_vtable = {};
    font_api_fill_vtable(&font_vtable);
    
    // NOTE(allen): memory
    linuxvars.frame_arena = make_arena_system();
    linuxvars.clipboard_arena = make_arena_system();
    render_target.arena = make_arena_system(KB(256));
    
    //linuxvars.fontconfig = FcInitLoadConfigAndFonts();
    
    linuxvars.cursor_show = MouseCursorShow_Always;
    linuxvars.prev_cursor_show = MouseCursorShow_Always;
    
    dll_init_sentinel(&linuxvars.free_linux_objects);
    dll_init_sentinel(&linuxvars.timer_objects);
    
    //InitializeCriticalSection(&win32vars.thread_launch_mutex);
    //InitializeConditionVariable(&win32vars.thread_launch_cv);
    
    linuxvars.clipboard_catch_all = false;
    
    // NOTE(allen): load core
    System_Library core_library = {};
    App_Functions app = {};
    {
        App_Get_Functions *get_funcs = 0;
        Scratch_Block scratch(&linuxvars.tctx);
        List_String_Const_u8 search_list = {};
        def_search_list_add_system_path(scratch, &search_list, SystemPath_Binary);
        
        String_Const_u8 core_path = def_search_get_full_path(scratch, &search_list, SCu8("4ed_app.so"));
        if (system_load_library(scratch, core_path, &core_library)){
            get_funcs = (App_Get_Functions*)system_get_proc(core_library, "app_get_functions");
            if (get_funcs != 0){
                app = get_funcs();
            }
            else{
                char msg[] = "Failed to get application code from '4ed_app.so'.";
                system_error_box(msg);
            }
        }
        else{
            char msg[] = "Could not load '4ed_app.so'. This file should be in the same directory as the main '4ed' executable.";
            system_error_box(msg);
        }
    }
    
    // NOTE(allen): send system vtable to core
    app.load_vtables(&system_vtable, &font_vtable, &graphics_vtable);
    // get_logger calls log_init which is needed.
    //app.get_logger();
    linuxvars.log_string = app.get_logger();
    
    // NOTE(allen): init & command line parameters
    Plat_Settings plat_settings = {};
    void *base_ptr = 0;
    {
        Scratch_Block scratch(&linuxvars.tctx);
        String_Const_u8 curdir = system_get_path(scratch, SystemPath_CurrentDirectory);
        
        char **files = 0;
        i32 *file_count = 0;
        base_ptr = app.read_command_line(&linuxvars.tctx, curdir, &plat_settings, &files, &file_count, argc, argv);
        /* TODO(inso): what is this doing?
        {
            i32 end = *file_count;
            i32 i = 0, j = 0;
            for (; i < end; ++i){
                if (system_file_can_be_made(scratch, (u8*)files[i])){
                    files[j] = files[i];
                    ++j;
                }
            }
            *file_count = j;
        }*/
    }
    
    // NOTE(allen): setup user directory override
    if (plat_settings.user_directory != 0){
        lnx_override_user_directory = plat_settings.user_directory;
    }
    
    // NOTE(allen): load custom layer
    System_Library custom_library = {};
    Custom_API custom = {};
    {
        char custom_not_found_msg[] = "Did not find a library for the custom layer.";
        char custom_fail_load_msg[] = "Failed to load custom code due to missing version information.  Try rebuilding with buildsuper.";
        char custom_fail_version_msg[] = "Failed to load custom code due to a version mismatch.  Try rebuilding with buildsuper.";
        char custom_fail_init_apis[] = "Failed to load custom code due to missing 'init_apis' symbol.  Try rebuilding with buildsuper";
        
        Scratch_Block scratch(&linuxvars.tctx);
        String_Const_u8 default_file_name = string_u8_litexpr("custom_4coder.so");
        List_String_Const_u8 search_list = {};
        def_search_list_add_system_path(scratch, &search_list, SystemPath_UserDirectory);
        def_search_list_add_system_path(scratch, &search_list, SystemPath_Binary);
        String_Const_u8 custom_file_names[2] = {};
        i32 custom_file_count = 1;
        if (plat_settings.custom_dll != 0){
            custom_file_names[0] = SCu8(plat_settings.custom_dll);
            if (!plat_settings.custom_dll_is_strict){
                custom_file_names[1] = default_file_name;
                custom_file_count += 1;
            }
        }
        else{
            custom_file_names[0] = default_file_name;
        }
        String_Const_u8 custom_file_name = {};
        for (i32 i = 0; i < custom_file_count; i += 1){
            custom_file_name = def_search_get_full_path(scratch, &search_list, custom_file_names[i]);
            if (custom_file_name.size > 0){
                break;
            }
        }
        b32 has_library = false;
        if (custom_file_name.size > 0){
            if (system_load_library(scratch, custom_file_name, &custom_library)){
                has_library = true;
            }
        }
        
        if (!has_library){
            system_error_box(custom_not_found_msg);
        }
        custom.get_version = (_Get_Version_Type*)system_get_proc(custom_library, "get_version");
        if (custom.get_version == 0){
            system_error_box(custom_fail_load_msg);
        }
        else if (custom.get_version(MAJOR, MINOR, PATCH) == 0){
            system_error_box(custom_fail_version_msg);
        }
        custom.init_apis = (_Init_APIs_Type*)system_get_proc(custom_library, "init_apis");
        if (custom.init_apis == 0){
            system_error_box(custom_fail_init_apis);
        }
    }
    
    //linux_x11_init(argc, argv, &plat_settings);
    //linux_keycode_init(linuxvars.X11.dpy);
    
    linux_wayland_init(argc, argv, &plat_settings);
    
    linux_epoll_init();
    
    GlobalRunning = true;
    linuxvars.audio_thread = system_thread_launch(&linux_audio_main, NULL);
    
    
    // app init
    {
        Scratch_Block scratch(&linuxvars.tctx);
        String_Const_u8 curdir = system_get_path(scratch, SystemPath_CurrentDirectory);
        app.init(&linuxvars.tctx, &render_target, base_ptr, curdir, custom);
    }
    
    linuxvars.global_frame_mutex = system_mutex_make();
    system_mutex_acquire(linuxvars.global_frame_mutex);
    
    linux_schedule_step();
    b32 first_step = true;
    u64 timer_start = system_now_time();
    
    while (GlobalRunning) {
        
#if 0
        if (XEventsQueued(linuxvars.X11.dpy, QueuedAlready)){
            linux_handle_x11_events();
        }
#endif
        
#if 0
        if(PollHandles[0].revents & POLLIN)
        {
            
        }
#endif
        system_mutex_release(linuxvars.global_frame_mutex);
        
        if(!first_step)
        {
            while(wl_display_prepare_read(linuxvars.Wayland.Display))
            {
                wl_display_dispatch_pending(linuxvars.Wayland.Display);
            }
            wl_display_flush(linuxvars.Wayland.Display);
            
            struct epoll_event events[16];
            int num_events = epoll_wait(linuxvars.epoll, events, ArrayCount(events), -1);
            
            if (num_events == -1){
                if (errno != EINTR){
                    perror("epoll_wait");
                    //LOG("epoll_wait\n");
                }
                continue;
            }
            
            if(!linux_epoll_process(events, num_events)) {
                continue;
            }
        }
        
        system_mutex_acquire(linuxvars.global_frame_mutex);
        
        wl_display_dispatch_pending(linuxvars.Wayland.Display);
        
        //if(!linuxvars.Wayland.WaitingForPresent)
        {
            wl_egl_window_resize(linuxvars.Wayland.Window, linuxvars.Wayland.BufferWidth, linuxvars.Wayland.BufferHeight, 0, 0);
            
            linuxvars.last_step_time = system_now_time();
            
#if 0
            // NOTE(allen): Frame Clipboard Input
            // Request clipboard contents from X11 on first step, or every step if they don't have XFixes notification ability.
            if (first_step || (!linuxvars.X11.has_xfixes && linuxvars.clipboard_catch_all)){
                XConvertSelection(linuxvars.X11.dpy, linuxvars.X11.atom_CLIPBOARD, linuxvars.X11.atom_UTF8_STRING, linuxvars.X11.atom_CLIPBOARD, linuxvars.X11.win, CurrentTime);
            }
#endif
            render_target.width = linuxvars.Wayland.BufferWidth;
            render_target.height = linuxvars.Wayland.BufferHeight;
            
            Application_Step_Input input = {};
            
            if (linuxvars.received_new_clipboard && linuxvars.clipboard_catch_all){
                input.clipboard = linuxvars.clipboard_contents;
            }
            linuxvars.received_new_clipboard = false;
            
            input.first_step = first_step;
            input.dt = frame_useconds/1000000.f; // variable?
            input.events = linuxvars.input.trans.event_list;
            input.trying_to_kill = linuxvars.input.trans.trying_to_kill;
            
            input.mouse.out_of_window = linuxvars.input.pers.mouse_out_of_window;
            input.mouse.p = linuxvars.input.pers.mouse;
            input.mouse.l = linuxvars.input.pers.mouse_l;
            input.mouse.r = linuxvars.input.pers.mouse_r;
            input.mouse.press_l = linuxvars.input.trans.mouse_l_press;
            input.mouse.release_l = linuxvars.input.trans.mouse_l_release;
            input.mouse.press_r = linuxvars.input.trans.mouse_r_press;
            input.mouse.release_r = linuxvars.input.trans.mouse_r_release;
            input.mouse.wheel = linuxvars.input.trans.mouse_wheel;
            
            // NOTE(allen): Application Core Update
            Application_Step_Result result = {};
            if (app.step != 0){
                result = app.step(&linuxvars.tctx, &render_target, base_ptr, &input);
            }
            
            // NOTE(allen): Finish the Loop
            if (result.perform_kill){
                GlobalRunning = false;
                break;
            }
            
#if 0
            // NOTE(NAME): Switch to New Title
            if (result.has_new_title){
                XStoreName(linuxvars.X11.dpy, linuxvars.X11.win, result.title_string);
            }
            
            // NOTE(allen): Switch to New Cursor
            if (result.mouse_cursor_type != linuxvars.cursor && !linuxvars.input.pers.mouse_l){
                XCursor c = linuxvars.X11.xcursors[result.mouse_cursor_type];
                if (linuxvars.cursor_show){
                    XDefineCursor(linuxvars.X11.dpy, linuxvars.X11.win, c);
                }
                linuxvars.cursor = result.mouse_cursor_type;
            }
#endif
            
            // NOTE(allen): Switch to New Cursor
            if (result.mouse_cursor_type != linuxvars.cursor && !linuxvars.input.pers.mouse_l)
            {
                u32 Shape = WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_DEFAULT;
                switch(result.mouse_cursor_type)
                {
                    case APP_MOUSE_CURSOR_IBEAM:
                    {
                        Shape = WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_TEXT;
                    } break;
                    
                    case APP_MOUSE_CURSOR_LEFTRIGHT:
                    {
                        Shape = WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_EW_RESIZE;
                    } break;
                    
                    case APP_MOUSE_CURSOR_UPDOWN:
                    {
                        Shape = WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_NS_RESIZE;
                    } break;
                }
                
                wp_cursor_shape_device_v1_set_shape(linuxvars.Wayland.CursorShapeDevice, linuxvars.Wayland.LastPointerEnterSerial, Shape);
                linuxvars.cursor = result.mouse_cursor_type;
            }
            
            gl_render(&render_target);
            
            linuxvars.Wayland.FrameCallback = wl_surface_frame(linuxvars.Wayland.Surface);
            wl_callback_add_listener(linuxvars.Wayland.FrameCallback, &linuxvars.Wayland.FrameCallbackListener, 0);
            eglSwapBuffers(linuxvars.EGL.Display, linuxvars.EGL.Surface);
            linuxvars.Wayland.WaitingForPresent = true;
            
            // TODO(allen): don't let the screen size change until HERE after the render
            
            // NOTE(allen): Schedule a step if necessary
            if (result.animating){
                linux_schedule_step();
            }
            
            first_step = false;
            
            linalloc_clear(&linuxvars.frame_arena);
            block_zero_struct(&linuxvars.input.trans);
        }
    }
    
    system_thread_join(linuxvars.audio_thread);
    
    return 0;
}

// NOTE(inso): to prevent me continuously messing up indentation
// vim: et:ts=4:sts=4:sw=4
