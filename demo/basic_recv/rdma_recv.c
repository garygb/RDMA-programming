#include <infiniband/verbs.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <arpa/inet.h>
#include <unistd.h>

// MR的大小
#define REGION_SIZE 0x1800
// minimum CQ size
#define CQ_SIZE 0x100
//maximum number of outstanding WQ that can be posted to SQ
#define MAX_NUM_RECVS 0x10
#define MAX_GATHER_ENTRIES 2
#define MAX_SCATTER_ENTRIES 2

#define TEXT_MSG "Hello UD :)"

#define WELL_KNOWN_QKEY 0x11111111

// for time testing
#define MEM_POOL_SIZE 2000 //单位:MB
#define BLOCK_SIZE 1024 //一次从内存中将数据读取到文件的大小
#define DEBUG

// usage: ./rdma_recv
// 程序会打印出可以与之建立通信的QPN

// 实际过程中需要传入的参数：
// 1. devname
// 2. dev_port
int main(int argc, char** argv) {

    struct ibv_device **device_list;
    int    number_device;
    int    i;
    char   *devname = "mlx4_0";
    // char   dest_gid_str[] = "fe80:0000:0000:0000:0202:c9ff:fe05:6a21"; // 2.1 10G网卡第一个端口
    // Infiniband only
    // uint16_t  dest_lid = 0;
    int dev_port = 1; // 网卡的第一个端口为1

    //如何获得destination QP number?
    // int dest_qpn = 0;


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

    char *mr_buffer = (char*) malloc(MEM_POOL_SIZE * 1024 * 1024);
    if (!mr_buffer) {
        fprintf(stderr, "Could't allocate %d MB memory.\n", MEM_POOL_SIZE);
        return 0;
    }
    // char mr_buffer[REGION_SIZE];
    // 第四步：将分配的这段MR注册给网卡
    // ***与Send端不同-1***
    // 这里Permission改为支持本地写，详见user manual手册
    struct ibv_mr *mr = ibv_reg_mr(pd, mr_buffer, MEM_POOL_SIZE * 1024 * 1024, IBV_ACCESS_LOCAL_WRITE);
    if (!mr) {
        fprintf(stderr, "Couldn't register MR.\n");
        goto close_pd;
    }

    // printf("Register MR successful.\n");

    // 第五步：创建CQ
    // 后三个参数只有在处理completion events时使用
    // ？？回调函数是否可以在这里指定
    struct ibv_cq *cq = ibv_create_cq(context, CQ_SIZE, NULL, NULL, 0);
    if (!cq) {
        fprintf(stderr, "Couldn't create CQ.\n");
        goto free_mr;
    }

    // 第六步：创建QP
    // 设置QP属性
    struct ibv_qp_init_attr attr = {
        // qp_type: transport service type request for this QP
        // UD: unreliable datagram(这里之后需要修改！！)
        .qp_type = IBV_QPT_UD,
        .send_cq = cq,
        .recv_cq = cq,
        // QP send和QP recv元素的属性
        .cap = {
            //maximum number of outstanding WQ that can be posted to SQ
            // ***与Send端不同-2***
            // 因为该程序值需要receive，因此max_send_wr由MAX_NUM_SENDS改为0
            // max_recv_wr由0改为MAX_NUM_RECVS
            .max_send_wr  = 0,
            .max_recv_wr  = MAX_NUM_RECVS,
            .max_send_sge = MAX_GATHER_ENTRIES,
            .max_recv_sge = MAX_SCATTER_ENTRIES,
        },
    };

    struct ibv_qp *qp = ibv_create_qp(pd, &attr);
    if (!qp) {
        fprintf(stderr, "Couldn't create QP.\n");
        goto free_cq;
    }

    // //********* For testing **********
    // struct ibv_device_attr dev_attr;
    // if (ibv_query_device(context, &dev_attr) != 0) {
    //     fprintf(stderr, "Errror in querying device infomation.");
    //     goto free_cq;
    // }
    // // 32 for this NIC
    // printf("Max scatter gatter elements supported by this NIC is: %d.\n", dev_attr.max_sge);

    // 第七步：将QP状态切换到RTS
    struct ibv_qp_attr qp_modify_attr;

    qp_modify_attr.qp_state = IBV_QPS_INIT; // translate to init state
    qp_modify_attr.pkey_index = 0;
    qp_modify_attr.port_num = dev_port; // primary physical port
    qp_modify_attr.qkey = WELL_KNOWN_QKEY; // For UD only

    // RESET -> INIT
    if (ibv_modify_qp(qp, &qp_modify_attr,
        IBV_QP_STATE        |
        IBV_QP_PKEY_INDEX   |
		IBV_QP_PORT         |
		IBV_QP_QKEY)) {
        fprintf(stderr, "Failed to modify QP to INIT.\n");
        goto free_qp;
    }
	
    memset(&qp_modify_attr, 0, sizeof(qp_modify_attr));

    qp_modify_attr.qp_state = IBV_QPS_RTR;
    
    // INIT -> RTR
    if (ibv_modify_qp(qp, &qp_modify_attr, IBV_QP_STATE)) {
        fprintf(stderr, "Failed to modify QP to RTR.\n");
        goto free_qp;
    }

    // ***与Send端不同-3***
    // QP只需要进入RTR状态就行

    // memset(&qp_modify_attr, 0, sizeof(qp_modify_attr));

    // qp_modify_attr.qp_state = IBV_QPS_RTS;
    // qp_modify_attr.sq_psn = 1;
    // // RTR -> RTS
    // if (ibv_modify_qp(qp, &qp_modify_attr, IBV_QP_STATE | IBV_QP_SQ_PSN)) {
    //     fprintf(stderr, "Failed to modify QP to RTS.\n");
    //     goto free_qp;
    // }

    // ***与Send端不同-4***
    // 不需要创建Address Vector   

    // 第八步：创建Address Vector
    // union ibv_gid dest_gid;
    // if (parse_gid(dest_gid_str, &dest_gid)) {
    //     usage(argv[0]);
    //     goto free_qp;
    // }
    
    // struct ibv_ah_attr ah_attr;

    // // indicating the presence of destination gid
    // ah_attr.is_global = 1;
    // ah_attr.grh.dgid = dest_gid;
    // // the index of the source gid of our packet
    // ah_attr.grh.sgid_index = 0;
    // // achieved by running "ibstatus"
    // ah_attr.dlid = dest_lid;
    // // port num
    // ah_attr.port_num = dev_port;
 	// ah_attr.grh.hop_limit = 1;
	// ah_attr.sl = 0;   

    // struct ibv_ah *ah = ibv_create_ah(pd, &ah_attr);
    // if (!ah) {
    //     fprintf(stderr, "Failed to address handle.\n");
    //     goto free_qp;
    // }

    // ***与Send端不同-5***
    // Post Work Request配置不同

    // 第九步：Post Work Request
    // work request data structure
    struct ibv_recv_wr wr;
    struct ibv_recv_wr *bad_wr;
    struct ibv_sge list;

    fprintf(stdout, "Listening on QP Number 0x%06x\n", qp->qp_num);
    sleep(1);

#define MAX_MSG_SIZE 500 // MB  

    for (i = 0; i < 4; i++) {
        // 填好收到哪里，收多少大，mr的local key
        list.addr = (uint64_t)(mr_buffer + MAX_MSG_SIZE*1024*1024 * i);
        list.length = MAX_MSG_SIZE*1024*1024;
        list.lkey = mr->lkey;

        //填好Work Request
        wr.wr_id = i;
        wr.sg_list = &list;
        wr.num_sge = 1;
        wr.next = NULL;

        // 调用ibv_post_recv函数来收数据
        // *****注意：ibv_post_recv函数必须在发送之前被调用。*****
        if (ibv_post_recv(qp, &wr, &bad_wr)) {
            fprintf(stderr, "Function ibv_post_recv failed.\n");
            goto free_qp;
        }
    }

/*
    sprintf(mr_buffer, TEXT_MSG);
    list.addr = (uint64_t)mr_buffer;
    list.length = strlen(TEXT_MSG);
    // 注册好了MR本身就带有了local key
    list.lkey = mr->lkey;

    // user defined 64-bit identifier, 
    // will be returned in the completion element for this WR
    wr.wr_id = 0;
    // a pointer to list of buffers(and the sizes where data is located)
    wr.sg_list = &list;
    // number of entries in scatter gatter lists
    wr.num_sge = 1;
    // operation we will perform
    wr.opcode = IBV_WR_SEND;
    // set individual properties:
    // IBV_SEND_SIGNALED means generation of a completion element
    // once the data is transmitted
    wr.send_flags = IBV_SEND_SIGNALED;
    // next指向了下一个WR（在post了一系列WR时候用到）
    // 因为我们只是post了一个Send,所以这里使用NULL
    wr.next = NULL;

    // specify the target destination using previous created address vector
    wr.wr.ud.ah = ah;
    // specify the target QP number
    wr.wr.ud.remote_qpn = dest_qpn;
    // qkey value of remote QP
    // WELL_KNOWN_QKEY generatee all QP in this example share the same qkey
    wr.wr.ud.remote_qkey = WELL_KNOWN_QKEY;

    // 三个参数：
    // 1. created QP
    // 2. pointer to the first WR 
    // 3. pointer to a list of Bad Work Request in case of failure
    if (ibv_post_send(qp, &wr, &bad_wr)) {
        fprintf(stderr, "Function ibv_post_send failed.\n");
        goto free_qp;
    }
*/

    // ***与Send端不同-6***
    // Poll for completion不同(这里只是写法上有区别，操作流程没变)

    // 第十步：Poll for completion (retrieve Work Completion from CQ)
    
    // Receive端的WC entry包含了：
    // 1. Source MAC/LID of sender
    // 2. Source QP
    // 3. Destination QP
    // 4. Destination MAC/LID index
    // 5. Size of data scattered by the adapter
    struct ibv_wc wc;
    int ne;

    // 当一个接收的WR被消费，一个complete entry一定会被产生
    for (i = 0; i < 4; i++) {
        do {
            ne = ibv_poll_cq(cq, 1, &wc);
        } while (ne == 0);
        
        // call return negative value if function fails
        if (ne < 0) {
            fprintf(stderr, "CQ is in error state.");
            goto free_qp;
        }

        if (wc.status) {
            fprintf(stderr, "Bad completion (status %d).\n", (int)wc.status);
            goto free_qp;
        } else {
            // 对于unreliable datagram QP, 有一个Global Routing Header(GRH)，
            // 这个值写在Receive Buffer之前，大小为40 Bytes
            char* start_msg_addr = mr_buffer + MAX_MSG_SIZE*1024*1024*i + 40;
            printf("received: %s\n", );

            FilE * fp = NULL;
            fp = fopen("./receive.jpg", "wb");

            if (!fp) {
                fprintf(stderr, "Can't open target file.\n");
                return 0;
            }

            long file_size = 22988927;

            int i = 0;
            for (i = 0; i < file_size/BLOCK_SIZE; i++) {
                fwrite(start_msg_addr + i*BLOCK_SIZE, sizeof(char), BLOCK_SIZE, fp);
            }
            int remainder = file_size - (BLOCK_SIZE * i);
            fwrite(start_msg_addr + i*BLOCK_SIZE, sizeof(char), remainder, fp);
            fclose(fp);
        }
    }

/*
    // Arguments:
    // 1. CQ be retrieved
    // 2. maximum number of completions to read from a CQ
    // 3. points to a array of CQ entries to be filled in case completions to be found
    // Return:
    // if one or more entries are found, this call returns the number of completed entries(但不会超过num_entries,即第二个参数)
    // if no entries to be found, this call will return 0
    do {
        ne = ibv_poll_cq(cq, 1, &wc);
    } while (ne == 0);

    // call return negative value if function fails
    if (ne < 0) {
        fprintf(stderr, "CQ is in error state.");
        goto free_qp;
    }

    if (wc.status) {
        fprintf(stderr, "Bad completion (status %d).\n", (int)wc.status);
        goto free_qp;
    }
*/


free_qp:    
    ibv_destroy_qp(qp);

free_cq:
    ibv_destroy_cq(cq);

free_mr:
    ibv_dereg_mr(mr);

close_pd:
    ibv_dealloc_pd(pd);

close_device:
    ibv_close_device(context);

free_dev_list:
    ibv_free_device_list(device_list);

    return 0;
}
