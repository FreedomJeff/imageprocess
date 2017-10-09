#include <stdio.h>
#include <string.h>
#include <stdint.h>
#include <fcntl.h>
#include <pthread.h>
#include <errno.h>
#include <sys/mman.h>
#include <poll.h>
#include <gst/gst.h>
#include <stdlib.h>
#include <gtk/gtk.h>
#include <linux/input.h>
#include <fcntl.h>
#include <gst/video/videooverlay.h>
#include <gdk/gdk.h>
#include <time.h>
#include <sys/time.h>
#include <unistd.h>

#if defined (GDK_WINDOWING_X11)
#include <gdk/gdkx.h>
#elif defined (GDK_WINDOWING_WIN32)
#include <gdk/gdkwin32.h>
#elif defined (GDK_WINDOWING_QUARTZ)
#include <gdk/gdkquartz.h>
#endif

#include "exynos_v4l2.h"
#include "node.h"
#include "debug.h"
#include "ion.h"
#include "fimc-is-metadata.h"

#define EINVAL 100

#define STREAM_PARAMETER_MAX_LENGTH 300
#define EVENT_NAME "/dev/input/by-path/platform-gpio_keys-event"
#define POWER_VALUE 0x74

#define DUMP_FILE_PATH "/usr/share/misc/";
//#define DUMP_UYVY

//#define GST_SCALE
#define PIX_YUVI420
#define TIMEOUT    1500
#define SENSOR_WIDTH 1280
#define SENSOR_HEIGHT_720P 720
#define SENSOR_HEIGHT_QVGA 960
#define V4L2_REQUEST_BUFF 8
#define READ_WRITE_FILE_SIZE 640*480*2
#define debug_framerate 0

enum camera_num {
        camera_1 = 1,
        camera_2,
        camera_3,
        camera_4,
};

/* camera mode: heigt 720 or height 960 */
enum camera_mod {
        camera_720 = 0,
        camera_960,
};

typedef struct ion_buffer {
	int fd;
	char *addr;
	int size;
	int ion_client;
	int status;
	int dqbuf_ok;
	pthread_mutex_t lock;
	int index;
	int count;
};

static pthread_mutex_t taa_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t taa_cond = PTHREAD_COND_INITIALIZER;

static pthread_mutex_t isp_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t isp_cond = PTHREAD_COND_INITIALIZER;

static int g_fd_flite, g_fd_3aa_p, g_fd_3aa_o, g_fd_3aa_c, g_fd_scp_p, g_fd_isp_o, g_fd_isp_p, g_fd_dis_o, g_fd_3ac;
static int m_fd, m_fd_3aa_p, m_fd_3aa_o, m_fd_3aa_c, m_fd_scp, m_fd_isp_o;
static int ionFd, m_ionClient;
static unsigned long bufSize;
static struct v4l2_requestbuffers m_v4l2ReqBufs;
static struct v4l2_format m_v4l2Format;
static struct ion_buffer ion_buf_isp[V4L2_REQUEST_BUFF][2];
static struct ion_buffer ion_buf_bayer[6][2];
static struct ion_buffer ion_buf_3a1[9][2];
static struct ion_buffer ion_buf_scp[12][3];
static struct camera2_shot_ext g_shot_ext_3aa_o, g_shot_ext_isp_o;
struct camera2_shot_ext *tmp_shot_ext;
static int g_fcnt = -1;
static int g_need_data = 1;
static int g_preview_w = 0;
static int g_preview_h = 0;

static GstState gstState = GST_STATE_NULL;
static GstElement *play = NULL;
static GstElement* p_appsrc;
static gulong video_window_xid = 0;
static int g_frame_count = 0;
pthread_t pid_3aa, pid_isp;
int previewFlag = 0;
int mIonCameraClient = 0;
GtkWidget *main_window;
GtkWidget *video_window;



static GstBusSyncReply
bus_sync_handler (GstBus * bus, GstMessage * message, gpointer user_data)
{
	// ignore anything but 'prepare-xwindow-id' element messages
	if (!gst_is_video_overlay_prepare_window_handle_message (message))
	return GST_BUS_PASS;
	//if (!gst_structure_has_name (message->structure, "prepare-xwindow-id"))
	//return GST_BUS_PASS;

	if (video_window_xid != 0) {
		GstVideoOverlay *overlay;

		// GST_MESSAGE_SRC (message) will be the video sink element
		overlay = GST_VIDEO_OVERLAY (GST_MESSAGE_SRC (message));
		gst_video_overlay_set_window_handle (overlay, video_window_xid);
	} else {
		g_warning ("Should have obtained video_window_xid by now!");
	}

	gst_message_unref (message);
	return GST_BUS_DROP;
}

static void
video_widget_realize_cb (GtkWidget * widget, gpointer data)
{
#if GTK_CHECK_VERSION(2,18,0)
	// This is here just for pedagogical purposes, GDK_WINDOW_XID will call
	// it as well in newer Gtk versions
	if (!gdk_window_ensure_native (widget->window))
		g_error ("Couldn't create native window needed for GstXOverlay!");
#endif

#ifdef GDK_WINDOWING_X11
  video_window_xid = GDK_WINDOW_XID (gtk_widget_get_window (widget));
#endif
}

static gboolean bus_callback(GstBus *bus, GstMessage *message, gpointer data)
{
	switch (GST_MESSAGE_TYPE(message)) {
		case GST_MESSAGE_ERROR:
		{
			GError *err;
			gchar *debug;
			gst_message_parse_error(message, &err, &debug);
			g_print("Error: %s\n", err->message);
			g_error_free(err);
			g_free(debug);
			gtk_main_quit();
			break;
		}
		case GST_MESSAGE_EOS:
			g_print("End of stream\n");
			gst_element_set_state(play, GST_STATE_PAUSED);
			gstState = GST_STATE_PAUSED;
			break;
			default:
			break;
	}
	return TRUE;
}

static void delete_event_cb (GtkWidget *widget, GdkEvent *event) {
	if(gstState != GST_STATE_NULL) {
		gst_element_set_state (play, GST_STATE_NULL);
	}
	gstState = GST_STATE_NULL;
	previewFlag = 0;
	gtk_main_quit ();
}


