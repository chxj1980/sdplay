/**
* @file traversal_dir.c
* @author rigensen
* @brief 
* @date 三 10/ 9 11:55:06 2019
*/
#include <stdio.h>
#include <string.h>
#include <time.h>
#include <dirent.h>
#include <sys/time.h>
#include <inttypes.h>
#include <stdint.h>

int64_t get_time_us()
{
    struct timeval tv;

    gettimeofday(&tv, NULL);
    return tv.tv_sec*(int64_t)(1000000)+tv.tv_usec;
}

int main()
{
    DIR *dir = NULL;
    struct dirent *node_ptr = NULL;
    int i = 0;
    int64_t start,end;

    start = get_time_us();
    dir = opendir("./");
    while( (node_ptr = readdir(dir)) ) {
        if (node_ptr->d_type == DT_REG && (strncmp(node_ptr->d_name, ".", 1)) ) {
            printf("[%05d] %s\n", i++,  node_ptr->d_name );
        }
    }

    closedir(dir);
    end = get_time_us();
    printf("\ncost time:%"PRId64"us\n", end-start);

    return 0;
}

