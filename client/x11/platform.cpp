/*
   Copyright (C) 2009 Red Hat, Inc.

   This program is free software; you can redistribute it and/or
   modify it under the terms of the GNU General Public License as
   published by the Free Software Foundation; either version 2 of
   the License, or (at your option) any later version.

   This program is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
   GNU General Public License for more details.

   You should have received a copy of the GNU General Public License
   along with this program.  If not, see <http://www.gnu.org/licenses/>.
*/

#include "common.h"

#include <X11/Xlib.h>
#include <X11/Xutil.h>
#include <X11/XKBlib.h>
#include <X11/Xresource.h>
#include <X11/cursorfont.h>
#include <X11/extensions/Xrandr.h>
#include <X11/extensions/render.h>
#include <X11/extensions/XKB.h>
#include <X11/extensions/Xrender.h>
#include <unistd.h>
#include <sys/epoll.h>
#include <sys/resource.h>
#include <sys/types.h>
#include <sys/syscall.h>
#include <unistd.h>
#include <fcntl.h>
#include <set>
#include <values.h>
#include <signal.h>
#include <config.h>

#include "platform.h"
#include "application.h"
#include "utils.h"
#include "x_platform.h"
#include "debug.h"
#include "monitor.h"
#include "rect.h"
#include "record.h"
#include "playback.h"
#include "resource.h"
#include "res.h"
#include "cursor.h"
#include "process_loop.h"

#define DWORD uint32_t
#define BOOL bool
#include "named_pipe.h"

//#define X_DEBUG_SYNC(display) XSync(display, False)
#define X_DEBUG_SYNC(display)
#ifdef HAVE_XRANDR12
#define USE_XRANDR_1_2
#endif

static Display* x_display = NULL;
static XVisualInfo **vinfo = NULL;
static GLXFBConfig **fb_config;

static XContext win_proc_context;
static ProcessLoop* main_loop = NULL;
static int focus_count = 0;

static bool using_xrandr_1_0 = false;
#ifdef USE_XRANDR_1_2
static bool using_xrandr_1_2 = false;
#endif

static int xrandr_event_base;
static int xrandr_error_base;
static int xrandr_major = 0;
static int xrandr_minor = 0;

static bool using_xrender_0_5 = false;

static unsigned int caps_lock_mask = 0;
static unsigned int num_lock_mask = 0;

class DefaultEventListener: public Platform::EventListener {
public:
    virtual void on_app_activated() {}
    virtual void on_app_deactivated() {}
    virtual void on_monitors_change() {}
};

static DefaultEventListener default_event_listener;
static Platform::EventListener* event_listener = &default_event_listener;

class DefaultDisplayModeListner: public Platform::DisplayModeListner {
public:
    void on_display_mode_change() {}
};

static DefaultDisplayModeListner default_display_mode_listener;
static Platform::DisplayModeListner* display_mode_listener = &default_display_mode_listener;


NamedPipe::ListenerRef NamedPipe::create(const char *name, ListenerInterface& listener_interface)
{
    ASSERT(main_loop && main_loop->is_same_thread(pthread_self()));
    return (ListenerRef)(new LinuxListener(name, listener_interface, *main_loop));
}

void NamedPipe::destroy(ListenerRef listener_ref)
{
    ASSERT(main_loop && main_loop->is_same_thread(pthread_self()));
    delete (LinuxListener *)listener_ref;
}

void NamedPipe::destroy_connection(ConnectionRef conn_ref)
{
    ASSERT(main_loop && main_loop->is_same_thread(pthread_self()));
    delete (Session *)conn_ref;
}

int32_t NamedPipe::read(ConnectionRef conn_ref, uint8_t* buf, int32_t size)
{
    if (((Session *)conn_ref) != NULL) {
        return ((Session *)conn_ref)->read(buf, size);
    }
    return -1;
}

int32_t NamedPipe::write(ConnectionRef conn_ref, const uint8_t* buf, int32_t size)
{
    if (((Session *)conn_ref) != NULL) {
        return ((Session *)conn_ref)->write(buf, size);
    }
    return -1;
}

class XEventHandler: public EventSources::File {
public:
    XEventHandler(Display& x_display, XContext& win_proc_context);
    virtual void on_event();
    virtual int get_fd() {return _x_fd;}

private:
    Display& _x_display;
    XContext& _win_proc_context;
    int _x_fd;
};

XEventHandler::XEventHandler(Display& x_display, XContext& win_proc_context)
    : _x_display (x_display)
    , _win_proc_context (win_proc_context)
{
    if ((_x_fd = ConnectionNumber(&x_display)) == -1) {
        THROW("get x fd failed");
    }
}

void XEventHandler::on_event()
{
    while (XPending(&_x_display)) {
        XPointer proc_pointer;
        XEvent event;

        XNextEvent(&_x_display, &event);
        if (event.xany.window == None) {
            LOG_WARN("invalid window");
            continue;
        }

        if (XFindContext(&_x_display, event.xany.window, _win_proc_context, &proc_pointer)) {
            THROW("no window proc");
        }
        ((XPlatform::win_proc_t)proc_pointer)(event);
    }
}

Display* XPlatform::get_display()
{
    return x_display;
}

XVisualInfo** XPlatform::get_vinfo()
{
    return vinfo;
}

GLXFBConfig** XPlatform::get_fbconfig()
{
    return fb_config;
}

void XPlatform::set_win_proc(Window win, win_proc_t proc)
{
    if (XSaveContext(x_display, win, win_proc_context, (XPointer)proc)) {
        THROW("set win proc pailed");
    }
}

void XPlatform::cleare_win_proc(Window win)
{
    XDeleteContext(x_display, win, win_proc_context);
}

void Platform::send_quit_request()
{
    ASSERT(main_loop);
    main_loop->quit(0);
}

uint64_t Platform::get_monolithic_time()
{
    struct timespec time_space;
    clock_gettime(CLOCK_MONOTONIC, &time_space);
    return uint64_t(time_space.tv_sec) * 1000 * 1000 * 1000 + uint64_t(time_space.tv_nsec);
}

void Platform::get_temp_dir(std::string& path)
{
    path = "/tmp/";
}

void Platform::msleep(unsigned int millisec)
{
    usleep(millisec * 1000);
}

void Platform::yield()
{
    pthread_yield();
}

void Platform::set_thread_priority(void* thread, Platform::ThreadPriority in_priority)
{
    ASSERT(thread == NULL);
    int priority;

    switch (in_priority) {
    case PRIORITY_TIME_CRITICAL:
        priority = -20;
        break;
    case PRIORITY_HIGH:
        priority = -2;
        break;
    case PRIORITY_ABOVE_NORMAL:
        priority = -1;
        break;
    case PRIORITY_NORMAL:
        priority = 0;
        break;
    case PRIORITY_BELOW_NORMAL:
        priority = 1;
        break;
    case PRIORITY_LOW:
        priority = 2;
        break;
    case PRIORITY_IDLE:
        priority = 19;
        break;
    default:
        THROW("invalid priority %d", in_priority);
    }

    pid_t tid = syscall(SYS_gettid);
    if (setpriority(PRIO_PROCESS, tid, priority) == -1) {
        DBG(0, "setpriority failed %s", strerror(errno));
    }
}

void Platform::set_event_listener(EventListener* listener)
{
    event_listener = listener ? listener : &default_event_listener;
}

void Platform::set_display_mode_listner(DisplayModeListner* listener)
{
    display_mode_listener = listener ? listener : &default_display_mode_listener;
}

#ifdef USE_XRANDR_1_2
class FreeScreenResources {
public:
    void operator () (XRRScreenResources* res) { XRRFreeScreenResources(res);}
};
typedef _AutoRes<XRRScreenResources, FreeScreenResources> AutoScreenRes;

class FreeOutputInfo {
public:
    void operator () (XRROutputInfo* output_info) { XRRFreeOutputInfo(output_info);}
};

typedef _AutoRes<XRROutputInfo, FreeOutputInfo> AutoOutputInfo;

class FreeCrtcInfo {
public:
    void operator () (XRRCrtcInfo* crtc_info) { XRRFreeCrtcInfo(crtc_info);}
};
typedef _AutoRes<XRRCrtcInfo, FreeCrtcInfo> AutoCrtcInfo;

static XRRModeInfo* find_mod(XRRScreenResources* res, RRMode mode)
{
    for (int i = 0; i < res->nmode; i++) {
        if (res->modes[i].id == mode) {
            return &res->modes[i];
        }
    }
    return NULL;
}

#endif

//#define SHOW_SCREEN_INFO
#ifdef SHOW_SCREEN_INFO

static float mode_refresh(XRRModeInfo *mode_info)
{
    if (!mode_info->hTotal || !mode_info->vTotal) {
        return 0;
    }

    return ((float)mode_info->dotClock / ((float)mode_info->hTotal * (float)mode_info->vTotal));
}