void on_play_widget_button_press(GtkWidget *widget, GdkEvent *event, gpointer data) {
	if(event->button.type == GDK_BUTTON_PRESS) {
		if(gstState != GST_STATE_NULL) {
			gst_element_set_state (play, GST_STATE_NULL);
		}
		gstState = GST_STATE_NULL;
		previewFlag = 0;
		gtk_main_quit ();

	}
}

/* This creates all the GTK+ widgets that compose our application, and registers the callbacks */
static void create_ui () {

	GtkWidget *main_box;

	main_window = gtk_window_new (GTK_WINDOW_TOPLEVEL);
	gtk_window_fullscreen(GTK_WINDOW(main_window));
	g_signal_connect (G_OBJECT (main_window), "delete-event", G_CALLBACK (delete_event_cb), NULL);

	video_window = gtk_drawing_area_new ();
	g_signal_connect (video_window, "realize",G_CALLBACK (video_widget_realize_cb), NULL);
	gtk_widget_set_double_buffered (video_window, FALSE);

	main_box = gtk_vbox_new (FALSE, 0);
	gtk_box_pack_start (GTK_BOX (main_box), video_window, TRUE, TRUE, 0);
	gtk_container_add (GTK_CONTAINER (main_window), main_box);
	gtk_window_set_title(GTK_WINDOW(main_window), "Camera_MIPI");
	gtk_window_set_position(GTK_WINDOW(main_window), GTK_WIN_POS_CENTER);
	gtk_window_set_default_size (GTK_WINDOW (main_window), 1280, 800);

	gtk_widget_add_events(GTK_WIDGET(main_window),	GDK_POINTER_MOTION_MASK | GDK_BUTTON_PRESS_MASK);
	g_signal_connect(main_window, "button-press-event", G_CALLBACK(on_play_widget_button_press), NULL);

	gtk_widget_show_all (main_window);
	gtk_widget_realize (video_window);
	g_assert (video_window_xid != 0);
	printf("hemx->>>end of create ui!\n");
}

static int ion_ioctl(int fd, int req, void *arg)
{
	int ret = ioctl(fd, req, arg);
	if (ret < 0) {
		ALOGE("ioctl %x failed with code %d\n", req, ret);
		return ret;
	}
	return ret;
}

int ion_alloc(int fd, size_t len, size_t align, unsigned int heap_mask,
		unsigned int flags, int *handle)
{
	int ret;
	struct ion_allocation_data data = {
		.len = len,
		.align = align,
		.heap_id_mask = heap_mask,
		.flags = flags,
	};

	if (handle == NULL)
		return -EINVAL;

	ret = ion_ioctl(fd, ION_IOC_ALLOC, &data);
	if (ret < 0)
		return ret;
	*handle = data.handle;
	return ret;
}

int ion_share(int fd, int handle, int *share_fd)
{
	int ret;
	struct ion_fd_data data = {
		.handle = handle,
	};

	if (share_fd == NULL)
		return -EINVAL;

	ret = ion_ioctl(fd, ION_IOC_SHARE, &data);
	if (ret < 0)
		return ret;
	if (data.fd < 0) {
		ALOGE("share ioctl returned negative fd\n");
		return -EINVAL;
	}
	*share_fd = data.fd;
	return ret;
}

int ion_free(int fd, ion_user_handle_t handle)
{
	struct ion_handle_data data = {
			.handle = handle,
	};
	return ion_ioctl(fd, ION_IOC_FREE, &data);
}

int ion_alloc_fd(int fd, size_t len, size_t align, unsigned int heap_mask,unsigned int flags, int *handle_fd)
{
	int handle;
	int ret;

	ret = ion_alloc(fd, len, align, heap_mask, flags, &handle);
	if (ret < 0)
		return ret;
	ret = ion_share(fd, handle, handle_fd);
	ion_free(fd, handle);
	return ret;
}

int ion_close(int fd)
{
	int ret = close(fd);
	if (ret < 0)
		return ret;
	return ret;
}

int select_prev_resolution(int fd)
{
        int ret = 0;
        int camera_position = 0;
        int camera_count = 0;
        /* Default camera firmware */
        int camera_fw = camera_960;
        char fw_info = 0;

        ret = exynos_v4l2_g_ctrl(fd, V4L2_CID_CAM_SENSOR_FW_VER, &fw_info);
        if (ret < 0) {
                ALOGE("Get firmware info failed!\n");
                return ret;
        }
        camera_position = fw_info & 0x0F;
        camera_fw = (fw_info >> 4) & 0x0F;
        if (camera_position & (0x1 << 0))
                camera_count++;
        if (camera_position & (0x1 << 1))
                camera_count++;
        if (camera_position & (0x1 << 2))
                camera_count++;
        if (camera_position & (0x1 << 3))
                camera_count++;
        ALOGE("MAX9286 camera num: %d, camera firmware: %d.\n", camera_count, camera_fw);
        g_preview_w = SENSOR_WIDTH * camera_count;

        if (camera_720 == camera_fw) {
                g_preview_h = SENSOR_HEIGHT_720P;
        } else if (camera_960 == camera_fw) {
                g_preview_h = SENSOR_HEIGHT_QVGA;
        } else {
                ALOGE("Not supported camera firmware.\n");
        }
        ALOGE("Preview width:%d height: %d.\n", g_preview_w, g_preview_h);
        return ret;
}

int yuyv422toyuvI420(unsigned char *yuv420, const unsigned char *yuv422, unsigned int width, unsigned int height)
{
        unsigned char *y = yuv420;
        unsigned char *u = yuv420 + width * height;
        unsigned char *v = yuv420 + width * height + width * height / 4;

        unsigned int i,j;
        unsigned int base_h;
        unsigned int is_y = 1, is_u = 1;
        unsigned int y_index = 0, u_index = 0, v_index = 0;
        unsigned long yuv422_length = 2 * width * height;

        for(i = 0; i < yuv422_length; i += 2) {
                *(y + y_index) = *(yuv422 + i);
                y_index++;
        }

        for(i = 0; i < height; i += 2) {
                base_h = i * width * 2;
                for(j = base_h+1; j<base_h+width*2; j+=2){
                        if(is_u){
                                *(u+u_index) = *(yuv422+j);
                                u_index++;
                                is_u = 0;
                        } else {
                                *(v+v_index) = *(yuv422+j);
                                v_index++;
                                is_u = 1;
                        }
                }
        }

        return 1;
}

