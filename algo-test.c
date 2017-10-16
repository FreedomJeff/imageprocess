#include <stdio.h>
#include <string.h>

#define CAMERA_WIDTH 1280
#define CAMERA_HEIGHT 960
#define CAMERA_NUM 4

#define READ_WRITE_FILE_SIZE (CAMERA_WIDTH * CAMERA_NUM * CAMERA_HEIGHT * 2)
#define CAMERA_REMOSAIC

unsigned char buf_frame[READ_WRITE_FILE_SIZE] = {0};  
unsigned char buf_remosaic[READ_WRITE_FILE_SIZE] = {0};
unsigned char buf_downscale[CAMERA_WIDTH*CAMERA_HEIGHT*2]= {0};

typedef unsigned char u8;
typedef unsigned short u16;
typedef unsigned int u32;

u8 filebuf[5120*960*3/2] = {0};//for i420@5120x960

#define WIDTH_SCALE 1280
#define HEIGHT_SCALE 240
u8 i420_buf[WIDTH_SCALE*HEIGHT_SCALE*3/2] = {0};

int uyvy_remosaic(u8 *old_yuv422, u8 *new_yuv422, u32 width, u32 height)
{
    u32 offset_screen3;
    u32 old_line_size;
    u32 new_line_size;
    u32 i;
    if (NULL == old_yuv422 || NULL == new_yuv422) {
        return -1; 
    }
    offset_screen3 = width * height * 2 * 2;
    old_line_size = width * 2 * 4;
    new_line_size = width * 2 * 2;
    for(i = 0; i < height; i++) {
        //screen 1+2
        memcpy(new_yuv422+i*new_line_size, old_yuv422+i*old_line_size, new_line_size);
        //screen 3+4
        memcpy(new_yuv422+i*new_line_size+ offset_screen3, old_yuv422+i*old_line_size+new_line_size, new_line_size);
    }
    return 0;
}
int uyvy_downscale_quarter(u8 *old_yuv422, u8 *new_yuv422, u32 width, u32 height)
{
        u32 old_line_size;
        u32 new_line_size;
        u32 i,j;
        u16* pOldScreen12;
        u16* pNewScreen12;
        if (NULL == old_yuv422 || NULL == new_yuv422) {
            return -1; 
        }   
        old_line_size = width * 2 * 4;
        new_line_size = width * 2 * 2;
        pOldScreen12=(u16*)old_yuv422;
        pNewScreen12=(u16*)new_yuv422;
        for(i = 0; i < height/2; i++) {
                pOldScreen12=(u16*)(old_yuv422+i*2*old_line_size);
                pNewScreen12=(u16*)(new_yuv422+i*new_line_size);		
                for(j = 0; j < width*2; j++) {
                    pNewScreen12[j]=pOldScreen12[j*2];//UY
                    pNewScreen12[j+1]=pOldScreen12[j*2+1];//VY
		}            
        }   
        return 0;
}

int camera_scale_down_software(u8 *src, u32 src_w, u32 src_h, u8 *dst, u32 dst_w, u32 dst_h)
{
    u8 *dst_y_buf;
    u8 *dst_uv_buf;
    u8 *src_y_buf;
    u8 *src_uv_buf;
    u32 dst_uv_w;
    u32 dst_uv_h;
    u32 cur_w = 0;
    u32 cur_h = 0;
    u32 cur_size = 0;
    u32 cur_byte = 0;
    u32 ratio_w;
    u32 ratio_h;
    u32 i, j;
    if (NULL == dst || NULL == src) {
        return -1;
    }
    dst_y_buf = dst;
    dst_uv_buf = dst + dst_w * dst_h;
    src_y_buf = src;
    src_uv_buf = src + src_w * src_h;
    dst_uv_w = dst_w / 2;
    dst_uv_h = dst_h / 2;
    ratio_w = (src_w << 10) / dst_w;
    ratio_h = (src_h << 10) / dst_h;
    for (i = 0; i < dst_h; i++) {
        cur_h = (ratio_h * i) >> 10;
        cur_size = cur_h * src_w;
        for (j = 0; j < dst_w; j++) {
            cur_w = (ratio_w * j) >> 10;
            *dst_y_buf++ = src_y_buf[cur_size + cur_w];
        }
    }
    for (i = 0; i < dst_uv_h; i++) {
        cur_h = (ratio_h * i) >> 10;
        cur_size = cur_h * (src_w / 2) * 2;
        for (j = 0; j < dst_uv_w; j++) {
            cur_w = (ratio_w * j) >> 10;
            cur_byte = cur_size + cur_w * 2;
            *dst_uv_buf++ = src_uv_buf[cur_byte];     // u
            *dst_uv_buf++ = src_uv_buf[cur_byte + 1]; // v
        }
    }
    printf("done");
    return 0;
}