static void show_scren_info()
{
    int screen = DefaultScreen(x_display);
    Window root_window = RootWindow(x_display, screen);

    int minWidth;
    int minHeight;
    int maxWidth;
    int maxHeight;

    AutoScreenRes res(XRRGetScreenResources(x_display, root_window));

    if (!res.valid()) {
        throw Exception(fmt("%s: get screen resources failed") % __FUNCTION__);
    }

    XRRGetScreenSizeRange(x_display, root_window, &minWidth, &minHeight,
                          &maxWidth, &maxHeight);
    printf("screen: min %dx%d max %dx%d\n", minWidth, minHeight,
           maxWidth, maxHeight);

    int i, j;

    for (i = 0; i < res->noutput; i++) {
        AutoOutputInfo output_info(XRRGetOutputInfo(x_display, res.get(), res->outputs[i]));

        printf("output %s", output_info->name);
        if (output_info->crtc == None) {
            printf(" crtc None");
        } else {
            printf(" crtc 0x%lx", output_info->crtc);
        }
        switch (output_info->connection) {
        case RR_Connected:
            printf(" Connected");
            break;
        case RR_Disconnected:
            printf(" Disconnected");
            break;
        case RR_UnknownConnection:
            printf(" UnknownConnection");
            break;
        }
        printf(" ncrtc %u nclone %u nmode %u\n",
               output_info->ncrtc,
               output_info->nclone,
               output_info->nmode);
        for (j = 0; j < output_info->nmode; j++) {
            XRRModeInfo* mode = find_mod(res.get(), output_info->modes[j]);
            printf("\t%lu:", output_info->modes[j]);
            if (!mode) {
                printf(" ???\n");
                continue;
            }
            printf(" %s %ux%u %f\n", mode->name, mode->width, mode->height, mode_refresh(mode));
        }
    }

    for (i = 0; i < res->ncrtc; i++) {
        AutoCrtcInfo crtc_info(XRRGetCrtcInfo(x_display, res.get(), res->crtcs[i]));
        printf("crtc: 0x%lx x %d y %d  width %u height %u  mode %lu\n",
               res->crtcs[i],
               crtc_info->x, crtc_info->y,
               crtc_info->width, crtc_info->height, crtc_info->mode);
    }
}

#endif

enum RedScreenRotation {
    RED_SCREEN_ROTATION_0,
    RED_SCREEN_ROTATION_90,
    RED_SCREEN_ROTATION_180,
    RED_SCREEN_ROTATION_270,
};

enum RedSubpixelOrder {
    RED_SUBPIXEL_ORDER_UNKNOWN,
    RED_SUBPIXEL_ORDER_H_RGB,
    RED_SUBPIXEL_ORDER_H_BGR,
    RED_SUBPIXEL_ORDER_V_RGB,
    RED_SUBPIXEL_ORDER_V_BGR,
    RED_SUBPIXEL_ORDER_NONE,
};

static void root_win_proc(XEvent& event);
static void process_monitor_configure_events(Window root);

class XMonitor;
typedef std::list<XMonitor*> XMonitorsList;

class XScreen {
public:
    XScreen(Display* display, int screen);
    virtual ~XScreen() {}

    virtual void publish_monitors(MonitorsList& monitors) = 0;

    Display* get_display() {return _display;}
    int get_screen() {return _screen;}

    void set_broken() {_broken = true;}
    bool is_broken() const {return _broken;}
    int get_width() const {return _width;}
    void set_width(int width) {_width = width;}
    int get_height() const { return _height;}
    void set_height(int height) {_height = height;}
    Point get_position() const {return _position;}

private:
    Display* _display;
    int _screen;
    Point _position;
    int _width;
    int _height;
    bool _broken;
};

XScreen::XScreen(Display* display, int screen)
    : _display (display)
    , _screen (screen)
    , _broken (false)
{
    int root = RootWindow(display, screen);

    XWindowAttributes attrib;
    XGetWindowAttributes(display, root, &attrib);

    _position.x = attrib.x;
    _position.y = attrib.y;
    _width = attrib.width;
    _height = attrib.height;
}

class StaticScreen: public XScreen, public Monitor {
public:
    StaticScreen(Display* display, int screen, int& next_mon_id)
        : XScreen(display, screen)
        , Monitor(next_mon_id++)
        , _out_of_sync (false)
    {
    }

    virtual void publish_monitors(MonitorsList& monitors)
    {
        monitors.push_back(this);
    }

    virtual void set_mode(int width, int height)
    {
        _out_of_sync = width > get_width() || height > get_height();
    }

    virtual void restore() {}
    virtual int get_depth() { return XPlatform::get_vinfo()[0]->depth;}
    virtual Point get_position() { return XScreen::get_position();}
    virtual Point get_size() const { Point pt = {get_width(), get_height()}; return pt;}
    virtual bool is_out_of_sync() { return _out_of_sync;}
    virtual int get_screen_id() { return get_screen();}

private:
    bool _out_of_sync;
};

class DynamicScreen: public XScreen, public Monitor {
public:
    DynamicScreen(Display* display, int screen, int& next_mon_id);
    virtual ~DynamicScreen();

    void publish_monitors(MonitorsList& monitors);
    virtual void set_mode(int width, int height);
    virtual void restore();
    virtual int get_depth() { return XPlatform::get_vinfo()[0]->depth;}
    virtual Point get_position() { return XScreen::get_position();}
    virtual Point get_size() const { Point pt = {get_width(), get_height()}; return pt;}
    virtual bool is_out_of_sync() { return _out_of_sync;}
    virtual int get_screen_id() { return get_screen();}

private:
    bool set_screen_size(int size_index);

private:
    int _saved_width;
    int _saved_height;
    bool _out_of_sync;
};

DynamicScreen::DynamicScreen(Display* display, int screen, int& next_mon_id)
    : XScreen(display, screen)
    , Monitor(next_mon_id++)
    , _saved_width (get_width())
    , _saved_height (get_height())
    , _out_of_sync (false)
{
    X_DEBUG_SYNC(display);
    Window root_window = RootWindow(display, screen);
    XSelectInput(display, root_window, StructureNotifyMask);
    XRRSelectInput(display, root_window, RRScreenChangeNotifyMask);
    XPlatform::set_win_proc(root_window, root_win_proc);
    X_DEBUG_SYNC(display);
}

DynamicScreen::~DynamicScreen()
{
    restore();
}

void DynamicScreen::publish_monitors(MonitorsList& monitors)
{
    monitors.push_back(this);
}

class SizeInfo {
public:
    SizeInfo(int int_index, XRRScreenSize* in_size) : index (int_index), size (in_size) {}

    int index;
    XRRScreenSize* size;
};

class SizeCompare {
public:
    bool operator () (const SizeInfo& size1, const SizeInfo& size2) const
    {
        int area1 = size1.size->width * size1.size->height;
        int area2 = size2.size->width * size2.size->height;
        return area1 < area2 || (area1 == area2 && size1.index < size2.index);
    }
};

void DynamicScreen::set_mode(int width, int height)
{
    int num_sizes;

    X_DEBUG_SYNC(get_display());
    XRRScreenSize* sizes = XRRSizes(get_display(), get_screen(), &num_sizes);

    typedef std::set<SizeInfo, SizeCompare> SizesSet;
    SizesSet sizes_set;

    for (int i = 0; i < num_sizes; i++) {
        if (sizes[i].width >= width && sizes[i].height >= height) {
            sizes_set.insert(SizeInfo(i, &sizes[i]));
        }
    }
    _out_of_sync = true;
    if (!sizes_set.empty() && set_screen_size((*sizes_set.begin()).index)) {
        _out_of_sync = false;
    }
    X_DEBUG_SYNC(get_display());
}

void DynamicScreen::restore()
{
    X_DEBUG_SYNC(get_display());
    if (is_broken() || (get_width() == _saved_width && get_height() == _saved_height)) {
        return;
    }
    int num_sizes;

    XRRScreenSize* sizes = XRRSizes(get_display(), get_screen(), &num_sizes);
    for (int i = 0; i < num_sizes; i++) {
        if (sizes[i].width == _saved_width && sizes[i].height == _saved_height) {
            set_screen_size(i);
            return;
        }
    }
    X_DEBUG_SYNC(get_display());
    LOG_WARN("can't find startup mode");
}

bool DynamicScreen::set_screen_size(int size_index)
{
    X_DEBUG_SYNC(get_display());
    Window root_window = RootWindow(get_display(), get_screen());
    XRRScreenConfiguration* config;

    if (!(config = XRRGetScreenInfo(get_display(), root_window))) {
        LOG_WARN("get scren info failed");
        return false;
    }
    Rotation rotation;
    XRRConfigCurrentConfiguration(config, &rotation);

    Monitor::self_monitors_change++;
    /*what status*/
    XRRSetScreenConfig(get_display(), config, root_window, size_index, rotation, CurrentTime);
    process_monitor_configure_events(root_window);
    Monitor::self_monitors_change--;
    XRRFreeScreenConfigInfo(config);
    X_DEBUG_SYNC(get_display());

    int num_sizes;
    XRRScreenSize* sizes = XRRSizes(get_display(), get_screen(), &num_sizes);
    if (num_sizes <= size_index) {
        THROW("invalid sizes size");
    }
    set_width(sizes[size_index].width);
    set_height(sizes[size_index].height);
    return true;
}

#ifdef USE_XRANDR_1_2

class MultyMonScreen: public XScreen {
public:
    MultyMonScreen(Display* display, int screen, int& next_mon_id);
    virtual ~MultyMonScreen();

    virtual void publish_monitors(MonitorsList& monitors);

    void disable();
    void enable();