int uyvy422toyuvI420(unsigned char *yuv420, const unsigned char *yuv422, unsigned int width, unsigned int height)
{
        unsigned char *y = yuv420;
        unsigned char *u = yuv420 + width * height;
        unsigned char *v = yuv420 + width * height + width * height / 4;

        unsigned int i,j;
        unsigned int base_h;
        unsigned int is_y = 1, is_u = 1;
        unsigned int y_index = 0, u_index = 0, v_index = 0;
        unsigned long yuv422_length = width * height * 2;

        for(i = 1; i < yuv422_length; i += 2) {
                *(y + y_index) = *(yuv422 + i);
                y_index++;
        }

        for(i = 0; i < height; i += 2) {
                base_h = i * width *2;
                for(j = base_h; j<base_h+width*2; j+=2) {
                        if(is_u){
                                *(u+u_index) = *(yuv422+j);
                                u_index++;
                                is_u = 0;
                        } else {
                                *(v+v_index) = *(yuv422+j);
                                v_index++;
                                is_u = 1;
                        }
                }
        }

        return 1;
}

int calc_buffer_size(int w, int h)
{
        return (w * h) << 2;
}

int _ion_buffer_alloc(int client, size_t* size, int buf_cnt, int plane_cnt, struct ion_buffer buf[][2]) {
	size_t m_ionAlign = 0;
	unsigned int m_ionHeapMask = ION_HEAP_SYSTEM_MASK;
	unsigned int m_ionFlags = EXYNOS_CAMERA_BUFFER_ION_FLAG_NONCACHED;
	int i, j, ret;

	buf[0][0].count = buf_cnt;
	buf[0][0].index = 0;
	pthread_mutex_init(&(buf[0][0].lock),NULL);
	for(i = 0; i < buf_cnt; i++) {
		for(j = 0; j < plane_cnt; j++) {
			buf[i][j].status = 0;
			buf[i][j].size = size[j];
			buf[i][j].ion_client = client;
			buf[i][j].fd = -1;
			ret = ion_alloc_fd(client, size[j], m_ionAlign, m_ionHeapMask, m_ionFlags, &(buf[i][j].fd));
			if (ret < 0) {
				ALOGE("ion_alloc_fd(fd=%d) failed:%d", client, ret);
				return -1;
			}
			buf[i][j].addr = (char *)mmap(NULL, size[j], PROT_READ|PROT_WRITE, MAP_SHARED, buf[i][j].fd, 0);
			if (buf[i][j].addr == NULL) {
				ALOGE("ion map failed");
				return -1;
			}
		}
	}
}

int _ion_buffer_alloc_scp(int client, size_t* size, int buf_cnt, int plane_cnt, struct ion_buffer buf[][3]) {
	size_t m_ionAlign = 0;
	unsigned int m_ionHeapMask = ION_HEAP_SYSTEM_MASK;
	unsigned int m_ionFlags = EXYNOS_CAMERA_BUFFER_ION_FLAG_NONCACHED;
	int i, j, ret;

	buf[0][0].count = buf_cnt;
	buf[0][0].index = 0;
	pthread_mutex_init(&(buf[0][0].lock),NULL);
	for(j = 0; j < plane_cnt; j++) {
		for(i = 0; i < buf_cnt; i++) {
			buf[i][j].status = 0;
			buf[i][j].size = size[j];
			buf[i][j].ion_client = client;
			ret = ion_alloc_fd(client, size[j], m_ionAlign, m_ionHeapMask, m_ionFlags, &(buf[i][j].fd));
			ALOGD("i:%d j:%d fd:%d ret:%d", i, j, buf[i][j].fd, ret);
			if (ret < 0) {
				ALOGE("ion_alloc_fd(fd=%d) failed:%d", client, ret);
				return -1;
			}
			buf[i][j].addr = (char *)mmap(NULL, size[j], PROT_READ|PROT_WRITE, MAP_SHARED, buf[i][j].fd, 0);
			if (buf[i][j].addr == NULL) {
				ALOGE("ion map failed");
				return -1;
			}
		}
	}
}

int _ion_buffer_free(int client, int buf_cnt, int plane_cnt, struct ion_buffer buf[][2]) {
	size_t m_ionAlign = 0;
	unsigned int m_ionHeapMask = ION_HEAP_SYSTEM_MASK;
	unsigned int m_ionFlags = EXYNOS_CAMERA_BUFFER_ION_FLAG_NONCACHED;
	int i, j, ret;
	for(j = 0; j < plane_cnt; j++) {
		for(i = 0; i < buf_cnt; i++) {
			if (j == 1) {
				if (munmap(buf[i][j].addr, buf[i][j].size) < 0) {
					ALOGE("munmap failed");
					return -1;
				}
			}
			ion_close(buf[i][j].fd);
		}
	}
}

int _ion_buffer_scp_free(int client, int buf_cnt, int plane_cnt, struct ion_buffer buf[][3]) {
	size_t m_ionAlign = 0;
	unsigned int m_ionHeapMask = ION_HEAP_SYSTEM_MASK;
	unsigned int m_ionFlags = EXYNOS_CAMERA_BUFFER_ION_FLAG_NONCACHED;
	int i, j, ret;
	for(j = 0; j < plane_cnt; j++) {
		for(i = 0; i < buf_cnt; i++) {
			if (j == 1) {
				if (munmap(buf[i][j].addr, buf[i][j].size) < 0) {
					ALOGE("munmap failed");
					return -1;
				}
			}
			ion_close(buf[i][j].fd);
		}
	}
}