int uyvy422toyuvI420(unsigned char *yuv420, const unsigned char *yuv422, unsigned int width, unsigned int height)
{
    printf("start.\n");
        unsigned char *y = yuv420;
        unsigned char *u = yuv420 + width * height;
        unsigned char *v = yuv420 + width * height + width * height / 4;

        unsigned int i,j;
        unsigned int base_h;
        unsigned int is_y = 1, is_u = 1;
        unsigned int y_index = 0, u_index = 0, v_index = 0;
        unsigned long yuv422_length = 2 * width * height;

        for(i = 1; i < yuv422_length; i += 2) {
                *(y + y_index) = *(yuv422 + i);
                y_index++;
        }

        for(i = 0; i < height; i += 2) {
                base_h = i * width * 2;
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

        printf("end.\n");
        return 1;
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

int crop_yuv (char* data, char*dst, int width, int height, int goalwidth, int goalheight)
{
	int i, j;
	int h_div = 0, w_div = 0;
	w_div= (width - goalwidth) / 2;
	if (w_div % 2)
        	w_div--;
	h_div= (height - goalheight) / 2;
	if (h_div % 2)
		h_div--;
	//u_div = (height-goalheight)/4;
	int src_y_length = width *height;
	int dst_y_length =goalwidth * goalheight; 
	for (i = 0; i <goalheight; i++) 
		for (j = 0; j <goalwidth; j++) { 
			dst[i* goalwidth + j] = data[(i + h_div) * width + j + w_div]; 
		}
	int index = dst_y_length; 
	int src_begin =src_y_length + h_div * width / 4; 
	int src_u_length =src_y_length / 4;
	int dst_u_length =dst_y_length / 4;
	for (i = 0; i <goalheight / 2; i++) 
		for (j = 0; j <goalwidth / 2; j++) { 
			int p = src_begin + i *(width >> 1) + (w_div >> 1) + j; 
			dst[index]= data[p]; 
			dst[dst_u_length+ index++] = data[p + src_u_length]; 
		} 
	return 0;
}

int main(void)  
{  int len = 0;  
    FILE *fpr, *fpw;  
    //fpr = fopen( "1280_720_1843200_num9.yuv", "rb" );  
    fpr = fopen( "5120_960_9830400_num11.yuv", "rb" );  
    fpw = fopen( "target.yuv", "wb" );  
    if( fpr == NULL || fpw == NULL )  
    {  
        printf("can not read or write file\n");  
        fcloseall();  
        return 1;  
    }  
    fread( buf_frame, READ_WRITE_FILE_SIZE, 1, fpr );
    //if(uyvy422toyuvI420(filebuf, buf_frame, 5120,960))
    //    printf("ok\n");  
    printf("size:%d",sizeof(filebuf));  

#if 0//def CAMERA_SCALE_DOWN
    camera_scale_down_software(filebuf, 5120,960,i420_buf,WIDTH_SCALE,HEIGHT_SCALE);
    fwrite( &i420_buf, sizeof(i420_buf), 1, fpw);
#endif

#ifdef CAMERA_REMOSAIC
    //uyvy_remosaic(buf_frame,buf_remosaic,CAMERA_WIDTH,CAMERA_HEIGHT);
	uyvy_downscale_quarter(buf_frame,buf_downscale,CAMERA_WIDTH,CAMERA_HEIGHT);
    fwrite(&buf_downscale, sizeof(buf_downscale), 1, fpw);
#endif
    fcloseall();  
    return 0;  
}  