    bool set_monitor_mode(XMonitor& monitor, const XRRModeInfo& mode_info);

private:
    void set_size(int width, int height);
    void get_trans_size(int& width, int& hight);
    Point get_trans_top_left();
    Point get_trans_bottom_right();
    bool changed();

    XMonitor* crtc_overlap_test(int x, int y, int width, int height);
    void monitors_cleanup();
    void restore();

private:
    int _min_width;
    int _min_height;
    int _max_width;
    int _max_height;
    int _saved_width;
    int _saved_height;
    int _saved_width_mm;
    int _saved_height_mm;
    XMonitorsList _monitors;
};

#define MAX_TRANS_DEPTH 3

class XMonitor: public Monitor {
public:
    XMonitor(MultyMonScreen& container, int id, RRCrtc crtc);
    virtual ~XMonitor();

    virtual void set_mode(int width, int height);
    virtual void restore();
    virtual int get_depth();
    virtual Point get_position();
    virtual Point get_size() const;
    virtual bool is_out_of_sync();
    virtual int get_screen_id() { return _container.get_screen();}

    void add_clone(XMonitor *clone);
    void revert();
    void disable();
    void enable();

    void set_mode(const XRRModeInfo& mode);
    const Rect& get_prev_area();
    Rect& get_trans_area();
    void pin() { _pin_count++;}
    void unpin() { ASSERT(_pin_count > 0); _pin_count--;}
    bool is_pinned() {return !!_pin_count;}
    void commit_trans_position();
    void set_pusher(XMonitor& pusher) { _pusher = &pusher;}
    XMonitor* get_pusher() { return _pusher;}
    void push_trans();
    void begin_trans();
    bool mode_changed();
    bool position_changed();

    static void inc_change_ref() { Monitor::self_monitors_change++;}
    static void dec_change_ref() { Monitor::self_monitors_change--;}

private:
    void update_position();
    bool finde_mode_in_outputs(RRMode mode, int start_index, XRRScreenResources* res);
    bool finde_mode_in_clones(RRMode mode, XRRScreenResources* res);
    XRRModeInfo* find_mode(int width, int height, XRRScreenResources* res);

private:
    MultyMonScreen& _container;
    RRCrtc _crtc;
    XMonitorsList _clones;
    Point _position;
    Point _size;
    RRMode _mode;
    Rotation _rotation;
    int _noutput;
    RROutput* _outputs;

    Point _saved_position;
    Point _saved_size;
    RRMode _saved_mode;
    Rotation _saved_rotation;

    bool _out_of_sync;
    RedScreenRotation _red_rotation;
    RedSubpixelOrder _subpixel_order;

    int _trans_depth;
    Rect _trans_area[MAX_TRANS_DEPTH];
    int _pin_count;
    XMonitor* _pusher;
};

MultyMonScreen::MultyMonScreen(Display* display, int screen, int& next_mon_id)
    : XScreen(display, screen)
    , _saved_width (get_width())
    , _saved_height (get_height())
    , _saved_width_mm (DisplayWidthMM(display, screen))
    , _saved_height_mm (DisplayHeightMM(display, screen))
{
    X_DEBUG_SYNC(get_display());
    Window root_window = RootWindow(display, screen);
    XRRGetScreenSizeRange(display, root_window, &_min_width, &_min_height,
                          &_max_width, &_max_height);

    AutoScreenRes res(XRRGetScreenResources(display, root_window));
    if (!res.valid()) {
        THROW("get screen resources failed");
    }

#ifdef SHOW_SCREEN_INFO
    show_scren_info();
#endif
    try {
        for (int i = 0; i < res->ncrtc; i++) {
            AutoCrtcInfo crtc_info(XRRGetCrtcInfo(display, res.get(), res->crtcs[i]));

            if (!crtc_info.valid()) {
                THROW("get crtc info failed");
            }

            if (crtc_info->mode == None) {
                continue;
            }

            ASSERT(crtc_info->noutput);

            XMonitor* clone_mon = crtc_overlap_test(crtc_info->x, crtc_info->y,
                                                    crtc_info->width, crtc_info->height);

            if (clone_mon) {
                clone_mon->add_clone(new XMonitor(*this, next_mon_id++, res->crtcs[i]));
                continue;
            }

            _monitors.push_back(new XMonitor(*this, next_mon_id++, res->crtcs[i]));
        }
    } catch (...) {
        monitors_cleanup();
        throw;
    }

    XSelectInput(display, root_window, StructureNotifyMask);
    XRRSelectInput(display, root_window, RRScreenChangeNotifyMask);
    XPlatform::set_win_proc(root_window, root_win_proc);
    X_DEBUG_SYNC(get_display());
}

MultyMonScreen::~MultyMonScreen()
{
    restore();
    monitors_cleanup();
}

XMonitor* MultyMonScreen::crtc_overlap_test(int x, int y, int width, int height)
{
    XMonitorsList::iterator iter = _monitors.begin();
    for (; iter != _monitors.end(); iter++) {
        XMonitor* mon = *iter;

        Point pos = mon->get_position();
        Point size = mon->get_size();

        if (x == pos.x && y == pos.y && width == size.x && height == size.y) {
            return mon;
        }

        if (x < pos.x + size.x && x + width > pos.x && y < pos.y + size.y && y + height > pos.y) {
            THROW("unsupported partial overlap");
        }
    }
    return NULL;
}

void MultyMonScreen::publish_monitors(MonitorsList& monitors)
{
    XMonitorsList::iterator iter = _monitors.begin();
    for (; iter != _monitors.end(); iter++) {
        monitors.push_back(*iter);
    }
}

void MultyMonScreen::monitors_cleanup()
{
    while (!_monitors.empty()) {
        XMonitor* monitor = _monitors.front();
        _monitors.pop_front();
        delete monitor;
    }
}

void MultyMonScreen::disable()
{
    XMonitorsList::iterator iter = _monitors.begin();
    for (; iter != _monitors.end(); iter++) {
        (*iter)->disable();
    }
}

void MultyMonScreen::enable()
{
    XMonitorsList::iterator iter = _monitors.begin();
    for (; iter != _monitors.end(); iter++) {
        (*iter)->enable();
    }
}

void MultyMonScreen::set_size(int width, int height)
{
    X_DEBUG_SYNC(get_display());
    Window root_window = RootWindow(get_display(), get_screen());
    set_width(width);
    int width_mm = (int)((double)_saved_width_mm / _saved_width * width);
    set_height(height);
    int height_mm = (int)((double)_saved_height_mm / _saved_height * height);
    XRRSetScreenSize(get_display(), root_window, width, height, width_mm, height_mm);
    X_DEBUG_SYNC(get_display());
}

bool MultyMonScreen::changed()
{
    if (get_width() != _saved_width || get_height() != _saved_height) {
        return true;
    }

    XMonitorsList::iterator iter = _monitors.begin();
    for (; iter != _monitors.end(); iter++) {
        if ((*iter)->mode_changed() || (*iter)->position_changed()) {
            return true;
        }
    }
    return false;
}

void MultyMonScreen::restore()
{
    if (is_broken() || !changed()) {
        return;
    }
    X_DEBUG_SYNC(get_display());
    XMonitor::inc_change_ref();
    disable();
    Window root_window = RootWindow(get_display(), get_screen());

    XRRSetScreenSize(get_display(), root_window, _saved_width,
                     _saved_height,
                     _saved_width_mm, _saved_height_mm);
    XMonitorsList::iterator iter = _monitors.begin();
    for (; iter != _monitors.end(); iter++) {
        (*iter)->revert();
    }
    enable();
    process_monitor_configure_events(root_window);
    XMonitor::dec_change_ref();
    X_DEBUG_SYNC(get_display());
}

Point MultyMonScreen::get_trans_top_left()
{
    Point position;
    position.y = position.x = MAXINT;

    XMonitorsList::iterator iter = _monitors.begin();
    for (; iter != _monitors.end(); iter++) {
        Rect& area = (*iter)->get_trans_area();
        position.x = MIN(position.x, area.left);
        position.y = MIN(position.y, area.top);
    }
    return position;
}

Point MultyMonScreen::get_trans_bottom_right()
{
    Point position;
    position.y = position.x = MININT;

    XMonitorsList::iterator iter = _monitors.begin();
    for (; iter != _monitors.end(); iter++) {
        Rect& area = (*iter)->get_trans_area();
        position.x = MAX(position.x, area.right);
        position.y = MAX(position.y, area.bottom);
    }
    return position;
}

void MultyMonScreen::get_trans_size(int& width, int& height)
{
    ASSERT(get_trans_top_left().x == 0 && get_trans_top_left().y == 0);
    Point bottom_right = get_trans_bottom_right();
    ASSERT(bottom_right.x > 0 && bottom_right.y > 0);
    width = bottom_right.x;
    height = bottom_right.y;
}

#endif

/*class Variant {
    static void get_area_in_front(const Rect& base, int size, Rect& area)
    static int get_push_distance(const Rect& fix_area, const Rect& other)
    static int get_head(const Rect& area)
    static int get_tail(const Rect& area)
    static void move_head(Rect& area, int delta)
    static int get_pull_distance(const Rect& fix_area, const Rect& other)
    static void offset(Rect& area, int delta)
    static void shrink(Rect& area, int delta)
    static int get_distance(const Rect& area, const Rect& other_area)
    static bool is_on_tail(const Rect& area, const Rect& other_area)
    static bool is_on_perpendiculars(const Rect& area, const Rect& other_area)
}*/

