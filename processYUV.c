#include <stdio.h>
#include <stdlib.h>
#include <string.h>

int main(int argc, char **argv)
{
    FILE * pFile;
    FILE * yFile;
    FILE * greyFile;
    long lsize, ysize;
    char *buffer = NULL;
    char *y_buffer;
    size_t result;

    /* open the file in binary format. */
    pFile = fopen("previewyuvdata_frame180_num185_width1280_height960_id.yuv", "rb");//YUV420(nv12)
    if (pFile == NULL) {
        fputs("file error", stderr); 
        exit(1);
    }  

    /* get the size of the file. */
    fseek(pFile, 0, SEEK_END);
    lsize = ftell(pFile);
    rewind(pFile);
    ysize = lsize * 2 / 3; 
    printf("size of YUV file:%ld;size of Y data:%ld\n", lsize, ysize);//1843200(1280*960*3/2),[YYYYYYYYYVVUU]
    
    /* allocate memory to save the opened file.*/
    buffer = (char *) malloc(lsize * sizeof (char));
    y_buffer = (char *)malloc(ysize * sizeof (char));

    if (buffer == NULL) {
        fputs("memory alloc error",stderr);
        exit(-1);
    }  

    /* copy file to memory. */
    result = fread(buffer, 1, lsize, pFile);
    if (result != lsize) {
        fputs("reading file error", stderr); 
        exit(-2);
    }  

    rewind(pFile);
    
    memset(buffer + ysize, 0, lsize - ysize);
    greyFile = fopen("greyImage.yuv", "wb");
    result = fwrite(buffer, 1, lsize, greyFile);
    rewind(pFile);


    /* copy Y data to memory. */
    result = fread(y_buffer, 1, ysize, pFile);
    if (result != ysize) {
        fputs("read Y data error", stderr);
        exit(-3);
    }
    yFile = fopen("yImage.yuv", "wb");
    if (yFile == NULL) {
        fputs("open new file error", stderr);
        exit(-4);
    }

    /* write the Y data to file.*/
    result = fwrite(y_buffer, 1, ysize, yFile);
    if (result != ysize) {
        fputs("write y data to file error", stderr);
        exit(-5);
    }

    /* close file and free memory.*/
    fclose(greyFile);
    fclose(pFile);
    fclose(yFile);
    free(buffer);
    free(y_buffer);

    return 0;
}