static void dumpYUV(void *addr, int size, int index)
{

    ALOGD("dumpYUV E\n");
    char name[128] = {0};
    int count = 0;
    snprintf(name, sizeof(name), "/usr/share/misc/%d_%d_%d_num%d.yuv",
                                g_preview_w, g_preview_h, size, index);

    FILE *file_fd = fopen(name, "wb");

    if (file_fd == NULL) {
        ALOGE("open yuv file fail!\n");
    } else {
        count = fwrite(addr, 1, size, file_fd);
        if (count != size) {
            ALOGE("write yuv fail!\n");
        }
        fclose(file_fd);
    }

    ALOGD("dumpYUV X\n");
}

void sysUsecTime()  
{  
        struct timeval tv;
        struct timezone tz;
        struct tm *p;
        gettimeofday(&tv, &tz);

        p = localtime(&tv.tv_sec);
        ALOGE("%d-%d-%d %d:%d.%d.%ld\n",
                1900+p->tm_year, 1+p->tm_mon, p->tm_mday,
                p->tm_hour, p->tm_min, p->tm_sec, tv.tv_usec);
}

bool dumpToFile(char *filename, char *srcBuf, unsigned int size)
{
	FILE *yuvFd = NULL;
	char *buffer = NULL;

	yuvFd = fopen(filename, "rw");

	if (yuvFd == NULL) {
		ALOGE("ERR(%s):open(%s) fail",
			__func__, filename);
		return false;
	}

	buffer = (char *)malloc(size);

	if (buffer == NULL) {
		ALOGE("ERR(%s):malloc file", __func__);
		fclose(yuvFd);
		return false;
	}

	memcpy(buffer, srcBuf, size);

	fflush(stdout);

	fwrite(buffer, 1, size, yuvFd);

	fflush(yuvFd);

	if (yuvFd)
		fclose(yuvFd);
	if (buffer)
		free(buffer);

	ALOGE("DEBUG(%s):filedump(%s, size(%d) is successed!!",
		__func__, filename, size);

	return true;
}

void printPixFmt(int pixFmt) {

	switch(pixFmt) {
		case V4L2_PIX_FMT_RGB565:
			ALOGD("pix format: V4L2_PIX_FMT_RGB565");
		case V4L2_PIX_FMT_RGB32:
			ALOGD("pix format: V4L2_PIX_FMT_RGB32");
		case V4L2_PIX_FMT_NV21:
			ALOGD("pix format: V4L2_PIX_FMT_NV21");
		case V4L2_PIX_FMT_YVU420:
			ALOGD("pix format: V4L2_PIX_FMT_YVU420");
		case V4L2_PIX_FMT_NV12T:
			ALOGD("pix format: V4L2_PIX_FMT_NV12T");
		case V4L2_PIX_FMT_YUYV:
			ALOGD("pix format: V4L2_PIX_FMT_YUYV");
		case V4L2_PIX_FMT_YUV422P:
			ALOGD("pix format: V4L2_PIX_FMT_YUV422P");
		case V4L2_PIX_FMT_NV21T:
			ALOGD("pix format: V4L2_PIX_FMT_NV21T");
		case V4L2_PIX_FMT_INTERLEAVED:
			ALOGD("pix format: V4L2_PIX_FMT_INTERLEAVED");
		default:
			ALOGD("pix format: unknow -> %d", pixFmt);
	}
}

bool cfg_from_File(char *filename, char *srcBuf, unsigned int size)
{
	FILE *yuvFd = NULL;
	char *buffer = NULL;

	yuvFd = fopen(filename, "r+");

	if (yuvFd == NULL) {
		ALOGE("ERR(%s):open(%s) fail",
			__func__, filename);
	return false;
	}

	fread(srcBuf, 1, size, yuvFd);



	if (yuvFd)
		fclose(yuvFd);
	if (buffer)
		free(buffer);

	ALOGD("DEBUG(%s):filedump(%s, size(%d) is successed!!",
		__func__, filename, size);

	return true;
}

void copy_cfg_from_output(struct camera2_shot_ext *shot_ext_src, struct camera2_shot_ext *shot_ext_dst) {
	if (shot_ext_src != NULL && shot_ext_dst != NULL) {
		memcpy(&shot_ext_dst->shot.ctl, &shot_ext_src->shot.ctl, sizeof(struct camera2_ctl) - sizeof(struct camera2_entry_ctl));
		memcpy(&shot_ext_dst->shot.udm, &shot_ext_src->shot.udm, sizeof(struct camera2_udm));
		memcpy(&shot_ext_dst->shot.dm, &shot_ext_src->shot.dm, sizeof(struct camera2_dm));

		shot_ext_dst->setfile = shot_ext_src->setfile;
		shot_ext_dst->drc_bypass = shot_ext_src->drc_bypass;
		shot_ext_dst->dis_bypass = shot_ext_src->dis_bypass;
		shot_ext_dst->dnr_bypass = shot_ext_src->dnr_bypass;
		shot_ext_dst->fd_bypass = shot_ext_src->fd_bypass;

		shot_ext_dst->shot.dm.request.frameCount = shot_ext_src->shot.dm.request.frameCount;
		shot_ext_dst->shot.magicNumber= shot_ext_src->shot.magicNumber;
	}
}

int ion_buffer_alloc() {
	int ret;
	int m_ionClient = open("/dev/ion", O_RDWR);
	if (m_ionClient < 0)
		ALOGE("open /dev/ion failed!\n");
	ALOGD("open ion done!!");
	size_t size[3] = {0};

	memset(ion_buf_isp, 0, sizeof(struct ion_buffer) * 8 * 2);
	size[0] = calc_buffer_size(g_preview_w, g_preview_h);
	size[1] = 32768;
	ret = _ion_buffer_alloc(m_ionClient, size, 8, 2, ion_buf_isp);
	if (ret < 0) {
		ALOGE("ion_buf_isp ion_buffer_alloc(fd=%d) failed:%d", m_ionClient, ret);
		return -1;
	}
	ALOGD("ion_buf_isp alloc memery done");

	return 0;
}