#ifdef USE_XRANDR_1_2

class SortRightToLeft {
public:
    bool operator () (XMonitor* mon1, XMonitor* mon2) const
    {
        return mon1->get_trans_area().right > mon2->get_trans_area().right;
    }
};

typedef std::multiset<XMonitor*, SortRightToLeft> PushLeftSet;

class LeftVariant {
public:

    static void get_area_in_front(const Rect& base, int size, Rect& area)
    {
        area.right = base.left;
        area.left = area.right - size;
        area.bottom = base.bottom;
        area.top = base.top;
    }

    static int get_push_distance(const Rect& fix_area, const Rect& other)
    {
        return other.right - fix_area.left;
    }

    static int get_head(const Rect& area)
    {
        return area.left;
    }

    static int get_tail(const Rect& area)
    {
        return area.right;
    }

    static void move_head(Rect& area, int delta)
    {
        area.left -= delta;
        ASSERT(area.right >= area.left);
    }

    static int get_pull_distance(const Rect& fix_area, const Rect& other)
    {
        return other.left - fix_area.right;
    }

    static void offset(Rect& area, int delta)
    {
        rect_offset(area, -delta, 0);
    }

    static void shrink(Rect& area, int delta)
    {
        area.right -= delta;
        ASSERT(area.right > area.left);
    }

    static int get_distance(const Rect& area, const Rect& other_area)
    {
        return other_area.left - area.left;
    }

    static bool is_on_tail(const Rect& area, const Rect& other_area)
    {
        return area.right == other_area.left && other_area.top < area.bottom &&
               other_area.bottom > area.top;
    }

    static bool is_on_perpendiculars(const Rect& area, const Rect& other_area)
    {
        return (other_area.bottom == area.top || other_area.top == area.bottom) &&
               other_area.left < area.right && other_area.right > area.left;
    }
};

class SortLeftToRight {
public:
    bool operator () (XMonitor* mon1, XMonitor* mon2) const
    {
        return mon1->get_trans_area().left < mon2->get_trans_area().left;
    }
};

typedef std::multiset<XMonitor*, SortLeftToRight> PushRightSet;

class RightVariant {
public:

    static void get_area_in_front(const Rect& base, int size, Rect& area)
    {
        area.left = base.right;
        area.right = area.left + size;
        area.top = base.top;
        area.bottom = base.bottom;
    }

    static int get_push_distance(const Rect& fix_area, const Rect& other)
    {
        return fix_area.right - other.left;
    }

    static int get_head(const Rect& area)
    {
        return area.right;
    }

    static int get_tail(const Rect& area)
    {
        return area.left;
    }

    static void move_head(Rect& area, int delta)
    {
        area.right += delta;
        ASSERT(area.right >= area.left);
    }

    static int get_pull_distance(const Rect& fix_area, const Rect& other)
    {
        return fix_area.left - other.right;
    }

    static void offset(Rect& area, int delta)
    {
        rect_offset(area, delta, 0);
    }

    static bool is_on_tail(const Rect& area, const Rect& other_area)
    {
        return other_area.right == area.left && other_area.top < area.bottom &&
               other_area.bottom > area.top;
    }

    static bool is_on_perpendiculars(const Rect& area, const Rect& other_area)
    {
        return (other_area.bottom == area.top || other_area.top == area.bottom) &&
               other_area.left < area.right && other_area.right > area.left;
    }
};

class SortBottomToTop {
public:
    bool operator () (XMonitor* mon1, XMonitor* mon2) const
    {
        return mon1->get_trans_area().bottom > mon2->get_trans_area().bottom;
    }
};

typedef std::multiset<XMonitor*, SortBottomToTop> PushTopSet;

class TopVariant {
public:
    static void get_area_in_front(const Rect& base, int size, Rect& area)
    {
        area.left = base.left;
        area.right = base.right;
        area.bottom = base.top;
        area.top = area.bottom - size;
    }

    static int get_push_distance(const Rect& fix_area, const Rect& other)
    {
        return other.bottom - fix_area.top;
    }

    static int get_head(const Rect& area)
    {
        return area.top;
    }

    static int get_tail(const Rect& area)
    {
        return area.bottom;
    }

    static void move_head(Rect& area, int delta)
    {
        area.top -= delta;
        ASSERT(area.bottom >= area.top);
    }

    static int get_pull_distance(const Rect& fix_area, const Rect& other)
    {
        return other.top - fix_area.bottom;
    }

    static void offset(Rect& area, int delta)
    {
        rect_offset(area, 0, -delta);
    }

    static void shrink(Rect& area, int delta)
    {
        area.bottom -= delta;
        ASSERT(area.bottom > area.top);
    }

    static int get_distance(const Rect& area, const Rect& other_area)
    {
        return other_area.top - area.top;
    }

    static bool is_on_tail(const Rect& area, const Rect& other_area)
    {
        return area.bottom == other_area.top && other_area.left < area.right &&
               other_area.right > area.left;
    }

    static bool is_on_perpendiculars(const Rect& area, const Rect& other_area)
    {
        return (other_area.right == area.left || other_area.left == area.right) &&
               other_area.top < area.bottom && other_area.bottom > area.top;
    }
};

class SortTopToBottom {
public:
    bool operator () (XMonitor* mon1, XMonitor* mon2) const
    {
        return mon1->get_trans_area().top < mon2->get_trans_area().top;
    }
};

typedef std::multiset<XMonitor*, SortTopToBottom> PushBottomSet;

class BottomVariant {
public:

    static void get_area_in_front(const Rect& base, int size, Rect& area)
    {
        area.left = base.left;
        area.right = base.right;
        area.top = base.bottom;
        area.bottom = area.top + size;
    }

    static int get_push_distance(const Rect& fix_area, const Rect& other)
    {
        return fix_area.bottom - other.top;
    }

    static int get_head(const Rect& area)
    {
        return area.bottom;
    }

    static int get_tail(const Rect& area)
    {
        return area.top;
    }

    static void move_head(Rect& area, int delta)
    {
        area.bottom += delta;
        ASSERT(area.bottom >= area.top);
    }

    static int get_pull_distance(const Rect& fix_area, const Rect& other)
    {
        return fix_area.top - other.bottom;
    }

    static void offset(Rect& area, int delta)
    {
        rect_offset(area, 0, delta);
    }

    static bool is_on_tail(const Rect& area, const Rect& other_area)
    {
        return other_area.bottom == area.top && other_area.left < area.right &&
               other_area.right > area.left;
    }

    static bool is_on_perpendiculars(const Rect& area, const Rect& other_area)
    {
        return (other_area.right == area.left || other_area.left == area.right) &&
               other_area.top < area.bottom && other_area.bottom > area.top;
    }
};

volatile int wait_for_me = false;

template <class Variant>
static void bounce_back(XMonitor& monitor, const XMonitorsList& monitors, int head, int distance)
{
    ASSERT(distance > 0);
    while (wait_for_me);

    for (XMonitorsList::const_iterator iter = monitors.begin(); iter != monitors.end(); iter++) {
        Rect& area = (*iter)->get_trans_area();
        if (Variant::get_tail(area) == head && (*iter)->get_pusher() == &monitor) {
            Variant::offset(area, -distance);
            bounce_back<Variant>(**iter, monitors, Variant::get_head(area) + distance, distance);
            //todo: pull_back monitors on perpendiculars
        }
    }
}

template <class Variant, class SortList, class SortListIter>
static int push(XMonitor& pusher, XMonitor& monitor, const XMonitorsList& monitors, int delta)
{
    monitor.pin();
    monitor.set_pusher(pusher);

    SortList sort;
    XMonitorsList::const_iterator iter = monitors.begin();
    for (; iter != monitors.end(); iter++) {
        if (*iter == &monitor) {
            continue;
        }
        sort.insert(*iter);
    }

    Rect area_to_clear;
    Variant::get_area_in_front(monitor.get_trans_area(), delta, area_to_clear);

    SortListIter sort_iter = sort.begin();

    for (; sort_iter != sort.end(); sort_iter++) {
        const Rect& other_area = (*sort_iter)->get_trans_area();

        if (rect_intersects(area_to_clear, other_area)) {
            int distance = Variant::get_push_distance(area_to_clear, other_area);
            ASSERT(distance > 0);
            if (!(*sort_iter)->is_pinned()) {
                distance = distance - push<Variant, SortList, SortListIter>(monitor, **sort_iter,
                                                                            monitors, distance);
            }

            if (distance) {
                delta -= distance;
                bounce_back<Variant>(monitor, monitors, Variant::get_head(area_to_clear), distance);
                Variant::move_head(area_to_clear, -distance);
            }
        }
    }
    Variant::offset(monitor.get_trans_area(), delta);

    const Rect& area = monitor.get_prev_area();
    for (iter = monitors.begin(); iter != monitors.end(); iter++) {
        if ((*iter)->is_pinned()) {
            continue;
        }

        const Rect& other_area = (*iter)->get_prev_area();
        if (Variant::is_on_perpendiculars(area, other_area)) {
            int current_distance = Variant::get_pull_distance(monitor.get_trans_area(),
                                                              (*iter)->get_trans_area());
            int base_distance = Variant::get_pull_distance(area, other_area);
            int distance = current_distance - base_distance;
            if (distance > 0) {
                push<Variant, SortList, SortListIter>(monitor, **iter, monitors, distance);
            }
        } else if (Variant::is_on_tail(area, other_area)) {
            int distance = Variant::get_pull_distance(monitor.get_trans_area(),
                                                      (*iter)->get_trans_area());
            ASSERT(distance >= 0);
            push<Variant, SortList, SortListIter>(monitor, **iter, monitors, distance);
        }
    }
    return delta;
}

