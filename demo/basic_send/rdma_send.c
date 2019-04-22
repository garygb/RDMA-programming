#include <infiniband/verbs.h>
#include <stdio.h>

// MR的大小
#define REGION_SIZE 0x1800

// 实际过程中需要传入的参数：
// 1. device_name
int main() {

    struct ibv_device **device_list;
    int    number_device;
    int    i;
    char   *devname = "mlx4_0";

    device_list = ibv_get_device_list(&number_device);

    // 没找到device_list的情况
    if (!device_list) {
        perror("Failed to get IB device list");
        return 1;
    }

    // 第一步：获取设备列表
    for (i = 0; i < number_device; i++) {
        if (!strcmp(ibv_get_device_name(device_list[i]), devname)) {
            break;
        }
        // printf("%s\n", ibv_get_device_name(device_list[i]));
    }

    if (i == number_device) {
        fprintf(stderr, "RDMA device %s not found.\n", devname);
        goto free_dev_list;
    }

    struct ibv_device *device = device_list[i];

    // 第二步：打开设备(设备名指定)
    struct ibv_context *context = ibv_open_device(device);
    if (!context) {
        fprintf(stderr, "Could't get context for %s.\n", ibv_get_device_name(device));
        goto free_dev_list;
    }

    // 第三步：获得Protection Domain
    struct ibv_pd *pd = ibv_alloc_pd(context);
    if (!pd) {
        fprintf(stderr, "Couldn't allocate PD.\n");
        goto close_device;
    }

    char mr_buffer[REGION_SIZE];
    // 第四步：将分配的这段MR注册给网卡
    // 查看了user manual手册，Permission这个参数设置为0代表只能本地读
    struct ibv_mr *mr = ibv_reg_mr(pd, mr_buffer, REGION_SIZE, 0);
    if (!mr) {
        fprintf(stderr, "Couldn't register MR.\n");
        goto close_pd;
    }

    printf("Everything is successful by now.\n");


close_pd:
    ibv_dealloc_pd(pd);

close_device:
    ibv_close_device(context);

free_dev_list:
    ibv_free_device_list(device_list);

    return 0;
}