int ion_buffer_free() {
	int ret;

	ret = _ion_buffer_free(m_ionClient, 9, 2, ion_buf_isp);
	if (ret < 0) {
		ALOGE("ion_buf_isp ion_buffer_free(fd=%d) failed:%d", m_ionClient, ret);
		return -1;
	}
	ALOGD("ion_buf_isp free memery done");

	ret = _ion_buffer_free(m_ionClient, 6, 2, ion_buf_isp);
	if (ret < 0) {
		ALOGE("ion_buf_bayer ion_buffer_free(fd=%d) failed:%d", m_ionClient, ret);
		return -1;
	}
	ALOGD("ion_buf_bayer free memery done");

	ret = _ion_buffer_free(m_ionClient, 9, 2, ion_buf_3a1);
	if (ret < 0) {
		ALOGE("ion_buf_3a1 ion_buffer_free(fd=%d) failed:%d", m_ionClient, ret);
		return -1;
	}
	ALOGD("ion_buf_3a1 free memery done");

	ret = _ion_buffer_scp_free(m_ionClient, 12, 3, ion_buf_scp);
	if (ret < 0) {
		ALOGE("ion_buf_scp ion_buffer_free(fd=%d) failed:%d", m_ionClient, ret);
		return -1;
	}
	ALOGD("ion_buf_scp free memery done");

	return 0;
}

int dqbuf_status(struct ion_buffer buf[][2], int buf_index, int status) {
	buf[buf_index][0].dqbuf_ok = status;
}

int qbuf_status(struct ion_buffer buf[][2], int buf_index, int status) {
	pthread_mutex_lock(&(buf[0][0].lock));
	buf[buf_index][0].status = status;
	pthread_mutex_unlock(&(buf[0][0].lock));
}

int qbuf_scp_status(struct ion_buffer buf[][3], int buf_index, int status) {
	pthread_mutex_lock(&(buf[0][0].lock));
	buf[buf_index][0].status = status;
	pthread_mutex_unlock(&(buf[0][0].lock));
}

int qbuf_status_count(struct ion_buffer buf[][2], int status) {
	int i;
	int ret = 0;
	for (i = 0; i < buf[0][0].count; i++) {
		//ALOGD("check buf[%d][0] due status:%d, actually status:%d, count:%d", i, buf[i][0].status, buf[0][0].count);
		if (buf[i][0].status == status)
			ret++;
	}
	return ret;
}

int qbuf_chk_status(struct ion_buffer buf[][2], int status, int dbg) {
	int i;
	for (i = 0; i < buf[0][0].count; i++) {
		if (dbg)
			ALOGD("check buf[%d][0] due status:%d, actually status:%d, count:%d", i, status, buf[i][0].status, buf[0][0].count);
		if (buf[i][0].status == status)
			return i;
	}
	return -1;
}

int qbuf_status_print(struct ion_buffer buf[][2]) {
	int i;
	for (i = 0; i < buf[0][0].count; i++) {
		ALOGD("check buf[%d][0] -> status:%d, count:%d", i, buf[i][0].status, buf[0][0].count);
	}
	return 0;
}

int qbuf_one(int fd, int buf_index, int plane_cnt, int type, struct ion_buffer buf[][2], int status, int idx_begin) {
	struct v4l2_buffer v4l2_buf;
	struct v4l2_plane planes[VIDEO_MAX_PLANES];
	int i, ret;

	memset(&v4l2_buf, 0, sizeof(struct v4l2_buffer));
	memset(&planes, 0, sizeof(struct v4l2_plane) * VIDEO_MAX_PLANES);
	v4l2_buf.m.planes = planes;
	v4l2_buf.type     = type;
	v4l2_buf.memory   = V4L2_CAMERA_MEMORY_TYPE;
	v4l2_buf.index    = buf_index;
	v4l2_buf.length   = plane_cnt;
	for (i = 0; i < (int)v4l2_buf.length; i++) {
		if (v4l2_buf.memory == V4L2_MEMORY_DMABUF) {
			v4l2_buf.m.planes[i].m.fd = buf[buf_index][i].fd;
		} else if (v4l2_buf.memory == V4L2_CAMERA_MEMORY_TYPE) {
			v4l2_buf.m.planes[i].m.userptr = buf[buf_index][i].addr;
		} else {
			ALOGE("invalid srcNode->memory(%d)", v4l2_buf.memory);
			return -1;
		}

		v4l2_buf.m.planes[i].length = buf[buf_index][i].size;
	}
	ret = exynos_v4l2_qbuf(fd, &v4l2_buf);
	if (ret < 0) {
		ALOGE("exynos_v4l2_qbuf(m_fd:%d, buf->index:%d) fail (%d)", fd, v4l2_buf.index, ret);
		return ret;
	}
	//set_buf_status(buf_index, 1, buf);
	if (status != 9999) {
		pthread_mutex_lock(&(buf[0][0].lock));
		buf[buf_index][0].status = 1;
		pthread_mutex_unlock(&(buf[0][0].lock));
	}
        //printf("QBUF_one done!\n");
	ALOGD("init buff index:%d", buf_index);
	return buf_index;
}