template <class Variant>
static void pin(XMonitor& monitor, const XMonitorsList& monitors)
{
    const Rect& area = monitor.get_trans_area();

    for (XMonitorsList::const_iterator iter = monitors.begin(); iter != monitors.end(); iter++) {
        const Rect& other_area = (*iter)->get_trans_area();
        if ((*iter)->is_pinned()) {
            continue;
        }
        if (Variant::is_on_tail(other_area, area) ||
                                                  Variant::is_on_perpendiculars(area, other_area)) {
            (*iter)->pin();
            pin<Variant>(**iter, monitors);
        }
    }
}

template <class Variant, class SortList, class SortListIter>
static void shrink(XMonitor& monitor, const XMonitorsList& monitors, int delta)
{
    monitor.pin();
    pin<Variant>(monitor, monitors);
    ASSERT(delta > 0);

    SortList sort;
    XMonitorsList::const_iterator iter = monitors.begin();
    for (; iter != monitors.end(); iter++) {
        if (*iter == &monitor) {
            continue;
        }
        sort.insert(*iter);
    }

    const Rect area = monitor.get_trans_area();
    Variant::shrink(monitor.get_trans_area(), delta);
    for (SortListIter sort_iter = sort.begin(); sort_iter != sort.end(); sort_iter++) {
        const Rect& other_area = (*sort_iter)->get_trans_area();
        if (Variant::is_on_perpendiculars(area, other_area)) {
            int distance = Variant::get_distance(area, other_area);
            if (distance > 0) {
                distance = MIN(distance, delta);
                push<Variant, SortList, SortListIter>(monitor, **sort_iter, monitors, distance);
            }
        } else if (Variant::is_on_tail(area, other_area)) {
            push<Variant, SortList, SortListIter>(monitor, **sort_iter, monitors, delta);
        }
    }
}

template <class Variant, class SortList, class SortListIter>
static void expand(XMonitor& monitor, const XMonitorsList& monitors, int delta)
{
    monitor.pin();
    ASSERT(delta > 0);

    SortList sort;
    XMonitorsList::const_iterator iter = monitors.begin();
    for (; iter != monitors.end(); iter++) {
        if (*iter == &monitor) {
            continue;
        }
        sort.insert(*iter);
    }

    Rect area_to_clear;
    Variant::get_area_in_front(monitor.get_trans_area(), delta, area_to_clear);

    for (SortListIter sort_iter = sort.begin(); sort_iter != sort.end(); sort_iter++) {
        const Rect& other_area = (*sort_iter)->get_trans_area();

        if (rect_intersects(area_to_clear, other_area)) {
            int distance = Variant::get_push_distance(area_to_clear, other_area);
            ASSERT(distance > 0);
            ASSERT(!(*sort_iter)->is_pinned());
#ifdef RED_DEBUG
            int actual =
#endif
            push<Variant, SortList, SortListIter>(monitor, **sort_iter, monitors, distance);
            ASSERT(actual == distance);
        }
    }
    Variant::move_head(monitor.get_trans_area(), delta);
}

bool MultyMonScreen::set_monitor_mode(XMonitor& monitor, const XRRModeInfo& mode_info)
{
    if (is_broken()) {
        return false;
    }

    Point size = monitor.get_size();
    int dx = mode_info.width - size.x;
    int dy = mode_info.height - size.y;

    XMonitorsList::iterator iter = _monitors.begin();

    for (; iter != _monitors.end(); iter++) {
        (*iter)->begin_trans();
    }

    if (dx > 0) {
        expand<RightVariant, PushRightSet, PushRightSet::iterator>(monitor, _monitors, dx);
    } else if (dx < 0) {
        shrink<LeftVariant, PushLeftSet, PushLeftSet::iterator>(monitor, _monitors, -dx);
    }

    for (iter = _monitors.begin(); iter != _monitors.end(); iter++) {
        (*iter)->push_trans();
    }

    if (dy > 0) {
        expand<BottomVariant, PushBottomSet, PushBottomSet::iterator>(monitor, _monitors, dy);
    } else if (dy < 0) {
        shrink<TopVariant, PushTopSet, PushTopSet::iterator>(monitor, _monitors, -dy);
    }

    int screen_width;
    int screen_height;

    get_trans_size(screen_width, screen_height);

    if (screen_width > _max_width || screen_height > _max_height) {
        return false;
    }

    screen_width = MAX(screen_width, _min_width);
    screen_height = MAX(screen_height, _min_height);

    XMonitor::inc_change_ref();
    disable();
    for (iter = _monitors.begin(); iter != _monitors.end(); iter++) {
        (*iter)->commit_trans_position();
    }
    X_DEBUG_SYNC(get_display());
    monitor.set_mode(mode_info);
    set_size(screen_width, screen_height);
    enable();
    Window root_window = RootWindow(get_display(), get_screen());
    process_monitor_configure_events(root_window);
    XMonitor::dec_change_ref();
    X_DEBUG_SYNC(get_display());
    return true;
}

XMonitor::XMonitor(MultyMonScreen& container, int id, RRCrtc crtc)
    : Monitor(id)
    , _container (container)
    , _crtc (crtc)
    , _out_of_sync (false)
{
    update_position();
    _saved_position = _position;
    _saved_size = _size;
    _saved_mode = _mode;
    _saved_rotation = _rotation;
}

XMonitor::~XMonitor()
{
    while (!_clones.empty()) {
        XMonitor* clone = _clones.front();
        _clones.pop_front();
        delete clone;
    }
    delete[] _outputs;
}

void XMonitor::update_position()
{
    Display* display = _container.get_display();
    X_DEBUG_SYNC(display);
    Window root_window = RootWindow(display, _container.get_screen());
    AutoScreenRes res(XRRGetScreenResources(display, root_window));

    if (!res.valid()) {
        THROW("get screen resources failed");
    }

    AutoCrtcInfo crtc_info(XRRGetCrtcInfo(display, res.get(), _crtc));

    ASSERT(crtc_info->noutput);

    _position.x = crtc_info->x;
    _position.y = crtc_info->y;
    _size.x = crtc_info->width;
    _size.y = crtc_info->height;

    switch (crtc_info->rotation & 0xf) {
    case RR_Rotate_0:
        _red_rotation = RED_SCREEN_ROTATION_0;
        break;
    case RR_Rotate_90:
        _red_rotation = RED_SCREEN_ROTATION_90;
        break;
    case RR_Rotate_180:
        _red_rotation = RED_SCREEN_ROTATION_180;
        break;
    case RR_Rotate_270:
        _red_rotation = RED_SCREEN_ROTATION_270;
        break;
    default:
        THROW("invalid rotation");
    }

    if (crtc_info->noutput > 1) {
        //todo: set valid subpixel order in case all outputs share the same type
        _subpixel_order = RED_SUBPIXEL_ORDER_UNKNOWN;
    } else {
        AutoOutputInfo output_info(XRRGetOutputInfo(display, res.get(), crtc_info->outputs[0]));

        switch (output_info->subpixel_order) {
        case SubPixelUnknown:
            _subpixel_order = RED_SUBPIXEL_ORDER_UNKNOWN;
        break;
        case SubPixelHorizontalRGB:
            _subpixel_order = RED_SUBPIXEL_ORDER_H_RGB;
            break;
        case SubPixelHorizontalBGR:
            _subpixel_order = RED_SUBPIXEL_ORDER_H_BGR;
            break;
        case SubPixelVerticalRGB:
            _subpixel_order = RED_SUBPIXEL_ORDER_V_RGB;
            break;
        case SubPixelVerticalBGR:
            _subpixel_order = RED_SUBPIXEL_ORDER_V_BGR;
            break;
        case SubPixelNone:
            _subpixel_order = RED_SUBPIXEL_ORDER_NONE;
            break;
        default:
            THROW("invalid subpixel order");
        }
    }

    _mode = crtc_info->mode;
    _rotation = crtc_info->rotation;
    _noutput = crtc_info->noutput;
    _outputs = new RROutput[_noutput];
    memcpy(_outputs, crtc_info->outputs, _noutput * sizeof(RROutput));
    X_DEBUG_SYNC(display);
}

bool XMonitor::finde_mode_in_outputs(RRMode mode, int start_index, XRRScreenResources* res)
{
    int i, j;

    X_DEBUG_SYNC(_container.get_display());
    for (i = start_index; i < _noutput; i++) {
        AutoOutputInfo output_info(XRRGetOutputInfo(_container.get_display(), res, _outputs[i]));
        for (j = 0; j < output_info->nmode; j++) {
            if (output_info->modes[j] == mode) {
                break;
            }
        }
        if (j == output_info->nmode) {
            return false;
        }
    }
    X_DEBUG_SYNC(_container.get_display());
    return true;
}

bool XMonitor::finde_mode_in_clones(RRMode mode, XRRScreenResources* res)
{
    XMonitorsList::iterator iter = _clones.begin();
    for (; iter != _clones.end(); iter++) {
        if (!(*iter)->finde_mode_in_outputs(mode, 0, res)) {
            return false;
        }
    }
    return true;
}

class ModeInfo {
public:
    ModeInfo(int int_index, XRRModeInfo* in_info) : index (int_index), info (in_info) {}

    int index;
    XRRModeInfo* info;
};

class ModeCompare {
public:
    bool operator () (const ModeInfo& mode1, const ModeInfo& mode2) const
    {
        int area1 = mode1.info->width * mode1.info->height;
        int area2 = mode2.info->width * mode2.info->height;
        return area1 < area2 || (area1 == area2 && mode1.index < mode2.index);
    }
};

XRRModeInfo* XMonitor::find_mode(int width, int height, XRRScreenResources* res)
{
    typedef std::set<ModeInfo, ModeCompare> ModesSet;
    ModesSet modes_set;
    X_DEBUG_SYNC(_container.get_display());
    AutoOutputInfo output_info(XRRGetOutputInfo(_container.get_display(), res, _outputs[0]));
    for (int i = 0; i < output_info->nmode; i++) {
        XRRModeInfo* mode_inf = find_mod(res, output_info->modes[i]);
        if (mode_inf->width >= width && mode_inf->height >= height) {
            modes_set.insert(ModeInfo(i, mode_inf));
        }
    }

    while (!modes_set.empty()) {
        ModesSet::iterator iter = modes_set.begin();

        if (!finde_mode_in_outputs((*iter).info->id, 1, res)) {
            modes_set.erase(iter);
            continue;
        }

        if (!finde_mode_in_clones((*iter).info->id, res)) {
            modes_set.erase(iter);
            continue;
        }
        return (*iter).info;
    }
    X_DEBUG_SYNC(_container.get_display());
    return NULL;
}

void XMonitor::set_mode(int width, int height)
{
    if (width == _size.x && height == _size.y) {
        _out_of_sync = false;
        return;
    }
    Display* display = _container.get_display();
    X_DEBUG_SYNC(display);
    Window root_window = RootWindow(display, _container.get_screen());
    AutoScreenRes res(XRRGetScreenResources(display, root_window));
    if (!res.valid()) {
        THROW("get screen resource failed");
    }
    XRRModeInfo* mode_info = find_mode(width, height, res.get());

    if (!mode_info || !_container.set_monitor_mode(*this, *mode_info)) {
        _out_of_sync = true;
        X_DEBUG_SYNC(display);
        return;
    }
    _out_of_sync = false;
}

void XMonitor::revert()
{
    _position = _saved_position;
    _size = _saved_size;
    _mode = _saved_mode;
    _rotation = _saved_rotation;
    XMonitorsList::iterator iter = _clones.begin();
    for (; iter != _clones.end(); iter++) {
        (*iter)->revert();
    }
}

void XMonitor::disable()
{
    Display* display = _container.get_display();
    X_DEBUG_SYNC(display);
    Window root_window = RootWindow(display, _container.get_screen());
    AutoScreenRes res(XRRGetScreenResources(display, root_window));
    if (!res.valid()) {
        THROW("get screen resources failed");
    }
    XRRSetCrtcConfig(display, res.get(), _crtc, CurrentTime,
                     0, 0, None, RR_Rotate_0, NULL, 0);

    XMonitorsList::iterator iter = _clones.begin();
    for (; iter != _clones.end(); iter++) {
        (*iter)->disable();
    }
    X_DEBUG_SYNC(display);
}

void XMonitor::enable()
{
    Display* display = _container.get_display();
    X_DEBUG_SYNC(display);
    Window root_window = RootWindow(display, _container.get_screen());
    AutoScreenRes res(XRRGetScreenResources(display, root_window));
    if (!res.valid()) {
        THROW("get screen resources failed");
    }
    XRRSetCrtcConfig(display, res.get(), _crtc, CurrentTime,
                     _position.x, _position.y,
                     _mode, _rotation,
                     _outputs, _noutput);

    XMonitorsList::iterator iter = _clones.begin();
    for (; iter != _clones.end(); iter++) {
        (*iter)->enable();
    }
    X_DEBUG_SYNC(display);
}

bool XMonitor::mode_changed()
{
    return _size.x != _saved_size.x || _size.y != _saved_size.y ||
           _mode != _saved_mode || _rotation != _saved_rotation;
}

bool XMonitor::position_changed()
{
    return _position.x != _saved_position.x || _position.y != _saved_position.y;
}

void XMonitor::restore()
{
    if (!mode_changed()) {
        return;
    }
    set_mode(_saved_size.x, _saved_size.y);
}

int XMonitor::get_depth()
{
    return XPlatform::get_vinfo()[0]->depth;
}

Point XMonitor::get_position()
{
    return _position;
}

Point XMonitor::get_size() const
{
    return _size;
}

bool XMonitor::is_out_of_sync()
{
    return _out_of_sync;
}

void XMonitor::add_clone(XMonitor *clone)
{
    _clones.push_back(clone);
}

const Rect& XMonitor::get_prev_area()
{
    return _trans_area[_trans_depth - 1];
}

Rect& XMonitor::get_trans_area()
{
    return _trans_area[_trans_depth];
}

void XMonitor::push_trans()
{
    _trans_depth++;
    ASSERT(_trans_depth < MAX_TRANS_DEPTH);
    _trans_area[_trans_depth] = _trans_area[_trans_depth - 1];
    _pin_count = 0;
    _pusher = NULL;
}

void XMonitor::begin_trans()
{
    _trans_area[0].left = _position.x;
    _trans_area[0].right = _trans_area[0].left + _size.x;
    _trans_area[0].top = _position.y;
    _trans_area[0].bottom = _trans_area[0].top + _size.y;
    _trans_area[1] = _trans_area[0];
    _trans_depth = 1;
    _pin_count = 0;
    _pusher = NULL;
}

void XMonitor::commit_trans_position()
{
    _position.x = _trans_area[_trans_depth].left;
    _position.y = _trans_area[_trans_depth].top;
    XMonitorsList::iterator iter = _clones.begin();
    for (; iter != _clones.end(); iter++) {
        (*iter)->_position = _position;
    }
}

void XMonitor::set_mode(const XRRModeInfo& mode)
{
    _mode = mode.id;
    _size.x = mode.width;
    _size.y = mode.height;
    XMonitorsList::iterator iter = _clones.begin();
    for (; iter != _clones.end(); iter++) {
        (*iter)->set_mode(mode);
    }
}

#endif

static MonitorsList monitors;

typedef std::list<XScreen*> ScreenList;
static ScreenList screens;

const MonitorsList& Platform::init_monitors()
{
    int next_mon_id = 0;
    ASSERT(screens.empty());
#ifdef USE_XRANDR_1_2
    if (using_xrandr_1_2) {
        for (int i = 0; i < ScreenCount(x_display); i++) {
            screens.push_back(new MultyMonScreen(x_display, i, next_mon_id));
        }
    } else
#endif
    if (using_xrandr_1_0) {
        for (int i = 0; i < ScreenCount(x_display); i++) {
            screens.push_back(new DynamicScreen(x_display, i, next_mon_id));
        }
    } else {
        for (int i = 0; i < ScreenCount(x_display); i++) {
            screens.push_back(new StaticScreen(x_display, i, next_mon_id));
        }
    }

    ASSERT(monitors.empty());
    ScreenList::iterator iter = screens.begin();
    for (; iter != screens.end(); iter++) {
        (*iter)->publish_monitors(monitors);
    }
    return monitors;
}

void Platform::destroy_monitors()
{
    monitors.clear();
    while (!screens.empty()) {
        XScreen* screen = screens.front();
        screens.pop_front();
        delete screen;
    }
}

bool Platform::is_monitors_pos_valid()
{
    return (ScreenCount(x_display) == 1);
}

static void root_win_proc(XEvent& event)
{
#ifdef USE_XRANDR_1_2
    ASSERT(using_xrandr_1_0 || using_xrandr_1_2);
#else
    ASSERT(using_xrandr_1_0);
#endif
    if (event.type == ConfigureNotify || event.type - xrandr_event_base == RRScreenChangeNotify) {
        XRRUpdateConfiguration(&event);
        if (event.type - xrandr_event_base == RRScreenChangeNotify) {
            display_mode_listener->on_display_mode_change();
        }

        if (Monitor::is_self_change()) {
            return;
        }

        ScreenList::iterator iter = screens.begin();
        for (; iter != screens.end(); iter++) {
            (*iter)->set_broken();
        }
        event_listener->on_monitors_change();
    }

    /*switch (event.type - xrandr_event_base) {
    case RRScreenChangeNotify:
        //XRRScreenChangeNotifyEvent * = (XRRScreenChangeNotifyEvent *) &event;
        XRRUpdateConfiguration(&event);
        break;
    }*/
}