int qbuf_scp_one(int fd, int buf_index, int plane_cnt, int type, struct ion_buffer buf[][3], int status, int idx_begin) {
	struct v4l2_buffer v4l2_buf;
	struct v4l2_plane planes[VIDEO_MAX_PLANES];
	int i, ret;

	if (status != 9999) {
		buf_index = -1;
		if (idx_begin) {
			i = 0;
		} else {
			i = buf[0][0].index;
			buf[0][0].index++;
			if (buf[0][0].index >= buf[0][0].count)
				buf[0][0].index = 0;	
		}
		for (;i < buf[0][0].count; i++) {
			ALOGD(" buf[%d][0] status=%d", i, buf[i][0].status);
			if (buf[i][0].status == status) {
				buf_index = i;
				break;
			}
		}
		if (buf_index < 0)
			return -100;
	}
	memset(&v4l2_buf, 0, sizeof(struct v4l2_buffer));
	memset(&planes, 0, sizeof(struct v4l2_plane) * VIDEO_MAX_PLANES);
	v4l2_buf.m.planes = planes;
	v4l2_buf.type     = type;
	v4l2_buf.memory   = V4L2_CAMERA_MEMORY_TYPE;
	v4l2_buf.index    = buf_index;
	v4l2_buf.length   = plane_cnt;
	for (i = 0; i < (int)v4l2_buf.length; i++) {
		if (v4l2_buf.memory == V4L2_MEMORY_DMABUF) {
			v4l2_buf.m.planes[i].m.fd = buf[buf_index][i].fd;
		} else if (v4l2_buf.memory == V4L2_CAMERA_MEMORY_TYPE) {
			v4l2_buf.m.planes[i].m.userptr = buf[buf_index][i].addr;
		} else {
			ALOGE("invalid srcNode->memory(%d)", v4l2_buf.memory);
			return -1;
		}

		v4l2_buf.m.planes[i].length = buf[buf_index][i].size;
	}
	if (v4l2_buf.memory == V4L2_MEMORY_DMABUF) {
		v4l2_buf.m.planes[v4l2_buf.length - 1].m.fd = buf[buf_index][v4l2_buf.length - 1].fd;
	} else if (v4l2_buf.memory == V4L2_CAMERA_MEMORY_TYPE) {
		v4l2_buf.m.planes[v4l2_buf.length - 1].m.userptr = buf[buf_index][v4l2_buf.length - 1].addr;
	} else {
		ALOGE("invalid meta(%d)", v4l2_buf.memory);
		return -1;
	}
	/* set fence */
	v4l2_buf.flags = V4L2_BUF_FLAG_USE_SYNC;
	v4l2_buf.reserved = -1;
	ret = exynos_v4l2_qbuf(fd, &v4l2_buf);
	if (ret < 0) {
		ALOGE("exynos_v4l2_qbuf(m_fd:%d, buf->index:%d) fail (%d)", fd, index, ret);
		return ret;
	}
	//set_buf_status(buf_index, 1, buf);
	if (status != 9999) {
		pthread_mutex_lock(&(buf[0][0].lock));
		buf[buf_index][0].status = 1;
		pthread_mutex_unlock(&(buf[0][0].lock));
	}
	ALOGD("init buff index:%d", buf_index);
	return buf_index;
}

int dqbuf_one(int fd, int plane_cnt, int type) {
	struct v4l2_buffer v4l2_buf;
	struct v4l2_plane planes[VIDEO_MAX_PLANES];
	int index, ret;

	memset(&v4l2_buf, 0, sizeof(struct v4l2_buffer));
	memset(&planes, 0, sizeof(struct v4l2_plane) * VIDEO_MAX_PLANES);
	v4l2_buf.type       = type;
	v4l2_buf.memory     = V4L2_CAMERA_MEMORY_TYPE;
	v4l2_buf.m.planes   = planes;
	v4l2_buf.length     = plane_cnt;
	//v4l2_buf.reserved = 0;
	ret = exynos_v4l2_dqbuf(fd, &v4l2_buf);
	//v4l2_buf.reserved = 0;
	if (ret < 0) {
		if (ret != -EAGAIN)
			ALOGE("exynos_v4l2_dqbuf(fd:%d) fail (%d)", fd, ret);
		return ret;
	}
	 index = v4l2_buf.index;
	 if (index < 0) {
		ALOGE("Invalid index(%d) fail", index);
		return index;
	}
	//printf("dqbuf_one done!\n");
	return index;
}

void preview_thread_isp(void) {

	int i,err,dqbuf_index,index_scp_p,buf_size,ret;
	GstFlowReturn flowRet = GST_FLOW_OK;
	unsigned char *planeOne =  NULL;
        unsigned char *yuv420 = NULL;
        unsigned char *testYUV = NULL;
	unsigned char *bgr_data = NULL;
        const int count = 40;
        const long timeOut = 1500;

        struct pollfd events;
        /*for test
        FILE *fpr = NULL;
        fpr = fopen("/usr/share/misc/640_480_614400_num80.yuv", "rb" );  
        if( fpr == NULL)  
        {  
            printf("can not read or write file\n");  
            fclose(fpr);  
            return 1;  
        }
        testYUV = (unsigned char *) malloc(READ_WRITE_FILE_SIZE);
        memset(testYUV, 0x0, READ_WRITE_FILE_SIZE);
        fread(testYUV, READ_WRITE_FILE_SIZE, 1, fpr);
        *///for test end
	while(previewFlag) {
                memset(&events, 0x0, sizeof (struct pollfd));
		events.fd = g_fd_flite;
		events.events = POLLIN | POLLERR;
		err = poll(&events, 1, TIMEOUT);
		if (err > 0)
		{
			dqbuf_index=dqbuf_one(g_fd_flite, 2, V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE);
		} else if (0 == err) {
                        ALOGE("poll date timeout.\n");
                        continue;
                } else {
                        ALOGE("poll err ret(%d)!\n", err);
                        continue;
                }
		index_scp_p = dqbuf_index;

		// Push plane 1 data
		buf_size = ion_buf_isp[index_scp_p][0].size ;
		if (ion_buf_isp[index_scp_p][0].size > 0) {
#if 1
                        
#ifdef DUMP_UYVY 
                        dumpYUV(ion_buf_isp[dqbuf_index][0].addr,
                                        ion_buf_isp[dqbuf_index][0].size,
                                                g_frame_count);
#endif
                        g_frame_count++;
                        planeOne = (unsigned char *)malloc(buf_size);
                        memset(planeOne, 0x0, buf_size);
			memcpy(planeOne, ion_buf_isp[index_scp_p][0].addr,
                                               ion_buf_isp[index_scp_p][0].size);
#ifdef GST_SCALE
                        //usleep(100000);
        		GstBuffer *planeOneData = gst_buffer_new_wrapped(planeOne, buf_size);
#endif

#ifdef PIX_YUVI420
                        yuv420 = (unsigned char *)malloc(g_preview_w * g_preview_h * 3 / 2);
                        memset(yuv420, 0x0, g_preview_w * g_preview_h * 3 / 2);
                        ret = uyvy422toyuvI420(yuv420, planeOne, g_preview_w, g_preview_h);
#ifdef dump_yuvI420
                        dumpYUV(yuv420, g_preview_w * g_preview_h * 3 / 2, g_frame_count);
#endif//dump_yuvI420
			GstBuffer *planeOneData = gst_buffer_new_wrapped(yuv420, g_preview_w * g_preview_h * 3 / 2);
#endif//PIX_YUVI420
			g_signal_emit_by_name(p_appsrc, "push-buffer", planeOneData, &flowRet);
			if (flowRet != GST_FLOW_OK)
			{
				ALOGD("send planeOneData to pipeline failed\n");
			}

#ifdef PIX_YUVI420
                        free(yuv420);
                        yuv420 = NULL;
#endif
			free(planeOne);
                        planeOne = NULL;
#endif//for #if 1, debug kernel framerate when set to 0.
                        sysUsecTime();
                }

		ret = qbuf_one(g_fd_flite, dqbuf_index, 2, V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE, ion_buf_isp, 0, 0);
		if (ret == -100) {
			ALOGD("isp wait, index:%d", ion_buf_isp[0][0].index);
			qbuf_status_print(ion_buf_isp);
		}

	}

}