static void process_monitor_configure_events(Window root)
{
    XSync(x_display, False);
    XEvent event;

    while (XCheckTypedWindowEvent(x_display, root, ConfigureNotify, &event)) {
        root_win_proc(event);
    }

    while (XCheckTypedWindowEvent(x_display, root, xrandr_event_base + RRScreenChangeNotify,
                                  &event)) {
        root_win_proc(event);
    }
}

static void cleanup(void)
{
    int i;

    DBG(0, "");
    if (!Monitor::is_self_change()) {
        Platform::destroy_monitors();
    }
    if (vinfo) {
        for (i = 0; i < ScreenCount(x_display); ++i) {
            XFree(vinfo[i]);
        }
        delete vinfo;
        vinfo = NULL;
    }
    if (fb_config) {
        for (i = 0; i < ScreenCount(x_display); ++i) {
            XFree(fb_config[i]);
        }
        delete fb_config;
        fb_config = NULL;
    }
}

static void quit_handler(int sig)
{
    LOG_INFO("signal %d", sig);
    Platform::send_quit_request();
}

static void abort_handler(int sig)
{
    LOG_INFO("signal %d", sig);
    Platform::destroy_monitors();
}

static void init_xrandr()
{
    Bool xrandr_ext = XRRQueryExtension(x_display, &xrandr_event_base, &xrandr_error_base);
    if (xrandr_ext) {
        XRRQueryVersion(x_display, &xrandr_major, &xrandr_minor);
        if (xrandr_major < 1) {
            return;
        }
#ifdef USE_XRANDR_1_2
        if (xrandr_major == 1 && xrandr_minor < 2) {
            using_xrandr_1_0 = true;
            return;
        }
        using_xrandr_1_2 = true;
#else
        using_xrandr_1_0 = true;
#endif
    }
}

static void init_xrender()
{
    int event_base;
    int error_base;
    int major;
    int minor;

    using_xrender_0_5 = XRenderQueryExtension(x_display, &event_base, &error_base) &&
        XRenderQueryVersion(x_display, &major, &minor) && (major > 0 || minor >= 5);
}

unsigned int get_modifier_mask(KeySym modifier)
{
    int mask = 0;
    int i;

    XModifierKeymap* map = XGetModifierMapping(x_display);
    KeyCode keycode = XKeysymToKeycode(x_display, modifier);
    if (keycode == NoSymbol) {
        return 0;
    }

    for (i = 0; i < 8; i++) {
        if (map->modifiermap[map->max_keypermod * i] == keycode) {
            mask = 1 << i;
        }
    }
    XFreeModifiermap(map);
    return mask;
}

static void init_kbd()
{
    int xkb_major = XkbMajorVersion;
    int xkb_minor = XkbMinorVersion;
    int opcode;
    int event;
    int error;

    if (!XkbLibraryVersion(&xkb_major, &xkb_minor) ||
        !XkbQueryExtension(x_display, &opcode, &event, &error, &xkb_major, &xkb_minor)) {
        return;
    }
    caps_lock_mask = get_modifier_mask(XK_Caps_Lock);
    num_lock_mask = get_modifier_mask(XK_Num_Lock);
}

static int x_error_handler(Display* display, XErrorEvent* error_event)
{
    char error_str[256];
    char request_str[256];
    char number_str[32];

    char* display_name = XDisplayString(display);
    XGetErrorText(display, error_event->error_code, error_str, sizeof(error_str));

    if (error_event->request_code < 128) {
        snprintf(number_str, sizeof(number_str), "%d", error_event->request_code);
        XGetErrorDatabaseText(display, "XRequest", number_str, "",
                              request_str, sizeof(request_str));
    } else {
        snprintf(request_str, sizeof(request_str), "%d", error_event->request_code);
    }

    LOG_ERROR("x error on display %s error %s minor %u request %s",
              display_name,
              error_str,
              (uint32_t)error_event->minor_code,
              request_str);
    exit(-1);
    return 0;
}

static int x_io_error_handler(Display* display)
{
    LOG_ERROR("x io error on %s", XDisplayString(display));
    exit(-1);
    return 0;
}

static XVisualInfo* get_x_vis_info(int screen)
{
    XVisualInfo vtemplate;
    int count;

    Visual* visual = DefaultVisualOfScreen(ScreenOfDisplay(x_display, screen));
    vtemplate.screen = screen;
    vtemplate.visualid = XVisualIDFromVisual(visual);
    return XGetVisualInfo(x_display, VisualIDMask | VisualScreenMask, &vtemplate, &count);
}

void Platform::init()
{
    int err, ev;
    int threads_enable;

    DBG(0, "");

    threads_enable = XInitThreads();


    if (!(x_display = XOpenDisplay(NULL))) {
        THROW("open X display failed");
    }

    vinfo = new XVisualInfo *[ScreenCount(x_display)];
    memset(vinfo, 0, sizeof(XVisualInfo *) * ScreenCount(x_display));
    fb_config = new GLXFBConfig *[ScreenCount(x_display)];
    memset(fb_config, 0, sizeof(GLXFBConfig *) * ScreenCount(x_display));

    // working with KDE and visual from glXGetVisualFromFBConfig is not working
    // well. for now disabling OGL.
    if (0 && threads_enable && glXQueryExtension(x_display, &err, &ev)) {
        int num_configs;
        int attrlist[] = {
            GLX_RENDER_TYPE, GLX_RGBA_BIT,
            GLX_DRAWABLE_TYPE, GLX_PBUFFER_BIT | GLX_WINDOW_BIT,
            GLX_X_VISUAL_TYPE, GLX_TRUE_COLOR,
            GLX_RED_SIZE, 8,
            GLX_GREEN_SIZE, 8,
            GLX_BLUE_SIZE, 8,
            GLX_ALPHA_SIZE, 8,
            GLX_STENCIL_SIZE, 4,
            GLX_DEPTH_SIZE, 0,
            None
        };

        for (int i = 0; i < ScreenCount(x_display); ++i) {
            if (!(fb_config[i] = glXChooseFBConfig(x_display, i, attrlist, &num_configs))) {
                vinfo[i] = get_x_vis_info(i);
            } else {
                ASSERT(num_configs > 0);
                vinfo[i] = glXGetVisualFromFBConfig(x_display, fb_config[i][0]);
            }

            if (!vinfo[i]) {
                THROW("XGetVisualInfo failed");
            }
        }
    } else {
        for (int i = 0; i < ScreenCount(x_display); ++i) {
            if (!(vinfo[i] = get_x_vis_info(i))) {
                THROW("XGetVisualInfo failed");
            }
        }
    }

    XSetErrorHandler(x_error_handler);
    XSetIOErrorHandler(x_io_error_handler);

    win_proc_context = XUniqueContext();

    init_kbd();
    init_xrandr();
    init_xrender();

    struct sigaction act;
    memset(&act, 0, sizeof(act));
    sigfillset(&act.sa_mask);

    act.sa_handler = quit_handler;
    sigaction(SIGINT, &act, NULL);
    sigaction(SIGHUP, &act, NULL);
    sigaction(SIGTERM, &act, NULL);
    sigaction(SIGQUIT, &act, NULL);

    act.sa_handler = SIG_IGN;
    sigaction(SIGPIPE, &act, NULL);

    act.sa_flags = SA_RESETHAND;
    act.sa_handler = abort_handler;
    sigaction(SIGABRT, &act, NULL);
    sigaction(SIGILL, &act, NULL);
    sigaction(SIGSEGV, &act, NULL);
    sigaction(SIGFPE, &act, NULL);

    atexit(cleanup);
}

void Platform::set_process_loop(ProcessLoop& main_process_loop)
{
    main_loop = &main_process_loop;
    XEventHandler *x_event_handler;
    x_event_handler = new XEventHandler(*x_display, win_proc_context);
    main_loop->add_file(*x_event_handler);
}

uint32_t Platform::get_keyboard_modifiers()
{
    XKeyboardState keyboard_state;
    uint32_t modifiers = 0;

    XGetKeyboardControl(x_display, &keyboard_state);

    if (keyboard_state.led_mask & 0x01) {
        modifiers |= CAPS_LOCK_MODIFIER;
    }
    if (keyboard_state.led_mask & 0x02) {
        modifiers |= NUM_LOCK_MODIFIER;
    }
    if (keyboard_state.led_mask & 0x04) {
        modifiers |= SCROLL_LOCK_MODIFIER;
    }
    return modifiers;
}

enum XLed {
    X11_CAPS_LOCK_LED = 1,
    X11_NUM_LOCK_LED,
    X11_SCROLL_LOCK_LED,
};

static void  set_keyboard_led(XLed led, int set)
{
    switch (led) {
    case X11_CAPS_LOCK_LED:
        if (caps_lock_mask) {
            XkbLockModifiers(x_display, XkbUseCoreKbd, caps_lock_mask, set ? caps_lock_mask : 0);
        }
        return;
    case X11_NUM_LOCK_LED:
        if (num_lock_mask) {
            XkbLockModifiers(x_display, XkbUseCoreKbd, num_lock_mask, set ? num_lock_mask : 0);
        }
        return;
    case X11_SCROLL_LOCK_LED:
        XKeyboardControl keyboard_control;
        keyboard_control.led_mode = set ? LedModeOn : LedModeOff;
        keyboard_control.led = led;
        XChangeKeyboardControl(x_display, KBLed | KBLedMode, &keyboard_control);
        return;
    }
}