int get_buf_status(int index, struct ion_buffer buf[][2]) {
	int ret;
	pthread_mutex_lock(&(buf[index][0].lock));
	ret = buf[index][0].status;
	pthread_mutex_unlock(&(buf[index][0].lock));
	return ret;
}

int set_buf_status(int index, int status, struct ion_buffer buf[][2]) {
	pthread_mutex_lock(&(buf[index][0].lock));
	buf[index][0].status = status;
	pthread_mutex_unlock(&(buf[index][0].lock));
	return 0;
}

int scp_p_dump(int index) {
	#if 1
	int i;
	int begin = 20000;
	char value;
	ALOGE("scp preview buffer adder:%p, size:%d", ion_buf_scp[index][0].addr, ion_buf_scp[index][0].size);
	for(i = begin; i < begin + 1; i++) {
		value = (char)*(ion_buf_scp[index][0].addr + i);
		ALOGE("scp buffer data[%d]=0x%x", i, (int)value);
	}
	#endif
}

int scp_p_polling(void)
{
	struct pollfd events;

	/* 50 msec * 100 = 5sec */
	int cnt = 100;
	long sec = 50; /* 50 msec */

	int ret = 0;
	int pollRet = 0;

	events.fd = g_fd_scp_p;
	events.events = POLLIN | POLLRDNORM | POLLOUT | POLLWRNORM | POLLERR;
	events.revents = 0;

	while (cnt--) {
		pollRet = poll(&events, 1, sec);
		if (pollRet < 0) {
			ret = -1;
		} else if (0 < pollRet) {
			if (events.revents & POLLIN) {
				break;
			} else if (events.revents & POLLERR) {
				ret = -1;
			}
		}
	}

	if (ret < 0 || cnt <= 0) {
		ALOGE("poll[%d], pollRet(%d) event(0x%x), cnt(%d)", g_fd_scp_p, pollRet, events.revents, cnt);
		if (cnt <= 0)
			ret = -1;
	}
	ALOGD("poll");
	return ret;
}

int close_is() {
	int ret;
	struct v4l2_buffer v4l2_buf;
	ret = exynos_v4l2_s_ctrl(g_fd_flite, V4L2_CID_IS_S_STREAM, 0);
	if (ret < 0) {
		ALOGE("exynos_v4l2_s_ctrl(fd:%d) fail (%d) [V4L2_CID_IS_S_STREAM(%d), value %d]", g_fd_flite, ret, V4L2_CID_IS_S_STREAM, 0);
		goto failed;
	}
	ALOGE("g_fd_flite set ctrl done!");

	ret = exynos_v4l2_streamoff(g_fd_flite, V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE);
	if (ret < 0) {
		ALOGE("exynos_v4l2_streamoff(fd:%d) fail (%d)", g_fd_flite, ret);
		goto failed;
	}
	ALOGE("g_fd_flite stream off done!");
	memset(&m_v4l2ReqBufs, 0x0, sizeof(struct v4l2_requestbuffers));
	m_v4l2ReqBufs.count  = 0;
	m_v4l2ReqBufs.type   = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
	m_v4l2ReqBufs.memory = V4L2_CAMERA_MEMORY_TYPE;
	ret = exynos_v4l2_reqbufs(g_fd_flite, &m_v4l2ReqBufs);
	if (ret < 0) {
		ALOGE("g_fd_flite exynos_v4l2_reqbufs(fd:%d, count:%d) fail (%d)", g_fd_flite, m_v4l2ReqBufs.count, ret);
		goto failed;
	}
	ALOGE("g_fd_flite set v4l2 request buffers done!!");

	#if 1
	exynos_v4l2_close(g_fd_flite);

	#endif

	ion_buffer_free();

	return 0;
failed:
	return ret;
}

int main_is() {
	int err;
	bool found = false;
	int index_scp_p = 2;
	struct v4l2_buffer  enqueue  , dequeue;
	int ret;
	int buf_size = 0;
	unsigned int input, i,j,dqbuf_index;
	struct v4l2_streamparm streamParam;
        char yuyv[2560*720*2]  = {0};
        char *yuv[8] = {NULL};
	struct v4l2_control ctrl;
	//CLEAR(ctrl);
	ctrl.id = CAM_CID_CAPTURE_MODE;
	ctrl.value = false;

	//s_fmt
	struct v4l2_format v4l2_fmt;
	//CLEAR(v4l2_fmt);
	v4l2_fmt.type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
	v4l2_fmt.fmt.pix_mp.width = g_preview_w;
	v4l2_fmt.fmt.pix_mp.height = g_preview_h;
	v4l2_fmt.fmt.pix_mp.pixelformat = V4L2_PIX_FMT_YUYV;
	v4l2_fmt.fmt.pix_mp.field = 0;
	err = ioctl(g_fd_flite, VIDIOC_S_FMT, &v4l2_fmt);

	streamParam.type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
	streamParam.parm.capture.timeperframe.numerator   = 1;
	streamParam.parm.capture.timeperframe.denominator = 30;
	err = ioctl(g_fd_flite, VIDIOC_S_PARM,&streamParam);
	/* fimc_v4l2_reqbufs */
	struct v4l2_requestbuffers req;
	//CLEAR(req);
	req.count = V4L2_REQUEST_BUFF;
	req.type   = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
	req.memory = V4L2_CAMERA_MEMORY_TYPE;
	err = ioctl(g_fd_flite, VIDIOC_REQBUFS, &req);
	if (ion_buffer_alloc()) {
		return -1;
	}
	for(i=0;i<req.count;i++) {

                ret = qbuf_one(g_fd_flite, i, 2, V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE, ion_buf_isp, 0, 0);
		if (ret == -100) {
			ALOGD("isp wait, index:%d", ion_buf_isp[0][0].index);
			qbuf_status_print(ion_buf_isp);
		}
	}

	//stream on
	enum v4l2_buf_type type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
	// CLEAR(ctrl);
	ctrl.id = V4L2_CID_IS_S_STREAM;
	ctrl.value = IS_ENABLE_STREAM;
	ret = ioctl(g_fd_flite, VIDIOC_S_CTRL, &ctrl);
	if (ret < 0)
	        ALOGE("s_stream: error %d", ret);
	ret = ioctl(g_fd_flite,VIDIOC_STREAMON, &type);
	if(ret<0)
	        ALOGE("FLiteV4l2 stream: error %d", ret);

	ret = pthread_create(&pid_isp, NULL, (void *) preview_thread_isp,NULL);
failed:
	//exynos_v4l2_close(m_fd);
	return 0;
}

static void gst_need_data(GstElement *source, guint size, gpointer user_data) {
	g_need_data = 1;
	ALOGD("+++");
}

static void gst_enough_data(GstElement *source, gpointer user_data) {
	g_need_data = 0;
	ALOGD("+++");
}

void *thread_function(void *arg) {
	int rc;
	struct input_event event;
	int fd = open(EVENT_NAME, O_RDWR, 0);
	while ((rc = read(fd, &event, sizeof(event))) > 0) {
		if ((event.type == EV_KEY) && (event.code == POWER_VALUE)) {
			if (event.value) {
				printf("button press ........................... \n");
				if(gstState == GST_STATE_PLAYING) {
					gtk_main_quit ();
					gst_element_set_state (play, GST_STATE_NULL);
					gstState = GST_STATE_NULL;
				}
			}
		}
	}
}

int main(int argc, char *argv[]) {
	GstBus *bus;
	gchar para[STREAM_PARAMETER_MAX_LENGTH];
	char node_name[30];
	int ret;
	int res;
        int fd_flite;
        char* mFLITE = "/dev/video101";

	pthread_t power_thread;
	res = pthread_create(&power_thread, NULL, thread_function, NULL);
	if (res != 0) {
		perror("Thread creation failed!");
		exit(EXIT_FAILURE);
	}
	g_fd_flite = exynos_v4l2_open(mFLITE, O_RDWR, 0);
	if (g_fd_flite < 0) {
		ALOGE("fd_flite exynos_v4l2_open(%s) fail, ret(%d)", mFLITE, g_fd_flite);
		goto failed;
	}
        ret = exynos_v4l2_s_input(g_fd_flite, INPUT_INDEX_FLITE);
	if (ret < 0) {
		ALOGE("fd_flite exynos_v4l2_s_input(fd:%d) fail (%d)", g_fd_flite, ret);
		goto failed;
	}
        /* Configure preview width and height */
        select_prev_resolution(g_fd_flite);
#if 1
	gtk_init (&argc, &argv);
	gst_init (&argc, &argv);
	create_ui();


#ifdef PIX_YUVI420
        snprintf(para, STREAM_PARAMETER_MAX_LENGTH,
                "appsrc name=videoappsrc is-live=true ! videoparse format=GST_VIDEO_FORMAT_I420 "
                        "width=%d height=%d framerate=30/1 ! videoconvert ! glimagesink display=:0 sync=false",
                                g_preview_w, g_preview_h);
#endif

#ifdef GST_SCALE
        snprintf(para, STREAM_PARAMETER_MAX_LENGTH,
                "appsrc name=videoappsrc is-live=true ! videoparse format=GST_VIDEO_FORMAT_UYVY "
                        "width=%d height=%d framerate=30/1 ! videoconvert ! videoscale ! ximagesink display=:0 sync=false",
                                1024, 576);
#endif

#ifdef GST_UYVY
        snprintf(para, STREAM_PARAMETER_MAX_LENGTH,
                "appsrc name=videoappsrc is-live=true ! videoparse format=GST_VIDEO_FORMAT_UYVY "
                        "width=%d height=%d framerate=30/1 ! videoconvert ! glimagesink display=:0 sync=false",
                                g_preview_w, g_preview_h);
#endif
	ALOGE("para is %s\n", para);
        play = gst_parse_launch (para, NULL);
	if (play == NULL)
	{
		ALOGE("Can't create pipeline\n");
	}
	p_appsrc = gst_bin_get_by_name (GST_BIN (play), "videoappsrc");
	g_signal_connect (p_appsrc, "need-data", G_CALLBACK (gst_need_data), NULL);
	g_signal_connect (p_appsrc, "enough-data", G_CALLBACK (gst_enough_data), NULL);
	bus = gst_pipeline_get_bus (GST_PIPELINE (play));
	gst_bus_set_sync_handler (bus, (GstBusSyncHandler) bus_sync_handler, NULL, NULL);
	gst_bus_add_watch (bus, bus_callback, NULL);
	gst_object_unref (bus);
	gst_element_set_state (play, GST_STATE_PLAYING);
	gstState = GST_STATE_PLAYING;
#endif

	previewFlag = 1;
	main_is();
	gtk_main ();
	previewFlag = 0;
	close_is();
	if(gstState != GST_STATE_NULL)
	{
		gst_element_set_state (play, GST_STATE_NULL);
		gst_object_unref (play);
	}
failed:
	return 0;
}