void Platform::set_keyboard_modifiers(uint32_t modifiers)
{
    uint32_t now = get_keyboard_modifiers();

    if ((now & CAPS_LOCK_MODIFIER) != (modifiers & CAPS_LOCK_MODIFIER)) {
        set_keyboard_led(X11_CAPS_LOCK_LED, !!(modifiers & CAPS_LOCK_MODIFIER));
    }
    if ((now & NUM_LOCK_MODIFIER) != (modifiers & NUM_LOCK_MODIFIER)) {
        set_keyboard_led(X11_NUM_LOCK_LED, !!(modifiers & NUM_LOCK_MODIFIER));
    }
    if ((now & SCROLL_LOCK_MODIFIER) != (modifiers & SCROLL_LOCK_MODIFIER)) {
        set_keyboard_led(X11_SCROLL_LOCK_LED, !!(modifiers & SCROLL_LOCK_MODIFIER));
    }
}

WaveRecordAbstract* Platform::create_recorder(RecordClinet& client,
                                              uint32_t sampels_per_sec,
                                              uint32_t bits_per_sample,
                                              uint32_t channels)
{
    return new WaveRecorder(client, sampels_per_sec, bits_per_sample, channels);
}

WavePlaybackAbstract* Platform::create_player(uint32_t sampels_per_sec,
                                              uint32_t bits_per_sample,
                                              uint32_t channels)
{
    return new WavePlayer(sampels_per_sec, bits_per_sample, channels);
}

void XPlatform::on_focus_in()
{
    if (focus_count++ == 0) {
        event_listener->on_app_activated();
    }
}

void XPlatform::on_focus_out()
{
    ASSERT(focus_count > 0);
    if (--focus_count == 0) {
        event_listener->on_app_deactivated();
    }
}

class XBaseLocalCursor: public LocalCursor {
public:
    XBaseLocalCursor() : _handle (0) {}
    ~XBaseLocalCursor();
    void set(Window window);

protected:
    Cursor _handle;
};

void XBaseLocalCursor::set(Window window)
{
    if (_handle) {
        XDefineCursor(x_display, window, _handle);
    }
}

XBaseLocalCursor::~XBaseLocalCursor()
{
    if (_handle) {
        XFreeCursor(x_display, _handle);
    }
}

class XLocalCursor: public XBaseLocalCursor {
public:
    XLocalCursor(CursorData* cursor_data);
};

static inline uint8_t get_pix_mask(const uint8_t* data, int offset, int pix_index)
{
    return data[offset + (pix_index >> 3)] & (0x80 >> (pix_index % 8));
}

static inline uint32_t get_pix_hack(int pix_index, int width)
{
    return (((pix_index % width) ^ (pix_index / width)) & 1) ? 0xc0303030 : 0x30505050;
}

XLocalCursor::XLocalCursor(CursorData* cursor_data)
{
    const CursorHeader& header = cursor_data->header();
    const uint8_t* data = cursor_data->data();
    int cur_size = header.width * header.height;
    uint8_t pix_mask;
    uint32_t pix;
    uint16_t i;
    int size;

    if (!get_size_bits(header, size)) {
        THROW("invalid curosr type");
    }

    uint32_t* cur_data = new uint32_t[cur_size];

    switch (header.type) {
    case CURSOR_TYPE_ALPHA:
        break;
    case CURSOR_TYPE_COLOR32:
        memcpy(cur_data, data, cur_size * sizeof(uint32_t));
        for (i = 0; i < cur_size; i++) {
            pix_mask = get_pix_mask(data, size, i);
            if (pix_mask && *((uint32_t*)data + i) == 0xffffff) {
                cur_data[i] = get_pix_hack(i, header.width);
            } else {
                cur_data[i] |= (pix_mask ? 0 : 0xff000000);
            }
        }
        break;
    case CURSOR_TYPE_COLOR16:
        for (i = 0; i < cur_size; i++) {
            pix_mask = get_pix_mask(data, size, i);
            pix = *((uint16_t*)data + i);
            if (pix_mask && pix == 0x7fff) {
                cur_data[i] = get_pix_hack(i, header.width);
            } else {
                cur_data[i] = ((pix & 0x1f) << 3) | ((pix & 0x3e0) << 6) | ((pix & 0x7c00) << 9) |
                    (pix_mask ? 0 : 0xff000000);
            }
        }
        break;
    case CURSOR_TYPE_MONO:
        for (i = 0; i < cur_size; i++) {
            pix_mask = get_pix_mask(data, 0, i);
            pix = get_pix_mask(data, size, i);
            if (pix_mask && pix) {
                cur_data[i] = get_pix_hack(i, header.width);
            } else {
                cur_data[i] = (pix ? 0xffffff : 0) | (pix_mask ? 0 : 0xff000000);
            }
        }
        break;
    case CURSOR_TYPE_COLOR4:
        for (i = 0; i < cur_size; i++) {
            pix_mask = get_pix_mask(data, size + (sizeof(uint32_t) << 4), i);
            int idx = (i & 1) ? (data[i >> 1] & 0x0f) : ((data[i >> 1] & 0xf0) >> 4);
            pix = *((uint32_t*)(data + size) + idx);
            if (pix_mask && pix == 0xffffff) {
                cur_data[i] = get_pix_hack(i, header.width);
            } else {
                cur_data[i] = pix | (pix_mask ? 0 : 0xff000000);
            }
        }
        break;
    case CURSOR_TYPE_COLOR24:
    case CURSOR_TYPE_COLOR8:
    default:
        LOG_WARN("unsupported cursor type %d", header.type);
        _handle = XCreateFontCursor(x_display, XC_arrow);
        delete[] cur_data;
        return;
    }

    XImage image;
    memset(&image, 0, sizeof(image));
    image.width = header.width;
    image.height = header.height;
    image.data = (header.type == CURSOR_TYPE_ALPHA ? (char*)data : (char*)cur_data);
    image.byte_order = LSBFirst;
    image.bitmap_unit = 32;
    image.bitmap_bit_order = LSBFirst;
    image.bitmap_pad = 8;
    image.bytes_per_line = header.width << 2;
    image.depth = 32;
    image.format = ZPixmap;
    image.bits_per_pixel = 32;
    image.red_mask = 0x00ff0000;
    image.green_mask = 0x0000ff00;
    image.blue_mask = 0x000000ff;
    if (!XInitImage(&image)) {
        THROW("init image failed");
    }

    Window root_window = RootWindow(x_display, DefaultScreen(x_display));
    Pixmap pixmap = XCreatePixmap(x_display, root_window, header.width, header.height, 32);

    XGCValues gc_vals;
    gc_vals.function = GXcopy;
    gc_vals.foreground = ~0;
    gc_vals.background = 0;
    gc_vals.plane_mask = AllPlanes;

    GC gc = XCreateGC(x_display, pixmap, GCFunction | GCForeground | GCBackground | GCPlaneMask,
                      &gc_vals);
    XPutImage(x_display, pixmap, gc, &image, 0, 0, 0, 0, header.width, header.height);
    XFreeGC(x_display, gc);

    XRenderPictFormat *xformat = XRenderFindStandardFormat(x_display, PictStandardARGB32);
    Picture picture = XRenderCreatePicture(x_display, pixmap, xformat, 0, NULL);
    _handle = XRenderCreateCursor(x_display, picture, header.hot_spot_x, header.hot_spot_y);
    XRenderFreePicture(x_display, picture);
    XFreePixmap(x_display, pixmap);
    delete[] cur_data;
}

LocalCursor* Platform::create_local_cursor(CursorData* cursor_data)
{
    ASSERT(using_xrender_0_5);
    return new XLocalCursor(cursor_data);
}

class XInactiveCursor: public XBaseLocalCursor {
public:
    XInactiveCursor() { _handle = XCreateFontCursor(x_display, XC_X_cursor);}
};

LocalCursor* Platform::create_inactive_cursor()
{
    return new XInactiveCursor();
}

class XDefaultCursor: public XBaseLocalCursor {
public:
    XDefaultCursor() { _handle = XCreateFontCursor(x_display, XC_top_left_arrow);}
};

LocalCursor* Platform::create_default_cursor()
{
    return new XDefaultCursor();
}

class PlatformTimer: public Timer {
public:
    PlatformTimer(timer_proc_t proc, void* opaque) : _proc(proc), _opaque(opaque) {}
    void response(AbstractProcessLoop& events_loop) {_proc(_opaque, (TimerID)this);}

private:
    timer_proc_t _proc;
    void* _opaque;
};

TimerID Platform::create_interval_timer(timer_proc_t proc, void* opaque)
{
    return (TimerID)(new PlatformTimer(proc, opaque));
}

bool Platform::activate_interval_timer(TimerID timer, unsigned int millisec)
{
    ASSERT(main_loop);
    main_loop->activate_interval_timer((PlatformTimer*)timer, millisec);
    return true;
}

bool Platform::deactivate_interval_timer(TimerID timer)
{
    ASSERT(main_loop);
    main_loop->deactivate_interval_timer((PlatformTimer*)timer);
    return true;
}

void Platform::destroy_interval_timer(TimerID timer)
{
    deactivate_interval_timer(timer);
    ((PlatformTimer*)timer)->unref();
}
